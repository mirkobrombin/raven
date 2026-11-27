// A program on an X server that gives a window of its own to a Windows program under Raven to show inside, as a host gives a plugin's
// editor its window: makes a blue window, runs the program with the window's id, and waits until a child of the window shows the
// program's red. Prints the child's size and the colour found in it.
//
// usage: x11_embed_test RAVEN PROGRAM

#include <xcb/xcb.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int count, char **arguments)
{
	if (count != 3) {
		std::fputs("usage: x11_embed_test RAVEN PROGRAM\n", stderr);
		return 2;
	}
	xcb_connection_t *const connection = xcb_connect(nullptr, nullptr);
	if (xcb_connection_has_error(connection)) {
		std::fputs("x11_embed_test: no X server\n", stderr);
		return 1;
	}
	xcb_screen_t *const screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
	const xcb_window_t host = xcb_generate_id(connection);
	const uint32_t values[] = {0x0000ffU};
	xcb_create_window(connection, screen->root_depth, host, screen->root, 0, 0, 400, 300, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
		XCB_CW_BACK_PIXEL, values);
	xcb_map_window(connection, host);
	xcb_flush(connection);
	char id[32];
	std::snprintf(id, sizeof id, "%x", host);
	const std::string line = std::string("--command-line=embed.exe ") + id;
	const pid_t child = ::fork();
	if (child == 0) {
		char run[] = "--run-process", native[] = "--execution=native", desktop[] = "--desktop=x11", timeout[] = "--timeout-ms=20000";
		char *const command[] = {arguments[1], run, native, desktop, timeout, const_cast<char *>(line.c_str()), arguments[2], nullptr};
		::execv(arguments[1], command);
		::_exit(127);
	}
	int status = 0;
	const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	bool found = false;
	while (!found && std::chrono::steady_clock::now() < limit && ::waitpid(child, &status, WNOHANG) == 0) {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		xcb_query_tree_reply_t *const tree = xcb_query_tree_reply(connection, xcb_query_tree(connection, host), nullptr);
		if (!tree)
			continue;
		const xcb_window_t *const children = xcb_query_tree_children(tree);
		for (int index = 0; index < xcb_query_tree_children_length(tree) && !found; ++index) {
			xcb_get_geometry_reply_t *const geometry = xcb_get_geometry_reply(connection, xcb_get_geometry(connection, children[index]), nullptr);
			xcb_get_image_reply_t *const image = xcb_get_image_reply(connection,
				xcb_get_image(connection, XCB_IMAGE_FORMAT_Z_PIXMAP, host, 100, 50, 1, 1, UINT32_MAX), nullptr);
			if (geometry && image && xcb_get_image_data_length(image) >= 4) {
				uint32_t pixel = 0;
				std::memcpy(&pixel, xcb_get_image_data(image), 4);
				pixel &= 0xffffffU;
				if (pixel == 0xff0000U) {
					std::printf("embedded: child %ux%u at %d,%d, pixel %06x\n", geometry->width, geometry->height, geometry->x, geometry->y, pixel);
					found = true;
				}
			}
			std::free(geometry);
			std::free(image);
		}
		std::free(tree);
	}
	if (::waitpid(child, &status, WNOHANG) == 0) {
		::kill(child, SIGTERM);
		::waitpid(child, &status, 0);
	}
	xcb_disconnect(connection);
	if (!found) {
		std::fputs("x11_embed_test: no child of the host window showed the program's red\n", stderr);
		return 1;
	}
	return 0;
}
