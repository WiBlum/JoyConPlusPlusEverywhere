#include <windows.h>
#include <hidapi.h>
#include <BluetoothAPIs.h>
#pragma comment(lib, "Bthprops.lib")

#include <winrt/base.h>  // init_apartment, check_hresult, to_string, auto_revoke
#include <winrt/Windows.Foundation.h>                     // array_view, to_hstring
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.h>               // BluetoothLEDevice
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Storage.Streams.h>                 // DataReader
#include <dispatcherqueue.h>                               // CreateDispatcherQueueController

#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include <mutex>
#include <thread>
#include <chrono>
#include <algorithm>

using namespace winrt;
using namespace winrt::Windows::Foundation;        // array_view, to_hstring, to_string
using namespace winrt::Windows::Storage::Streams;  // DataReader, IBuffer
using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Bluetooth::Advertisement;
using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;

struct Device {
    bool        isBLE;
    std::string address;   // “XX:XX:…”
    bool        connected;
    std::string name;
};

//------------------------------------------------------------------------------
// Helpers

static std::string wstrToUtf8(const WCHAR* w) {
    if (!w) return {};
    int n = ::WideCharToMultiByte(
        CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr
    );
    if (n <= 0) return {};
    std::string s(n, '\0');
    ::WideCharToMultiByte(
        CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr
    );
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

static std::string addrClassic(const BLUETOOTH_ADDRESS& a) {
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (int i = 5; i >= 0; --i) {
        ss << std::setw(2) << int(a.rgBytes[i]);
        if (i) ss << ':';
    }
    return ss.str();
}

static std::string addrBLE(uint64_t raw) {
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (int i = 5; i >= 0; --i) {
        ss << std::setw(2) << int((raw >> (i * 8)) & 0xFF);
        if (i) ss << ':';
    }
    return ss.str();
}

static void hexDump(const void* data, size_t size) {
    auto p = reinterpret_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        std::printf("%02X ", p[i]);
        if ((i + 1) % 16 == 0 && i + 1 < size) std::printf("\n");
    }
    std::printf("\n");
}

//------------------------------------------------------------------------------
// Classic scan (unchanged)

std::vector<Device> scan_classic() {
    std::vector<Device> out;

    if (hid_init())
        return out;

    struct hid_device_info* devs = hid_enumerate(0x057e, 0x0); // 0x057e = Nintendo
    struct hid_device_info* cur_dev = devs;

    while (cur_dev) {
        // Joy-Con L: PID 0x2006, Joy-Con R: PID 0x2007
        if (cur_dev->vendor_id == 0x057e &&
            (cur_dev->product_id == 0x2006 || cur_dev->product_id == 0x2007)) {

            Device d;
            d.isBLE = false;
            d.address = ""; // HIDAPI does not provide Bluetooth address
            d.connected = true; // If it's enumerated, it's connected
            if (cur_dev->product_id == 0x2006)
                d.name = "Joy-Con 1 (L)";
            else
                d.name = "Joy-Con 1 (R)";

            out.push_back(d);

            std::wcout << L"Found " << d.name.c_str()
                << L" | Path: " << cur_dev->path
                << L" | Manufacturer: " << (cur_dev->manufacturer_string ? cur_dev->manufacturer_string : L"")
                << L" | Product: " << (cur_dev->product_string ? cur_dev->product_string : L"")
                << std::endl;
        }
        cur_dev = cur_dev->next;
    }

    hid_free_enumeration(devs);
    hid_exit();

    return out;
}

//------------------------------------------------------------------------------
// BLE scan (fixed ReadBytes)

std::vector<Device> scan_ble() {
    std::vector<Device> devices;
    std::mutex          mtx;

    const uint16_t NINTENDO_BLE_CID = 0x0553;
    const uint8_t  JOYCON_R_SIDE = 0x66;
    const uint8_t  JOYCON_L_SIDE = 0x67;

    BluetoothLEAdvertisementWatcher watcher{};
    watcher.ScanningMode(BluetoothLEScanningMode::Active);

    watcher.Received(
        [&](auto const&, BluetoothLEAdvertisementReceivedEventArgs const& evt) {
            bool hasNintendo = false;
            for (auto const& md : evt.Advertisement().ManufacturerData()) {
                if (md.CompanyId() == NINTENDO_BLE_CID) { hasNintendo = true; break; }
            }
            if (!hasNintendo) return;

            uint8_t side = 0xff;
            for (auto const& md : evt.Advertisement().ManufacturerData()) {
                if (md.CompanyId() != NINTENDO_BLE_CID) continue;
                IBuffer buf = md.Data();
                auto reader = DataReader::FromBuffer(buf);
                uint32_t len = buf.Length();
                std::vector<uint8_t> bytes(len);
                // <-- fixed: use array_view
                reader.ReadBytes(array_view<uint8_t>(bytes.data(), bytes.size()));
                if (len > 5) side = bytes[5];
                break;
            }

            std::string friendly;
            if (side == JOYCON_R_SIDE)      friendly = "Joy-Con 2 (R)";
            else if (side == JOYCON_L_SIDE) friendly = "Joy-Con 2 (L)";
            else                          friendly = "Nintendo BLE Device";

            auto addr = addrBLE(evt.BluetoothAddress());
            std::lock_guard<std::mutex> lk(mtx);
            if (std::none_of(devices.begin(), devices.end(),
                [&](auto const& d) {return d.address == addr; }))
            {
                Device d{ true, addr, false, friendly };
                devices.push_back(d);
                std::cout << "RAW ADV from " << addr
                    << " | RSSI " << evt.RawSignalStrengthInDBm()
                    << " dBm | " << friendly << "\n";
                for (auto const& sec : evt.Advertisement().DataSections()) {
                    uint8_t type = sec.DataType();
                    IBuffer bufSec = sec.Data();
                    auto rdr = DataReader::FromBuffer(bufSec);
                    uint32_t lenSec = bufSec.Length();
                    std::vector<uint8_t> bb(lenSec);
                    // <-- fixed here, too
                    rdr.ReadBytes(array_view<uint8_t>(bb.data(), bb.size()));
                    std::cout << "  AD Type 0x" << std::hex << int(type)
                        << std::dec << " [" << lenSec << " bytes]: ";
                    for (auto b : bb) {
                        std::cout << std::hex << std::setw(2) << std::setfill('0')
                            << int(b) << ' ';
                    }
                    std::cout << std::dec << "\n";
                }
                std::cout << "\n";
            }
        });

    watcher.Start();
    std::this_thread::sleep_for(std::chrono::seconds(5));
    watcher.Stop();
    return devices;
}

//------------------------------------------------------------------------------
// BLE connect & subscribe

static uint64_t parseBleAddress(std::string const& s) {
    uint64_t v = 0; const char* p = s.c_str();
    while (*p) {
        auto hexVal = [&](char c) {
            return std::isdigit(c) ? (c - '0') : (std::tolower(c) - 'a' + 10);
            };
        unsigned hi = hexVal(*p++), lo = hexVal(*p++);
        unsigned byte = (hi << 4) | lo;
        v = (v << 8) | byte;
        if (*p == ':' || *p == '-') ++p;
    }
    return v;
}

static constexpr GUID MyServiceUuid = {
  0xab7de9be,0x89fe,0x49ad,
  {0x82,0x8f,0x11,0x8f,0x09,0xdf,0x7f,0xd0}
};


static void OnCharChanged(
    GattCharacteristic const& ch,
    GattValueChangedEventArgs const& args)
{
    std::string uuid = to_string(to_hstring(ch.Uuid()));

    DataReader rdr = DataReader::FromBuffer(args.CharacteristicValue());
    std::vector<uint8_t> buf(rdr.UnconsumedBufferLength());
    rdr.ReadBytes(array_view<uint8_t>(buf.data(), buf.size()));
    std::cout << "⟶ Notify from [" << uuid << "] (" << buf.size() << " bytes): ";
    for (auto b : buf) std::printf("%02X ", b);
    std::puts("");
}


void dumpServices(BluetoothLEDevice const& ble) {
    auto sres = ble.GetGattServicesAsync().get();
    if (sres.Status() != GattCommunicationStatus::Success) {
        std::cerr << "Failed to enumerate services\n";
        return;
    }
    auto services = sres.Services();
    std::cout << "Discovered " << services.Size() << " services:\n";
    for (auto const& svc : services) {
        std::string svcUuidStr = to_string(to_hstring(svc.Uuid()));
        std::cout << "  Service UUID: " << svcUuidStr << "\n";

        auto cres = svc.GetCharacteristicsAsync().get();
        if (cres.Status() != GattCommunicationStatus::Success) {
            std::cerr << "    ❌ Failed to enumerate characteristics\n";
            continue;
        }
        auto chars = cres.Characteristics();
        std::cout << "    Found " << chars.Size() << " characteristics:\n";
        for (auto const& ch : chars) {
            std::string charUuidStr = to_string(to_hstring(ch.Uuid()));
            auto props = ch.CharacteristicProperties();
            std::cout << "      Char UUID: " << charUuidStr
                << " | Properties: 0x" << std::hex << int(props) << std::dec;

            bool canNotify = ((props & GattCharacteristicProperties::Notify) != GattCharacteristicProperties::None)
                || ((props & GattCharacteristicProperties::Indicate) != GattCharacteristicProperties::None);
            if (canNotify) {
                std::cout << " [Notify/Indicate]";
                // Optionally subscribe:
                ch.ValueChanged(
                    auto_revoke,
                    [](GattCharacteristic const& s, GattValueChangedEventArgs const& a) {
                        OnCharChanged(s, a);
                    }
                );
                auto status = ch
                    .WriteClientCharacteristicConfigurationDescriptorAsync(
                        GattClientCharacteristicConfigurationDescriptorValue::Notify
                    ).get();
                if (status == GattCommunicationStatus::Success)
                    std::cout << " (Subscribed)";
                else
                    std::cout << " (Subscribe failed)";
            }
            std::cout << "\n";
        }
    }
}


bool connect_and_subscribe(std::string const& addrStr) {
    uint64_t addr = parseBleAddress(addrStr);

    auto bleOp = BluetoothLEDevice::FromBluetoothAddressAsync(addr).get();
    if (!bleOp) {
        std::cerr << "❌ BLE connect failed\n";
        return false;
    }
    BluetoothLEDevice ble = bleOp;
    dumpServices(ble);

    // Enumerate all services and find the one matching MyServiceUuid
    auto sres = ble.GetGattServicesAsync().get();
    if (sres.Status() != GattCommunicationStatus::Success) {
        std::cerr << "❌ Failed to enumerate services\n";
        return false;
    }

    GattDeviceService targetService{ nullptr };
    for (auto const& svc : sres.Services()) {
        std::string svcUuidStr = to_string(to_hstring(svc.Uuid()));
        std::string myUuidStr = to_string(to_hstring(winrt::guid(MyServiceUuid)));
        std::cout << "  [DEBUG] Service UUID: " << svcUuidStr
            << " | MyServiceUuid: " << myUuidStr << std::endl;
        if (svc.Uuid() == winrt::guid(MyServiceUuid)) {
            targetService = svc;
            break;
        }
    }
    if (!targetService) {
        std::cerr << "❌ Service not found (manual search)\n";
        return false;
    }

    auto cres = targetService.GetCharacteristicsAsync().get();
    if (cres.Status() != GattCommunicationStatus::Success) {
        std::cerr << "❌ Char enumeration failed\n";
        return false;
    }


    bool any = false;
    for (auto const& ch : cres.Characteristics()) {
        auto props = ch.CharacteristicProperties();

        if ((props & GattCharacteristicProperties::WriteWithoutResponse) != GattCharacteristicProperties::None
            || (props & GattCharacteristicProperties::Write) != GattCharacteristicProperties::None)
        {
            std::cout << "📝 Found writable characteristic: "
                << to_string(to_hstring(ch.Uuid())) << "\n";

            // Save the characteristic to write to it after
        }

        bool canNotify = ((props & GattCharacteristicProperties::Notify) != GattCharacteristicProperties::None)
            || ((props & GattCharacteristicProperties::Indicate) != GattCharacteristicProperties::None);
        if (!canNotify) continue;

        any = true;
        ch.ValueChanged(
            auto_revoke,
            [](GattCharacteristic const& s, GattValueChangedEventArgs const& a) {
                OnCharChanged(s, a);
            }
        );
        auto status = ch
            .WriteClientCharacteristicConfigurationDescriptorAsync(
                GattClientCharacteristicConfigurationDescriptorValue::Notify
            ).get();

        hstring hUuid = to_hstring(ch.Uuid());
        std::string sUuid = to_string(hUuid);
        if (status == GattCommunicationStatus::Success)
            std::cout << "✅ Subscribed to char " << sUuid << "\n";
        else
            std::cerr << "⚠️ Failed to enable Notify on " << sUuid << "\n";
    }

    if (!any) {
        std::cerr << "❌ No Notify-capable characteristics\n";
        return false;
    }
    std::cout << "✔️ Waiting for notifications...\n";
    return true;
}