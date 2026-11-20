#pragma once

#include "objects.h"
#include "shared_file.h"

namespace raven {

std::optional<uint32_t> mutex_access(uint32_t requested);

struct MutexState {
	std::atomic<uint64_t> owner{0};
	std::atomic<int32_t> recursion{0};
};

class MutexObject : public KernelObject {
public:
	// A private mutex lives in the process's table (kernel_waits.h) until it is named or shared.
	MutexObject();
	explicit MutexObject(int descriptor);
	~MutexObject() override;
	std::u16string_view type_name() const override { return u"Mutant"; }
	bool closes_beside() const override { return true; }
	uint32_t default_access() const override { return 0x1f0001; }
	int signaled(uint32_t thread) const override;
	bool try_consume(uint32_t thread) override;
	void consume(uint32_t thread) override { try_consume(thread); }
	bool release(uint32_t thread);
	// Leaves the table for shared memory a name gives: the caller points `state` there.
	void leave_table();
	uint32_t table_object() const { std::lock_guard lock(mutex); return waits_object; }
	uint32_t table_mutex() const override { return table_object(); }
	// The low half of the owner, which holds the thread: it changes when the mutex is released or taken.
	std::atomic<uint32_t> *wait_word() const override;
	bool describe(InheritedHandle &record) const override;
	MutexState local;
	mutable MutexState *state = &local;
	std::shared_ptr<SharedFile> shared;

private:
	uint64_t me(uint32_t thread) const;
	mutable std::mutex mutex;
	mutable int descriptor = -1;
	mutable uint32_t waits_object = 0;
};

}
