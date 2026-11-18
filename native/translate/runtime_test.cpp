#include "runtime.h"
#include "test_process.h"

#include <cstdio>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>

using namespace raven::translate;

static std::filesystem::path guests;
static int failures = 0;

static void check(bool condition, const std::string &message)
{
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message.c_str());
		++failures;
	}
}

static const char *reason_name(ExitReason reason)
{
	static const char *const names[] = {"none", "lookup_miss", "chain", "returned", "syscall", "breakpoint", "unsupported", "stopped",
		"fault"};
	return names[static_cast<unsigned>(reason)];
}

static std::string describe(const RunResult &result)
{
	char buffer[96];
	std::snprintf(buffer, sizeof buffer, "%s %#llx", reason_name(result.reason), static_cast<unsigned long long>(result.value));
	return buffer;
}

// Fake system functions: native code of this process that the guest reaches through thunks.
static uint64_t exit_process_calls = 0;
static uint64_t fake_exit_process(uint64_t code)
{
	++exit_process_calls;
	Runtime::current()->request_stop(code);
	return 0;
}

static uint64_t last_error_calls = 0;
static uint64_t nested_entry = 0;
static uint64_t nested_results = 0;
static uint64_t fake_get_last_error()
{
	++last_error_calls;
	// A native function that calls back into guest code while a guest call to it is on the stack.
	if (nested_entry) {
		const RunResult inner = Runtime::current()->call(nested_entry);
		if (inner.reason == ExitReason::returned)
			nested_results += inner.value;
	}
	if (last_error_calls == 1000)
		Runtime::current()->request_stop(last_error_calls);
	return 0;
}

static const char16_t command_line[] = u"\"C:\\guest\\arguments.exe\" \"\" plain \"with space\" \"a\\\"b\" C:\\tail\\ --";
static uint64_t fake_get_command_line()
{
	return reinterpret_cast<uint64_t>(command_line);
}

static const std::map<std::string, TestProcess::Native> natives = {
	{"ExitProcess", {reinterpret_cast<const void *>(&fake_exit_process), 1}},
	{"GetLastError", {reinterpret_cast<const void *>(&fake_get_last_error), 0}},
	{"GetCommandLineW", {reinterpret_cast<const void *>(&fake_get_command_line), 0}},
};

struct Fixture {
	TestProcess process;
	JitCodeCache cache{64 << 20};
	Runtime runtime;
	explicit Fixture(TranslatorOptions options = {}) : runtime(process, cache, options) { process.attach(runtime); }
	uint64_t load(const char *name)
	{
		const uint64_t base = process.load(guests / name);
		process.bind_imports(natives);
		process.prepare_thread();
		return base;
	}
};

static void returns_its_status()
{
	Fixture fixture;
	const uint64_t base = fixture.load("return-73.exe");
	const RunResult result = fixture.runtime.call(fixture.process.entry(base));
	check(result.reason == ExitReason::returned && result.value == 73, "return-73.exe returned " + describe(result));
}

static void sums_its_loop(MemoryOrdering ordering)
{
	TranslatorOptions options;
	options.ordering = ordering;
	Fixture fixture(options);
	const uint64_t base = fixture.load("entry.exe");
	const RunResult result = fixture.runtime.call(fixture.process.entry(base));
	const auto name = std::to_string(static_cast<int>(ordering));
	check(result.reason == ExitReason::returned && result.value == 500500, "entry.exe (ordering " + name + ") returned " + describe(result));
	const TranslationStats &stats = fixture.runtime.translation_stats();
	check(fixture.runtime.stats().chains > 0, "entry.exe chained no branch");
	switch (ordering) {
	case MemoryOrdering::hardware:
		check(stats.barriers == 0 && stats.ordered_loads == 0 && stats.ordered_stores == 0, "hardware TSO emitted ordering");
		break;
	case MemoryOrdering::rcpc:
		check(stats.barriers == 0 && stats.ordered_loads > 0 && stats.ordered_stores > 0, "rcpc emitted no LDAPR/STLR, or barriers");
		break;
	case MemoryOrdering::barriers:
		check(stats.barriers > 0 && stats.ordered_loads == 0, "barrier ordering emitted no DMB");
		break;
	}
}

static void exits_through_a_native_function()
{
	exit_process_calls = 0;
	Fixture fixture;
	const uint64_t base = fixture.load("startup.exe");
	const RunResult result = fixture.runtime.call(fixture.process.entry(base));
	check(result.reason == ExitReason::stopped && result.value == 0 && exit_process_calls == 1,
		"startup.exe ended with " + describe(result) + " after " + std::to_string(exit_process_calls) + " ExitProcess calls");
}

static void loops_on_a_native_function()
{
	last_error_calls = 0;
	nested_entry = 0;
	Fixture fixture;
	const uint64_t base = fixture.load("api_loop.exe");
	const RunResult result = fixture.runtime.call(fixture.process.entry(base));
	check(result.reason == ExitReason::stopped && result.value == 1000, "api_loop.exe ended with " + describe(result));
}

static void calls_back_into_the_guest()
{
	last_error_calls = 0;
	nested_results = 0;
	Fixture fixture;
	const uint64_t inner = fixture.load("return-73.exe");
	const uint64_t outer = fixture.load("api_loop.exe");
	nested_entry = fixture.process.entry(inner);
	const RunResult result = fixture.runtime.call(fixture.process.entry(outer));
	nested_entry = 0;
	check(result.reason == ExitReason::stopped && result.value == 1000 && nested_results == 73 * 1000,
		"nested calls ended with " + describe(result) + ", inner results summed to " + std::to_string(nested_results));
}

static void reads_its_command_line()
{
	Fixture fixture;
	const uint64_t base = fixture.load("arguments.exe");
	const RunResult result = fixture.runtime.call(fixture.process.entry(base));
	check(result.reason == ExitReason::returned && result.value == 0, "arguments.exe returned " + describe(result));
}

static void sees_its_code_rewritten()
{
	Fixture fixture;
	const uint64_t base = fixture.load("return-73.exe");
	const uint64_t entry = fixture.process.entry(base);
	RunResult result = fixture.runtime.call(entry);
	check(result.reason == ExitReason::returned && result.value == 73, "before the write: " + describe(result));
	// mov eax, 0x49 becomes mov eax, 0x2a: the code page is watched, so the write reaches the runtime.
	*reinterpret_cast<volatile uint8_t *>(entry + 1) = 0x2a;
	result = fixture.runtime.call(entry);
	check(result.reason == ExitReason::returned && result.value == 0x2a, "after the write: " + describe(result));
	check(fixture.runtime.stats().invalidated_blocks > 0, "no translation was dropped");
}

static void rewrites_its_own_code()
{
	Fixture fixture;
	const uint64_t base = fixture.load("rewrite.exe");
	const RunResult result = fixture.runtime.call(fixture.process.entry(base));
	check(result.reason == ExitReason::returned && result.value == 12, "rewrite.exe returned " + describe(result));
	check(fixture.runtime.stats().invalidated_blocks > 0, "the guest's own write dropped no translation");
}

// The checksums the native arm64 build of testdata/workloads.c returns on its first call.
static void runs_a_workload(const char *name, uint64_t expected)
{
	Fixture fixture;
	const uint64_t base = fixture.load(name);
	const RunResult result = fixture.runtime.call(fixture.process.entry(base));
	check(result.reason == ExitReason::returned && result.value == expected, std::string(name) + " returned " + describe(result));
}

static void reuses_a_saved_translation()
{
	const std::filesystem::path path = std::filesystem::temp_directory_path() / "raven-translate-test.cache";
	uint64_t key = 0, first_placement = 0;
	{
		Fixture fixture;
		const uint64_t base = fixture.load("entry.exe");
		key = Runtime::cache_key(fixture.process.file(base), base, {});
		check(fixture.runtime.call(fixture.process.entry(base)).value == 500500, "first run");
		first_placement = reinterpret_cast<uint64_t>(fixture.runtime.translated_code(fixture.process.entry(base)));
		fixture.runtime.save_cache(path, key);
	}
	Fixture fixture;
	const uint64_t base = fixture.load("entry.exe");
	// Move everything the cache places, so code that held an absolute address of its first placement would break.
	fixture.cache.allocate(4096 + 16);
	const size_t loaded = fixture.runtime.load_cache(path, key);
	const uint64_t placement = reinterpret_cast<uint64_t>(fixture.runtime.translated_code(fixture.process.entry(base)));
	check(placement && placement != first_placement, "the cached block landed where it was first placed");
	const RunResult result = fixture.runtime.call(fixture.process.entry(base));
	check(loaded > 0, "nothing loaded from the cache");
	check(result.reason == ExitReason::returned && result.value == 500500, "cached entry.exe returned " + describe(result));
	check(fixture.runtime.translation_stats().blocks == 0, "translated " + std::to_string(fixture.runtime.translation_stats().blocks) +
		" blocks despite the cache");
	check(fixture.runtime.load_cache(path, key + 1) == 0, "a cache with another key was loaded");
	std::filesystem::remove(path);
}

// In this process the runtime runs one guest thread: a second would share the fault handler's unsynchronized state and natives
// would need per-thread stops. Adding one without a runner, or any once natives are bound, must be refused.
static void stays_single_threaded()
{
	Fixture fixture;
	bool refused = false;
	try {
		fixture.runtime.add_thread(nullptr);
	} catch (const std::invalid_argument &) {
		refused = true;
	}
	check(refused, "a second guest thread without a runner was accepted");
	fixture.load("startup.exe");
	struct Idle final : CodeRunner {
		uint32_t enter(uint64_t, uint64_t, uint64_t) override { return 0; }
	} idle;
	refused = false;
	try {
		fixture.runtime.add_thread(&idle);
	} catch (const std::logic_error &) {
		refused = true;
	}
	check(refused, "a guest thread was added to a runtime with native functions bound");
}

// movq between general and XMM registers keeps its value across blocks only where nothing between them writes v0-v7: not in this
// process, so without TranslatorOptions::vector_registers the block must end before it.
static void translates_movq_only_where_vectors_persist()
{
	const uint8_t code[] = {0x66, 0x48, 0x0f, 0x6e, 0xcf, 0x66, 0x4c, 0x0f, 0x7e, 0xc8, 0xc3}; // movq xmm1, rdi; movq rax, xmm9; ret
	TranslationStats stats;
	TranslatorOptions options;
	check(translate_block(code, 0x1000, options, stats).instructions == 0, "movq was translated without vector_registers");
	options.vector_registers = true;
	const uint32_t translated = translate_block(code, 0x1000, options, stats).instructions;
	check(translated == 3, "with vector_registers, the block should hold both movq and the ret: " + std::to_string(translated));
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		std::fprintf(stderr, "usage: runtime_test GUEST_DIRECTORY\n");
		return 2;
	}
	guests = argv[1];
	const std::pair<const char *, std::function<void()>> cases[] = {
		{"return_client", returns_its_status},
		{"entry, hardware TSO", [] { sums_its_loop(MemoryOrdering::hardware); }},
		{"entry, LDAPR/STLR", [] { sums_its_loop(MemoryOrdering::rcpc); }},
		{"entry, barriers", [] { sums_its_loop(MemoryOrdering::barriers); }},
		{"startup", exits_through_a_native_function},
		{"api_loop", loops_on_a_native_function},
		{"callback", calls_back_into_the_guest},
		{"arguments_client", reads_its_command_line},
		{"self-modifying code, native write", sees_its_code_rewritten},
		{"self-modifying code, guest write", rewrites_its_own_code},
		{"ahead-of-time cache", reuses_a_saved_translation},
		{"crc32 workload", [] { runs_a_workload("crc32.exe", 0x310c3e6c); }},
		{"fnv workload", [] { runs_a_workload("fnv.exe", 0xacb8b4443b0adc25); }},
		{"histogram workload", [] { runs_a_workload("histogram.exe", 0x28e14b303c16e232); }},
		{"one guest thread in this process", stays_single_threaded},
		{"movq only with vector_registers", translates_movq_only_where_vectors_persist},
	};
	for (const auto &[name, run] : cases) {
		const int before = failures;
		try {
			run();
		} catch (const std::exception &error) {
			std::fprintf(stderr, "FAIL: %s threw: %s\n", name, error.what());
			++failures;
		}
		std::printf("%s: %s\n", name, failures == before ? "ok" : "FAILED");
	}
	return failures == 0 ? 0 : 1;
}
