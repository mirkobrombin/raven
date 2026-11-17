#pragma once

#include <chrono>
#include <csignal>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <termios.h>
#include <utility>
#include <vector>

namespace raven {

// Console input modes (SetConsoleMode on an input handle).
inline constexpr uint32_t console_processed_input = 0x1, console_line_input = 0x2, console_echo_input = 0x4, console_window_input = 0x8,
	console_virtual_terminal_input = 0x200, console_input_modes = 0x3ff;
// Console output modes (SetConsoleMode on an output handle).
inline constexpr uint32_t console_processed_output = 0x1, console_wrap_at_eol = 0x2, console_virtual_terminal_processing = 0x4,
	console_disable_newline_auto_return = 0x8, console_output_modes = 0x1f;
// Character attribute bits beyond the two colour nibbles.
inline constexpr uint16_t console_reverse_video = 0x4000, console_underscore = 0x8000;

// A key press or release, as a KEY_EVENT_RECORD carries it; or, with `event` WINDOW_BUFFER_SIZE_EVENT (4), the new size of the window
// in `key` (columns) and `scan` (rows).
struct ConsoleKey {
	uint16_t event = 1;
	bool down = true;
	uint16_t repeat = 1, key = 0, scan = 0;
	char16_t character = 0;
	uint32_t controls = 0;
	bool operator==(const ConsoleKey &) const = default;
};

// Turns what a terminal sends into key presses, each followed by its release. Returns the bytes used: an escape sequence or UTF-8
// character cut off at the end stays for the next call unless `flush` says no more is coming, and then a lone ESC is the Escape key.
// With `virtual_terminal`, escape sequences are not interpreted: every character is a key of its own, as with
// ENABLE_VIRTUAL_TERMINAL_INPUT.
size_t decode_terminal_keys(std::string_view bytes, bool flush, bool virtual_terminal, std::vector<ConsoleKey> &keys);
// The SGR sequence that draws in a console character attribute; white on black is the terminal's own default colours.
std::string attribute_sequence(uint16_t attribute);

// Follows the cursor through what is written to a terminal, moving it as the terminal does: text by its width on screen, wrapping
// at the right margin, scrolling at the bottom, control characters, and the escape sequences that move the cursor or save and restore
// it. Bytes may arrive in any pieces; a sequence or UTF-8 character cut off at the end of one is finished by the next.
class CursorTracker {
public:
	void resize(int16_t columns, int16_t rows);
	void place(int16_t column, int16_t row);
	// `return_on_newline`: a line feed also returns the carriage, as the terminal's ONLCR output setting does.
	void feed(std::string_view bytes, bool return_on_newline = true);
	int16_t column() const { return x; }
	int16_t row() const { return y; }
	int16_t columns() const { return width; }
	int16_t rows() const { return height; }

private:
	void print(int cells);
	void line_feed();
	void control_sequence(char final);
	void clamp();
	enum class State { text, escape, control, string, string_escape, designate } state = State::text;
	int16_t width = 80, height = 25, x = 0, y = 24, saved_x = 0, saved_y = 0;
	// The terminal holds a character written in the last column there until the next one, which wraps first.
	bool wrap_pending = false;
	std::string parameters, character;
};

// The cells a character takes on a terminal: none for combining marks, two for wide East Asian characters and emoji.
int character_cells(char32_t character);

// The terminal that is the process's console, opened anew for reading and writing: the controlling terminal, or the one the start that
// asked a warm host for this process handed over with its standard streams (see set_console_terminal). -1 without a console.
int open_console_terminal();
// Whether the process has a console terminal to open.
bool has_console_terminal();
// Makes a descriptor the start of this process handed over its console terminal: a warm host runs in a session of its own, without
// the terminal its client was started from. A descriptor that is no terminal leaves the process without a console.
void set_console_terminal(int descriptor);
// Gives up the terminal handed over, to pass on: -1 when there is none. The process is left with the controlling terminal, if any.
int take_console_terminal();

// The console: the host terminal behind the standard handles. A process has one, so its state lives here; the input modes live in the
// terminal itself, so every process sharing the terminal sees the same ones, as processes sharing a Windows console do.
class TerminalConsole {
public:
	static TerminalConsole &instance();
	~TerminalConsole();
	TerminalConsole(const TerminalConsole &) = delete;
	TerminalConsole &operator=(const TerminalConsole &) = delete;

	// The input mode the terminal is in now; bits the terminal has no setting for are as the program last set them.
	uint32_t input_mode(int descriptor);
	// Puts the terminal into an input mode; false for a combination Windows rejects (echo without line input).
	bool set_input_mode(int descriptor, uint32_t mode);
	uint32_t output_mode() const { return output; }
	void set_output_mode(int descriptor, uint32_t mode);

	// Writes to the terminal, following the cursor through it.
	bool write(int descriptor, std::string_view bytes);
	// Follows the cursor through bytes the program wrote to the terminal some other way (WriteFile).
	void note_output(int descriptor, std::string_view bytes);

	// Line mode: whether a whole line has arrived (or the input ended), reading what the terminal has without waiting.
	bool line_ready(int descriptor);
	// The line ReadConsole returns, ending in CR LF, up to `limit` characters; the rest is kept for the next read.
	std::u16string take_line(size_t limit);
	// Key events: whether any have arrived, reading what the terminal has without waiting. An escape sequence cut off at the end is
	// taken as typed keys once nothing has followed it for 30 ms, or at once with `settle`.
	bool keys_ready(int descriptor, bool settle);
	// True while bytes at the end wait for the rest of an escape sequence.
	bool partial() const { return !pending.empty(); }
	std::vector<ConsoleKey> peek_keys(size_t limit, bool remove);
	size_t key_count() const { return keys.size(); }
	// The characters of the typed keys that have one, for ReadConsole outside line mode.
	std::u16string take_characters(size_t limit);
	// True once the terminal reported the end of its input.
	bool ended() const { return finished; }
	void flush_input(int descriptor);

	struct Size {
		int16_t columns, rows;
	};
	Size size(int descriptor) const;
	// The cursor, zero-based. It is followed through the output; the terminal is asked where it is only when the console may have lost
	// track (at first, after another process wrote, after a resize), and only while the terminal answers.
	std::pair<int16_t, int16_t> cursor(int descriptor);
	// Another process may write to the terminal: the cursor is asked for again before it is next reported.
	void mark_stale() { stale = true; }
	// Moves by relative motions from where the cursor is followed to be, so moves stay right on a terminal that never says where it is.
	bool move_cursor(int descriptor, int16_t column, int16_t row);
	// Writes text from a cell on, leaving the cursor where it was (WriteConsoleOutputCharacter).
	bool write_characters(int descriptor, std::u16string_view text, int16_t column, int16_t row);
	// Scrolls whole rows, top to bottom inclusive, up by `lines` (down when negative); the rows uncovered are blanked.
	bool scroll(int descriptor, int16_t top, int16_t bottom, int lines);
	// The window changed size (SIGWINCH): async-signal-safe.
	void note_resize() { resized_flag = 1; }
	uint16_t attribute() const { return colours; }
	bool set_attribute(int descriptor, uint16_t attribute);
	bool cursor_visible() const { return visible; }
	uint32_t cursor_size() const { return cursor_percent; }
	bool set_cursor_info(int descriptor, uint32_t percent, bool show);
	// Writes `count` copies of a character from a cell on, leaving the cursor where it was.
	bool fill_characters(int descriptor, char16_t character, uint32_t count, int16_t column, int16_t row);
	const std::u16string &title() const { return caption; }
	// Keeps the title and shows it in the terminal's window title; a negative descriptor only keeps it.
	bool set_title(int descriptor, std::u16string text);
	// What WriteFile puts on a screen buffer: text in the output code page, written to the terminal as UTF-8.
	bool write_text(int descriptor, std::string_view bytes);
	// A console starts on the OEM code page, as on Windows; the text a program reads and writes in it is UTF-8 on the terminal.
	uint32_t input_code_page = 437, output_code_page = 437;

	// Puts the terminal back as it was before this process changed it.
	void restore();
	// The same from a signal handler, with async-signal-safe calls only.
	void restore_from_signal();

private:
	TerminalConsole() = default;
	void remember(int descriptor);
	// Reads what the terminal has now, without waiting; `raw` reads keys one by one even in line mode.
	void gather(int descriptor, bool raw);
	// Keeps the tracker's window the terminal's size; a change of size means the cursor must be asked for again.
	void follow_size(int descriptor);
	// The motions from the followed cursor to a cell.
	std::string motion(int16_t column, int16_t row) const;
	// Asks the terminal where the cursor is; nullopt when it does not answer.
	std::optional<std::pair<int16_t, int16_t>> query_cursor(int descriptor);
	std::optional<termios> original;
	int original_descriptor = -1;
	uint32_t extra_input = 0x1f0;
	uint32_t output = console_processed_output | console_wrap_at_eol;
	// Bytes read from the terminal and not yet handed out, and the keys and line made from them.
	std::string pending;
	std::deque<ConsoleKey> keys;
	std::u16string line;
	bool line_complete = false, finished = false;
	// When the bytes still waiting for the rest of an escape sequence last grew.
	std::chrono::steady_clock::time_point partial_since;
	uint16_t colours = 7;
	bool visible = true;
	uint32_t cursor_percent = 25;
	std::u16string caption;
	bool cursor_answers = true;
	CursorTracker tracker;
	bool sized = false, stale = true;
	volatile sig_atomic_t resized_flag = 0;
};

}
