#include "windows.h"
#include "nt/text.h"

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <utility>

namespace raven {

namespace {

struct Rect {
	int32_t left, top, right, bottom;
	bool empty() const { return right <= left || bottom <= top; }
};

Rect read_rect(ApiCall &call, uint64_t address)
{
	const auto bytes = call.read(address, 16);
	Rect rect;
	std::memcpy(&rect, bytes.data(), 16);
	return rect;
}

void write_rect(ApiCall &call, uint64_t address, const Rect &rect)
{
	call.write(address, std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(&rect), 16));
}

std::vector<uint8_t> raw_mouse_bytes(const std::array<int32_t,6> &report,size_t width,uint64_t wparam=0)
{
	const size_t header=width==4 ? 16 : 24;
	std::vector<uint8_t> bytes(header+24);
	const auto put=[&](size_t at,uint64_t value,size_t length) { std::memcpy(bytes.data()+at,&value,length); };
	put(4,bytes.size(),4); put(8,1,width); put(8+width,wparam,width);
	put(header+4,uint16_t(report[2]),2); put(header+6,uint16_t(int16_t(report[3])),2);
	put(header+12,uint32_t(report[0]),4); put(header+16,uint32_t(report[1]),4);
	return bytes;
}

}

void NativeWindows::register_api(ApiRegistry &registry)
{
	register_message_api(registry);
	register_manager_api(registry);
	register_menu_api(registry);
	register_ext_api(registry);
	register_scroll_api(registry);
	register_long_api(registry);
	register_ansi_api(registry);
	register_theme_api(registry);

	// Rectangle helpers are pure arithmetic on guest RECTs.
	registry.add("InflateRect", 3, [](ApiCall &call) {
		auto rect = read_rect(call, call[0]);
		rect.left -= call.i32(1);
		rect.right += call.i32(1);
		rect.top -= call.i32(2);
		rect.bottom += call.i32(2);
		write_rect(call, call[0], rect);
		call.ret(1);
	});
	registry.add("OffsetRect", 3, [](ApiCall &call) {
		auto rect = read_rect(call, call[0]);
		rect.left += call.i32(1);
		rect.right += call.i32(1);
		rect.top += call.i32(2);
		rect.bottom += call.i32(2);
		write_rect(call, call[0], rect);
		call.ret(1);
	});
	registry.add("SetRectEmpty", 1, [](ApiCall &call) {
		write_rect(call, call[0], {0, 0, 0, 0});
		call.ret(1);
	});
	registry.add("CopyRect", 2, [](ApiCall &call) {
		write_rect(call, call[0], read_rect(call, call[1]));
		call.ret(1);
	});
	registry.add("EqualRect", 2, [](ApiCall &call) {
		const auto a = read_rect(call, call[0]), b = read_rect(call, call[1]);
		call.ret(a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom);
	});
	registry.add("IsRectEmpty", 1, [](ApiCall &call) { call.ret(read_rect(call, call[0]).empty()); });
	registry.add("IntersectRect", 3, [](ApiCall &call) {
		const auto a = read_rect(call, call[1]), b = read_rect(call, call[2]);
		Rect result{std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right), std::min(a.bottom, b.bottom)};
		const bool empty = a.empty() || b.empty() || result.empty();
		write_rect(call, call[0], empty ? Rect{0, 0, 0, 0} : result);
		call.ret(!empty);
	});
	registry.add("UnionRect", 3, [](ApiCall &call) {
		const auto a = read_rect(call, call[1]), b = read_rect(call, call[2]);
		Rect result;
		if (a.empty() && b.empty()) result = {0, 0, 0, 0};
		else if (a.empty()) result = b;
		else if (b.empty()) result = a;
		else result = {std::min(a.left, b.left), std::min(a.top, b.top), std::max(a.right, b.right), std::max(a.bottom, b.bottom)};
		write_rect(call, call[0], result);
		call.ret(!(a.empty() && b.empty()));
	});

	// Character classification follows the invariant (en-US) rules of the Windows ANSI/Unicode helpers.
	registry.add("CharUpperW", 1, [](ApiCall &call) {
		if (call[0] <= 0xffff) {
			call.ret(upper_case(char16_t(call[0])));
			return;
		}
		auto text = call.wide(call[0]);
		for (auto &character : text) character = upper_case(character);
		call.put_wide(call[0], text, text.size() + 1);
		call.ret(call[0]);
	});
	registry.add("CharLowerW", 1, [](ApiCall &call) {
		if (call[0] <= 0xffff) {
			call.ret(fold_case(char16_t(call[0])));
			return;
		}
		auto text = call.wide(call[0]);
		for (auto &character : text) character = fold_case(character);
		call.put_wide(call[0], text, text.size() + 1);
		call.ret(call[0]);
	});
	// The Buff forms change that many characters in place, zeros included, and answer how many they processed.
	for (const bool lower : {true, false})
		registry.add(lower ? "CharLowerBuffW" : "CharUpperBuffW", 2, [lower](ApiCall &call) {
			if (!call[0] || !call.u32(1)) {
				call.ret(0);
				return;
			}
			auto bytes = call.read(call[0], size_t(call.u32(1)) * 2);
			for (size_t at = 0; at + 1 < bytes.size(); at += 2) {
				const auto character = char16_t(bytes[at] | bytes[at + 1] << 8);
				const char16_t changed = lower ? fold_case(character) : upper_case(character);
				bytes[at] = uint8_t(changed);
				bytes[at + 1] = uint8_t(changed >> 8);
			}
			call.write(call[0], bytes);
			call.ret(call.u32(1));
		});
	registry.add("IsCharAlphaW", 1, [](ApiCall &call) { call.ret(std::iswalpha(wint_t(call.u32(0) & 0xffff)) != 0); });
	registry.add("IsCharAlphaNumericW", 1, [](ApiCall &call) { call.ret(std::iswalnum(wint_t(call.u32(0) & 0xffff)) != 0); });
	registry.add("IsCharLowerW", 1, [](ApiCall &call) { call.ret(std::iswlower(wint_t(call.u32(0) & 0xffff)) != 0); });
	registry.add("IsCharUpperW", 1, [](ApiCall &call) { call.ret(std::iswupper(wint_t(call.u32(0) & 0xffff)) != 0); });
	registry.add("MessageBeep", 1, [](ApiCall &call) { call.ret(1); });
	registry.add("LockWindowUpdate", 1, [](ApiCall &call) { call.ret(1); });
	registry.add("NotifyWinEvent", 4, [](ApiCall &) {});
	registry.add("FlashWindowEx", 1, [](ApiCall &call) { call.ret(0); });
	registry.add("GetDoubleClickTime", 0, [](ApiCall &call) { call.ret(500); });
	registry.add("GetCaretBlinkTime", 0, [](ApiCall &call) { call.ret(530); });
	registry.add("GetKeyboardLayout", 1, [](ApiCall &call) { call.ret(0x04090409); });
	registry.add("GetMessageExtraInfo", 0, [](ApiCall &call) { call.ret(0); });
	// The one keyboard layout, US English, by its KLID.
	registry.add("GetKeyboardLayoutNameW", 1, [](ApiCall &call) {
		call.put_wide(call[0], u"00000409", 9);
		call.ret(1);
	});
	registry.add("GetKeyboardLayoutNameA", 1, [](ApiCall &call) {
		call.put_narrow(call[0], "00000409", 9);
		call.ret(1);
	});
	// No accessibility server runs, so there are no standard accessible objects to hand out.
	registry.add("CreateStdAccessibleObject", 4, [](ApiCall &call) {
		if (call[3])
			call.store_pointer(call[3], 0);
		call.ret(0x80004001U);
	});
	// When the user last gave input, in GetTickCount's clock; the session counts as just used.
	registry.add("GetLastInputInfo", 1, [&registry](ApiCall &call) {
		if (!call[0] || call.load<uint32_t>(call[0]) != 8) {
			call.fail(87);
			return;
		}
		uint64_t now = 0;
		if (const auto ticks = registry.find("GetTickCount")) {
			ApiCall inner(call.process, call.environment, {});
			ticks->second(inner);
			now = inner.result;
		}
		call.store<uint32_t>(call[0] + 4, uint32_t(now));
		call.ret(1);
	});
	registry.add("GetMessageTime", 0, [](ApiCall &call) {
		call.ret(uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()));
	});
	// The handles a process holds in the window system: windows, device contexts and drawing objects are one table here.
	registry.add("GetGuiResources", 2, [this](ApiCall &call) {
		const auto flags = call.u32(1);
		if (flags > 4 || flags == 3) {
			call.fail(87);
			return;
		}
		call.ret(std::max<size_t>(1, objects.count()));
	});
	// CURSORINFO: always showing, and the shape is not told apart here.
	registry.add("GetCursorInfo", 1, [this](ApiCall &call) {
		const size_t width = call.pointer_width();
		if (!call[0] || call.load<uint32_t>(call[0]) != (width == 4 ? 20 : 24)) {
			call.fail(87, 0);
			return;
		}
		call.store<uint32_t>(call[0] + 4, 1);
		call.store_pointer(call[0] + 8, 0);
		call.store<int32_t>(call[0] + 8 + width, cursor_x);
		call.store<int32_t>(call[0] + 12 + width, cursor_y);
		call.ret(1);
	});
	registry.add("GetCursorPos", 1, [this](ApiCall &call) {
		call.store<int32_t>(call[0], cursor_x);
		call.store<int32_t>(call[0] + 4, cursor_y);
		call.ret(1);
	});
	// Raw input: the mouse (usage page 1, usage 2) is the device that reports; RIDEV_REMOVE ends the registration and
	// RIDEV_NOLEGACY leaves out the ordinary mouse messages. Keyboards keep their ordinary messages.
	registry.add("GetRawInputDeviceList", 3, [](ApiCall &call) {
		const size_t width = call.pointer_width(), size = width == 4 ? 8 : 16;
		if (!call[1] || call.u32(2) != size || (call[0] & 3)) {
			call.fail(87, UINT32_MAX);
			return;
		}
		if (!call[0]) {
			call.store<uint32_t>(call[1], 1);
			call.ret(0);
			return;
		}
		if (!call.load<uint32_t>(call[1])) {
			call.store<uint32_t>(call[1], 1);
			call.fail(122, UINT32_MAX);
			return;
		}
		std::vector<uint8_t> bytes(size);
		bytes[0] = 1;
		call.write(call[0], bytes);
		call.ret(1);
	});
	for (const auto suffix : {std::string("A"), std::string("W")}) {
		registry.add("GetRawInputDeviceInfo" + suffix, 4, [suffix](ApiCall &call) {
			if (call[0] != 1 || !call[3] || (call[2] & 3)) {
				call.fail(call[0] != 1 ? 6 : 87, UINT32_MAX);
				return;
			}
			std::vector<uint8_t> bytes;
			uint32_t required = 0;
			if (call.u32(1) == 0x20000007U) {
				const std::u16string name = u"\\\\?\\RAVEN#POINTER#0";
				required = uint32_t(name.size() + 1);
				bytes.resize(required * (suffix == "W" ? 2 : 1));
				if (suffix == "W")
					std::memcpy(bytes.data(), name.c_str(), bytes.size());
				else
					for (size_t index = 0; index < name.size(); ++index)
						bytes[index] = uint8_t(name[index]);
			} else if (call.u32(1) == 0x2000000bU) {
				const uint32_t info[8] = {32, 0, 1, 3, 0, 1, 0, 0};
				required = sizeof(info);
				bytes.resize(sizeof(info));
				std::memcpy(bytes.data(), info, sizeof(info));
			} else {
				call.fail(call.u32(1) == 0x20000005U ? 50 : 87, UINT32_MAX);
				return;
			}
			if (!call[2]) {
				call.store<uint32_t>(call[3], required);
				call.ret(0);
				return;
			}
			if (call.load<uint32_t>(call[3]) < required) {
				call.store<uint32_t>(call[3], required);
				call.fail(122, UINT32_MAX);
				return;
			}
			if (call.u32(1) == 0x2000000bU && call.load<uint32_t>(call[2]) != required) {
				call.fail(87, UINT32_MAX);
				return;
			}
			call.write(call[2], bytes);
			call.ret(bytes.size());
		});
	}
	registry.add("RegisterRawInputDevices", 3, [this](ApiCall &call) {
		const size_t width = call.pointer_width(), size = width == 4 ? 12 : 16;
		if (!call[0] || !call.u32(1) || call.u32(2) != size) {
			call.fail(87);
			return;
		}
		for (uint32_t index = 0; index < call.u32(1); ++index) {
			const uint64_t entry = call[0] + index * size;
			const uint16_t page = call.load<uint16_t>(entry), usage = call.load<uint16_t>(entry + 2);
			const uint32_t flags = call.load<uint32_t>(entry + 4);
			const uint64_t target = call.load_pointer(entry + 8);
			if (target && !std::as_const(objects).window(target)) {
				call.fail(1400);
				return;
			}
			if (page != 1 || usage != 2)
				continue;
			if (flags & 1) {
				raw_mouse = {false, false, raw_mouse.relative};
				continue;
			}
			raw_mouse.registered = true;
			raw_mouse.no_legacy = (flags & 0x30) == 0x30;
			raw_mouse.target = target;
		}
		call.ret(1);
	});
	// RAWINPUT: the header (type, size, device, wParam) and RAWMOUSE (flags, button flags and data, raw buttons, last X and Y, extra).
	registry.add("GetRawInputData", 5, [this](ApiCall &call) {
		const size_t width = call.pointer_width(), header = width == 4 ? 16 : 24, total = header + 24;
		const uint32_t command = call.u32(1);
		const auto found = raw_reports.find(call[0]);
		if ((command != 0x10000003U && command != 0x10000005U) || call.u32(4) != header || !call[3] || found == raw_reports.end()) {
			call.fail(found == raw_reports.end() ? 6 : 87, UINT32_MAX);
			return;
		}
		const size_t size = command == 0x10000005U ? header : total;
		if (!call[2]) {
			call.store<uint32_t>(call[3], uint32_t(size));
			call.ret(0);
			return;
		}
		if (call.load<uint32_t>(call[3]) < size) {
			call.store<uint32_t>(call[3], uint32_t(size));
			call.fail(122, UINT32_MAX);
			return;
		}
		auto bytes = raw_mouse_bytes(found->second, width);
		bytes.resize(size);
		call.write(call[2], bytes);
		call.ret(size);
	});
	registry.add("GetRawInputBuffer", 3, [this](ApiCall &call) {
		const size_t width=call.pointer_width(), header=width==4 ? 16 : 24;
		if (!call[1] || call.u32(2)!=header || (call[0] & (width==4 ? 7U : 3U))) {
			call.fail(87,UINT32_MAX);
			return;
		}
		try {
			const auto capacity=call[0] ? call.load<uint32_t>(call[1]) : 0;
			const auto me=environment.thread_id();
			if (presentation.poll)
				for (const auto &event : presentation.poll(std::chrono::milliseconds(0))) pending_events.push_back(event);
			for (auto remaining=pending_events.size();remaining;--remaining) {
				const auto event=pending_events.front(); pending_events.pop_front();
				if (event.window && (!std::as_const(objects).window(event.window) || !std::as_const(objects).window(event.window)->visible)) continue;
				const auto thread=owner(event.window ? event.window : focus);
				if (thread && thread!=me) pending_events.push_back(event);
				else receive(event);
			}
			std::vector<uint8_t> bytes;
			std::set<uint64_t> taken;
			for (const auto &message : messages) {
				if (message.message!=0xff || !(message.kinds & 0x400) || (message.thread && message.thread!=me)) continue;
				const auto found=raw_reports.find(message.lparam);
				if (found==raw_reports.end()) continue;
				// WOW64 buffers carry the native 24-byte header, unlike GetRawInputData's 16-byte header.
				const auto record=raw_mouse_bytes(found->second,8,message.wparam);
				if (!call[0] || bytes.size()+record.size()>capacity) {
					if (bytes.empty()) {
						call.store<uint32_t>(call[1],uint32_t(record.size()));
						if (call[0]) call.fail(122,UINT32_MAX); else call.ret(0);
						return;
					}
					break;
				}
				bytes.insert(bytes.end(),record.begin(),record.end());
				taken.insert(message.lparam);
			}
			if (!call[0]) call.store<uint32_t>(call[1],0);
			else if (!bytes.empty()) call.write(call[0],bytes);
			// Copy first: a bad output pointer must leave the reports available for a later read.
			std::erase_if(messages,[&](const Message &message) {
				return message.message==0xff && (message.kinds & 0x400) && (!message.thread || message.thread==me) && taken.contains(message.lparam);
			});
			for (const auto handle : taken) raw_reports.erase(handle);
			call.ret(taken.size());
		} catch (const ApiFault &) { call.fail(998,UINT32_MAX); }
	});
	registry.add("SetCursorPos", 2, [this](ApiCall &call) {
		cursor_x = call.i32(0);
		cursor_y = call.i32(1);
		update_warp_lock(true);
		call.ret(1);
	});
	registry.add("ShowCursor", 1, [this](ApiCall &call) {
		cursor_count += call.u32(0) ? 1 : -1;
		update_warp_lock(false);
		show_cursor();
		call.ret(uint32_t(cursor_count));
	});
	registry.add("SetCursor", 1, [this](ApiCall &call) {
		const auto previous = current_cursor;
		current_cursor = call[0];
		update_warp_lock(false);
		show_cursor();
		call.ret(previous);
	});
	registry.add("DestroyCursor", 1, [](ApiCall &call) { call.ret(1); });
	// Message boxes need the dialog manager; until it exists the text is reported and the first button is chosen.
	const auto message_box = [](ApiCall &call, bool wide) {
		const auto text = wide ? call.wide(call[1], 8192) : from_utf8(call.narrow(call[1], 8192));
		const auto caption = call[2] ? (wide ? call.wide(call[2], 512) : from_utf8(call.narrow(call[2], 512))) : std::u16string(u"Error");
		std::fprintf(stderr, "raven: MessageBox \"%s\": %s\n", to_utf8(caption).c_str(), to_utf8(text).c_str());
		const uint32_t type = call.u32(3) & 0xf;
		// MB_OK/OKCANCEL answer IDOK, ABORTRETRYIGNORE IDABORT, YESNOCANCEL/YESNO IDYES, RETRYCANCEL IDRETRY, CANCELTRYCONTINUE IDCANCEL.
		static constexpr uint32_t answers[] = {1, 1, 3, 6, 6, 4, 2, 0};
		call.ret(type < 7 ? answers[type] : 1);
	};
	registry.add("MessageBoxW", 4, [message_box](ApiCall &call) { message_box(call, true); });
	registry.add("MessageBoxA", 4, [message_box](ApiCall &call) { message_box(call, false); });
	registry.add("GetCapture", 0, [this](ApiCall &call) { call.ret(capture && std::as_const(objects).window(capture) ? capture : 0); });
}

void NativeWindows::register_message_api(ApiRegistry &registry)
{
	registry.add("RegisterWindowMessageA", 1, [this](ApiCall &call) {
		const auto narrow = call.narrow(call[0], 255);
		const std::u16string name(narrow.begin(), narrow.end());
		if (name.empty()) {
			call.fail(87);
			return;
		}
		auto found = registered_messages.find(name);
		if (found == registered_messages.end()) {
			if (registered_messages.size() >= 0x3fff) {
				call.fail(8);
				return;
			}
			found = registered_messages.emplace(name, uint32_t(0xc000 + registered_messages.size())).first;
		}
		call.ret(found->second);
	});
	registry.add("RegisterWindowMessageW", 1, [this](ApiCall &call) {
		const auto name = call.wide(call[0], 255);
		if (name.empty()) {
			call.fail(87);
			return;
		}
		// Window message names compare case-sensitively, unlike class names.
		auto found = registered_messages.find(name);
		if (found == registered_messages.end()) {
			if (registered_messages.size() >= 0x3fff) {
				call.fail(8);
				return;
			}
			found = registered_messages.emplace(name, uint32_t(0xc000 + registered_messages.size())).first;
		}
		call.ret(found->second);
	});
}

}
