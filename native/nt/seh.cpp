#include <algorithm>
#include "kernel_context.h"
#include "exceptions.h"
#include "unwind.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <unordered_map>
#include <iostream>

namespace raven {

namespace {

constexpr size_t record_size = 152;
constexpr uint32_t flag_noncontinuable = 1, flag_unwinding = 2, flag_exit_unwind = 4, flag_target_unwind = 0x20, flag_nested = 0x10;
constexpr uint32_t status_unwind_consolidate = 0x80000029U, status_unwind = 0xc0000027U, status_noncontinuable = 0xc0000025U, status_invalid_disposition = 0xc0000026U;
// DISPATCHER_CONTEXT is 80 bytes; the fields below are its byte offsets.
constexpr size_t dispatcher_size = 80;

uint64_t load64(const std::vector<uint8_t> &bytes, size_t at)
{
	uint64_t value;
	std::memcpy(&value, bytes.data() + at, 8);
	return value;
}

uint32_t le32(const std::vector<uint8_t> &bytes, size_t at)
{
	uint32_t value;
	std::memcpy(&value, bytes.data() + at, 4);
	return value;
}

void put32(std::vector<uint8_t> &bytes, size_t at, uint32_t value)
{
	std::memcpy(bytes.data() + at, &value, 4);
}

void put64(std::vector<uint8_t> &bytes, size_t at, uint64_t value)
{
	std::memcpy(bytes.data() + at, &value, 8);
}

// Bump allocation of guest scratch memory for records and contexts; released when the outermost dispatch ends.
class Scratch {
public:
	explicit Scratch(KernelContext &context) : context(context) {}
	// Gives the memory back, once the thread that used it has ended.
	void free()
	{
		if (base)
			context.memory.free(base, 0, 0x8000);
		base = 0;
		top = 0;
	}
	Scratch(const Scratch &) = delete;
	Scratch &operator=(const Scratch &) = delete;
	uint64_t take(size_t size)
	{
		if (!base) {
			const auto range = context.memory.allocate(0, capacity, 0x3000, 4);
			if (range.status)
				throw std::runtime_error("native exception dispatch cannot reserve guest memory");
			base = range.address;
		}
		top = (top + 15) & ~size_t(15);
		if (top + size > capacity)
			throw std::runtime_error("native exception dispatch exhausted its guest memory");
		const uint64_t address = base + top;
		top += size;
		return address;
	}
	size_t mark() const { return top; }
	void release(size_t to) { top = to; }

private:
	KernelContext &context;
	uint64_t base = 0;
	size_t top = 0;
	static constexpr size_t capacity = 1 << 20;
};

}

class Exceptions {
public:
	Exceptions(KernelContext &context, NativeUnwinder &unwinder) : context(context), unwinder(unwinder) {}
	// Where a hardware fault's dispatcher frame put the exception's CONTEXT and record on the faulting thread's stack, as Windows does
	// below the faulting stack pointer; the next raise takes them (see raise_fault).
	struct StackFrame {
		uint64_t context, record;
	};
	// The address of the RaiseException stub, where the exceptions it raises seem to come from.
	uint64_t raise_stub = 0;

	bool trace = std::getenv("RAVEN_TRACE_SEH") != nullptr;
	uint64_t trap_gate = 0;

	// Walks the stack from the raising frame; true when a handler resumed execution through `resume`.
	bool dispatch(std::vector<uint8_t> record, ContextBytes origin, ContextBytes &resume, std::optional<StackFrame> on_stack = std::nullopt)
	{
		const size_t mark = mine().scratch.mark();
		const uint64_t guest_record = on_stack ? on_stack->record : mine().scratch.take(record_size);
		const uint64_t dispatcher = mine().scratch.take(dispatcher_size);
		const uint64_t frame_context = mine().scratch.take(x64_context::size);
		// As RtlDispatchException passes them: the handler's ContextRecord is the context the exception happened in, which execution
		// resumes from when a handler continues it, and DispatcherContext->ContextRecord is the frame's context unwound to its caller.
		const uint64_t exception_context = on_stack ? on_stack->context : mine().scratch.take(x64_context::size);
		context.process.write_memory(guest_record, record);
		context.process.write_memory(exception_context, origin);
		mine().active.push_back({origin, context.process.callback_depth(), mark});
		if (trace)
		{
			std::fprintf(stderr, "seh dispatch begin: code=%x active=%zu depth=%zu scratch=%zx params=%x info1=%llx info2=%llx rsp=%llx\n", le32(record, 0), mine().active.size(),
				context.process.callback_depth(), mine().scratch.mark(), le32(record, 0x18), (unsigned long long)load64(record, 0x28), (unsigned long long)load64(record, 0x30),
				(unsigned long long)context_rsp(origin));
			if (le32(record, 0x18) >= 2) {
				try {
					const auto data = context.process.read_memory(load64(record, 0x28), 96);
					std::fprintf(stderr, "seh object:");
					for (const auto byte : data) std::fprintf(stderr, " %02x", byte);
					std::fprintf(stderr, "\n");
				} catch (const std::runtime_error &) {
				}
			}
		}
		bool handled = false;
		ContextBytes current = origin;
		size_t continuation_cursor = mine().continuation_start;
		// An exception raised inside a handler continues its search through the dispatcher's frames, which the host callback
		// boundary stands for, into the context of the exception that the handler was running for.
		size_t outer = mine().active.size() - 1;
		// The callback nesting of the frames being searched: past the return gate of a callback that no dispatch made, the search goes
		// on in the frames of the API call that made it, as Windows searches past a window procedure into the caller of DispatchMessage.
		size_t level = mine().active.back().depth;
		for (int frames = 0; frames < 1024; ++frames) {
			const uint64_t pc = context_rip(current);
			if (!pc)
				break;
			const auto function = unwinder.lookup(pc);
			if (!function && pc == context.process.return_gate() && outer == 0 && continuation_cursor >= mine().continuation.size() && level > 0) {
				current = context_from_process(context.process.callback_origin(--level));
				if (trace)
					std::fprintf(stderr, "seh frame: callback boundary, continuing at level %zu from %llx\n", level, (unsigned long long)context_rip(current));
				continue;
			}
			if (!function) {
				// A catch block's callback returns through the consolidation gate: the frames beyond it are those of the function
				// whose catch block is running, from where it was unwound to.
				if (const auto *pending = consolidation_returning_to(pc, current)) {
					current = pending->target;
					continue;
				}
				if (pc == context.process.return_gate() && outer > 0) {
					current = mine().active[--outer].origin;
					level = mine().active[outer].depth;
					continue;
				}
				// A search raised from a handler that an unwind is running continues in the frames of whoever called the unwind,
				// then in the contexts of the dispatches that the unwind has already crossed.
				if (pc == context.process.return_gate() && continuation_cursor < mine().continuation.size()) {
					level = mine().continuation_depths[continuation_cursor];
					current = mine().continuation[continuation_cursor++];
					continue;
				}
				if (trace)
					std::fprintf(stderr, "seh frame: pc=%llx has no unwind data%s\n", (unsigned long long)pc, pc == context.process.return_gate() ? " (callback boundary)" : "");
				// With no outer dispatch the boundary ends the search: no frames of the guest's caller are on this stack.
				if (pc == context.process.return_gate() || !readable(pc))
					break;
				unwinder.unwind_leaf(current);
				continue;
			}
			ContextBytes next = current;
			const auto result = unwinder.unwind(1, pc, *function, next);
			if (trace)
				std::fprintf(stderr, "seh frame: pc=%llx function=%x..%x unwind=%x handler=%llx\n", (unsigned long long)pc, function->begin, function->end,
					function->unwind, (unsigned long long)result.handler);
			if (result.handler) {
				context.process.write_memory(frame_context, next);
				std::vector<uint8_t> descriptor(dispatcher_size);
				put64(descriptor, 0, pc);
				put64(descriptor, 8, function->image_base);
				put64(descriptor, 0x10, function->address);
				put64(descriptor, 0x18, result.establisher);
				put64(descriptor, 0x20, 0);
				put64(descriptor, 0x28, frame_context);
				put64(descriptor, 0x30, result.handler);
				put64(descriptor, 0x38, result.handler_data);
				put64(descriptor, 0x40, 0);
				bool repeat = true;
				while (repeat && !handled) {
					repeat = false;
					const uint64_t handler = load64(descriptor, 0x30), establisher = load64(descriptor, 0x18);
					context.process.write_memory(dispatcher, descriptor);
					if (trace)
						std::fprintf(stderr, "seh: pc=%llx handler=%llx frame=%llx\n", (unsigned long long)load64(descriptor, 0), (unsigned long long)handler,
							(unsigned long long)establisher);
					const uint64_t arguments[] = {guest_record, establisher, exception_context, dispatcher};
					const auto outcome = context.call(handler, arguments);
					if (outcome.stop != NativeStop::returned)
						throw outcome;
					const uint32_t disposition = uint32_t(outcome.value);
					if (disposition == 0) {
						if (le32(record, 4) & flag_noncontinuable)
							throw std::runtime_error("native exception handler continued a noncontinuable exception");
						resume = context.process.read_memory(exception_context, x64_context::size);
						handled = true;
					} else if (disposition == 3) {
						// A collided unwind: the handler belongs to a frame that stands for another dispatch, such as the stub that
						// the .NET runtime raises a fault from, and has copied that dispatch's state into the dispatcher context. The
						// search goes on with that frame's handler and, after it, from the frame's caller the context describes.
						descriptor = context.process.read_memory(dispatcher, dispatcher_size);
						next = context.process.read_memory(load64(descriptor, 0x28), x64_context::size);
						context.process.write_memory(frame_context, next);
						put64(descriptor, 0x28, frame_context);
						repeat = true;
					} else if (disposition != 1 && disposition != 2) {
						throw std::runtime_error("native exception handler at " + std::to_string(handler) + " for pc " + std::to_string(pc) +
							" returned the invalid disposition " + std::to_string(disposition));
					}
				}
				if (handled)
					break;
			}
			current = next;
		}
		if (trace)
			std::fprintf(stderr, "seh dispatch end: handled=%d active=%zu\n", handled, mine().active.size());
		mine().active.pop_back();
		mine().scratch.release(mark);
		return handled;
	}

	// What a trace says of an exception beyond its code: the address an access violation touched, or the parameters of any other kind
	// that is not a C++ throw, such as the HRESULT of a .NET exception.
	static void describe_record(const std::vector<uint8_t> &record, uint64_t rsp)
	{
		const uint32_t code = le32(record, 0), count = std::min<uint32_t>(le32(record, 0x18), 15);
		if (code == 0xc0000005U && count >= 2) {
			std::fprintf(stderr, "raven:   %s %llx with rsp %llx\n", load64(record, 0x20) == 1 ? "writing" : load64(record, 0x20) == 8 ? "running" :
				"reading", (unsigned long long)load64(record, 0x28), (unsigned long long)rsp);
		} else if (code != 0xc0000005U && code != 0xe06d7363U && count) {
			std::fprintf(stderr, "raven:   parameters");
			for (uint32_t index = 0; index < count; ++index)
				std::fprintf(stderr, " %llx", (unsigned long long)load64(record, 0x20 + index * 8));
			std::fprintf(stderr, "\n");
		}
	}

	// Handles one exception end to end; either redirects the guest (throwing ExecutionRedirected) or ends the process.
	[[noreturn]] void raise(std::vector<uint8_t> record, ContextBytes origin, const std::function<NativeResult()> &unhandled)
	{
		const auto on_stack = mine().fault_frame;
		mine().fault_frame.reset();
		if (std::getenv("RAVEN_WATCH_EXCEPTION") && le32(record, 0x18) >= 2) {
			// Diagnosis aid: report every write to the second word of the thrown object.
			uint64_t object;
			std::memcpy(&object, record.data() + 0x28, 8);
			context.process.watch_write(object);
		}
		// RAVEN_TRACE_EXCEPTIONS names every exception when it is raised, before any handler sees it, with the code addresses on its stack.
		if (std::getenv("RAVEN_TRACE_EXCEPTIONS")) {
			const auto *module = unwinder.module_at(context_rip(origin));
			static const auto started = std::chrono::steady_clock::now();
			std::fprintf(stderr, "raven: exception %08x at %llx (%s+%llx) raised on thread %u at %lld ms\n", le32(record, 0),
				(unsigned long long)context_rip(origin), module ? module->name.c_str() : "unknown",
				module ? (unsigned long long)(context_rip(origin) - module->loaded.base()) : 0ULL, unsigned(context.threads.current_id()),
				(long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
			uint64_t rsp = 0;
			std::memcpy(&rsp, origin.data() + 0x98, 8);
			describe_record(record, rsp);
			try {
				const auto stack = context.process.read_memory(rsp, 0x300);
				for (size_t at = 0; at + 8 <= stack.size(); at += 8) {
					uint64_t value = 0;
					std::memcpy(&value, stack.data() + at, 8);
					if (const auto *owner = unwinder.module_at(value))
						std::fprintf(stderr, "raven:   +%zx %s+%llx\n", at, owner->name.c_str(), (unsigned long long)(value - owner->loaded.base()));
				}
			} catch (const std::exception &) {
			}
		}
		ContextBytes resume;
		const size_t depth = context.process.callback_depth();
		// A vectored handler that answers EXCEPTION_CONTINUE_EXECUTION resumes the program where it stopped, with the context it left.
		if (!vectored.empty()) {
			const size_t mark = mine().scratch.mark();
			const uint64_t guest_record = on_stack ? on_stack->record : mine().scratch.take(record_size);
			const uint64_t guest_context = on_stack ? on_stack->context : mine().scratch.take(x64_context::size), pointers = mine().scratch.take(16);
			context.process.write_memory(guest_record, record);
			context.process.write_memory(guest_context, origin);
			std::vector<uint8_t> block(16);
			put64(block, 0, guest_record);
			put64(block, 8, guest_context);
			context.process.write_memory(pointers, block);
			const auto handlers = vectored;
			for (const auto &item : handlers) {
				if (item.after)
					continue;
				const uint64_t arguments[] = {pointers};
				const auto outcome = context.call(item.handler, arguments);
				if (outcome.stop != NativeStop::returned)
					throw outcome;
				if (int32_t(outcome.value) == -1) {
					resume = context.process.read_memory(guest_context, x64_context::size);
					mine().scratch.release(mark);
					redirect_to(resume, depth);
				}
			}
			mine().scratch.release(mark);
		}
		if (dispatch(record, origin, resume, on_stack))
			redirect_to(resume, depth);
		// An exception no frame handled goes to the program's last-chance filter, which usually ends the process quietly: with
		// RAVEN_BACKTRACE set it is named first, with the code addresses on its stack.
		if (std::getenv("RAVEN_BACKTRACE")) {
			const auto *module = unwinder.module_at(context_rip(origin));
			std::fprintf(stderr, "raven: exception %08x at %llx (%s+%llx) reaches the unhandled exception filter\n", le32(record, 0),
				(unsigned long long)context_rip(origin), module ? module->name.c_str() : "unknown",
				module ? (unsigned long long)(context_rip(origin) - module->loaded.base()) : 0ULL);
			uint64_t rsp = 0;
			std::memcpy(&rsp, origin.data() + 0x98, 8);
			describe_record(record, rsp);
			try {
				const auto stack = context.process.read_memory(rsp, 0x400);
				for (size_t at = 0; at + 8 <= stack.size(); at += 8) {
					uint64_t value = 0;
					std::memcpy(&value, stack.data() + at, 8);
					if (const auto *owner = unwinder.module_at(value))
						std::fprintf(stderr, "raven:   +%zx %s+%llx\n", at, owner->name.c_str(), (unsigned long long)(value - owner->loaded.base()));
				}
			} catch (const std::exception &) {
			}
		}
		if (context.unhandled_filter) {
			const size_t mark = mine().scratch.mark();
			const uint64_t guest_record = mine().scratch.take(record_size), guest_context = mine().scratch.take(x64_context::size), pointers = mine().scratch.take(16);
			context.process.write_memory(guest_record, record);
			context.process.write_memory(guest_context, origin);
			std::vector<uint8_t> block(16);
			put64(block, 0, guest_record);
			put64(block, 8, guest_context);
			context.process.write_memory(pointers, block);
			const uint64_t arguments[] = {pointers};
			const auto outcome = context.call(context.unhandled_filter, arguments);
			if (outcome.stop != NativeStop::returned)
				throw outcome;
			if (int32_t(outcome.value) == -1) {
				resume = context.process.read_memory(guest_context, x64_context::size);
				mine().scratch.release(mark);
				redirect_to(resume, depth);
			}
			mine().scratch.release(mark);
		}
		const uint32_t code = le32(record, 0);
		const auto *module = unwinder.module_at(context_rip(origin));
		std::fprintf(stderr, "raven: unhandled exception %08x at %llx (%s+%llx)\n", code, (unsigned long long)context_rip(origin),
			module ? module->name.c_str() : "unknown", module ? (unsigned long long)(context_rip(origin) - module->loaded.base()) : 0ULL);
		if (std::getenv("RAVEN_BACKTRACE")) {
			// Registers and the code addresses found on the stack, which is enough to name the callers of a fault.
			const auto reg = [&](size_t at) { uint64_t value = 0; std::memcpy(&value, origin.data() + at, 8); return value; };
			std::fprintf(stderr, "raven: rax %llx rbx %llx rcx %llx rdx %llx rsi %llx rdi %llx rsp %llx\n", (unsigned long long)reg(0x78),
				(unsigned long long)reg(0x90), (unsigned long long)reg(0x80), (unsigned long long)reg(0x88), (unsigned long long)reg(0xa8),
				(unsigned long long)reg(0xb0), (unsigned long long)reg(0x98));
			std::fprintf(stderr, "raven: rbp %llx r8 %llx r9 %llx r10 %llx r11 %llx r12 %llx r13 %llx r14 %llx r15 %llx\n", (unsigned long long)reg(0xa0),
				(unsigned long long)reg(0xb8), (unsigned long long)reg(0xc0), (unsigned long long)reg(0xc8), (unsigned long long)reg(0xd0),
				(unsigned long long)reg(0xd8), (unsigned long long)reg(0xe0), (unsigned long long)reg(0xe8), (unsigned long long)reg(0xf0));
			// Registers that point at text, which often names what the failing code was looking for.
			for (const size_t at : {0x78, 0x80, 0x88, 0x90, 0xa0, 0xa8, 0xb0, 0xb8, 0xc0, 0xc8, 0xd0, 0xd8, 0xe0, 0xe8, 0xf0})
				if (const auto text = describe_text(context.process, reg(at)); !text.empty())
					std::fprintf(stderr, "raven:   register at %zx holds <%s>\n", at, text.c_str());
			try {
				const auto stack = context.process.read_memory(reg(0x98), 0x800);
				for (size_t at = 0; at + 8 <= stack.size(); at += 8) {
					uint64_t value = 0;
					std::memcpy(&value, stack.data() + at, 8);
					if (const auto *owner = unwinder.module_at(value))
						std::fprintf(stderr, "raven:   +%zx %s+%llx\n", at, owner->name.c_str(), (unsigned long long)(value - owner->loaded.base()));
				}
			} catch (const std::exception &) {
			}
		}
		throw unhandled();
	}

	// Execution resumes at the callback nesting where the exception was raised; handler calls above it are abandoned.
	[[noreturn]] void redirect_to(const ContextBytes &given, size_t depth, size_t mark = SIZE_MAX)
	{
		auto target = given;
		// Continuing an exception RaiseException raised returns from RaiseException to its caller.
		if (raise_stub && context_rip(target) == raise_stub) {
			uint64_t rip = 0;
			const auto top = context.process.read_memory(context_rsp(target), 8);
			std::memcpy(&rip, top.data(), 8);
			set_context_rip(target, rip);
			set_context_rsp(target, context_rsp(target) + 8);
		}
		auto native = context.process.context();
		apply_context(native, target);
		context.process.redirect(native, depth);
		if (const char *count = std::getenv("RAVEN_STEP_REDIRECT")) {
			// Diagnosis aid: log the instructions executed right after execution is redirected.
			static int redirects = 0;
			if (++redirects == (std::getenv("RAVEN_STEP_NTH") ? std::atoi(std::getenv("RAVEN_STEP_NTH")) : 1))
				context.process.trace_instructions(size_t(std::atoi(count)), [this](uint64_t rip) {
					const auto *module = unwinder.module_at(rip);
					std::fprintf(stderr, "step %s+%llx\n", module ? module->name.c_str() : "?", module ? (unsigned long long)(rip - module->loaded.base()) : (unsigned long long)rip);
				});
		}
		// Guest memory of the dispatches that this jump abandons can be reused; outer dispatches keep theirs. Dispatches begun at or above
		// the nesting execution resumes at are over: their host frames are left by the jump.
		auto &state = mine();
		size_t abandoned = SIZE_MAX;
		while (!state.active.empty() && state.active.back().depth >= depth) {
			abandoned = std::min(abandoned, state.active.back().mark);
			state.active.pop_back();
		}
		while (!state.unwinds.empty() && state.unwinds.back().depth >= depth) {
			abandoned = std::min(abandoned, state.unwinds.back().mark);
			state.unwinds.pop_back();
		}
		// Catch blocks whose frames lie below the stack the jump goes to were left by it, as a rethrow leaves the catch it runs in.
		std::erase_if(state.consolidations, [&](const auto &pending) {
			if (pending.return_slot >= context_rsp(target))
				return false;
			abandoned = std::min(abandoned, pending.mark);
			return true;
		});
		if (!state.consolidations.empty() && state.consolidations.back().return_slot == context_rsp(target)) {
			// Entering a catch block: what the jump abandons goes back when the block returns.
			auto &entering = state.consolidations.back();
			entering.mark = std::min(entering.mark, abandoned);
		} else {
			// A jump that no unwind ended, such as the .NET runtime's restore of the context after a catch, leaves the memory of
			// the dispatches and unwinds it abandons too; the records of catch blocks still running stay.
			if (abandoned != SIZE_MAX) {
				size_t floor = abandoned;
				for (const auto &pending : state.consolidations)
					floor = std::max(floor, pending.end);
				mark = std::min(mark, floor);
			}
			if (mark != SIZE_MAX)
				state.scratch.release(std::min(mark, state.scratch.mark()));
		}
		if (trace)
			std::fprintf(stderr, "seh redirect to %llx at nesting %zu of %zu, scratch %zx, %zu dispatches\n", (unsigned long long)context_rip(target), depth,
				context.process.callback_depth(), state.scratch.mark(), state.active.size());
		throw ExecutionRedirected{depth};
	}

	// The caller's context at a trapped API call: the return address on top of the stack is the next instruction.
	ContextBytes caller_context()
	{
		auto native = context.process.context();
		auto bytes = context_from_process(native);
		const uint64_t rsp = context_rsp(bytes);
		const auto top = context.process.read_memory(rsp, 8);
		uint64_t rip;
		std::memcpy(&rip, top.data(), 8);
		set_context_rip(bytes, rip);
		set_context_rsp(bytes, rsp + 8);
		return bytes;
	}

	std::vector<uint8_t> new_record(uint32_t code, uint32_t flags, uint64_t address, std::span<const uint64_t> parameters)
	{
		std::vector<uint8_t> record(record_size);
		put32(record, 0, code);
		put32(record, 4, flags);
		put64(record, 0x10, address);
		put32(record, 0x18, uint32_t(parameters.size()));
		for (size_t index = 0; index < parameters.size(); ++index)
			put64(record, 0x20 + index * 8, parameters[index]);
		return record;
	}

	// Second pass: runs termination handlers frame by frame up to the target, then resumes there.
	// The caller's own record, when it gave one, is the one every handler sees and whose flags change, as on Windows: a runtime knows its
	// exception again in the second pass by that record's address.
	[[noreturn]] void unwind(uint64_t target_frame, uint64_t target_ip, std::vector<uint8_t> record, uint64_t return_value, uint64_t caller_record = 0)
	{
		put32(record, 4, le32(record, 4) | flag_unwinding | (target_frame ? 0 : flag_exit_unwind));
		// Crossing a host callback boundary consumes the dispatch it belongs to; the last one consumed says where execution resumes.
		size_t depth = context.process.callback_depth();
		const size_t mark = mine().scratch.mark();
		size_t resume_mark = mark;
		mine().unwinds.push_back({{}, depth, mark});
		const uint64_t guest_record = caller_record ? caller_record : mine().scratch.take(record_size);
		const uint64_t dispatcher = mine().scratch.take(dispatcher_size);
		const uint64_t frame_context = mine().scratch.take(x64_context::size);
		ContextBytes current = caller_context();
		// The deepest stack in use: what the frames being unwound left there, the thrown object among it, stays intact below the target.
		const uint64_t deepest = context_rsp(current);
		const size_t previous_start = mine().continuation_start, previous_size = mine().continuation.size();
		size_t outer_cursor = previous_start;
		mine().continuation_start = mine().continuation.size();
		mine().continuation.push_back(current);
		mine().continuation_depths.push_back(depth);
		struct Release {
			std::vector<ContextBytes> &stack;
			std::vector<size_t> &depths;
			size_t &start, previous_start, previous_size;
			~Release()
			{
				stack.resize(previous_size);
				depths.resize(previous_size);
				start = previous_start;
			}
		} release{mine().continuation, mine().continuation_depths, mine().continuation_start, previous_start, previous_size};
		bool reached = false;
		for (int frames = 0; frames < 1024 && !reached; ++frames) {
			const uint64_t pc = context_rip(current);
			if (pc == context.process.return_gate() && !mine().active.empty()) {
				// The host callback boundary stands in for the dispatcher's own frames: continue from where the exception was raised.
				current = mine().active.back().origin;
				depth = mine().active.back().depth;
				mine().continuation.push_back(current);
				mine().continuation_depths.push_back(depth);
				resume_mark = std::min(resume_mark, mine().active.back().mark);
				mine().unwinds.back().mark = resume_mark;
				mine().active.pop_back();
				continue;
			}
			// With no dispatch left to cross, an unwind raised from inside another unwind continues through the contexts
			// that the enclosing unwind recorded.
			// Execution resumes no deeper than the callback nesting those contexts were left at.
			if (pc == context.process.return_gate() && outer_cursor < previous_size) {
				depth = std::min(depth, mine().continuation_depths[outer_cursor]);
				current = mine().continuation[outer_cursor++];
				continue;
			}
			// Past the return gate of a callback that no dispatch made, the frames are those of the API call that made it; execution
			// resumes at that nesting, abandoning the callback.
			if (pc == context.process.return_gate() && depth > 0) {
				current = context_from_process(context.process.callback_origin(--depth));
				continue;
			}
			if (!pc)
				break;
			const auto function = unwinder.lookup(pc);
			if (!function) {
				// Unwinding past a catch block's callback abandons that catch: its function's frame follows.
				if (const auto *pending = consolidation_returning_to(pc, current)) {
					current = pending->target;
					mine().consolidations.erase(mine().consolidations.begin() + (pending - mine().consolidations.data()), mine().consolidations.end());
					continue;
				}
				unwinder.unwind_leaf(current);
				continue;
			}
			ContextBytes next = current;
			const auto result = unwinder.unwind(2, pc, *function, next);
			const bool at_target = target_frame && result.establisher == target_frame;
			if (trace)
				std::fprintf(stderr, "seh unwind frame: pc=%llx fn=%x..%x establisher=%llx rsp=%llx->%llx handler=%llx%s\n", (unsigned long long)pc, function->begin,
					function->end, (unsigned long long)result.establisher, (unsigned long long)context_rsp(current), (unsigned long long)context_rsp(next),
					(unsigned long long)result.handler, at_target ? " TARGET" : "");
			if (target_frame && result.establisher > target_frame && !at_target) {
				std::fprintf(stderr, "raven: unwind passed its target frame\n");
				context.process.abandon_call();
				throw ProcessExit{status_invalid_disposition, pc, true};
			}
			if (result.handler) {
				put32(record, 4, (le32(record, 4) & ~flag_target_unwind) | (at_target ? flag_target_unwind : 0));
				context.process.write_memory(guest_record, record);
				context.process.write_memory(frame_context, current);
				std::vector<uint8_t> descriptor(dispatcher_size);
				put64(descriptor, 0, pc);
				put64(descriptor, 8, function->image_base);
				put64(descriptor, 0x10, function->address);
				put64(descriptor, 0x18, result.establisher);
				put64(descriptor, 0x20, target_ip);
				put64(descriptor, 0x28, frame_context);
				put64(descriptor, 0x30, result.handler);
				put64(descriptor, 0x38, result.handler_data);
				context.process.write_memory(dispatcher, descriptor);
				if (trace)
					std::fprintf(stderr, "seh unwind: pc=%llx handler=%llx frame=%llx%s\n", (unsigned long long)pc, (unsigned long long)result.handler,
						(unsigned long long)result.establisher, at_target ? " target" : "");
				const uint64_t arguments[] = {guest_record, result.establisher, frame_context, dispatcher};
				const auto outcome = context.call(result.handler, arguments);
				if (outcome.stop != NativeStop::returned)
					throw outcome;
				// The handler of the target frame owns the context that execution resumes from, and C++ frame handlers
				// leave the continuation address of the catch block they ran in the dispatcher's TargetIp.
				if (at_target || uint32_t(outcome.value) == 3)
					current = context.process.read_memory(frame_context, x64_context::size);
				if (at_target)
					target_ip = load64(context.process.read_memory(dispatcher, dispatcher_size), 0x20);
			}
			if (at_target) {
				reached = true;
				break;
			}
			current = next;
		}
		if (!reached && target_frame) {
			std::fprintf(stderr, "raven: unwind never reached its target frame\n");
			context.process.abandon_call();
			throw ProcessExit{status_invalid_disposition, context_rip(current), true};
		}
		// Dispatches the unwind never crossed, because their handler's frames lead straight back to the raise, end with it as well.
		for (const auto &item : mine().active)
			if (item.depth >= depth)
				resume_mark = std::min(resume_mark, item.mark);
		if (trace)
			std::fprintf(stderr, "seh resume at %llx (context rip %llx) rsp=%llx\n", (unsigned long long)target_ip, (unsigned long long)context_rip(current),
				(unsigned long long)context_rsp(current));
		if (le32(record, 0) == status_unwind_consolidate && trap_gate) {
			// Run the runtime's callback below every frame the exception went through, as Windows runs it from inside the unwind: the
			// catch block may still read the thrown object where the throw left it. Its return trap resumes execution.
			const uint64_t callback = load64(record, 0x20);
			const uint64_t base = (std::min(context_rsp(current), deepest) - record_size - 0x40) & ~uint64_t(15);
			context.process.write_memory(base, record);
			const uint64_t slot = base - 0x20 - 8;
			std::vector<uint8_t> gate(8);
			put64(gate, 0, trap_gate);
			context.process.write_memory(slot, gate);
			// The records of the dispatch stay where they are while the catch block runs: a rethrow inside it reaches the exception it
			// handles through them, as on Windows, where they are still on the stack. They are released when the callback returns.
			mine().consolidations.push_back({current, slot, depth, resume_mark, mine().scratch.mark()});
			auto entry = current;
			set_context_rip(entry, callback);
			set_context_rsp(entry, slot);
			set_context_register(entry, 1, base);
			redirect_to(entry, depth);
		}
		if (target_ip)
			set_context_rip(current, target_ip);
		set_context_register(current, 0, return_value);
		redirect_to(current, depth, resume_mark);
	}

	struct Active {
		ContextBytes origin;
		size_t depth;
		size_t mark;
	};
	// Vectored handlers, called before the frames of the stack are searched: a cookie for each, the handler and whether it is a continue handler.
	struct Vectored {
		uint64_t cookie, handler;
		bool after;
	};
	std::vector<Vectored> vectored;
	// A C++ catch is entered through STATUS_UNWIND_CONSOLIDATE: after unwinding, the runtime's callback runs on the
	// target frame's stack and returns the address where execution resumes. Each pending callback remembers the frame it belongs to.
	struct Consolidation {
		ContextBytes target;
		uint64_t return_slot;
		size_t depth;
		size_t mark;
		// Where the scratch memory in use when the catch began ends.
		size_t end;
	};
	// What one thread's exceptions are in the middle of. Threads take turns anywhere, inside a handler's callback too, so every
	// dispatch, unwind and pending catch belongs to the thread that started it, with its own scratch memory.
	struct ThreadState {
		explicit ThreadState(KernelContext &context) : scratch(context) {}
		std::optional<StackFrame> fault_frame;
		std::vector<Active> active;
		// The unwinds under way, with the start of the scratch memory each will give back, the dispatches it crossed included.
		std::vector<Active> unwinds;
		// The contexts a running unwind was called from and the dispatch contexts it has crossed, in the order a stack walk meets them.
		std::vector<ContextBytes> continuation;
		// The callback nesting at each of those contexts.
		std::vector<size_t> continuation_depths;
		size_t continuation_start = 0;
		std::vector<Consolidation> consolidations;
		Scratch scratch;
	};

	ThreadState &mine() const
	{
		const uint32_t id = context.threads.current_id();
		auto found = threads.find(id);
		if (found != threads.end())
			return *found->second;
		// The states of threads that have ended go with them.
		std::erase_if(threads, [this](const auto &item) {
			if (context.threads.find(item.first))
				return false;
			if (trace)
				std::fprintf(stderr, "seh: thread %u state released\n", item.first);
			item.second->scratch.free();
			return true;
		});
		return *threads.emplace(id, std::make_unique<ThreadState>(context)).first->second;
	}

	// The pending catch whose callback returns to the consolidation gate from this context, if any.
	const Consolidation *consolidation_returning_to(uint64_t pc, const ContextBytes &frame) const
	{
		if (!trap_gate || pc != trap_gate)
			return nullptr;
		const uint64_t slot = context_rsp(frame) - 8;
		const auto found = std::find_if(mine().consolidations.begin(), mine().consolidations.end(), [slot](const auto &item) { return item.return_slot == slot; });
		return found == mine().consolidations.end() ? nullptr : &*found;
	}

	[[noreturn]] void finish_consolidation(uint64_t resume_ip)
	{
		const uint64_t slot = context_rsp(context_from_process(context.process.context())) - 8;
		const auto found = std::find_if(mine().consolidations.begin(), mine().consolidations.end(), [slot](const auto &item) { return item.return_slot == slot; });
		if (found == mine().consolidations.end())
			throw std::runtime_error("native unwind consolidation returned without a pending catch");
		auto target = found->target;
		const size_t depth = found->depth, mark = found->mark;
		// Callbacks that never returned (a rethrow left them) are abandoned together with this one.
		mine().consolidations.erase(found, mine().consolidations.end());
		set_context_rip(target, resume_ip);
		redirect_to(target, depth, mark);
	}

private:
	KernelContext &context;
	NativeUnwinder &unwinder;
	mutable std::unordered_map<uint32_t, std::unique_ptr<ThreadState>> threads;
	bool readable(uint64_t address) const
	{
		try {
			context.process.read_memory(address, 1);
			return true;
		} catch (const std::runtime_error &) {
			return false;
		}
	}
};

void register_exception_api(ApiRegistry &registry, KernelContext &context, NativeUnwinder &unwinder)
{
	auto exceptions = std::make_shared<Exceptions>(context, unwinder);
	context.raise_fault = [exceptions, &context, &unwinder](const NativeResult &fault) {
		auto native = context.process.context();
		auto origin = context_from_process(native);
		const uint64_t rip = context_rip(origin);
		context.process.enter_fault_dispatch();
		// A jump to unreadable memory faults where no instruction can be read: it is an ordinary access violation, never a fast fail.
		std::vector<uint8_t> code(2, 0);
		try {
			code = context.process.read_memory(rip, 2);
		} catch (const std::runtime_error &) {
		}
		if (fault.signal == SIGSEGV && code[0] == 0xcd && code[1] == 0x29) {
			const auto *module = unwinder.module_at(rip);
			std::fprintf(stderr, "raven: fast fail %llu at %s+%llx\n", (unsigned long long)context_register(origin, 1),
				module ? module->name.c_str() : "unknown", module ? (unsigned long long)(rip - module->loaded.base()) : (unsigned long long)rip);
			// The code addresses on the stack name who gave up, which RAVEN_BACKTRACE shows.
			if (std::getenv("RAVEN_BACKTRACE"))
				try {
					const auto stack = context.process.read_memory(context_rsp(origin), 0x400);
					for (size_t at = 0; at + 8 <= stack.size(); at += 8) {
						uint64_t value = 0;
						std::memcpy(&value, stack.data() + at, 8);
						if (const auto *owner = unwinder.module_at(value))
							std::fprintf(stderr, "raven:   +%zx %s+%llx\n", at, owner->name.c_str(), (unsigned long long)(value - owner->loaded.base()));
					}
				} catch (const std::exception &) {
				}
			context.process.abandon_call();
			throw ProcessExit{0xc0000409U, rip, true};
		}
		uint32_t status = 0xc0000005U;
		std::vector<uint64_t> parameters;
		switch (fault.signal) {
		case SIGSEGV: parameters = {0, fault.fault_address}; break;
		case SIGBUS: status = 0xc0000006U; parameters = {0, fault.fault_address, 0xc0000006U}; break;
		case SIGFPE:
			status = fault.signal_code == FPE_INTDIV ? 0xc0000094U : fault.signal_code == FPE_INTOVF ? 0xc0000095U :
				fault.signal_code == FPE_FLTDIV ? 0xc000008eU : fault.signal_code == FPE_FLTOVF ? 0xc0000091U : 0xc000008fU;
			break;
		case SIGILL: status = 0xc000001dU; break;
		case SIGTRAP: status = 0x80000003U; break;
		default: break;
		}
		auto record = exceptions->new_record(status, 0, rip, parameters);
		// As KiUserExceptionDispatcher finds them: the CONTEXT on a 16-byte boundary below the faulting stack pointer and the record above
		// it, and the handlers' calls below both. The .NET runtime turns a fault in managed code into an exception from these.
		const uint64_t frame = (context_rsp(origin) - 0x40 - x64_context::size - ((record_size + 15) & ~size_t(15))) & ~uint64_t(15);
		try {
			context.process.write_memory(frame, origin);
			context.process.write_memory(frame + x64_context::size, record);
			auto lowered = native;
			lowered.registers.rsp = frame - 8;
			context.process.set_context(lowered);
			exceptions->mine().fault_frame = Exceptions::StackFrame{frame, frame + x64_context::size};
		} catch (const std::runtime_error &) {
			// A stack that cannot take the frame (overflow): the exception goes out from the dispatcher's own memory.
		}
		exceptions->raise(std::move(record), std::move(origin), [fault] { return fault; });
	};
	context.describe_stack = [&context, &unwinder] {
		std::string text;
		try {
			auto bytes = context_from_process(context.process.context());
			// Skip the API stub: the first frame is the guest code that called it.
			unwinder.unwind_leaf(bytes);
			for (int frame = 0; frame < 24; ++frame) {
				const uint64_t pc = context_rip(bytes);
				const auto *module = unwinder.module_at(pc);
				if (!module)
					break;
				char buffer[96];
				std::snprintf(buffer, sizeof buffer, " %s+%llx", module->name.c_str(), (unsigned long long)(pc - module->loaded.base()));
				text += buffer;
				const auto function = unwinder.lookup(pc);
				if (!function)
					unwinder.unwind_leaf(bytes);
				else
					unwinder.unwind(0, pc, *function, bytes);
			}
		} catch (const std::exception &) {
		}
		return text;
	};
	registry.add("RtlCaptureContext", 1, [exceptions, &context](ApiCall &call) {
		call.write(call[0], exceptions->caller_context());
	});
	registry.add("RtlLookupFunctionEntry", 3, [&unwinder](ApiCall &call) {
		const auto function = unwinder.lookup(call[0]);
		if (call[1]) call.store<uint64_t>(call[1], function ? function->image_base : 0);
		call.ret(function ? function->address : 0);
	});
	registry.add("RtlPcToFileHeader", 2, [&unwinder](ApiCall &call) {
		const auto *module = unwinder.module_at(call[0]);
		const uint64_t base = module ? module->loaded.base() : 0;
		if (call[1]) call.store<uint64_t>(call[1], base);
		call.ret(base);
	});
	registry.add("RtlVirtualUnwind", 8, [&unwinder](ApiCall &call) {
		const auto entry = call.read(call[3], 12);
		FunctionEntry function;
		function.address = call[3];
		std::memcpy(&function.begin, entry.data(), 4);
		std::memcpy(&function.end, entry.data() + 4, 4);
		std::memcpy(&function.unwind, entry.data() + 8, 4);
		function.image_base = call[1];
		// Code a generator emitted belongs to no module: its unwind data is read from the program's memory.
		function.module = unwinder.module_at(call[1]);
		auto bytes = call.read(call[4], x64_context::size);
		// KNONVOLATILE_CONTEXT_POINTERS: the entries of the registers this frame restores are replaced, the others kept.
		NativeUnwinder::ContextPointers pointers{};
		if (call[7]) {
			const auto table = call.read(call[7], sizeof pointers);
			std::memcpy(pointers.data(), table.data(), sizeof pointers);
		}
		const auto before = bytes;
		const auto result = unwinder.unwind(call.u32(0), call[2], function, bytes, call[7] ? &pointers : nullptr);
		if (std::getenv("RAVEN_TRACE_SEH")) {
			std::fprintf(stderr, "seh virtual unwind: pc=%llx base=%llx fn=%x..%x unwind=%x rip %llx->%llx rsp %llx->%llx rbp %llx->%llx info",
				(unsigned long long)call[2], (unsigned long long)call[1], function.begin, function.end, function.unwind,
				(unsigned long long)context_rip(before), (unsigned long long)context_rip(bytes), (unsigned long long)context_rsp(before),
				(unsigned long long)context_rsp(bytes), (unsigned long long)context_register(before, 5), (unsigned long long)context_register(bytes, 5));
			try {
				for (const auto byte : call.read(call[1] + function.unwind, 24))
					std::fprintf(stderr, " %02x", byte);
			} catch (...) {
			}
			std::fprintf(stderr, "\n");
		}
		call.write(call[4], bytes);
		if (call[7])
			call.write(call[7], std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(pointers.data()), sizeof pointers));
		if (call[5]) call.store<uint64_t>(call[5], result.handler_data);
		if (call[6]) call.store<uint64_t>(call[6], result.establisher);
		call.ret(result.handler);
	});
	registry.add("RaiseException", 4, [exceptions, &context](ApiCall &call) {
		const uint32_t count = std::min<uint32_t>(call.u32(2), 15);
		std::vector<uint64_t> parameters(count);
		for (uint32_t index = 0; call[3] && index < count; ++index)
			parameters[index] = call.load<uint64_t>(call[3] + index * 8ULL);
		// The exception comes from inside RaiseException, as on Windows, where both its address and the captured context name the system
		// library and not the caller: the .NET runtime takes a fault reported inside its own code for a fatal one. The search reaches the
		// caller by unwinding the stub as a leaf, and a handler that continues returns there (see redirect_to).
		auto origin = context_from_process(context.process.context());
		exceptions->raise_stub = context_rip(origin);
		auto record = exceptions->new_record(call.u32(0), call.u32(1) & flag_noncontinuable, context_rip(origin), parameters);
		exceptions->raise(std::move(record), std::move(origin), [&context] { return native_raise_exception(context.process); });
	});
	registry.add("RtlRaiseException", 1, [exceptions, &context](ApiCall &call) {
		auto origin = exceptions->caller_context();
		auto record = call.read(call[0], record_size);
		put64(record, 0x10, context_rip(origin));
		exceptions->raise(std::move(record), std::move(origin), [&context] { return native_raise_rtl_exception(context.process, false); });
	});
	registry.add("RtlUnwindEx", 6, [exceptions, &context](ApiCall &call) {
		if (std::getenv("RAVEN_TRACE_SEH") && call[2]) {
			// Show the thrown object as the unwinder sees it: it must still be intact here.
			const auto bytes = call.read(call[2], record_size);
			const uint64_t object = load64(bytes, 0x28);
			try {
				const auto data = call.read(object, 24);
				std::fprintf(stderr, "seh unwind object %llx:", (unsigned long long)object);
				for (const auto byte : data) std::fprintf(stderr, " %02x", byte);
				std::fprintf(stderr, "\n");
			} catch (const ApiFault &) {
			}
		}
		(void)context;
		std::vector<uint8_t> record;
		if (call[2])
			record = call.read(call[2], record_size);
		else
			record = exceptions->new_record(status_unwind, 0, 0, {});
		exceptions->unwind(call[0], call[1], std::move(record), call[3], call[2]);
	});
	registry.add("RtlUnwind", 4, [exceptions](ApiCall &call) {
		std::vector<uint8_t> record;
		if (call[2])
			record = call.read(call[2], record_size);
		else
			record = exceptions->new_record(status_unwind, 0, 0, {});
		exceptions->unwind(call[0], call[1], std::move(record), call[3], call[2]);
	});
	// _local_unwind, which the 64-bit kernel32 publishes for compilers: the unwind of a function to a target of its own, with no record.
	registry.add("_local_unwind", 2, [exceptions](ApiCall &call) {
		exceptions->unwind(call[0], call[1], exceptions->new_record(status_unwind, 0, 0, {}), 0, 0);
	});
	// The context resumes at the callback nesting its stack belongs to: callbacks begun from deeper in the stack than its frame, such as the
	// handler calls of a dispatch that a runtime leaves this way for a catch block, are abandoned.
	const auto restore = [exceptions, &context](ApiCall &call) {
		const auto target = call.read(call[0], x64_context::size);
		size_t depth = context.process.callback_depth();
		while (depth > 0 && context.process.callback_origin(depth - 1).registers.rsp <= context_rsp(target))
			--depth;
		exceptions->redirect_to(target, depth);
	};
	exceptions->trap_gate = 0;
	try {
		exceptions->trap_gate = context.modules.resolve({"kernel32.dll", "RcConsolidateFrames", 0, false, 0});
	} catch (const std::runtime_error &) {
	}
	registry.add("RcConsolidateFrames", 0, [exceptions, &context](ApiCall &call) {
		(void)call;
		exceptions->finish_consolidation(context.process.context().registers.rax);
	});
	registry.add("RtlRestoreContext", 2, restore);
	registry.add("NtContinue", 2, restore);
	registry.add("UnhandledExceptionFilter", 1, [](ApiCall &call) { call.ret(0); });
	const auto add_vectored = [exceptions, &context](ApiCall &call, bool after) {
		if (!call[1]) {
			call.fail(87, 0);
			return;
		}
		const auto cookie = context.allocator.allocate(16, true);
		if (!cookie) {
			call.fail(8, 0);
			return;
		}
		Exceptions::Vectored item{cookie, call[1], after};
		// A nonzero first argument puts the handler at the head of the chain.
		if (call.u32(0) && !after)
			exceptions->vectored.insert(exceptions->vectored.begin(), item);
		else
			exceptions->vectored.push_back(item);
		call.ret(cookie);
	};
	registry.add("AddVectoredExceptionHandler", 2, [add_vectored](ApiCall &call) { add_vectored(call, false); });
	registry.add("AddVectoredContinueHandler", 2, [add_vectored](ApiCall &call) { add_vectored(call, true); });
	const auto remove_vectored = [exceptions, &context](ApiCall &call) {
		auto &list = exceptions->vectored;
		const auto found = std::find_if(list.begin(), list.end(), [&](const auto &item) { return item.cookie == call[0]; });
		if (found == list.end()) {
			call.ret(0);
			return;
		}
		list.erase(found);
		context.allocator.release(call[0]);
		call.ret(1);
	};
	registry.add("RemoveVectoredExceptionHandler", 1, remove_vectored);
	registry.add("RemoveVectoredContinueHandler", 1, remove_vectored);
	// Function tables of generated code: the unwinder reads them from the program's memory, and calls a table's callback from there to
	// find the entry of an address, as a runtime that compiles code registers its code heaps.
	unwinder.set_guest_call([&context](uint64_t function, uint64_t first, uint64_t second) {
		const uint64_t arguments[] = {first, second};
		const auto outcome = context.call(function, arguments);
		if (outcome.stop != NativeStop::returned)
			throw outcome;
		return outcome.value;
	});
	registry.add("RtlAddFunctionTable", 3, [&unwinder](ApiCall &call) {
		if (!call[0] || !call.u32(1)) {
			call.ret(0);
			return;
		}
		uint64_t length = 0;
		for (uint32_t index = 0; index < call.u32(1); ++index)
			length = std::max<uint64_t>(length, call.load<uint32_t>(call[0] + index * 12ULL + 4));
		unwinder.add_table({call[0], call[2], length, call[0], call.u32(1), 0, 0});
		call.ret(1);
	});
	registry.add("RtlDeleteFunctionTable", 1, [&unwinder](ApiCall &call) { call.ret(unwinder.remove_table(call[0])); });
	// The identifier of a callback table has its two low bits set, which tells it apart from the address of an entry array.
	registry.add("RtlInstallFunctionTableCallback", 6, [&unwinder](ApiCall &call) {
		if ((call[0] & 3) != 3 || !call[3] || !call[2]) {
			call.ret(0);
			return;
		}
		unwinder.add_table({call[0], call[1], call.u32(2), 0, 0, call[3], call[4]});
		call.ret(1);
	});
	// A code generator registers the unwind data of the code it emits; the table stands as a handle until the generator deletes it.
	registry.add("RtlAddGrowableFunctionTable", 6, [](ApiCall &call) {
		static uint64_t next = 0x7000;
		if (!call[0]) {
			call.ret(0xc000000dU);
			return;
		}
		call.store_pointer(call[0], next += 0x10);
		call.ret(0);
	});
	registry.add("RtlGrowFunctionTable", 2, [](ApiCall &call) { call.ret(0); });
	registry.add("RtlDeleteGrowableFunctionTable", 1, [](ApiCall &call) { call.ret(0); });
	(void)status_noncontinuable;
	(void)flag_nested;
}

}
