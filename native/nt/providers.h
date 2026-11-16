#pragma once

#include "environment.h"
#include "desktop/presentation.h"
#include <functional>
#include <optional>

namespace raven {

// Original name-only PE64 modules for implemented environment and loader handlers.
std::vector<uint8_t> environment_provider(std::string_view name, uint64_t base);
std::vector<uint8_t> runtime_provider(std::string_view name, uint64_t base,
	std::span<const std::string_view> exports, bool x86 = false,
	std::span<const std::string_view> data_exports = {},
	std::span<const std::pair<std::string_view, uint16_t>> ordinal_exports = {},
	// Gives hot kernel32 services guest-side fast paths; their slow-path trap sits at a fixed offset inside the stub.
	bool fast_paths = false);
// A 64-bit provider made for one base, moved to another: its code and tables are relative to the image and only the header names the base,
// so a provider is made once even though its size decides where it goes.
std::vector<uint8_t> moved_provider(std::vector<uint8_t> bytes, uint64_t base);
// Where the breakpoint of a 64-bit provider export sits inside its stub: past the guest-side path of a fast or served stub, at its start
// otherwise.
size_t fast_stub_trap_offset(std::string_view module, std::string_view name);
// The breakpoint a 64-bit provider export ends in when the runtime serves it: behind a guest-side prelude when the export has one.
uint64_t api_trap_address(const PEModule &provider, std::string_view module, std::string_view name, uint32_t address);
// Runs a PE32 process in host compatibility mode; only the API providers listed in x86_process.cpp exist so far.
// A source of libraries shipped beside the program lets a program import them statically: they are loaded with it, and their entry points run
// before the program's.
NativeResult run_native_process_x86(ModuleSet &modules, PEModule &entry,
	const std::string &bootstrap, std::chrono::milliseconds timeout, std::optional<NativeLaunch> launch = {},
	DesktopPresentation presentation = {},
	std::function<std::optional<ModuleBytes>(std::string_view)> source = {});
NativeResult run_native_process(ModuleSet &modules, PEModule &entry,
	const std::string &bootstrap, std::chrono::milliseconds timeout,
	std::function<std::optional<ModuleBytes>(std::string_view)> source = {},
	std::function<void(std::span<const uint8_t>)> debug = {}, DesktopPresentation presentation = {},
	std::optional<NativeLaunch> launch = {});
// A runtime that ends with the one process it runs says so before it runs it: what the process leaves is then not freed block by block
// once the guest ended, as the kernel takes it back whole, and in a start forked from a warm host freeing would first copy every page it
// shares with the host. releasing_memory() tells the runtime's operator delete whether to free.
void end_with_process();
bool releasing_memory();

}
