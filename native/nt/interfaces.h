#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace raven {

// IFTYPE values as the interface table reports them.
inline constexpr uint32_t if_type_ethernet = 6, if_type_loopback = 24, if_type_wireless = 71;

// One of the host's network interfaces, as far as the guest is told about it: no address, only the identity the interface calls describe.
struct HostInterface {
	std::string name;
	uint32_t index = 0, mtu = 1500, type = if_type_ethernet, status = 2;
	uint8_t hardware[8]{};
	uint32_t hardware_length = 0;
};

// The interfaces of the host, in the order the system lists them.
std::vector<HostInterface> host_interfaces();

// Bytes of a MIB_IFROW: a 256-character name, the identity and the counters, then a 256-byte description.
inline constexpr size_t mib_if_row_size = 860;

// A MIB_IFROW record of an interface. The counters stay zero: nothing here measures them, and a program that reads them reads what a
// freshly started interface reports.
std::vector<uint8_t> mib_if_row(const HostInterface &interface);

}
