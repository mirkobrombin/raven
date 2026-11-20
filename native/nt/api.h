#pragma once

#include "environment.h"

#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <utility>
#include <string>
#include <string_view>

namespace raven {

// Raised by guest-memory accessors when the guest passed a pointer that cannot be read or written.
struct ApiFault {};
// Thrown by a call served beside the turn (ApiCall::alone) that needs what only the turn gives, such as memory the child cannot be reached
// at directly: the call is then answered the usual way, from the start.
struct ApiNeedsTurn {};

// One guest call of a native API: argument words, the return value and bounds-checked guest memory.
// The text a guest pointer addresses when it holds a short run of printable characters, narrow or wide; empty otherwise. For traces.
std::string describe_text(NativeProcess &process, uint64_t address);

class ApiCall {
public:
	ApiCall(NativeProcess &process, NativeEnvironment &environment, std::vector<uint64_t> arguments);
	uint64_t operator[](size_t index) const { return arguments.at(index); }
	uint32_t u32(size_t index) const { return uint32_t(arguments.at(index)); }
	int32_t i32(size_t index) const { return int32_t(arguments.at(index)); }
	// Sets the value the guest receives in RAX; handlers that return nothing leave it zero.
	void ret(uint64_t value) { result = value; }
	// Fails the call the way Windows does: sets the last error and returns the given failure value.
	void fail(uint32_t error, uint64_t value = 0);
	uint32_t last_error() const;
	void set_last_error(uint32_t error);

	std::vector<uint8_t> read(uint64_t address, size_t size) const;
	void write(uint64_t address, std::span<const uint8_t> bytes) const;
	template<typename T> T load(uint64_t address) const
	{
		const auto bytes = read(address, sizeof(T));
		T value{};
		std::memcpy(&value, bytes.data(), sizeof(T));
		return value;
	}
	template<typename T> void store(uint64_t address, T value) const
	{
		write(address, std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(&value), sizeof(T)));
	}
	// A pointer-sized value for the guest: four bytes for a 32-bit program, eight for a 64-bit one.
	void store_pointer(uint64_t address, uint64_t value) const;
	uint64_t load_pointer(uint64_t address) const;
	// The size of a pointer in the guest: four bytes for a 32-bit program, eight for a 64-bit one.
	size_t pointer_width() const;
	// Zero-terminated guest strings; a string longer than limit characters faults the call.
	std::u16string wide(uint64_t address, size_t limit = 32768) const;
	std::string narrow(uint64_t address, size_t limit = 32768) const;
	// Copies text and a terminator when capacity allows; returns the characters required without the terminator.
	size_t put_wide(uint64_t address, std::u16string_view text, size_t capacity) const;
	size_t put_narrow(uint64_t address, std::string_view text, size_t capacity) const;

	NativeProcess &process;
	NativeEnvironment &environment;
	uint64_t result = 0;
	// Served beside the turn by a host thread that does not hold it: guest memory, and the last error in the block of the host thread's
	// own guest thread, are read and written straight from the child (see ApiNeedsTurn).
	bool alone = false;

private:
	std::vector<uint64_t> arguments;
};

// The name of the API call being served, for diagnostics.
extern const char *api_in_progress;

using ApiHandler = std::function<void(ApiCall &)>;

// Name-keyed API handlers. The argument count is the number of words the handler may read.
class ApiRegistry {
public:
	void add(std::string_view name, uint32_t arguments, ApiHandler handler);
	bool has(std::string_view name) const { return entries.contains(std::string(name)); }
	// The handler and word count registered under a name, for adapters that reuse it.
	std::optional<std::pair<uint32_t,ApiHandler>> find(std::string_view name) const;
	// Runs the handler for an API trap; false when no handler is registered under this name.
	// For a 32-bit caller, the pseudo handles -1 and -2 arrive sign-extended, as the handlers of a 64-bit process expect them.
	bool dispatch(NativeProcess &process, NativeEnvironment &environment, std::string_view name, bool widen_handles = false) const;

private:
	struct Entry {
		uint32_t arguments;
		ApiHandler handler;
	};
	std::map<std::string, Entry> entries;
};

}
