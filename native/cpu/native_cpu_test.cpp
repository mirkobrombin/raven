#include "native_cpu.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <functional>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <signal.h>
#include <source_location>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

static void check(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

static void put(std::span<uint8_t> bytes, size_t offset, uint64_t value, size_t width = 4)
{
	for (size_t i = 0; i < width; ++i)
		bytes[offset + i] = static_cast<uint8_t>(value >> (i * 8));
}

static std::vector<uint8_t> fixture(std::initializer_list<uint8_t> code, bool is_64 = true)
{
	std::vector<uint8_t> bytes(0x600);
	put(bytes, 0, 0x5a4d, 2);
	put(bytes, 0x3c, 0x80);
	put(bytes, 0x80, 0x4550);
	put(bytes, 0x84, is_64 ? 0x8664 : 0x14c, 2);
	put(bytes, 0x86, 2, 2);
	put(bytes, 0x94, is_64 ? 240 : 224, 2);
	put(bytes, 0x96, 2, 2);
	put(bytes, 0x98, is_64 ? 0x20b : 0x10b, 2);
	put(bytes, 0xa8, 0x1000);
	put(bytes, is_64 ? 0xb0 : 0xb4, is_64 ? 0x140000000ULL : 0x400000, is_64 ? 8 : 4);
	put(bytes, 0xb8, 0x1000);
	put(bytes, 0xbc, 0x200);
	put(bytes, 0xd0, 0x5000);
	put(bytes, 0xd4, 0x200);
	put(bytes, is_64 ? 0x104 : 0xf4, 16);
	for (size_t i = 0; i < 2; ++i) {
		const size_t section = (is_64 ? 0x188 : 0x178) + i * 40;
		bytes[section] = '.';
		bytes[section + 1] = i == 0 ? 't' : 'd';
		put(bytes, section + 8, 0x400);
		put(bytes, section + 12, i == 0 ? 0x1000 : 0x3000);
		put(bytes, section + 16, 0x200);
		put(bytes, section + 20, 0x200 + i * 0x200);
		put(bytes, section + 36, i == 0 ? 0x60000020 : 0xc0000040);
	}
	std::copy(code.begin(), code.end(), bytes.begin() + 0x200);
	return bytes;
}

static void rejected(const std::function<void()> &operation, std::source_location where = std::source_location::current())
{
	try {
		operation();
	} catch (const std::runtime_error &) {
		return;
	}
	throw std::runtime_error("unsupported native setup was accepted (line " + std::to_string(where.line()) + ")");
}

static void no_children()
{
	int status = 0;
	errno = 0;
	check(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD, "native guest leaked or was not reaped");
}

static uint64_t value(std::span<const uint8_t> bytes)
{
	uint64_t result = 0;
	for (size_t byte = 0; byte < bytes.size(); ++byte)
		result |= uint64_t(bytes[byte]) << (byte * 8);
	return result;
}

static void parallel_stop_contracts(const std::string &bootstrap)
{
	const auto timeout = std::chrono::seconds(2);
	const auto bytes = fixture({0x83, 0x3a, 0, 0x74, 0xfb, 0x48, 0x89, 0xc8, 0xc3});
	const raven::PEImage image(bytes);
	raven::LoadedImage loaded(image, image.image_base);
	raven::LoadedImage *images[] = {&loaded};
	raven::NativeProcess process(images, bootstrap, timeout);
	check(process.parallel(), "stop delivery test requires parallel tasks");
	const auto gates = process.map_memory(4096, PROT_READ | PROT_WRITE);
	const auto stack = process.map_memory(65536, PROT_READ | PROT_WRITE);
	auto other = process.new_thread(stack + 65536, 0);
	bool nested = false, released[2]{};
	unsigned releases[2]{};
	uint64_t marker = 0;
	process.set_execution_wait([&](std::function<bool()> ready, auto until) {
		const unsigned index = nested ? 1 : 0;
		if (!released[index]) {
			check(!ready(), "gated task stopped before its release");
			if (!nested) {
				const auto saved = process.save_thread(false);
				process.load_thread(other);
				nested = true;
				const uint64_t arguments[]{marker + 1, gates + 8};
				const auto result = process.call(image.image_base + 0x1000, arguments, timeout);
				check(result.stop == raven::NativeStop::returned && result.value == marker + 1,
					"second task lost its delayed stop or return value");
				other = process.save_thread(false);
				process.load_thread(saved);
				nested = false;
			}
			released[index] = true;
			++releases[index];
			const uint8_t one = 1;
			process.write_memory(gates + index * 8, {&one, 1});
		}
		while (!ready() && std::chrono::steady_clock::now() < until)
			std::this_thread::sleep_for(std::chrono::microseconds(50));
	});
	for (unsigned round = 0; round < 32; ++round) {
		const uint8_t zero[16]{};
		process.write_memory(gates, zero);
		released[0] = released[1] = false;
		marker = 0x1234000000000000ULL + round * 2;
		const uint64_t arguments[]{marker, gates};
		const auto result = process.call(image.image_base + 0x1000, arguments, timeout);
		check(result.stop == raven::NativeStop::returned && result.value == marker,
			"first task lost its delayed stop after another task ran");
	}
	check(releases[0] == 32 && releases[1] == 32, "stop delivery test did not delay both tasks on every round");
	process.set_execution_wait({});
	process.stop_thread(other);
	process.unmap_memory(stack);
	process.unmap_memory(gates);
}

static void memory_contracts(const std::string &bootstrap)
{
	const auto timeout = std::chrono::seconds(2);
	auto bytes = fixture({0x89, 0x11, 0x8b, 0x01, 0xc3});
	bytes[0x240] = 0xff;
	bytes[0x241] = 0xe1;
	{
		const raven::PEImage image(bytes);
		raven::LoadedImage loaded(image, image.image_base);
		raven::LoadedImage *images[] = {&loaded};
		raven::NativeProcess process(images, bootstrap, timeout);
		const auto before = process.context();
		rejected([&] { process.map_memory(0, PROT_READ); });
		rejected([&] { process.map_memory(4095, PROT_READ); });
		rejected([&] { process.map_memory(4096, PROT_READ, 1); });
		rejected([&] { process.map_memory(8192, PROT_READ, 0x7ffffffff000ULL); });
		rejected([&] { process.map_memory(4096, 0x400); });
		rejected([&] { process.map_memory(4096, PROT_READ, image.image_base); });
		check(process.context().registers.rip == before.registers.rip, "failed allocation lost the stopped context");
		const auto allocation = process.map_memory(12288, PROT_READ | PROT_WRITE, 0x190000000ULL);
		check(allocation == 0x190000000ULL && process.read_memory(allocation, 12288) == std::vector<uint8_t>(12288), "dynamic mapping differs or is not zeroed");
		const auto atomic = allocation + 8200;
		check(process.compare_exchange(atomic, 0, 0x89abcdef, 4) == 0 && value(process.read_memory(atomic, 8)) == 0x89abcdef,
			"32-bit atomic exchange changed adjacent bytes or returned the wrong value");
		check(process.compare_exchange(atomic, 0, 7, 4) == 0x89abcdef && value(process.read_memory(atomic, 8)) == 0x89abcdef,
			"failed atomic comparison changed the word");
		check(process.compare_exchange(atomic, 0x89abcdef, 0x1122334455667788ULL, 8) == 0x89abcdef &&
			value(process.read_memory(atomic, 8)) == 0x1122334455667788ULL, "64-bit atomic exchange truncated its operand");
		rejected([&] { process.compare_exchange(atomic + 1, 0, 1, 4); });
		rejected([&] { process.compare_exchange(atomic, 0, 1, 2); });
		check(process.context().registers.rip == before.registers.rip, "atomic operation changed the caller's context");
		std::vector<uint8_t> unaligned(8191);
		for (size_t i = 0; i < unaligned.size(); ++i) unaligned[i] = static_cast<uint8_t>(i % 251);
		process.write_memory(allocation + 1, unaligned);
		check(process.read_memory(allocation + 1, unaligned.size()) == unaligned &&
			process.read_memory(allocation, 1)[0] == 0 && process.read_memory(allocation + 8192, 1)[0] == 0,
			"unaligned cross-page copy damaged payload or neighboring bytes");
		const uint8_t edge[] = {0xab, 0xcd};
		process.protect_memory(allocation + 4096, 4096, PROT_WRITE);
		process.write_memory(allocation + 4095, edge);
		rejected([&] { process.read_memory(allocation + 4096, 1); });
		process.protect_memory(allocation + 4096, 4096, PROT_READ | PROT_WRITE);
		check(process.read_memory(allocation + 4095, 2) == std::vector<uint8_t>(edge, edge + 2),
			"cross-page write required read access to writable guest memory");
		const std::vector<uint8_t> payload(12288, 0x5a);
		process.write_memory(allocation, payload);
		process.protect_memory(allocation + 4096, 4096, PROT_READ);
		rejected([&] { process.compare_exchange(allocation + 4096, 0x5a5a5a5a, 1, 4); });
		rejected([&] { process.write_memory(allocation + 4096, std::span<const uint8_t>(payload).first(1)); });
		rejected([&] { process.write_memory(allocation + 4095, std::span<const uint8_t>(payload).first(2)); });
		check(process.read_memory(allocation, payload.size()) == payload, "rejected cross-protection write changed memory");
		const uint64_t arguments[] = {allocation + 4096, 42};
		const auto fault = process.call(image.image_base + 0x1000, arguments, timeout);
		check(fault.stop == raven::NativeStop::fault && fault.signal == SIGSEGV && fault.fault_address == allocation + 4096,
			"guest write ignored dynamic read-only protection");
		process.protect_memory(allocation + 4096, 4096, PROT_READ | PROT_WRITE);
		const auto returned = process.resume(timeout);
		check(returned.stop == raven::NativeStop::returned && returned.value == 42, "dynamic protection repair lost the faulting instruction");
		process.protect_memory(allocation, 12288, PROT_NONE);
		rejected([&] { process.read_memory(allocation, 1); });
		process.protect_memory(allocation, 12288, PROT_READ | PROT_WRITE);
		rejected([&] { process.protect_memory(allocation + 4096, 12288, PROT_READ); });
		rejected([&] { process.protect_memory(image.image_base, 4096, PROT_READ | PROT_WRITE); });
		rejected([&] { process.unmap_memory(allocation + 4096); });
		rejected([&] { process.unmap_memory(image.image_base); });
		process.unmap_memory(allocation);
		rejected([&] { process.read_memory(allocation, 1); });
		rejected([&] { process.unmap_memory(allocation); });
		check(process.map_memory(4096, PROT_READ, allocation) == allocation, "release did not free the entire allocation");
		check(process.read_memory(allocation, 4) == std::vector<uint8_t>(4), "released allocation contents survived remapping");
		process.unmap_memory(allocation);
		const auto code = process.map_memory(4096, PROT_READ | PROT_WRITE);
		const uint8_t instructions[] = {0xb8, 77, 0, 0, 0, 0xc3};
		process.write_memory(code, instructions);
		rejected([&] { process.call(code, {}, timeout); });
		const uint64_t destination[] = {code};
		const auto nx = process.call(image.image_base + 0x1040, destination, timeout);
		check(nx.stop == raven::NativeStop::fault && nx.signal == SIGSEGV && nx.fault_address == code, "guest jump executed dynamic non-executable memory");
		process.protect_memory(code, 4096, PROT_READ | PROT_EXEC);
		check(process.resume(timeout).value == 77, "dynamic executable memory did not resume");
		process.unmap_memory(code);
	}
	no_children();
	for (bool syscall : {false, true}) {
		bytes = syscall ? fixture({0xb8, 60, 0, 0, 0, 0x31, 0xff, 0x0f, 0x05, 0xc3}) : fixture({0xcc, 0xc3});
		{
			const raven::PEImage image(bytes);
			raven::LoadedImage loaded(image, image.image_base);
			raven::LoadedImage *images[] = {&loaded};
			raven::NativeProcess process(images, bootstrap, timeout);
			if (!syscall)
				process.add_api_trap(image.image_base + 0x1000);
			const uint64_t arguments[] = {17, 23};
			const auto stopped = process.call(image.image_base + 0x1000, arguments, timeout);
			check(stopped.stop == (syscall ? raven::NativeStop::syscall : raven::NativeStop::api), "memory suspension fixture did not stop");
			const auto before = process.context();
			const auto mapped = process.map_memory(4096, PROT_READ | PROT_WRITE);
			process.protect_memory(mapped, 4096, PROT_READ);
			process.unmap_memory(mapped);
			const auto after = process.context();
			check(std::memcmp(&after.registers, &before.registers, sizeof(user_regs_struct)) == 0 &&
				std::equal(before.xstate.begin(), before.xstate.begin() + 416, after.xstate.begin()), "memory gate lost suspended context");
			if (syscall) {
				auto response = after;
				response.registers.rax = 88;
				response.registers.orig_rax = uint64_t(-1);
				process.set_context(response);
			} else {
				check(process.api_arguments(2) == std::vector<uint64_t>{17, 23}, "memory gate lost pending API arguments");
				process.return_api(88);
			}
			const auto returned = process.resume(timeout);
			check(returned.stop == raven::NativeStop::returned && returned.value == 88, "memory gate executed blocked exit or lost the pending return");
		}
		no_children();
	}
	std::cout << "Dynamic memory allocation, protections, release, instruction fault repair and API/syscall context preservation passed\n";
}

static void image_mapping_contracts(const std::string &bootstrap)
{
	const auto timeout = std::chrono::seconds(2);
	for (bool pending : {false, true}) {
		auto bytes = fixture({0xcc, 0xc3});
		const raven::PEImage image(bytes);
		raven::LoadedImage loaded(image, image.image_base);
		raven::LoadedImage *images[] = {&loaded};
		{
			raven::NativeProcess process(images, bootstrap, timeout);
			std::ifstream children("/proc/self/task/" + std::to_string(getpid()) + "/children");
			pid_t child = -1;
			children >> child;
			// The children file is optional in the host kernel; status still identifies the process this test owns.
			if (child <= 0)
				for (const auto &entry : std::filesystem::directory_iterator("/proc")) {
					const auto id = entry.path().filename().string();
					if (id.empty() || id.find_first_not_of("0123456789") != std::string::npos) continue;
					std::ifstream status(entry.path() / "status");
					std::string field;
					while (status >> field) {
						if (field == "PPid:") {
							pid_t parent = -1;
							status >> parent;
							if (parent == getpid()) child = std::stoi(id);
							break;
						}
						std::getline(status, field);
					}
				}
			check(child > 0, "cannot inspect owned dynamic image child");
			const auto descriptors = [&] {
				size_t count = 0;
				for (const auto &entry : std::filesystem::directory_iterator("/proc/" + std::to_string(child) + "/fd")) {
					(void)entry;
					++count;
				}
				return count;
			};
			const auto descriptor_count = descriptors();
			if (pending) {
				process.add_api_trap(image.image_base + 0x1000);
				const uint64_t arguments[] = {17, 23};
				check(process.call(image.image_base + 0x1000, arguments, timeout).stop == raven::NativeStop::api,
					"dynamic image fixture did not reach its pending API");
			}
			const auto before = process.context();
			// This memfd is created after the child, so inherited descriptors cannot satisfy the mapping.
			auto dynamic_bytes = fixture({0x89, 0x11, 0x8b, 0x01, 0xc3});
			put(dynamic_bytes, 0xb0, 0x18a000000ULL, 8);
			dynamic_bytes[0x240] = 0xcc;
			dynamic_bytes[0x241] = 0xc3;
			const raven::PEImage dynamic_image(dynamic_bytes);
			raven::LoadedImage dynamic(dynamic_image, dynamic_image.image_base);
			rejected([&] { process.map_image(loaded); });
			process.map_image(dynamic);
			rejected([&] { process.map_image(dynamic); });
			rejected([&] { process.unmap_image(image.image_base); });
			rejected([&] { process.unmap_image(dynamic.base() + 4096); });
			rejected([&] { process.unmap_memory(dynamic.base()); });
			rejected([&] { process.protect_memory(dynamic.base(), 4096, PROT_READ | PROT_WRITE); });
			rejected([&] { process.write_memory(dynamic.base() + 0x1000, std::vector<uint8_t>{0}); });
			rejected([&] { process.read_memory(dynamic.base() + 0x2000, 1); });
			check(process.read_memory(dynamic.base() + 0x3200, 4) == std::vector<uint8_t>(4),
				"dynamic image BSS is not zero-filled");
			put(dynamic.data(), 0x3003, 0x12345678);
			check(value(process.read_memory(dynamic.base() + 0x3003, 4)) == 0x12345678,
				"dynamic image does not share parent storage");
			const auto after = process.context();
			check(std::memcmp(&after.registers, &before.registers, sizeof(user_regs_struct)) == 0 &&
				std::equal(before.xstate.begin(), before.xstate.begin() + 416, after.xstate.begin()),
				"dynamic image mapping changed caller context");
			const uint64_t arguments[] = {dynamic.base() + 0x3003, 0x76543210};
			const auto result = pending ? process.callback(dynamic.base() + 0x1000, arguments, timeout) :
				process.call(dynamic.base() + 0x1000, arguments, timeout);
			check(result.stop == raven::NativeStop::returned && result.value == 0x76543210 &&
				value(dynamic.data().subspan(0x3003, 4)) == 0x76543210,
				"newly mapped guest image did not execute or share its writes");
			process.add_api_trap(dynamic.base() + 0x1040);
			process.unmap_image(dynamic.base());
			rejected([&] { process.read_memory(dynamic.base(), 1); });
			rejected([&] { process.unmap_image(dynamic.base()); });
			if (pending) {
				check(process.api_arguments(2) == std::vector<uint64_t>{17, 23},
					"dynamic image operations lost pending API arguments");
				process.return_api(88);
				check(process.resume(timeout).value == 88, "dynamic image operations lost API return");
			}
			process.map_image(dynamic);
			const auto trap = process.call(dynamic.base() + 0x1040, {}, timeout);
			check(trap.stop == raven::NativeStop::fault && trap.signal == SIGTRAP,
				"unmapped image retained an API trap after remapping");
			check(process.resume(timeout).stop == raven::NativeStop::returned, "remapped image trap could not resume");
			process.unmap_image(dynamic.base());
			const auto collision = process.map_memory(dynamic.data().size(), PROT_READ | PROT_WRITE, dynamic.base());
			rejected([&] { process.map_image(dynamic); });
			check(process.read_memory(collision, 4) == std::vector<uint8_t>(4),
				"failed image mapping replaced an existing allocation");
			process.unmap_memory(collision);
			process.map_image(dynamic);
			process.unmap_image(dynamic.base());
			check(descriptors() == descriptor_count, "dynamic image mapping leaked child descriptors");
		}
		no_children();
	}
	std::cout << "Post-start shared image mappings, permissions, collisions, release and pending API context passed\n";
}

static void callback_contracts(const std::string &bootstrap)
{
	const auto timeout = std::chrono::milliseconds(2000);
	auto bytes = fixture({0xcc, 0xc3});
	bytes[0x280] = 0xcc;
	bytes[0x281] = 0xc3;
	const std::vector<uint8_t> sum{0x48, 0x89, 0xc8, 0x48, 0x01, 0xd0, 0x4c, 0x01, 0xc0, 0x4c, 0x01, 0xc8,
		0x48, 0x03, 0x44, 0x24, 40, 0x48, 0x03, 0x44, 0x24, 48, 0xc3};
	std::copy(sum.begin(), sum.end(), bytes.begin() + 0x240);
	{
		const raven::PEImage image(bytes);
		raven::LoadedImage loaded(image, image.image_base);
		raven::LoadedImage *images[] = {&loaded};
		raven::NativeProcess process(images, bootstrap, timeout);
		process.add_api_trap(image.image_base + 0x1000);
		process.add_api_trap(image.image_base + 0x1080);
		const uint64_t outer_args[] = {1, 2, 3, 4, 5, 6};
		const uint64_t inner_args[] = {10, 20, 30, 40, 50, 60};
		const uint64_t leaf_args[] = {100, 200, 300, 400, 500, 600};
		rejected([&] { process.callback(image.image_base + 0x1040, leaf_args, timeout); });
		check(process.call(image.image_base + 0x1000, outer_args, timeout).stop == raven::NativeStop::api, "outer API did not stop");
		const auto outer = process.context();
		rejected([&] { process.callback(image.image_base + 0x3000, leaf_args, timeout); });
		rejected([&] { process.callback(image.image_base + 0x1040, leaf_args, std::chrono::milliseconds(0)); });
		check(process.context().registers.rsp == outer.registers.rsp && process.api_arguments(6) ==
			std::vector<uint64_t>{1, 2, 3, 4, 5, 6}, "rejected callback changed its pending caller");
		auto result = process.callback(image.image_base + 0x1080, inner_args, timeout);
		check(result.stop == raven::NativeStop::api && process.api_arguments(6) == std::vector<uint64_t>{10, 20, 30, 40, 50, 60},
			"callback API arguments differ");
		const auto inner = process.context();
		check(inner.registers.rsp < outer.registers.rsp && inner.registers.rsp % 16 == 8, "callback did not use a separate aligned stack frame");
		const auto allocation = process.map_memory(4096, PROT_READ | PROT_WRITE);
		process.protect_memory(allocation, 4096, PROT_READ);
		process.unmap_memory(allocation);
		check(process.context().registers.rsp == inner.registers.rsp && process.api_arguments(6) ==
			std::vector<uint64_t>{10, 20, 30, 40, 50, 60}, "memory operation lost a nested callback caller");
		result = process.callback(image.image_base + 0x1040, leaf_args, timeout);
		check(result.stop == raven::NativeStop::returned && result.value == 2100 &&
			process.context().registers.rsp == inner.registers.rsp && process.api_arguments(6) ==
			std::vector<uint64_t>{10, 20, 30, 40, 50, 60}, "nested callback did not restore its immediate API caller");
		process.return_api(211);
		result = process.resume(timeout);
		check(result.stop == raven::NativeStop::returned && result.value == 211 && process.context().registers.rsp == outer.registers.rsp &&
			process.api_arguments(6) == std::vector<uint64_t>{1, 2, 3, 4, 5, 6}, "callback return lost its outer API caller");
		process.return_api(232);
		result = process.resume(timeout);
		check(result.stop == raven::NativeStop::returned && result.value == 232, "outer entry did not complete after nested callbacks");
	}
	no_children();
	// A callback preserves nonvolatile registers itself; the runtime restores the suspended API's integer context.
	bytes = fixture({0xcc, 0xc3});
	const std::vector<uint8_t> clobber{0x41, 0x54, 0x48, 0x83, 0xec, 16, 0xf3, 0x44, 0x0f, 0x7f, 0x3c, 0x24,
		0x49, 0xc7, 0xc4, 99, 0, 0, 0, 0x66, 0x45, 0x0f, 0xef, 0xff,
		0xf3, 0x44, 0x0f, 0x6f, 0x3c, 0x24, 0x48, 0x83, 0xc4, 16, 0x41, 0x5c, 0xb8, 42, 0, 0, 0, 0xc3};
	std::copy(clobber.begin(), clobber.end(), bytes.begin() + 0x240);
	{
		const raven::PEImage image(bytes);
		raven::LoadedImage loaded(image, image.image_base);
		raven::LoadedImage *images[] = {&loaded};
		raven::NativeProcess process(images, bootstrap, timeout);
		process.add_api_trap(image.image_base + 0x1000);
		auto initial = process.context();
		initial.registers.r12 = 0x12345678;
		const size_t xmm15 = offsetof(user_fpregs_struct, xmm_space) + 15 * 16;
		put(initial.xstate, xmm15, 0x12345678, 8);
		put(initial.xstate, 512, value(std::span<const uint8_t>(initial.xstate).subspan(512, 8)) | 2, 8);
		process.set_context(initial);
		check(process.call(image.image_base + 0x1000, {}, timeout).stop == raven::NativeStop::api, "state fixture API did not stop");
		const auto saved = process.context();
		const auto returned = process.callback(image.image_base + 0x1040, {}, timeout);
		const auto restored = process.context();
		check(returned.stop == raven::NativeStop::returned && returned.value == 42 && restored.registers.r12 == saved.registers.r12 &&
			restored.registers.rip == saved.registers.rip && restored.registers.rsp == saved.registers.rsp &&
			std::equal(saved.xstate.begin(), saved.xstate.begin() + 416, restored.xstate.begin()), "callback did not restore integer/x87/XMM caller state");
		process.return_api(42);
		check(process.resume(timeout).value == 42, "restored API did not finish");
	}
	no_children();
	bytes = fixture({0xcc, 0xc3});
	{
		const raven::PEImage image(bytes);
		raven::LoadedImage loaded(image, image.image_base);
		raven::LoadedImage *images[] = {&loaded};
		raven::NativeProcess process(images, bootstrap, timeout);
		process.add_api_trap(image.image_base + 0x1000);
		check(process.call(image.image_base + 0x1000, {}, timeout).stop == raven::NativeStop::api, "depth fixture API did not stop");
		for (size_t depth = 0; depth < 64; ++depth)
			check(process.callback(image.image_base + 0x1000, {}, timeout).stop == raven::NativeStop::api, "nested depth fixture did not stop");
		rejected([&] { process.callback(image.image_base + 0x1000, {}, timeout); });
		for (size_t depth = 0; depth < 64; ++depth) {
			process.return_api(depth);
			const auto returned = process.resume(timeout);
			check(returned.stop == raven::NativeStop::returned && returned.value == depth, "depth unwind lost callback value");
		}
		process.return_api(64);
		check(process.resume(timeout).value == 64, "depth unwind did not restore the root API");
	}
	no_children();
	for (bool infinite : {false, true}) {
		bytes = fixture({0xcc, 0xc3});
		bytes[0x240] = infinite ? 0xeb : 0x0f;
		bytes[0x241] = infinite ? 0xfe : 0x0b;
		bytes[0x242] = 0xc3;
		{
			const raven::PEImage image(bytes);
			raven::LoadedImage loaded(image, image.image_base);
			raven::LoadedImage *images[] = {&loaded};
			raven::NativeProcess process(images, bootstrap, timeout);
			process.add_api_trap(image.image_base + 0x1000);
			check(process.call(image.image_base + 0x1000, {}, timeout).stop == raven::NativeStop::api, "negative callback fixture API did not stop");
			const auto stopped = process.callback(image.image_base + 0x1040, {}, std::chrono::milliseconds(100));
			if (infinite) {
				check(stopped.stop == raven::NativeStop::timeout, "callback infinite loop did not terminate");
				rejected([&] { process.return_api(0); });
			} else {
				check(stopped.stop == raven::NativeStop::fault && stopped.signal == SIGILL, "callback fault was not reported");
				auto repaired = process.context();
				repaired.registers.rip += 2;
				process.set_context(repaired);
				check(process.resume(timeout).stop == raven::NativeStop::returned, "repaired callback did not return");
				process.return_api(1);
				check(process.resume(timeout).value == 1, "callback fault repair lost the root API");
			}
		}
		no_children();
	}
	bytes = fixture({0xcc, 0xc3});
	const std::vector<uint8_t> exit_call{0xb8, 60, 0, 0, 0, 0x31, 0xff, 0x0f, 0x05, 0xc3};
	std::copy(exit_call.begin(), exit_call.end(), bytes.begin() + 0x240);
	{
		const raven::PEImage image(bytes);
		raven::LoadedImage loaded(image, image.image_base);
		raven::LoadedImage *images[] = {&loaded};
		raven::NativeProcess process(images, bootstrap, timeout);
		process.add_api_trap(image.image_base + 0x1000);
		const uint64_t arguments[] = {123};
		check(process.call(image.image_base + 0x1000, arguments, timeout).stop == raven::NativeStop::api, "syscall callback root did not stop");
		const auto saved = process.context();
		const auto stopped = process.callback(image.image_base + 0x1040, {}, timeout);
		check(stopped.stop == raven::NativeStop::syscall && stopped.value == 60, "callback executed Linux exit instead of stopping");
		rejected([&] { process.return_api(0); });
		auto response = process.context();
		response.registers.rax = 73;
		response.registers.orig_rax = uint64_t(-1);
		process.set_context(response);
		const auto returned = process.resume(timeout);
		check(returned.stop == raven::NativeStop::returned && returned.value == 73 &&
			process.context().registers.rsp == saved.registers.rsp && process.api_arguments(1)[0] == 123,
			"syscall callback completion lost its pending caller");
		process.return_api(returned.value);
		check(process.resume(timeout).value == 73, "syscall callback root did not finish");
	}
	no_children();
	std::cout << "Nested callbacks, separate stack frames, caller restoration, depth limit, syscall containment, repaired faults and terminal timeout passed\n";
}

static void persistent_contracts(const std::string &bootstrap)
{
	const auto timeout = std::chrono::milliseconds(2000);
	auto bytes = fixture({0xff, 0x05, 0xfa, 0x1f, 0, 0, 0x8b, 0x05, 0xf4, 0x1f, 0, 0, 0xc3});
	{
		const raven::PEImage image(bytes);
		raven::LoadedImage loaded(image, image.image_base);
		raven::LoadedImage *images[] = {&loaded};
		raven::NativeProcess process(images, bootstrap, timeout);
		const auto first = process.call(image.image_base + image.entry, {}, timeout);
		const auto second = process.call(image.image_base + image.entry, {}, timeout);
		check(first.stop == raven::NativeStop::returned && first.value == 1 &&
			second.stop == raven::NativeStop::returned && second.value == 2, "persistent calls lost guest memory");
		const std::vector<uint8_t> patch{1, 2, 3, 4, 5, 6, 7, 8, 9};
		process.write_memory(image.image_base + 0x30ff, patch);
		check(process.read_memory(image.image_base + 0x30ff, patch.size()) == patch &&
			loaded.data()[0x30fe] == 0 && loaded.data()[0x3108] == 0, "unaligned guest memory access changed adjacent bytes");
		rejected([&] { process.read_memory(image.image_base + 0x2000, 1); });
		rejected([&] { process.read_memory(UINT64_MAX, 2); });
		rejected([&] { process.read_memory(image.image_base + 0x1fff, 2); });
		rejected([&] { process.write_memory(image.image_base, patch); });
		rejected([&] { process.write_memory(image.image_base + 0x3fff, patch); });
		check(loaded.data()[0x3fff] == 0, "rejected cross-boundary write changed memory");
		rejected([&] { process.call(image.image_base + 0x3000, {}, timeout); });
		rejected([&] { process.call(image.image_base + 0x1000, {}, std::chrono::milliseconds(0)); });
		const std::vector<uint64_t> excessive(129);
		rejected([&] { process.call(image.image_base + 0x1000, excessive, timeout); });
		rejected([&] { process.resume(timeout); });
		rejected([&] { process.return_api(0); });
		const auto saved = process.context();
		auto invalid = saved;
		invalid.xstate.resize(1);
		rejected([&] { process.set_context(invalid); });
		check(process.context().registers.rip == saved.registers.rip, "invalid XSTATE altered integer context");
	}
	no_children();
	for (uint8_t segment : {uint8_t(0x64), uint8_t(0x65)}) {
		bytes = fixture({segment, 0x48, 0x8b, 0x04, 0x25, 0x30, 0, 0, 0, 0xc3});
		const raven::PEImage image(bytes);
		raven::LoadedImage loaded(image, image.image_base);
		const uint64_t marker = 0x123456789abcdef0ULL;
		put(loaded.data(), 0x3030, marker, 8);
		raven::LoadedImage *images[] = {&loaded};
		raven::NativeProcess process(images, bootstrap, timeout);
		auto context = process.context();
		if (segment == 0x64)
			context.registers.fs_base = image.image_base + 0x3000;
		else
			context.registers.gs_base = image.image_base + 0x3000;
		process.set_context(context);
		const auto result = process.call(image.image_base + 0x1000, {}, timeout);
		check(result.stop == raven::NativeStop::returned && result.value == marker, "native call lost its FS/GS address base");
	}
	no_children();
	// Keep this trap off the RET fast path: the generic completion must validate its return stack before answering.
	bytes = fixture({0xcc, 0x90, 0xc3});
	{
		const raven::PEImage image(bytes);
		raven::LoadedImage loaded(image, image.image_base);
		raven::LoadedImage *images[] = {&loaded};
		raven::NativeProcess process(images, bootstrap, timeout);
		process.add_api_trap(image.image_base + 0x1000);
		rejected([&] { process.add_api_trap(image.image_base + 0x1001); });
		rejected([&] { process.add_api_trap(image.image_base + 0x3000); });
		const uint64_t arguments[] = {1, 2, 3, 4, 5, 6};
		auto result = process.call(image.image_base + 0x1000, arguments, timeout);
		check(result.stop == raven::NativeStop::api && result.value == image.image_base + 0x1000 &&
			process.api_arguments(6) == std::vector<uint64_t>{1, 2, 3, 4, 5, 6}, "API register or stack arguments differ");
		check(process.context().registers.rsp % 16 == 8, "API entry stack alignment differs");
		rejected([&] { process.resume(timeout); });
		rejected([&] { process.call(image.image_base + 0x1000, {}, timeout); });
		rejected([&] { process.api_arguments(129); });
		const auto saved = process.context();
		auto bad_stack = saved;
		bad_stack.registers.rsp = 0;
		process.set_context(bad_stack);
		rejected([&] { process.return_api(21); });
		process.set_context(saved);
		process.return_api(21);
		rejected([&] { process.return_api(21); });
		result = process.resume(timeout);
		check(result.stop == raven::NativeStop::returned && result.value == 21, "API completion did not return to the guest caller");
		result = process.call(image.image_base + 0x1000, arguments, timeout);
		check(result.stop == raven::NativeStop::api, "second API call lost its registered trap");
		process.return_api(33);
		result = process.resume(timeout);
		check(result.stop == raven::NativeStop::returned && result.value == 33, "second API completion retained a pending response");
	}
	no_children();
	bytes = fixture({0xcc, 0xc3});
	bytes[0x280] = 0xcc;
	{
		const raven::PEImage image(bytes);
		raven::LoadedImage loaded(image, image.image_base);
		raven::LoadedImage *images[] = {&loaded};
		raven::NativeProcess process(images, bootstrap, timeout);
		process.add_api_trap(image.image_base + 0x1080);
		const auto result = process.call(image.image_base + 0x1000, {}, timeout);
		check(result.stop == raven::NativeStop::fault && result.signal == SIGTRAP,
			"registered API elsewhere consumed an unrelated guest breakpoint");
		rejected([&] { process.return_api(0); });
	}
	no_children();
	bytes = fixture({0x48, 0x83, 0xec, 40, 0xb9, 7, 0, 0, 0, 0xe8, 0x72, 0, 0, 0,
		0x48, 0x83, 0xc4, 40, 0x83, 0xc0, 1, 0xc3});
	bytes[0x280] = 0xcc;
	{
		const raven::PEImage image(bytes);
		raven::LoadedImage loaded(image, image.image_base);
		raven::LoadedImage *images[] = {&loaded};
		raven::NativeProcess process(images, bootstrap, timeout);
		process.add_api_trap(image.image_base + 0x1080);
		auto result = process.call(image.image_base + 0x1000, {}, timeout);
		check(result.stop == raven::NativeStop::api && process.api_arguments(1)[0] == 7, "guest CALL did not reach its API");
		process.return_api(10);
		result = process.resume(timeout);
		check(result.stop == raven::NativeStop::returned && result.value == 11, "guest did not continue its instructions after API completion");
	}
	no_children();
	// FLD1, CALL trap, MOVQ RAX,XMM15, FISTP into shared BSS, RET.
	bytes = fixture({0xd9, 0xe8, 0x48, 0x83, 0xec, 40, 0xe8, 0x75, 0, 0, 0,
		0x48, 0x83, 0xc4, 40, 0x66, 0x4c, 0x0f, 0x7e, 0xf8, 0xdf, 0x3d, 0xe6, 0x1f, 0, 0, 0xc3});
	bytes[0x280] = 0xcc;
	{
		const raven::PEImage image(bytes);
		raven::LoadedImage loaded(image, image.image_base);
		raven::LoadedImage *images[] = {&loaded};
		raven::NativeProcess process(images, bootstrap, timeout);
		process.add_api_trap(image.image_base + 0x1080);
		auto context = process.context();
		const uint64_t marker = 0x123456789abcdef0ULL;
		context.registers.r12 = marker;
		const size_t xmm15 = offsetof(user_fpregs_struct, xmm_space) + 15 * 16;
		check(context.xstate.size() >= 576, "host XSTATE lacks its legacy/header regions");
		put(context.xstate, xmm15, marker, 8);
		put(context.xstate, 512, value(std::span<const uint8_t>(context.xstate).subspan(512, 8)) | 2, 8);
		process.set_context(context);
		auto result = process.call(image.image_base + 0x1000, {}, timeout);
		check(result.stop == raven::NativeStop::api, "floating-state fixture did not reach its trap");
		const auto stopped = process.context();
		process.set_context(stopped);
		process.return_api(10);
		result = process.resume(timeout);
		check(result.stop == raven::NativeStop::returned && result.value == marker &&
			value(std::span<const uint8_t>(loaded.data()).subspan(0x3000, 8)) == 1 &&
			process.context().registers.r12 == marker, "XMM15, x87 or nonvolatile GPR state was lost across an API stop");
	}
	no_children();
	bytes = fixture({0xb8, 60, 0, 0, 0, 0x0f, 0x05, 0x48, 0xff, 0xc0, 0xc3});
	{
		const raven::PEImage image(bytes);
		raven::LoadedImage loaded(image, image.image_base);
		raven::LoadedImage *images[] = {&loaded};
		raven::NativeProcess process(images, bootstrap, timeout);
		auto result = process.call(image.image_base + 0x1000, {}, timeout);
		check(result.stop == raven::NativeStop::syscall && result.value == 60, "persistent syscall did not stop");
		auto context = process.context();
		context.registers.rax = 41;
		context.registers.orig_rax = uint64_t(-1);
		process.set_context(context);
		result = process.resume(timeout);
		check(result.stop == raven::NativeStop::returned && result.value == 42, "syscall completion executed Linux exit or lost its result");
	}
	no_children();
	bytes = fixture({0x0f, 0x0b, 0xc3});
	{
		const raven::PEImage image(bytes);
		raven::LoadedImage loaded(image, image.image_base);
		raven::LoadedImage *images[] = {&loaded};
		raven::NativeProcess process(images, bootstrap, timeout);
		const auto fault = process.call(image.image_base + 0x1000, {}, timeout);
		check(fault.stop == raven::NativeStop::fault && fault.signal == SIGILL && fault.fault_address == image.image_base + 0x1000,
			"native fault address is missing");
		auto context = process.context();
		context.registers.rip += 2;
		process.set_context(context);
		check(process.resume(timeout).stop == raven::NativeStop::returned, "fault context could not resume after repair");
	}
	no_children();
	bytes = fixture({0xeb, 0xfe});
	{
		const raven::PEImage image(bytes);
		raven::LoadedImage loaded(image, image.image_base);
		raven::LoadedImage *images[] = {&loaded};
		raven::NativeProcess process(images, bootstrap, timeout);
		check(process.call(image.image_base + 0x1000, {}, std::chrono::milliseconds(100)).stop == raven::NativeStop::timeout,
			"persistent infinite loop did not time out");
		rejected([&] { process.context(); });
		rejected([&] { process.resume(timeout); });
	}
	no_children();
	std::cout << "Persistent calls, registered API stops/resumption, XSTATE, guest memory, repaired faults and terminal timeouts passed\n";
}

int main(int argc, char **argv)
{
	try {
		check(argc == 2 || (argc == 3 && (std::string(argv[2]) == "--memory" || std::string(argv[2]) == "--parallel-stops")),
			"bootstrap path required");
		const std::string bootstrap = argv[1];
		if (argc == 3) {
			if (std::string(argv[2]) == "--parallel-stops") parallel_stop_contracts(bootstrap);
			else memory_contracts(bootstrap);
			no_children();
			std::cout << "Native " << (std::string(argv[2]) == "--parallel-stops" ? "parallel stop" : "memory") << " contracts passed\n";
			return 0;
		}
		const auto run = [&](std::vector<uint8_t> &bytes, std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
			const raven::PEImage image(bytes);
			raven::LoadedImage loaded(image, image.image_base);
			const auto result = raven::run_native_entry(image, loaded, bootstrap, timeout);
			no_children();
			return result;
		};
		auto bytes = fixture({0xb8, 42, 0, 0, 0, 0xc3});
		auto result = run(bytes);
		check(result.stop == raven::NativeStop::returned && result.value == 42, "original guest instructions did not return 42");
		// Windows x64 function entry has RSP mod 16 == 8 and 32 writable shadow bytes.
		bytes = fixture({0x48, 0x89, 0xe0, 0x83, 0xe0, 15, 0x48, 0xc7, 0x44, 0x24, 32, 1, 0, 0, 0, 0xc3});
		result = run(bytes);
		check(result.stop == raven::NativeStop::returned && result.value == 8, "Windows x64 stack contract differs");
		bytes = fixture({0xc7, 0x05, 0xf6, 0x1f, 0, 0, 0x52, 0, 0, 0, 0xc3});
		{
			const raven::PEImage image(bytes);
			raven::LoadedImage loaded(image, image.image_base);
			result = raven::run_native_entry(image, loaded, bootstrap, std::chrono::milliseconds(2000));
			check(result.stop == raven::NativeStop::returned && loaded.data()[0x3000] == 0x52,
				"guest write was not visible through shared image backing");
			const int descriptor_flags = fcntl(loaded.descriptor(), F_GETFD);
			check(descriptor_flags >= 0 && (descriptor_flags & FD_CLOEXEC) != 0,
				"native child changed the parent's descriptor inheritance");
		}
		no_children();
		// Writes into read-only headers, execution in data, reads in image holes and UD2.
		for (auto fault : {
			fixture({0xc7, 0x05, 0x36, 0xf0, 0xff, 0xff, 0x52, 0, 0, 0, 0xc3}),
			fixture({0xe9, 0xfb, 0x1f, 0, 0}),
			fixture({0x8b, 0x05, 0xfa, 15, 0, 0, 0xc3})}) {
			result = run(fault);
			check(result.stop == raven::NativeStop::fault && result.signal == SIGSEGV, "hardware image protections were not enforced");
		}
		bytes = fixture({0x0f, 0x0b});
		result = run(bytes);
		check(result.stop == raven::NativeStop::fault && result.signal == SIGILL, "invalid instruction was not reported");
		bytes = fixture({0xcc, 0xc3});
		result = run(bytes);
		check(result.stop == raven::NativeStop::fault && result.signal == SIGTRAP, "guest INT3 was mistaken for normal return");
		bytes = fixture({0xe8, 0xfb, 0xff, 0xff, 0xff});
		result = run(bytes);
		check(result.stop == raven::NativeStop::fault && result.signal == SIGSEGV, "stack overflow did not hit its guard");
		bytes = fixture({0x48, 0x8d, 0x44, 0x24, 8, 0xc6, 0, 0xc3, 0xff, 0xe0});
		result = run(bytes);
		check(result.stop == raven::NativeStop::fault && result.signal == SIGSEGV, "native stack was executable");
		// Linux exit would terminate the child if executed. An unavailable compatibility gate faults before syscall entry instead.
		for (auto call : {fixture({0xb8, 60, 0, 0, 0, 0x31, 0xff, 0x0f, 0x05, 0xc3}),
			fixture({0xb8, 1, 0, 0, 0, 0x31, 0xdb, 0xcd, 0x80, 0xc3})}) {
			result = run(call);
			const bool rejected_gate = call[0x207] == 0xcd && result.stop == raven::NativeStop::fault && result.signal == SIGSEGV &&
				result.signal_code == SI_KERNEL && result.instruction == raven::PEImage(call).image_base + 0x1007;
			check((result.stop == raven::NativeStop::syscall && (result.value == 60 || result.value == 1)) || rejected_gate,
				"guest syscall escaped the runtime boundary");
		}
		bytes = fixture({0xb8, 1, 0, 0, 0, 0x31, 0xdb, 0x0f, 0x34, 0xc3});
		result = run(bytes);
		check((result.stop == raven::NativeStop::syscall && result.value == 1) ||
			(result.stop == raven::NativeStop::fault && (result.signal == SIGSEGV || result.signal == SIGILL)),
			"SYSENTER neither stopped nor faulted inside the boundary");
		// Pwrite would change shared backing if the stopped host syscall were executed.
		bytes = fixture({0xb8, 18, 0, 0, 0, 0xbf, 0, 0, 0, 0, 0x48, 0xbe, 0, 0, 0, 0, 0, 0, 0, 0,
			0xba, 1, 0, 0, 0, 0x49, 0xc7, 0xc2, 0, 0x30, 0, 0, 0x0f, 0x05, 0xc3});
		{
			const raven::PEImage image(bytes);
			raven::LoadedImage loaded(image, image.image_base);
			put(loaded.data(), 0x1006, static_cast<uint64_t>(loaded.descriptor()));
			put(loaded.data(), 0x100c, image.image_base + 0x3100, 8);
			loaded.data()[0x3100] = 0x52;
			result = raven::run_native_entry(image, loaded, bootstrap, std::chrono::milliseconds(2000));
			check(result.stop == raven::NativeStop::syscall && result.value == 18 && loaded.data()[0x3000] == 0,
				"stopped syscall modified shared backing");
			const uint8_t control = 0x52;
			check(pwrite(loaded.descriptor(), &control, 1, 0x3000) == 1 && loaded.data()[0x3000] == control,
				"syscall negative control did not target writable shared backing");
		}
		no_children();
		bytes = fixture({0xeb, 0xfe});
		result = run(bytes, std::chrono::milliseconds(100));
		check(result.stop == raven::NativeStop::timeout, "infinite guest loop was not bounded");
		bytes = fixture({0xc3});
		{
			const raven::PEImage image(bytes);
			raven::LoadedImage loaded(image, image.image_base);
			rejected([&] { raven::run_native_entry(image, loaded, bootstrap + ".missing", std::chrono::milliseconds(2000)); });
			rejected([&] { raven::run_native_entry(image, loaded, bootstrap, std::chrono::milliseconds(0)); });
		}
		no_children();
		put(bytes, 0xb0, 0x700000000000ULL, 8);
		rejected([&] { run(bytes); });
		no_children();
		bytes = fixture({0xc3});
		put(bytes, 0x110, 0x3000);
		put(bytes, 0x114, 40);
		put(bytes, 0x400, 0x3080);
		put(bytes, 0x40c, 0x30c0);
		put(bytes, 0x410, 0x30a0);
		put(bytes, 0x480, (uint64_t(1) << 63) | 1, 8);
		const std::string module = "kernel32.dll";
		std::copy(module.begin(), module.end(), bytes.begin() + 0x4c0);
		rejected([&] { run(bytes); });
		bytes = fixture({0xc3});
		put(bytes, 0x96, 0x2002, 2);
		rejected([&] { run(bytes); });
		for (size_t directory : {size_t(9), size_t(13), size_t(14)}) {
			bytes = fixture({0xc3});
			put(bytes, 0x108 + directory * 8, 0x3000);
			put(bytes, 0x10c + directory * 8, 40);
			rejected([&] { run(bytes); });
		}
		bytes = fixture({0xc3}, false);
		rejected([&] { run(bytes); });
		bytes = fixture({0xc3});
		put(bytes, 0xa8, 0x3000);
		rejected([&] { run(bytes); });
		for (size_t initialization : {size_t(0), size_t(9), size_t(13), size_t(14)}) {
			raven::ModuleSet modules;
			auto &entry = modules.add("main.exe", fixture({0xc3}), 0x140000000ULL);
			auto dependency = fixture({0xc3});
			put(dependency, 0xb0, 0x141000000ULL, 8);
			put(dependency, 0x96, 0x2002, 2);
			if (initialization) {
				put(dependency, 0xa8, 0);
				put(dependency, 0x108 + initialization * 8, 0x3000);
				put(dependency, 0x10c + initialization * 8, 40);
			}
			modules.add("dependency.dll", dependency, 0x141000000ULL);
			rejected([&] { raven::run_native_modules(modules, entry, bootstrap, std::chrono::milliseconds(2000)); });
		}
		{
			raven::ModuleSet empty;
			raven::PEModule foreign("main.exe", fixture({0xc3}), 0x140000000ULL);
			rejected([&] { raven::run_native_modules(empty, foreign, bootstrap, std::chrono::milliseconds(2000)); });
		}
		no_children();
		persistent_contracts(bootstrap);
		callback_contracts(bootstrap);
		memory_contracts(bootstrap);
		image_mapping_contracts(bootstrap);
		std::cout << "Native PE64 instructions, shared writes, stack, protections, syscall containment, faults and cleanup passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
