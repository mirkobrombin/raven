#include "runtime.h"

#include "a64_emitter.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <new>
#include <stdexcept>

namespace raven::translate {

namespace {

// Bumped whenever the code the translator emits changes, so caches written by an older translator stop matching.
constexpr uint64_t translator_version = 3;
constexpr uint32_t cache_magic = 0x43545652; // "RVTC"

thread_local Runtime::Thread *running = nullptr;

using EnterFunction = uint32_t (*)(CpuState *, const void *);

uint64_t now_ns()
{
	return static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Translated code reads a lookup entry with one LDP while other guest threads run. With FEAT_LSE2, a 16-byte aligned LDP and STP are
// single-copy atomic, so a reader sees the whole old or the whole new entry; entries are 16 bytes in a 16-byte aligned table. The DMB
// orders the code the entry names, written and made visible to instruction fetch before, ahead of the entry.
void store_entry(LookupEntry &entry, uint64_t guest, uint64_t host)
{
#if defined(__aarch64__)
	__asm__ volatile("dmb ish\n\tstp %1, %2, [%0]" : : "r"(&entry), "r"(guest), "r"(host) : "memory");
#else
	entry = {guest, host};
#endif
}

uint64_t fnv(uint64_t hash, const void *bytes, size_t size)
{
	const auto *data = static_cast<const uint8_t *>(bytes);
	for (size_t i = 0; i < size; ++i)
		hash = (hash ^ data[i]) * 0x100000001b3ull;
	return hash;
}


}

Runtime::Thread::Thread(Runtime &runtime, CodeRunner *runner)
	: runtime(runtime), runner(runner), state_memory(runtime.cache, sizeof(CpuState)), cpu(*new (state_memory.data.host) CpuState{})
{
	// Translated code reads the table and the trampoline at the addresses it sees, which are not the runtime's pointers in a VM.
	cpu.lookup_table = reinterpret_cast<LookupEntry *>(runtime.lookup_memory.data.address);
	cpu.exit_trampoline = reinterpret_cast<uint64_t>(runtime.exit);
}

bool (*Runtime::host_has_afp)() = host::alternate_floating_point;

Runtime::Runtime(GuestMemory &memory, CodeAllocator &cache, TranslatorOptions options, CodeRunner *runner)
	: memory(memory), cache(cache), options(options), lookup_memory(cache, sizeof(LookupEntry) << lookup_bits),
	  lookup(static_cast<LookupEntry *>(lookup_memory.data.host), size_t(1) << lookup_bits)
{
	// Translated code runs with FPCR.AH set; without FEAT_AFP the bit reads as zero and SSE would quietly follow Arm instead of x86.
	// A runner (a vCPU) checks its own CPU.
	if (!runner && !host_has_afp())
		throw std::runtime_error("this CPU lacks FEAT_AFP (FPCR.AH), which translated SSE code needs for x86's NaN, min and max");
	std::fill(lookup.begin(), lookup.end(), LookupEntry{});
	restore_host_fpcr = !runner || runner->restores_host_fpcr();
	place_fixed_code();
	threads.push_back(std::unique_ptr<Thread>(new Thread(*this, runner)));
	first = threads.front().get();
}

Runtime::Thread &Runtime::add_thread(CodeRunner *runner)
{
	std::lock_guard guard(lock);
	if (!runner)
		throw std::invalid_argument("a guest thread in this process is the runtime's only one");
	if (!natives.empty())
		throw std::logic_error("native functions are bound: the runtime runs its first thread alone");
	// Giving back an FPCR the runner does not need costs time only; not giving back one it needs would leave the guest's to the host.
	if (runner->restores_host_fpcr() && !restore_host_fpcr)
		throw std::invalid_argument("this runtime's trampolines leave the guest's FPCR, which this runner's host code cannot have");
	threads.push_back(std::unique_ptr<Thread>(new Thread(*this, runner)));
	return *threads.back();
}

void Runtime::remove_thread(Thread &thread)
{
	std::lock_guard guard(lock);
	if (&thread == first)
		throw std::invalid_argument("the first guest thread stays with its runtime");
	std::erase_if(threads, [&](const auto &owned) { return owned.get() == &thread; });
}

Runtime::~Runtime()
{
	for (const auto &[guest, block] : blocks)
		memory.watch_code(block->guest_start, std::max<uint32_t>(block->guest_size, 1), false);
}

Runtime *Runtime::current()
{
	return running ? &running->runtime : nullptr;
}

uint8_t *Runtime::place(const std::vector<uint32_t> &code)
{
	uint8_t *at = cache.allocate(code.size() * 4);
	if (!at)
		throw std::runtime_error("the code cache is full");
	cache.write(at, code.data(), code.size() * 4);
	return at;
}

void Runtime::place_fixed_code()
{
	enter = place(enter_trampoline(restore_host_fpcr));
	exit = place(exit_trampoline(restore_host_fpcr));
	return_address = place(return_stub());
	return_guest = reinterpret_cast<uint64_t>(return_address);
}

void Runtime::bind_native(uint64_t address, const void *function, unsigned integer_arguments)
{
	std::lock_guard guard(lock);
	if (!memory.is_native(address))
		throw std::invalid_argument("the code map does not call that address native");
	// A thunk calls the function at its address in this process, which code running elsewhere cannot reach.
	if (std::any_of(threads.begin(), threads.end(), [](const auto &thread) { return thread->runner; }))
		throw std::logic_error("native functions need translated code that runs in this process");
	natives[address] = place(native_thunk(reinterpret_cast<uint64_t>(function), integer_arguments));
}

Runtime::Arm64ecRoutines Runtime::arm64ec_routines(std::span<const std::pair<uint64_t, uint64_t>> native)
{
	std::lock_guard guard(lock);
	if (arm64ec.dispatch_ret && !std::equal(native.begin(), native.end(), arm64ec_ranges.begin(), arm64ec_ranges.end()))
		throw std::logic_error("check_icall was placed for other ARM64EC ranges");
	if (!arm64ec.dispatch_ret) {
		arm64ec_ranges.assign(native.begin(), native.end());
		const auto at = [](const uint8_t *code) { return reinterpret_cast<uint64_t>(code); };
		arm64ec.dispatch_call = at(place(arm64ec_dispatch_call()));
		arm64ec.dispatch_ret = at(place(arm64ec_dispatch_ret()));
		arm64ec.check_icall = at(place(arm64ec_check_icall(native)));
		arm64ec.unsupported = at(place(arm64ec_unsupported()));
	}
	return arm64ec;
}

void Runtime::remember(uint64_t guest, uint8_t *host)
{
	store_entry(lookup[guest & ((1u << lookup_bits) - 1)], guest, reinterpret_cast<uint64_t>(host));
}

Runtime::Block *Runtime::install(TranslatedBlock &&translated)
{
	auto block = std::make_unique<Block>();
	block->guest_start = translated.guest_start;
	block->guest_size = translated.guest_size;
	block->exits = std::move(translated.exits);
	block->code = std::move(translated.code);
	block->host = place(block->code);
	const uint64_t granule = memory.watch_granule();
	const uint64_t last = block->guest_start + std::max<uint32_t>(block->guest_size, 1) - 1;
	for (uint64_t at = block->guest_start & ~(granule - 1); at <= last; at += granule)
		blocks_by_granule[at].push_back(block->guest_start);
	memory.watch_code(block->guest_start, std::max<uint32_t>(block->guest_size, 1), true);
	Block *placed = block.get();
	blocks[block->guest_start] = std::move(block);
	return placed;
}

Runtime::Block *Runtime::translate(uint64_t guest)
{
	for (;;) {
		const auto code = memory.code(guest);
		if (code.empty())
			return nullptr;
		// Translated from a copy, checked against guest memory once install has watched it: a guest store that landed between the
		// copy and the watch reached no translation, so a changed byte means translating again.
		const size_t most = std::min<size_t>(code.size(), size_t(options.max_instructions) * 15);
		const std::vector<uint8_t> copy(code.begin(), code.begin() + static_cast<std::ptrdiff_t>(most));
		const uint64_t start = now_ns();
		TranslatedBlock translated = translate_block(copy, guest, options, translation);
		counters.translate_ns += now_ns() - start;
		Block *block = install(std::move(translated));
		const auto now = memory.code(guest);
		if (now.size() >= block->guest_size && std::equal(copy.begin(), copy.begin() + block->guest_size, now.begin()))
			return block;
		// The dropped code stays in the cache until a reset, as code_written leaves it.
		invalidate(*block);
		blocks.erase(guest);
		for (auto &[granule, starts] : blocks_by_granule)
			std::erase(starts, guest);
	}
}

uint8_t *Runtime::host_code(uint64_t guest)
{
	uint8_t *host = nullptr;
	if (guest == return_guest)
		host = return_address;
	else if (const auto native = natives.find(guest); native != natives.end())
		host = native->second;
	else if (const auto stub = native_stubs.find(guest); stub != native_stubs.end())
		host = stub->second;
	else if (memory.is_native(guest)) {
		// Neither a function with an entry thunk nor a return address: the run stops as unsupported there.
		if (const uint64_t thunk = memory.entry_thunk(guest))
			host = native_stubs[guest] = place(arm64ec_entry(guest, thunk));
		else if (memory.native_return(guest))
			host = native_stubs[guest] = place(arm64ec_resume(guest));
	} else if (const auto found = blocks.find(guest); found != blocks.end())
		host = found->second->host;
	else if (Block *block = translate(guest))
		host = block->host;
	if (host)
		remember(guest, host);
	return host;
}

uint32_t Runtime::translate_ahead(uint64_t address)
{
	std::lock_guard guard(lock);
	if (const auto found = blocks.find(address); found != blocks.end())
		return found->second->guest_size;
	const Block *block = translate(address);
	return block ? block->guest_size : 0;
}

const void *Runtime::translated_code(uint64_t address) const
{
	std::lock_guard guard(lock);
	const auto found = blocks.find(address);
	return found == blocks.end() ? nullptr : found->second->host;
}

void Runtime::chain(uint8_t *site, uint64_t target)
{
	uint8_t *host = host_code(target);
	if (!host)
		return;
	const int64_t words = (host - site) / 4;
	if (!cache.contains(site) || !cache.contains(host) || !a64::Assembler::b_reaches(words))
		return;
	uint32_t original;
	cache.read(site, &original, 4);
	const uint32_t branch = a64::Assembler::encode_b(words);
	// Another guest thread took the same exit before this one got the lock.
	if (original == branch)
		return;
	// One aligned word: a b replaced by a b, which the Arm ARM allows other guest threads to execute while it changes.
	cache.write(site, &branch, 4);
	if (const auto found = blocks.find(target); found != blocks.end())
		found->second->incoming.emplace_back(site, original);
	++counters.chains;
}

uint32_t Runtime::Thread::enter_code(const uint8_t *code)
{
	if (runner)
		return runner->enter(reinterpret_cast<uint64_t>(runtime.enter), state_memory.data.address, reinterpret_cast<uint64_t>(code));
	return reinterpret_cast<EnterFunction>(runtime.enter)(&cpu, code);
}

RunResult Runtime::Thread::run()
{
	for (;;) {
		uint8_t *code;
		{
			std::lock_guard guard(runtime.lock);
			code = runtime.host_code(cpu.rip);
			++runtime.counters.runs;
		}
		if (!code)
			return {ExitReason::unsupported, cpu.rip};
		const auto reason = static_cast<ExitReason>(enter_code(code));
		switch (reason) {
		case ExitReason::lookup_miss: {
			std::lock_guard guard(runtime.lock);
			++runtime.counters.lookup_misses;
			break;
		}
		case ExitReason::chain: {
			std::lock_guard guard(runtime.lock);
			runtime.chain(reinterpret_cast<uint8_t *>(cpu.exit_info), cpu.rip);
			break;
		}
		case ExitReason::returned: return {reason, cpu.gpr[rax]};
		case ExitReason::stopped: {
			cpu.stop_requested = 0;
			return {reason, cpu.stop_code};
		}
		default: return {reason, cpu.rip};
		}
	}
}

Runtime::Thread &Runtime::calling_thread()
{
	return running && &running->runtime == this ? *running : main_thread();
}

RunResult Runtime::call(uint64_t address, std::initializer_list<uint64_t> arguments)
{
	return calling_thread().call(address, arguments);
}

void Runtime::request_stop(uint64_t code)
{
	calling_thread().request_stop(code);
}

RunResult Runtime::Thread::call(uint64_t address, std::initializer_list<uint64_t> arguments)
{
	if (arguments.size() > 4)
		throw std::invalid_argument("guest calls take up to four arguments here");
	Thread *outer = running;
	running = this;
	++depth;
	// A call made while guest code waits in a native function: keep what the outer run needs when it resumes.
	const uint64_t saved_rip = cpu.rip, saved_frame = cpu.host_frame, saved_rsp = cpu.gpr[rsp];
	uint64_t stack = (saved_rsp & ~uint64_t(15)) - 32 - 8;
	runtime.memory.write(stack, &runtime.return_guest, 8);
	cpu.gpr[rsp] = stack;
	static constexpr Gpr argument_registers[] = {rcx, rdx, r8, r9};
	size_t index = 0;
	for (uint64_t argument : arguments)
		cpu.gpr[argument_registers[index++]] = argument;
	cpu.rip = address;
	RunResult result;
	try {
		result = run();
	} catch (...) {
		--depth;
		running = outer;
		throw;
	}
	cpu.rip = saved_rip;
	cpu.host_frame = saved_frame;
	cpu.gpr[rsp] = saved_rsp;
	--depth;
	running = outer;
	return result;
}

void Runtime::Thread::request_stop(uint64_t code)
{
	cpu.stop_requested = 1;
	cpu.stop_code = code;
}

void Runtime::invalidate(Block &block)
{
	for (const auto &[site, original] : block.incoming)
		cache.write(site, &original, 4);
	block.incoming.clear();
	// Chains out of this block: their targets must not restore a branch in code that is gone.
	for (const BlockExit &exit : block.exits)
		if (const auto target = blocks.find(exit.target); target != blocks.end()) {
			auto &incoming = target->second->incoming;
			uint8_t *site = block.host + 4 * exit.word;
			incoming.erase(std::remove_if(incoming.begin(), incoming.end(), [&](const auto &entry) { return entry.first == site; }),
				incoming.end());
		}
	for (LookupEntry &entry : lookup)
		if (entry.host == reinterpret_cast<uint64_t>(block.host))
			store_entry(entry, 0, 0);
	memory.watch_code(block.guest_start, std::max<uint32_t>(block.guest_size, 1), false);
	++counters.invalidated_blocks;
}

void Runtime::code_written(uint64_t address)
{
	std::lock_guard guard(lock);
	const uint64_t granule = address & ~uint64_t(memory.watch_granule() - 1);
	const auto found = blocks_by_granule.find(granule);
	if (found == blocks_by_granule.end())
		return;
	const std::vector<uint64_t> starts = std::move(found->second);
	blocks_by_granule.erase(found);
	bool dropped = false;
	for (uint64_t start : starts) {
		const auto block = blocks.find(start);
		if (block == blocks.end())
			continue;
		invalidate(*block->second);
		// The code stays in the cache until it is reset: the guest may still be running it on its way out.
		blocks.erase(block);
		dropped = true;
	}
	// A guest thread spinning in a chain that was just undone may go on fetching the old branch until a context synchronization
	// event; the thread that wrote is in the host already.
	if (dropped)
		for (const auto &thread : threads)
			if (thread->runner && thread.get() != running)
				thread->runner->quiesce();
}

uint64_t Runtime::cache_key(std::span<const uint8_t> image, uint64_t base, const TranslatorOptions &options)
{
	uint64_t hash = 0xcbf29ce484222325ull;
	hash = fnv(hash, image.data(), image.size());
	hash = fnv(hash, &base, sizeof base);
	hash = fnv(hash, &translator_version, sizeof translator_version);
	const uint64_t fields[] = {static_cast<uint64_t>(options.ordering), options.order_stack, options.max_instructions};
	return fnv(hash, fields, sizeof fields);
}

void Runtime::save_cache(const std::filesystem::path &path, uint64_t key) const
{
	std::lock_guard guard(lock);
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	auto put = [&](const auto &value) { out.write(reinterpret_cast<const char *>(&value), sizeof value); };
	put(cache_magic);
	put(key);
	put(static_cast<uint64_t>(blocks.size()));
	for (const auto &[guest, block] : blocks) {
		put(block->guest_start);
		put(block->guest_size);
		put(static_cast<uint32_t>(block->code.size()));
		put(static_cast<uint32_t>(block->exits.size()));
		out.write(reinterpret_cast<const char *>(block->code.data()), static_cast<std::streamsize>(block->code.size() * 4));
		for (const BlockExit &exit : block->exits) {
			put(exit.word);
			put(exit.target);
		}
	}
	if (!out)
		throw std::runtime_error("cannot write the translation cache " + path.string());
}

size_t Runtime::load_cache(const std::filesystem::path &path, uint64_t key)
{
	std::lock_guard guard(lock);
	const uint64_t start = now_ns();
	std::ifstream in(path, std::ios::binary);
	auto get = [&](auto &value) { in.read(reinterpret_cast<char *>(&value), sizeof value); return bool(in); };
	uint32_t magic = 0;
	uint64_t stored_key = 0, count = 0;
	if (!get(magic) || magic != cache_magic || !get(stored_key) || stored_key != key || !get(count))
		return 0;
	size_t loaded = 0;
	for (uint64_t i = 0; i < count; ++i) {
		TranslatedBlock translated;
		uint32_t words = 0, exits = 0;
		if (!get(translated.guest_start) || !get(translated.guest_size) || !get(words) || !get(exits))
			break;
		translated.code.resize(words);
		in.read(reinterpret_cast<char *>(translated.code.data()), static_cast<std::streamsize>(words) * 4);
		for (uint32_t e = 0; e < exits; ++e) {
			BlockExit exit{};
			get(exit.word);
			get(exit.target);
			translated.exits.push_back(exit);
		}
		if (!in || blocks.count(translated.guest_start) || memory.code(translated.guest_start).size() < translated.guest_size)
			continue;
		install(std::move(translated));
		++loaded;
	}
	counters.cache_loaded_blocks += loaded;
	counters.cache_load_ns += now_ns() - start;
	return loaded;
}

}
