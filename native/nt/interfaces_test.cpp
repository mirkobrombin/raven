#include "interfaces.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

static void check(bool condition, const char *message)
{
	if (!condition) {
		std::cerr << message << '\n';
		std::exit(1);
	}
}

// The field offsets of MIB_IFROW as Windows declares it, which is what a program reads the record with.
namespace offset {
constexpr size_t name = 0, index = 512, type = 516, mtu = 520, speed = 524, hardware_length = 528, hardware = 532;
constexpr size_t admin_status = 540, oper_status = 544, last_change = 548, description_length = 600, description = 604;
}

template<typename T> static T load(const std::vector<uint8_t> &row, size_t at)
{
	T value{};
	std::memcpy(&value, row.data() + at, sizeof value);
	return value;
}

// The wide name of a record, read character by character so nothing depends on the alignment of the record.
static std::u16string wide_name(const std::vector<uint8_t> &row)
{
	std::u16string text;
	for (size_t at = offset::name; at < offset::index; at += 2) {
		const uint16_t character = load<uint16_t>(row, at);
		if (!character)
			break;
		text.push_back(char16_t(character));
	}
	return text;
}

int main()
{
	raven::HostInterface ethernet;
	ethernet.name = "eth0";
	ethernet.index = 7;
	ethernet.mtu = 1500;
	ethernet.status = 1;
	ethernet.hardware[0] = 0x02;
	ethernet.hardware[5] = 0xff;
	ethernet.hardware_length = 6;

	const auto row = raven::mib_if_row(ethernet);
	check(row.size() == 860, "a MIB_IFROW record is not 860 bytes");
	check(load<uint32_t>(row, offset::index) == 7, "the index differs");
	check(load<uint32_t>(row, offset::type) == raven::if_type_ethernet, "an ethernet interface is not IFTYPE_ETHERNET");
	check(load<uint32_t>(row, offset::mtu) == 1500, "the MTU differs");
	check(load<uint32_t>(row, offset::speed) == 1000000000, "an ethernet interface does not report a gigabit link");
	check(load<uint32_t>(row, offset::hardware_length) == 6, "the hardware address length differs");
	check(row[offset::hardware] == 0x02 && row[offset::hardware + 5] == 0xff, "the hardware address differs");
	check(row[offset::hardware + 6] == 0 && row[offset::hardware + 7] == 0, "bytes past the hardware address were written");
	check(load<uint32_t>(row, offset::admin_status) == 1, "the interface is not administratively enabled");
	check(load<uint32_t>(row, offset::oper_status) == 1, "a running interface is not reported up");
	check(load<uint32_t>(row, offset::description_length) == 4, "the description length differs");
	check(std::memcmp(row.data() + offset::description, "eth0", 4) == 0, "the description is not the interface name");
	check(row[offset::description + 4] == 0, "a byte past the description was written");
	check(wide_name(row) == u"\\DEVICE\\TCPIP_{00000000-0000-0000-0000-000000000007}", "the name of the interface differs");
	check(row[offset::last_change] == 0, "a counter was written");

	raven::HostInterface loopback;
	loopback.name = "lo";
	loopback.index = 1;
	loopback.type = raven::if_type_loopback;
	loopback.status = 2;
	const auto loop = raven::mib_if_row(loopback);
	check(load<uint32_t>(loop, offset::type) == raven::if_type_loopback, "a loopback interface is not IFTYPE_LOOPBACK");
	check(load<uint32_t>(loop, offset::speed) == 10000000, "a loopback interface does not report a ten megabit link");
	check(load<uint32_t>(loop, offset::oper_status) == 2, "an interface that is not running is not reported down");
	check(load<uint32_t>(loop, offset::hardware_length) == 0, "a loopback interface has a hardware address");

	// A name longer than the record holds is cut at the last byte it has, not written past the end.
	raven::HostInterface long_name;
	long_name.name = std::string(300, 'a');
	const auto cut = raven::mib_if_row(long_name);
	check(load<uint32_t>(cut, offset::description_length) == 255, "a long description is not cut to 255 bytes");
	check(std::memcmp(cut.data() + offset::description, std::string(255, 'a').data(), 255) == 0, "the cut description differs");

	// Every interface the host reports is described under the index and the name it was found by, so GetIfEntry can name it again.
	for (const auto &interface : raven::host_interfaces()) {
		const auto described = raven::mib_if_row(interface);
		check(load<uint32_t>(described, offset::index) == interface.index, "the record of an interface carries another index");
		const size_t length = std::min<size_t>(interface.name.size(), 255);
		check(load<uint32_t>(described, offset::description_length) == length, "the record of an interface carries another name");
		check(std::memcmp(described.data() + offset::description, interface.name.data(), length) == 0, "the description of an interface differs");
	}
	return 0;
}
