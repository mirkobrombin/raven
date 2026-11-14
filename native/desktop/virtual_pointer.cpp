// A pointer for the tests that drive a program on the isolated compositor: it presses the left button at the first point given, drags
// through the others and releases at the last, which the tools those tests have cannot do, since wlrctl moves and clicks but holds no
// button. It speaks the compositor's virtual pointer protocol, wlr-virtual-pointer-unstable-v1, whose requests are named here in the
// order the protocol numbers them.
//
// usage: raven-virtual-pointer WIDTH HEIGHT X Y [X Y]...
//   WIDTH and HEIGHT are the size of the output the points are measured on.

#include <wayland-client.h>
#include <linux/input-event-codes.h>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <utility>
#include <vector>

namespace {

const wl_interface *no_types[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
const wl_message pointer_requests[] = {
	{"motion", "uff", no_types}, {"motion_absolute", "uuuuu", no_types}, {"button", "uuu", no_types}, {"axis", "uuf", no_types},
	{"frame", "", no_types}, {"axis_source", "u", no_types}, {"axis_stop", "uu", no_types}, {"axis_discrete", "uufi", no_types},
	{"destroy", "", no_types}};
const wl_interface pointer_interface = {"zwlr_virtual_pointer_v1", 1, 9, pointer_requests, 0, nullptr};
const wl_interface *create_types[2] = {&wl_seat_interface, &pointer_interface};
const wl_message manager_requests[] = {{"create_virtual_pointer", "?on", create_types}, {"destroy", "", no_types}};
const wl_interface manager_interface = {"zwlr_virtual_pointer_manager_v1", 1, 2, manager_requests, 0, nullptr};
enum : uint32_t { request_motion_absolute = 1, request_button = 2, request_frame = 4 };

struct Globals {
	wl_seat *seat = nullptr;
	wl_proxy *manager = nullptr;
};

void global(void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t)
{
	auto &globals = *static_cast<Globals *>(data);
	if (!std::strcmp(interface, wl_seat_interface.name) && !globals.seat)
		globals.seat = static_cast<wl_seat *>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
	else if (!std::strcmp(interface, manager_interface.name))
		globals.manager = static_cast<wl_proxy *>(wl_registry_bind(registry, name, &manager_interface, 1));
}

void global_gone(void *, wl_registry *, uint32_t) {}

uint32_t now()
{
	timespec time{};
	clock_gettime(CLOCK_MONOTONIC, &time);
	return uint32_t(time.tv_sec * 1000 + time.tv_nsec / 1000000);
}

void rest(long milliseconds)
{
	const timespec pause{milliseconds / 1000, milliseconds % 1000 * 1000000};
	nanosleep(&pause, nullptr);
}

}

int main(int argc, char **argv)
{
	if (argc < 5 || argc % 2 == 0) {
		std::cerr << "usage: raven-virtual-pointer WIDTH HEIGHT X Y [X Y]...\n";
		return 2;
	}
	const auto width = uint32_t(std::atoi(argv[1])), height = uint32_t(std::atoi(argv[2]));
	std::vector<std::pair<int, int>> points;
	for (int index = 3; index + 1 < argc; index += 2)
		points.emplace_back(std::atoi(argv[index]), std::atoi(argv[index + 1]));
	wl_display *display = wl_display_connect(nullptr);
	if (!display) {
		std::cerr << "no compositor to connect to\n";
		return 1;
	}
	Globals globals;
	static const wl_registry_listener listener{global, global_gone};
	wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &listener, &globals);
	wl_display_roundtrip(display);
	if (!globals.manager) {
		std::cerr << "the compositor has no virtual pointers\n";
		return 1;
	}
	wl_proxy *pointer = wl_proxy_marshal_flags(globals.manager, 0, &pointer_interface, 1, 0, globals.seat, nullptr);
	wl_display_roundtrip(display);
	const auto move = [&](int x, int y) {
		wl_proxy_marshal_flags(pointer, request_motion_absolute, nullptr, 1, 0, now(), uint32_t(std::max(x, 0)), uint32_t(std::max(y, 0)), width,
			height);
		wl_proxy_marshal_flags(pointer, request_frame, nullptr, 1, 0);
		wl_display_flush(display);
	};
	const auto button = [&](uint32_t state) {
		wl_proxy_marshal_flags(pointer, request_button, nullptr, 1, 0, now(), uint32_t(BTN_LEFT), state);
		wl_proxy_marshal_flags(pointer, request_frame, nullptr, 1, 0);
		wl_display_flush(display);
	};
	move(points.front().first, points.front().second);
	rest(150);
	button(1);
	rest(80);
	// A hand moves through every place on its way: a few pixels at a time, a step every few milliseconds.
	for (size_t index = 1; index < points.size(); ++index) {
		const auto [from_x, from_y] = points[index - 1];
		const auto [to_x, to_y] = points[index];
		const int steps = std::max(1, std::max(std::abs(to_x - from_x), std::abs(to_y - from_y)) / 3);
		for (int step = 1; step <= steps; ++step) {
			move(from_x + (to_x - from_x) * step / steps, from_y + (to_y - from_y) * step / steps);
			rest(12);
		}
	}
	rest(80);
	button(0);
	wl_display_roundtrip(display);
	rest(100);
	wl_display_disconnect(display);
	return 0;
}
