#include "windows.h"
#include "nonclient.h"

#include <algorithm>
#include <cstring>

namespace raven {

namespace {

constexpr uint32_t ws_visible = 0x10000000U;

uint64_t extra_access(std::vector<uint8_t> &bytes, int32_t index, size_t width, bool write, uint64_t value)
{
	if (index < 0 || size_t(index) > bytes.size() || width > bytes.size() - size_t(index))
		throw uint32_t(1413);
	uint64_t previous = 0;
	std::memcpy(&previous, bytes.data() + index, width);
	if (write)
		std::memcpy(bytes.data() + index, &value, width);
	return previous;
}

}

uint32_t system_metric(const DesktopMetrics &screen, int32_t index)
{
	switch (index) {
	case 0: return uint32_t(screen.screen_x);
	case 1: return uint32_t(screen.screen_y);
	case 2: case 3: case 9: case 10: case 20: case 21: return 17;
	case 4: return uint32_t(screen.caption);
	case 5: case 6: return uint32_t(screen.border_x);
	case 7: return uint32_t(screen.dialog_x);
	case 8: return uint32_t(screen.dialog_y);
	case 11: case 12: case 13: case 14: return 32;
	case 15: return uint32_t(screen.menu);
	case 16: return uint32_t(screen.screen_x);
	case 17: return uint32_t(screen.screen_y - screen.caption);
	case 19: return 1;
	case 22: case 23: case 41: case 44: case 63: case 67: case 70: case 73: case 74: case 82: case 86: case 87: case 88: case 89: return 0;
	case 28: return 136;
	case 29: return 39;
	case 30: return uint32_t(screen.button_x);
	case 31: return uint32_t(screen.button_y);
	case 32: return uint32_t(screen.frame_x);
	case 33: return uint32_t(screen.frame_y);
	case 34: return 136;
	case 35: return 39;
	case 36: case 37: case 68: case 69: return 4;
	case 38: case 39: return 75;
	case 40: return 0;
	case 42: return 0;
	case 43: return 3;
	case 45: case 46: return 2;
	case 47: case 48: return 160;
	case 49: case 50: return 16;
	case 51: return uint32_t(screen.small_caption);
	case 52: return uint32_t(screen.small_button_x);
	case 53: return uint32_t(screen.small_button_y);
	case 54: return uint32_t(screen.small_button_x);
	case 55: return uint32_t(screen.menu);
	case 56: return 8;
	case 57: return 160;
	case 58: return 24;
	case 59: case 61: return uint32_t(screen.screen_x + 8);
	case 60: case 62: return uint32_t(screen.screen_y + 8);
	case 71: case 72: return 15;
	case 75: case 91: return 1;
	case 76: case 77: return 0;
	case 78: return uint32_t(screen.screen_x);
	case 79: return uint32_t(screen.screen_y);
	case 80: case 81: return 1;
	case 83: case 84: return 1;
	case 92: return 0;
	case 97: return 0;
	case 0x1000: case 0x2000: case 0x2001: return 0;
	default: return 0;
	}
}

// Window and class data access: every documented index, for both widths and both character sets.
void NativeWindows::register_long_api(ApiRegistry &registry)
{
	const auto window_long = [this](ApiCall &call, bool write, bool wide) {
		auto *window = objects.window(call[0]);
		if (!window) {
			call.fail(1400);
			return;
		}
		const int32_t index = call.i32(1);
		const uint64_t value = write ? (wide ? call[2] : uint64_t(uint32_t(call[2]))) : 0;
		const auto narrow = [&](uint64_t result) { return wide ? result : uint64_t(uint32_t(result)); };
		try {
			if (index >= 0) {
				call.ret(narrow(extra_access(window->extra, index, wide ? 8 : 4, write, value)));
				return;
			}
			// Pointer-sized values have no 32-bit accessor on a 64-bit system; on a 32-bit one they are 32 bits wide.
			if (!wide && process.mode() != NativeMode::x86 && (index == -4 || index == -6 || index == -8)) {
				call.fail(1413);
				return;
			}
			switch (index) {
			case -21: {
				const auto previous = window->userdata;
				if (write)
					window->userdata = wide ? value : uint64_t(int64_t(int32_t(uint32_t(value))));
				call.ret(narrow(previous));
				return;
			}
			case -4: {
				const auto previous = window->procedure;
				if (write) {
					if (!value) {
						call.fail(87);
						return;
					}
					window->procedure = value;
				}
				call.ret(narrow(previous));
				return;
			}
			case -6: {
				const auto previous = window->instance;
				if (write)
					window->instance = value;
				call.ret(narrow(previous));
				return;
			}
			case -12: {
				const auto previous = window->child_id;
				if (write)
					window->child_id = value;
				call.ret(narrow(previous));
				return;
			}
			case -8: {
				const auto previous = window->parent ? window->parent : window->owner;
				if (write) {
					if (window->parent) {
						const uint64_t arguments[] = {call[0], value};
						(void)arguments;
						window->parent = value;
					} else {
						window->owner = value;
					}
				}
				call.ret(narrow(previous));
				return;
			}
			case -16:
			case -20: {
				const bool style = index == -16;
				// The index travels in the message as a signed word of the program's own width.
				const uint64_t index_word = process.mode() == NativeMode::x86 ? uint64_t(uint32_t(index)) : uint64_t(int64_t(index));
				const uint32_t previous = style ? window->style : window->ex_style;
				if (write) {
					uint32_t next = uint32_t(value);
					// The window may adjust the change before it applies.
					const auto scratch = scratch_allocate(8);
					std::vector<uint8_t> bytes(8);
					std::memcpy(bytes.data(), &previous, 4);
					std::memcpy(bytes.data() + 4, &next, 4);
					process.write_memory(scratch, bytes);
					send(call[0], 0x7c, index_word, scratch);
					if (!objects.window(call[0]))
						return;
					const auto changed = process.read_memory(scratch, 8);
					std::memcpy(&next, changed.data() + 4, 4);
					window = objects.window(call[0]);
					if (style) {
						window->style = next;
						const bool visible = (next & ws_visible) != 0;
						if (visible != window->visible)
							window->visible = visible;
					} else {
						window->ex_style = next;
					}
					std::memcpy(bytes.data() + 4, &next, 4);
					process.write_memory(scratch, bytes);
					send(call[0], 0x7d, index_word, scratch);
					if (objects.window(call[0]) && objects.window_visible(call[0])) {
						invalidate_tree(call[0], true);
						drawing(call[0]);
					}
				}
				call.ret(previous);
				return;
			}
			default:
				call.fail(1413);
			}
		} catch (const uint32_t &error) {
			call.fail(error);
		}
	};
	for (const auto *name : {"GetWindowLongW", "GetWindowLongA"})
		registry.add(name, 2, [=](ApiCall &call) { window_long(call, false, false); });
	for (const auto *name : {"GetWindowLongPtrW", "GetWindowLongPtrA"})
		registry.add(name, 2, [=](ApiCall &call) { window_long(call, false, true); });
	for (const auto *name : {"SetWindowLongW", "SetWindowLongA"})
		registry.add(name, 3, [=](ApiCall &call) { window_long(call, true, false); });
	for (const auto *name : {"SetWindowLongPtrW", "SetWindowLongPtrA"})
		registry.add(name, 3, [=](ApiCall &call) { window_long(call, true, true); });

	const auto class_long = [this](ApiCall &call, bool write, bool wide) {
		const auto *window = objects.window(call[0]);
		if (!window) {
			call.fail(1400);
			return;
		}
		auto *definition = objects.window_class(window->class_id);
		const int32_t index = call.i32(1);
		const uint64_t value = write ? (wide ? call[2] : uint64_t(uint32_t(call[2]))) : 0;
		const auto narrow = [&](uint64_t result) { return wide ? result : uint64_t(uint32_t(result)); };
		try {
			if (index >= 0) {
				call.ret(narrow(extra_access(definition->extra, index, wide ? 8 : 4, write, value)));
				return;
			}
			const auto exchange = [&](uint64_t &field) {
				const auto previous = field;
				if (write)
					field = value;
				call.ret(narrow(previous));
			};
			switch (index) {
			case -32: call.ret(definition->atom); return;
			case -26: {
				uint64_t style = definition->style;
				const auto previous = style;
				if (write)
					definition->style = uint32_t(value);
				call.ret(narrow(previous));
				return;
			}
			case -20: call.ret(definition->extra.size()); return;
			case -18: {
				const auto previous = definition->window_extra;
				if (write)
					definition->window_extra = uint32_t(value);
				call.ret(previous);
				return;
			}
			case -16: exchange(definition->instance); return;
			case -24: exchange(definition->procedure); return;
			case -10: exchange(definition->background); return;
			case -12: exchange(definition->cursor); return;
			case -14: exchange(definition->icon); return;
			case -34: {
				uint64_t small = 0;
				exchange(small);
				return;
			}
			case -8: call.ret(0); return;
			default: call.fail(1413);
			}
		} catch (const uint32_t &error) {
			call.fail(error);
		}
	};
	for (const auto *name : {"GetClassLongW", "GetClassLongA"})
		registry.add(name, 2, [=](ApiCall &call) { class_long(call, false, false); });
	for (const auto *name : {"GetClassLongPtrW", "GetClassLongPtrA"})
		registry.add(name, 2, [=](ApiCall &call) { class_long(call, false, true); });
	for (const auto *name : {"SetClassLongW", "SetClassLongA"})
		registry.add(name, 3, [=](ApiCall &call) { class_long(call, true, false); });
	for (const auto *name : {"SetClassLongPtrW", "SetClassLongPtrA"})
		registry.add(name, 3, [=](ApiCall &call) { class_long(call, true, true); });
	registry.add("CallWindowProcW", 5, [this](ApiCall &call) {
		if (!call[0]) {
			call.fail(87);
			return;
		}
		const uint64_t arguments[] = {call[1], call[2], call[3], call[4]};
		const auto result = invoke(call[0], arguments);
		if (result.stop != NativeStop::returned)
			throw result;
		call.ret(result.value);
	});

	// System metrics: the classic desktop values; the frame values come from the nonclient metrics in use.
	// The frame a style adds around a client rectangle, at a given DPI: the frame of 96 DPI scaled by the ratio.
	registry.add("AdjustWindowRectExForDpi", 5, [this](ApiCall &call) {
		const uint32_t dpi = call.u32(4);
		if (!call[0] || !dpi) {
			call.fail(87);
			return;
		}
		const auto bytes = call.read(call[0], 16);
		int32_t value[4];
		std::memcpy(value, bytes.data(), 16);
		const auto frame = adjust_window_rect(objects.nonclient_metrics(), {0, 0, 0, 0}, call.u32(1), call.u32(2) != 0, call.u32(3));
		const auto scale = [dpi](int32_t extent) { return int32_t((int64_t(extent) * dpi + (extent < 0 ? -48 : 48)) / 96); };
		const int32_t adjusted[4] = {value[0] + scale(frame.left), value[1] + scale(frame.top), value[2] + scale(frame.right), value[3] + scale(frame.bottom)};
		call.write(call[0], std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(adjusted), 16));
		call.ret(1);
	});
	// The clip rectangle: the whole screen unless the program confined the cursor.
	registry.add("GetClipCursor", 1, [this](ApiCall &call) {
		if (!call[0]) {
			call.fail(87);
			return;
		}
		const auto &screen = objects.nonclient_metrics();
		const std::array<int32_t, 4> whole{0, 0, int32_t(screen.screen_x), int32_t(screen.screen_y)};
		const auto rect = clip_rect.value_or(whole);
		call.write(call[0], std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(rect.data()), 16));
		call.ret(1);
	});
	// Confining the cursor keeps the host pointer in the part of the program's window the rectangle covers (holding it still when the
	// rectangle is a point); the whole screen or no rectangle lets it go.
	registry.add("ClipCursor", 1, [this](ApiCall &call) {
		const auto &screen = objects.nonclient_metrics();
		std::optional<std::array<int32_t, 4>> rect;
		if (call[0]) {
			const auto bytes = call.read(call[0], 16);
			std::array<int32_t, 4> value{};
			std::memcpy(value.data(), bytes.data(), 16);
			value[0] = std::max(value[0], 0);
			value[1] = std::max(value[1], 0);
			value[2] = std::min(value[2], int32_t(screen.screen_x));
			value[3] = std::min(value[3], int32_t(screen.screen_y));
			if (value[2] < value[0] || value[3] < value[1]) {
				call.fail(87);
				return;
			}
			if (value[0] > 0 || value[1] > 0 || value[2] < int32_t(screen.screen_x) || value[3] < int32_t(screen.screen_y))
				rect = value;
		}
		uint64_t window = 0;
		if (rect) {
			// The window that holds the rectangle: the active one when it does, otherwise the first visible top-level window that does.
			const int32_t cx = ((*rect)[0] + (*rect)[2]) / 2, cy = ((*rect)[1] + (*rect)[3]) / 2;
			const auto holds = [&](uint64_t handle) {
				const auto *candidate = objects.window(handle);
				return candidate && !candidate->parent && objects.window_visible(handle) && cx >= candidate->outer.left &&
					cx < candidate->outer.right && cy >= candidate->outer.top && cy < candidate->outer.bottom;
			};
			if (active && holds(active))
				window = active;
			else
				for (const auto handle : objects.window_handles())
					if (holds(handle)) {
						window = handle;
						break;
					}
		}
		clip_rect = rect;
		if (presentation.confine) {
			if (clip_window && clip_window != window)
				presentation.confine(clip_window, std::nullopt);
			clip_window = window;
			if (window) {
				const auto *target = objects.window(window);
				presentation.confine(window, std::array<int32_t, 4>{(*rect)[0] - target->outer.left, (*rect)[1] - target->outer.top,
					(*rect)[2] - target->outer.left, (*rect)[3] - target->outer.top});
			}
		}
		call.ret(1);
	});
	// The feedback shown for pen and touch, and which gestures a window wants: input arrives as mouse input, so the settings are accepted.
	registry.add("SetWindowFeedbackSetting", 5, [](ApiCall &call) { call.ret(1); });
	registry.add("SetGestureConfig", 5, [](ApiCall &call) { call.ret(1); });
	// Touch input reaches windows as mouse input here; registering for it changes nothing and succeeds.
	registry.add("RegisterTouchWindow", 2, [this](ApiCall &call) {
		if (!objects.window(call[0])) {
			call.fail(1400);
			return;
		}
		call.ret(1);
	});
	registry.add("UnregisterTouchWindow", 1, [this](ApiCall &call) {
		if (!objects.window(call[0])) {
			call.fail(1400);
			return;
		}
		call.ret(1);
	});
	registry.add("GetSystemMetrics", 1, [this](ApiCall &call) { call.ret(system_metric(objects.nonclient_metrics(), call.i32(0))); });
	// The metrics at another DPI: sizes of interface parts scale from the 96 dpi screen, counts, flags and screen sizes do not.
	registry.add("GetSystemMetricsForDpi", 2, [this](ApiCall &call) {
		const int32_t index = call.i32(0);
		const uint32_t dpi = call.u32(1);
		if (!dpi) {
			call.fail(87);
			return;
		}
		constexpr int32_t sizes[] = {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 18, 20, 21, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 45, 46, 47,
			48, 49, 50, 51, 52, 53, 54, 55, 57, 58, 59, 60, 61, 62, 68, 69, 71, 72, 83, 84, 92};
		const auto value = int32_t(system_metric(objects.nonclient_metrics(), index));
		const bool size = std::find(std::begin(sizes), std::end(sizes), index) != std::end(sizes);
		call.ret(uint32_t(size && dpi != 96 ? int32_t((int64_t(value) * dpi + 48) / 96) : value));
	});
}

}
