#include "src/bluetooth.h"            // scan_classic, scan_ble, connect_and_subscribe

#include <winrt/base.h>               // init_apartment, check_hresult, put_abi
#include <winrt/Windows.System.h>     // DispatcherQueueController
#include <dispatcherqueue.h>          // CreateDispatcherQueueController

#include <iostream>
#include <vector>
#include <limits>
#include <ios>

#undef max


using namespace std;
using namespace winrt;
using namespace winrt::Windows::System;

int main()
{
    // 1) Initialize COM/WinRT on this thread
    init_apartment(apartment_type::single_threaded);

    // 2) Prepare and create the DispatcherQueueController
    ::DispatcherQueueOptions options{};
    options.dwSize = sizeof(options);
    options.threadType = DQTYPE_THREAD_CURRENT;
    options.apartmentType = DQTAT_COM_STA;

    DispatcherQueueController controller{ nullptr };
    check_hresult(
        ::CreateDispatcherQueueController(
            options,
            reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(
                put_abi(controller))
        )
    );

    while (true)
    {
        cout << "\n=== Joy-Con Scanner ===\n"
            << "1) HID Scan (Joy-Con 1)\n"
            << "2) BLE Scan (Joy-Con 2)\n"
            << "0) Exit\n"
            << "Choice: ";

        int choice;
        if (!(cin >> choice) || choice == 0)
            break;

        if (choice == 1)
        {
            auto classics = scan_classic();
            if (classics.empty()) {
                cout << "No HID devices found, please manually pair in Blueooth settings\n";
            }
            else {
                for (size_t i = 0; i < classics.size(); ++i) {
                    auto& d = classics[i];
                    cout << (i + 1) << ") [CL] " << d.address
                        << " \"" << d.name << "\""
                        << (d.connected ? " (connected)" : "")
                        << "\n";
                }
            }
        }
        else if (choice == 2)
        {
            auto bles = scan_ble();
            if (bles.empty()) {
                cout << "No BLE devices found.\n";
                continue;
            }

            // list
            for (size_t i = 0; i < bles.size(); ++i) {
                auto& d = bles[i];
                cout << (i + 1) << ") [BLE] "
                    << d.address
                    << " \"" << d.name << "\"\n";
            }

            cout << "Select device to connect (0 = cancel): ";
            int sel;
            cin >> sel;
            if (sel <= 0 || sel > (int)bles.size()) {
                cout << "Cancelled.\n";
            }
            else {
                // consume leftover newline
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

                auto& target = bles[sel - 1];
                cout << "Connecting to " << target.address << " …\n";

                if (connect_and_subscribe(target.address)) {
                    cout << "▶️  Streaming started. Press ENTER to disconnect.\n";
                    cin.get();  // wait for ENTER
                    cout << "Disconnected.\n";
                }
                else {
                    cout << "Failed to connect/subscribe.\n";
                }
            }
        }
        else
        {
            cout << "Invalid choice\n";
        }
    }

    return 0;
}