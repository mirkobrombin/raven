#include "access_token.h"
#include "kernel_context.h"
#include "text.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cstring>
#include <ctime>
#include <fstream>
#include <ifaddrs.h>
#include <map>
#include <optional>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace raven {

namespace {

constexpr uint32_t error_buffer_overflow = 111, error_no_data = 232, error_invalid_parameter = 87, wsaservice_not_found = 10108, wsa_invalid_handle = 6, wsa_not_enough_memory = 8, error_io_pending = 997, error_not_supported = 50, error_more_data = 234, error_no_more_items = 259;
constexpr uint32_t skip_unicast = 1, skip_anycast = 2, skip_multicast = 4, skip_dns_server = 8;
constexpr uint32_t windows_inet6 = 23;
constexpr uint32_t if_type_ethernet = 6, if_type_loopback = 24, if_type_wireless = 71;

// A set of device interfaces that SetupDi hands out; the host exposes no devices to the guest, so the set stays empty.
class DeviceInfoSet : public KernelObject {};

// An event log query: the log holds no records, so the query only ever reports that nothing more is there.
class EventQuery : public KernelObject {};

struct Address {
	std::vector<uint8_t> socket_address;
	uint8_t prefix = 0;
};

struct Adapter {
	std::string name;
	uint32_t index = 0, mtu = 1500, type = if_type_ethernet, status = 2;
	uint8_t hardware[8]{};
	uint32_t hardware_length = 0;
	std::vector<Address> unicast;
};

// A SOCKADDR_IN or SOCKADDR_IN6 in Windows layout, without a port.
std::vector<uint8_t> windows_address(const sockaddr *host)
{
	std::vector<uint8_t> bytes;
	if (host->sa_family == AF_INET) {
		bytes.assign(16, 0);
		const uint16_t family = AF_INET;
		std::memcpy(bytes.data(), &family, 2);
		std::memcpy(bytes.data() + 4, &reinterpret_cast<const sockaddr_in *>(host)->sin_addr, 4);
	} else {
		const auto *source = reinterpret_cast<const sockaddr_in6 *>(host);
		bytes.assign(28, 0);
		const uint16_t family = uint16_t(windows_inet6);
		std::memcpy(bytes.data(), &family, 2);
		std::memcpy(bytes.data() + 8, &source->sin6_addr, 16);
		std::memcpy(bytes.data() + 24, &source->sin6_scope_id, 4);
	}
	return bytes;
}

uint8_t prefix_length(const sockaddr *mask)
{
	if (!mask)
		return 0;
	const uint8_t *bytes = mask->sa_family == AF_INET ? reinterpret_cast<const uint8_t *>(&reinterpret_cast<const sockaddr_in *>(mask)->sin_addr)
		: reinterpret_cast<const uint8_t *>(&reinterpret_cast<const sockaddr_in6 *>(mask)->sin6_addr);
	const size_t size = mask->sa_family == AF_INET ? 4 : 16;
	uint8_t bits = 0;
	for (size_t index = 0; index < size; ++index)
		bits = uint8_t(bits + __builtin_popcount(bytes[index]));
	return bits;
}

std::vector<Adapter> host_adapters(uint32_t family)
{
	std::vector<Adapter> adapters;
	ifaddrs *list = nullptr;
	if (::getifaddrs(&list))
		return adapters;
	std::map<std::string, size_t> found;
	for (const auto *entry = list; entry; entry = entry->ifa_next) {
		if (!entry->ifa_name)
			continue;
		auto position = found.find(entry->ifa_name);
		if (position == found.end()) {
			Adapter adapter;
			adapter.name = entry->ifa_name;
			adapter.index = ::if_nametoindex(entry->ifa_name);
			adapter.status = (entry->ifa_flags & IFF_UP) && (entry->ifa_flags & IFF_RUNNING) ? 1 : 2;
			adapter.type = (entry->ifa_flags & IFF_LOOPBACK) ? if_type_loopback :
				::access(("/sys/class/net/" + adapter.name + "/wireless").c_str(), F_OK) == 0 ? if_type_wireless : if_type_ethernet;
			if (const int probe = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0); probe >= 0) {
				ifreq request{};
				std::strncpy(request.ifr_name, entry->ifa_name, IFNAMSIZ - 1);
				if (::ioctl(probe, SIOCGIFMTU, &request) == 0)
					adapter.mtu = uint32_t(request.ifr_mtu);
				if (adapter.type != if_type_loopback && ::ioctl(probe, SIOCGIFHWADDR, &request) == 0) {
					std::memcpy(adapter.hardware, request.ifr_hwaddr.sa_data, 6);
					adapter.hardware_length = 6;
				}
				::close(probe);
			}
			position = found.emplace(adapter.name, adapters.size()).first;
			adapters.push_back(std::move(adapter));
		}
		if (!entry->ifa_addr || (entry->ifa_addr->sa_family != AF_INET && entry->ifa_addr->sa_family != AF_INET6))
			continue;
		if (family && family != uint32_t(entry->ifa_addr->sa_family == AF_INET ? AF_INET : windows_inet6))
			continue;
		adapters[position->second].unicast.push_back({windows_address(entry->ifa_addr), prefix_length(entry->ifa_netmask)});
	}
	::freeifaddrs(list);
	// A family filter keeps only the adapters that have an address of that family.
	if (family)
		adapters.erase(std::remove_if(adapters.begin(), adapters.end(), [](const Adapter &adapter) { return adapter.unicast.empty(); }), adapters.end());
	return adapters;
}

}

// The adapter list is built from the host interfaces. The records follow the IP_ADAPTER_ADDRESSES layout of the caller's pointer width.
void register_host_api(ApiRegistry &registry, KernelContext &context)
{
	auto &objects = context.objects;
	registry.add("GetAdaptersAddresses", 5, [](ApiCall &call) {
		const uint32_t family = call.u32(0), flags = call.u32(1);
		if (!call[4] || (family != 0 && family != AF_INET && family != windows_inet6)) {
			call.ret(error_invalid_parameter);
			return;
		}
		const size_t width = call.pointer_width();
		const auto adapters = host_adapters(family);
		if (adapters.empty()) {
			call.ret(error_no_data);
			return;
		}
		// Offsets in IP_ADAPTER_ADDRESSES_LH: the pointer fields follow the 64-bit alignment of the speeds and the 8-byte LUID.
		const auto align = [](size_t value, size_t boundary) { return (value + boundary - 1) & ~(boundary - 1); };
		size_t cursor = 8;
		const size_t next_at = cursor; cursor += width;
		const size_t name_at = cursor; cursor += width;
		const size_t unicast_at = cursor; cursor += width;
		cursor += 2 * width;
		const size_t dns_at = cursor; cursor += width;
		const size_t suffix_at = cursor; cursor += width;
		const size_t description_at = cursor; cursor += width;
		const size_t friendly_at = cursor; cursor += width;
		const size_t physical_at = cursor; cursor += 8;
		const size_t physical_length_at = cursor; cursor += 4;
		const size_t flags_at = cursor; cursor += 4;
		const size_t mtu_at = cursor; cursor += 4;
		const size_t type_at = cursor; cursor += 4;
		const size_t status_at = cursor; cursor += 4;
		const size_t index6_at = cursor; cursor += 4 + 64;
		cursor = align(cursor, width);
		cursor += width;
		cursor = align(cursor, 8);
		const size_t transmit_at = cursor; cursor += 16;
		cursor = align(cursor, width);
		cursor += 2 * width;
		const size_t metric4_at = cursor; cursor += 8;
		cursor = align(cursor, 8);
		cursor += 8;
		cursor = align(cursor, width);
		cursor += 2 * width;
		cursor += 4;
		cursor = align(cursor, 4);
		const size_t guid_at = cursor; cursor += 16;
		cursor += 8;
		cursor = align(cursor, width);
		cursor += 2 * width;
		cursor += 130;
		cursor = align(cursor, 4);
		cursor += 8;
		cursor = align(cursor, width);
		cursor += width;
		const size_t record = align(cursor, 8);
		// IP_ADAPTER_UNICAST_ADDRESS_LH: length, flags, next, the socket address, three origins, three lifetimes and the prefix length.
		size_t unicast_cursor = 8 + width;
		const size_t sockaddr_pointer_at = unicast_cursor; unicast_cursor += width;
		const size_t sockaddr_length_at = unicast_cursor; unicast_cursor += 4;
		unicast_cursor = align(unicast_cursor, width);
		const size_t origin_at = unicast_cursor; unicast_cursor += 24;
		const size_t prefix_at = unicast_cursor;
		const size_t unicast_record = align(prefix_at + 1, width);
		size_t needed = 0;
		for (const auto &adapter : adapters) {
			needed += record;
			if (!(flags & skip_unicast))
				for (const auto &address : adapter.unicast)
					needed += unicast_record + align(address.socket_address.size(), 8);
			needed += 2 * (adapter.name.size() + 1) + 2 * (adapter.name.size() + 1) + 64;
			needed = align(needed, 8);
		}
		const uint32_t capacity = call.load<uint32_t>(call[4]);
		if (!call[3] || capacity < needed) {
			call.store<uint32_t>(call[4], uint32_t(needed));
			call.ret(error_buffer_overflow);
			return;
		}
		std::vector<uint8_t> block(needed, 0);
		const uint64_t base = call[3];
		const auto put32 = [&](size_t at, uint32_t value) { std::memcpy(block.data() + at, &value, 4); };
		const auto put_pointer = [&](size_t at, uint64_t value) { std::memcpy(block.data() + at, &value, width); };
		size_t position = 0, previous = 0;
		for (size_t number = 0; number < adapters.size(); ++number) {
			const auto &adapter = adapters[number];
			const size_t self = position;
			position += record;
			put32(self, uint32_t(record));
			put32(self + 4, adapter.index);
			if (number)
				put_pointer(previous + next_at, base + self);
			previous = self;
			// The adapter name is a GUID string made from the interface index, stable for the interface.
			char guid[64];
			std::snprintf(guid, sizeof(guid), "{00000000-0000-0000-0000-%012x}", adapter.index);
			std::memcpy(block.data() + position, guid, std::strlen(guid) + 1);
			put_pointer(self + name_at, base + position);
			position += 48;
			for (const size_t at : {description_at, friendly_at}) {
				for (size_t character = 0; character < adapter.name.size(); ++character)
					block[position + 2 * character] = uint8_t(adapter.name[character]);
				put_pointer(self + at, base + position);
				position += 2 * (adapter.name.size() + 1);
			}
			put_pointer(self + suffix_at, base + position);
			position += 2;
			(void)dns_at;
			std::memcpy(block.data() + self + physical_at, adapter.hardware, 8);
			put32(self + physical_length_at, adapter.hardware_length);
			put32(self + flags_at, 0x10 | 0x80);
			put32(self + mtu_at, adapter.mtu);
			put32(self + type_at, adapter.type);
			put32(self + status_at, adapter.status);
			put32(self + index6_at, adapter.index);
			put32(self + metric4_at, 25);
			put32(self + metric4_at + 4, 25);
			(void)transmit_at;
			(void)guid_at;
			position = align(position, 8);
			if (!(flags & skip_unicast)) {
				size_t last = 0;
				for (const auto &address : adapter.unicast) {
					const size_t entry = position;
					position += unicast_record;
					put32(entry, uint32_t(unicast_record));
					put32(entry + 4, 0);
					const size_t stored = position;
					std::memcpy(block.data() + stored, address.socket_address.data(), address.socket_address.size());
					position += align(address.socket_address.size(), 8);
					put_pointer(entry + sockaddr_pointer_at, base + stored);
					put32(entry + sockaddr_length_at, uint32_t(address.socket_address.size()));
					block[entry + prefix_at] = address.prefix;
					put32(entry + origin_at, 3);
					put32(entry + origin_at + 4, 3);
					put32(entry + origin_at + 8, 4);
					put32(entry + origin_at + 12, 0xffffffffU);
					put32(entry + origin_at + 16, 0xffffffffU);
					put32(entry + origin_at + 20, 0xffffffffU);
					if (last)
						put_pointer(last + 8, base + entry);
					else
						put_pointer(self + unicast_at, base + entry);
					last = entry;
				}
			}
			position = align(position, 8);
		}
		call.write(base, std::span<const uint8_t>(block.data(), std::min(position, block.size())));
		(void)skip_anycast; (void)skip_multicast; (void)skip_dns_server;
		call.ret(0);
	});
	// The IPv4 adapters in IP_ADAPTER_INFO records, loopback left out; addresses past the first of an adapter are chained after the records.
	registry.add("GetAdaptersInfo", 2, [](ApiCall &call) {
		if (!call[1]) {
			call.ret(error_invalid_parameter);
			return;
		}
		auto adapters = host_adapters(AF_INET);
		adapters.erase(std::remove_if(adapters.begin(), adapters.end(), [](const Adapter &adapter) { return adapter.type == if_type_loopback; }),
			adapters.end());
		if (adapters.empty()) {
			call.ret(error_no_data);
			return;
		}
		const size_t width = call.pointer_width();
		const auto align = [](size_t value, size_t boundary) { return (value + boundary - 1) & ~(boundary - 1); };
		// IP_ADDR_STRING: next, the address and the mask as dotted text, a context.
		const size_t string_size = align(width + 36, width);
		const size_t name_at = width + 4, description_at = name_at + 260, address_length_at = description_at + 132, address_at = address_length_at + 4;
		const size_t index_at = address_at + 8, type_at = index_at + 4, current_at = align(type_at + 8, width);
		const size_t list_at = align(current_at + width, width), gateway_at = list_at + string_size, dhcp_at = gateway_at + string_size;
		const size_t wins_at = dhcp_at + string_size, primary_at = align(wins_at + 4, width), secondary_at = primary_at + string_size;
		const size_t record = secondary_at + string_size + 2 * width;
		size_t extra = 0;
		for (const auto &adapter : adapters)
			extra += (adapter.unicast.size() - 1) * string_size;
		const size_t needed = adapters.size() * record + extra;
		const uint32_t capacity = call.load<uint32_t>(call[1]);
		if (!call[0] || capacity < needed) {
			call.store<uint32_t>(call[1], uint32_t(needed));
			call.ret(error_buffer_overflow);
			return;
		}
		std::vector<uint8_t> block(needed, 0);
		const uint64_t base = call[0];
		const auto put32 = [&](size_t at, uint32_t value) { std::memcpy(block.data() + at, &value, 4); };
		const auto put_pointer = [&](size_t at, uint64_t value) { std::memcpy(block.data() + at, &value, width); };
		const auto put_text = [&](size_t at, const std::string &text) { std::memcpy(block.data() + at, text.data(), text.size()); };
		const auto dotted = [](const uint8_t *bytes) {
			return std::to_string(bytes[0]) + "." + std::to_string(bytes[1]) + "." + std::to_string(bytes[2]) + "." + std::to_string(bytes[3]);
		};
		const auto put_string = [&](size_t at, const uint8_t *address, uint8_t prefix) {
			const uint32_t mask = prefix ? htonl(~uint32_t(0) << (32 - prefix)) : 0;
			put_text(at + width, address ? dotted(address) : "0.0.0.0");
			put_text(at + width + 16, dotted(reinterpret_cast<const uint8_t *>(&mask)));
		};
		size_t chained = adapters.size() * record;
		for (size_t number = 0; number < adapters.size(); ++number) {
			const auto &adapter = adapters[number];
			const size_t self = number * record;
			if (number + 1 < adapters.size())
				put_pointer(self, base + self + record);
			put32(self + width, adapter.index);
			char guid[64];
			std::snprintf(guid, sizeof(guid), "{00000000-0000-0000-0000-%012x}", adapter.index);
			put_text(self + name_at, guid);
			put_text(self + description_at, adapter.name.substr(0, 131));
			put32(self + address_length_at, adapter.hardware_length);
			std::memcpy(block.data() + self + address_at, adapter.hardware, 8);
			put32(self + index_at, adapter.index);
			put32(self + type_at, adapter.type);
			size_t previous = self + list_at;
			for (size_t which = 0; which < adapter.unicast.size(); ++which) {
				const auto &address = adapter.unicast[which];
				size_t at = previous;
				if (which) {
					at = chained;
					chained += string_size;
					put_pointer(previous, base + at);
				}
				put_string(at, address.socket_address.data() + 4, address.prefix);
				previous = at;
			}
			for (const size_t at : {gateway_at, dhcp_at, primary_at, secondary_at})
				put_string(self + at, nullptr, 0);
		}
		call.write(base, block);
		call.ret(0);
	});
	// Winsock events are ordinary manual-reset events.
	registry.add("WSACreateEvent", 0, [&objects](ApiCall &call) {
		const auto made = objects.create_event(0x1f0003, 0, false);
		if (made.status) {
			call.fail(wsa_not_enough_memory, 0);
			return;
		}
		call.ret(made.value);
	});
	registry.add("WSASetEvent", 1, [&objects](ApiCall &call) {
		if (objects.set_event(call[0], true).status) {
			call.fail(wsa_invalid_handle, 0);
			return;
		}
		call.ret(1);
	});
	registry.add("WSAResetEvent", 1, [&objects](ApiCall &call) {
		if (objects.set_event(call[0], false).status) {
			call.fail(wsa_invalid_handle, 0);
			return;
		}
		call.ret(1);
	});
	registry.add("WSACloseEvent", 1, [&objects](ApiCall &call) {
		if (objects.close(call[0])) {
			call.fail(wsa_invalid_handle, 0);
			return;
		}
		call.ret(1);
	});
	// The Winsock wait is the wait for several objects with the same answers: the index of the event, WSA_WAIT_TIMEOUT or
	// WSA_WAIT_IO_COMPLETION.
	registry.add("WSAWaitForMultipleEvents", 5, [&registry](ApiCall &call) {
		const auto wait = registry.find("WaitForMultipleObjectsEx");
		ApiCall inner(call.process, call.environment, {call.u32(0), call[1], call.u32(2), call.u32(3), call.u32(4)});
		wait->second(inner);
		call.ret(inner.result);
	});
	// A change notification waits for an address change that is never reported: the host's interfaces are not watched.
	registry.add("NotifyAddrChange", 2, [](ApiCall &call) { call.ret(call[1] ? error_io_pending : error_not_supported); });
	registry.add("NotifyRouteChange", 2, [](ApiCall &call) { call.ret(call[1] ? error_io_pending : error_not_supported); });
	// The computer name is the host's, in the forms Windows reports it.
	const auto computer_name = [](uint32_t type) -> std::optional<std::u16string> {
		char host[256] = {};
		::gethostname(host, sizeof(host) - 1);
		std::string name(host);
		const auto dot = name.find('.');
		if (type == 0 || type == 4) {
			std::string netbios = name.substr(0, dot);
			for (auto &character : netbios)
				character = char(std::toupper(static_cast<unsigned char>(character)));
			netbios.resize(std::min<size_t>(netbios.size(), 15));
			return std::u16string(netbios.begin(), netbios.end());
		}
		if (type == 1 || type == 5)
			return std::u16string(name.begin(), name.begin() + std::string::difference_type(dot == std::string::npos ? name.size() : dot));
		if (type == 2 || type == 6)
			return dot == std::string::npos ? std::u16string() : std::u16string(name.begin() + std::string::difference_type(dot) + 1, name.end());
		if (type == 3 || type == 7)
			return std::u16string(name.begin(), name.end());
		return std::nullopt;
	};
	const auto report_name = [computer_name](ApiCall &call, uint32_t type, bool wide) {
		const auto name = computer_name(type);
		if (!name || !call[2]) {
			call.fail(error_invalid_parameter, 0);
			return;
		}
		const uint32_t capacity = call.load<uint32_t>(call[2]);
		if (!call[1] || capacity <= name->size()) {
			call.store<uint32_t>(call[2], uint32_t(name->size() + 1));
			call.fail(error_more_data, 0);
			return;
		}
		if (wide)
			call.put_wide(call[1], *name, capacity);
		else
			call.put_narrow(call[1], std::string(name->begin(), name->end()), capacity);
		call.store<uint32_t>(call[2], uint32_t(name->size()));
		call.ret(1);
	};
	registry.add("GetComputerNameExW", 3, [report_name](ApiCall &call) {
		// The name goes to the second argument and its length to the third, as in GetComputerNameW.
		report_name(call, call.u32(0), true);
	});
	// Network accounts: the one account of the prefix, a member of Administrators, on a machine in a workgroup. Buffers come from the
	// guest allocator and go back through NetApiBufferFree.
	registry.add("NetUserGetInfo", 4, [&context](ApiCall &call) {
		const auto user = context.paths.user_name();
		if (!call[3]) {
			call.ret(87);
			return;
		}
		call.store_pointer(call[3], 0);
		const auto asked = call[1] ? call.wide(call[1], 256) : std::u16string();
		const auto fold = [](std::u16string text) {
			for (auto &character : text)
				if (character >= u'a' && character <= u'z')
					character = char16_t(character - 32);
			return text;
		};
		if (fold(asked) != fold(user)) {
			call.ret(2221);
			return;
		}
		const uint32_t level = call.u32(2);
		if (level != 0 && level != 1) {
			call.ret(124);
			return;
		}
		const size_t width = call.pointer_width();
		const size_t record = level == 0 ? width : width == 4 ? 32 : 56;
		const size_t total = record + (user.size() + 1) * 2 + 2;
		const uint64_t block = context.allocator.allocate(total);
		if (!block) {
			call.ret(8);
			return;
		}
		const uint64_t name = block + record, empty = name + (user.size() + 1) * 2;
		call.put_wide(name, user, user.size() + 1);
		call.store<uint16_t>(empty, 0);
		call.store_pointer(block, name);
		if (level == 1) {
			const size_t word = width;
			call.store_pointer(block + word, 0);
			call.store<uint32_t>(block + 2 * word, 0);
			call.store<uint32_t>(block + 2 * word + 4, 2);
			call.store_pointer(block + 2 * word + 8, empty);
			call.store_pointer(block + 3 * word + 8, empty);
			call.store<uint32_t>(block + 4 * word + 8, 0x10201);
			call.store_pointer(block + (width == 4 ? 28 : 48), empty);
		}
		call.store_pointer(call[3], block);
		call.ret(0);
	});
	registry.add("NetGetJoinInformation", 3, [&context](ApiCall &call) {
		if (!call[1] || !call[2]) {
			call.ret(87);
			return;
		}
		const std::u16string workgroup = u"WORKGROUP";
		const uint64_t block = context.allocator.allocate((workgroup.size() + 1) * 2);
		if (!block) {
			call.ret(8);
			return;
		}
		call.put_wide(block, workgroup, workgroup.size() + 1);
		call.store_pointer(call[1], block);
		call.store<uint32_t>(call[2], 2);
		call.ret(0);
	});
	registry.add("NetApiBufferFree", 1, [&context](ApiCall &call) {
		if (call[0])
			context.allocator.release(call[0]);
		call.ret(0);
	});
	// A local profile that stays (PT_LOCAL is zero).
	registry.add("GetProfileType", 1, [](ApiCall &call) {
		if (!call[0]) {
			call.fail(87, 0);
			return;
		}
		call.store<uint32_t>(call[0], 0);
		call.ret(1);
	});
	registry.add("CancelIPChangeNotify", 1, [](ApiCall &call) { call.ret(1); });
	// No enclave technology is present.
	registry.add("IsEnclaveTypeSupported", 1, [](ApiCall &call) { call.fail(50, 0); });
	// Logging on as another account needs credentials this machine cannot check.
	registry.add("LogonUserW", 6, [](ApiCall &call) {
		if (call[5])
			call.store_pointer(call[5], 0);
		call.fail(1326, 0);
	});
	registry.add("IsUserCetAvailableInEnvironment", 1, [](ApiCall &call) { call.ret(0); });
	// Group policy never changes here, so a registration has nothing to signal.
	registry.add("RegisterGPNotification", 2, [](ApiCall &call) { call.ret(1); });
	registry.add("UnregisterGPNotification", 1, [](ApiCall &call) { call.ret(1); });
	// Performance counters: there is no counter provider, so a query cannot be made (PDH_CSTATUS_NO_OBJECT).
	registry.add("PdhOpenQueryW", 3, [](ApiCall &call) {
		if (call[2])
			call.store_pointer(call[2], 0);
		call.ret(0xc0000bb8U);
	});
	// The account name: the size counts the terminator, both on the way in and when the buffer is too small.
	for (const bool wide : {true, false})
		registry.add(wide ? "GetUserNameW" : "GetUserNameA", 2, [&context, wide](ApiCall &call) {
			const auto name = context.paths.user_name();
			if (!call[1]) {
				call.fail(error_invalid_parameter, 0);
				return;
			}
			const uint32_t capacity = call.load<uint32_t>(call[1]);
			if (!call[0] || capacity <= name.size()) {
				call.store<uint32_t>(call[1], uint32_t(name.size() + 1));
				call.fail(122, 0);
				return;
			}
			if (wide)
				call.put_wide(call[0], name, capacity);
			else
				call.put_narrow(call[0], to_utf8(name), capacity);
			call.store<uint32_t>(call[1], uint32_t(name.size() + 1));
			call.ret(1);
		});
	// The account in the formats a machine outside a domain has: SAM-compatible (computer\user) and the display name.
	registry.add("GetUserNameExW", 3, [&context, computer_name](ApiCall &call) {
		const auto user = context.paths.user_name();
		std::u16string name;
		if (call.u32(0) == 2)
			name = computer_name(0).value_or(u"RAVEN") + u"\\" + user;
		else if (call.u32(0) == 3)
			name = user;
		else {
			call.fail(1332, 0);
			return;
		}
		if (!call[2]) {
			call.fail(error_invalid_parameter, 0);
			return;
		}
		const uint32_t capacity = call.load<uint32_t>(call[2]);
		if (!call[1] || capacity <= name.size()) {
			call.store<uint32_t>(call[2], uint32_t(name.size() + 1));
			call.fail(error_more_data, 0);
			return;
		}
		call.put_wide(call[1], name, capacity);
		call.store<uint32_t>(call[2], uint32_t(name.size()));
		call.ret(1);
	});
	// The machine is in a workgroup, not a domain: its directory object has no name, as on a Windows computer that joined none.
	registry.add("GetComputerObjectNameW", 3, [](ApiCall &call) { call.fail(1351, 0); });
	registry.add("GetComputerObjectNameA", 3, [](ApiCall &call) { call.fail(1351, 0); });
	// A buffer too small for the name, or none, gets the size it needs and ERROR_BUFFER_OVERFLOW (GetComputerNameEx answers ERROR_MORE_DATA).
	registry.add("GetComputerNameW", 2, [computer_name](ApiCall &call) {
		const auto name = computer_name(0);
		if (!call[1]) {
			call.fail(error_invalid_parameter, 0);
			return;
		}
		const uint32_t capacity = call.load<uint32_t>(call[1]);
		if (!call[0] || capacity <= name->size()) {
			call.store<uint32_t>(call[1], uint32_t(name->size() + 1));
			call.fail(111, 0);
			return;
		}
		call.put_wide(call[0], *name, capacity);
		call.store<uint32_t>(call[1], uint32_t(name->size()));
		call.ret(1);
	});
	// A buffer too small for the name, or none, gets the size it needs and ERROR_BUFFER_OVERFLOW (GetComputerNameEx answers ERROR_MORE_DATA).
	registry.add("GetComputerNameA", 2, [computer_name](ApiCall &call) {
		const auto name = computer_name(0);
		if (!call[1]) {
			call.fail(error_invalid_parameter, 0);
			return;
		}
		const uint32_t capacity = call.load<uint32_t>(call[1]);
		if (!call[0] || capacity <= name->size()) {
			call.store<uint32_t>(call[1], uint32_t(name->size() + 1));
			call.fail(111, 0);
			return;
		}
		call.put_narrow(call[0], std::string(name->begin(), name->end()), capacity);
		call.store<uint32_t>(call[1], uint32_t(name->size()));
		call.ret(1);
	});
	// Device enumeration finds nothing: the set opens, holds no interfaces and closes.
	// The setup classes Windows installs and their GUIDs; a name resolves to its class, compared without regard to case.
	static const std::pair<std::u16string_view, std::array<uint8_t, 16>> setup_classes[] = {
		{u"DiskDrive", {0x67, 0xe9, 0x36, 0x4d, 0x25, 0xe3, 0xce, 0x11, 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}},
		{u"Display", {0x68, 0xe9, 0x36, 0x4d, 0x25, 0xe3, 0xce, 0x11, 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}},
		{u"Keyboard", {0x6b, 0xe9, 0x36, 0x4d, 0x25, 0xe3, 0xce, 0x11, 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}},
		{u"Media", {0x6c, 0xe9, 0x36, 0x4d, 0x25, 0xe3, 0xce, 0x11, 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}},
		{u"Monitor", {0x6e, 0xe9, 0x36, 0x4d, 0x25, 0xe3, 0xce, 0x11, 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}},
		{u"Mouse", {0x6f, 0xe9, 0x36, 0x4d, 0x25, 0xe3, 0xce, 0x11, 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}},
		{u"Net", {0x72, 0xe9, 0x36, 0x4d, 0x25, 0xe3, 0xce, 0x11, 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}},
		{u"Ports", {0x78, 0xe9, 0x36, 0x4d, 0x25, 0xe3, 0xce, 0x11, 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}},
		{u"System", {0x7d, 0xe9, 0x36, 0x4d, 0x25, 0xe3, 0xce, 0x11, 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}},
		{u"HIDClass", {0xa0, 0x17, 0x5a, 0x74, 0xd3, 0x74, 0xd0, 0x11, 0xb6, 0xfe, 0x00, 0xa0, 0xc9, 0x0f, 0x57, 0xda}},
		{u"USB", {0x60, 0x9e, 0xfc, 0x36, 0x65, 0xc4, 0xcf, 0x11, 0x80, 0x56, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}},
		{u"Processor", {0xc3, 0x7d, 0x12, 0x50, 0x36, 0x0f, 0x5e, 0x41, 0xa6, 0xcc, 0x4c, 0xb3, 0xbe, 0x91, 0x0b, 0x65}},
		{u"AudioEndpoint", {0x18, 0xe1, 0xfc, 0xc1, 0xdf, 0xa7, 0x7a, 0x4a, 0xb9, 0xe5, 0x2e, 0x7c, 0x76, 0x4e, 0x8b, 0x52}},
	};
	// SetupDiClassGuidsFromName[Ex](name, list, size, required[, machine, reserved]): the GUIDs of the classes with that name.
	const auto class_guids = [](ApiCall &call, bool ansi) {
		if (!call[0] || !call[3]) {
			call.fail(87, 0);
			return;
		}
		const auto name = ansi ? [&] { const auto text = call.narrow(call[0], 64); return std::u16string(text.begin(), text.end()); }() : call.wide(call[0], 64);
		const auto fold = [](std::u16string_view text) {
			std::u16string lower(text);
			for (auto &c : lower)
				if (c >= u'A' && c <= u'Z')
					c = char16_t(c + 32);
			return lower;
		};
		std::vector<const std::array<uint8_t, 16> *> found;
		for (const auto &[class_name, guid] : setup_classes)
			if (fold(class_name) == fold(name))
				found.push_back(&guid);
		call.store<uint32_t>(call[3], uint32_t(found.size()));
		if (found.size() > call.u32(2)) {
			call.fail(122, 0);
			return;
		}
		for (size_t index = 0; index < found.size(); ++index)
			call.write(call[1] + index * 16, std::vector<uint8_t>(found[index]->begin(), found[index]->end()));
		call.ret(1);
	};
	registry.add("SetupDiClassGuidsFromNameW", 4, [class_guids](ApiCall &call) { class_guids(call, false); });
	registry.add("SetupDiClassGuidsFromNameA", 4, [class_guids](ApiCall &call) { class_guids(call, true); });
	registry.add("SetupDiClassGuidsFromNameExW", 6, [class_guids](ApiCall &call) { class_guids(call, false); });
	registry.add("SetupDiClassGuidsFromNameExA", 6, [class_guids](ApiCall &call) { class_guids(call, true); });
	registry.add("SetupDiGetClassDevsW", 4, [&objects](ApiCall &call) {
		const auto made = objects.add_object(std::make_shared<DeviceInfoSet>());
		if (made.status) {
			call.fail(8, UINT64_MAX);
			return;
		}
		call.ret(made.value);
	});
	// The device sets here are empty: enumerating their devices ends at once, and every interface list holds only its terminator.
	registry.add("SetupDiEnumDeviceInfo", 3, [&objects](ApiCall &call) {
		if (!objects.object_as<DeviceInfoSet>(call[0])) {
			call.fail(6, 0);
			return;
		}
		call.fail(error_no_more_items, 0);
	});
	for (const char *name : {"CM_Get_Device_Interface_List_SizeW", "CM_Get_Device_Interface_List_SizeA"})
		registry.add(name, 4, [](ApiCall &call) {
			if (!call[0]) {
				call.ret(3);
				return;
			}
			call.store<uint32_t>(call[0], 1);
			call.ret(0);
		});
	registry.add("CM_Get_Device_Interface_ListW", 5, [](ApiCall &call) {
		if (!call[2] || !call.u32(3)) {
			call.ret(3);
			return;
		}
		call.store<uint16_t>(call[2], 0);
		call.ret(0);
	});
	registry.add("CM_Get_Device_Interface_ListA", 5, [](ApiCall &call) {
		if (!call[2] || !call.u32(3)) {
			call.ret(3);
			return;
		}
		call.store<uint8_t>(call[2], 0);
		call.ret(0);
	});
	registry.add("SetupDiEnumDeviceInterfaces", 5, [&objects](ApiCall &call) {
		if (!objects.object_as<DeviceInfoSet>(call[0])) {
			call.fail(6, 0);
			return;
		}
		call.fail(error_no_more_items, 0);
	});
	registry.add("SetupDiDestroyDeviceInfoList", 1, [&objects](ApiCall &call) {
		if (!objects.object_as<DeviceInfoSet>(call[0])) {
			call.fail(6, 0);
			return;
		}
		objects.close(call[0]);
		call.ret(1);
	});
	registry.add("EvtQuery", 4, [&objects](ApiCall &call) {
		const auto made = objects.add_object(std::make_shared<EventQuery>());
		if (made.status) {
			call.fail(8, 0);
			return;
		}
		call.ret(made.value);
	});
	registry.add("EvtNext", 6, [&objects](ApiCall &call) {
		if (!objects.object_as<EventQuery>(call[0])) {
			call.fail(6, 0);
			return;
		}
		if (call[4])
			call.store<uint32_t>(call[4], 0);
		call.fail(error_no_more_items, 0);
	});
	registry.add("EvtClose", 1, [&objects](ApiCall &call) {
		if (!objects.object_as<EventQuery>(call[0])) {
			call.fail(6, 0);
			return;
		}
		objects.close(call[0]);
		call.ret(1);
	});
	// A device context has no color profile assigned.
	registry.add("GetICMProfileW", 3, [](ApiCall &call) { call.fail(2, 0); });
	// Device notifications are registered and never sent: the host reports no device arrivals to the program.
	registry.add("RegisterDeviceNotificationW", 3, [&objects](ApiCall &call) {
		const auto made = objects.add_object(std::make_shared<DeviceInfoSet>());
		if (made.status) {
			call.fail(8, 0);
			return;
		}
		call.ret(made.value);
	});
	registry.add("RegisterDeviceNotificationA", 3, [&objects](ApiCall &call) {
		const auto made = objects.add_object(std::make_shared<DeviceInfoSet>());
		if (made.status) {
			call.fail(8, 0);
			return;
		}
		call.ret(made.value);
	});
	registry.add("UnregisterDeviceNotification", 1, [&objects](ApiCall &call) {
		if (!objects.object_as<DeviceInfoSet>(call[0])) {
			call.fail(6, 0);
			return;
		}
		objects.close(call[0]);
		call.ret(1);
	});
	registry.add("EvtCreateRenderContext", 4, [&objects](ApiCall &call) {
		const auto made = objects.add_object(std::make_shared<EventQuery>());
		if (made.status) {
			call.fail(8, 0);
			return;
		}
		call.ret(made.value);
	});
	// What the thread's queue holds: the high word the kinds of message waiting, the low word the ones that arrived since the last call.
	registry.add("GetQueueStatus", 1, [&context](ApiCall &call) {
		const uint32_t kinds = call.u32(0);
		if (kinds & ~0x1dffU) { call.fail(87, 0); return; }
		call.ret(context.queue_status ? context.queue_status(context.threads.current_id(), kinds, true) : 0);
	});
	// Only keyboard and mouse-button input counts here, not posted messages, motion, timers or paint.
	registry.add("GetInputState", 0, [&context](ApiCall &call) {
		call.ret(context.queue_status && (context.queue_status(context.threads.current_id(), 5, false) >> 16) ? 1 : 0);
	});
	// Whether the message being handled was sent from another thread that waits for the reply: the window procedures of a program run
	// on the thread that sent to them, so the answer is no (ISMEX_NOSEND).
	registry.add("InSendMessage", 0, [](ApiCall &call) { call.ret(0); });
	// The message filter that keeps lower integrity processes out: the prefix has no integrity levels, so every change is accepted
	// (the per-window form is with the window functions).
	registry.add("ChangeWindowMessageFilter", 2, [](ApiCall &call) { call.ret(1); });
	registry.add("InSendMessageEx", 1, [](ApiCall &call) { call.ret(0); });
	// Power information: the processors with their frequencies from the host, a machine on mains power with no battery, and no sleep states.
	registry.add("CallNtPowerInformation", 5, [](ApiCall &call) {
		constexpr uint32_t status_success = 0, status_buffer_too_small = 0xc0000023U, status_invalid_parameter = 0xc000000dU;
		const uint32_t level = call.u32(0);
		std::vector<uint8_t> data;
		if (level == 11) {
			// PROCESSOR_POWER_INFORMATION per processor: number, maximum, current and limit in MHz, idle states.
			const long count = std::max<long>(1, ::sysconf(_SC_NPROCESSORS_ONLN));
			for (long index = 0; index < count; ++index) {
				const auto read_khz = [&](const char *file) -> uint32_t {
					std::ifstream input("/sys/devices/system/cpu/cpu" + std::to_string(index) + "/cpufreq/" + file);
					uint64_t khz = 0;
					input >> khz;
					return uint32_t(khz / 1000);
				};
				uint32_t maximum = read_khz("cpuinfo_max_freq"), current = read_khz("scaling_cur_freq");
				if (!maximum) {
					std::ifstream cpuinfo("/proc/cpuinfo");
					for (std::string line; std::getline(cpuinfo, line);)
						if (line.starts_with("cpu MHz")) {
							maximum = uint32_t(std::stod(line.substr(line.find(':') + 1)));
							break;
						}
				}
				if (!current)
					current = maximum;
				const uint32_t fields[6] = {uint32_t(index), maximum, current, maximum, 0, 0};
				const auto raw = reinterpret_cast<const uint8_t *>(fields);
				data.insert(data.end(), raw, raw + sizeof(fields));
			}
		} else if (level == 5) {
			// SYSTEM_BATTERY_STATE: AC line on, no battery present.
			data.assign(32, 0);
			data[0] = 1;
		} else if (level == 4) {
			data.assign(76, 0);
		} else {
			call.ret(status_invalid_parameter);
			return;
		}
		if (!call[3] || call.u32(4) < data.size()) {
			call.ret(status_buffer_too_small);
			return;
		}
		call.write(call[3], data);
		call.ret(status_success);
	});
	// Interrupt time is the monotonic clock in 100 ns ticks, which does not run while the host sleeps.
	const auto interrupt_time = [](ApiCall &call) {
		if (!call[0]) {
			call.fail(87);
			return;
		}
		timespec now{};
		::clock_gettime(CLOCK_MONOTONIC, &now);
		call.store<uint64_t>(call[0], (uint64_t(now.tv_sec) * 1000000000ULL + uint64_t(now.tv_nsec)) / 100);
		call.ret(1);
	};
	registry.add("QueryUnbiasedInterruptTime", 1, interrupt_time);
	registry.add("QueryUnbiasedInterruptTimePrecise", 1, [interrupt_time](ApiCall &call) {
		interrupt_time(call);
		call.ret(1);
	});
	registry.add("QueryInterruptTimePrecise", 1, [](ApiCall &call) {
		if (!call[0]) {
			call.fail(87);
			return;
		}
		timespec now{};
		::clock_gettime(CLOCK_BOOTTIME, &now);
		call.store<uint64_t>(call[0], (uint64_t(now.tv_sec) * 1000000000ULL + uint64_t(now.tv_nsec)) / 100);
	});
	// The user's region is United States; the keyboard layout is US English.
	registry.add("GetUserGeoID", 1, [](ApiCall &call) { call.ret(244); });
	// The region's two-letter code; the count includes the terminator, and a call without a buffer asks for it.
	registry.add("GetUserDefaultGeoName", 2, [](ApiCall &call) {
		constexpr std::u16string_view name = u"US";
		if (!call[0] || call.i32(1) <= 0) {
			call.ret(call.i32(1) == 0 ? uint32_t(name.size() + 1) : 0);
			return;
		}
		if (size_t(call.i32(1)) <= name.size()) {
			call.fail(122, 0);
			return;
		}
		call.put_wide(call[0], name, size_t(call.i32(1)));
		call.ret(uint32_t(name.size() + 1));
	});
	registry.add("GetGeoInfoW", 5, [](ApiCall &call) {
		if (call.u32(0) != 244) {
			call.fail(87, 0);
			return;
		}
		std::u16string text;
		switch (call.u32(1)) {
		case 4: text = u"US"; break;
		case 5: text = u"USA"; break;
		case 8: text = u"United States"; break;
		case 9: text = u"United States"; break;
		default:
			call.fail(87, 0);
			return;
		}
		if (call[2] && call.i32(3) > 0) {
			if (size_t(call.i32(3)) <= text.size()) {
				call.fail(122, 0);
				return;
			}
			call.put_wide(call[2], text, size_t(call.i32(3)));
		}
		call.ret(text.size() + 1);
	});
	registry.add("GetKeyboardLayoutList", 2, [](ApiCall &call) {
		if (call.i32(0) > 0 && call[1])
			call.store_pointer(call[1], 0x04090409);
		call.ret(1);
	});
	// The drives the prefix has: C: and Z: always, and any other letter with a folder behind it. GetLogicalDriveStrings lists their roots.
	const auto drives = [&context] {
		uint32_t mask = 0;
		for (char16_t letter = u'A'; letter <= u'Z'; ++letter) {
			const std::u16string root{letter, u':', u'\\'};
			if (const auto resolved = context.paths.resolve(root); resolved && resolved->exists)
				mask |= 1u << (letter - u'A');
		}
		return mask;
	};
	registry.add("GetLogicalDrives", 0, [drives](ApiCall &call) { call.ret(drives()); });
	// QueryDosDevice: each drive of the prefix is a volume device, \Device\HarddiskVolumeN in letter order; without a name, the list of
	// the drives' device names.
	for (const bool wide : {true, false})
		registry.add(wide ? "QueryDosDeviceW" : "QueryDosDeviceA", 3, [drives, wide](ApiCall &call) {
			const uint32_t mask = drives(), capacity = call.u32(2);
			std::u16string result;
			if (!call[0]) {
				for (char16_t letter = u'A'; letter <= u'Z'; ++letter)
					if (mask & (1u << (letter - u'A')))
						result += std::u16string{letter, u':', 0};
			} else {
				const auto name = wide ? call.wide(call[0], 64) : widen_ascii(call.narrow(call[0], 64));
				const char16_t letter = name.size() == 2 && name[1] == u':' ? char16_t(name[0] & ~0x20) : 0;
				if (!letter || letter < u'A' || letter > u'Z' || !(mask & (1u << (letter - u'A')))) {
					call.fail(2);
					return;
				}
				uint32_t number = 1;
				for (char16_t other = u'A'; other < letter; ++other)
					if (mask & (1u << (other - u'A')))
						++number;
				result = u"\\Device\\HarddiskVolume" + widen_ascii(std::to_string(number)) + std::u16string(1, 0);
			}
			result.push_back(0);
			if (capacity < result.size()) {
				call.fail(122);
				return;
			}
			if (wide) {
				std::vector<uint8_t> bytes(result.size() * 2);
				std::memcpy(bytes.data(), result.data(), bytes.size());
				call.write(call[1], bytes);
			} else {
				std::vector<uint8_t> bytes(result.begin(), result.end());
				call.write(call[1], bytes);
			}
			call.ret(result.size());
		});
	for (const bool wide : {true, false})
		registry.add(wide ? "GetLogicalDriveStringsW" : "GetLogicalDriveStringsA", 2, [drives, wide](ApiCall &call) {
			std::u16string text;
			const uint32_t mask = drives();
			for (char16_t letter = u'A'; letter <= u'Z'; ++letter)
				if (mask & (1u << (letter - u'A')))
					text += std::u16string{letter, u':', u'\\', 0};
			if (call.u32(0) <= text.size()) {
				call.ret(text.size() + 1);
				return;
			}
			if (wide) call.put_wide(call[1], text, text.size() + 1);
			else call.put_narrow(call[1], to_utf8(text), text.size() + 1);
			call.ret(text.size());
		});
	// A drive's volume: the prefix keeps the files, so every drive reports one NTFS volume.
	registry.add("GetVolumeInformationW", 8, [&context](ApiCall &call) {
		if (call[0]) {
			const auto root = context.paths.resolve(call.wide(call[0], 260));
			if (!root || !root->exists) {
				call.fail(3);
				return;
			}
		}
		if (call[1] && call.u32(2))
			call.put_wide(call[1], u"", call.u32(2));
		if (call[3])
			call.store<uint32_t>(call[3], 0x52415645);
		if (call[4])
			call.store<uint32_t>(call[4], 255);
		if (call[5])
			call.store<uint32_t>(call[5], 0x0000000f);
		if (call[6] && call.u32(7))
			call.put_wide(call[6], u"NTFS", call.u32(7));
		call.ret(1);
	});
	registry.add("GetVolumeInformationA", 8, [&context](ApiCall &call) {
		if (call[0]) {
			const auto root = context.paths.resolve(widen_ascii(call.narrow(call[0], 260)));
			if (!root || !root->exists) {
				call.fail(3);
				return;
			}
		}
		if (call[1] && call.u32(2))
			call.put_narrow(call[1], "", call.u32(2));
		if (call[3])
			call.store<uint32_t>(call[3], 0x52415645);
		if (call[4])
			call.store<uint32_t>(call[4], 255);
		if (call[5])
			call.store<uint32_t>(call[5], 0x0000000f);
		if (call[6] && call.u32(7))
			call.put_narrow(call[6], "NTFS", call.u32(7));
		call.ret(1);
	});
	// DPI awareness contexts are the pseudo handles -1 to -5; the process is system DPI aware at 96 dots per inch.
	const auto awareness_of = [](uint64_t context) -> int32_t {
		switch (int64_t(context)) {
		case -1: case -5: return 0;
		case -2: return 1;
		case -3: case -4: return 2;
		default: return -1;
		}
	};
	registry.add("GetAwarenessFromDpiAwarenessContext", 1, [awareness_of](ApiCall &call) { call.ret(uint32_t(awareness_of(call[0]))); });
	// Two contexts are equal when they are the same context, not when they share an awareness: per-monitor aware and its second version,
	// unaware and unaware with GDI scaling, are different contexts (WPF tells them apart to keep the bounds of each display per context).
	registry.add("AreDpiAwarenessContextsEqual", 2, [awareness_of](ApiCall &call) {
		if (awareness_of(call[0]) < 0 || awareness_of(call[1]) < 0) {
			call.fail(87);
			return;
		}
		call.ret(int64_t(call[0]) == int64_t(call[1]));
	});
	registry.add("GetWindowDpiAwarenessContext", 1, [](ApiCall &call) { call.ret(uint64_t(-2)); });
	registry.add("GetClassWord", 2, [](ApiCall &call) { call.ret(0); });
	// The cursor is whatever the program set last; none is shown by the host.
	registry.add("GetCursor", 0, [](ApiCall &call) { call.ret(0); });
	// The root of a path's volume: the drive letter and a separator.
	registry.add("GetVolumePathNameW", 3, [](ApiCall &call) {
		if (!call[0] || !call[1]) {
			call.fail(87);
			return;
		}
		const auto path = call.wide(call[0], 32768);
		std::u16string root;
		if (path.size() >= 2 && path[1] == u':')
			root = path.substr(0, 2) + u"\\";
		else
			root = u"C:\\";
		if (call.u32(2) <= root.size()) {
			call.fail(122);
			return;
		}
		call.put_wide(call[1], root, call.u32(2));
		call.ret(1);
	});
	// Writing a crash dump is not provided.
	registry.add("MiniDumpWriteDump", 7, [](ApiCall &call) { call.fail(120); });
	// Window event hooks are accepted and never called: the host reports no accessibility events.
	registry.add("SetWinEventHook", 7, [&objects](ApiCall &call) {
		const auto made = objects.add_object(std::make_shared<DeviceInfoSet>());
		if (made.status) {
			call.fail(8, 0);
			return;
		}
		call.ret(made.value);
	});
	registry.add("UnhookWinEvent", 1, [&objects](ApiCall &call) {
		if (!objects.object_as<DeviceInfoSet>(call[0])) {
			call.fail(6, 0);
			return;
		}
		objects.close(call[0]);
		call.ret(1);
	});
	// Every process runs in session 1, the interactive one.
	registry.add("ProcessIdToSessionId", 2, [](ApiCall &call) {
		if (!call[1]) {
			call.fail(87);
			return;
		}
		call.store<uint32_t>(call[1], 1);
		call.ret(1);
	});
	const auto set_dll_directory = [&context](ApiCall &call, bool wide) {
		std::optional<std::u16string> path;
		if (call[0]) path = wide ? call.wide(call[0]) : from_ansi(call.narrow(call[0]), context.launch.ansi_code_page);
		if (!context.paths.set_dll_directory(std::move(path))) { call.fail(123); return; }
		call.ret(1);
	};
	registry.add("SetDllDirectoryW", 1, [set_dll_directory](ApiCall &call) { set_dll_directory(call, true); });
	registry.add("SetDllDirectoryA", 1, [set_dll_directory](ApiCall &call) { set_dll_directory(call, false); });
	// The interface a packet to an address would leave by: the first adapter that is up and is not the loopback one, else the loopback.
	const auto best_interface = [](ApiCall &call) {
		if (!call[1]) {
			call.fail(87);
			return;
		}
		uint32_t chosen = 0, loopback = 0;
		for (const auto &adapter : host_adapters(0)) {
			if (adapter.status != 1)
				continue;
			if (adapter.type == if_type_loopback)
				loopback = adapter.index;
			else if (!chosen)
				chosen = adapter.index;
		}
		if (!chosen)
			chosen = loopback;
		if (!chosen) {
			call.ret(1231);
			return;
		}
		call.store<uint32_t>(call[1], chosen);
		call.ret(0);
	};
	registry.add("GetBestInterface", 2, best_interface);
	registry.add("GetBestInterfaceEx", 2, best_interface);
	// The interfaces as MIB_IFROW records, the shape the older table calls and GetIfEntry report them in: the name the TCP/IP stack knows
	// an interface by, its index and kind, and what it carries. A program that asks how the host is networked reads these before it opens
	// a socket, and one that finds no row cannot tell a missing interface from a missing network.
	const auto fill_if_row = [](std::vector<uint8_t> &row, const Adapter &adapter) {
		const auto put32 = [&](size_t at, uint32_t value) { std::memcpy(row.data() + at, &value, 4); };
		// \DEVICE\TCPIP_{index}: Windows names an interface by a GUID, and every interface here has an index it answers to.
		char name[64];
		const int written = std::snprintf(name, sizeof name, "\\DEVICE\\TCPIP_{00000000-0000-0000-0000-%012u}", adapter.index);
		std::u16string wide;
		for (int at = 0; at < written && at < int(sizeof name); ++at)
			wide.push_back(char16_t(uint8_t(name[at])));
		std::memcpy(row.data(), wide.data(), std::min(wide.size(), size_t(255)) * 2);
		put32(512, adapter.index);
		put32(516, adapter.type);
		put32(520, adapter.mtu);
		put32(524, adapter.type == if_type_loopback ? 10000000 : 1000000000);
		put32(528, adapter.hardware_length);
		std::memcpy(row.data() + 532, adapter.hardware, std::min<uint32_t>(adapter.hardware_length, 8));
		put32(540, 1);
		put32(544, adapter.status == 1 ? 1 : 4);
		const size_t description = std::min<size_t>(adapter.name.size(), 255);
		put32(600, uint32_t(description));
		std::memcpy(row.data() + 604, adapter.name.data(), description);
	};
	registry.add("GetIfEntry", 1, [fill_if_row](ApiCall &call) {
		if (!call[0]) {
			call.fail(87);
			return;
		}
		const uint32_t index = call.load<uint32_t>(call[0] + 512);
		const auto adapters = host_adapters(0);
		const auto adapter = std::find_if(adapters.begin(), adapters.end(), [&](const Adapter &item) { return item.index == index; });
		if (!index || adapter == adapters.end()) {
			call.fail(87);
			return;
		}
		std::vector<uint8_t> row(860, 0);
		fill_if_row(row, *adapter);
		call.write(call[0], std::span<const uint8_t>(row.data(), row.size()));
		call.ret(0);
	});
	registry.add("GetNumberOfInterfaces", 1, [](ApiCall &call) {
		if (!call[0]) {
			call.fail(87);
			return;
		}
		call.store<uint32_t>(call[0], uint32_t(host_adapters(0).size()));
		call.ret(0);
	});
	// GetIfTable(table, size, order): a count, then the rows; the size the caller holds is given back when it is too small.
	registry.add("GetIfTable", 3, [fill_if_row](ApiCall &call) {
		if (!call[1]) {
			call.fail(87);
			return;
		}
		auto adapters = host_adapters(0);
		if (call[2])
			std::sort(adapters.begin(), adapters.end(), [](const Adapter &left, const Adapter &right) { return left.index < right.index; });
		const size_t needed = 4 + adapters.size() * 860;
		const uint32_t capacity = call.load<uint32_t>(call[1]);
		if (!call[0] || capacity < needed) {
			call.store<uint32_t>(call[1], uint32_t(needed));
			call.ret(error_buffer_overflow);
			return;
		}
		std::vector<uint8_t> table(needed, 0);
		const uint32_t count = uint32_t(adapters.size());
		std::memcpy(table.data(), &count, 4);
		for (size_t number = 0; number < adapters.size(); ++number) {
			const size_t at = 4 + number * 860;
			std::vector<uint8_t> row(860, 0);
			fill_if_row(row, adapters[number]);
			std::memcpy(table.data() + at, row.data(), row.size());
		}
		call.write(call[0], std::span<const uint8_t>(table.data(), table.size()));
		call.store<uint32_t>(call[1], uint32_t(needed));
		call.ret(0);
	});
	// The class of human interface devices; none is present.
	registry.add("HidD_GetHidGuid", 1, [](ApiCall &call) {
		static const uint8_t guid[16] = {0xb2, 0x55, 0x1e, 0x4d, 0x6f, 0xf1, 0xcf, 0x11, 0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30};
		if (call[0])
			call.write(call[0], std::span<const uint8_t>(guid, 16));
	});
	// Device interface notifications are accepted and never sent.
	registry.add("CM_Register_Notification", 4, [&objects](ApiCall &call) {
		const auto made = objects.add_object(std::make_shared<DeviceInfoSet>());
		if (made.status || !call[2]) {
			call.ret(made.status ? 0x3 : 0x1b);
			return;
		}
		call.store_pointer(call[2], made.value);
		call.ret(0);
	});
	registry.add("CM_Unregister_Notification", 1, [&objects](ApiCall &call) {
		objects.close(call[0]);
		call.ret(0);
	});
	// The scheduling class of the process, kept as the program sets it; the host does not change how the process is scheduled.
	auto priority_class = std::make_shared<uint32_t>(0x20);
	registry.add("GetPriorityClass", 1, [priority_class](ApiCall &call) { call.ret(*priority_class); });
	registry.add("SetPriorityClass", 2, [priority_class](ApiCall &call) {
		const uint32_t value = call.u32(1);
		if (value != 0x20 && value != 0x40 && value != 0x80 && value != 0x100 && value != 0x4000 && value != 0x8000 && value != 0x00100000 && value != 0x00200000) {
			call.fail(87);
			return;
		}
		*priority_class = value;
		call.ret(1);
	});
	// Prefetching memory is a hint: the pages are the host's to keep.
	registry.add("PrefetchVirtualMemory", 4, [](ApiCall &call) { call.ret(1); });
	// Power setting notifications are registered and never sent.
	registry.add("RegisterPowerSettingNotification", 3, [&objects](ApiCall &call) {
		const auto made = objects.add_object(std::make_shared<DeviceInfoSet>());
		if (made.status) {
			call.fail(8, 0);
			return;
		}
		call.ret(made.value);
	});
	registry.add("UnregisterPowerSettingNotification", 1, [&objects](ApiCall &call) {
		objects.close(call[0]);
		call.ret(1);
	});
	// Remote desktop session queries have no answer here.
	registry.add("WTSQuerySessionInformationW", 5, [](ApiCall &call) { call.fail(1, 0); });
	registry.add("WTSFreeMemory", 1, [](ApiCall &) {});
	// No input method editor is installed.
	registry.add("ImmGetIMEFileNameW", 3, [](ApiCall &call) { call.ret(0); });
	registry.add("ImmGetIMEFileNameA", 3, [](ApiCall &call) { call.ret(0); });
	// The policies of a desktop program, which has no package: each call succeeds and names the classic desktop's behaviour, as the
	// AppPolicy* enumerations number it.
	static constexpr std::pair<const char *, uint32_t> policies[] = {{"AppPolicyGetThreadInitializationType", 0}, {"AppPolicyGetClrCompat", 1},
		{"AppPolicyGetWindowingModel", 2}, {"AppPolicyGetProcessTerminationMethod", 0}, {"AppPolicyGetShowDeveloperDiagnostic", 1},
		{"AppPolicyGetCreateFileAccess", 0}, {"AppPolicyGetLifecycleManagement", 0}, {"AppPolicyGetMediaFoundationCodecLoading", 0}};
	for (const auto &[name, value] : policies)
		registry.add(name, 2, [value](ApiCall &call) {
			if (!call[1]) {
				call.ret(87);
				return;
			}
			call.store<uint32_t>(call[1], value);
			call.ret(0);
		});
	// Error reporting takes the modules a runtime names for its crash reports, the blocks and files to add to one and the programs to leave
	// out of them; no report is ever sent, so each registration succeeds with S_OK and keeps nothing.
	for (const auto name : {"WerRegisterRuntimeExceptionModule", "WerUnregisterRuntimeExceptionModule", "WerRegisterMemoryBlock",
			"WerUnregisterMemoryBlock", "WerRegisterFile", "WerUnregisterFile", "WerSetFlags", "WerAddExcludedApplication",
			"WerRemoveExcludedApplication", "WerRegisterCustomMetadata", "WerUnregisterCustomMetadata"})
		registry.add(name, 2, [](ApiCall &call) { call.ret(0); });
	// Binding writes the addresses of a program's imports into its file ahead of time, which only spares the loader work; the loader here
	// resolves every import when it loads the image, so an image left unbound is as good as a bound one (Windows Installer binds the
	// programs a package names in its BindImage table).
	registry.add("BindImage", 3, [](ApiCall &call) { call.ret(1); });
	registry.add("BindImageEx", 5, [](ApiCall &call) { call.ret(1); });
	// The guest's threads take turns on one host thread, so every write is seen by all of them before the call returns.
	registry.add("FlushProcessWriteBuffers", 0, [](ApiCall &) {});
	// UI Automation: no assistive client is listening on this desktop, so providers are never asked for, events go nowhere and every
	// notification succeeds; a window that answers WM_GETOBJECT with a provider gets nothing back.
	registry.add("UiaClientsAreListening", 0, [](ApiCall &call) { call.ret(0); });
	registry.add("UiaReturnRawElementProvider", 4, [](ApiCall &call) { call.ret(0); });
	registry.add("UiaLookupId", 2, [](ApiCall &call) { call.ret(0); });
	for (const auto name : {"UiaRaiseAutomationEvent", "UiaRaiseAutomationPropertyChangedEvent", "UiaRaiseStructureChangedEvent",
			"UiaRaiseNotificationEvent", "UiaRaiseAsyncContentLoadedEvent", "UiaRaiseTextEditTextChangedEvent", "UiaRaiseChangesEvent",
			"UiaRaiseActiveTextPositionChangedEvent", "UiaDisconnectProvider", "UiaDisconnectAllProviders"})
		registry.add(name, 0, [](ApiCall &call) { call.ret(0); });
	// The providers of window frames belong to a client side that is not there: the request is not implemented, as for a system without
	// UI Automation. The objects that stand for "not supported" and "mixed" values come from the shell component; without it they are
	// answered the same way.
	for (const auto name : {"UiaGetReservedNotSupportedValue", "UiaGetReservedMixedAttributeValue", "UiaHostProviderFromHwnd",
			"UiaProviderFromIAccessible"})
		registry.add(name, 0, [name](ApiCall &call) {
			const size_t out = std::string_view(name) == "UiaHostProviderFromHwnd" ? 1 : std::string_view(name) == "UiaProviderFromIAccessible" ? 3 : 0;
			const auto arguments = call.process.api_arguments(out + 1);
			if (arguments[out])
				call.store_pointer(arguments[out], 0);
			call.ret(0x80004001U);
		});
	// The accounts the machine knows: their names, the SIDs they stand for, how each is used and the domain that holds it. The user is the
	// one GetUserName names, in the machine's own domain.
	struct Account {
		std::u16string name, domain;
		std::vector<uint8_t> sid;
		uint32_t use;
	};
	const auto accounts = [&context]() {
		char host[256] = {};
		::gethostname(host, sizeof(host) - 1);
		std::string netbios(host);
		netbios = netbios.substr(0, netbios.find('.'));
		for (auto &character : netbios)
			character = char(std::toupper(static_cast<unsigned char>(character)));
		return std::vector<Account>{{u"Administrators", u"BUILTIN", make_sid(5, {32, 544}), 4}, {u"Users", u"BUILTIN", make_sid(5, {32, 545}), 4},
			{u"Everyone", u"", make_sid(1, {0}), 5}, {u"SYSTEM", u"NT AUTHORITY", make_sid(5, {18}), 5},
			{u"Authenticated Users", u"NT AUTHORITY", make_sid(5, {11}), 5}, {u"LOCAL SERVICE", u"NT AUTHORITY", make_sid(5, {19}), 5},
			{u"NETWORK SERVICE", u"NT AUTHORITY", make_sid(5, {20}), 5}, {u"INTERACTIVE", u"NT AUTHORITY", make_sid(5, {4}), 5},
			{u"NETWORK", u"NT AUTHORITY", make_sid(5, {2}), 5}, {u"CREATOR OWNER", u"", make_sid(3, {0}), 5},
			{u"TrustedInstaller", u"NT SERVICE", make_sid(5, {80, 956008885, 3418522649U, 1831038044, 1853292631, 2271478464U}), 5},
			{u"ALL APPLICATION PACKAGES", u"APPLICATION PACKAGE AUTHORITY", make_sid(15, {2, 1}), 5},
			{context.paths.user_name(), from_utf8(netbios), user_sid(), 1}};
	};
	// Both lookups fill a SID or name and the domain the same way: sizes first, then the text when the buffers hold it.
	const auto answer = [](ApiCall &call, const Account &account, bool by_name) {
		const size_t first = by_name ? account.sid.size() : account.name.size() + 1;
		if (!call[3] || !call[5]) {
			call.fail(87, 0);
			return;
		}
		const uint32_t first_capacity = call.load<uint32_t>(call[3]), domain_capacity = call.load<uint32_t>(call[5]);
		call.store<uint32_t>(call[3], uint32_t(first));
		call.store<uint32_t>(call[5], uint32_t(account.domain.size() + 1));
		if (!call[2] || first_capacity < first || !call[4] || domain_capacity < account.domain.size() + 1) {
			call.fail(122, 0);
			return;
		}
		if (by_name)
			call.write(call[2], account.sid);
		else {
			call.put_wide(call[2], account.name, account.name.size() + 1);
			call.store<uint32_t>(call[3], uint32_t(account.name.size()));
		}
		call.put_wide(call[4], account.domain, account.domain.size() + 1);
		call.store<uint32_t>(call[6], account.use);
		call.store<uint32_t>(call[5], uint32_t(account.domain.size()));
		call.ret(1);
	};
	registry.add("LookupAccountNameW", 7, [accounts, answer](ApiCall &call) {
		auto name = call[1] ? call.wide(call[1], 256) : std::u16string();
		const auto slash = name.find(u'\\');
		if (slash != name.npos)
			name = name.substr(slash + 1);
		const auto known = accounts();
		// No name at all is the user's own account.
		const auto found = std::find_if(known.begin(), known.end(), [&](const Account &account) {
			return name.empty() ? account.use == 1 : equal_fold(account.name, name);
		});
		if (found == known.end()) {
			call.fail(1332, 0);
			return;
		}
		answer(call, *found, true);
	});
	registry.add("LookupAccountSidW", 7, [accounts, answer](ApiCall &call) {
		if (!call[1]) {
			call.fail(87, 0);
			return;
		}
		const auto header = call.read(call[1], 2);
		const auto sid = call.read(call[1], 8 + size_t(header[1]) * 4);
		const auto known = accounts();
		const auto found = std::find_if(known.begin(), known.end(), [&](const Account &account) { return account.sid == sid; });
		if (found == known.end()) {
			call.fail(1332, 0);
			return;
		}
		answer(call, *found, false);
	});
	// The local security authority: a policy handle for the lookups, which translate account names to SIDs from the same accounts.
	registry.add("LsaOpenPolicy", 4, [](ApiCall &call) {
		if (!call[3]) {
			call.ret(0xc000000dU);
			return;
		}
		call.store_pointer(call[3], 0x15a0);
		call.ret(0);
	});
	registry.add("LsaClose", 1, [](ApiCall &call) { call.ret(0); });
	// Granting or removing account rights needs an administrator; the user programs run as is not one, so Windows denies it as here.
	registry.add("LsaAddAccountRights", 4, [](ApiCall &call) { call.ret(0xc0000022U); });
	registry.add("LsaRemoveAccountRights", 5, [](ApiCall &call) { call.ret(0xc0000022U); });
	registry.add("LsaFreeMemory", 1, [&context](ApiCall &call) {
		if (call[0])
			context.allocator.release(call[0]);
		call.ret(0);
	});
	registry.add("LsaNtStatusToWinError", 1, [](ApiCall &call) {
		const uint32_t status = call.u32(0);
		call.ret(status == 0 ? 0 : status == 0xc0000073U ? 1332 : status == 0x107 ? 1332 : status == 0xc0000022U ? 5 : status == 0xc000000dU ? 87 :
			status == 0xc0000017U ? 8 : 317);
	});
	// LsaLookupNames2(policy, flags, count, names, domains, sids): LSA_TRANSLATED_SID2 for each name, "account" or "domain\account", and
	// the LSA_REFERENCED_DOMAIN_LIST of the domains they belong to, each in memory LsaFreeMemory gives back. STATUS_SOME_NOT_MAPPED or
	// STATUS_NONE_MAPPED when names are not known.
	registry.add("LsaLookupNames2", 6, [&context, accounts](ApiCall &call) {
		const uint32_t count = call.u32(2);
		if (!call[3] || !call[4] || !call[5] || !count || count > 1000) {
			call.ret(0xc000000dU);
			return;
		}
		const size_t width = call.pointer_width(), text_size = width == 8 ? 16 : 8, translated_size = width == 8 ? 24 : 16;
		const size_t trust_size = text_size + width;
		const auto known = accounts();
		std::vector<const Account *> found(count, nullptr);
		std::vector<std::u16string> domains;
		std::vector<std::vector<uint8_t>> domain_sids;
		std::vector<int32_t> domain_of(count, -1);
		for (uint32_t index = 0; index < count; ++index) {
			const uint64_t entry = call[3] + index * uint64_t(text_size);
			const uint16_t length = call.load<uint16_t>(entry);
			const uint64_t buffer = width == 8 ? call.load<uint64_t>(entry + 8) : call.load<uint32_t>(entry + 4);
			std::u16string name;
			if (buffer && length) {
				const auto bytes = call.read(buffer, length & ~1U);
				for (size_t at = 0; at + 1 < bytes.size(); at += 2)
					name.push_back(char16_t(bytes[at] | bytes[at + 1] << 8));
			}
			const auto slash = name.find(u'\\');
			const auto account_name = slash == name.npos ? name : name.substr(slash + 1);
			const auto match = std::find_if(known.begin(), known.end(), [&](const Account &account) {
				return equal_fold(account.name, account_name) && (slash == name.npos || equal_fold(account.domain, name.substr(0, slash)));
			});
			if (match == known.end())
				continue;
			found[index] = &*match;
			if (match->domain.empty())
				continue;
			const auto known_domain = std::find_if(domains.begin(), domains.end(), [&](const auto &domain) { return equal_fold(domain, match->domain); });
			domain_of[index] = int32_t(known_domain - domains.begin());
			if (known_domain == domains.end()) {
				domains.push_back(match->domain);
				// The domain's SID is the account's without its last part.
				auto sid = match->sid;
				if (sid.size() > 8 && sid[1] > 0) {
					sid[1] = uint8_t(sid[1] - 1);
					sid.resize(sid.size() - 4);
				}
				domain_sids.push_back(std::move(sid));
			}
		}
		// The domain list: its header, the entries, then their names and SIDs.
		size_t domain_bytes = 16 + domains.size() * trust_size;
		for (size_t index = 0; index < domains.size(); ++index)
			domain_bytes += (domains[index].size() + 1) * 2 + domain_sids[index].size();
		size_t sid_bytes = count * translated_size;
		for (const auto *account : found)
			if (account)
				sid_bytes += account->sid.size();
		const uint64_t domain_block = context.allocator.allocate(domain_bytes, true), sid_block = context.allocator.allocate(sid_bytes, true);
		if (!domain_block || !sid_block) {
			if (domain_block) context.allocator.release(domain_block);
			if (sid_block) context.allocator.release(sid_block);
			call.ret(0xc0000017U);
			return;
		}
		const auto pointer_at = [&](uint64_t address, uint64_t value) {
			if (width == 8) call.store<uint64_t>(address, value);
			else call.store<uint32_t>(address, uint32_t(value));
		};
		call.store<uint32_t>(domain_block, uint32_t(domains.size()));
		pointer_at(domain_block + (width == 8 ? 8 : 4), domains.empty() ? 0 : domain_block + 16);
		uint64_t cursor = domain_block + 16 + domains.size() * trust_size;
		for (size_t index = 0; index < domains.size(); ++index) {
			const uint64_t entry = domain_block + 16 + index * trust_size;
			call.store<uint16_t>(entry, uint16_t(domains[index].size() * 2));
			call.store<uint16_t>(entry + 2, uint16_t((domains[index].size() + 1) * 2));
			pointer_at(entry + (width == 8 ? 8 : 4), cursor);
			call.put_wide(cursor, domains[index], domains[index].size() + 1);
			cursor += (domains[index].size() + 1) * 2;
			pointer_at(entry + text_size, cursor);
			call.write(cursor, domain_sids[index]);
			cursor += domain_sids[index].size();
		}
		cursor = sid_block + count * translated_size;
		uint32_t mapped = 0;
		for (uint32_t index = 0; index < count; ++index) {
			const uint64_t entry = sid_block + index * translated_size;
			const size_t after_use = width == 8 ? 8 : 4;
			if (!found[index]) {
				call.store<uint32_t>(entry, 8);
				call.store<int32_t>(entry + after_use + width, -1);
				continue;
			}
			++mapped;
			call.store<uint32_t>(entry, found[index]->use);
			pointer_at(entry + after_use, cursor);
			call.write(cursor, found[index]->sid);
			cursor += found[index]->sid.size();
			call.store<int32_t>(entry + after_use + width, domain_of[index]);
		}
		pointer_at(call[4], domain_block);
		pointer_at(call[5], sid_block);
		call.ret(mapped == count ? 0 : mapped ? 0x107 : 0xc0000073U);
	});
	// Session change messages are never sent: the registration succeeds and nothing arrives.
	registry.add("WTSRegisterSessionNotification", 2, [](ApiCall &call) { call.ret(1); });
	registry.add("WTSUnRegisterSessionNotification", 1, [](ApiCall &call) { call.ret(1); });
	registry.add("WTSRegisterSessionNotificationEx", 3, [](ApiCall &call) { call.ret(1); });
	registry.add("WTSUnRegisterSessionNotificationEx", 2, [](ApiCall &call) { call.ret(1); });
	// The credential store is empty: enumerating and reading find nothing.
	for (const auto name : {"CredEnumerateW", "CredEnumerateA"})
		registry.add(name, 4, [](ApiCall &call) {
			if (!call[2] || !call[3]) {
				call.fail(87);
				return;
			}
			call.store<uint32_t>(call[2], 0);
			call.store_pointer(call[3], 0);
			call.fail(1168);
		});
	for (const auto name : {"CredReadW", "CredReadA"})
		registry.add(name, 4, [](ApiCall &call) {
			if (!call[0] || !call[3]) {
				call.fail(87);
				return;
			}
			call.store_pointer(call[3], 0);
			call.fail(1168);
		});
	// No pointer devices other than the mouse are present.
	registry.add("RegisterPointerDeviceNotifications", 2, [](ApiCall &call) { call.ret(1); });
	registry.add("GetPointerDevices", 2, [](ApiCall &call) {
		if (!call[0]) {
			call.fail(error_invalid_parameter, 0);
			return;
		}
		call.store<uint32_t>(call[0], 0);
		call.ret(1);
	});
	// A URL is accepted as typed when it already names a scheme; otherwise the usual prefix is added.
	registry.add("UrlFixupW", 3, [](ApiCall &call) {
		auto url = call[0] ? call.wide(call[0], 2084) : std::u16string();
		if (url.find(u"://") == url.npos && !url.empty())
			url.insert(0, url.starts_with(u"ftp.") ? u"ftp://" : u"http://");
		if (!call[1] || url.size() >= call.u32(2)) {
			call.ret(0x80070057U);
			return;
		}
		call.put_wide(call[1], url, call.u32(2));
		call.ret(0);
	});
	// No name-space provider is installed, so a service lookup has nothing to search.
	registry.add("WSALookupServiceBeginW", 3, [](ApiCall &call) { call.fail(wsaservice_not_found, UINT64_MAX); });
	registry.add("WSALookupServiceBeginA", 3, [](ApiCall &call) { call.fail(wsaservice_not_found, UINT64_MAX); });
	registry.add("WSALookupServiceNextW", 4, [](ApiCall &call) { call.fail(wsa_invalid_handle, UINT64_MAX); });
	registry.add("WSALookupServiceNextA", 4, [](ApiCall &call) { call.fail(wsa_invalid_handle, UINT64_MAX); });
	registry.add("WSALookupServiceEnd", 1, [](ApiCall &call) { call.fail(wsa_invalid_handle, UINT64_MAX); });
}

}
