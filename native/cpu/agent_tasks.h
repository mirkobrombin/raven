#pragma once

#include "agent_context.h"

#include <cstddef>
#include <cstdint>
#include <sys/ptrace.h>
#include <sys/types.h>

// Guest tasks handed to the agent: they run untraced, and what used to stop them for ptrace stops them in the agent's handler
// instead (see agent.S). The runtime keeps driving them with ptrace's requests, waits and interrupts; for a task held here these go
// to the agent over the task's block, for any other task to the kernel. The runtime's handling of a stop is the same either way.

namespace raven::agent {

// The signal the runtime interrupts a handed-over task with, as it stops a traced one with SIGSTOP; its stop reads as SIGSTOP.
constexpr int interrupt_signal = 64;
// The word of a task's block whose first byte is its selector for Syscall User Dispatch.
constexpr size_t dispatch_selector = 407;

struct Task {
	pid_t task = 0;
	// The thread group the task belongs to, and whether the task leads it (a child of the runtime's, whose end waitpid reports).
	pid_t group = 0;
	// The guest's process group, which every task of the guest stays in and which ends with the guest.
	pid_t process = 0;
	uint64_t *block = nullptr;
	// The alternate signal stack, as the runtime and the guest see it.
	uint8_t *stack = nullptr;
	uint64_t stack_guest = 0;
	Segments segments;
	// The size of the extended state as ptrace gives it, learned while the task was traced. A signal frame may carry less (the
	// components the task has not been allowed); the rest reads as the components' initial state.
	size_t xstate_size = 0;
	bool running = false;
	// The system call a SIGSYS stop turned away, as orig_rax reads it; -1 for any other stop.
	int64_t call = -1;
};

// Requests for `task` go to the agent from now on; the task is running toward its next stop in the handler.
void hold(const Task &task);
// Forgets every task of the thread group or of the guest's process group, which has ended.
void release_group(pid_t group);
bool held(pid_t task);

// ptrace(2) for any task.
long request(enum __ptrace_request operation, pid_t task, void *address, void *data);
// waitpid(2) for any task. A held task's stop reads as a traced one's: a turned-away system call as SIGTRAP | 0x80, the interrupt as
// SIGSTOP, anything else as its signal.
pid_t wait(pid_t task, int *status, int options);
// Interrupts a running task, which then stops as wait reports: SIGSTOP for a traced one, interrupt_signal for a held one.
int interrupt(pid_t task);
// Whether a held task has stopped, so wait would not block.
bool stopped(pid_t task);

}
