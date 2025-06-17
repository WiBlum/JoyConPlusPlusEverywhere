#include "joycon.h"
#include "constants.h"
#include <iostream>
#include <cstring>
#include <chrono>
#include <thread>
#include <algorithm>
#include <mutex>

JoyCon::JoyCon(uint16_t vendor_id, uint16_t product_id, const std::wstring& serial, bool simple_mode)
    : vendor_id_(vendor_id),
      product_id_(product_id),
      serial_(serial),
      simple_mode_(simple_mode),
      packet_number_(0),
      rumble_data_(DEFAULT_RUMBLE_DATA),
      joycon_device_(nullptr),
      running_(true)
{
    if (vendor_id != JOYCON_VENDOR_ID) {
        throw std::invalid_argument("vendor_id is invalid");
    }
    if (JOYCON_PRODUCT_IDS.find(product_id) == JOYCON_PRODUCT_IDS.end()) {
        throw std::invalid_argument("product_id is invalid");
    }

    set_accel_calibration({0, 0, 0}, {1, 1, 1});
    set_gyro_calibration({0, 0, 0}, {1, 1, 1});

    joycon_device_ = open(vendor_id, product_id, serial);
    read_joycon_data();
    setup_sensors();

    update_input_report_thread_ = std::thread(&JoyCon::update_input_report, this);
}

JoyCon::~JoyCon() {
    running_ = false;
    if (update_input_report_thread_.joinable()) {
        update_input_report_thread_.join();
    }
    close();
}

hid_device* JoyCon::open(uint16_t vendor_id, uint16_t product_id, const std::wstring& serial) {
    hid_device* dev = nullptr;
    if (!serial.empty()) {
        dev = hid_open(vendor_id, product_id, serial.c_str());
    } else {
        dev = hid_open(vendor_id, product_id, nullptr);
    }
    if (!dev) {
        throw std::runtime_error("joycon connect failed");
    }
    return dev;
}

void JoyCon::close() {
    if (joycon_device_) {
        hid_close(joycon_device_);
        joycon_device_ = nullptr;
    }
}

std::array<uint8_t, JoyCon::INPUT_REPORT_SIZE> JoyCon::read_input_report() const {
    std::array<uint8_t, INPUT_REPORT_SIZE> buf{};
    int res = hid_read(joycon_device_, buf.data(), INPUT_REPORT_SIZE);
    if (res < 0) {
        throw std::runtime_error("Failed to read input report");
    }
    return buf;
}

void JoyCon::write_output_report(const std::vector<uint8_t>& command) {
    int res = hid_write(joycon_device_, command.data(), command.size());
    if (res < 0) {
        throw std::runtime_error("Failed to write output report");
    }
}

std::pair<bool, std::vector<uint8_t>> JoyCon::send_subcmd_get_response(uint8_t subcommand, const std::vector<uint8_t>& argument) {
    std::vector<uint8_t> cmd = {0x01, packet_number_};
    cmd.insert(cmd.end(), rumble_data_.begin(), rumble_data_.end());
    cmd.push_back(subcommand);
    cmd.insert(cmd.end(), argument.begin(), argument.end());
    write_output_report(cmd);
    packet_number_ = (packet_number_ + 1) & 0xF;

    std::array<uint8_t, INPUT_REPORT_SIZE> report = read_input_report();
    while (report[0] != 0x21) {
        report = read_input_report();
    }
    if (report[1] == subcommand) {
        throw std::runtime_error("THREAD carefully");
    }
    bool ack = (report[13] & 0x80) != 0;
    std::vector<uint8_t> data(report.begin() + 13, report.end());
    return {ack, data};
}

std::vector<uint8_t> JoyCon::spi_flash_read(uint32_t address, uint8_t size) {
    if (size > 0x1d) throw std::invalid_argument("size too large for SPI read");
    std::vector<uint8_t> argument;
    for (int i = 0; i < 4; ++i) argument.push_back((address >> (8 * i)) & 0xFF);
    argument.push_back(size);
    auto [ack, report] = send_subcmd_get_response(0x10, argument);
    if (!ack) throw std::runtime_error("After SPI read: got NACK");
    if (!(report[0] == 0x90 && report[1] == 0x10)) throw std::runtime_error("Unexpected ACK in SPI read");
    if (!std::equal(argument.begin(), argument.end(), report.begin() + 2)) throw std::runtime_error("SPI argument mismatch");
    return std::vector<uint8_t>(report.begin() + 7, report.begin() + 7 + size);
}

void JoyCon::update_input_report() {
    while (running_) {
        std::array<uint8_t, INPUT_REPORT_SIZE> report = read_input_report();
        while (report[0] != 0x30) {
            report = read_input_report();
        }
        {
            std::lock_guard<std::mutex> lock(report_mutex_);
            input_report_ = report;
        }
        for (auto& cb : input_hooks_) {
            cb(*this);
        }
        // Optionally sleep for a polling interval
        // std::this_thread::sleep_for(std::chrono::duration<double>(INPUT_REPORT_PERIOD));
    }
}

void JoyCon::read_joycon_data() {
    auto color_data = spi_flash_read(0x6050, 6);

    std::vector<uint8_t> imu_cal;
    if (spi_flash_read(0x8026, 2) == std::vector<uint8_t>{0xB2, 0xA1}) {
        imu_cal = spi_flash_read(0x8028, 24);
    } else {
        imu_cal = spi_flash_read(0x6020, 24);
    }

    color_body_ = {color_data[0], color_data[1], color_data[2]};
    color_btn_  = {color_data[3], color_data[4], color_data[5]};

    set_accel_calibration(
        {to_int16le_from_2bytes(imu_cal[0], imu_cal[1]),
         to_int16le_from_2bytes(imu_cal[2], imu_cal[3]),
         to_int16le_from_2bytes(imu_cal[4], imu_cal[5])},
        {to_int16le_from_2bytes(imu_cal[6], imu_cal[7]),
         to_int16le_from_2bytes(imu_cal[8], imu_cal[9]),
         to_int16le_from_2bytes(imu_cal[10], imu_cal[11])}
    );
    set_gyro_calibration(
        {to_int16le_from_2bytes(imu_cal[12], imu_cal[13]),
         to_int16le_from_2bytes(imu_cal[14], imu_cal[15]),
         to_int16le_from_2bytes(imu_cal[16], imu_cal[17])},
        {to_int16le_from_2bytes(imu_cal[18], imu_cal[19]),
         to_int16le_from_2bytes(imu_cal[20], imu_cal[21]),
         to_int16le_from_2bytes(imu_cal[22], imu_cal[23])}
    );
}

void JoyCon::setup_sensors() {
    write_output_report({0x01, packet_number_, rumble_data_[0], rumble_data_[1], rumble_data_[2], rumble_data_[3], rumble_data_[4], rumble_data_[5], rumble_data_[6], rumble_data_[7], 0x40, 0x01});
    packet_number_ = (packet_number_ + 1) & 0xF;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    write_output_report({0x01, packet_number_, rumble_data_[0], rumble_data_[1], rumble_data_[2], rumble_data_[3], rumble_data_[4], rumble_data_[5], rumble_data_[6], rumble_data_[7], 0x03, 0x30});
    packet_number_ = (packet_number_ + 1) & 0xF;
}

int16_t JoyCon::to_int16le_from_2bytes(uint8_t hbytebe, uint8_t lbytebe) {
    uint16_t uint16le = (lbytebe << 8) | hbytebe;
    return (uint16le < 32768) ? uint16le : (uint16le - 65536);
}

int JoyCon::get_nbit_from_input_report(const std::array<uint8_t, INPUT_REPORT_SIZE>& report, size_t offset_byte, int offset_bit, int nbit) const {
    uint8_t byte = report[offset_byte];
    return (byte >> offset_bit) & ((1 << nbit) - 1);
}

// Calibration
void JoyCon::set_gyro_calibration(const std::array<int16_t, 3>& offset_xyz, const std::array<int16_t, 3>& coeff_xyz) {
    GYRO_OFFSET_X_ = offset_xyz[0];
    GYRO_OFFSET_Y_ = offset_xyz[1];
    GYRO_OFFSET_Z_ = offset_xyz[2];
    GYRO_COEFF_X_ = (coeff_xyz[0] != 0x343b) ? 0x343b / static_cast<float>(coeff_xyz[0]) : 1.0f;
    GYRO_COEFF_Y_ = (coeff_xyz[1] != 0x343b) ? 0x343b / static_cast<float>(coeff_xyz[1]) : 1.0f;
    GYRO_COEFF_Z_ = (coeff_xyz[2] != 0x343b) ? 0x343b / static_cast<float>(coeff_xyz[2]) : 1.0f;
}

void JoyCon::set_accel_calibration(const std::array<int16_t, 3>& offset_xyz, const std::array<int16_t, 3>& coeff_xyz) {
    ACCEL_OFFSET_X_ = offset_xyz[0];
    ACCEL_OFFSET_Y_ = offset_xyz[1];
    ACCEL_OFFSET_Z_ = offset_xyz[2];
    ACCEL_COEFF_X_ = (coeff_xyz[0] != 0x4000) ? 0x4000 / static_cast<float>(coeff_xyz[0]) : 1.0f;
    ACCEL_COEFF_Y_ = (coeff_xyz[1] != 0x4000) ? 0x4000 / static_cast<float>(coeff_xyz[1]) : 1.0f;
    ACCEL_COEFF_Z_ = (coeff_xyz[2] != 0x4000) ? 0x4000 / static_cast<float>(coeff_xyz[2]) : 1.0f;
}

void JoyCon::register_update_hook(std::function<void(JoyCon&)> callback) {
    input_hooks_.push_back(callback);
}

bool JoyCon::is_left() const {
    return product_id_ == JOYCON_L_PRODUCT_ID;
}

bool JoyCon::is_right() const {
    return product_id_ == JOYCON_R_PRODUCT_ID;
}

// Button getters (now take a report parameter)
#define BUTTON_GETTER(NAME, BYTE, BIT, NBIT) \
    int JoyCon::get_##NAME(const std::array<uint8_t, INPUT_REPORT_SIZE>& report) const { return get_nbit_from_input_report(report, BYTE, BIT, NBIT); }

BUTTON_GETTER(battery_charging, 2, 4, 1)
BUTTON_GETTER(battery_level, 2, 5, 3)
BUTTON_GETTER(button_y, 3, 0, 1)
BUTTON_GETTER(button_x, 3, 1, 1)
BUTTON_GETTER(button_b, 3, 2, 1)
BUTTON_GETTER(button_a, 3, 3, 1)
BUTTON_GETTER(button_right_sr, 3, 4, 1)
BUTTON_GETTER(button_right_sl, 3, 5, 1)
BUTTON_GETTER(button_r, 3, 6, 1)
BUTTON_GETTER(button_zr, 3, 7, 1)
BUTTON_GETTER(button_minus, 4, 0, 1)
BUTTON_GETTER(button_plus, 4, 1, 1)
BUTTON_GETTER(button_r_stick, 4, 2, 1)
BUTTON_GETTER(button_l_stick, 4, 3, 1)
BUTTON_GETTER(button_home, 4, 4, 1)
BUTTON_GETTER(button_capture, 4, 5, 1)
BUTTON_GETTER(button_charging_grip, 4, 7, 1)
BUTTON_GETTER(button_down, 5, 0, 1)
BUTTON_GETTER(button_up, 5, 1, 1)
BUTTON_GETTER(button_right, 5, 2, 1)
BUTTON_GETTER(button_left, 5, 3, 1)
BUTTON_GETTER(button_left_sr, 5, 4, 1)
BUTTON_GETTER(button_left_sl, 5, 5, 1)
BUTTON_GETTER(button_l, 5, 6, 1)
BUTTON_GETTER(button_zl, 5, 7, 1)

#undef BUTTON_GETTER

// Stick getters (use local report)
int JoyCon::get_stick_left_horizontal(const std::array<uint8_t, INPUT_REPORT_SIZE>& report) const {
    return get_nbit_from_input_report(report, 6, 0, 8) | (get_nbit_from_input_report(report, 7, 0, 4) << 8);
}
int JoyCon::get_stick_left_vertical(const std::array<uint8_t, INPUT_REPORT_SIZE>& report) const {
    return get_nbit_from_input_report(report, 7, 4, 4) | (get_nbit_from_input_report(report, 8, 0, 8) << 4);
}
int JoyCon::get_stick_right_horizontal(const std::array<uint8_t, INPUT_REPORT_SIZE>& report) const {
    return get_nbit_from_input_report(report, 9, 0, 8) | (get_nbit_from_input_report(report, 10, 0, 4) << 8);
}
int JoyCon::get_stick_right_vertical(const std::array<uint8_t, INPUT_REPORT_SIZE>& report) const {
    return get_nbit_from_input_report(report, 10, 4, 4) | (get_nbit_from_input_report(report, 11, 0, 8) << 4);
}

// Accel/Gyro getters (use local report)
float JoyCon::get_accel_x(const std::array<uint8_t, INPUT_REPORT_SIZE>& report, int sample_idx) const {
    if (sample_idx < 0 || sample_idx > 2) throw std::out_of_range("sample_idx");
    int16_t data = to_int16le_from_2bytes(report[13 + sample_idx * 12], report[14 + sample_idx * 12]);
    return (data - ACCEL_OFFSET_X_) * ACCEL_COEFF_X_;
}
float JoyCon::get_accel_y(const std::array<uint8_t, INPUT_REPORT_SIZE>& report, int sample_idx) const {
    if (sample_idx < 0 || sample_idx > 2) throw std::out_of_range("sample_idx");
    int16_t data = to_int16le_from_2bytes(report[15 + sample_idx * 12], report[16 + sample_idx * 12]);
    return (data - ACCEL_OFFSET_Y_) * ACCEL_COEFF_Y_;
}
float JoyCon::get_accel_z(const std::array<uint8_t, INPUT_REPORT_SIZE>& report, int sample_idx) const {
    if (sample_idx < 0 || sample_idx > 2) throw std::out_of_range("sample_idx");
    int16_t data = to_int16le_from_2bytes(report[17 + sample_idx * 12], report[18 + sample_idx * 12]);
    return (data - ACCEL_OFFSET_Z_) * ACCEL_COEFF_Z_;
}
float JoyCon::get_gyro_x(const std::array<uint8_t, INPUT_REPORT_SIZE>& report, int sample_idx) const {
    if (sample_idx < 0 || sample_idx > 2) throw std::out_of_range("sample_idx");
    int16_t data = to_int16le_from_2bytes(report[19 + sample_idx * 12], report[20 + sample_idx * 12]);
    return (data - GYRO_OFFSET_X_) * GYRO_COEFF_X_;
}
float JoyCon::get_gyro_y(const std::array<uint8_t, INPUT_REPORT_SIZE>& report, int sample_idx) const {
    if (sample_idx < 0 || sample_idx > 2) throw std::out_of_range("sample_idx");
    int16_t data = to_int16le_from_2bytes(report[21 + sample_idx * 12], report[22 + sample_idx * 12]);
    return (data - GYRO_OFFSET_Y_) * GYRO_COEFF_Y_;
}
float JoyCon::get_gyro_z(const std::array<uint8_t, INPUT_REPORT_SIZE>& report, int sample_idx) const {
    if (sample_idx < 0 || sample_idx > 2) throw std::out_of_range("sample_idx");
    int16_t data = to_int16le_from_2bytes(report[23 + sample_idx * 12], report[24 + sample_idx * 12]);
    return (data - GYRO_OFFSET_Z_) * GYRO_COEFF_Z_;
}

// Status (uses a local copy of the report for all fields)
JoyCon::Status JoyCon::get_status() const {
    Status s;
    std::array<uint8_t, INPUT_REPORT_SIZE> report;
    {
        std::lock_guard<std::mutex> lock(report_mutex_);
        report = input_report_;
    }
    s.battery.charging = get_battery_charging(report);
    s.battery.level = get_battery_level(report);
    s.buttons.right.y = get_button_y(report);
    s.buttons.right.x = get_button_x(report);
    s.buttons.right.b = get_button_b(report);
    s.buttons.right.a = get_button_a(report);
    s.buttons.right.sr = get_button_right_sr(report);
    s.buttons.right.sl = get_button_right_sl(report);
    s.buttons.right.r = get_button_r(report);
    s.buttons.right.zr = get_button_zr(report);
    s.buttons.right.plus = get_button_plus(report);
    s.buttons.right.home = get_button_home(report);
    s.buttons.left.down = get_button_down(report);
    s.buttons.left.up = get_button_up(report);
    s.buttons.left.right = get_button_right(report);
    s.buttons.left.left = get_button_left(report);
    s.buttons.left.sr = get_button_left_sr(report);
    s.buttons.left.sl = get_button_left_sl(report);
    s.buttons.left.l = get_button_l(report);
    s.buttons.left.zl = get_button_zl(report);
    s.buttons.left.minus = get_button_minus(report);
    s.buttons.left.capture = get_button_capture(report);
    s.analog_sticks.left.horizontal = get_stick_left_horizontal(report) - status_offset_.stick_left_horizontal;
    s.analog_sticks.left.vertical   = get_stick_left_vertical(report)   - status_offset_.stick_left_vertical;
    s.analog_sticks.left.pressed    = get_button_l_stick(report);
    s.analog_sticks.right.horizontal = get_stick_right_horizontal(report) - status_offset_.stick_right_horizontal;
    s.analog_sticks.right.vertical   = get_stick_right_vertical(report)   - status_offset_.stick_right_vertical;
    s.analog_sticks.right.pressed    = get_button_r_stick(report);
    s.accel.x = get_accel_x(report);
    s.accel.y = get_accel_y(report);
    s.accel.z = get_accel_z(report);
    s.gyro.x = get_gyro_x(report) - status_offset_.gyro_x;
    s.gyro.y = get_gyro_y(report) - status_offset_.gyro_y;
    s.gyro.z = get_gyro_z(report) - status_offset_.gyro_z;
    return s;
}

void JoyCon::status_offset() {
    std::array<uint8_t, INPUT_REPORT_SIZE> report;
    {
        std::lock_guard<std::mutex> lock(report_mutex_);
        report = input_report_;
    }
    status_offset_.stick_left_horizontal = get_stick_left_horizontal(report);
    status_offset_.stick_left_vertical = get_stick_left_vertical(report);
    status_offset_.stick_right_horizontal = get_stick_right_horizontal(report);
    status_offset_.stick_right_vertical = get_stick_right_vertical(report);
    status_offset_.gyro_x = get_gyro_x(report);
    status_offset_.gyro_y = get_gyro_y(report);
    status_offset_.gyro_z = get_gyro_z(report);
}


// Lamp and rumble
void JoyCon::set_player_lamp_on(int on_pattern) {
    std::vector<uint8_t> cmd = {0x01, packet_number_};
    cmd.insert(cmd.end(), rumble_data_.begin(), rumble_data_.end());
    cmd.push_back(0x30);
    cmd.push_back(on_pattern & 0xF);
    write_output_report(cmd);
    packet_number_ = (packet_number_ + 1) & 0xF;
}

void JoyCon::set_player_lamp_flashing(int player_number) {
    int binaryPattern = 1;
    switch (player_number) {
        case 1: binaryPattern = 1; break;
        case 2: binaryPattern = 3; break;
        case 3: binaryPattern = 7; break;
        case 4: binaryPattern = 15; break;
        case 5: binaryPattern = 9; break;
        case 6: binaryPattern = 10; break;
        case 7: binaryPattern = 11; break;
        case 8: binaryPattern = 6; break;
        default: throw std::invalid_argument("Invalid player number");
    }
    std::vector<uint8_t> cmd = {0x01, packet_number_};
    cmd.insert(cmd.end(), rumble_data_.begin(), rumble_data_.end());
    cmd.push_back(0x30);
    cmd.push_back((binaryPattern & 0xF) << 4);
    write_output_report(cmd);
    packet_number_ = (packet_number_ + 1) & 0xF;
}

void JoyCon::set_player_lamp(int player_number) {
    int binaryPattern = 1;
    switch (player_number) {
        case 1: binaryPattern = 1; break;
        case 2: binaryPattern = 3; break;
        case 3: binaryPattern = 7; break;
        case 4: binaryPattern = 15; break;
        case 5: binaryPattern = 9; break;
        case 6: binaryPattern = 10; break;
        case 7: binaryPattern = 11; break;
        case 8: binaryPattern = 6; break;
        default: throw std::invalid_argument("Invalid player number");
    }
    std::vector<uint8_t> cmd = {0x01, packet_number_};
    cmd.insert(cmd.end(), rumble_data_.begin(), rumble_data_.end());
    cmd.push_back(0x30);
    cmd.push_back(binaryPattern & 0xF);
    write_output_report(cmd);
    packet_number_ = (packet_number_ + 1) & 0xF;
}

void JoyCon::send_rumble(const std::array<uint8_t, 8>& data) {
    rumble_data_ = data;
    std::vector<uint8_t> cmd = {0x10, packet_number_};
    cmd.insert(cmd.end(), rumble_data_.begin(), rumble_data_.end());
    write_output_report(cmd);
    packet_number_ = (packet_number_ + 1) & 0xF;
}

void JoyCon::enable_vibration(bool enable) {
    std::vector<uint8_t> cmd = {0x01, packet_number_};
    cmd.insert(cmd.end(), rumble_data_.begin(), rumble_data_.end());
    cmd.push_back(0x48);
    cmd.push_back(enable ? 0x01 : 0x00);
    write_output_report(cmd);
    packet_number_ = (packet_number_ + 1) & 0xF;
}

void JoyCon::rumble_simple() {
    send_rumble({0x98, 0x2e, 0xc6, 0x48, 0x98, 0x1e, 0xc6, 0x47});
}

void JoyCon::rumble_bump() {
    send_rumble({0x98, 0x1e, 0xc1, 0x51, 0x98, 0x1e, 0xc1, 0x12});
}

void JoyCon::rumble_stop() {
    send_rumble(DEFAULT_RUMBLE_DATA);
}

void JoyCon::disconnect_device() {
    std::vector<uint8_t> cmd = {0x01, packet_number_};
    cmd.insert(cmd.end(), rumble_data_.begin(), rumble_data_.end());
    cmd.push_back(0x06);
    cmd.push_back(0x00);
    write_output_report(cmd);
    packet_number_ = (packet_number_ + 1) & 0xF;
}
