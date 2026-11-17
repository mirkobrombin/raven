#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

// The code of gates.S, which the runtime copies into a guest, and the offsets within it the runtime and the guest agree on.
extern "C" const uint8_t raven_served_stub[], raven_served_stub_end[], raven_served_stub_store[], raven_served_stub_spin[],
	raven_served_stub_wait[], raven_served_stub_tail[], raven_served_stub_yield[], raven_served_stub_trap[], raven_local_block[],
	raven_local_block_end[], raven_asked_tail[], raven_asked_tail_end[], raven_gate_page[], raven_gate_page_end[],
	raven_gate_exchange32[], raven_gate_exchange64[], raven_gate_exchange32_done[], raven_gate_exchange64_done[], raven_gate_request[],
	raven_gate_request_return[], raven_gate_request_exchange64[], raven_gate_request_exchange32[], raven_gate_request_syscall[], raven_gate_request_syscall_done[];

namespace raven::served_stub {

// The room each stub has: the code and the slack after it.
constexpr size_t size = 160;
// The words of a thread's block the stubs and the runtime stamp a call's times in (see gates.S).
constexpr size_t entered = 12, last_stub = 13, answered = 14, resumed = 15;
// The entry time of the last call a host thread answered beside the turn: the one call its stop may be let go on from.
constexpr size_t answered_call = 20;

inline std::span<const uint8_t> code() { return {raven_served_stub, size_t(raven_served_stub_end - raven_served_stub)}; }
inline std::span<const uint8_t> local_block() { return {raven_local_block, size_t(raven_local_block_end - raven_local_block)}; }
inline std::span<const uint8_t> asked_tail() { return {raven_asked_tail, size_t(raven_asked_tail_end - raven_asked_tail)}; }
inline std::span<const uint8_t> gate_page() { return {raven_gate_page, size_t(raven_gate_page_end - raven_gate_page)}; }
inline size_t offset(const uint8_t *label) { return size_t(label - raven_served_stub); }
inline size_t gate_offset(const uint8_t *label) { return size_t(label - raven_gate_page); }
// Where the stores begin, where a routine asked of the guest goes back to waiting, the breakpoint a long wait stops at, the tail, the
// breakpoint an answer that must stop takes, and the breakpoint the call falls back to.
inline size_t store() { return offset(raven_served_stub_store); }
inline size_t spin() { return offset(raven_served_stub_spin); }
inline size_t wait() { return offset(raven_served_stub_wait); }
inline size_t tail() { return offset(raven_served_stub_tail); }
inline size_t yield() { return offset(raven_served_stub_yield); }
inline size_t trap() { return offset(raven_served_stub_trap); }

}
