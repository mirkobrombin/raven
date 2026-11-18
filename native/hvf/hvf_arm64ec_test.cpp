// An x64 program (testdata/ec_client.c) runs translated at EL0 and calls an ARM64EC DLL (testdata/ec_library.c) that runs natively at
// EL0 in the same vCPU, through the compiler's export and entry thunks, and is called back through its exit thunk. Results must match
// the library built for this host. x18 must hold the thread's TEB in native code, also after forced exits. A warm loop of calls into
// native code must not leave the vCPU, and its cost is printed next to the same loop calling x64 code.
#include "runtime.h"
#include "vcpu.h"
#include "vm.h"
#include "vm_process.h"

#include <atomic>
#include <chrono>
#include <bit>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <thread>

using namespace raven;

extern "C" {
int64_t ec_zero(void);
int64_t ec_add(int64_t a, int64_t b);
int64_t ec_sum1(int64_t a);
int64_t ec_sum3(int64_t a, int64_t b, int64_t c);
int64_t ec_sum4(int64_t a, int64_t b, int64_t c, int64_t d);
int64_t ec_sum5(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e);
int64_t ec_sum6(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f);
int64_t ec_sum7(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f, int64_t g);
int64_t ec_sum8(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f, int64_t g, int64_t h);
uint64_t ec_hash(const char *text);
int64_t ec_call(int64_t (*callback)(int64_t, int64_t), int64_t a, int64_t b);
double ec_scale(int64_t a, double b);
double ec_keep(int64_t (*callback)(int64_t, int64_t), double x, int64_t a);
int64_t ec_call6(int64_t (*callback)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t), int64_t a);
int64_t ec_call_double(int64_t (*callback)(int64_t, double), int64_t a, double x);
int64_t ec_call_checked(int64_t (*callback)(int64_t, int64_t), int64_t a, int64_t b);
}

namespace {

int failures = 0;

void check(bool condition, const std::string &message)
{
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message.c_str());
		++failures;
	}
}

// The client's callbacks, as the oracle calls them.
int64_t keep(int64_t a, int64_t b)
{
	return a * 5 + b;
}

int64_t checked(int64_t a, int64_t b)
{
	return a * 1000 - b;
}

int64_t sum6(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f)
{
	return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f;
}

int64_t double_callback(int64_t a, double x)
{
	return std::bit_cast<int64_t>(x) + a;
}

int64_t combine(int64_t a, int64_t b)
{
	return a * 10 - b;
}

uint64_t now_ns()
{
	return static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

}

int main(int argc, char **argv)
{
	if (argc < 2) {
		std::fprintf(stderr, "usage: hvf_arm64ec_test GUEST_DIRECTORY\n");
		return 2;
	}
	const std::filesystem::path guests = argv[1];
	try {
		hvf::Vm vm(256 << 20);
		hvf::VmProcess process(vm);
		process.load(guests / "ec_library.dll");
		const uint64_t entry = process.entry(process.load(guests / "ec_client.exe"));
		process.bind_imports();
		translate::TranslatorOptions options;
		options.ordering = translate::MemoryOrdering::hardware;
		hvf::VmCodeCache cache(vm, 16 << 20, 1 << 20);
		hvf::Vcpu vcpu(vm, true);
		translate::Runtime runtime(process, cache, options, &vcpu);
		hvf::VmProcess::Attachment attached(process, runtime);
		process.link_arm64ec(runtime);
		process.prepare_thread(runtime.state(), vcpu);
		const uint64_t teb = runtime.state().gs_base;
		auto call = [&](int64_t role, int64_t a, int64_t b) {
			const translate::RunResult result = runtime.call(entry, {uint64_t(role), uint64_t(a), uint64_t(b)});
			if (result.reason != translate::ExitReason::returned)
				throw std::runtime_error("role " + std::to_string(role) + " stopped with reason " +
					std::to_string(static_cast<unsigned>(result.reason)) + " at " + std::to_string(result.value));
			return static_cast<int64_t>(result.value);
		};

		struct Case {
			const char *name;
			int64_t role, a, b, expected;
		};
		const Case cases[] = {
			{"no arguments", 0, 0, 0, ec_zero()},
			{"one argument", 10, 7, 0, ec_sum1(7)},
			{"two arguments", 1, 40, 2, ec_add(40, 2)},
			{"three arguments", 11, 3, 5, ec_sum3(3, 5, 4)},
			{"four arguments", 12, 3, 5, ec_sum4(3, 5, 4, 6)},
			{"five arguments, one on the stack", 13, 3, 5, ec_sum5(3, 5, 4, 6, 5)},
			{"six arguments, two on the stack", 14, 3, 5, ec_sum6(3, 5, 4, 6, 5, 7)},
			{"seven arguments, three on the stack", 15, 3, 5, ec_sum7(3, 5, 4, 6, 5, 7, 6)},
			{"eight arguments, four on the stack", 2, 3, 5, ec_sum8(3, 5, 4, 6, 5, 7, 6, 8)},
			{"a string", 3, 0, 0, static_cast<int64_t>(ec_hash("translated x64 hands native code a string"))},
			{"an integer and a double", 8, 6, std::bit_cast<int64_t>(1.5), std::bit_cast<int64_t>(ec_scale(6, 1.5))},
			{"a callback into x64", 4, 7, 4, ec_call(combine, 7, 4)},
			{"a double kept in d8 across a cold callback", 16, 7, std::bit_cast<int64_t>(1.5),
				std::bit_cast<int64_t>(ec_keep(keep, 1.5, 7))},
			{"a callback with six arguments, two on the stack", 21, 3, 0, ec_call6(sum6, 3)},
			{"a callback with an integer and a double", 22, 5, std::bit_cast<int64_t>(2.5), ec_call_double(double_callback, 5, 2.5)},
			{"__os_arm64x_check_call to x64", 17, 9, 4, ec_call_checked(checked, 9, 4)},
			{"__os_arm64x_check_call to native code", 18, 9, 4, ec_call_checked(nullptr, 9, 4)},
			{"XMM6 and XMM7 across native code that writes v6 and v7", 23, 0x1234, 0x34, 0x1200},
			{"x18 at the TEB", 5, 0, 0, 1},
		};
		// Each case runs cold: its runs of the dispatcher count the host exits it took (lookup misses and chains included).
		for (const Case &c : cases) {
			const uint64_t runs = runtime.stats().runs;
			int64_t got;
			try {
				got = call(c.role, c.a, c.b);
			} catch (const std::exception &) {
				// A guest exception leaves the vCPU unusable: name the case and end the test.
				std::fprintf(stderr, "FAIL: %s threw\n", c.name);
				throw;
			}
			check(got == c.expected, std::string(c.name) + ": " + std::to_string(got) + ", native " + std::to_string(c.expected));
			const uint64_t took = runtime.stats().runs - runs;
			check(took >= 2, std::string(c.name) + ": ran warm, in " + std::to_string(took) + " run");
			std::printf("%s: %lld (native %lld), %llu runs\n", c.name, static_cast<long long>(got), static_cast<long long>(c.expected),
				static_cast<unsigned long long>(took));
		}

		// x18 across forced exits: a watcher kicks the vCPU while it reads the TEB through x18 again and again.
		std::atomic<bool> done{false};
		// Where each forced exit stopped the vCPU: in native code or elsewhere.
		std::atomic<uint64_t> native_cancels{0};
		vcpu.on_cancel = [&](hvf::Vcpu &stopped) {
			if (process.is_native(stopped.reg(HV_REG_PC)))
				++native_cancels;
		};
		std::thread watcher([&] {
			while (!done) {
				hv_vcpu_t id = vcpu.id();
				hv_vcpus_exit(&id, 1);
				std::this_thread::sleep_for(std::chrono::microseconds(200));
			}
		});
		// Stops the watcher also when a call throws, which would otherwise end the test in std::terminate.
		struct Join {
			std::atomic<bool> &done;
			std::thread &watcher;
			~Join()
			{
				done = true;
				if (watcher.joinable())
					watcher.join();
			}
		} join{done, watcher};
		int64_t good = 0;
		const uint64_t cancels = vcpu.stats().cancels;
		for (int i = 0; i < 2000; ++i)
			good += call(5, 0, 0);
		const uint64_t short_cancels = vcpu.stats().cancels - cancels;
		const uint64_t short_native = native_cancels;
		// The same inside one long native loop, so that the forced exits land in native code.
		constexpr int64_t reads = 200000000;
		const int64_t right = call(9, reads, 0);
		done = true;
		watcher.join();
		vcpu.on_cancel = nullptr;
		const uint64_t loop_cancels = vcpu.stats().cancels - cancels - short_cancels, loop_native = native_cancels - short_native;
		std::printf("x18 at the TEB in 2000 native calls across %llu forced exits (%llu in native code): %lld right; in %lld reads of one "
			"native loop across %llu forced exits (%llu in native code): %lld right; x18 %#llx, TEB %#llx\n",
			static_cast<unsigned long long>(short_cancels), static_cast<unsigned long long>(short_native), static_cast<long long>(good),
			static_cast<long long>(reads), static_cast<unsigned long long>(loop_cancels), static_cast<unsigned long long>(loop_native),
			static_cast<long long>(right), static_cast<unsigned long long>(vcpu.reg(HV_REG_X18)), static_cast<unsigned long long>(teb));
		check(good == 2000 && right == reads && vcpu.reg(HV_REG_X18) == teb, "x18 did not hold the TEB");
		check(loop_native >= 10, "too few forced exits landed in native code");

		// A second guest thread on its own vCPU and host thread gets its own TEB, which its native code reads through its own x18.
		{
			uint64_t second_teb = 0, second_x18 = 0;
			int64_t second_self = 0, second_count = 0;
			std::string second_error;
			std::thread second([&] {
				try {
					hvf::Vcpu other(vm, true);
					translate::Runtime::Thread &thread = runtime.add_thread(&other);
					// The thread goes before its runner, also when a call throws.
					struct Remove {
						translate::Runtime &runtime;
						translate::Runtime::Thread &thread;
						~Remove() { runtime.remove_thread(thread); }
					} remove{runtime, thread};
					process.prepare_thread(thread.state(), other);
					second_teb = thread.state().gs_base;
					second_self = static_cast<int64_t>(thread.call(entry, {5, 0, 0}).value);
					second_count = static_cast<int64_t>(thread.call(entry, {9, 1000000, 0}).value);
					second_x18 = other.reg(HV_REG_X18);
				} catch (const std::exception &error) {
					second_error = error.what();
				}
			});
			second.join();
			std::printf("second thread: TEB %#llx, x18 %#llx, NT_TIB.Self through x18 matches gs:[0x30]: %lld, %lld of 1000000 reads "
				"right; first thread still %lld\n",
				static_cast<unsigned long long>(second_teb), static_cast<unsigned long long>(second_x18),
				static_cast<long long>(second_self), static_cast<long long>(second_count), static_cast<long long>(call(5, 0, 0)));
			check(second_error.empty(), "second thread: " + second_error);
			check(second_teb && second_teb != teb && second_x18 == second_teb && second_self == 1 && second_count == 1000000 &&
					vcpu.reg(HV_REG_X18) == teb,
				"the second thread's x18 did not hold its own TEB");
		}

		// Warm: a loop of native calls stays in the vCPU; its cost next to the same loop calling x64 code.
		call(6, 10, 1);
		call(7, 10, 1);
		const uint64_t runs = runtime.stats().runs, misses = runtime.stats().lookup_misses;
		const hvf::Vcpu::Stats before = vcpu.stats();
		constexpr int64_t n = 1000000;
		uint64_t start = now_ns();
		const int64_t native_sum = call(6, n, 3);
		const double native_ns = double(now_ns() - start) / double(n);
		// One entry of the vCPU and one run of the dispatcher, ended by the return: no host exit of any kind in between.
		const hvf::Vcpu::Stats after = vcpu.stats();
		check(runtime.stats().runs - runs == 1 && runtime.stats().lookup_misses == misses && after.entries - before.entries == 1 &&
				after.cancels == before.cancels && after.write_faults == before.write_faults,
			"a warm loop of calls into native code left the vCPU");
		start = now_ns();
		const int64_t x64_sum = call(7, n, 3);
		const double x64_ns = double(now_ns() - start) / double(n);
		check(native_sum == x64_sum && native_sum == n * (n - 1) / 2 + 3 * n, "the call loops disagree");
		std::printf("per call, x64 -> native ARM64EC -> x64: %.2f ns; x64 -> x64 through a function pointer: %.2f ns\n", native_ns,
			x64_ns);

		// The callback warm: x64 -> native -> x64 -> native -> x64 without leaving the vCPU.
		{
			const uint64_t runs_before = runtime.stats().runs;
			const hvf::Vcpu::Stats entered = vcpu.stats();
			const int64_t got = call(4, 7, 4);
			check(got == ec_call(combine, 7, 4) && runtime.stats().runs - runs_before == 1 &&
					vcpu.stats().entries - entered.entries == 1 && vcpu.stats().cancels == entered.cancels,
				"a warm callback left the vCPU");
		}

		// Native code x64 must not reach: with no entry thunk, and at the start of the code map with no word before it. Both must stop
		// the call as unsupported at that address, not run it. Last, since a guest exception leaves the thread's stack as it was.
		auto stops_at = [&](const char *name, uint64_t address) {
			std::string outcome;
			try {
				const translate::RunResult result = runtime.call(entry, {20, address, 0});
				outcome = "reason " + std::to_string(static_cast<unsigned>(result.reason)) + " at " + std::to_string(result.value);
				check(result.reason == translate::ExitReason::unsupported && result.value == address,
					std::string(name) + ": " + outcome + ", expected unsupported at " + std::to_string(address));
			} catch (const std::exception &error) {
				outcome = error.what();
				check(false, std::string(name) + ": " + outcome);
			}
			std::printf("x64 calling %s %#llx: %s\n", name, static_cast<unsigned long long>(address), outcome.c_str());
		};
		stops_at("native code without an entry thunk", static_cast<uint64_t>(call(19, 0, 0)));
		// ec_library.dll's image base (CMakeLists.txt) plus its .text RVA, where its ARM64EC code map range starts.
		stops_at("the first word of the ARM64EC code", 0x190001000);
	} catch (const std::exception &error) {
		check(false, error.what());
	}
	if (failures)
		return 1;
	std::printf("hvf arm64ec: PASS\n");
	return 0;
}
