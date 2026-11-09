#pragma once

#include "inherit.h"
#include "handle_channel.h"
#include "objects.h"

#include <filesystem>
#include <string>
#include <memory>
#include <string>
#include <sys/types.h>
#include <vector>
#include <unistd.h>

namespace raven {

// A process that Raven started for the guest: another instance of the runtime that runs the child program on its own.
class ProcessObject : public KernelObject {
public:
	std::u16string_view type_name() const override { return u"Process"; }
	uint32_t default_access() const override { return 0x1fffff; }
	explicit ProcessObject(pid_t pid, bool foreign = false) : pid(pid), foreign(foreign) {}
	~ProcessObject() override;
	bool describe(InheritedHandle &record) const override;
	// Waiting for a process succeeds once it has ended; the host reports that, not another guest thread.
	int signaled(uint32_t) const override { return exited() ? 1 : 0; }
	bool exited() const;
	// STILL_ACTIVE (259) while the process runs.
	uint32_t exit_code() const;
	// Ends the process the way TerminateProcess does, with the given exit code.
	void terminate(uint32_t code);
	const pid_t pid;
	// A process of the same runtime that is not this one's child (its parent, a sibling): it can be waited for and ended, but nobody reaps it here.
	const bool foreign;
	// The file name of the program the process runs, for process listings.
	std::u16string name;
	// Where handles go that are duplicated into the process.
	std::unique_ptr<HandleChannel> channel;
	// The file the process leaves its whole 32-bit exit code in; a host exit status keeps only the low byte of it.
	int exit_file = -1;

private:
	// A host handle to the process, opened when the handle first travels to another process.
	mutable int process_descriptor = -1;
	void reap() const;
	mutable bool done = false;
	mutable uint32_t code = 259;
	uint32_t requested = 0;
	bool killed = false;
};

// The calling process as an object other calls can name: what OpenProcess returns for the process's own id.
class SelfProcessObject : public KernelObject {
public:
	std::u16string_view type_name() const override { return u"Process"; }
	uint32_t default_access() const override { return 0x1fffff; }
	int signaled(uint32_t) const override { return 0; }
	~SelfProcessObject() override;
	// Another process that receives the handle names this one by its process id.
	bool describe(InheritedHandle &record) const override;

private:
	mutable int process_descriptor = -1;
};

// The host handle a process handle carries to another process: a pidfd naming the process, whose id travels beside it.
bool describe_process(pid_t pid, int &descriptor, InheritedHandle &record);

// Whether the process id belongs to a running instance of this runtime, the only processes a guest may name that it did not start.
bool runtime_process_alive(pid_t pid);
// The children that are still known by their process id, and the ones alive.
std::shared_ptr<ProcessObject> find_child(pid_t pid);
// Keeps the channel owners alive while queued transfers and requests are consumed, including a child's final messages.
std::vector<std::shared_ptr<ProcessObject>> retained_children();
std::vector<std::shared_ptr<ProcessObject>> live_children();

// The main thread of a child process: signaled when the process has ended.
class ChildThreadObject : public KernelObject {
public:
	std::u16string_view type_name() const override { return u"Thread"; }
	uint32_t default_access() const override { return 0x1fffff; }
	const std::shared_ptr<ProcessObject> &owner() const { return process; }
	explicit ChildThreadObject(std::shared_ptr<ProcessObject> process) : process(std::move(process)) {}
	int signaled(uint32_t) const override { return process->exited() ? 1 : 0; }
	// A thread started suspended waits at a gate; this opens it and returns the suspend count it had.
	uint32_t resume()
	{
		if (gate < 0)
			return 0;
		const char open = 1;
		(void)!::write(gate, &open, 1);
		::close(gate);
		gate = -1;
		return 1;
	}
	void hold(int descriptor) { gate = descriptor; }
	~ChildThreadObject() override
	{
		if (gate >= 0)
			::close(gate);
	}

private:
	int gate = -1;
	std::shared_ptr<ProcessObject> process;
};

struct ChildLaunch {
	std::filesystem::path image;
	std::u16string command_line;
	std::u16string directory;
	// Descriptors that become the child's standard input, output and error.
	int standard[3] = {0, 1, 2};
	// The environment block the child starts with (entries "NAME=VALUE" each ended by a zero, then one more zero).
	std::u16string environment;
	// When set, the child waits for a byte on this descriptor's other end before it runs; `gate` receives the end the parent keeps.
	bool suspended = false;
	int *gate = nullptr;
	// Handles the child receives under the values they have in the creator; `descriptor` is the creator's host descriptor behind each.
	std::vector<InheritedHandle> inherited;
};

// Starts the child; null with the Windows error in `error` when the host could not.
std::shared_ptr<ProcessObject> spawn_child(const ChildLaunch &launch, uint32_t &error);
// Serve the creator's requests while the primary thread waits for ResumeThread.
void wait_suspended_process(int descriptor, NativeObjects &objects);
// True while any child has not been reaped yet: guest threads then wait on the host rather than on each other.
bool children_running();
// A guest thread that waits for something only the host can deliver, such as network data, counts for as long as the object lives.
class HostWait {
public:
	HostWait();
	~HostWait();
	HostWait(const HostWait &) = delete;
	HostWait &operator=(const HostWait &) = delete;
};
// True while a child runs or a guest thread waits on the host: waiting threads are then not deadlocked.
bool host_events_pending();

}
