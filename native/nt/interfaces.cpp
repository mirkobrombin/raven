#include "interfaces.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ifaddrs.h>
#include <map>
#include <net/if.h>
#include <set>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace raven {

std::vector<HostInterface> host_interfaces()
{
	std::vector<HostInterface> interfaces;
	ifaddrs *list = nullptr;
	if (::getifaddrs(&list))
		return interfaces;
	std::set<std::string> seen;
	for (const auto *entry = list; entry; entry = entry->ifa_next) {
		if (!entry->ifa_name || !seen.insert(entry->ifa_name).second)
			continue;
		HostInterface interface;
		interface.name = entry->ifa_name;
		interface.index = ::if_nametoindex(entry->ifa_name);
		interface.status = (entry->ifa_flags & IFF_UP) && (entry->ifa_flags & IFF_RUNNING) ? 1 : 2;
		interface.type = (entry->ifa_flags & IFF_LOOPBACK) ? if_type_loopback :
			::access(("/sys/class/net/" + interface.name + "/wireless").c_str(), F_OK) == 0 ? if_type_wireless : if_type_ethernet;
		if (const int probe = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0); probe >= 0) {
			ifreq request{};
			std::strncpy(request.ifr_name, entry->ifa_name, IFNAMSIZ - 1);
			if (::ioctl(probe, SIOCGIFMTU, &request) == 0)
				interface.mtu = uint32_t(request.ifr_mtu);
			if (interface.type != if_type_loopback && ::ioctl(probe, SIOCGIFHWADDR, &request) == 0) {
				std::memcpy(interface.hardware, request.ifr_hwaddr.sa_data, 6);
				interface.hardware_length = 6;
			}
			::close(probe);
		}
		interfaces.push_back(std::move(interface));
	}
	::freeifaddrs(list);
	return interfaces;
}

std::vector<uint8_t> mib_if_row(const HostInterface &interface)
{
	std::vector<uint8_t> row(mib_if_row_size, 0);
	const auto put32 = [&](size_t at, uint32_t value) { std::memcpy(row.data() + at, &value, 4); };
	// \DEVICE\TCPIP_{index}: Windows names an interface by the GUID the driver gave it, and every interface here has an index it answers
	// to instead, so the name carries that and keeps the shape a program parses.
	char name[64];
	const int written = std::snprintf(name, sizeof name, "\\DEVICE\\TCPIP_{00000000-0000-0000-0000-%012u}", interface.index);
	const size_t name_length = std::min<size_t>(written > 0 ? size_t(written) : 0, 255);
	for (size_t at = 0; at < name_length; ++at)
		row[at * 2] = uint8_t(name[at]);
	put32(512, interface.index);
	put32(516, interface.type);
	put32(520, interface.mtu);
	// The link speed is the one a medium of this kind would have; nothing here measures it.
	put32(524, interface.type == if_type_loopback ? 10000000 : 1000000000);
	put32(528, interface.hardware_length);
	std::memcpy(row.data() + 532, interface.hardware, std::min<uint32_t>(interface.hardware_length, 8));
	// AdminStatus is enabled; OperStatus is up only while the interface carries traffic.
	put32(540, 1);
	put32(544, interface.status == 1 ? 1 : 2);
	const size_t description = std::min<size_t>(interface.name.size(), 255);
	put32(600, uint32_t(description));
	std::memcpy(row.data() + 604, interface.name.data(), description);
	return row;
}

}
