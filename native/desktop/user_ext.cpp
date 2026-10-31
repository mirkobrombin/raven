#include "windows.h"
#include "gdi.h"
#include "nt/text.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iostream>
#include <sys/mman.h>

namespace raven {

namespace {

struct Rect {
	int32_t left, top, right, bottom;
};

Rect read_rect(ApiCall &call, uint64_t address)
{
	Rect rect;
	const auto bytes = call.read(address, 16);
	std::memcpy(&rect, bytes.data(), 16);
	return rect;
}

void write_rect(ApiCall &call, uint64_t address, const Rect &rect)
{
	call.write(address, std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(&rect), 16));
}

// US keyboard layout: virtual key by set-1 scan code (non-extended keys).
constexpr uint8_t scan_to_vk[89] = {
	0, 0x1b, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', 0xbd, 0xbb, 0x08, 0x09,
	'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', 0xdb, 0xdd, 0x0d, 0x11, 'A', 'S',
	'D', 'F', 'G', 'H', 'J', 'K', 'L', 0xba, 0xde, 0xc0, 0x10, 0xdc, 'Z', 'X', 'C', 'V',
	'B', 'N', 'M', 0xbc, 0xbe, 0xbf, 0x10, 0x6a, 0x12, 0x20, 0x14, 0x70, 0x71, 0x72, 0x73, 0x74,
	0x75, 0x76, 0x77, 0x78, 0x79, 0x90, 0x91, 0x24, 0x26, 0x21, 0x6d, 0x25, 0x0c, 0x27, 0x6b, 0x23,
	0x28, 0x22, 0x2d, 0x2e, 0, 0, 0, 0x7a, 0x7b,
};

// Character a virtual key produces on the US layout; the second value applies with Shift.
std::pair<char16_t, char16_t> key_characters(uint32_t vk)
{
	if (vk >= 'A' && vk <= 'Z')
		return {char16_t(vk + 32), char16_t(vk)};
	if (vk >= '0' && vk <= '9') {
		static const char16_t shifted[] = u")!@#$%^&*(";
		return {char16_t(vk), shifted[vk - '0']};
	}
	switch (vk) {
	case 0x20: return {' ', ' '};
	case 0x0d: return {'\r', '\r'};
	case 0x09: return {'\t', '\t'};
	case 0x08: return {'\b', '\b'};
	case 0x1b: return {0x1b, 0x1b};
	case 0xba: return {';', ':'};
	case 0xbb: return {'=', '+'};
	case 0xbc: return {',', '<'};
	case 0xbd: return {'-', '_'};
	case 0xbe: return {'.', '>'};
	case 0xbf: return {'/', '?'};
	case 0xc0: return {'`', '~'};
	case 0xdb: return {'[', '{'};
	case 0xdc: return {'\\', '|'};
	case 0xdd: return {']', '}'};
	case 0xde: return {'\'', '"'};
	case 0x6a: return {'*', '*'};
	case 0x6b: return {'+', '+'};
	case 0x6d: return {'-', '-'};
	case 0x6e: return {'.', '.'};
	case 0x6f: return {'/', '/'};
	default:
		if (vk >= 0x60 && vk <= 0x69)
			return {char16_t('0' + vk - 0x60), char16_t('0' + vk - 0x60)};
		return {0, 0};
	}
}

std::u16string key_name(uint32_t vk)
{
	if (vk >= 'A' && vk <= 'Z') return std::u16string(1, char16_t(vk));
	if (vk >= '0' && vk <= '9') return std::u16string(1, char16_t(vk));
	if (vk >= 0x70 && vk <= 0x87) return u"F" + from_utf8(std::to_string(vk - 0x6f));
	switch (vk) {
	case 0x08: return u"Backspace";
	case 0x09: return u"Tab";
	case 0x0d: return u"Enter";
	case 0x10: return u"Shift";
	case 0x11: return u"Ctrl";
	case 0x12: return u"Alt";
	case 0x14: return u"Caps Lock";
	case 0x1b: return u"Esc";
	case 0x20: return u"Space";
	case 0x21: return u"Page Up";
	case 0x22: return u"Page Down";
	case 0x23: return u"End";
	case 0x24: return u"Home";
	case 0x25: return u"Left";
	case 0x26: return u"Up";
	case 0x27: return u"Right";
	case 0x28: return u"Down";
	case 0x2d: return u"Insert";
	case 0x2e: return u"Delete";
	case 0x90: return u"Num Lock";
	default: {
		const auto pair = key_characters(vk);
		return pair.first ? std::u16string(1, pair.first) : u"";
	}
	}
}

// Fills NONCLIENTMETRICSW with the metrics this desktop uses; the fonts are the classic 9 point UI face.
void write_logfont(std::vector<uint8_t> &bytes, size_t at, int32_t height, int32_t weight)
{
	std::memcpy(bytes.data() + at, &height, 4);
	std::memcpy(bytes.data() + at + 16, &weight, 4);
	bytes[at + 23] = 1;
	bytes[at + 26] = 5;
	bytes[at + 27] = 0x22;
	const char16_t face[] = u"Segoe UI";
	for (size_t i = 0; i < 8; ++i) {
		bytes[at + 28 + i * 2] = uint8_t(face[i]);
		bytes[at + 29 + i * 2] = uint8_t(face[i] >> 8);
	}
}

}

uint64_t NativeWindows::scratch_allocate(size_t size)
{
	constexpr size_t arena = 256 * 1024;
	if (!scratch_base)
		scratch_base = process.map_memory(arena, PROT_READ | PROT_WRITE);
	// A simple ring: handlers copy what they need out before the next call reuses the space.
	size = (size + 15) & ~size_t(15);
	if (size > arena)
		throw uint32_t(8);
	if (scratch_used + size > arena)
		scratch_used = 0;
	const auto address = scratch_base + scratch_used;
	scratch_used += size;
	return address;
}

bool NativeWindows::hook_applies(const Hook &hook) const
{
	return !hook.thread || hook.thread == environment.thread_id();
}

bool NativeWindows::has_hook(int32_t id) const
{
	return std::any_of(hooks.begin(), hooks.end(), [&](const Hook &hook) { return hook.id == id && hook_applies(hook); });
}

// Runs the hook chain of one kind; `after` names the hook whose successors should run (CallNextHookEx).
uint64_t NativeWindows::call_hooks(int32_t id, int32_t code, uint64_t wparam, uint64_t lparam, uint64_t after)
{
	bool started = after == 0;
	for (const auto &hook : std::vector<Hook>(hooks)) {
		if (hook.id != id || !hook_applies(hook))
			continue;
		if (!started) {
			started = hook.handle == after;
			continue;
		}
		const uint64_t arguments[] = {uint64_t(uint32_t(code)), wparam, lparam};
		const auto result = invoke(hook.procedure, arguments);
		if (result.stop != NativeStop::returned)
			throw result;
		return result.value;
	}
	return 0;
}

void NativeWindows::caret_sync()
{
	if (!caret || !caret->window || !objects.window(caret->window))
		return;
	const bool want = caret->hidden <= 0 && objects.window_visible(caret->window);
	if (want == caret->drawn)
		return;
	if (caret->blank) {
		caret->drawn = want;
		return;
	}
	const auto dc = surface_dc(caret->window);
	auto *device = objects.dc(dc);
	if (!device)
		return;
	try {
		Painter painter(objects, *device);
		for (int32_t y = 0; y < caret->height; ++y)
			for (int32_t x = 0; x < caret->width; ++x)
				painter.plot(int64_t(caret->x) + x + device->origin_x, int64_t(caret->y) + y + device->origin_y, 0, 6);
		if (painter.changed())
			drawing(caret->window);
	} catch (const uint32_t &) {
		return;
	}
	caret->drawn = want;
}

void NativeWindows::caret_idle()
{
	if (!caret || caret->hidden > 0)
		return;
	const auto now = std::chrono::steady_clock::now();
	if (now < caret->next)
		return;
	caret->next = now + std::chrono::milliseconds(530);
	if (caret->blank) {
		caret->drawn = !caret->drawn;
		return;
	}
	// Blink: invert the rectangle in place without changing the show count.
	const auto dc = surface_dc(caret->window);
	auto *device = objects.dc(dc);
	if (!device)
		return;
	try {
		Painter painter(objects, *device);
		for (int32_t y = 0; y < caret->height; ++y)
			for (int32_t x = 0; x < caret->width; ++x)
				painter.plot(int64_t(caret->x) + x + device->origin_x, int64_t(caret->y) + y + device->origin_y, 0, 6);
		if (painter.changed())
			drawing(caret->window);
		caret->drawn = !caret->drawn;
	} catch (const uint32_t &) {
	}
}

void NativeWindows::track_pointer(uint64_t window)
{
	if (!tracking.window || !(tracking.flags & 2))
		return;
	// WM_MOUSELEAVE when the pointer moves out of the tracked window (children count as inside).
	if (window == tracking.window || objects.is_child(tracking.window, window))
		return;
	const auto tracked = tracking.window;
	tracking = {};
	if (objects.window(tracked))
		post(tracked, 0x2a3, 0, 0);
}

void NativeWindows::register_ext_api(ApiRegistry &registry)
{
	// ---- DPI: one 96 dpi screen ----
	registry.add("GetDpiForSystem", 0, [](ApiCall &call) { call.ret(96); });
	registry.add("GetDpiForWindow", 1, [](ApiCall &call) { call.ret(96); });
	registry.add("GetSystemDpiForProcess", 1, [](ApiCall &call) { call.ret(96); });
	registry.add("GetMessagePos", 0, [this](ApiCall &call) { call.ret(uint32_t(uint16_t(cursor_x)) | (uint32_t(uint16_t(cursor_y)) << 16)); });
	registry.add("mouse_event", 5, [](ApiCall &call) { call.ret(0); });
	registry.add("DragDetect", 3, [](ApiCall &call) { call.ret(0); });
	registry.add("SetWindowContextHelpId", 2, [](ApiCall &call) { call.ret(1); });
	registry.add("SubtractRect", 3, [](ApiCall &call) {
		const auto minuend = read_rect(call, call[1]), subtrahend = read_rect(call, call[2]);
		Rect result = minuend;
		const bool empty = minuend.right <= minuend.left || minuend.bottom <= minuend.top;
		const Rect overlap{std::max(minuend.left, subtrahend.left), std::max(minuend.top, subtrahend.top),
			std::min(minuend.right, subtrahend.right), std::min(minuend.bottom, subtrahend.bottom)};
		if (empty) {
			write_rect(call, call[0], {0, 0, 0, 0});
			call.ret(0);
			return;
		}
		// The difference stays a rectangle only when the subtrahend spans the minuend on one axis.
		if (overlap.right > overlap.left && overlap.bottom > overlap.top) {
			if (overlap.left <= minuend.left && overlap.right >= minuend.right) {
				if (overlap.top <= minuend.top)
					result.top = overlap.bottom;
				else if (overlap.bottom >= minuend.bottom)
					result.bottom = overlap.top;
			} else if (overlap.top <= minuend.top && overlap.bottom >= minuend.bottom) {
				if (overlap.left <= minuend.left)
					result.left = overlap.right;
				else if (overlap.right >= minuend.right)
					result.right = overlap.left;
			}
		}
		const bool nothing = result.right <= result.left || result.bottom <= result.top;
		write_rect(call, call[0], nothing ? Rect{0, 0, 0, 0} : result);
		call.ret(!nothing);
	});
	registry.add("GetTopWindow", 1, [this](ApiCall &call) {
		const auto parent = call[0] ? call[0] : desktop_window();
		const auto children = objects.child_windows(call[0] ? parent : 0);
		call.ret(children.empty() ? 0 : children.back());
	});

	// Message filtering never blocks.
	registry.add("ChangeWindowMessageFilterEx", 4, [this](ApiCall &call) {
		if (!objects.window(call[0])) {
			call.fail(1400);
			return;
		}
		call.ret(1);
	});

	// ---- timers owned by the system (carets, control scrolling) ----
	registry.add("SetSystemTimer", 4, [this](ApiCall &call) {
		if (call[0] && !objects.window(call[0])) {
			call.fail(1400);
			return;
		}
		const auto interval = std::clamp<uint32_t>(call.u32(2), 10, 0x7fffffff);
		std::erase_if(timers, [&](const Timer &timer) { return timer.window == call[0] && timer.id == call[1]; });
		if (timers.size() >= 1024) {
			call.fail(8);
			return;
		}
		timers.push_back({call[0], call[1], call[3], interval, std::chrono::steady_clock::now() + std::chrono::milliseconds(interval), true, environment.thread_id()});
		call.ret(call[1]);
	});
	registry.add("KillSystemTimer", 2, [this](ApiCall &call) {
		const auto before = timers.size();
		std::erase_if(timers, [&](const Timer &timer) { return timer.window == call[0] && timer.id == call[1]; });
		call.ret(before != timers.size());
	});

	// ---- caret ----
	registry.add("CreateCaret", 4, [this](ApiCall &call) {
		if (!objects.window(call[0])) {
			call.fail(1400);
			return;
		}
		if (caret && caret->drawn) {
			caret->hidden = 1;
			caret_sync();
		}
		int32_t width = call.i32(2), height = call.i32(3);
		if (width <= 0) width = 1;
		if (height <= 0) height = 16;
		caret = Caret{call[0], width, height, 0, 0, 1, false, false, std::chrono::steady_clock::now() + std::chrono::milliseconds(530)};
		if (call[1] > 1)
			if (const auto *shape = objects.bitmap(call[1]); shape && shape->depth == 1 && !shape->shared && !shape->address)
				caret->blank = std::all_of(shape->bits.begin(), shape->bits.end(), [](uint8_t byte) { return byte == 0; });
		call.ret(1);
	});
	registry.add("DestroyCaret", 0, [this](ApiCall &call) {
		if (!caret) {
			call.fail(1413);
			return;
		}
		caret->hidden = 1;
		caret_sync();
		caret.reset();
		call.ret(1);
	});
	registry.add("SetCaretPos", 2, [this](ApiCall &call) {
		if (!caret) {
			call.fail(1413);
			return;
		}
		const bool shown = caret->drawn;
		if (shown) {
			const auto hidden = caret->hidden;
			caret->hidden = 1;
			caret_sync();
			caret->hidden = hidden;
		}
		caret->x = call.i32(0);
		caret->y = call.i32(1);
		caret_sync();
		call.ret(1);
	});
	registry.add("GetCaretPos", 1, [this](ApiCall &call) {
		if (!caret) {
			call.fail(1413);
			return;
		}
		call.store<int32_t>(call[0], caret->x);
		call.store<int32_t>(call[0] + 4, caret->y);
		call.ret(1);
	});
	registry.add("ShowCaret", 1, [this](ApiCall &call) {
		if (!caret || (call[0] && caret->window != call[0])) {
			call.ret(0);
			return;
		}
		caret->hidden = std::max(0, caret->hidden - 1);
		caret_sync();
		call.ret(1);
	});
	registry.add("HideCaret", 1, [this](ApiCall &call) {
		if (!caret || (call[0] && caret->window != call[0])) {
			call.ret(0);
			return;
		}
		++caret->hidden;
		caret_sync();
		call.ret(1);
	});
	registry.add("SetCaretBlinkTime", 1, [](ApiCall &call) { call.ret(1); });

	// ---- in-process clipboard ----
	registry.add("RegisterClipboardFormatW", 1, [this](ApiCall &call) {
		const auto name = call.wide(call[0], 255);
		if (name.empty()) {
			call.fail(87);
			return;
		}
		auto found = registered_messages.find(u"clipboard:" + name);
		if (found == registered_messages.end())
			found = registered_messages.emplace(u"clipboard:" + name, uint32_t(0xc000 + registered_messages.size())).first;
		call.ret(found->second);
	});
	registry.add("RegisterClipboardFormatA", 1, [this](ApiCall &call) {
		const auto name = from_ansi(call.narrow(call[0], 255), 1252);
		if (name.empty()) {
			call.fail(87);
			return;
		}
		auto found = registered_messages.find(u"clipboard:" + name);
		if (found == registered_messages.end())
			found = registered_messages.emplace(u"clipboard:" + name, uint32_t(0xc000 + registered_messages.size())).first;
		call.ret(found->second);
	});
	// The name of a registered format; the predefined formats have none.
	const auto format_name = [this](uint32_t format) -> std::optional<std::u16string> {
		for (const auto &[key, id] : registered_messages)
			if (id == format && key.starts_with(u"clipboard:"))
				return key.substr(10);
		return std::nullopt;
	};
	registry.add("GetClipboardFormatNameW", 3, [format_name](ApiCall &call) {
		const auto name = format_name(call.u32(0));
		const int32_t capacity = call.i32(2);
		if (!name || capacity <= 0) {
			call.fail(87);
			return;
		}
		const auto text = name->substr(0, size_t(capacity - 1));
		call.put_wide(call[1], text, size_t(capacity));
		call.ret(text.size());
	});
	registry.add("GetClipboardFormatNameA", 3, [format_name](ApiCall &call) {
		const auto name = format_name(call.u32(0));
		const int32_t capacity = call.i32(2);
		if (!name || capacity <= 0) {
			call.fail(87);
			return;
		}
		auto text = to_ansi(*name, 1252);
		text = text.substr(0, size_t(capacity - 1));
		call.put_narrow(call[1], text, size_t(capacity));
		call.ret(text.size());
	});
	// Text copied by the guest is offered to the host selection when the clipboard closes; text another host client
	// copied replaces the contents the next time the guest looks, with the line ends converted on both sides.
	const auto publish_host = [this](ApiCall &call) {
		if (!presentation.clipboard_publish || clipboard_published == clipboard_sequence)
			return;
		clipboard_published = clipboard_sequence;
		const auto found = clipboard_data.find(13);
		if (found == clipboard_data.end() || !found->second)
			return;
		const auto text = to_utf8(call.wide(found->second));
		std::string host;
		for (size_t at = 0; at < text.size(); ++at)
			if (text[at] != '\r' || at + 1 >= text.size() || text[at + 1] != '\n')
				host.push_back(text[at]);
		presentation.clipboard_publish(host);
	};
	const auto import_host = [this](ApiCall &call) {
		if (!presentation.clipboard_fetch || !global_allocator || clipboard_window)
			return;
		const auto fetched = presentation.clipboard_fetch();
		if (!fetched || fetched->first == host_selection)
			return;
		host_selection = fetched->first;
		std::u16string wide;
		for (const auto character : from_utf8(fetched->second)) {
			if (character == u'\n' && (wide.empty() || wide.back() != u'\r'))
				wide.push_back(u'\r');
			wide.push_back(character);
		}
		const auto unicode = global_allocator((wide.size() + 1) * 2), ansi = global_allocator(wide.size() + 1);
		if (!unicode || !ansi)
			return;
		call.write(unicode, std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(wide.c_str()), (wide.size() + 1) * 2));
		std::string narrow;
		for (const auto character : wide)
			narrow.push_back(character < 0x100 ? char(character) : '?');
		call.write(ansi, std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(narrow.c_str()), narrow.size() + 1));
		clipboard_data.clear();
		clipboard_data[13] = unicode;
		clipboard_data[1] = ansi;
		clipboard_owner = 0;
		++clipboard_sequence;
		clipboard_published = clipboard_sequence;
	};
	registry.add("OpenClipboard", 1, [this, import_host](ApiCall &call) {
		if (clipboard_window == UINT64_MAX) {
			call.fail(1418);
			return;
		}
		import_host(call);
		clipboard_window = call[0] ? call[0] : UINT64_MAX - 1;
		call.ret(1);
	});
	registry.add("CloseClipboard", 0, [this, publish_host](ApiCall &call) {
		if (!clipboard_window) {
			call.fail(1418);
			return;
		}
		clipboard_window = 0;
		publish_host(call);
		call.ret(1);
	});
	registry.add("EmptyClipboard", 0, [this](ApiCall &call) {
		if (!clipboard_window) {
			call.fail(1418);
			return;
		}
		clipboard_data.clear();
		clipboard_owner = clipboard_window == UINT64_MAX - 1 ? 0 : clipboard_window;
		++clipboard_sequence;
		call.ret(1);
	});
	registry.add("SetClipboardData", 2, [this](ApiCall &call) {
		if (!clipboard_window) {
			call.fail(1418);
			return;
		}
		if (call[1])
			clipboard_data[call.u32(0)] = call[1];
		else
			clipboard_data.erase(call.u32(0));
		++clipboard_sequence;
		call.ret(call[1]);
	});
	registry.add("GetClipboardData", 1, [this](ApiCall &call) {
		if (!clipboard_window) {
			call.fail(1418);
			return;
		}
		const auto found = clipboard_data.find(call.u32(0));
		call.ret(found == clipboard_data.end() ? 0 : found->second);
	});
	registry.add("IsClipboardFormatAvailable", 1, [this, import_host](ApiCall &call) { import_host(call); call.ret(clipboard_data.contains(call.u32(0))); });
	registry.add("CountClipboardFormats", 0, [this, import_host](ApiCall &call) { import_host(call); call.ret(clipboard_data.size()); });
	registry.add("EnumClipboardFormats", 1, [this, import_host](ApiCall &call) {
		import_host(call);
		const auto next = clipboard_data.upper_bound(call.u32(0));
		call.ret(next == clipboard_data.end() ? 0 : next->first);
	});
	registry.add("GetClipboardOwner", 0, [this](ApiCall &call) { call.ret(clipboard_owner); });
	registry.add("GetClipboardSequenceNumber", 0, [this, import_host](ApiCall &call) { import_host(call); call.ret(clipboard_sequence); });
	registry.add("SetClipboardViewer", 1, [](ApiCall &call) { call.ret(0); });
	registry.add("ChangeClipboardChain", 2, [](ApiCall &call) { call.ret(1); });

	// ---- keyboard state and layout ----
	// The left and right modifier keys share the state of their generic key.
	const auto generic_key = [](uint32_t vk) { return vk >= 0xa0 && vk <= 0xa5 ? 0x10 + (vk - 0xa0) / 2 : vk; };
	registry.add("GetKeyState", 1, [this, generic_key](ApiCall &call) {
		const auto vk = generic_key(call.u32(0) & 0xff);
		call.ret(uint16_t(((vk_state[vk] & 0x80) ? 0xff80 : 0) | (vk_state[vk] & 1)));
	});
	registry.add("GetAsyncKeyState", 1, [this, generic_key](ApiCall &call) {
		const auto vk = generic_key(call.u32(0) & 0xff);
		call.ret((vk_async[vk] & 0x80) ? 0x8000 : 0);
	});
	registry.add("GetKeyboardState", 1, [this](ApiCall &call) {
		call.write(call[0], std::span<const uint8_t>(vk_state.data(), vk_state.size()));
		call.ret(1);
	});
	registry.add("SetKeyboardState", 1, [this](ApiCall &call) {
		const auto bytes = call.read(call[0], vk_state.size());
		std::copy(bytes.begin(), bytes.end(), vk_state.begin());
		call.ret(1);
	});
	for (const bool extended : {false, true}) {
		registry.add(extended ? "GetClassInfoExA" : "GetClassInfoA", 3, [this, extended](ApiCall &call) {
			const uint64_t name = call[1];
			const uint64_t args[] = {call[0], name, call[2]};
			try {
				const uint16_t id = name && name <= UINT16_MAX ? atom(name, call[0]) : objects.find_class(call[0], call.narrow(name));
				call.ret(fill_class_info(id, extended, args));
			} catch (uint32_t error) {
				call.fail(error);
			}
		});
	}
	registry.add("MapVirtualKeyW", 2, [](ApiCall &call) {
		const uint32_t code = call.u32(0), type = call.u32(1);
		if (type == 0 || type == 4) {
			for (uint32_t scan = 1; scan < 89; ++scan)
				if (scan_to_vk[scan] == code || (code == 0xa0 && scan == 42) || (code == 0xa1 && scan == 54) || (code == 0xa2 && scan == 29) || (code == 0xa4 && scan == 56)) {
					call.ret(scan);
					return;
				}
			call.ret(0);
		} else if (type == 1 || type == 3) {
			call.ret(code < 89 ? scan_to_vk[code] : 0);
		} else if (type == 2) {
			call.ret(key_characters(code).second ? std::toupper(key_characters(code).first) : 0);
		} else {
			call.ret(0);
		}
	});
	registry.add("MapVirtualKeyA", 2, [](ApiCall &call) {
		const uint32_t code = call.u32(0), type = call.u32(1);
		if (type == 1 || type == 3)
			call.ret(code < 89 ? scan_to_vk[code] : 0);
		else if (type == 0) {
			for (uint32_t scan = 1; scan < 89; ++scan)
				if (scan_to_vk[scan] == code) {
					call.ret(scan);
					return;
				}
			call.ret(0);
		} else {
			call.ret(key_characters(code).first ? std::toupper(key_characters(code).first) : 0);
		}
	});
	registry.add("VkKeyScanW", 1, [](ApiCall &call) {
		const char16_t c = char16_t(call.u32(0));
		for (uint32_t vk = 8; vk < 0xe0; ++vk) {
			const auto pair = key_characters(vk);
			if (pair.first == c) {
				call.ret(vk);
				return;
			}
			if (pair.second == c && pair.second != pair.first) {
				call.ret(vk | 0x100);
				return;
			}
		}
		call.ret(0xffff);
	});
	registry.add("GetKeyNameTextW", 3, [](ApiCall &call) {
		const uint32_t scan = (call.u32(0) >> 16) & 0xff;
		const bool extended = call.u32(0) & (1U << 24);
		uint32_t vk = scan < 89 ? scan_to_vk[scan] : 0;
		if (extended) {
			if (scan == 0x47) vk = 0x24; else if (scan == 0x4f) vk = 0x23; else if (scan == 0x48) vk = 0x26; else if (scan == 0x50) vk = 0x28;
			else if (scan == 0x4b) vk = 0x25; else if (scan == 0x4d) vk = 0x27; else if (scan == 0x52) vk = 0x2d; else if (scan == 0x53) vk = 0x2e;
			else if (scan == 0x49) vk = 0x21; else if (scan == 0x51) vk = 0x22;
		}
		const auto name = key_name(vk);
		if (name.empty() || call.i32(2) <= 0) {
			call.ret(0);
			return;
		}
		const size_t capacity = size_t(call.i32(2));
		call.put_wide(call[1], name.substr(0, capacity - 1), capacity);
		call.ret(std::min(name.size(), capacity - 1));
	});
	const auto to_unicode = [](ApiCall &call, bool wide) {
		const uint32_t vk = call.u32(0);
		const auto state = call.read(call[2], 256);
		const bool shift = state[0x10] & 0x80, control = state[0x11] & 0x80, caps = state[0x14] & 1;
		auto pair = key_characters(vk);
		char16_t result = shift ? pair.second : pair.first;
		if (vk >= 'A' && vk <= 'Z' && caps)
			result = shift ? pair.first : pair.second;
		if (control && vk >= 'A' && vk <= 'Z')
			result = char16_t(vk - 'A' + 1);
		if (!result) {
			call.ret(0);
			return;
		}
		if (wide)
			call.store<uint16_t>(call[3], result);
		else
			call.store<uint16_t>(call[3], uint16_t(result & 0xff));
		call.ret(1);
	};
	registry.add("ToAscii", 5, [=](ApiCall &call) { to_unicode(call, false); });
	registry.add("ToUnicode", 6, [=](ApiCall &call) {
		// ToUnicode takes a wide buffer and a count; reuse the translation with a two-byte store.
		to_unicode(call, true);
	});
	// The Ex forms add the keyboard layout to translate with; the only layout here is the one the plain forms use.
	registry.add("ToUnicodeEx", 7, [=](ApiCall &call) { to_unicode(call, true); });
	registry.add("ToAsciiEx", 6, [=](ApiCall &call) { to_unicode(call, false); });

	// ---- tracking and hooks ----
	registry.add("TrackMouseEvent", 1, [this](ApiCall &call) {
		const uint32_t flags = call.load<uint32_t>(call[0] + 4);
		const uint64_t window = call.load<uint64_t>(call[0] + 8);
		if (flags & 0x80000000U) {
			if (tracking.window == window)
				tracking = {};
			call.ret(1);
			return;
		}
		if (!objects.window(window)) {
			call.fail(1400);
			return;
		}
		tracking = {window, flags};
		call.ret(1);
	});
	registry.add("_TrackMouseEvent", 1, [this](ApiCall &call) {
		const uint32_t flags = call.load<uint32_t>(call[0] + 4);
		const uint64_t window = call.load<uint64_t>(call[0] + 8);
		if (objects.window(window) && !(flags & 0x80000000U))
			tracking = {window, flags};
		call.ret(1);
	});
	registry.add("SetWindowsHookExW", 4, [this](ApiCall &call) {
		const int32_t id = call.i32(0);
		if (id < -1 || id > 14 || !call[1]) {
			call.fail(87);
			return;
		}
		Hook hook{++next_hook, id, call[1], call.u32(3)};
		hooks.insert(hooks.begin(), hook);
		call.ret(hook.handle);
	});
	registry.add("SetWindowsHookExA", 4, [this](ApiCall &call) {
		const int32_t id = call.i32(0);
		if (id < -1 || id > 14 || !call[1]) {
			call.fail(87);
			return;
		}
		Hook hook{++next_hook, id, call[1], call.u32(3)};
		hooks.insert(hooks.begin(), hook);
		call.ret(hook.handle);
	});
	// AnimateWindow shows or hides at once: the slide, roll and blend effects only change how the transition looks. Showing a visible
	// window or hiding a hidden one fails, as it does on Windows.
	registry.add("AnimateWindow", 3, [this](ApiCall &call) {
		const uint32_t flags = call.u32(2);
		if (!objects.window(call[0])) {
			call.fail(1400);
			return;
		}
		const bool hide = flags & 0x10000, visible = objects.window_visible(call[0]);
		if (hide != visible) {
			call.ret(0);
			return;
		}
		try {
			show_window(call[0], hide ? 0 : (flags & 0x20000) ? 5 : 8);
		} catch (uint32_t error) {
			call.fail(error);
			return;
		}
		call.ret(1);
	});
	registry.add("UnhookWindowsHookEx", 1, [this](ApiCall &call) {
		const auto before = hooks.size();
		std::erase_if(hooks, [&](const Hook &hook) { return hook.handle == call[0]; });
		if (before == hooks.size())
			call.fail(1404);
		else
			call.ret(1);
	});
	registry.add("CallNextHookEx", 4, [this](ApiCall &call) {
		const auto found = std::find_if(hooks.begin(), hooks.end(), [&](const Hook &hook) { return hook.handle == call[0]; });
		if (found == hooks.end()) {
			call.ret(0);
			return;
		}
		call.ret(call_hooks(found->id, call.i32(1), call[2], call[3], call[0]));
	});
	registry.add("CallMsgFilterW", 2, [this](ApiCall &call) {
		call.ret(call_hooks(-1, call.i32(1), 0, call[0]));
	});

	// ---- window regions and the system menu ----
	registry.add("SetWindowRgn", 3, [this](ApiCall &call) {
		if (!objects.window(call[0])) {
			call.fail(1400);
			return;
		}
		call.ret(1);
	});
	registry.add("GetWindowRgn", 2, [this](ApiCall &call) {
		if (!objects.window(call[0])) {
			call.fail(1400);
			return;
		}
		call.ret(0);
	});
	registry.add("GetSystemMenu", 2, [this](ApiCall &call) {
		const auto *window = objects.window(call[0]);
		if (!window) {
			call.fail(1400);
			return;
		}
		if (call.u32(1)) {
			call.ret(0);
			return;
		}
		const auto handle = system_menu(call[0]);
		if (!handle) {
			call.fail(8);
			return;
		}
		call.ret(handle);
	});

	// ---- scrolling the client area ----
	registry.add("ScrollWindowEx", 8, [this](ApiCall &call) {
		auto *window = objects.window(call[0]);
		if (!window) {
			call.fail(1400);
			return;
		}
		const int32_t dx = call.i32(1), dy = call.i32(2);
		Rect area{0, 0, window->width, window->height};
		if (call[3])
			area = read_rect(call, call[3]);
		Rect clip = area;
		if (call[4]) {
			const auto given = read_rect(call, call[4]);
			clip = {std::max(area.left, given.left), std::max(area.top, given.top), std::min(area.right, given.right), std::min(area.bottom, given.bottom)};
		}
		const uint32_t flags = call.u32(7);
		const auto dc_handle = surface_dc(call[0]);
		auto *dc = objects.dc(dc_handle);
		if (dc && clip.right > clip.left && clip.bottom > clip.top && (dx || dy)) {
			// The destination is the scrolled rectangle limited to the clip rectangle.
			const Rect target{std::max(clip.left, area.left + dx), std::max(clip.top, area.top + dy),
				std::min(clip.right, area.right + dx), std::min(clip.bottom, area.bottom + dy)};
			if (target.right > target.left && target.bottom > target.top)
				blit(objects, *dc, target.left, target.top, target.right - target.left, target.bottom - target.top, dc,
					target.left - dx, target.top - dy, target.right - target.left, target.bottom - target.top, 0x00cc0020);
			// What the scroll did not cover must be redrawn.
			const Rect covered = target;
			auto invalidate = [&](Rect part) {
				if (part.right > part.left && part.bottom > part.top && (flags & 2))
					objects.invalidate(call[0], {part.left, part.top, part.right, part.bottom}, (flags & 4) != 0);
			};
			invalidate({clip.left, clip.top, clip.right, covered.top});
			invalidate({clip.left, covered.bottom, clip.right, clip.bottom});
			invalidate({clip.left, covered.top, covered.left, covered.bottom});
			invalidate({covered.right, covered.top, clip.right, covered.bottom});
			if (flags & 1)
				for (const auto child : objects.child_windows(call[0]))
					shift_window(child, dx, dy);
			drawing(call[0]);
		}
		if (call[6])
			write_rect(call, call[6], {0, 0, 0, 0});
		call.ret(1);
	});
	registry.add("InvalidateRgn", 3, [this](ApiCall &call) {
		if (!objects.window(call[0])) {
			call.fail(1400);
			return;
		}
		if (!call[1]) {
			const auto *window = objects.window(call[0]);
			objects.invalidate(call[0], {0, 0, window->width, window->height}, call.u32(2) != 0);
		} else if (const auto *region = objects.region(call[1])) {
			const auto bounds = region->bounds();
			objects.invalidate(call[0], bounds, call.u32(2) != 0);
		} else {
			call.fail(6);
			return;
		}
		call.ret(1);
	});
	registry.add("GetDCEx", 3, [this](ApiCall &call) {
		const auto target = call[0] ? call[0] : desktop_window();
		if (!objects.window(target)) {
			// A window of another process: a context over the pixels shared with it.
			try {
				call.ret(foreign_dc(target));
			} catch (uint32_t error) {
				call.fail(error);
			}
			return;
		}
		// Device contexts are always window-relative and clipped to the window; region flags narrow them further.
		const uint32_t flags = call.u32(2);
		const bool whole = flags & 1;
		const auto base = surface_dc(target);
		auto *dc = objects.dc(base);
		if (!dc) {
			call.fail(1400);
			return;
		}
		DesktopDC device{dc->bitmap, dc->default_bitmap, target};
		device.leased = true;
		device.font = dc->font;
		objects.configure_window_dc(target, device, whole);
		if (call[1] && (flags & 0x60)) {
			if (const auto *region = objects.region(call[1])) {
				DesktopRegion copy = *region;
				device.application_clip = std::move(copy);
			}
		}
		const auto handle = objects.add(device);
		if (!handle) {
			call.fail(8);
			return;
		}
		call.ret(handle);
	});

	// ---- ANSI entry points over the Unicode implementation ----
	const auto call_wide = [this](ApiCall &call, uint64_t window, uint32_t message, uint64_t wparam, uint64_t lparam) -> uint64_t {
		return send(window, message, wparam, lparam);
	};
	const auto narrow_to_scratch = [this](const std::string &text) {
		const auto wide = from_ansi(text, 1252);
		std::vector<uint8_t> bytes((wide.size() + 1) * 2, 0);
		std::memcpy(bytes.data(), wide.data(), wide.size() * 2);
		const auto at = scratch_allocate(bytes.size());
		process.write_memory(at, bytes);
		return at;
	};
	// Messages that carry text need their strings converted in both directions.
	const auto send_ansi = [=, this](ApiCall &call, uint64_t window, uint32_t message, uint64_t wparam, uint64_t lparam) -> uint64_t {
		const bool text_in = message == 0x0c || message == 0xc2 || message == 0x143 || message == 0x14a || message == 0x14c || message == 0x14d ||
			message == 0x180 || message == 0x181 || message == 0x18f || message == 0x18c;
		const bool text_out = message == 0x0d || message == 0x148 || message == 0x189;
		if (text_in && lparam)
			return call_wide(call, window, message, wparam, narrow_to_scratch(call.narrow(lparam, 65536)));
		if (text_out && lparam) {
			size_t capacity = message == 0x0d ? size_t(wparam) : 4096;
			if (capacity == 0)
				return 0;
			const auto buffer = scratch_allocate(capacity * 2 + 16);
			const auto result = call_wide(call, window, message, wparam, buffer);
			if (result == UINT64_MAX || (message == 0x0d && !capacity))
				return result;
			const auto text = call.wide(buffer, capacity);
			call.put_narrow(lparam, to_ansi(text, 1252), capacity);
			return std::min<size_t>(text.size(), capacity - 1);
		}
		if ((message == 0x102 || message == 0x106 || message == 0x103) && wparam < 0x100)
			wparam = from_ansi(std::string(1, char(wparam)), 1252)[0];
		return call_wide(call, window, message, wparam, lparam);
	};
	registry.add("SendMessageA", 4, [=](ApiCall &call) { call.ret(send_ansi(call, call[0], call.u32(1), call[2], call[3])); });
	registry.add("PostMessageA", 4, [=, this](ApiCall &call) {
		auto wparam = call[2];
		if ((call.u32(1) == 0x102 || call.u32(1) == 0x106) && wparam < 0x100)
			wparam = from_ansi(std::string(1, char(wparam)), 1252)[0];
		try {
			post(call[0], call.u32(1), wparam, call[3]);
			call.ret(1);
		} catch (const uint32_t &error) {
			call.fail(error);
		}
	});
	registry.add("SetWindowTextA", 2, [=, this](ApiCall &call) {
		send_ansi(call, call[0], 0x0c, 0, call[1]);
		call.ret(1);
	});
	registry.add("GetWindowTextA", 3, [=, this](ApiCall &call) {
		if (call.i32(2) <= 0) {
			call.ret(0);
			return;
		}
		call.ret(send_ansi(call, call[0], 0x0d, call[2], call[1]));
	});
	registry.add("TrackPopupMenu", 7, [=, this](ApiCall &call) {
		try {
			call.ret(track_popup_menu(call[0], call.u32(1), call.i32(2), call.i32(3), call[5]));
		} catch (const uint32_t &error) {
			call.fail(error);
		}
	});
	registry.add("TrackPopupMenuEx", 6, [=, this](ApiCall &call) {
		try {
			call.ret(track_popup_menu(call[0], call.u32(1), call.i32(2), call.i32(3), call[4]));
		} catch (const uint32_t &error) {
			call.fail(error);
		}
	});
	registry.add("GetWindowTextLengthA", 1, [=, this](ApiCall &call) { call.ret(send(call[0], 0x0e, 0, 0)); });
	registry.add("GetDlgItemTextA", 4, [=, this](ApiCall &call) {
		const auto child = [&]() -> uint64_t {
			for (const auto candidate : objects.child_windows(call[0]))
				if (uint32_t(objects.window(candidate)->child_id) == call.u32(1))
					return candidate;
			return 0;
		}();
		if (!child || call.i32(3) <= 0) {
			if (call.i32(3) > 0)
				call.store<uint8_t>(call[2], 0);
			call.ret(0);
			return;
		}
		call.ret(send_ansi(call, child, 0x0d, call[3], call[2]));
	});
	registry.add("SetDlgItemTextA", 3, [=, this](ApiCall &call) {
		for (const auto candidate : objects.child_windows(call[0]))
			if (uint32_t(objects.window(candidate)->child_id) == call.u32(1)) {
				send_ansi(call, candidate, 0x0c, 0, call[2]);
				call.ret(1);
				return;
			}
		call.ret(0);
	});
	registry.add("CallWindowProcA", 5, [this](ApiCall &call) {
		const uint64_t arguments[] = {call[1], call[2], call[3], call[4]};
		const auto result = invoke(call[0], arguments);
		if (result.stop != NativeStop::returned)
			throw result;
		call.ret(result.value);
	});
	registry.add("DefWindowProcA", 4, [=, this](ApiCall &call) {
		const uint64_t arguments[] = {call[0], call[1], call[2], call[3]};
		call.ret(default_message(arguments));
	});
	// Wide characters are single units here (surrogate pairs are two characters to these functions, as on Windows).
	registry.add("CharNextW", 1, [](ApiCall &call) {
		const auto c = call.load<uint16_t>(call[0]);
		call.ret(c ? call[0] + 2 : call[0]);
	});
	registry.add("CharPrevW", 2, [](ApiCall &call) { call.ret(call[1] > call[0] ? call[1] - 2 : call[0]); });
	registry.add("CharPrevA", 2, [](ApiCall &call) { call.ret(call[1] > call[0] ? call[1] - 1 : call[0]); });
	registry.add("CharNextA", 1, [](ApiCall &call) {
		const auto c = call.load<uint8_t>(call[0]);
		call.ret(c ? call[0] + 1 : call[0]);
	});
	// System-wide hot keys belong to the desktop, which delivers none to a guest: each registration is accepted and no WM_HOTKEY arrives.
	registry.add("RegisterHotKey", 4, [](ApiCall &call) { call.ret(1); });
	registry.add("UnregisterHotKey", 2, [](ApiCall &call) { call.ret(1); });
	registry.add("SystemParametersInfoA", 4, [](ApiCall &call) { call.ret(1); });
	// The metrics that carry fonts, laid out for a DPI: NONCLIENTMETRICSW (sizes, then the caption, small caption, menu, status and
	// message fonts), the icon title LOGFONTW and ICONMETRICSW (size, horizontal and vertical spacing, wrap flag, font). False for any
	// other action.
	const auto font_metrics = [](ApiCall &call, uint32_t action, uint32_t parameter, uint64_t pointer, uint32_t dpi) {
		const auto scale = [dpi](int32_t value) { return int32_t((int64_t(value) * dpi + 48) / 96); };
		switch (action) {
		case 0x29: {
			const uint32_t size = parameter ? parameter : call.load<uint32_t>(pointer);
			std::vector<uint8_t> bytes(std::max<size_t>(size, 504), 0);
			const auto put32 = [&](size_t at, int32_t value) { std::memcpy(bytes.data() + at, &value, 4); };
			put32(0, int32_t(size));
			put32(4, scale(1));
			put32(8, scale(17)); put32(12, scale(17));
			put32(16, scale(18)); put32(20, scale(19));
			write_logfont(bytes, 24, scale(-12), 400);
			put32(116, scale(17)); put32(120, scale(19));
			write_logfont(bytes, 124, scale(-12), 400);
			put32(216, scale(18)); put32(220, scale(19));
			write_logfont(bytes, 224, scale(-12), 400);
			write_logfont(bytes, 316, scale(-12), 400);
			write_logfont(bytes, 408, scale(-12), 400);
			if (size > 500)
				put32(500, scale(4));
			call.write(pointer, std::span<const uint8_t>(bytes.data(), std::min<size_t>(size, bytes.size())));
			return true;
		}
		case 0x1f: {
			std::vector<uint8_t> bytes(92, 0);
			write_logfont(bytes, 0, scale(-12), 400);
			call.write(pointer, bytes);
			return true;
		}
		case 0x2d: {
			std::vector<uint8_t> bytes(108, 0);
			const int32_t values[] = {108, scale(75), scale(75), 1};
			std::memcpy(bytes.data(), values, 16);
			write_logfont(bytes, 16, scale(-12), 400);
			call.write(pointer, bytes);
			return true;
		}
		default:
			return false;
		}
	};
	// SystemParametersInfoForDpi(action, parameter, data, update, dpi) answers only the font metrics, scaled for the DPI it names.
	registry.add("SystemParametersInfoForDpi", 5, [font_metrics](ApiCall &call) {
		if (!call[2] || !call.u32(4) || !font_metrics(call, call.u32(0), call.u32(1), call[2], call.u32(4))) {
			call.fail(87);
			return;
		}
		call.ret(1);
	});
	registry.add("SystemParametersInfoW", 4, [this, font_metrics](ApiCall &call) {
		const uint32_t action = call.u32(0), parameter = call.u32(1);
		const uint64_t pointer = call[2];
		const auto &screen = objects.nonclient_metrics();
		if (font_metrics(call, action, parameter, pointer, 96)) {
			call.ret(1);
			return;
		}
		switch (action) {
		case 0x30: {
			const int32_t rect[] = {0, 0, screen.screen_x, screen.screen_y};
			call.write(pointer, std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(rect), 16));
			call.ret(1);
			return;
		}
		default:
			break;
		}
		// Scalar and boolean queries: the answers describe the single classic desktop.
		const auto answer = [&](uint32_t value) {
			if (pointer)
				call.store<uint32_t>(pointer, value);
			call.ret(1);
		};
		switch (action) {
		case 0x68: answer(3); return;
		case 0x6c: answer(1); return;
		case 0x6a: answer(400); return;
		case 0x26: case 0x4a: case 0x1008: case 0x100a: case 0x1012: case 0x1024: answer(1); return;
		case 0x200e: case 0x2010: answer(1); return;
		case 0x1022: case 0x1026: case 0x1042: answer(0); return;
		case 0x0a: case 0x0d: answer(500); return;
		case 0x0b: answer(0); return;
		default:
			call.ret(1);
		}
	});
}

}
