#pragma once

#include <csignal>
#include <cstdint>
#include <sys/user.h>
#include <ucontext.h>

// The registers of a thread held in the agent, as the runtime reads them from a stopped task: the interrupted context the kernel left
// on the thread's alternate stack, put in the shape PTRACE_GETREGS gives. What a signal frame does not carry comes from what the runtime
// already knows of the thread: the bases of FS and GS, and the data segments of a 32-bit thread.

namespace raven::agent {

struct Segments {
	uint64_t fs_base = 0, gs_base = 0, ds = 0, es = 0;
};

// The selectors the frame packs into one word: CS, GS, FS and then SS, which the kernel saves when UC_SIGCONTEXT_SS is set.
inline uint16_t selector(const ucontext_t &context, unsigned index)
{
	return uint16_t(uint64_t(context.uc_mcontext.gregs[REG_CSGSFS]) >> (16 * index));
}

// `call` is the system call a SIGSYS turned away, or -1, as orig_rax reads after any other stop.
inline user_regs_struct registers(const ucontext_t &context, const Segments &segments, int64_t call = -1)
{
	const auto *g = context.uc_mcontext.gregs;
	user_regs_struct r{};
	r.r15 = g[REG_R15];
	r.r14 = g[REG_R14];
	r.r13 = g[REG_R13];
	r.r12 = g[REG_R12];
	r.rbp = g[REG_RBP];
	r.rbx = g[REG_RBX];
	r.r11 = g[REG_R11];
	r.r10 = g[REG_R10];
	r.r9 = g[REG_R9];
	r.r8 = g[REG_R8];
	r.rax = g[REG_RAX];
	r.rcx = g[REG_RCX];
	r.rdx = g[REG_RDX];
	r.rsi = g[REG_RSI];
	r.rdi = g[REG_RDI];
	r.orig_rax = uint64_t(call);
	r.rip = g[REG_RIP];
	r.cs = selector(context, 0);
	r.eflags = g[REG_EFL];
	r.rsp = g[REG_RSP];
	r.ss = selector(context, 3);
	r.fs_base = segments.fs_base;
	r.gs_base = segments.gs_base;
	r.ds = segments.ds;
	r.es = segments.es;
	r.fs = selector(context, 2);
	r.gs = selector(context, 1);
	return r;
}

// Puts registers back into the frame, which rt_sigreturn then loads. A frame cannot change the bases of FS and GS or the data
// segments: a change to them is refused (false) and nothing is written.
inline bool set_registers(ucontext_t &context, const user_regs_struct &r, const Segments &segments)
{
	if (r.fs_base != segments.fs_base || r.gs_base != segments.gs_base || r.ds != segments.ds || r.es != segments.es)
		return false;
	auto *g = context.uc_mcontext.gregs;
	g[REG_R15] = int64_t(r.r15);
	g[REG_R14] = int64_t(r.r14);
	g[REG_R13] = int64_t(r.r13);
	g[REG_R12] = int64_t(r.r12);
	g[REG_RBP] = int64_t(r.rbp);
	g[REG_RBX] = int64_t(r.rbx);
	g[REG_R11] = int64_t(r.r11);
	g[REG_R10] = int64_t(r.r10);
	g[REG_R9] = int64_t(r.r9);
	g[REG_R8] = int64_t(r.r8);
	g[REG_RAX] = int64_t(r.rax);
	g[REG_RCX] = int64_t(r.rcx);
	g[REG_RDX] = int64_t(r.rdx);
	g[REG_RSI] = int64_t(r.rsi);
	g[REG_RDI] = int64_t(r.rdi);
	g[REG_RIP] = int64_t(r.rip);
	g[REG_EFL] = int64_t(r.eflags);
	g[REG_RSP] = int64_t(r.rsp);
	// CS and SS go back with the frame, so an answer can move a thread between 64-bit and 32-bit code; GS and FS are kept as they were.
	const uint64_t packed = uint64_t(g[REG_CSGSFS]);
	g[REG_CSGSFS] = int64_t((packed & 0x0000ffffffff0000ULL) | (r.cs & 0xffff) | (uint64_t(r.ss & 0xffff) << 48));
	context.uc_flags |= 0x2 /* UC_SIGCONTEXT_SS */;
	return true;
}

}
