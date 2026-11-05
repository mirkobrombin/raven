#include "api.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace raven {

namespace {

bool memory_error(const std::runtime_error &failure)
{
	const std::string_view message = failure.what();
	return message.find("readable") != message.npos || message.find("writable") != message.npos;
}

}

ApiCall::ApiCall(NativeProcess &process, NativeEnvironment &environment, std::vector<uint64_t> arguments)
	: process(process), environment(environment), arguments(std::move(arguments)) {}

void ApiCall::fail(uint32_t error, uint64_t value)
{
	environment.set_last_error(error);
	result = value;
}

uint32_t ApiCall::last_error() const
{
	return environment.last_error();
}

std::string describe_text(NativeProcess &process, uint64_t address)
{
	if (address < 0x10000 || address >= 0x800000000000ULL)
		return {};
	try {
		const auto bytes = process.read_memory(address, std::min<uint64_t>(1000, 4096 - (address & 4095)));
		if (bytes.size() < 4)
			return {};
		const bool wide = bytes[1] == 0 && bytes[0] >= 32 && bytes[0] < 127 && bytes[2] >= 32 && bytes[2] < 127 && bytes[3] == 0;
		std::string text;
		for (size_t at = 0; at + (wide ? 1 : 0) < bytes.size(); at += wide ? 2 : 1) {
			const auto character = bytes[at];
			if (character == 0 && (!wide || bytes[at + 1] == 0))
				break;
			// Log lines end in a newline: a text may stop at the first character that is not printable.
			if (character < 32 || character > 126 || (wide && bytes[at + 1])) {
				if (character == '\r' || character == '\n')
					break;
				return text.size() >= 8 ? text : std::string();
			}
			text.push_back(char(character));
		}
		return text.size() >= 3 ? text : std::string();
	} catch (const std::runtime_error &) {
		return {};
	}
}

void ApiCall::store_pointer(uint64_t address, uint64_t value) const
{
	if (process.mode() == NativeMode::x86)
		store<uint32_t>(address, uint32_t(value));
	else
		store<uint64_t>(address, value);
}

uint64_t ApiCall::load_pointer(uint64_t address) const
{
	return process.mode() == NativeMode::x86 ? uint64_t(load<uint32_t>(address)) : load<uint64_t>(address);
}

size_t ApiCall::pointer_width() const
{
	return process.mode() == NativeMode::x86 ? 4 : 8;
}

void ApiCall::set_last_error(uint32_t error)
{
	environment.set_last_error(error);
}

std::vector<uint8_t> ApiCall::read(uint64_t address, size_t size) const
{
	if (!size)
		return {};
	try {
		return process.read_memory(address, size);
	} catch (const std::runtime_error &failure) {
		if (!memory_error(failure))
			throw;
		static const bool traced = std::getenv("RAVEN_TRACE_FAIL") != nullptr;
		if (traced)
			std::fprintf(stderr, "raven: %s reads %zu bytes at %llx\n", api_in_progress, size, static_cast<unsigned long long>(address));
		throw ApiFault{};
	}
}

void ApiCall::write(uint64_t address, std::span<const uint8_t> bytes) const
{
	if (bytes.empty())
		return;
	try {
		process.write_memory(address, bytes);
	} catch (const std::runtime_error &failure) {
		if (!memory_error(failure))
			throw;
		static const bool traced = std::getenv("RAVEN_TRACE_FAIL") != nullptr;
		if (traced)
			std::fprintf(stderr, "raven: %s writes %zu bytes at %llx\n", api_in_progress, bytes.size(), static_cast<unsigned long long>(address));
		throw ApiFault{};
	}
}

// A string longer than its caller allows fails the call as a fault does; RAVEN_TRACE_FAIL tells the two apart.
static void trace_long_string(uint64_t address, size_t limit)
{
	static const bool traced = std::getenv("RAVEN_TRACE_FAIL") != nullptr;
	if (traced)
		std::fprintf(stderr, "raven: %s reads a string longer than %zu characters at %llx\n", api_in_progress, limit,
			static_cast<unsigned long long>(address));
}

std::u16string ApiCall::wide(uint64_t address, size_t limit) const
{
	std::u16string text;
	// Read page-bounded chunks so a string ending just before unmapped memory is still readable.
	while (text.size() <= limit) {
		const size_t room = 4096 - (address + text.size() * 2) % 4096;
		const auto bytes = read(address + text.size() * 2, room & 1 ? room - 1 : room);
		for (size_t at = 0; at + 1 < bytes.size(); at += 2) {
			const char16_t character = char16_t(bytes[at] | bytes[at + 1] << 8);
			if (!character)
				return text;
			text.push_back(character);
		}
	}
	trace_long_string(address, limit);
	throw ApiFault{};
}

std::string ApiCall::narrow(uint64_t address, size_t limit) const
{
	std::string text;
	while (text.size() <= limit) {
		const size_t room = 4096 - (address + text.size()) % 4096;
		const auto bytes = read(address + text.size(), room);
		for (const auto byte : bytes) {
			if (!byte)
				return text;
			text.push_back(char(byte));
		}
	}
	trace_long_string(address, limit);
	throw ApiFault{};
}

size_t ApiCall::put_wide(uint64_t address, std::u16string_view text, size_t capacity) const
{
	if (capacity > text.size()) {
		std::vector<uint8_t> bytes((text.size() + 1) * 2);
		std::memcpy(bytes.data(), text.data(), text.size() * 2);
		write(address, bytes);
	}
	return text.size();
}

size_t ApiCall::put_narrow(uint64_t address, std::string_view text, size_t capacity) const
{
	if (capacity > text.size()) {
		std::vector<uint8_t> bytes(text.size() + 1);
		std::memcpy(bytes.data(), text.data(), text.size());
		write(address, bytes);
	}
	return text.size();
}

void ApiRegistry::add(std::string_view name, uint32_t arguments, ApiHandler handler)
{
	if (!entries.emplace(std::string(name), Entry{arguments, std::move(handler)}).second)
		throw std::runtime_error("native API registered twice: " + std::string(name));
}

std::optional<std::pair<uint32_t,ApiHandler>> ApiRegistry::find(std::string_view name) const
{
	const auto found = entries.find(std::string(name));
	if (found == entries.end())
		return std::nullopt;
	return std::make_pair(found->second.arguments, found->second.handler);
}

const char *api_in_progress = "";

bool ApiRegistry::dispatch(NativeProcess &process, NativeEnvironment &environment, std::string_view name, bool widen_handles) const
{
	const auto found = entries.find(std::string(name));
	if (found == entries.end())
		return false;
	auto words = process.api_arguments(found->second.arguments);
	if (widen_handles)
		for (auto &word : words)
			if (word == 0xffffffffULL || word == 0xfffffffeULL)
				word |= 0xffffffff00000000ULL;
	ApiCall call(process, environment, std::move(words));
	api_in_progress = found->first.c_str();
	try {
		found->second.handler(call);
	} catch (const ApiFault &) {
		static const bool traced = std::getenv("RAVEN_TRACE_FAIL") != nullptr;
		if (traced) {
			std::fprintf(stderr, "raven: %s read memory the guest does not own, arguments", found->first.c_str());
			for (size_t index = 0; index < found->second.arguments; ++index)
				std::fprintf(stderr, " %llx", (unsigned long long)call[index]);
			std::fprintf(stderr, "\n");
		}
		call.fail(998);
	} catch (const uint32_t &error) {
		// The window and drawing layers report a bad handle or argument by throwing the Windows error: the call fails with it.
		call.fail(error);
	}
	process.return_api(call.result);
	return true;
}

}
