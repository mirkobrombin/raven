#pragma once

#include "host_names.h"
#include "text.h"

#include <atomic>
#include <cerrno>
#include <cctype>
#include <climits>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <linux/futex.h>
#include <memory>
#include <mutex>
#include <string>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>

namespace raven {

// The number of shared objects this process holds; waits on them are polled, so the scheduler looks more often while any exists.
extern std::atomic<int> shared_object_count;

// The directory that holds the shared objects of the prefix.
inline std::filesystem::path shared_objects_directory()
{
	std::filesystem::path base;
	if (const char *runtime = std::getenv("XDG_RUNTIME_DIR"); runtime && *runtime)
		base = runtime;
	else
		base = NativePrefix::from_environment().root();
	return base / ("raven-objects-" + prefix_tag());
}

// A word every process of the prefix maps and bumps when it changes a shared object another process may be waiting for: an idle process
// sleeps until it changes instead of looking at its shared objects every few milliseconds. Null when it cannot be made.
inline std::atomic<uint32_t> *shared_changes()
{
	static std::atomic<uint32_t> *word = [] () -> std::atomic<uint32_t> * {
		const auto directory = shared_objects_directory();
		std::error_code failure;
		std::filesystem::create_directories(directory, failure);
		const int descriptor = ::open((directory / ".changes").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
		if (descriptor < 0)
			return nullptr;
		if (::ftruncate(descriptor, 4096) != 0) {
			::close(descriptor);
			return nullptr;
		}
		void *mapped = ::mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
		::close(descriptor);
		return mapped == MAP_FAILED ? nullptr : static_cast<std::atomic<uint32_t> *>(mapped);
	}();
	return word;
}

// Wakes the processes that sleep on a shared object's word (KernelObject::wait_word).
inline void wake_word(std::atomic<uint32_t> *word)
{
	if (word)
		::syscall(SYS_futex, word, FUTEX_WAKE, INT32_MAX, nullptr, nullptr, 0);
}

// Sleeps until one of the shared words, or the process's own `call` word, differs from the value it had, or until `until` (on the
// monotonic clock). False when the kernel has no futex_waitv (before Linux 5.16).
inline bool wait_words(const std::vector<std::atomic<uint32_t> *> &words, const std::vector<uint32_t> &seen, std::atomic<uint32_t> &call,
	uint32_t call_seen, const timespec *until)
{
	struct futex_waitv entries[65]{};
	size_t count = 0;
	for (size_t index = 0; index < words.size() && index < 64; ++index)
		entries[count++] = {seen[index], uint64_t(reinterpret_cast<uintptr_t>(words[index])), FUTEX_32, 0};
	entries[count++] = {call_seen, uint64_t(reinterpret_cast<uintptr_t>(&call)), FUTEX_32 | FUTEX_PRIVATE_FLAG, 0};
	const long result = ::syscall(SYS_futex_waitv, entries, count, 0, until, CLOCK_MONOTONIC);
	return result >= 0 || errno != ENOSYS;
}

inline void announce_shared_change()
{
	if (auto *word = shared_changes()) {
		word->fetch_add(1);
		::syscall(SYS_futex, word, FUTEX_WAKE, INT32_MAX, nullptr, nullptr, 0);
	}
}

// The eventfd of the process's scheduler, written whenever the shared word changes.
inline std::atomic<int> shared_change_listener{-1};

// Starts, once, the thread that waits for changes to the shared word and wakes the scheduler.
inline void listen_shared_changes()
{
	static std::once_flag started;
	std::call_once(started, [] {
		auto *word = shared_changes();
		if (!word)
			return;
		std::thread([word] {
			for (;;) {
				const uint32_t seen = word->load();
				::syscall(SYS_futex, word, FUTEX_WAIT, seen, nullptr, nullptr, 0);
				if (const int descriptor = shared_change_listener.load(); descriptor >= 0) {
					const uint64_t one = 1;
					[[maybe_unused]] const auto written = ::write(descriptor, &one, sizeof one);
				}
			}
		}).detach();
	});
}

// Holds the lock of a directory of shared object files until the returned descriptor is closed; -1 when the lock file cannot be made.
inline int lock_directory(const std::filesystem::path &directory)
{
	const int guard = ::open((directory / ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	if (guard >= 0)
		while (::flock(guard, LOCK_EX) != 0 && errno == EINTR) {}
	return guard;
}

// Removes the files of a directory of shared objects that no live process holds: what processes that were killed left behind. The
// caller holds the directory's lock.
inline void sweep_directory(const std::filesystem::path &directory)
{
	std::error_code error;
	for (const auto &entry : std::filesystem::directory_iterator(directory, error)) {
		if (entry.path().filename() == ".lock" || entry.path().filename() == ".changes")
			continue;
		const int probe = ::open(entry.path().c_str(), O_RDWR | O_CLOEXEC);
		if (probe < 0)
			continue;
		if (::flock(probe, LOCK_EX | LOCK_NB) == 0)
			::unlink(entry.path().c_str());
		::close(probe);
	}
}

// The state of a named kernel object that several processes of a prefix share: a small host file every holder maps. A holder keeps a
// shared lock on the file, which tells a live object from one that a past run left; the last holder removes the file. Opening, creating
// and removing objects take one lock of the whole directory in turn: a lock changing from exclusive to shared is briefly released, and
// another process must not take the object for unheld then and truncate it under its creator.
class SharedFile {
public:
	SharedFile() = default;
	SharedFile(const SharedFile &) = delete;
	SharedFile &operator=(const SharedFile &) = delete;
	~SharedFile()
	{
		if (data)
			::munmap(data, size);
		const int guard = path.empty() ? -1 : lock_directory(std::filesystem::path(path).parent_path());
		if (descriptor >= 0)
			::close(descriptor);
		if (!path.empty()) {
			const int probe = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
			if (probe >= 0) {
				if (::flock(probe, LOCK_EX | LOCK_NB) == 0)
					::unlink(path.c_str());
				::close(probe);
			}
		}
		if (guard >= 0)
			::close(guard);
		if (counted)
			--shared_object_count;
	}

	// Opens or creates the object of a kind and name. `init` runs on the memory of a new object before anyone else can see it.
	static std::shared_ptr<SharedFile> open(const char *kind, const std::u16string &name, size_t size, bool create, bool &existed, uint32_t &error,
		const std::function<void(void *)> &init = {})
	{
		existed = false;
		listen_shared_changes();
		auto directory = shared_objects_directory();
		std::error_code failure;
		std::filesystem::create_directories(directory, failure);
		static std::once_flag swept;
		auto upper = to_utf8(name);
		for (auto &character : upper)
			character = char(std::toupper(static_cast<unsigned char>(character)));
		auto file = std::make_shared<SharedFile>();
		file->path = (directory / (std::string(kind) + "-" + hex_text(name_hash(upper)))).string();
		file->size = size;
		const int guard = lock_directory(directory);
		struct Release {
			int descriptor;
			~Release() { if (descriptor >= 0) ::close(descriptor); }
		} release{guard};
		std::call_once(swept, [&] { sweep_directory(directory); });
		file->descriptor = ::open(file->path.c_str(), (create ? O_CREAT : 0) | O_RDWR | O_CLOEXEC, 0600);
		if (file->descriptor < 0) {
			file->path.clear();
			error = create ? 5 : 2;
			return nullptr;
		}
		const bool first = ::flock(file->descriptor, LOCK_EX | LOCK_NB) == 0;
		if (first && !create) {
			// Nobody holds the file: the object does not exist any more.
			::flock(file->descriptor, LOCK_UN);
			file->path.clear();
			error = 2;
			return nullptr;
		}
		if (first && (::ftruncate(file->descriptor, 0) || ::ftruncate(file->descriptor, off_t(size)))) {
			file->path.clear();
			error = 8;
			return nullptr;
		}
		file->data = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, file->descriptor, 0);
		if (file->data == MAP_FAILED) {
			file->data = nullptr;
			file->path.clear();
			error = 8;
			return nullptr;
		}
		if (first) {
			if (init)
				init(file->data);
			::flock(file->descriptor, LOCK_SH);
		} else {
			::flock(file->descriptor, LOCK_SH);
			existed = true;
		}
		file->counted = true;
		++shared_object_count;
		return file;
	}

	void *data = nullptr;
	size_t size = 0;
	int file_descriptor() const { return descriptor; }

private:

	int descriptor = -1;
	std::string path;
	bool counted = false;
};

}
