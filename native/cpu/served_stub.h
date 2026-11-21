#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

// The code of gates.S, which the runtime copies into a guest, and the offsets within it the runtime and the guest agree on.
extern "C" const uint8_t raven_served_stub[], raven_served_stub_end[], raven_served_stub_store[], raven_served_stub_spin[],
	raven_served_stub_wait[], raven_served_stub_tail[], raven_served_stub_yield[], raven_served_stub_trap[], raven_local_block[],
	raven_local_block_end[], raven_asked_tail[], raven_asked_tail_end[], raven_gate_page[], raven_gate_page_end[],
	raven_gate_exchange32[], raven_gate_exchange64[], raven_gate_exchange32_done[], raven_gate_exchange64_done[], raven_gate_request[],
	raven_gate_request_return[], raven_gate_request_exchange64[], raven_gate_request_exchange32[], raven_gate_request_syscall[], raven_gate_request_syscall_done[],
	raven_answer_wrapper[], raven_answer_wrapper_end[], raven_answer_wrapper_pointer[], raven_answer_wrapper_answer[],
	raven_answer_wrapper_stub[], raven_answer_wrapper_gives_up[];

namespace raven::served_stub {

// The room each stub has: the code and the slack after it.
constexpr size_t size = 160;
// The words of a thread's block the stubs and the runtime stamp a call's times in (see gates.S).
constexpr size_t entered = 12, last_stub = 13, answered = 14, resumed = 15;
// The words the guest stamps a system call the agent takes in, with RAVEN_CYCLES (see raven_service_timed in preludes.S): as it asks for
// the service and as it goes on after the answer; where the runtime leaves the address the guest goes on at, and the call it answered.
constexpr size_t service_asked = 496, service_resumed = 497, service_landing_target = 498, service_last = 499;

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

// The wrapper of a call the guest may answer itself, and the offsets in it: where each rel32 field the runtime fills in ends (the served
// block's pointer, twice, the answer, the stub, twice) and where a wrapper that gave up on the answer goes on.
inline std::span<const uint8_t> answer_wrapper() { return {raven_answer_wrapper, size_t(raven_answer_wrapper_end - raven_answer_wrapper)}; }
inline size_t answer_offset(const uint8_t *label) { return size_t(label - raven_answer_wrapper); }
inline size_t answer_pointer() { return answer_offset(raven_answer_wrapper_pointer); }
inline size_t answer_call() { return answer_offset(raven_answer_wrapper_answer); }
inline size_t answer_stub() { return answer_offset(raven_answer_wrapper_stub); }
inline size_t answer_end() { return answer_offset(raven_answer_wrapper_end); }
inline size_t answer_gives_up() { return answer_offset(raven_answer_wrapper_gives_up); }

}
