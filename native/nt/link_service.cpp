#include "kernel_context.h"
#include "kernel_waits.h"
#include "link.h"
#include "shared_file.h"
#include "threads.h"

#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <climits>
#include <mutex>

namespace raven {

namespace {

// The memory the program that started the runtime shares with the Windows program it runs (--link-fd): mapped shared here, where the
// waits sleep on its words, and in the guest once the program asks for it. Both sides wake each other with futexes on the same pages, as
// no call or message passes between them.
struct Link {
	int descriptor = -1;
	uint64_t size = 0;
	uint8_t *host = nullptr;
	std::mutex lock;
	uint64_t guest = 0;
};

Link &link()
{
	static Link shared;
	return shared;
}

constexpr uint32_t wait_timeout = 0x102, wait_failed = 0xffffffff;

}

bool adopt_link(int descriptor)
{
	struct stat status {};
	if (descriptor < 0 || ::fstat(descriptor, &status) || status.st_size <= 0 || ::fcntl(descriptor, F_SETFD, FD_CLOEXEC))
		return false;
	void *const mapped = ::mmap(nullptr, size_t(status.st_size), PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
	if (mapped == MAP_FAILED)
		return false;
	auto &shared = link();
	shared.descriptor = descriptor;
	shared.size = uint64_t(status.st_size);
	shared.host = static_cast<uint8_t *>(mapped);
	return true;
}

void register_link_service(ApiRegistry &registry, KernelContext &context)
{
	// The word of the link at a guest address, here; none for an address the guest's view does not hold or a word it does not align.
	const auto word_at = [](uint64_t address) -> std::atomic<uint32_t> * {
		auto &shared = link();
		const uint64_t guest = [&shared] {
			const std::lock_guard guard(shared.lock);
			return shared.guest;
		}();
		if (!guest || address < guest || address - guest > shared.size - sizeof(uint32_t) || address % alignof(uint32_t))
			return nullptr;
		return reinterpret_cast<std::atomic<uint32_t> *>(shared.host + (address - guest));
	};

	// void *RavenLinkMap(SIZE_T *size): the link's memory in the guest, mapped at the first call and at the same address afterwards, with
	// its size at *size when the pointer is given. NULL with ERROR_FILE_NOT_FOUND for a runtime started without a link.
	registry.add("RavenLinkMap", 1, [&context](ApiCall &call) {
		auto &shared = link();
		if (!shared.host) {
			call.set_last_error(2);
			call.ret(0);
			return;
		}
		uint64_t guest = 0;
		{
			const std::lock_guard guard(shared.lock);
			if (!shared.guest) {
				const auto view = context.memory.map_view(shared.descriptor, 0, shared.size, 0x04);
				if (view.status || !view.address) {
					call.set_last_error(8);
					call.ret(0);
					return;
				}
				shared.guest = view.address;
			}
			guest = shared.guest;
		}
		if (call[0])
			call.store_pointer(call[0], shared.size);
		call.ret(guest);
	});

	// DWORD RavenLinkWait(const volatile LONG *word, LONG seen, DWORD milliseconds): sleeps while the word of the link holds `seen`, until
	// the other side changes and wakes it or the time passes; 0 (WAIT_OBJECT_0) once it differs, WAIT_TIMEOUT, or WAIT_FAILED with
	// ERROR_INVALID_PARAMETER for a word outside the link. INFINITE waits without end.
	registry.add("RavenLinkWait", 3, [&context, word_at](ApiCall &call) {
		auto *const word = word_at(call[0]);
		if (!word) {
			call.set_last_error(87);
			call.ret(wait_failed);
			return;
		}
		const uint32_t seen = call.u32(1), milliseconds = call.u32(2);
		std::optional<std::chrono::steady_clock::time_point> until;
		if (milliseconds != UINT32_MAX)
			until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
		if (word->load(std::memory_order_acquire) != seen) {
			call.ret(0);
			return;
		}
		auto &threads = context.threads;
		// Beside the turn the thread's host thread sleeps on the word itself; what it cannot wait for there goes the usual way.
		if (call.alone) {
			const auto changed = threads.wait_word_beside(*word, seen, until);
			if (!changed)
				throw ApiNeedsTurn{};
			call.ret(*changed ? 0 : wait_timeout);
			return;
		}
		// A serial process blocks only with its thread stopped, as the guest that spins in the stub holds the only task.
		if (call.process.serving() && !call.process.parallel()) {
			call.process.decline_served();
			return;
		}
		if (threads.waits_outside() && NativeThreads::can_wait_words()) {
			auto &self = threads.current();
			const std::vector<std::atomic<uint32_t> *> words{word};
			for (;;) {
				const uint32_t result = threads.wait_outside([&](std::optional<std::chrono::steady_clock::time_point> limit) {
					for (;;) {
						const uint32_t calls = self.calls.load();
						if (calls != self.calls_seen) {
							self.calls_seen = calls;
							return raven_wait_interrupted;
						}
						if (word->load(std::memory_order_acquire) != seen)
							return 0U;
						timespec deadline{};
						if (limit) {
							if (std::chrono::steady_clock::now() >= *limit)
								return wait_timeout;
							const auto left = limit->time_since_epoch();
							deadline.tv_sec = std::chrono::duration_cast<std::chrono::seconds>(left).count();
							deadline.tv_nsec = (std::chrono::duration_cast<std::chrono::nanoseconds>(left) % std::chrono::seconds(1)).count();
						}
						wait_words(words, {seen}, self.calls, calls, limit ? &deadline : nullptr);
					}
				}, until);
				if (result == 0 || result == wait_timeout) {
					call.ret(result);
					return;
				}
			}
		}
		// Without a host thread for each guest thread, nothing tells the scheduler the other process changed the word: the thread looks
		// at it again every millisecond.
		for (;;) {
			auto next = std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
			if (until)
				next = std::min(next, *until);
			if (threads.block([word, seen] { return word->load(std::memory_order_acquire) != seen; }, next, false, true)) {
				call.ret(0);
				return;
			}
			if (until && std::chrono::steady_clock::now() >= *until) {
				call.ret(wait_timeout);
				return;
			}
		}
	});

	// BOOL RavenLinkWake(volatile LONG *word): wakes whoever sleeps on the word of the link, on either side, once the caller changed it.
	registry.add("RavenLinkWake", 1, [word_at](ApiCall &call) {
		auto *const word = word_at(call[0]);
		if (!word) {
			call.set_last_error(87);
			call.ret(0);
			return;
		}
		::syscall(SYS_futex, word, FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
		call.ret(1);
	});
}

}
