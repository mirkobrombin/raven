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
int64_t ec_sum8(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f, int64_t g, int64_t h);
uint64_t ec_hash(const char *text);
int64_t ec_call(int64_t (*callback)(int64_t, int64_t), int64_t a, int64_t b);
double ec_scale(int64_t a, double b);
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
			{"two arguments", 1, 40, 2, ec_add(40, 2)},
			{"eight arguments, four on the stack", 2, 3, 5, ec_sum8(3, 5, 4, 6, 5, 7, 6, 8)},
			{"a string", 3, 0, 0, static_cast<int64_t>(ec_hash("translated x64 hands native code a string"))},
			{"an integer and a double", 8, 6, std::bit_cast<int64_t>(1.5), std::bit_cast<int64_t>(ec_scale(6, 1.5))},
			{"a callback into x64", 4, 7, 4, ec_call(combine, 7, 4)},
			{"x18 at the TEB", 5, 0, 0, 1},
		};
		for (const Case &c : cases) {
			const int64_t got = call(c.role, c.a, c.b);
			check(got == c.expected, std::string(c.name) + ": " + std::to_string(got) + ", native " + std::to_string(c.expected));
			std::printf("%s: %lld (native %lld)\n", c.name, static_cast<long long>(got), static_cast<long long>(c.expected));
		}

		// x18 across forced exits: a watcher kicks the vCPU while it reads the TEB through x18 again and again.
		std::atomic<bool> done{false};
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
		// The same inside one long native loop, so that the forced exits land in native code.
		constexpr int64_t reads = 200000000;
		const int64_t right = call(9, reads, 0);
		done = true;
		watcher.join();
		std::printf("x18 at the TEB in 2000 native calls across %llu forced exits: %lld right; in %lld reads of one native loop across "
			"%llu forced exits: %lld right; x18 %#llx, TEB %#llx\n",
			static_cast<unsigned long long>(short_cancels), static_cast<long long>(good), static_cast<long long>(reads),
			static_cast<unsigned long long>(vcpu.stats().cancels - cancels - short_cancels), static_cast<long long>(right),
			static_cast<unsigned long long>(vcpu.reg(HV_REG_X18)), static_cast<unsigned long long>(teb));
		check(good == 2000 && right == reads && vcpu.reg(HV_REG_X18) == teb, "x18 did not hold the TEB");
		check(vcpu.stats().cancels - cancels - short_cancels >= 10, "too few forced exits landed in the native loop");

		// Warm: a loop of native calls stays in the vCPU; its cost next to the same loop calling x64 code.
		call(6, 10, 1);
		call(7, 10, 1);
		const uint64_t runs = runtime.stats().runs, misses = runtime.stats().lookup_misses;
		constexpr int64_t n = 1000000;
		uint64_t start = now_ns();
		const int64_t native_sum = call(6, n, 3);
		const double native_ns = double(now_ns() - start) / double(n);
		check(runtime.stats().runs - runs == 1 && runtime.stats().lookup_misses == misses,
			"a warm loop of calls into native code left the vCPU");
		start = now_ns();
		const int64_t x64_sum = call(7, n, 3);
		const double x64_ns = double(now_ns() - start) / double(n);
		check(native_sum == x64_sum && native_sum == n * (n - 1) / 2 + 3 * n, "the call loops disagree");
		std::printf("per call, x64 -> native ARM64EC -> x64: %.2f ns; x64 -> x64 through a function pointer: %.2f ns\n", native_ns,
			x64_ns);
	} catch (const std::exception &error) {
		check(false, error.what());
	}
	if (failures)
		return 1;
	std::printf("hvf arm64ec: PASS\n");
	return 0;
}
