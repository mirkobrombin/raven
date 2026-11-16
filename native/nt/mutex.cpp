#include "mutex.h"
#include "kernel_waits.h"

#include <new>
#include <stdexcept>
#include <sys/stat.h>
#include <utility>

namespace raven {

std::optional<uint32_t> mutex_access(uint32_t requested)
{
	if (requested & ~0xf21f0003U)
		return std::nullopt;
	uint32_t access = requested & 0x1f0003;
	if (requested & 0x80000000U) access |= 0x20001;
	if (requested & 0x40000000U) access |= 0x20000;
	if (requested & 0x20000000U) access |= 0x120000;
	if (requested & (0x10000000U | 0x02000000U)) access |= 0x1f0001;
	return access;
}

MutexObject::MutexObject() : waits_object(raven_waits_create(kernel_waits(), raven_mutex, 0, 0))
{
}

void MutexObject::leave_table()
{
	std::lock_guard lock(mutex);
	if (waits_object)
		raven_waits_detach(kernel_waits(), std::exchange(waits_object, 0), nullptr, nullptr, nullptr);
}

MutexObject::MutexObject(int descriptor) : descriptor(descriptor)
{
	try {
		struct stat information{};
		if (descriptor < 0 || ::fstat(descriptor, &information) || information.st_size != sizeof(MutexState))
			throw std::runtime_error("invalid mutex storage");
		void *mapped = ::mmap(nullptr, sizeof(MutexState), PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
		if (mapped == MAP_FAILED)
			throw std::runtime_error("cannot map mutex storage");
		state = static_cast<MutexState *>(mapped);
		listen_shared_changes();
		++shared_object_count;
	} catch (...) {
		if (state != &local) ::munmap(state, sizeof(MutexState));
		if (descriptor >= 0) ::close(descriptor);
		throw;
	}
}

MutexObject::~MutexObject()
{
	if (waits_object)
		raven_waits_close(kernel_waits(), waits_object);
	if (descriptor >= 0) {
		::munmap(state, sizeof(MutexState));
		::close(descriptor);
		--shared_object_count;
	}
}

uint64_t MutexObject::me(uint32_t thread) const
{
	return (uint64_t(::getpid()) << 32) | thread;
}

int MutexObject::signaled(uint32_t thread) const
{
	std::lock_guard lock(mutex);
	uint32_t owner = 0;
	if (waits_object)
		return raven_waits_query(kernel_waits(), waits_object, &owner) == 0 && (owner == 0 || owner == thread);
	const auto holder = state->owner.load();
	return holder == 0 || holder == me(thread);
}

bool MutexObject::try_consume(uint32_t thread)
{
	std::lock_guard lock(mutex);
	if (waits_object) {
		const uint32_t result = raven_waits_wait(kernel_waits(), thread, &waits_object, 1, 0, 0, 0);
		return result == 0 || result == 0x80;
	}
	const auto self = me(thread);
	if (state->owner.load() == self) {
		++state->recursion;
		return true;
	}
	uint64_t free = 0;
	if (!state->owner.compare_exchange_strong(free, self))
		return false;
	state->recursion = 1;
	return true;
}

bool MutexObject::release(uint32_t thread)
{
	std::lock_guard lock(mutex);
	if (waits_object)
		return raven_waits_release_mutex(kernel_waits(), waits_object, thread, nullptr) == 0;
	if (state->owner.load() != me(thread) || state->recursion.load() <= 0)
		return false;
	if (--state->recursion == 0) {
		state->owner = 0;
		if (shared || descriptor >= 0)
			announce_shared_change();
	}
	return true;
}

bool MutexObject::describe(InheritedHandle &record) const
{
	std::lock_guard lock(mutex);
	if (!shared && descriptor < 0) {
		const int created = ::memfd_create("raven-mutex", MFD_CLOEXEC | MFD_ALLOW_SEALING);
		if (created < 0)
			return false;
		if (::ftruncate(created, sizeof(MutexState)) ||
			::fcntl(created, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL)) {
			::close(created);
			return false;
		}
		void *mapped = ::mmap(nullptr, sizeof(MutexState), PROT_READ | PROT_WRITE, MAP_SHARED, created, 0);
		if (mapped == MAP_FAILED) {
			::close(created);
			return false;
		}
		auto *storage = new (mapped) MutexState();
		// The mutex leaves the table with its owner; the waits there for it end, to wait again on the shared one.
		if (waits_object) {
			uint32_t owner = 0, depth = 0;
			raven_waits_detach(kernel_waits(), std::exchange(waits_object, 0), nullptr, &owner, &depth);
			state->owner = owner ? me(owner) : 0;
			state->recursion = int32_t(depth);
		}
		storage->owner = state->owner.load();
		storage->recursion = state->recursion.load();
		state = storage;
		descriptor = created;
		listen_shared_changes();
		++shared_object_count;
	}
	record.kind = "mutex";
	record.descriptor = shared ? shared->file_descriptor() : descriptor;
	return record.descriptor >= 0;
}

}
