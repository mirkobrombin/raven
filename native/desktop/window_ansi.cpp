#include "windows.h"
#include "nt/text.h"

#include <sys/mman.h>

namespace raven {

namespace {

constexpr uint32_t ansi_code_page = 1252;

// Guest scratch for the wide copies of a call's strings; released when the call returns.
class Scratch {
public:
	explicit Scratch(NativeProcess &process) : process(process) {}
	~Scratch()
	{
		for (const auto address : blocks)
			process.unmap_memory(address);
	}
	uint64_t block(size_t bytes)
	{
		const auto address = process.map_memory((bytes + 4095) & ~size_t(4095), PROT_READ | PROT_WRITE);
		blocks.push_back(address);
		return address;
	}

private:
	NativeProcess &process;
	std::vector<uint64_t> blocks;
};

// A guest ANSI string as UTF-16 in scratch memory; null and atoms (small values that are not addresses) pass through.
uint64_t widen(ApiCall &call, Scratch &scratch, uint64_t address)
{
	if (address <= 0xffff)
		return address;
	const auto text = from_ansi(call.narrow(address), ansi_code_page);
	const auto at = scratch.block((text.size() + 1) * 2);
	call.write(at, std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(text.c_str()), (text.size() + 1) * 2));
	return at;
}

}

// The ANSI window functions take the same structures as the wide ones apart from the strings in them, so each widens those and runs the wide
// handler on the result.
void NativeWindows::register_ansi_api(ApiRegistry &registry)
{
	const auto forward = [this](ApiCall &call, std::string_view wide_name, std::vector<uint64_t> words) {
		process.begin_translated_call(std::move(words), {});
		bool known = false;
		try {
			known = dispatch(wide_name);
		} catch (...) {
			process.end_translated_call();
			throw;
		}
		const auto result = process.end_translated_call();
		if (!known)
			throw std::runtime_error("native window function is not implemented: " + std::string(wide_name));
		call.ret(result);
	};
	const auto add = [&](const char *name, uint32_t arguments, std::function<void(ApiCall &)> handler) {
		if (!registry.has(name))
			registry.add(name, arguments, std::move(handler));
	};
	// WNDCLASSA and WNDCLASSEXA hold the menu name at 56 and the class name at 64, as the wide structures do.
	const auto class_function = [=](const char *wide_name, size_t size) {
		return [=](ApiCall &call) {
			Scratch scratch(call.process);
			auto definition = call.read(call[0], size);
			for (const size_t offset : {size_t(56), size_t(64)}) {
				uint64_t pointer = 0;
				for (size_t byte = 0; byte < 8; ++byte)
					pointer |= uint64_t(definition[offset + byte]) << (byte * 8);
				pointer = widen(call, scratch, pointer);
				for (size_t byte = 0; byte < 8; ++byte)
					definition[offset + byte] = uint8_t(pointer >> (byte * 8));
			}
			const auto copy = scratch.block(size);
			call.write(copy, definition);
			forward(call, wide_name, {copy});
		};
	};
	add("RegisterClassA", 1, class_function("RegisterClassW", 72));
	add("RegisterClassExA", 1, class_function("RegisterClassExW", 80));
	add("UnregisterClassA", 2, [=](ApiCall &call) {
		Scratch scratch(call.process);
		forward(call, "UnregisterClassW", {widen(call, scratch, call[0]), call[1]});
	});
	add("CreateWindowExA", 12, [=](ApiCall &call) {
		Scratch scratch(call.process);
		std::vector<uint64_t> words;
		for (size_t index = 0; index < 12; ++index)
			words.push_back(call[index]);
		words[1] = widen(call, scratch, words[1]);
		words[2] = widen(call, scratch, words[2]);
		forward(call, "CreateWindowExW", std::move(words));
	});
	// The message functions share their structures with the wide ones: the strings in the messages are the window procedure's concern.
	for (const auto &[ansi, wide, arguments] : {std::tuple{"PeekMessageA", "PeekMessageW", 5u}, std::tuple{"GetMessageA", "GetMessageW", 4u},
			std::tuple{"DispatchMessageA", "DispatchMessageW", 1u}, std::tuple{"PostThreadMessageA", "PostThreadMessageW", 4u}})
		add(ansi, arguments, [=](ApiCall &call) {
			std::vector<uint64_t> words;
			for (size_t index = 0; index < arguments; ++index)
				words.push_back(call[index]);
			forward(call, wide, std::move(words));
		});
}

}
