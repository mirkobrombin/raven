// Lays out monitors on an X server through RandR 1.5, as a desktop with several screens has them, for the X11 tests: each argument is
// WIDTHxHEIGHT+X+Y, a trailing * marks the primary one. The server forgets monitors when the client that made them leaves, so the tool
// prints "ready" once they are in place and keeps its connection until its standard input closes.
#include <xcb/xcb.h>
#include <xcb/xcbext.h>
#include <sys/uio.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

xcb_extension_t randr{"RANDR", 0};

bool send(xcb_connection_t *connection, uint8_t opcode, void *request, size_t size, bool reply)
{
	iovec parts[3]{};
	parts[2].iov_base = request;
	parts[2].iov_len = size;
	xcb_protocol_request_t description{1, &randr, opcode, uint8_t(!reply)};
	const auto sequence = xcb_send_request(connection, reply ? 0 : XCB_REQUEST_CHECKED, parts + 2, &description);
	xcb_generic_error_t *error = nullptr;
	if (reply) {
		void *answer = xcb_wait_for_reply(connection, sequence, &error);
		std::free(answer);
		std::free(error);
		return answer;
	}
	error = xcb_request_check(connection, xcb_void_cookie_t{sequence});
	std::free(error);
	return !error;
}

}

int main(int argc, char **argv)
{
	xcb_connection_t *connection = xcb_connect(nullptr, nullptr);
	if (xcb_connection_has_error(connection)) {
		std::fprintf(stderr, "x11-monitors: no X server\n");
		return 1;
	}
	const xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
	const auto *extension = xcb_get_extension_data(connection, &randr);
	struct { uint8_t major, minor; uint16_t length; uint32_t want_major, want_minor; } version{0, 0, 3, 1, 5};
	if (!extension || !extension->present || !send(connection, 0, &version, sizeof version, true)) {
		std::fprintf(stderr, "x11-monitors: the server has no RandR 1.5\n");
		return 1;
	}
	for (int index = 1; index < argc; ++index) {
		unsigned width = 0, height = 0;
		int x = 0, y = 0;
		if (std::sscanf(argv[index], "%ux%u+%d+%d", &width, &height, &x, &y) != 4) {
			std::fprintf(stderr, "x11-monitors: %s is not WIDTHxHEIGHT+X+Y\n", argv[index]);
			return 2;
		}
		const std::string name = "RAVEN-" + std::to_string(index);
		auto *atom = xcb_intern_atom_reply(connection, xcb_intern_atom(connection, 0, uint16_t(name.size()), name.c_str()), nullptr);
		if (!atom)
			return 1;
		struct {
			uint8_t major, minor; uint16_t length; uint32_t window, name; uint8_t primary, automatic; uint16_t outputs;
			int16_t x, y; uint16_t width, height; uint32_t millimetres_wide, millimetres_high;
		} monitor{0, 43, 8, screen->root, atom->atom, uint8_t(std::strchr(argv[index], '*') != nullptr), 0, 0, int16_t(x), int16_t(y),
			uint16_t(width), uint16_t(height), width * 254 / 960, height * 254 / 960};
		std::free(atom);
		if (!send(connection, 43, &monitor, sizeof monitor, false)) {
			std::fprintf(stderr, "x11-monitors: the server refused %s\n", argv[index]);
			return 1;
		}
	}
	std::printf("ready\n");
	std::fflush(stdout);
	while (std::fgetc(stdin) != EOF) {
	}
	xcb_disconnect(connection);
	return 0;
}
