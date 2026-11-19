#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

// The code of agent.S, which the runtime copies into a guest, and the offsets within it the runtime and the guest agree on.
extern "C" const uint8_t raven_agent[], raven_agent_end[], raven_agent_handler[], raven_agent_restorer[], raven_agent_service[],
	raven_agent_spawn[], raven_agent_spawned[], raven_agent_park[], raven_agent_born[];

namespace raven::agent {

// The size and alignment of a thread's alternate signal stack; its first word holds the address of the thread's block.
constexpr size_t stack_size = 65536;
// The words of a thread's block the agent and the runtime use (see agent.S).
constexpr size_t state = 400, signal = 401, information = 402, context = 403;
// The segment registers the runtime asks the handler to set before the thread goes on: which ones, the FS and GS bases, and the FS,
// GS, DS and ES selectors (loaded first).
constexpr size_t set_bases = 404, fs_base = 405, gs_base = 406, fs_selector = 408, gs_selector = 409, ds_selector = 410,
	es_selector = 411;
constexpr uint64_t set_fs = 1, set_gs = 2, load_fs = 4, load_gs = 8, load_ds = 16, load_es = 32;
// A thread the service thread starts reads its alternate stack (a stack_t), the range Syscall User Dispatch lets through and the
// selector from its block, and leaves there the results of setting them up.
constexpr size_t born_stack = 504, born_range = 508, born_results = 510;
// The values of the state word.
constexpr uint32_t idle = 0, posted = 1, answered = 2;

inline std::span<const uint8_t> code() { return {raven_agent, size_t(raven_agent_end - raven_agent)}; }
inline size_t handler() { return size_t(raven_agent_handler - raven_agent); }
inline size_t restorer() { return size_t(raven_agent_restorer - raven_agent); }
inline size_t service() { return size_t(raven_agent_service - raven_agent); }
// The routine that starts the service thread, and its breakpoint the starting task stops on.
inline size_t spawn() { return size_t(raven_agent_spawn - raven_agent); }
inline size_t spawned() { return size_t(raven_agent_spawned - raven_agent); }
// The breakpoint a traced task is sent to when it is handed to the agent.
inline size_t park() { return size_t(raven_agent_park - raven_agent); }
// The breakpoint a thread the service thread started stops on before it has a context of its own.
inline size_t born() { return size_t(raven_agent_born - raven_agent); }

// The words of the service thread's block (see agent.S): the state, the call's number, its six arguments and its result.
namespace service_block {
constexpr size_t state = 0, number = 1, arguments = 2, result = 8;
// The agent's own call that starts a guest thread (see agent.S): the top of its stack and its block.
constexpr uint64_t start_thread = 1000;
// The agent's own call that starts the guest's warden (see agent.S), which ends the guest's process group once the runtime's process,
// named by its id, ends.
constexpr uint64_t start_warden = 1001;
constexpr uint32_t idle = 0, waiting = 1, done = 2;
}

}
