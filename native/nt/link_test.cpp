// The Linux side of a link (link.h): shares memory with a Windows program the runtime runs, counts one round at a time on the first word
// and waits for the program's answer on the second, as a plugin bridge hands an audio block over and waits for it. Prints how long each
// round took from the count to the answer, and the processor time the runtime spent meanwhile. A gap between rounds paces them as audio
// blocks come, where the program sleeps between two.
//
// usage: link_test RAVEN PROGRAM [ROUNDS [GAP_MICROSECONDS]]

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static void check(bool value, const char *message)
{
	if (!value)
		throw std::runtime_error(message);
}

int main(int count, char **arguments)
{
	if (count < 3) {
		std::fputs("usage: link_test RAVEN PROGRAM [ROUNDS [GAP_MICROSECONDS]]\n", stderr);
		return 2;
	}
	const int rounds = count > 3 ? std::atoi(arguments[3]) : 2000;
	const auto gap = std::chrono::microseconds(count > 4 ? std::atoi(arguments[4]) : 0);
	try {
		const int memory = ::memfd_create("raven-link-test", 0);
		check(memory >= 0 && ::ftruncate(memory, 65536) == 0, "no shared memory");
		void *const mapped = ::mmap(nullptr, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, memory, 0);
		check(mapped != MAP_FAILED, "the shared memory does not map");
		auto *const words = static_cast<std::atomic<uint32_t> *>(mapped);
		words[2].store(uint32_t(rounds));
		const std::string link = "--link-fd=" + std::to_string(memory);
		const pid_t child = ::fork();
		check(child >= 0, "no child process");
		if (child == 0) {
			char run[] = "--run-process", native[] = "--execution=native", timeout[] = "--timeout-ms=600000";
			char *const command[] = {arguments[1], run, native, timeout, const_cast<char *>(link.c_str()), arguments[2], nullptr};
			::execv(arguments[1], command);
			::_exit(127);
		}
		std::vector<double> took;
		took.reserve(size_t(rounds));
		std::chrono::steady_clock::time_point began;
		for (int round = 1; round <= rounds; ++round) {
			if (round == 2)
				began = std::chrono::steady_clock::now();
			if (round > 1 && gap.count())
				std::this_thread::sleep_until(std::chrono::steady_clock::now() + gap);
			const auto start = std::chrono::steady_clock::now();
			words[0].store(uint32_t(round), std::memory_order_release);
			::syscall(SYS_futex, &words[0], FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
			const auto limit = start + std::chrono::seconds(round == 1 ? 60 : 5);
			for (uint32_t seen; (seen = words[1].load(std::memory_order_acquire)) != uint32_t(round);) {
				const auto now = std::chrono::steady_clock::now();
				int status = 0;
				check(now < limit, "the program does not answer");
				check(::waitpid(child, &status, WNOHANG) == 0, "the program ended before it answered");
				const timespec slice{0, 50'000'000};
				::syscall(SYS_futex, &words[1], FUTEX_WAIT, seen, &slice, nullptr, 0);
			}
			took.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count());
		}
		const double played = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
		int status = 0;
		check(::waitpid(child, &status, 0) == child, "the program cannot be waited for");
		check(WIFEXITED(status) && WEXITSTATUS(status) == 0, ("the program ended with status " + std::to_string(status)).c_str());
		rusage used{};
		::getrusage(RUSAGE_CHILDREN, &used);
		const double processor = (used.ru_utime.tv_sec + used.ru_stime.tv_sec) * 1e3 + (used.ru_utime.tv_usec + used.ru_stime.tv_usec) / 1e3;
		// The first round includes the program's start.
		took.erase(took.begin());
		std::sort(took.begin(), took.end());
		if (!took.empty())
			std::printf("link: %d rounds, median %.1f us, 99th percentile %.1f us, longest %.1f us; the runtime used %.0f ms of processor time "
				"in %.0f ms, its start included\n", rounds, took[took.size() / 2], took[took.size() * 99 / 100], took.back(), processor, played);
	} catch (const std::exception &error) {
		std::fprintf(stderr, "link_test: %s\n", error.what());
		return 1;
	}
	return 0;
}
