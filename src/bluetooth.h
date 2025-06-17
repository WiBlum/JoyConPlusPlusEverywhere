#pragma once

#include <string>
#include <vector>

struct Device {
	bool        isBLE;
	std::string address;   // printable “AA:BB:…”
	bool        connected; // only meaningful for Classic
	std::string name;
};

// your existing scans
std::vector<Device> scan_classic();
std::vector<Device> scan_ble();

// NEW: connect to a BLE device by address string ("AA:BB:CC:DD:EE:FF"),
// discover the 128-bit service, subscribe to all Notify chars, and
// print incoming packets. Returns true on success.
bool connect_and_subscribe(std::string const& addrStr);