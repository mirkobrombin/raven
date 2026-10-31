#include "windows.h"
#include "nt/nls.h"
#include "nt/text.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <sys/mman.h>

namespace raven {

namespace {

uint64_t read_le(std::span<const uint8_t> bytes, size_t at, size_t width)
{
	uint64_t value = 0;
	for (size_t byte = 0; byte < width; ++byte)
		value |= uint64_t(bytes[at + byte]) << (byte * 8);
	return value;
}

void write_le(std::span<uint8_t> bytes, size_t at, uint64_t value, size_t width)
{
	for (size_t byte = 0; byte < width; ++byte)
		bytes[at + byte] = uint8_t(value >> (byte * 8));
}

// One printf-style conversion of Win32 wsprintf: flags, width, precision, size prefix and type.
struct Conversion {
	bool left = false, zero = false, plus = false, space = false, alternate = false;
	int64_t width = 0, precision = -1;
	bool wide_size = false, narrow_size = false;
	char type = 0;
	bool width_argument = false, precision_argument = false;
};

std::optional<Conversion> parse_conversion(std::u16string_view format, size_t &at)
{
	Conversion item;
	for (; at < format.size(); ++at) {
		const auto c = format[at];
		if (c == u'-') item.left = true;
		else if (c == u'0') item.zero = true;
		else if (c == u'+') item.plus = true;
		else if (c == u' ') item.space = true;
		else if (c == u'#') item.alternate = true;
		else break;
	}
	if (at < format.size() && format[at] == u'*') {
		item.width_argument = true;
		++at;
	} else {
		for (; at < format.size() && format[at] >= u'0' && format[at] <= u'9'; ++at)
			item.width = std::min<int64_t>(item.width * 10 + (format[at] - u'0'), 1024);
	}
	if (at < format.size() && format[at] == u'.') {
		++at;
		item.precision = 0;
		if (at < format.size() && format[at] == u'*') {
			item.precision_argument = true;
			++at;
		} else {
			for (; at < format.size() && format[at] >= u'0' && format[at] <= u'9'; ++at)
				item.precision = std::min<int64_t>(item.precision * 10 + (format[at] - u'0'), 1024);
		}
	}
	for (; at < format.size(); ++at) {
		if (format[at] == u'l' || format[at] == u'w') item.wide_size = true;
		else if (format[at] == u'h') item.narrow_size = true;
		else break;
	}
	if (at >= format.size())
		return std::nullopt;
	const auto type = format[at++];
	if (type > 127 || std::string_view("cCdiuxXsS").find(char(type)) == std::string_view::npos)
		return std::nullopt;
	item.type = char(type);
	return item;
}

}

uint64_t NativeWindows::desktop_window()
{
	if (desktop && objects.window(desktop))
		return desktop;
	// The class needs a non-zero procedure to register; the window itself carries none and is answered by the system.
	DesktopClass definition{"#32769", 0, 1};
	const auto id = objects.register_class(std::move(definition));
	if (!id)
		throw uint32_t(8);
	const auto &screen = objects.nonclient_metrics();
	DesktopWindow record{id, 0, {}, 0, 0, screen.screen_x, screen.screen_y};
	record.style = 0x96000000;
	record.visible = true;
	desktop = objects.add(std::move(record));
	if (!desktop)
		throw uint32_t(8);
	return desktop;
}

// Re-runs WM_NCCALCSIZE for the current outer rectangle and adopts the client rectangle it answers.
void NativeWindows::recalculate_client(uint64_t handle,int32_t previous_width,int32_t previous_height,std::optional<std::pair<int32_t,int32_t>> origin)
{
	auto *window = objects.window(handle);
	if (!window)
		throw uint32_t(1400);
	const auto laid_out = origin ? *origin : std::pair<int32_t,int32_t>{window->x, window->y};
	const auto scratch = process.map_memory(4096, PROT_READ | PROT_WRITE);
	DesktopRect client{};
	bool resized = false;
	int32_t old_width = previous_width >= 0 ? previous_width : window->width, old_height = previous_height >= 0 ? previous_height : window->height;
	try {
		// The extended form Windows sends after creation: NCCALCSIZE_PARAMS holds the new window rectangle, the old one and the old client
		// rectangle, then a pointer to the WINDOWPOS that follows it. A program that draws its own frame answers only this form.
		const bool narrow = process.mode() == NativeMode::x86;
		const size_t pointer = narrow ? 4 : 8, position = 48 + pointer;
		std::vector<uint8_t> bytes(position + (narrow ? 28 : 40));
		const int32_t values[12] = {window->outer.left, window->outer.top, window->outer.right, window->outer.bottom,
			window->outer.left, window->outer.top, window->outer.right, window->outer.bottom,
			window->x, window->y, window->x + window->width, window->y + window->height};
		for (size_t index = 0; index < 12; ++index)
			write_le(std::span<uint8_t>(bytes), index * 4, uint32_t(values[index]), 4);
		write_le(std::span<uint8_t>(bytes), 48, scratch + position, pointer);
		write_le(std::span<uint8_t>(bytes), position, handle, pointer);
		const int32_t place[4] = {window->outer.left, window->outer.top, window->outer.right - window->outer.left, window->outer.bottom - window->outer.top};
		for (size_t index = 0; index < 4; ++index)
			write_le(std::span<uint8_t>(bytes), position + pointer * 2 + index * 4, uint32_t(place[index]), 4);
		write_le(std::span<uint8_t>(bytes), position + pointer * 2 + 16, 0x20 | 0x4 | 0x10, 4);
		process.write_memory(scratch, bytes);
		send(handle, 0x83, 1, scratch);
		window = objects.window(handle);
		if (!window)
			throw std::runtime_error("window was destroyed during frame calculation");
		const auto answer = process.read_memory(scratch, 16);
		client = {int32_t(read_le(answer, 0, 4)), int32_t(read_le(answer, 4, 4)), int32_t(read_le(answer, 8, 4)), int32_t(read_le(answer, 12, 4))};
		if (std::getenv("RAVEN_TRACE_NC")) fprintf(stderr, "nc %llx outer %d,%d,%d,%d -> %d,%d,%d,%d\n", (unsigned long long)handle, values[0], values[1], values[2], values[3], client.left, client.top, client.right, client.bottom);
	} catch (...) {
		process.unmap_memory(scratch);
		throw;
	}
	process.unmap_memory(scratch);
	// A window too small for its frame has an empty client area, never a negative one.
	client.right = std::max(client.right, client.left);
	client.bottom = std::max(client.bottom, client.top);
	window->x = client.left;
	window->y = client.top;
	window->width = client.right - client.left;
	window->height = client.bottom - client.top;
	if (client.left != laid_out.first || client.top != laid_out.second)
		for (const auto child : objects.child_windows(handle))
			shift_window(child, client.left - laid_out.first, client.top - laid_out.second);
	resized = window->width != old_width || window->height != old_height;
	objects.update_window_dcs(handle);
	if (resized)
		publish_window(handle, uint32_t(std::max(window->width, 0)), uint32_t(std::max(window->height, 0)));
	if (resized)
		send(handle, 5, 0, uint16_t(window->width) | (uint64_t(uint16_t(window->height)) << 16));
	if (objects.window(handle) && objects.window_visible(handle)) {
		window = objects.window(handle);
		objects.invalidate(handle, {0, 0, window->width, window->height}, true);
		present(handle);
	}
}

void NativeWindows::reshape(uint64_t handle, DesktopRect outer)
{
	auto *window = objects.window(handle);
	const bool moved = window->outer.left != outer.left || window->outer.top != outer.top;
	const bool resized = window->outer.right - window->outer.left != outer.right - outer.left ||
		window->outer.bottom - window->outer.top != outer.bottom - outer.top;
	const int32_t client_width = window->width, client_height = window->height;
	// Where the children were laid out: the client origin before the change, which they follow wherever the client area goes.
	const std::pair<int32_t, int32_t> laid_out{window->x, window->y};
	if (resized && window->bitmap) {
		// The surface is outer-sized, so a new size needs a new surface. A window that carries a graphics driver's surface keeps its host
		// window, because that surface stays attached to it; any other gets a new host window, which the compositor places afresh.
		const bool visible = window->visible;
		const bool kept = keep_host_window;
		keep_host_window = keep_host_window || hosts_surface(handle);
		resizing = true;
		try { release_surface(handle); } catch (...) { keep_host_window = kept; resizing = false; throw; }
		keep_host_window = kept;
		resizing = false;
		window = objects.window(handle);
		window->bitmap = 0;
		window->dc = 0;
		window->outer = outer;
		window->x = outer.left;
		window->y = outer.top;
		window->width = outer.right - outer.left;
		window->height = outer.bottom - outer.top;
		recalculate_client(handle, client_width, client_height, laid_out);
		(void)visible;
	} else {
		window->outer = outer;
		window->x = outer.left;
		window->y = outer.top;
		window->width = outer.right - outer.left;
		window->height = outer.bottom - outer.top;
		recalculate_client(handle, client_width, client_height, laid_out);
	}
	window = objects.window(handle);
	if (window && moved)
		send(handle, 3, 0, uint16_t(window->x) | (uint64_t(uint16_t(window->y)) << 16));
}

// Queues WM_TIMER for every expired timer; messages already waiting for the same timer are not duplicated.
void NativeWindows::post_due_timers()
{
	const auto now = std::chrono::steady_clock::now();
	for (auto &timer : timers) {
		if (timer.due > now)
			continue;
		const uint32_t kind = timer.system ? 0x118 : 0x113;
		const bool pending = std::any_of(messages.begin(), messages.end(), [&](const Message &message) {
			return message.message == kind && message.window == timer.window && message.wparam == timer.id;
		});
		if (!pending && messages.size() < 10000)
			queue({timer.window, timer.id, timer.procedure, kind, uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()), cursor_x, cursor_y, 0, timer.window ? 0 : timer.thread},0x10);
		timer.due = now + std::chrono::milliseconds(timer.interval);
	}
}

bool NativeWindows::system_dispatch(std::string_view name)
{
	const bool ansi = name == "wsprintfA" || name == "wnsprintfA" || name == "wvsprintfA" || name == "wvnsprintfA";
	// wnsprintf takes the buffer's size in characters before the format and never writes past it; the wv forms take their arguments
	// from a va_list, a pointer to them in memory.
	const bool bounded = name == "wnsprintfA" || name == "wnsprintfW" || name == "wvnsprintfA" || name == "wvnsprintfW";
	const bool listed = name.starts_with("wv");
	const bool format = name == "wsprintfW" || name == "wsprintfA" || name == "wvsprintfW" || name == "wvsprintfA" || bounded;
	if (name != "GetDesktopWindow" && name != "PtInRect" && name != "SetRect" && name != "TranslateAcceleratorW" && name != "SetMenu" &&
		name != "GetMenuItemRect" && name != "CheckMenuItem" && name != "SetCapture" &&
		name != "ReleaseCapture" && name != "SetTimer" && name != "SetCoalescableTimer" && name != "KillTimer" && !format)
		return false;
	uint64_t result = 0;
	try {
		if (name == "GetDesktopWindow") {
			process.api_arguments(0);
			result = desktop_window();
		} else if (name == "PtInRect") {
			const auto args = process.api_arguments(3);
			const auto bytes = process.read_memory(args[0], 16);
			const auto left = int32_t(read_le(bytes, 0, 4)), top = int32_t(read_le(bytes, 4, 4));
			const auto right = int32_t(read_le(bytes, 8, 4)), bottom = int32_t(read_le(bytes, 12, 4));
			// A 32-bit caller pushes the two coordinates; a 64-bit caller passes the POINT packed in one register.
			const bool packed = process.mode() != NativeMode::x86;
			const auto x = int32_t(uint32_t(args[1])), y = packed ? int32_t(uint32_t(args[1] >> 32)) : int32_t(uint32_t(args[2]));
			result = x >= left && x < right && y >= top && y < bottom;
		} else if (name == "TranslateAcceleratorW") {
			const auto args = process.api_arguments(3);
			const auto *table = objects.accelerators(args[1]);
			if (!table || !objects.window(args[0]))
				throw uint32_t(1400);
			const bool narrow = process.mode() == NativeMode::x86;
			const auto bytes = process.read_memory(args[2], narrow ? 28 : 48);
			const auto message = uint32_t(read_le(bytes, narrow ? 4 : 8, 4));
			const auto wparam = read_le(bytes, narrow ? 8 : 16, narrow ? 4 : 8);
			const auto state = modifier_state();
			for (const auto &entry : *table) {
				const bool virtual_key = entry.flags & 1;
				bool match = false;
				if (virtual_key && (message == 0x100 || message == 0x104))
					match = wparam == entry.key && bool(entry.flags & 4) == state.shift && bool(entry.flags & 8) == state.control &&
						bool(entry.flags & 0x10) == state.alt;
				else if (!virtual_key && message == 0x102 && !state.alt)
					match = wparam == entry.key;
				if (match) {
					send(args[0], 0x111, uint64_t(entry.command) | (uint64_t(1) << 16), 0);
					result = 1;
					break;
				}
			}
		} else if (name == "SetMenu") {
			const auto args = process.api_arguments(2);
			auto *window = objects.window(args[0]);
			if (!window)
				throw uint32_t(1400);
			if (window->parent || (args[1] && !objects.menu(args[1])))
				throw uint32_t(args[1] && !objects.menu(args[1]) ? 1401 : 120);
			window->menu = args[1];
			recalculate_client(args[0]);
			result = 1;
		} else if (name == "GetMenuItemRect") {
			const auto args = process.api_arguments(4);
			const auto *window = objects.window(args[0]);
			if (!window)
				throw uint32_t(1400);
			// Only the menu bar of its own window has a layout; popup menus are not laid out yet.
			if (args[1] != window->menu || !window->menu)
				throw uint32_t(120);
			const auto rects = menu_bar_items(*window);
			if (uint32_t(args[2]) >= rects.size())
				throw uint32_t(87);
			const auto &rect = rects[uint32_t(args[2])];
			std::array<uint8_t, 16> bytes{};
			const int32_t values[4] = {window->outer.left + rect.left, window->outer.top + rect.top, window->outer.left + rect.right,
				window->outer.top + rect.bottom};
			for (size_t index = 0; index < 4; ++index)
				write_le(bytes, index * 4, uint32_t(values[index]), 4);
			process.write_memory(args[3], bytes);
			result = 1;
		} else if (name == "CheckMenuItem") {
			const auto args = process.api_arguments(3);
			auto *menu = objects.edit_menu(args[0]);
			if (!menu)
				throw uint32_t(1401);
			const bool by_position = uint32_t(args[2]) & 0x400;
			DesktopMenuItem *found = nullptr;
			const auto search = [&](auto &&self, DesktopMenu &level) -> void {
				if (by_position) {
					if (!found && uint32_t(args[1]) < level.items.size())
						found = &level.items[uint32_t(args[1])];
					return;
				}
				for (auto &item : level.items) {
					if (!item.submenu && item.id == uint32_t(args[1])) {
						found = &item;
						return;
					}
					if (item.submenu)
						if (auto *child = objects.edit_menu(item.submenu)) {
							self(self, *child);
							if (found)
								return;
						}
				}
			};
			search(search, *menu);
			if (!found) {
				result = UINT32_MAX;
			} else {
				result = (found->state & 8) ? 8 : 0;
				found->state = (found->state & ~8U) | ((uint32_t(args[2]) & 8) ? 8U : 0U);
			}
		} else if (name == "SetCapture") {
			const auto args = process.api_arguments(1);
			if (!objects.window(args[0]))
				throw uint32_t(1400);
			result = capture;
			capture = args[0];
			if (result && result != args[0] && objects.window(result))
				send(result, 0x215, 0, args[0]);
			else if (result && !objects.window(result))
				result = 0;
		} else if (name == "ReleaseCapture") {
			process.api_arguments(0);
			if (capture) {
				const auto previous = capture;
				capture = 0;
				if (objects.window(previous))
					send(previous, 0x215, 0, 0);
			}
			result = 1;
		} else if (name == "SetTimer" || name == "SetCoalescableTimer") {
			// The coalescing tolerance is only a hint, so both entry points share one timer.
			const auto args = process.api_arguments(name == "SetTimer" ? 4 : 5);
			const auto interval = std::clamp<uint32_t>(uint32_t(args[2]), 10, 0x7fffffff);
			if (args[0] && !objects.window(args[0]))
				throw uint32_t(1400);
			uint64_t id = args[1];
			if (!args[0]) {
				// A thread timer without a window gets a fresh system identifier.
				id = next_timer_id++;
			} else if (!id) {
				throw uint32_t(87);
			}
			if (timers.size() >= 1024)
				throw uint32_t(8);
			std::erase_if(timers, [&](const Timer &timer) { return timer.window == args[0] && timer.id == id; });
			timers.push_back({args[0], id, args[3], interval, std::chrono::steady_clock::now() + std::chrono::milliseconds(interval), false, environment.thread_id()});
			result = id;
		} else if (name == "KillTimer") {
			const auto args = process.api_arguments(2);
			const auto before = timers.size();
			std::erase_if(timers, [&](const Timer &timer) { return timer.window == args[0] && timer.id == args[1]; });
			if (before == timers.size())
				throw uint32_t(1814);
			result = 1;
		} else if (name == "SetRect") {
			const auto args = process.api_arguments(5);
			std::array<uint8_t, 16> bytes{};
			for (size_t index = 0; index < 4; ++index)
				write_le(bytes, index * 4, uint32_t(args[1 + index]), 4);
			process.write_memory(args[0], bytes);
			result = 1;
		} else {
			// cdecl variadic: count the consumed words from the format string before reading them off the stack.
			const size_t first = bounded ? 2 : 1;
			const auto head = process.api_arguments(first + 1);
			std::u16string pattern;
			if (ansi) {
				std::string narrow;
				for (size_t index = 0; index < 4096; ++index) {
					const auto byte = process.read_memory(head[first] + index, 1)[0];
					if (!byte)
						break;
					narrow.push_back(char(byte));
				}
				pattern = from_ansi(narrow, 1252);
			} else {
				pattern = window_text(head[first]);
			}
			size_t words = 0;
			for (size_t at = 0; at < pattern.size();) {
				if (pattern[at++] != u'%')
					continue;
				if (at < pattern.size() && pattern[at] == u'%') {
					++at;
					continue;
				}
				const auto item = parse_conversion(pattern, at);
				if (!item)
					break;
				words += 1 + item->width_argument + item->precision_argument;
			}
			if (words > 64)
				throw uint32_t(87);
			const auto args = process.api_arguments(first + 1 + (listed ? 1 : words));
			size_t next = first + 1;
			const size_t slot = process.mode() == NativeMode::x86 ? 4 : 8;
			const auto take = [&]() -> uint64_t {
				if (!listed)
					return args[next++];
				const auto bytes = process.read_memory(args[first + 1] + (next++ - first - 1) * slot, slot);
				return read_le(bytes, 0, slot);
			};
			std::u16string output;
			for (size_t at = 0; at < pattern.size();) {
				const auto c = pattern[at++];
				if (c != u'%') {
					output.push_back(c);
					continue;
				}
				if (at < pattern.size() && pattern[at] == u'%') {
					output.push_back(u'%');
					++at;
					continue;
				}
				auto item = parse_conversion(pattern, at);
				if (!item)
					break;
				if (item->width_argument) {
					const auto value = int32_t(uint32_t(take()));
					item->left |= value < 0;
					item->width = std::min<int64_t>(std::abs(int64_t(value)), 1024);
				}
				if (item->precision_argument) {
					const auto value = int32_t(uint32_t(take()));
					item->precision = value < 0 ? -1 : std::min<int64_t>(value, 1024);
				}
				const auto value = take();
				std::u16string body;
				if (item->type == 'c' || item->type == 'C') {
					if (ansi && item->type == 'c')
						body = from_ansi(std::string(1, char(uint8_t(value))), 1252);
					else
						body.push_back(char16_t(uint16_t(value)));
				} else if (item->type == 's' || item->type == 'S') {
					// %s takes the string width of the function (UTF-16); %S, %hs and %ls swap it.
					bool wide = (item->type == 's') != ansi;
					if (item->narrow_size) wide = false;
					if (item->wide_size) wide = true;
					if (!value) {
						body = u"(null)";
					} else {
						for (size_t index = 0; index < 4096; ++index) {
							const auto bytes = process.read_memory(value + index * (wide ? 2 : 1), wide ? 2 : 1);
							const char16_t character = wide ? char16_t(bytes[0] | bytes[1] << 8) : char16_t(bytes[0]);
							if (!character || (item->precision >= 0 && int64_t(index) >= item->precision))
								break;
							body.push_back(character);
						}
					}
				} else {
					const bool signed_type = item->type == 'd' || item->type == 'i';
					uint32_t raw = uint32_t(value);
					if (item->narrow_size)
						raw = signed_type ? uint32_t(int32_t(int16_t(raw))) : uint32_t(uint16_t(raw));
					const bool negative = signed_type && int32_t(raw) < 0;
					uint64_t magnitude = signed_type ? (negative ? uint64_t(-int64_t(int32_t(raw))) : raw) : raw;
					std::string digits;
					const bool hexadecimal = item->type == 'x' || item->type == 'X';
					do {
						const auto digit = unsigned(magnitude % (hexadecimal ? 16 : 10));
						digits.push_back(char((digit < 10 ? '0' : (item->type == 'X' ? 'A' : 'a') - 10) + digit));
						magnitude /= hexadecimal ? 16 : 10;
					} while (magnitude);
					while (item->precision >= 0 && int64_t(digits.size()) < item->precision)
						digits.push_back('0');
					std::string prefix;
					if (negative) prefix = "-";
					else if (signed_type && item->plus) prefix = "+";
					else if (signed_type && item->space) prefix = " ";
					if (hexadecimal && item->alternate && raw) prefix = item->type == 'X' ? "0X" : "0x";
					if (item->zero && !item->left && item->precision < 0)
						while (int64_t(prefix.size() + digits.size()) < item->width)
							digits.push_back('0');
					std::reverse(digits.begin(), digits.end());
					for (const char character : prefix + digits)
						body.push_back(char16_t(character));
				}
				while (int64_t(body.size()) < item->width)
					body.insert(item->left ? body.end() : body.begin(), u' ');
				output += body;
			}
			const int64_t capacity = bounded ? int64_t(int32_t(uint32_t(head[1]))) : 1024;
			if (capacity <= 0)
				throw uint32_t(87);
			if (int64_t(output.size()) > capacity - 1)
				output.resize(size_t(capacity - 1));
			std::vector<uint8_t> bytes;
			if (ansi) {
				const auto narrow = to_ansi(output, 1252);
				bytes.assign(narrow.begin(), narrow.end());
				bytes.push_back(0);
				result = narrow.size();
			} else {
				bytes.resize((output.size() + 1) * 2);
				for (size_t index = 0; index < output.size(); ++index)
					write_le(bytes, index * 2, output[index], 2);
				result = output.size();
			}
			process.write_memory(head[0], bytes);
		}
	} catch (uint32_t error) { environment.set_last_error(error); }
	catch (const std::runtime_error &failure) {
		const std::string_view message = failure.what();
		if (message.find("readable") == message.npos && message.find("writable") == message.npos) throw;
		environment.set_last_error(998);
		result = 0;
	}
	process.return_api(result);
	return true;
}

}
