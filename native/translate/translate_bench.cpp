#include "runtime.h"
#include "test_process.h"
#include "x86_decoder.h"

#include "pe/pe.h"

#include <chrono>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

using namespace raven::translate;

// Measures the translator: translation speed over real code, the ahead-of-time cache against translating, entry.c's loop in each
// memory ordering, and the cost of crossing between translated and native code each way. Each metric is written as
// OUTPUT/<metric>.csv with one value per run; bench_summary.py turns them into the summary.
namespace {

std::filesystem::path guests, output;
int runs = 30;
bool round_only = false;

uint64_t now_ns()
{
	return static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void write_csv(const std::string &metric, const std::string &unit, const std::vector<double> &values)
{
	std::ofstream out(output / (metric + ".csv"));
	out << "run," << unit << "\n";
	for (size_t i = 0; i < values.size(); ++i)
		out << i << "," << values[i] << "\n";
	std::printf("%s: %zu runs written\n", metric.c_str(), values.size());
}

uint64_t stop_after = 0, native_calls = 0;
uint64_t counting_native()
{
	if (++native_calls == stop_after)
		Runtime::current()->request_stop(native_calls);
	return 0;
}

uint64_t ignore_native()
{
	return 0;
}

const std::map<std::string, TestProcess::Native> natives = {
	{"GetLastError", {reinterpret_cast<const void *>(&counting_native), 0}},
	{"ExitProcess", {reinterpret_cast<const void *>(&ignore_native), 1}},
	{"GetCommandLineW", {reinterpret_cast<const void *>(&ignore_native), 0}},
};

struct Fixture {
	TestProcess process;
	JitCodeCache cache{256 << 20};
	Runtime runtime;
	explicit Fixture(TranslatorOptions options = {}) : runtime(process, cache, options) { process.attach(runtime); }
	uint64_t load(const std::filesystem::path &path, bool bind = true)
	{
		const uint64_t base = process.load(path);
		if (bind)
			process.bind_imports(natives);
		process.prepare_thread();
		return base;
	}
};

// The code sections of an image as loaded.
std::vector<std::pair<uint64_t, uint64_t>> code_ranges(const std::vector<uint8_t> &file, uint64_t base)
{
	const raven::PEImage pe(file);
	std::vector<std::pair<uint64_t, uint64_t>> ranges;
	for (const raven::PESection &section : pe.sections)
		if (section.characteristics & 0x20000000u)
			ranges.emplace_back(base + section.address, base + section.address + section.virtual_size);
	return ranges;
}

// Walks a code section block by block, as a linear sweep would, skipping one instruction wherever the translator stops at once.
template<typename Translate>
void sweep(const TestProcess &process, uint64_t start, uint64_t end, Translate translate)
{
	for (uint64_t at = start; at < end;) {
		const uint32_t size = translate(at);
		if (size) {
			at += size;
			continue;
		}
		const auto code = process.code(at);
		const Instruction skipped = decode(code, at);
		at += skipped.length ? skipped.length : 1;
	}
}

void translation_speed(const std::filesystem::path &image)
{
	Fixture fixture;
	const uint64_t base = fixture.load(image, false);
	const auto ranges = code_ranges(fixture.process.file(base), base);
	std::vector<double> per_instruction, per_block;
	TranslationStats totals;
	uint64_t covered = 0, code_bytes = 0;
	for (int run = 0; run < runs; ++run) {
		TranslationStats stats;
		uint64_t bytes = 0;
		const uint64_t start = now_ns();
		for (const auto &[begin, end] : ranges)
			sweep(fixture.process, begin, end, [&](uint64_t at) {
				const TranslatedBlock block = translate_block(fixture.process.code(at), at, {}, stats);
				bytes += block.guest_size;
				return block.guest_size;
			});
		const uint64_t elapsed = now_ns() - start;
		per_instruction.push_back(double(elapsed) / double(stats.guest_instructions));
		per_block.push_back(double(elapsed) / double(stats.blocks));
		totals = stats;
		covered = bytes;
	}
	for (const auto &[begin, end] : ranges)
		code_bytes += end - begin;
	write_csv("translate_ns_per_guest_instruction", "ns", per_instruction);
	write_csv("translate_ns_per_block", "ns", per_block);
	std::printf("sweep of %s: %llu blocks, %llu guest instructions, %llu host words (%.2f per instruction), %llu of %llu code bytes "
		"translated (%.1f%%), %llu LDAPR, %llu STLR, %llu alignment checks\n", image.filename().c_str(),
		static_cast<unsigned long long>(totals.blocks), static_cast<unsigned long long>(totals.guest_instructions),
		static_cast<unsigned long long>(totals.host_words), double(totals.host_words) / double(totals.guest_instructions),
		static_cast<unsigned long long>(covered), static_cast<unsigned long long>(code_bytes), 100.0 * double(covered) / double(code_bytes),
		static_cast<unsigned long long>(totals.ordered_loads), static_cast<unsigned long long>(totals.ordered_stores),
		static_cast<unsigned long long>(totals.alignment_checks));
}

void ahead_of_time(const std::filesystem::path &image)
{
	const std::filesystem::path cache_path = output / "aot.cache";
	std::vector<double> translate_per_block, load_per_block, translate_total, load_total;
	for (int run = 0; run < runs; ++run) {
		uint64_t key = 0;
		size_t blocks = 0;
		{
			Fixture fixture;
			const uint64_t base = fixture.load(image, false);
			key = Runtime::cache_key(fixture.process.file(base), base, {});
			for (const auto &[begin, end] : code_ranges(fixture.process.file(base), base))
				sweep(fixture.process, begin, end, [&](uint64_t at) { return fixture.runtime.translate_ahead(at); });
			blocks = fixture.runtime.translation_stats().blocks;
			translate_total.push_back(double(fixture.runtime.stats().translate_ns) / 1e6);
			translate_per_block.push_back(double(fixture.runtime.stats().translate_ns) / double(blocks));
			fixture.runtime.save_cache(cache_path, key);
		}
		Fixture fixture;
		fixture.load(image, false);
		const size_t loaded = fixture.runtime.load_cache(cache_path, key);
		if (loaded != blocks)
			std::fprintf(stderr, "loaded %zu of %zu blocks\n", loaded, blocks);
		load_total.push_back(double(fixture.runtime.stats().cache_load_ns) / 1e6);
		load_per_block.push_back(double(fixture.runtime.stats().cache_load_ns) / double(loaded));
	}
	std::printf("ahead-of-time cache of %s: %.1f MiB\n", image.filename().c_str(),
		double(std::filesystem::file_size(cache_path)) / (1 << 20));
	std::filesystem::remove(cache_path);
	write_csv("aot_translate_ns_per_block", "ns", translate_per_block);
	write_csv("aot_load_ns_per_block", "ns", load_per_block);
	write_csv("aot_translate_ms_whole_image", "ms", translate_total);
	write_csv("aot_load_ms_whole_image", "ms", load_total);
}

void entry_loop(MemoryOrdering ordering, const char *name)
{
	TranslatorOptions options;
	options.ordering = ordering;
	Fixture fixture(options);
	const uint64_t entry = fixture.process.entry(fixture.load(guests / "entry.exe"));
	for (int warm = 0; warm < 100; ++warm)
		fixture.runtime.call(entry);
	constexpr int calls = 2000;
	std::vector<double> values;
	for (int run = 0; run < runs; ++run) {
		const uint64_t start = now_ns();
		for (int i = 0; i < calls; ++i)
			if (fixture.runtime.call(entry).value != 500500)
				throw std::runtime_error("entry.exe returned a wrong sum");
		values.push_back(double(now_ns() - start) / calls);
	}
	if (round_only) {
		std::printf("entry_translated_%s_ns_per_call %.3f\n", name, values.back());
		return;
	}
	write_csv(std::string("entry_translated_") + name + "_ns_per_call", "ns", values);
}

void guest_to_native()
{
	Fixture fixture;
	const uint64_t entry = fixture.process.entry(fixture.load(guests / "api_loop.exe"));
	constexpr uint64_t calls = 1000000;
	std::vector<double> values;
	for (int run = -1; run < runs; ++run) {
		native_calls = 0;
		stop_after = calls;
		const uint64_t start = now_ns();
		const RunResult result = fixture.runtime.call(entry);
		const uint64_t elapsed = now_ns() - start;
		if (result.reason != ExitReason::stopped || result.value != calls)
			throw std::runtime_error("api_loop.exe did not stop after its calls");
		if (run >= 0)
			values.push_back(double(elapsed) / calls);
	}
	write_csv("guest_to_native_call_ns", "ns", values);
}

void native_to_guest()
{
	Fixture fixture;
	const uint64_t entry = fixture.process.entry(fixture.load(guests / "return-73.exe"));
	constexpr int calls = 100000;
	fixture.runtime.call(entry);
	std::vector<double> values;
	for (int run = 0; run < runs; ++run) {
		const uint64_t start = now_ns();
		for (int i = 0; i < calls; ++i)
			fixture.runtime.call(entry);
		values.push_back(double(now_ns() - start) / calls);
	}
	write_csv("native_to_guest_call_ns", "ns", values);
}

}

int main(int argc, char **argv)
{
	if (argc < 3) {
		std::fprintf(stderr, "usage: translate_bench GUEST_DIRECTORY OUTPUT_DIRECTORY [LARGE_X64_IMAGE] [RUNS]\n");
		return 2;
	}
	// The runs share the machine with whatever else runs: ask for the performance cores.
#ifdef __APPLE__
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
	guests = argv[1];
	output = argv[2];
	if (output == "--entry-round") {
		// One run of each entry configuration, for bench_run.py to interleave with the native and Rosetta builds.
		runs = 1;
		round_only = true;
		entry_loop(MemoryOrdering::hardware, "plain");
		entry_loop(MemoryOrdering::rcpc, "ldapr_stlr");
		entry_loop(MemoryOrdering::barriers, "dmb");
		return 0;
	}
	std::filesystem::create_directories(output);
	if (argc > 4)
		runs = std::atoi(argv[4]);
	const std::filesystem::path large = argc > 3 ? std::filesystem::path(argv[3]) : guests / "arguments.exe";
	translation_speed(large);
	ahead_of_time(large);
	entry_loop(MemoryOrdering::hardware, "plain");
	entry_loop(MemoryOrdering::rcpc, "ldapr_stlr");
	entry_loop(MemoryOrdering::barriers, "dmb");
	guest_to_native();
	native_to_guest();
	return 0;
}
