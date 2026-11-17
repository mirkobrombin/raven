// The console functions of kernel32 over the host terminal: the standard handles that are a terminal, and CONIN$ and CONOUT$, are the
// console. Modes become terminal settings, colours and cursor moves become escape sequences, and the terminal's own line editing serves
// line input. Handles that are files or pipes are no console, so the functions fail on them with ERROR_INVALID_HANDLE as on Windows.

#include "console.h"

#include "child_process.h"
#include "file_objects.h"
#include "kernel_context.h"
#include "nls.h"
#include "text.h"
#include "warm_host.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>

namespace raven {

namespace {

struct ConsoleHandle {
	int descriptor;
	bool input;
};

// The standard handle a process received, read from its process parameters as GetStdHandle reads it.
uint64_t standard_handle(ApiCall &call, size_t index)
{
	const size_t width = call.pointer_width();
	const uint64_t parameters = call.load_pointer(call.environment.peb() + (width == 4 ? 0x10 : 0x20));
	return parameters ? call.load_pointer(parameters + (width == 4 ? 0x18 : 0x20) + index * width) : 0;
}

// The terminal behind a handle and whether it is the console's input or a screen buffer; nullopt for anything that is no console.
std::optional<ConsoleHandle> console_handle(KernelContext &context, ApiCall &call, uint64_t handle)
{
	int descriptor = -1;
	std::optional<bool> input;
	if (const auto file = context.objects.object_as<FileObject>(handle)) {
		descriptor = file->descriptor;
		if (file->dos_path == u"CONIN$")
			input = true;
		else if (file->dos_path == u"CONOUT$")
			input = false;
	} else
		descriptor = context.objects.descriptor(handle);
	if (descriptor < 0 || !::isatty(descriptor))
		return std::nullopt;
	if (!input) {
		const int access = ::fcntl(descriptor, F_GETFL) & O_ACCMODE;
		// A terminal open both ways is the input when it is the standard input, and a screen buffer otherwise.
		input = access == O_RDONLY || (access == O_RDWR && handle == standard_handle(call, 0));
	}
	return ConsoleHandle{descriptor, *input};
}

bool known_code_page(uint32_t page)
{
	return page == 437 || page == 850 || page == 1252 || page == 65001 || native_single_byte_page(page) != nullptr;
}

std::u16string decode(std::string_view bytes, uint32_t page) { return page == 65001 ? from_utf8(bytes) : from_ansi(bytes, page); }
std::string encode(std::u16string_view text, uint32_t page) { return page == 65001 ? to_utf8(text) : to_ansi(text, page); }

// Console control handlers (SetConsoleCtrlHandler) and the Ctrl+C presses that wait for them. The terminal signals a press as SIGINT;
// the scheduler then calls the handlers, newest first, each on a thread of its own, until one returns TRUE. When none does, the process
// ends with STATUS_CONTROL_C_EXIT, as the default handler of Windows ends it.
struct ControlHandlers {
	std::vector<uint64_t> routines;
	bool ignore = false;
	std::atomic<int> presses{0};
	NativeThreads *threads = nullptr;
	std::vector<uint64_t> asking;
	std::shared_ptr<GuestThread> running;
};

ControlHandlers *control_handlers = nullptr;

void interrupted(int)
{
	if (!control_handlers)
		return;
	// A second press before the first was served: the program cannot take it, so the process ends as an unhandled one does.
	if (control_handlers->presses.fetch_add(1) > 0) {
		TerminalConsole::instance().restore_from_signal();
		::signal(SIGINT, SIG_DFL);
		::raise(SIGINT);
		return;
	}
	control_handlers->threads->nudge();
}

constexpr uint32_t status_control_c_exit = 0xc000013a;

// Reads console input for ReadConsole and ReadFile: a line in line mode, else the characters of the keys typed, waiting until there is
// at least one. Other guest threads run meanwhile.
std::u16string read_input(KernelContext &context, int descriptor, size_t limit)
{
	auto &console = TerminalConsole::instance();
	HostWait waiting;
	if (console.input_mode(descriptor) & console_line_input) {
		context.threads.block([&] { return console.line_ready(descriptor); }, std::nullopt);
		return console.take_line(limit);
	}
	for (;;) {
		context.threads.block([&] { return console.keys_ready(descriptor, false) || console.ended(); }, std::nullopt);
		auto characters = console.take_characters(limit);
		if (!characters.empty() || console.ended())
			return characters;
	}
}

// KEY_EVENT records: the event type and padding, then the key's fields.
std::vector<uint8_t> input_records(const std::vector<ConsoleKey> &keys, bool wide, uint32_t page)
{
	std::vector<uint8_t> records(keys.size() * 20);
	for (size_t index = 0; index < keys.size(); ++index) {
		const auto &key = keys[index];
		uint8_t *record = records.data() + index * 20;
		std::memcpy(record, &key.event, 2);
		if (key.event == 4) {
			// WINDOW_BUFFER_SIZE_RECORD: the new size as a COORD.
			std::memcpy(record + 4, &key.key, 2);
			std::memcpy(record + 6, &key.scan, 2);
			continue;
		}
		const uint32_t down = key.down;
		uint16_t character = key.character;
		if (!wide && character) {
			const auto narrow = encode(std::u16string_view(&key.character, 1), page);
			character = narrow.size() == 1 ? uint8_t(narrow[0]) : 0;
		}
		std::memcpy(record + 4, &down, 4);
		std::memcpy(record + 8, &key.repeat, 2);
		std::memcpy(record + 10, &key.key, 2);
		std::memcpy(record + 12, &key.scan, 2);
		std::memcpy(record + 14, &character, 2);
		std::memcpy(record + 16, &key.controls, 4);
	}
	return records;
}

void put_coordinate(std::vector<uint8_t> &bytes, size_t at, int16_t x, int16_t y)
{
	std::memcpy(bytes.data() + at, &x, 2);
	std::memcpy(bytes.data() + at + 2, &y, 2);
}

}

bool read_console_file(KernelContext &context, ApiCall &call)
{
	const auto console = console_handle(context, call, call[0]);
	if (!console || !console->input || call[4])
		return false;
	const auto page = TerminalConsole::instance().input_code_page;
	// Characters are asked for one per byte the buffer holds; a multibyte code page may need fewer.
	const auto bytes = encode(read_input(context, console->descriptor, std::max<uint32_t>(call.u32(2), 1)), page);
	const size_t count = std::min<size_t>(bytes.size(), call.u32(2));
	call.write(call[1], std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(bytes.data()), count));
	if (call[3])
		call.store<uint32_t>(call[3], uint32_t(count));
	call.ret(1);
	return true;
}

bool open_console_device(KernelContext &context, ApiCall &call, std::u16string_view name, uint32_t access, bool inheritable)
{
	auto device = name;
	if (device.starts_with(u"\\\\.\\"))
		device.remove_prefix(4);
	const bool conin = equal_fold(device, u"CONIN$"), conout = equal_fold(device, u"CONOUT$");
	if (!conin && !conout && !equal_fold(device, u"CON"))
		return false;
	const int descriptor = open_console_terminal();
	if (descriptor < 0)
		return false;
	// CON is the input when it is opened only for reading, and the screen buffer otherwise.
	const bool input = conin || (!conout && !(access & (0x40000000U | 0x10000000U | 0x2U)));
	const auto made = context.objects.add_object(std::make_shared<FileObject>(descriptor, access, false, "/dev/tty",
		input ? u"CONIN$" : u"CONOUT$", 0));
	if (made.status) {
		call.fail(4, uint64_t(-1));
		return true;
	}
	if (inheritable)
		context.objects.set_inherit(made.value, true);
	call.ret(made.value);
	return true;
}

void register_console_api(ApiRegistry &registry, KernelContext &context)
{
	auto &console = TerminalConsole::instance();
	if (console.title().empty())
		console.set_title(-1, context.launch.image_path);
	const auto input_of = [&context](ApiCall &call) -> std::optional<ConsoleHandle> {
		const auto found = console_handle(context, call, call[0]);
		if (!found || !found->input) {
			call.fail(6);
			return std::nullopt;
		}
		return found;
	};
	const auto output_of = [&context](ApiCall &call) -> std::optional<ConsoleHandle> {
		const auto found = console_handle(context, call, call[0]);
		if (!found || found->input) {
			call.fail(6);
			return std::nullopt;
		}
		return found;
	};

	registry.add("GetConsoleMode", 2, [&context, &console](ApiCall &call) {
		const auto found = console_handle(context, call, call[0]);
		if (!found) {
			call.fail(6);
			return;
		}
		call.store<uint32_t>(call[1], found->input ? console.input_mode(found->descriptor) : console.output_mode());
		call.ret(1);
	});
	registry.add("SetConsoleMode", 2, [&context, &console](ApiCall &call) {
		const auto found = console_handle(context, call, call[0]);
		if (!found) {
			call.fail(6);
			return;
		}
		if (found->input ? !console.set_input_mode(found->descriptor, call.u32(1)) : bool(call.u32(1) & ~console_output_modes)) {
			call.fail(87);
			return;
		}
		if (!found->input)
			console.set_output_mode(found->descriptor, call.u32(1));
		call.ret(1);
	});

	const auto write_console = [output_of, &console](ApiCall &call, bool wide) {
		const auto found = output_of(call);
		if (!found)
			return;
		const uint32_t count = call.u32(2);
		const auto bytes = call.read(call[1], size_t(count) * (wide ? 2 : 1));
		std::string text;
		if (wide) {
			std::u16string characters(count, u'\0');
			std::memcpy(characters.data(), bytes.data(), bytes.size());
			text = to_utf8(characters);
		} else {
			const std::string_view narrow(reinterpret_cast<const char *>(bytes.data()), bytes.size());
			text = console.output_code_page == 65001 ? std::string(narrow) : to_utf8(decode(narrow, console.output_code_page));
		}
		if (!console.write(found->descriptor, text)) {
			call.fail(232);
			return;
		}
		if (call[3])
			call.store<uint32_t>(call[3], count);
		call.ret(1);
	};
	registry.add("WriteConsoleW", 5, [write_console](ApiCall &call) { write_console(call, true); });
	registry.add("WriteConsoleA", 5, [write_console](ApiCall &call) { write_console(call, false); });

	const auto read_console = [input_of, &context, &console](ApiCall &call, bool wide) {
		const auto found = input_of(call);
		if (!found)
			return;
		const uint32_t count = call.u32(2);
		std::u16string text;
		if (count)
			text = read_input(context, found->descriptor, count);
		size_t stored;
		if (wide) {
			stored = std::min<size_t>(text.size(), count);
			call.write(call[1], std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(text.data()), stored * 2));
		} else {
			const auto narrow = encode(text, console.input_code_page);
			stored = std::min<size_t>(narrow.size(), count);
			call.write(call[1], std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(narrow.data()), stored));
		}
		if (call[3])
			call.store<uint32_t>(call[3], uint32_t(stored));
		// CONSOLE_READCONSOLE_CONTROL: no control key ended the read.
		if (wide && call[4] && call.load<uint32_t>(call[4]) >= 16)
			call.store<uint32_t>(call[4] + 12, 0);
		call.ret(1);
	};
	registry.add("ReadConsoleW", 5, [read_console](ApiCall &call) { read_console(call, true); });
	registry.add("ReadConsoleA", 5, [read_console](ApiCall &call) { read_console(call, false); });

	// Input records: ReadConsoleInput waits for at least one, Peek and the CONSOLE_READ_NOWAIT flag (2) do not; Peek leaves them queued.
	const auto read_records = [input_of, &context, &console](ApiCall &call, bool wide, bool wait, bool remove) {
		const auto found = input_of(call);
		if (!found)
			return;
		const uint32_t length = call.u32(2);
		if (wait && length) {
			HostWait waiting;
			context.threads.block([&] { return console.keys_ready(found->descriptor, false) || console.ended(); }, std::nullopt);
		} else
			console.keys_ready(found->descriptor, false);
		const auto keys = console.peek_keys(length, remove);
		call.write(call[1], input_records(keys, wide, console.input_code_page));
		call.store<uint32_t>(call[3], uint32_t(keys.size()));
		call.ret(1);
	};
	registry.add("ReadConsoleInputW", 4, [read_records](ApiCall &call) { read_records(call, true, true, true); });
	registry.add("ReadConsoleInputA", 4, [read_records](ApiCall &call) { read_records(call, false, true, true); });
	registry.add("ReadConsoleInputExW", 5, [read_records](ApiCall &call) { read_records(call, true, !(call.u32(4) & 2), !(call.u32(4) & 1)); });
	registry.add("ReadConsoleInputExA", 5, [read_records](ApiCall &call) { read_records(call, false, !(call.u32(4) & 2), !(call.u32(4) & 1)); });
	registry.add("PeekConsoleInputW", 4, [read_records](ApiCall &call) { read_records(call, true, false, false); });
	registry.add("PeekConsoleInputA", 4, [read_records](ApiCall &call) { read_records(call, false, false, false); });
	registry.add("GetNumberOfConsoleInputEvents", 2, [input_of, &console](ApiCall &call) {
		const auto found = input_of(call);
		if (!found)
			return;
		console.keys_ready(found->descriptor, false);
		call.store<uint32_t>(call[1], uint32_t(console.key_count()));
		call.ret(1);
	});
	registry.add("FlushConsoleInputBuffer", 1, [input_of, &console](ApiCall &call) {
		const auto found = input_of(call);
		if (!found)
			return;
		console.flush_input(found->descriptor);
		call.ret(1);
	});

	// The screen buffer is the terminal's window: its size is the window's, and the cursor is where the terminal says it is.
	const auto screen_info = [&console](int descriptor, size_t size) {
		std::vector<uint8_t> info(size);
		// While a child program shares the terminal, what it wrote moved the cursor without this process seeing it.
		if (children_running())
			console.mark_stale();
		const auto cursor = console.cursor(descriptor);
		const auto window = console.size(descriptor);
		const uint16_t attribute = console.attribute();
		const size_t base = size == 22 ? 0 : 4;
		put_coordinate(info, base, window.columns, window.rows);
		put_coordinate(info, base + 4, std::min<int16_t>(cursor.first, int16_t(window.columns - 1)),
			std::min<int16_t>(cursor.second, int16_t(window.rows - 1)));
		std::memcpy(info.data() + base + 8, &attribute, 2);
		put_coordinate(info, base + 10, 0, 0);
		put_coordinate(info, base + 14, int16_t(window.columns - 1), int16_t(window.rows - 1));
		put_coordinate(info, base + 18, window.columns, window.rows);
		return info;
	};
	registry.add("GetConsoleScreenBufferInfo", 2, [output_of, screen_info](ApiCall &call) {
		const auto found = output_of(call);
		if (!found)
			return;
		call.write(call[1], screen_info(found->descriptor, 22));
		call.ret(1);
	});
	registry.add("GetConsoleScreenBufferInfoEx", 2, [output_of, screen_info](ApiCall &call) {
		const auto found = output_of(call);
		if (!found)
			return;
		if (call.load<uint32_t>(call[1]) != 96) {
			call.fail(87);
			return;
		}
		auto info = screen_info(found->descriptor, 96);
		const uint32_t size = 96;
		std::memcpy(info.data(), &size, 4);
		// The popup colours, then the sixteen colours of the console's standard palette as COLORREF values.
		const uint16_t popup = 0xf5;
		std::memcpy(info.data() + 26, &popup, 2);
		constexpr uint32_t palette[16] = {0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080, 0x008080, 0xc0c0c0,
			0x808080, 0xff0000, 0x00ff00, 0xffff00, 0x0000ff, 0xff00ff, 0x00ffff, 0xffffff};
		std::memcpy(info.data() + 32, palette, sizeof palette);
		call.write(call[1], info);
		call.ret(1);
	});
	registry.add("GetLargestConsoleWindowSize", 1, [output_of, &console](ApiCall &call) {
		const auto found = output_of(call);
		if (!found)
			return;
		const auto window = console.size(found->descriptor);
		call.ret(uint32_t(uint16_t(window.columns)) | uint32_t(uint16_t(window.rows)) << 16);
	});
	registry.add("SetConsoleTextAttribute", 2, [output_of, &console](ApiCall &call) {
		const auto found = output_of(call);
		if (!found)
			return;
		call.ret(console.set_attribute(found->descriptor, uint16_t(call.u32(1))));
	});
	// A COORD argument arrives as one word: the column in the low half, the row in the high half.
	const auto coordinate = [](uint32_t word) { return std::pair{int16_t(word & 0xffff), int16_t(word >> 16)}; };
	const auto inside = [&console](int descriptor, std::pair<int16_t, int16_t> cell) {
		const auto window = console.size(descriptor);
		return cell.first >= 0 && cell.second >= 0 && cell.first < window.columns && cell.second < window.rows;
	};
	registry.add("SetConsoleCursorPosition", 2, [output_of, coordinate, inside, &console](ApiCall &call) {
		const auto found = output_of(call);
		if (!found)
			return;
		const auto cell = coordinate(call.u32(1));
		if (!inside(found->descriptor, cell)) {
			call.fail(87);
			return;
		}
		call.ret(console.move_cursor(found->descriptor, cell.first, cell.second));
	});
	registry.add("GetConsoleCursorInfo", 2, [output_of, &console](ApiCall &call) {
		const auto found = output_of(call);
		if (!found)
			return;
		call.store<uint32_t>(call[1], console.cursor_size());
		call.store<uint32_t>(call[1] + 4, console.cursor_visible());
		call.ret(1);
	});
	registry.add("SetConsoleCursorInfo", 2, [output_of, &console](ApiCall &call) {
		const auto found = output_of(call);
		if (!found)
			return;
		if (!console.set_cursor_info(found->descriptor, call.load<uint32_t>(call[1]), call.load<uint32_t>(call[1] + 4))) {
			call.fail(87);
			return;
		}
		call.ret(1);
	});
	const auto fill_characters = [output_of, coordinate, inside, &console](ApiCall &call, bool wide) {
		const auto found = output_of(call);
		if (!found)
			return;
		const auto cell = coordinate(call.u32(3));
		if (!inside(found->descriptor, cell)) {
			call.fail(87);
			return;
		}
		char16_t character = char16_t(call.u32(1));
		if (!wide) {
			const char narrow = char(call.u32(1));
			const auto decoded = decode(std::string_view(&narrow, 1), console.output_code_page);
			character = decoded.empty() ? u'?' : decoded[0];
		}
		const auto window = console.size(found->descriptor);
		const uint32_t room = uint32_t(window.columns) * uint32_t(window.rows) - uint32_t(cell.second * window.columns + cell.first);
		const uint32_t count = std::min(call.u32(2), room);
		if (!console.fill_characters(found->descriptor, character, count, cell.first, cell.second)) {
			call.fail(232);
			return;
		}
		call.store<uint32_t>(call[4], count);
		call.ret(1);
	};
	registry.add("FillConsoleOutputCharacterW", 5, [fill_characters](ApiCall &call) { fill_characters(call, true); });
	registry.add("FillConsoleOutputCharacterA", 5, [fill_characters](ApiCall &call) { fill_characters(call, false); });
	const auto write_characters = [output_of, coordinate, inside, &console](ApiCall &call, bool wide) {
		const auto found = output_of(call);
		if (!found)
			return;
		const auto cell = coordinate(call.u32(3));
		if (!inside(found->descriptor, cell)) {
			call.fail(87);
			return;
		}
		const auto window = console.size(found->descriptor);
		const uint32_t room = uint32_t(window.columns) * uint32_t(window.rows) - uint32_t(cell.second * window.columns + cell.first);
		const uint32_t count = std::min(call.u32(2), room);
		const auto bytes = call.read(call[1], size_t(count) * (wide ? 2 : 1));
		std::u16string text;
		if (wide) {
			text.resize(count);
			std::memcpy(text.data(), bytes.data(), bytes.size());
		} else
			text = decode(std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()), console.output_code_page);
		if (!console.write_characters(found->descriptor, text, cell.first, cell.second)) {
			call.fail(232);
			return;
		}
		call.store<uint32_t>(call[4], count);
		call.ret(1);
	};
	registry.add("WriteConsoleOutputCharacterW", 5, [write_characters](ApiCall &call) { write_characters(call, true); });
	registry.add("WriteConsoleOutputCharacterA", 5, [write_characters](ApiCall &call) { write_characters(call, false); });
	// Scrolling moves whole rows of the window: a rectangle as wide as the window moved straight up or down, the rows it uncovers
	// blanked. Other moves cannot be made on a terminal, which cannot read its cells back; they leave the screen as it is.
	const auto scroll = [output_of, &console](ApiCall &call) {
		const auto found = output_of(call);
		if (!found)
			return;
		const auto rectangle = call.read(call[1], 8);
		int16_t left, top, right, bottom;
		std::memcpy(&left, rectangle.data(), 2);
		std::memcpy(&top, rectangle.data() + 2, 2);
		std::memcpy(&right, rectangle.data() + 4, 2);
		std::memcpy(&bottom, rectangle.data() + 6, 2);
		const auto window = console.size(found->descriptor);
		const auto destination = std::pair{int16_t(call.u32(3) & 0xffff), int16_t(call.u32(3) >> 16)};
		if (left > right || top > bottom) {
			call.fail(87);
			return;
		}
		top = std::max<int16_t>(top, 0);
		bottom = std::min<int16_t>(bottom, int16_t(window.rows - 1));
		const bool whole_rows = left <= 0 && right >= window.columns - 1 && destination.first == left;
		const int lines = top - destination.second;
		if (whole_rows && lines && top <= bottom) {
			// The region spans the rows that move and the rows they move into.
			const int16_t first = std::max<int16_t>(std::min<int16_t>(top, destination.second), 0);
			const int16_t last = std::min<int16_t>(std::max<int16_t>(bottom, int16_t(destination.second + bottom - top)), int16_t(window.rows - 1));
			if (!console.scroll(found->descriptor, first, last, lines)) {
				call.fail(232);
				return;
			}
		}
		call.ret(1);
	};
	registry.add("ScrollConsoleScreenBufferW", 5, scroll);
	registry.add("ScrollConsoleScreenBufferA", 5, scroll);
	// A terminal cannot recolour cells already drawn: the attribute is taken as applied, and later text is drawn in the current one.
	registry.add("WriteConsoleOutputAttribute", 5, [output_of, &console](ApiCall &call) {
		const auto found = output_of(call);
		if (!found)
			return;
		call.store<uint32_t>(call[4], call.u32(2));
		call.ret(1);
	});
	registry.add("FillConsoleOutputAttribute", 5, [output_of, coordinate, inside, &console](ApiCall &call) {
		const auto found = output_of(call);
		if (!found)
			return;
		const auto cell = coordinate(call.u32(3));
		if (!inside(found->descriptor, cell)) {
			call.fail(87);
			return;
		}
		const auto window = console.size(found->descriptor);
		const uint32_t room = uint32_t(window.columns) * uint32_t(window.rows) - uint32_t(cell.second * window.columns + cell.first);
		call.store<uint32_t>(call[4], std::min(call.u32(2), room));
		call.ret(1);
	});

	registry.add("GetConsoleCP", 0, [&console](ApiCall &call) { call.ret(console.input_code_page); });
	registry.add("GetConsoleOutputCP", 0, [&console](ApiCall &call) { call.ret(console.output_code_page); });
	registry.add("SetConsoleCP", 1, [&console](ApiCall &call) {
		if (!known_code_page(call.u32(0))) {
			call.fail(87);
			return;
		}
		console.input_code_page = call.u32(0);
		call.ret(1);
	});
	registry.add("SetConsoleOutputCP", 1, [&console](ApiCall &call) {
		if (!known_code_page(call.u32(0))) {
			call.fail(87);
			return;
		}
		console.output_code_page = call.u32(0);
		call.ret(1);
	});

	// The title goes to the terminal as its window title; without a terminal it is only kept.
	const auto title_output = []() {
		for (const int descriptor : {1, 2})
			if (::isatty(descriptor))
				return descriptor;
		return -1;
	};
	registry.add("SetConsoleTitleW", 1, [title_output, &console](ApiCall &call) {
		console.set_title(title_output(), call.wide(call[0]));
		call.ret(1);
	});
	registry.add("SetConsoleTitleA", 1, [title_output, &context, &console](ApiCall &call) {
		console.set_title(title_output(), from_ansi(call.narrow(call[0]), context.launch.ansi_code_page));
		call.ret(1);
	});
	const auto get_title = [&context, &console](ApiCall &call, bool wide) {
		const auto &title = console.title();
		const uint32_t capacity = call.u32(1);
		if (wide) {
			call.put_wide(call[0], title.substr(0, capacity ? capacity - 1 : 0), capacity);
			call.ret(std::min<size_t>(title.size(), capacity ? capacity - 1 : 0));
		} else {
			const auto narrow = to_ansi(title, context.launch.ansi_code_page);
			call.put_narrow(call[0], std::string_view(narrow).substr(0, capacity ? capacity - 1 : 0), capacity);
			call.ret(std::min<size_t>(narrow.size(), capacity ? capacity - 1 : 0));
		}
	};
	registry.add("GetConsoleTitleW", 2, [get_title](ApiCall &call) { get_title(call, true); });
	registry.add("GetConsoleTitleA", 2, [get_title](ApiCall &call) { get_title(call, false); });
	// The console has no window of its own: the terminal draws it.
	registry.add("GetConsoleWindow", 0, [](ApiCall &call) { call.ret(0); });
	// A process stays attached to the terminal it started on: attaching to another fails with ERROR_ACCESS_DENIED, as for a process that
	// already has a console, and without a terminal there is no console to attach to.
	registry.add("AttachConsole", 1, [](ApiCall &call) { call.fail(has_console_terminal() ? 5 : 6); });
	registry.add("FreeConsole", 0, [](ApiCall &call) { call.ret(1); });
	// The processes on the console are counted per terminal by the host, so each process lists itself.
	registry.add("GetConsoleProcessList", 2, [](ApiCall &call) {
		if (call.u32(1))
			call.store<uint32_t>(call[0], uint32_t(::getpid()));
		call.ret(1);
	});

	static ControlHandlers handlers;
	handlers.threads = &context.threads;
	registry.add("SetConsoleCtrlHandler", 2, [](ApiCall &call) {
		const uint64_t routine = call[0];
		const bool add = call.u32(1);
		if (!routine) {
			handlers.ignore = add;
			call.ret(1);
			return;
		}
		if (add) {
			handlers.routines.push_back(routine);
			call.ret(1);
			return;
		}
		const auto found = std::find(handlers.routines.rbegin(), handlers.routines.rend(), routine);
		if (found == handlers.routines.rend()) {
			call.fail(87);
			return;
		}
		handlers.routines.erase(std::next(found).base());
		call.ret(1);
	});
	// Whether the standard streams are a terminal is known once they are the request's, in a start a warm host forked ahead.
	warm_when_ready([&context] {
		const bool terminal = ::isatty(0) || ::isatty(1) || ::isatty(2);
		// The window changing size: the cursor must be asked for again, and programs that asked for window input get an event.
		if (terminal) {
			struct sigaction resized{};
			resized.sa_handler = [](int) { TerminalConsole::instance().note_resize(); };
			resized.sa_flags = SA_RESTART;
			sigemptyset(&resized.sa_mask);
			::sigaction(SIGWINCH, &resized, nullptr);
		}
		// Ctrl+C reaches 64-bit programs only: a 32-bit one keeps the host's default, which ends it.
		if (!terminal || context.process.mode() == NativeMode::x86 || control_handlers)
			return;
		control_handlers = &handlers;
		struct sigaction action{};
		action.sa_handler = interrupted;
		action.sa_flags = SA_RESTART;
		sigemptyset(&action.sa_mask);
		::sigaction(SIGINT, &action, nullptr);
		context.threads.add_service([&context] {
			auto &state = handlers;
			const auto start = [&context, &state](uint64_t routine, uint64_t parameter) {
				state.running = context.threads.create(routine, parameter, 64 * 1024, false);
			};
			// Nobody handled the press: ExitProcess runs on a thread of its own, as the default handler calls it.
			const auto end_process = [&context, start]() {
				const auto exit = context.modules.resolve(PEImport{"kernel32.dll", "ExitProcess", 0, false, 0});
				if (!exit) {
					TerminalConsole::instance().restore();
					::_exit(int(status_control_c_exit & 0xff));
				}
				start(exit, status_control_c_exit);
			};
			if (state.running) {
				if (state.running->state != GuestThread::State::done)
					return;
				const bool handled = state.running->exit_code != 0;
				state.running.reset();
				if (handled) {
					state.asking.clear();
					return;
				}
				if (state.asking.empty()) {
					end_process();
					return;
				}
				start(state.asking.back(), 0);
				state.asking.pop_back();
				return;
			}
			if (!state.presses.exchange(0) || state.ignore)
				return;
			state.asking = state.routines;
			if (state.asking.empty()) {
				end_process();
				return;
			}
			start(state.asking.back(), 0);
			state.asking.pop_back();
		});
	});
}

}
