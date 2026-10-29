#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <span>
#include <string_view>
#include <chrono>
#include <vector>
#include <string>

namespace raven {

// pointer_wheel: message carries the signed wheel delta in 1/120 notches (wparam 1 for the horizontal wheel), x and y the pointer position.
// popup_done: the compositor dismissed a popup (for example by a click outside it).
// pointer_relative: x and y carry the movement of the mouse itself, unaccelerated and not stopped by the screen edges.
enum class DesktopEventKind { message, pointer_motion, pointer_button, keymap, modifiers, keyboard_focus, key, popup_done, resize, pointer_wheel,
	pointer_relative };

struct DesktopEvent {
	uint64_t window;
	uint32_t message;
	uint64_t wparam=0,lparam=0;
	DesktopEventKind kind=DesktopEventKind::message;
	int32_t x=0,y=0;
	std::string keymap{};
	std::vector<uint32_t> held_keys{};
};

// The window system the presentation talks to, which decides the surface type a graphics driver is given.
enum class HostPlatform { wayland, xcb };

// A host surface a graphics driver can present into, over a window's client area: on Wayland the display connection and a surface, on X11
// the XCB connection and a child window with its visual.
struct HostSurface {
	void *display = nullptr;
	void *surface = nullptr;
	HostPlatform platform = HostPlatform::wayland;
	uint32_t window = 0, visual = 0;
};

struct DesktopPresentation {
	std::function<void(uint64_t,std::string_view,uint32_t,uint32_t,std::span<const uint8_t>)> present;
	std::function<void(uint64_t)> remove;
	std::function<std::vector<DesktopEvent>(std::chrono::milliseconds)> poll;
	// Shows or updates a popup anchored at (x, y) of its parent surface, taking a pointer and keyboard grab.
	std::function<void(uint64_t handle,uint64_t parent,int32_t x,int32_t y,uint32_t width,uint32_t height,std::span<const uint8_t> pixels)> popup;
	// Hands a pointer drag that began on the frame to the compositor: 0 moves the window, 1..10 resize from that edge.
	std::function<void(uint64_t handle,uint32_t action)> interactive;
	// Offers UTF-8 text as the host selection.
	std::function<void(std::string_view text)> clipboard_publish;
	// Text of a selection owned by another client with a counter that changes with every new selection; empty when there is none.
	std::function<std::optional<std::pair<uint64_t,std::string>>()> clipboard_fetch;
	// Returns the surface a driver renders to for the program's window (window), placed over the rectangle (x, y, width, height) of the
	// frame of the host window of its root (handle) while shown, and kept where nothing shows it otherwise, or when that host window does
	// not exist yet: a program makes its swap chain before it shows the window. Every window has a surface of its own. Pointer and
	// keyboard input keep going to the host window. Asking again for the same window moves, resizes, shows or hides the surface it has.
	std::function<std::optional<HostSurface>(uint64_t handle,uint64_t window,int32_t x,int32_t y,uint32_t width,uint32_t height,bool shown)>
		host_surface;
	std::function<void(uint64_t window)> release_host_surface;
	// Keeps the pointer inside the rectangle (left, top, right, bottom) of the window's frame, or holds it still when the rectangle is a
	// point; no rectangle lets it go.
	std::function<void(uint64_t handle,std::optional<std::array<int32_t,4>> rectangle)> confine;
	// Shows or updates a program's own popup window (a menu or a list it draws itself) at (x, y) of the surface of the window that owns it.
	// Windows has no grab: input outside it keeps going where it goes, and the program closes its popup itself.
	std::function<void(uint64_t handle,uint64_t owner,int32_t x,int32_t y,uint32_t width,uint32_t height,std::span<const uint8_t> pixels)>
		owned_popup;
	// The descriptor that becomes readable when events arrive, for a caller that waits on it with others and then polls without
	// waiting; -1 while there is none.
	std::function<int()> descriptor;
	// The display connection also serves contexts created before a window is shown.
	std::function<void *()> host_display;
	// Logical size of the primary output; absent when the backend has no output information.
	std::function<std::optional<std::pair<int32_t,int32_t>>()> screen_size;
	// Requests or leaves maximization while retaining the surface a graphics context renders into.
	std::function<void(uint64_t,bool)> maximize;
	// The pointer over the program's windows: a system cursor by its IDC_ identifier (32512 the arrow), or none for 0.
	std::function<void(uint32_t shape)> cursor;
	HostPlatform platform = HostPlatform::wayland;
};

}
