#include "providers.h"
#include "system_exports.h"
#include "loader.h"
#include "memory.h"
#include "threads.h"
#include "cpu/served_queue.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

static void check(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

static void rejected(const std::function<void()> &operation)
{
	try { operation(); } catch (const std::runtime_error &) { return; }
	throw std::runtime_error("unsupported native provider operation was accepted");
}

static void apc_wait_result(const std::string &bootstrap)
{
	const uint64_t base = 0x180000000ULL;
	const std::array<std::string_view, 1> names{"GetCurrentProcess"};
	const auto bytes = raven::runtime_provider("kernel32.dll", base, names);
	const raven::PEImage image(bytes);
	raven::LoadedImage loaded(image, base);
	raven::LoadedImage *images[] = {&loaded};
	raven::NativeProcess process(images, bootstrap, std::chrono::seconds(2));
	raven::NativeEnvironment environment(process, base, 73, 1);
	raven::NativeMemory memory(process, environment);
	raven::NativeThreads threads(process, environment, memory, nullptr, std::chrono::steady_clock::now() + std::chrono::seconds(1));
	threads.current().apcs.push_back({0x1234, 7});
	unsigned probes = 0;
	const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
	bool ready = false;
	do {
		ready = threads.block([&] {
			check(++probes <= 20, "pending APC repeatedly resumed a non-alertable wait");
			return false;
		}, until);
	} while (!ready && std::chrono::steady_clock::now() < until);
	check(!ready && threads.current().apcs.size() == 1, "non-alertable wait consumed its APC or ended ready");
	threads.current().apcs.clear();
}

static void served_thread_result(const std::string &bootstrap)
{
	const auto timeout = std::chrono::seconds(2);
	const uint64_t base = 0x180000000ULL;
	const std::array<std::string_view, 1> names{"vkCmdDrawIndexed"};
	const auto bytes = raven::runtime_provider("vulkan-1.dll", base, names);
	const raven::PEImage image(bytes);
	raven::LoadedImage loaded(image, base);
	raven::LoadedImage *images[] = {&loaded};
	raven::NativeProcess process(images, bootstrap, timeout);
	const uint64_t stub = base + image.exports().at(0).address;
	const int file = memfd_create("served-thread-test", MFD_CLOEXEC);
	check(file >= 0 && ftruncate(file, 4096) == 0, "cannot create served test block");
	auto *block = static_cast<uint64_t *>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, file, 0));
	check(block != MAP_FAILED, "cannot map served test block");
	const uint64_t guest = process.map_shared(file, 4096);
	close(file);
	const auto teb = process.map_memory(8192, PROT_READ | PROT_WRITE);
	auto context = process.context();
	context.registers.gs_base = teb;
	process.set_context(context);
	int32_t displacement = 0;
	std::memcpy(&displacement, loaded.data().data() + stub - base + 3, 4);
	std::memcpy(loaded.data().data() + stub - base + 7 + displacement, &guest, 8);
	const auto other_stack = process.map_memory(65536, PROT_READ | PROT_WRITE);
	const auto other_teb = process.map_memory(8192, PROT_READ | PROT_WRITE);
	const auto other = process.new_thread(other_stack + 65536, other_teb);
	unsigned calls = 0, switches = 0;
	bool nested = false;
	process.set_served_calls(block, {stub}, 112, 85, {stub + 109}, [&](uint64_t address) {
		check(address == stub, "served call used another stub");
		check(process.serving_teb() == (nested ? other_teb : teb), "served call lost its current TEB");
		const auto words = process.api_arguments(6);
		uint64_t sum = 0;
		for (size_t index = 0; index < words.size(); ++index) {
			check(words[index] == (nested ? 200 : 100) + index, "thread switch changed served arguments");
			sum += words[index];
		}
		++calls;
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		process.return_api(sum);
	});
	process.set_time_slicing(std::chrono::milliseconds(1), [&] { return !nested; }, [&] {
		check(process.context().registers.rip == stub + 110, "served thread yielded before its result was private");
		const auto saved = process.save_thread();
		nested = true;
		process.load_thread(other);
		const uint64_t arguments[] = {200, 201, 202, 203, 204, 205};
		const auto result = process.call(stub, arguments, timeout);
		check(result.stop == raven::NativeStop::returned && result.value == 1215, "second served thread lost its result");
		process.load_thread(saved);
		nested = false;
		++switches;
	});
	const uint64_t arguments[] = {100, 101, 102, 103, 104, 105};
	const auto result = process.call(stub, arguments, timeout);
	check(result.stop == raven::NativeStop::returned && result.value == 615 && calls == 2 && switches == 1,
		"served thread result was overwritten across the yield");
	process.set_time_slicing(std::chrono::milliseconds(0), {}, {});
	const auto unsliced = process.call(stub, arguments, timeout);
	check(unsliced.stop == raven::NativeStop::returned && unsliced.value == 615 && calls == 3 && switches == 1 && !block[11],
		"served call retained a disabled yield request");
	process.set_served_calls(nullptr, {}, 0, 0, {}, {});
	check(!process.serving_teb(), "served TEB escaped its call");
	process.unmap_memory(other_stack);
	process.unmap_memory(other_teb);
	process.unmap_memory(teb);
	process.unmap_memory(guest);
	munmap(block, 4096);
}

static void parallel_served_result(const std::string &bootstrap)
{
	const auto timeout = std::chrono::seconds(2);
	const uint64_t base = 0x180000000ULL;
	const std::array<std::string_view, 1> names{"vkCmdDrawIndexed"};
	const raven::PEImage image(raven::runtime_provider("vulkan-1.dll", base, names));
	raven::LoadedImage loaded(image, base);
	raven::LoadedImage *images[] = {&loaded};
	raven::NativeProcess process(images, bootstrap, timeout);
	check(process.parallel(), "parallel served test needs separate execution tasks");
	const uint64_t stub = base + image.exports().at(0).address;
	const std::array<uint8_t, 12> local{0x65, 0x4c, 0x8b, 0x14, 0x25, 0xf0, 0x1f, 0, 0, 0x90, 0x90, 0x90};
	std::copy(local.begin(), local.end(), loaded.data().data() + stub - base);
	const auto teb = process.map_memory(8192, PROT_READ | PROT_WRITE);
	auto context = process.context();
	context.registers.gs_base = teb;
	process.set_context(context);
	raven::NativeThreadState other;
	bool nested = false;
	unsigned calls = 0;
	process.set_served_calls(nullptr, {stub}, 112, 85, {}, [&](uint64_t address) {
		check(address == stub, "parallel call used another stub");
		check(process.serving_teb() == (nested ? other.context.registers.gs_base : teb), "served call lost its host-recorded TEB");
		const auto words = process.api_arguments(6);
		uint64_t sum = 0;
		for (size_t i = 0; i < words.size(); ++i) {
			check(words[i] == (nested ? 200 : 100) + i, "parallel served arguments crossed threads");
			sum += words[i];
		}
		if (!nested) {
			const auto saved = process.save_thread();
			process.load_thread(other);
			nested = true;
			const uint64_t arguments[] = {200, 201, 202, 203, 204, 205};
			const auto result = process.call(stub, arguments, timeout);
			check(result.stop == raven::NativeStop::returned && result.value == 1215, "nested served result crossed threads");
			other = process.save_thread();
			process.load_thread(saved);
			nested = false;
			check(process.api_arguments(6) == words, "resumed served call lost its stack arguments");
		}
		++calls;
		process.return_api(sum);
	});
	const auto other_teb = process.map_memory(8192, PROT_READ | PROT_WRITE);
	const auto stack = process.map_memory(65536, PROT_READ | PROT_WRITE);
	other = process.new_thread(stack + 65536, other_teb);
	const auto original = process.save_thread();
	const auto mark = [&](raven::NativeContext &value, uint64_t marker) {
		check(value.xstate.size() >= 576, "thread context lacks XSTATE header");
		value.registers.r12 = marker;
		std::memcpy(value.xstate.data() + 400, &marker, 8);
		uint64_t present = 0;
		std::memcpy(&present, value.xstate.data() + 512, 8);
		present |= 2;
		std::memcpy(value.xstate.data() + 512, &present, 8);
	};
	const auto marked = [&](uint64_t marker, uint64_t expected_teb) {
		const auto value = process.context();
		uint64_t xmm15 = 0;
		std::memcpy(&xmm15, value.xstate.data() + 400, 8);
		check(value.registers.r12 == marker && xmm15 == marker && value.registers.gs_base == expected_teb,
			"resident task context lost GPR, SIMD or TEB state");
	};
	auto first = process.context();
	mark(first, 0x1234567812345678ULL);
	process.set_context(first);
	auto resident = process.save_thread(false);
	check(resident.context_in_task && resident.context.xstate.empty() && resident.context.registers.r12 == first.registers.r12,
		"scheduler snapshot copied XSTATE or lost its readable integer state");
	// The integer copy is diagnostic metadata; the Linux task owns the state restored by this scheduler snapshot.
	resident.context.registers.r12 = 0;
	process.load_thread(other);
	auto second = process.context();
	mark(second, 0xabcdef12abcdef12ULL);
	process.set_context(second);
	other = process.save_thread(false);
	for (unsigned index = 0; index < 32; ++index) {
		process.load_thread(resident);
		marked(first.registers.r12, teb);
		process.load_thread(other);
		marked(second.registers.r12, other_teb);
	}
	auto explicit_snapshot = process.save_thread();
	check(!explicit_snapshot.context_in_task && !explicit_snapshot.context.xstate.empty(), "explicit thread snapshot lost its register copy");
	mark(explicit_snapshot.context, 0x7654321076543210ULL);
	process.load_thread(explicit_snapshot);
	marked(explicit_snapshot.context.registers.r12, other_teb);
	other = process.save_thread();
	process.load_thread(original);
	process.set_execution_wait([](std::function<bool()> ready, auto limit) {
		while (!ready() && std::chrono::steady_clock::now() < limit)
			std::this_thread::yield();
	});
	const uint64_t arguments[] = {100, 101, 102, 103, 104, 105};
	for (unsigned i = 0; i < 64; ++i) {
		const auto result = process.call(stub, arguments, timeout);
		check(result.stop == raven::NativeStop::returned && result.value == 615, "outer served result crossed threads");
	}
	check(calls == 128, "parallel served call was lost or repeated");
	process.stop_thread(other);
	// A responsive served loop must give ready host work a turn even when its guest never reaches the wait trap.
	loaded.data()[stub - base + 85] = 0x90;
	unsigned turns = 0;
	process.set_served_calls(nullptr, {stub}, 112, 85, {}, [&](uint64_t) {
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		process.return_api(615);
	});
	process.set_time_slicing(std::chrono::milliseconds(1), [] { return true; }, [&] { ++turns; });
	process.load_thread(process.save_thread());
	const auto fair = process.call(stub, arguments, timeout);
	check(fair.stop == raven::NativeStop::returned && fair.value == 615 && turns > 0,
		"parallel served loop starved ready host work");
	process.set_time_slicing(std::chrono::milliseconds(0), {}, {});
}

static void queued_served_result(const std::string &bootstrap, const std::string &fixture)
{
	const auto timeout = std::chrono::seconds(10);
	const uint64_t base = 0x180000000ULL;
	const std::array<std::string_view, 5> names{"vkCmdBindPipeline", "vkCmdDrawIndexed", "vkQueueSubmit", "vkCmdBindDescriptorSets", "vkCmdBindVertexBuffers"};
	auto provider_names = names;
	std::sort(provider_names.begin(), provider_names.end());
	raven::ModuleSet modules;
	auto &provider = modules.add("vulkan-1.dll", raven::runtime_provider("vulkan-1.dll", base, provider_names, false, {}, {}, true));
	std::ifstream stream(fixture, std::ios::binary);
	const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
	auto &program = modules.add("queued.exe", bytes);
	std::vector<raven::LoadedImage *> images{&provider.loaded, &program.loaded};
	uint64_t colour_entry = 0, colour_stub = 0;
#ifdef RAVEN_OPENGL
	const uint64_t colour_base = 0x190000000ULL;
	const std::array<std::string_view, 1> colour_names{"glColor4f"};
	auto &colour_provider = modules.add("opengl32.dll", raven::runtime_provider("opengl32.dll", colour_base, colour_names, false, {}, {}, true));
	const auto colour_rva = colour_provider.exports.at(0).address;
	colour_entry = colour_base + colour_rva;
	colour_stub = raven::api_trap_address(colour_provider, "opengl32.dll", "glColor4f", colour_rva) - 112;
	check(colour_entry != colour_stub, "queued colour test is missing its queue prelude");
	const std::array<uint8_t, 12> colour_local{0x65, 0x4c, 0x8b, 0x14, 0x25, 0xf0, 0x1f, 0, 0, 0x90, 0x90, 0x90};
	std::copy(colour_local.begin(), colour_local.end(), colour_provider.loaded.data().data() + colour_stub - colour_base);
	images.push_back(&colour_provider.loaded);
#endif
	raven::NativeProcess process(images, bootstrap, timeout);
	check(process.parallel(), "queued test needs parallel execution");
	std::array<uint64_t, 5> entries{}, stubs{};
	for (size_t index = 0; index < names.size(); ++index) {
		const uint32_t rva = provider.exports.at(provider.export_names.at(std::string(names[index]))).address;
		entries[index] = base + rva;
		stubs[index] = raven::api_trap_address(provider, "vulkan-1.dll", names[index], rva) - 112;
		const std::array<uint8_t, 12> local{0x65, 0x4c, 0x8b, 0x14, 0x25, 0xf0, 0x1f, 0, 0, 0x90, 0x90, 0x90};
		std::copy(local.begin(), local.end(), provider.loaded.data().data() + stubs[index] - base);
	}
	const auto teb = process.map_memory(8192, PROT_READ | PROT_WRITE);
	auto context = process.context();
	context.registers.gs_base = teb;
	process.set_context(context);
	const auto word = [&](uint64_t address) {
		const auto data = process.read_memory(address, 8);
		uint64_t value = 0;
		std::memcpy(&value, data.data(), 8);
		return value;
	};
	const auto put = [&](uint64_t address, uint64_t value) {
		process.write_memory(address, {reinterpret_cast<const uint8_t *>(&value), 8});
	};
	std::array<uint64_t, 2> expected{};
	std::array<unsigned, 2> colours{};
	std::array<unsigned, 2> copied{}, fallback{};
	bool queue_enabled = true;
	bool nested = false;
	raven::NativeThreadState other;
	const uint64_t entry = program.loaded.base() + program.image.entry;
	std::vector<uint64_t> served{stubs.begin(), stubs.end()}, queued{stubs[0], stubs[1], stubs[3], stubs[4]};
	if (colour_stub) { served.push_back(colour_stub); queued.push_back(colour_stub); }
	process.set_served_calls(nullptr, served, 112, 85, {}, [&](uint64_t stub) {
		if (stub == colour_stub) {
			const auto producer = process.serving_teb();
			check(producer == teb || producer == other.context.registers.gs_base, "queued colour lost its producer");
			const size_t thread = producer == teb ? 0 : 1;
			const std::array<float, 4> wanted{float((thread ? 200 : 100) + expected[thread]) + .25f,
				-float(expected[thread]) - .5f, 3.75f, -4.5f};
			const auto floating = process.api_float_arguments();
			for (size_t index = 0; index < wanted.size(); ++index) {
				float value = 0;
				std::memcpy(&value, &floating[index], sizeof value);
				check(value == wanted[index], "queued colour reused a float register or crossed threads");
			}
			++colours[thread];
			process.return_api(0);
			return;
		}
		const auto arguments = process.api_arguments(stub == stubs[3] ? 8 : stub == stubs[4] ? 5 : stub == stubs[1] ? 6 : stub == stubs[0] ? 3 : 1);
		check(arguments[0] == 100 || arguments[0] == 200, "queued marker crossed threads");
		const size_t thread = arguments[0] == 100 ? 0 : 1;
		check(process.serving_teb() == (thread == 0 ? teb : other.context.registers.gs_base), "queued call used the consumer's TEB");
		if (stub == stubs[1]) {
			for (size_t index = 1; index < 6; ++index)
				check(arguments[index] == expected[thread] + index - 1, "queued draw was reordered or lost its arguments");
			++expected[thread];
		} else if (stub == stubs[0]) {
			check(expected[thread] && arguments[1] == expected[thread] + 9 && arguments[2] == expected[thread] + 19,
				"queued bind overtook its draw");
		} else if (stub == stubs[3] || stub == stubs[4]) {
			const bool descriptors = stub == stubs[3];
			const uint64_t index = expected[thread] - 1, count = (index / 64) % 6, dynamic_count = (count + 2) % 6;
			check(arguments[1] == (descriptors ? 9 : 12) && arguments[descriptors ? 4 : 2] == count,
				"queued binding lost its count or scalar arguments");
			if (descriptors)
				check(arguments[2] == 10 && arguments[3] == 11 && arguments[6] == dynamic_count, "queued descriptors lost stack arguments");
			const uint64_t pointer = arguments[descriptors ? 5 : 3];
			const uint64_t block = word((thread == 0 ? teb : other.context.registers.gs_base) + 0x1ff0);
			const bool in_record = pointer >= block + raven::ServedQueue::records * 8 && pointer < block + 4096;
			check(process.serving_queued() == in_record, "queued delivery lost its trusted execution kind");
			if (in_record) ++copied[descriptors ? 0 : 1];
			if (count > 4 || (descriptors && dynamic_count > 4)) {
				check(!in_record, "oversized binding entered the queue");
				++fallback[descriptors ? 0 : 1];
			}
			if (!queue_enabled) check(!in_record, "disabled binding still entered the queue");
			for (uint64_t item = 0; item < count; ++item) {
				const uint64_t value = 0x1234567800000000ULL + arguments[0] + index * 100 + item;
				check(word(pointer + item * 8) == value + (descriptors ? 0 : 20), "queued binding retained the caller's reused array");
				if (!descriptors) check(word(arguments[4] + item * 8) == value + 40, "queued vertex offsets were not copied");
			}
			if (descriptors) {
				const auto bytes = process.read_memory(arguments[7], dynamic_count * 4);
				for (uint64_t item = 0; item < dynamic_count; ++item) {
					uint32_t value = 0;
					std::memcpy(&value, bytes.data() + item * 4, 4);
					check(value == 0x87650000U + uint32_t(arguments[0] + index * 100 + item), "queued dynamic offsets were not copied as 32-bit words");
				}
			}
		} else {
			check(stub == stubs[2], "queued test dispatched another export");
			check(!process.serving_queued(), "synchronous barrier inherited a queued call");
			if (!nested && thread == 0) {
				nested = true;
				const auto saved = process.save_thread();
				process.load_thread(other);
				const uint64_t parameters[]{entries[1], entries[0], entries[2], 200, entries[3], entries[4], colour_entry};
				const auto result = process.call(entry, parameters, timeout);
				if (result.stop != raven::NativeStop::returned || result.value || expected[1] != 4096)
					std::cerr << "nested queue: stop=" << int(result.stop) << " value=" << result.value << " ip=" << std::hex
						<< result.instruction << std::dec << " draws=" << expected[1] << '\n';
				check(result.stop == raven::NativeStop::returned && result.value == 0 && expected[1] == 4096,
					"nested queue did not finish in order");
				other = process.save_thread();
				process.load_thread(saved);
			}
		}
		process.return_api(expected[thread]);
	}, queued);
	const auto other_teb = process.map_memory(8192, PROT_READ | PROT_WRITE);
	const auto stack = process.map_memory(65536, PROT_READ | PROT_WRITE);
	other = process.new_thread(stack + 65536, other_teb);
	process.set_execution_wait([](std::function<bool()> ready, auto limit) {
		while (!ready() && std::chrono::steady_clock::now() < limit)
			std::this_thread::sleep_for(std::chrono::microseconds(50));
	});
	const uint64_t parameters[]{entries[1], entries[0], entries[2], 100, entries[3], entries[4], colour_entry};
	const auto result = process.call(entry, parameters, timeout);
	check(!process.serving_queued(), "queue execution kind leaked after return");
	check(result.stop == raven::NativeStop::returned && result.value == 0 && expected[0] == 4096,
		"queued fixture returned before every draw was served");
	check(copied[0] && copied[1] && fallback[0] && fallback[1], "binding test missed queued copies or oversized synchronous fallback");
	const uint64_t block = word(teb + 0x1ff0);
	const uint64_t head = word(block + raven::ServedQueue::head * 8);
	check(head > raven::ServedQueue::capacity * 4 && head == word(block + raven::ServedQueue::tail * 8),
		"queue did not wrap or remained pending at return");
	// Model a worker's publication through a shared atomic, without a Windows wait on the consuming thread.
	const uint64_t other_block = word(other_teb + 0x1ff0);
	const uint64_t other_head = word(other_block + raven::ServedQueue::head * 8);
	const uint64_t record = other_block + (raven::ServedQueue::records + (other_head % raven::ServedQueue::capacity) * raven::ServedQueue::words) * 8;
	const uint64_t published[]{stubs[1], 200, 4096, 4097, 4098, 4099, 4100, 0};
	process.write_memory(record, {reinterpret_cast<const uint8_t *>(published), sizeof published});
	put(other_block + raven::ServedQueue::head * 8, other_head + 1);
	const uint64_t marker[]{100};
	const auto observed = process.call(entries[2], marker, timeout);
	check(observed.stop == raven::NativeStop::returned && observed.value == 4096 && expected[1] == 4097,
		"synchronous caller did not observe another thread's published commands");
	process.stop_thread(other);
	process.unmap_memory(other.served_guest);
	put(block + raven::ServedQueue::enabled * 8, 0);
	queue_enabled = false;
	expected[0] = 0;
	const auto synchronous = process.call(entry, parameters, timeout);
	check(synchronous.stop == raven::NativeStop::returned && synchronous.value == 0 && expected[0] == 4096 &&
		word(block + raven::ServedQueue::head * 8) == head, "disabled queue did not use synchronous calls");
	if (colour_entry) check(colours[0] == 8192 && colours[1] == 4096, "queued colour count changed across enabled and synchronous paths");
	const auto invalid = [&](const char *message) {
		bool rejected = false;
		try {
			const uint64_t marker[]{100};
			process.call(entries[2], marker, timeout);
		} catch (const std::runtime_error &error) {
			check(std::string(error.what()).find(message) != std::string::npos, "invalid queue failed for another reason");
			rejected = true;
		}
		check(rejected, "invalid queue was accepted");
		check(!process.serving_queued(), "queue execution kind leaked after failure");
		check(process.stopped(), "invalid queue resumed its guest before rejection");
		process.end_call();
	};
	put(block + raven::ServedQueue::head * 8, head + raven::ServedQueue::capacity + 1);
	invalid("indices exceed capacity");
	put(block + (raven::ServedQueue::records + (head % raven::ServedQueue::capacity) * raven::ServedQueue::words) * 8, stubs[2]);
	put(block + raven::ServedQueue::head * 8, head + 1);
	invalid("synchronous or unknown call");
}

static void opengl_formats()
{
	for (const bool x86 : {false, true}) {
		raven::ModuleSet modules;
		const std::array<std::string_view, 6> formats{"BitBlt", "ChoosePixelFormat", "DescribePixelFormat", "GetPixelFormat", "SetPixelFormat", "SwapBuffers"};
		const std::array<std::string_view, 4> aliases{"wglChoosePixelFormat", "wglDescribePixelFormat", "wglGetPixelFormat", "wglSetPixelFormat"};
		auto &gdi = modules.add("gdi32.dll", raven::runtime_provider("gdi32.dll", 0x50000000, formats, x86), 0x50000000);
		gdi.runtime_api = true;
		raven::forward_opengl_formats(modules, gdi);
		for (const auto &symbol : gdi.exports)
			check(symbol.forwarder.empty(), "absent OpenGL backend changed GDI exports");
		auto &backend = modules.add("opengl32.dll", raven::runtime_provider("opengl32.dll", 0x51000000, aliases, x86), 0x51000000);
		backend.runtime_api = true;
		raven::forward_opengl_formats(modules, gdi);
		for (const auto &symbol : gdi.exports)
			check(symbol.forwarder.empty(), "OpenGL placeholder was used as a graphics backend");
		backend.runtime_api = false;
		raven::forward_opengl_formats(modules, gdi);
		for (size_t index = 0; index < aliases.size(); ++index)
			check(modules.resolve({"gdi32.dll", std::string(formats[index + 1]), 0, false, 0}) ==
				modules.resolve({"opengl32.dll", std::string(aliases[index]), 0, false, 0}), "GDI pixel format did not resolve to its WGL backend");
		check(gdi.exports.at(gdi.export_names.at("SwapBuffers")).forwarder.empty() &&
			gdi.exports.at(gdi.export_names.at("BitBlt")).forwarder.empty(), "missing WGL function or unrelated GDI export was forwarded");
		for (auto &symbol : gdi.exports)
			symbol.forwarder.clear();
		gdi.runtime_api = false;
		raven::forward_opengl_formats(modules, gdi);
		for (const auto &symbol : gdi.exports)
			check(symbol.forwarder.empty(), "supplied GDI implementation was replaced");
	}
}

int main(int argc, char **argv)
{
	try {
		if (argc == 3 && std::string(argv[2]) == "--apc-wait") {
			apc_wait_result(argv[1]);
			std::cout << "Pending APC leaves a non-alertable wait asleep\n";
			return 0;
		}
		if (argc == 4 && std::string(argv[2]) == "--queued-served") {
			queued_served_result(argv[1], argv[3]);
			std::cout << "Queued calls, ring wrap, thread isolation and failure checks passed\n";
			return 0;
		}
		if (argc == 3 && std::string(argv[2]) == "--parallel-served") {
			parallel_served_result(argv[1]);
			std::cout << "Parallel served arguments and results passed\n";
			return 0;
		}
		if (argc != 2)
			throw std::runtime_error("bootstrap path required");
		served_thread_result(argv[1]);
		opengl_formats();
		for (const auto name : {"kernel32.dll", "kernelbase.dll"}) {
			const std::array<std::string_view, 1> semaphore_names{"ReleaseSemaphore"};
			raven::ModuleSet semaphores;
			auto &semaphore = semaphores.add(name, raven::runtime_provider(name, 0x180000000ULL, semaphore_names));
			const auto rva = semaphore.exports.at(0).address;
			check(raven::api_trap_address(semaphore, name, "ReleaseSemaphore", rva) == semaphore.loaded.base() + rva + 112 &&
				semaphore.loaded.data()[rva + 112] == 0xcc, "semaphore release does not use a served stub");
			raven::ModuleSet clocks;
			const std::array<std::string_view, 3> names{"QueryPerformanceCounter", "QueryPerformanceFrequency", "VirtualAlloc"};
			auto &provider = clocks.add(name, raven::runtime_provider(name, 0x180000000ULL, names, false, {}, {}, true));
			const auto pointer = [&](const std::string &symbol) {
				const auto rva = provider.exports.at(provider.export_names.at(symbol)).address;
				int32_t displacement = 0;
				std::memcpy(&displacement, provider.loaded.data().data() + rva + 3, 4);
				return int64_t(rva) + 7 + displacement;
			};
			check(pointer("QueryPerformanceCounter") == pointer("QueryPerformanceFrequency") + 8 &&
				pointer("QueryPerformanceCounter") != pointer("VirtualAlloc"), "clock calibration shares a mutable call pointer");
		}
		const uint64_t base = 0x180000000ULL;
		auto bytes = raven::environment_provider("kernel32.dll", base);
		const raven::PEImage image(bytes);
		check(image.is_64 && image.entry == 0 && image.imports().empty() && image.exports().size() == 50,
			"provider PE metadata differs");
		for (const auto &symbol : image.exports())
			check(symbol.ordinal >= 0x10000 - image.exports().size() && symbol.ordinal <= UINT16_MAX && symbol.names.size() == 1 && symbol.forwarder.empty(),
				"provider numbered a name outside the top of the 16-bit ordinal range");
		const std::array<std::string_view,1> controls{"LoadIconWithScaleDown"};
		const auto control_bytes = raven::runtime_provider("comctl32.dll",base,controls);
		const auto control_exports = raven::PEImage(control_bytes).exports();
		check(control_exports.size() == 1 && control_exports[0].ordinal == 381 && control_exports[0].names[0] == controls[0],
			"verified icon control ordinal differs");
		const std::array<std::pair<std::string_view, uint16_t>, 18> ldap_ordinals{{
			{"ldap_set_optionW", 14}, {"ldap_first_entry", 26}, {"ldap_next_entry", 27}, {"ldap_msgfree", 41},
			{"ldap_unbind_s", 46}, {"ldap_bind_sW", 73}, {"ldap_value_free_len", 79}, {"ldap_err2stringA", 117},
			{"ldap_first_attributeW", 127}, {"ldap_get_dnW", 133}, {"ldap_get_values_lenW", 142}, {"ldap_initW", 145},
			{"ldap_memfreeW", 147}, {"ldap_next_attributeW", 167}, {"ldap_search_sW", 208},
			{"ldap_simple_bind_sW", 216}, {"ldap_sslinitW", 219}, {"ber_free", 301},
		}};
		for (const bool x86 : {false, true}) {
			const auto ldap_bytes = raven::runtime_provider("wldap32.dll", x86 ? 0x70000000ULL : base,
				raven::system_export_names("wldap32.dll"), x86, {}, raven::system_export_ordinals("wldap32.dll"));
			const auto exports = raven::PEImage(ldap_bytes).exports();
			for (const auto &[name, ordinal] : ldap_ordinals)
				check(std::any_of(exports.begin(), exports.end(), [&](const auto &symbol) {
					return symbol.ordinal == ordinal && symbol.names.size() == 1 && symbol.names[0] == name;
				}), "LDAP provider export ordinal differs");
		}
		const std::array<std::string_view, 4> natives{"NtClose", "NtOpenThread", "NtRavenMissing", "ZwOpenThread"};
		const auto nt_bytes = raven::runtime_provider("ntdll.dll", base, natives);
		const raven::PEImage nt_image(nt_bytes);
		const auto nt_exports = nt_image.exports();
		const auto service = nt_image.file_span(nt_exports[1].address, 11);
		check(service[0] == 0x4c && service[1] == 0x8b && service[2] == 0xd1 && service[3] == 0xb8 &&
			service[8] == 0x0f && service[9] == 0x05 && service[10] == 0xc3, "NT service entry differs");
		check(std::ranges::equal(service, nt_image.file_span(nt_exports[3].address, 11)), "Zw service identity differs");
		const std::array<std::string_view, 1> only_thread{"NtOpenThread"};
		const auto smaller = raven::runtime_provider("ntdll.dll", base + 65536, only_thread);
		const raven::PEImage smaller_image(smaller);
		check(std::ranges::equal(service, smaller_image.file_span(smaller_image.exports()[0].address, 11)),
			"NT service identity depends on the process import graph");
		check(nt_image.file_span(nt_exports[2].address, 1)[0] == 0xcc, "unknown NT function received a service identity");
		std::vector<std::string> storage;
		for (size_t index = 0; index < 128; ++index)
			storage.push_back("A" + std::string(3 - std::to_string(index).size(), '0') + std::to_string(index));
		std::vector<std::string_view> names;
		for (const auto &name : storage)
			names.push_back(name);
		const auto capacity_bytes = raven::runtime_provider("capacity.dll", base, names);
		const auto capacity_exports = raven::PEImage(capacity_bytes).exports();
		check(capacity_exports.size() == 128, "provider table capacity differs");
		for (size_t index = 0; index < names.size(); ++index)
			check(capacity_exports[index].names.at(0) == names[index] && capacity_exports[index].address == 0x1000 + index * 16,
				"provider tables overlap at capacity");
		names.resize(4097, "Z");
		rejected([&] { raven::runtime_provider("capacity.dll", base, names); });
		rejected([&] { raven::environment_provider("unknown.dll", base); });
		// Turn this original header into a bare executable whose entry returns 42.
		bytes[0x96] = 3;
		bytes[0x97] = 0;
		bytes[0xa8] = 0;
		bytes[0xa9] = 0x10;
		const uint8_t entry_code[] = {0xb8, 42, 0, 0, 0, 0xc3};
		std::copy(std::begin(entry_code), std::end(entry_code), bytes.begin() + 0x200);
		raven::ModuleSet modules;
		auto &entry = modules.add("program.exe", bytes, base);
		rejected([&] { raven::run_native_process(modules, entry, argv[1], std::chrono::milliseconds(0)); });
		const auto result = raven::run_native_process(modules, entry, argv[1], std::chrono::seconds(2));
		check(result.stop == raven::NativeStop::returned && result.value == 42, "provider placement fixture did not execute");
		// Providers land after the executable and never overlap each other, whatever their export tables weigh.
		std::vector<std::pair<uint64_t, uint64_t>> ranges;
		for (const auto &[name, module] : modules.all())
			ranges.emplace_back(module->loaded.base(), module->loaded.base() + module->loaded.data().size());
		std::sort(ranges.begin(), ranges.end());
		bool placed = modules.all().at("kernel32.dll")->loaded.base() >= base + 65536;
		for (size_t index = 1; index < ranges.size(); ++index)
			placed = placed && ranges[index - 1].second <= ranges[index].first;
		if (!placed)
			for (const auto &range : ranges)
				std::cerr << std::hex << range.first << ' ' << range.second << '\n';
		check(placed, "provider placement overlapped the executable or another provider");
		rejected([&] { modules.resolve({"kernel32.dll", "Unimplemented", 0, false, 0}); });
		rejected([&] { modules.resolve({"kernel32.dll", "", 1, true, 0}); });
		raven::ModuleSet supplied;
		auto &other_entry = supplied.add("program.exe", bytes, base);
		auto &original = supplied.add("kernel32.dll", raven::environment_provider("kernel32.dll", base + 65536), base + 65536);
		const std::span<const uint8_t> original_view = original.bytes;
		const std::vector<uint8_t> original_bytes(original_view.begin(), original_view.end());
		check(raven::run_native_process(supplied, other_entry, argv[1], std::chrono::seconds(2)).value == 42,
			"supplied-module graph did not execute");
		check(supplied.all().at("kernel32.dll").get() == &original && std::ranges::equal(std::span<const uint8_t>(original.bytes), original_bytes),
			"runtime replaced a supplied DLL");
		std::cout << "Original native provider PE exports, placement, supplied DLL preservation and unsupported symbol/ordinal contracts passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
