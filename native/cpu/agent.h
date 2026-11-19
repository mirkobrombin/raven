#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

// The code of agent.S, which the runtime copies into a guest, and the offsets within it the runtime and the guest agree on.
extern "C" const uint8_t raven_agent[], raven_agent_end[], raven_agent_handler[], raven_agent_restorer[];

namespace raven::agent {

// The size and alignment of a thread's alternate signal stack; its first word holds the address of the thread's block.
constexpr size_t stack_size = 65536;
// The words of a thread's block the agent and the runtime use (see agent.S).
constexpr size_t state = 400, signal = 401, information = 402, context = 403;
// The values of the state word.
constexpr uint32_t idle = 0, posted = 1, answered = 2;

inline std::span<const uint8_t> code() { return {raven_agent, size_t(raven_agent_end - raven_agent)}; }
inline size_t handler() { return size_t(raven_agent_handler - raven_agent); }
inline size_t restorer() { return size_t(raven_agent_restorer - raven_agent); }

}
