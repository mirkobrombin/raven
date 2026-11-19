#include "agent_tasks.h"
#include "agent.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <linux/futex.h>
#include <mutex>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>

#ifndef PTRACE_SYSEMU_SINGLESTEP
#define PTRACE_SYSEMU_SINGLESTEP 32
#endif

namespace raven::agent {

namespace {

std::mutex tasks_lock;
std::unordered_map<pid_t, Task> tasks;

Task *find(pid_t task)
{
	std::lock_guard lock(tasks_lock);
	const auto found = tasks.find(task);
	return found == tasks.end() ? nullptr : &found->second;
}

uint32_t *state_word(const Task &task)
{
	return reinterpret_cast<uint32_t *>(&task.block[state]);
}

// A guest address on the task's alternate stack, as the runtime sees it; null outside the stack.
template<typename T> T *on_stack(const Task &task, uint64_t address, size_t size = sizeof(T))
{
	if (address < task.stack_guest || address - task.stack_guest > stack_size || size > stack_size - (address - task.stack_guest))
		return nullptr;
	return reinterpret_cast<T *>(task.stack + (address - task.stack_guest));
}

ucontext_t *frame(const Task &task)
{
	return on_stack<ucontext_t>(task, task.block[context]);
}

// The extended state the kernel saved with the frame: its standard XSAVE layout and size, or the 512 bytes of FXSAVE alone.
uint8_t *extended_state(const Task &task, size_t &size)
{
	const ucontext_t *const context = frame(task);
	if (!context || !context->uc_mcontext.fpregs)
		return nullptr;
	auto *const area = on_stack<uint8_t>(task, uint64_t(context->uc_mcontext.fpregs), 512);
	if (!area)
		return nullptr;
	uint32_t magic = 0, length = 0;
	std::memcpy(&magic, area + 464, 4);
	std::memcpy(&length, area + 464 + 16, 4);
	size = magic == 0x46505853 /* FP_XSTATE_MAGIC1 */ && length >= 512 && on_stack<uint8_t>(task, uint64_t(context->uc_mcontext.fpregs),
		length) ? length : 512;
	return area;
}

int failed(int error)
{
	errno = error;
	return -1;
}

// The stop the agent posted, read as a traced task's.
int stop_status(Task &task)
{
	task.running = false;
	const int signal = int(task.block[agent::signal]);
	task.call = -1;
	if (signal == SIGSYS) {
		// As after a SYSEMU stop: orig_rax names the call and RAX reads -ENOSYS until the runtime answers.
		const auto *information = on_stack<siginfo_t>(task, task.block[agent::information]);
		task.call = information ? information->si_syscall : -1;
		if (ucontext_t *const context = frame(task))
			context->uc_mcontext.gregs[REG_RAX] = -ENOSYS;
		return ((SIGTRAP | 0x80) << 8) | 0x7f;
	}
	return ((signal == interrupt_signal ? SIGSTOP : signal) << 8) | 0x7f;
}

// Whether the held task is gone; its status is then the one waitpid reports.
bool ended(Task &task, int *status)
{
	if (task.task == task.group) {
		int reaped = 0;
		const pid_t result = ::waitpid(task.task, &reaped, WNOHANG | __WALL);
		// Reaped already, as the runtime reaps a whole process group once it ends the guest: the task is gone all the same.
		if (result == -1 && errno == ECHILD) {
			if (status)
				*status = SIGKILL;
			return true;
		}
		if (result != task.task)
			return false;
		if (status)
			*status = reaped;
		return true;
	}
	if (syscall(SYS_tgkill, task.group, task.task, 0) == 0 || errno != ESRCH)
		return false;
	if (status)
		*status = SIGKILL;
	return true;
}

long resume(Task &task, enum __ptrace_request operation, void *data)
{
	if (task.running)
		return failed(ESRCH);
	// A signal handed to the guest on resumption is not something the agent passes on.
	if (data)
		return failed(EINVAL);
	ucontext_t *const context = frame(task);
	if (!context)
		return failed(EFAULT);
	// SYSEMU runs guest code, whose system calls come to the runtime; CONT runs the runtime's own code, whose calls the kernel makes.
	const bool emulated = operation == PTRACE_SYSEMU || operation == PTRACE_SYSEMU_SINGLESTEP;
	*reinterpret_cast<volatile uint8_t *>(&task.block[dispatch_selector]) = emulated ? SYSCALL_DISPATCH_FILTER_BLOCK : SYSCALL_DISPATCH_FILTER_ALLOW;
	const bool step = operation == PTRACE_SINGLESTEP || operation == PTRACE_SYSEMU_SINGLESTEP;
	auto &flags = context->uc_mcontext.gregs[REG_EFL];
	flags = step ? flags | 0x100 : flags & ~0x100LL;
	task.running = true;
	task.call = -1;
	__atomic_store_n(state_word(task), answered, __ATOMIC_RELEASE);
	syscall(SYS_futex, state_word(task), FUTEX_WAKE, 1, nullptr, nullptr, 0);
	return 0;
}

// The debug registers have no counterpart in a signal frame: ptrace stays for them, as for any control of the task from outside. The
// task, held in the handler, is taken for the one request and let go again; the registers stay set without a tracer, and a breakpoint
// they raise reaches the handler as any other SIGTRAP.
long controlled(Task &task, enum __ptrace_request operation, void *address, void *data)
{
	const auto offset = reinterpret_cast<uintptr_t>(address);
	const auto first = offsetof(struct user, u_debugreg), last = first + sizeof(user::u_debugreg);
	if (offset < first || offset >= last || offset % sizeof(unsigned long))
		return failed(EIO);
	if (ptrace(PTRACE_SEIZE, task.task, nullptr, nullptr) == -1)
		return -1;
	if (ptrace(PTRACE_INTERRUPT, task.task, nullptr, nullptr) == -1) {
		const int error = errno;
		ptrace(PTRACE_DETACH, task.task, nullptr, nullptr);
		return failed(error);
	}
	int status = 0;
	pid_t result;
	while ((result = ::waitpid(task.task, &status, __WALL)) == -1 && errno == EINTR) {}
	if (result != task.task || !WIFSTOPPED(status))
		return failed(ESRCH);
	// A signal that came first stops the task for its delivery; it goes back with the detach.
	const int delivered = (status >> 16) == PTRACE_EVENT_STOP ? 0 : WSTOPSIG(status);
	errno = 0;
	const long answer = ptrace(operation, task.task, address, data);
	const int error = errno;
	ptrace(PTRACE_DETACH, task.task, nullptr, reinterpret_cast<void *>(uintptr_t(delivered)));
	errno = error;
	return answer;
}

long held_request(Task &task, enum __ptrace_request operation, void *address, void *data)
{
	switch (operation) {
	case PTRACE_CONT:
	case PTRACE_SYSEMU:
	case PTRACE_SINGLESTEP:
	case PTRACE_SYSEMU_SINGLESTEP:
		return resume(task, operation, data);
	case PTRACE_SETOPTIONS:
		return 0;
	case PTRACE_PEEKTEXT:
	case PTRACE_PEEKDATA: {
		long word = 0;
		iovec local{&word, sizeof word}, remote{address, sizeof word};
		if (process_vm_readv(task.task, &local, 1, &remote, 1, 0) != ssize_t(sizeof word))
			return failed(EIO);
		errno = 0;
		return word;
	}
	default:
		break;
	}
	// The rest reads or changes a stopped task.
	if (task.running)
		return failed(ESRCH);
	ucontext_t *const context = frame(task);
	if (!context)
		return failed(EFAULT);
	switch (operation) {
	case PTRACE_GETREGS:
		*static_cast<user_regs_struct *>(data) = registers(*context, task.segments, task.call);
		return 0;
	case PTRACE_SETREGS: {
		const auto &wanted = *static_cast<const user_regs_struct *>(data);
		// The frame carries neither the segment selectors nor the bases: the handler loads what changed on its way out, selectors
		// first. A selector that is not null sets its base from its descriptor, and writing the base after it would clear it again.
		auto &kept = task.segments;
		uint64_t asked = 0;
		const auto selector = [&](uint64_t now, uint64_t &was, size_t word, uint64_t bit) {
			if (now == was)
				return false;
			task.block[word] = now;
			asked |= bit;
			was = now;
			return now != 0;
		};
		const bool fs_described = selector(wanted.fs, kept.fs, fs_selector, load_fs);
		const bool gs_described = selector(wanted.gs, kept.gs, gs_selector, load_gs);
		selector(wanted.ds, kept.ds, ds_selector, load_ds);
		selector(wanted.es, kept.es, es_selector, load_es);
		if (wanted.fs_base != kept.fs_base && !fs_described) {
			task.block[fs_base] = wanted.fs_base;
			asked |= set_fs;
		}
		if (wanted.gs_base != kept.gs_base && !gs_described) {
			task.block[gs_base] = wanted.gs_base;
			asked |= set_gs;
		}
		task.block[set_bases] |= asked;
		kept.fs_base = wanted.fs_base;
		kept.gs_base = wanted.gs_base;
		set_registers(*context, wanted, kept);
		return 0;
	}
	case PTRACE_GETSIGINFO: {
		const auto *information = on_stack<siginfo_t>(task, task.block[agent::information]);
		if (!information)
			return failed(EFAULT);
		*static_cast<siginfo_t *>(data) = *information;
		return 0;
	}
	case PTRACE_GETFPREGS:
	case PTRACE_SETFPREGS: {
		size_t size = 0;
		uint8_t *const area = extended_state(task, size);
		if (!area)
			return failed(EFAULT);
		// The software bytes from 464 on describe the frame's own extended state and stay as the kernel wrote them.
		if (operation == PTRACE_GETFPREGS)
			std::memcpy(data, area, sizeof(user_fpregs_struct));
		else
			std::memcpy(area, data, 464);
		return 0;
	}
	case PTRACE_GETREGSET:
	case PTRACE_SETREGSET: {
		if (reinterpret_cast<uintptr_t>(address) != 0x202 /* NT_X86_XSTATE */)
			return failed(EINVAL);
		size_t size = 0;
		uint8_t *const area = extended_state(task, size);
		if (!area)
			return failed(EFAULT);
		auto &buffer = *static_cast<iovec *>(data);
		const size_t whole = std::max(task.xstate_size, size);
		if (operation == PTRACE_GETREGSET) {
			const size_t length = std::min(buffer.iov_len, whole);
			std::memcpy(buffer.iov_base, area, std::min(length, size));
			if (length > size)
				std::memset(static_cast<uint8_t *>(buffer.iov_base) + size, 0, length - size);
			buffer.iov_len = length;
			return 0;
		} else {
			// As the kernel's own: the whole state or nothing, in the standard layout, naming no component the frame does not carry.
			// rt_sigreturn would refuse anything else, and the thread would die of it.
			if (buffer.iov_len != whole)
				return failed(EINVAL);
			if (size > 512) {
				const auto *const given = static_cast<const uint8_t *>(buffer.iov_base);
				uint64_t features = 0, present = 0, compacted = 0;
				std::memcpy(&features, area + 464 + 8, 8);
				std::memcpy(&present, given + 512, 8);
				std::memcpy(&compacted, given + 520, 8);
				bool reserved = false;
				for (size_t index = 528; index < 576 && index < size; ++index)
					reserved |= given[index] != 0;
				if ((present & ~features) || compacted || reserved)
					return failed(EINVAL);
			}
			uint8_t software[48];
			std::memcpy(software, area + 464, sizeof software);
			std::memcpy(area, buffer.iov_base, size);
			std::memcpy(area + 464, software, sizeof software);
		}
		return 0;
	}
	case PTRACE_SETSIGINFO: {
		// What a stop is taken for, put back after an interrupt that came behind it (see NativeChild::pause_task).
		auto *information = on_stack<siginfo_t>(task, task.block[agent::information]);
		if (!information)
			return failed(EFAULT);
		*information = *static_cast<const siginfo_t *>(data);
		return 0;
	}
	case PTRACE_PEEKUSER:
	case PTRACE_POKEUSER:
		return controlled(task, operation, address, data);
	default:
		// Events and the rest have no counterpart in the agent.
		return failed(EIO);
	}
}

}

void hold(const Task &task)
{
	std::lock_guard lock(tasks_lock);
	tasks[task.task] = task;
	tasks[task.task].running = true;
}

void release_group(pid_t group)
{
	std::lock_guard lock(tasks_lock);
	std::erase_if(tasks, [&](const auto &entry) { return entry.second.group == group || entry.second.process == group; });
}

bool held(pid_t task)
{
	return find(task) != nullptr;
}

long request(enum __ptrace_request operation, pid_t task, void *address, void *data)
{
	if (Task *const found = find(task))
		return held_request(*found, operation, address, data);
	return ptrace(operation, task, address, data);
}

pid_t wait(pid_t task, int *status, int options)
{
	Task *const found = find(task);
	if (!found)
		return ::waitpid(task, status, options);
	Task &held = *found;
	for (;;) {
		const uint32_t now = __atomic_load_n(state_word(held), __ATOMIC_ACQUIRE);
		if (held.running && now == posted) {
			if (status)
				*status = stop_status(held);
			return task;
		}
		if (!held.running)
			return failed(ECHILD);
		if (ended(held, status)) {
			release_group(held.group);
			return task;
		}
		if (options & WNOHANG)
			return 0;
		// Asleep until the agent posts, a signal of the runtime's own cuts the wait short as it would cut waitpid's, or a while has
		// passed to look at whether the task is still there.
		const timespec slice{0, 10'000'000};
		if (syscall(SYS_futex, state_word(held), FUTEX_WAIT, now, &slice, nullptr, 0) == -1 && errno == EINTR)
			return -1;
	}
}

int interrupt(pid_t task)
{
	if (Task *const found = find(task))
		return int(syscall(SYS_tgkill, found->group, task, interrupt_signal));
	return kill(task, SIGSTOP);
}

bool stopped(pid_t task)
{
	Task *const found = find(task);
	return found && (!found->running || __atomic_load_n(state_word(*found), __ATOMIC_ACQUIRE) == posted);
}

}
