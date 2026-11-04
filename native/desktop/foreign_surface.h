#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace raven {

class SharedPixels;

// Windows of every process of a prefix are visible to the others, as one window system serves them all on Windows: a process that
// creates a window publishes its handle and client size, and another process that draws into it (Chromium's GPU process drawing the
// browser's windows, for one) gets a device context over pixels it shares with the owner, which shows them in the window.

// The window handles of this process carry a tag of their own, so no two processes of the host hand out the same one.
uint64_t window_handle_base();
// Owner side: the window exists with this client size, or no longer exists.
void publish_window(uint64_t window, uint32_t width, uint32_t height);
void withdraw_window(uint64_t window);
// Drawing side: the client size of a window another live process of the prefix owns.
std::optional<std::pair<uint32_t, uint32_t>> foreign_window_size(uint64_t window);
// The process that owns a window another live process of the prefix created.
std::optional<uint32_t> foreign_window_owner(uint64_t window);

// What another process may ask of a window it does not own: its styles, its parent, whether it is shown, the name of its class and
// its title (both UTF-8).
struct ForeignWindowTraits {
	uint32_t style = 0, ex_style = 0;
	std::string class_name;
	uint64_t parent = 0;
	bool visible = false;
	std::string title;
};
void publish_window_traits(uint64_t window, const ForeignWindowTraits &traits);
std::optional<ForeignWindowTraits> foreign_window_traits(uint64_t window);
// The windows other live processes of the prefix own, in no particular order.
std::vector<uint64_t> foreign_windows();

// A message posted to a window of another process waits in the windows directory until the owner takes it into its queue.
bool post_foreign_message(uint64_t window, uint32_t message, uint64_t wparam, uint64_t lparam);
// Owner side: hands every message posted to a window `owned` accepts to `use`, oldest first from each poster, and forgets it.
void take_foreign_messages(const std::function<bool(uint64_t)> &owned, const std::function<void(uint64_t, uint32_t, uint64_t, uint64_t)> &use);
// A message sent to a window of another process, with the bytes it carries (those of WM_COPYDATA), waits for the owner's answer for at
// most `timeout`; nothing when the time ran out or the owner ended first.
std::optional<uint64_t> send_foreign_message(uint64_t window, uint32_t message, uint64_t wparam, uint64_t lparam, const std::vector<uint8_t> &data,
	std::chrono::milliseconds timeout);
// Owner side: answers every message sent to a window `owned` accepts with what `use` returns.
void take_foreign_sends(const std::function<bool(uint64_t)> &owned,
	const std::function<uint64_t(uint64_t, uint32_t, uint64_t, uint64_t, const std::vector<uint8_t> &)> &use);

// Pixels of a window shared between the process that draws them and the owner: a header page, then 32-bit rows top down.
class ForeignSurface {
public:
	// Opens (creating or growing when needed) the shared pixels of a window for drawing.
	static std::shared_ptr<ForeignSurface> open_for_drawing(uint64_t window, uint32_t width, uint32_t height);
	~ForeignSurface();
	std::shared_ptr<SharedPixels> pixels;
	uint32_t width = 0, height = 0;
	// Marks a finished frame for the owner.
	void publish_frame();

private:
	int file = -1;
	uint32_t *header = nullptr;
};

// Owner side: when another process finished a new frame for the window, hands its pixels (32-bit rows, top down) to `use` and returns
// true; `seen` remembers the last frame taken.
bool take_foreign_frame(uint64_t window, uint32_t &seen, const std::function<void(const uint8_t *, uint32_t, uint32_t)> &use);
// Changes whenever another process starts or stops drawing into a window of the prefix (its pixels file is made or removed).
uint64_t foreign_frames_stamp();
// A descriptor that becomes readable when that may have happened, for a waiter to sleep on; drain it once woken. -1 without one.
int foreign_frames_descriptor();
void drain_foreign_frames_descriptor();

}
