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
constexpr uint64_t translator_version = 1;
constexpr uint32_t cache_magic = 0x43545652; // "RVTC"

thread_local Runtime *running = nullptr;

using EnterFunction = uint32_t (*)(CpuState *, const void *);

uint64_t now_ns()
{
	return static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint64_t fnv(uint64_t hash, const void *bytes, size_t size)
{
	const auto *data = static_cast<const uint8_t *>(bytes);
	for (size_t i = 0; i < size; ++i)
		hash = (hash ^ data[i]) * 0x100000001b3ull;
	return hash;
}


}

Runtime::Runtime(GuestMemory &memory, CodeAllocator &cache, TranslatorOptions options, CodeRunner *runner)
	: memory(memory), cache(cache), options(options), runner(runner), state_memory(cache.allocate_data(sizeof(CpuState))),
	  lookup_memory(cache.allocate_data(sizeof(LookupEntry) << lookup_bits)), cpu(*new (state_memory.host) CpuState{}),
	  lookup(static_cast<LookupEntry *>(lookup_memory.host), size_t(1) << lookup_bits)
{
	std::fill(lookup.begin(), lookup.end(), LookupEntry{});
	// Translated code reads the table at the address it sees, which is not the runtime's pointer in a VM.
	cpu.lookup_table = reinterpret_cast<LookupEntry *>(lookup_memory.address);
	place_fixed_code();
}

Runtime::~Runtime()
{
	for (const auto &[guest, block] : blocks)
		memory.watch_code(block->guest_start, block->guest_size, false);
}

Runtime *Runtime::current()
{
	return running;
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
	enter = place(enter_trampoline());
	exit = place(exit_trampoline());
	return_address = place(return_stub());
	return_guest = reinterpret_cast<uint64_t>(return_address);
	cpu.exit_trampoline = reinterpret_cast<uint64_t>(exit);
}

void Runtime::bind_native(uint64_t address, const void *function, unsigned integer_arguments)
{
	if (!memory.is_native(address))
		throw std::invalid_argument("the code map does not call that address native");
	// A thunk calls the function at its address in this process, which code running elsewhere cannot reach.
	if (runner)
		throw std::logic_error("native functions need translated code that runs in this process");
	natives[address] = place(native_thunk(reinterpret_cast<uint64_t>(function), integer_arguments));
}

void Runtime::remember(uint64_t guest, uint8_t *host)
{
	lookup[guest & ((1u << lookup_bits) - 1)] = {guest, reinterpret_cast<uint64_t>(host)};
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
	const auto code = memory.code(guest);
	if (code.empty())
		return nullptr;
	const uint64_t start = now_ns();
	TranslatedBlock translated = translate_block(code, guest, options, translation);
	counters.translate_ns += now_ns() - start;
	return install(std::move(translated));
}

uint8_t *Runtime::host_code(uint64_t guest)
{
	uint8_t *host = nullptr;
	if (guest == return_guest)
		host = return_address;
	else if (const auto native = natives.find(guest); native != natives.end())
		host = native->second;
	else if (const auto found = blocks.find(guest); found != blocks.end())
		host = found->second->host;
	else if (Block *block = translate(guest))
		host = block->host;
	if (host)
		remember(guest, host);
	return host;
}

uint32_t Runtime::translate_ahead(uint64_t address)
{
	if (const auto found = blocks.find(address); found != blocks.end())
		return found->second->guest_size;
	const Block *block = translate(address);
	return block ? block->guest_size : 0;
}

const void *Runtime::translated_code(uint64_t address) const
{
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
	cache.write(site, &branch, 4);
	if (const auto found = blocks.find(target); found != blocks.end())
		found->second->incoming.emplace_back(site, original);
	++counters.chains;
}

uint32_t Runtime::enter_code(const uint8_t *code)
{
	if (runner)
		return runner->enter(reinterpret_cast<uint64_t>(enter), state_memory.address, reinterpret_cast<uint64_t>(code));
	return reinterpret_cast<EnterFunction>(enter)(&cpu, code);
}

RunResult Runtime::run()
{
	for (;;) {
		uint8_t *code = host_code(cpu.rip);
		if (!code)
			return {ExitReason::unsupported, cpu.rip};
		++counters.runs;
		const auto reason = static_cast<ExitReason>(enter_code(code));
		switch (reason) {
		case ExitReason::lookup_miss: ++counters.lookup_misses; break;
		case ExitReason::chain: chain(reinterpret_cast<uint8_t *>(cpu.exit_info), cpu.rip); break;
		case ExitReason::returned: return {reason, cpu.gpr[rax]};
		case ExitReason::stopped: {
			cpu.stop_requested = 0;
			return {reason, cpu.stop_code};
		}
		default: return {reason, cpu.rip};
		}
	}
}

RunResult Runtime::call(uint64_t address, std::initializer_list<uint64_t> arguments)
{
	if (arguments.size() > 4)
		throw std::invalid_argument("guest calls take up to four arguments here");
	Runtime *outer = running;
	running = this;
	++depth;
	// A call made while guest code waits in a native function: keep what the outer run needs when it resumes.
	const uint64_t saved_rip = cpu.rip, saved_frame = cpu.host_frame, saved_rsp = cpu.gpr[rsp];
	uint64_t stack = (saved_rsp & ~uint64_t(15)) - 32 - 8;
	memory.write(stack, &return_guest, 8);
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

void Runtime::request_stop(uint64_t code)
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
			entry = {};
	memory.watch_code(block.guest_start, std::max<uint32_t>(block.guest_size, 1), false);
	++counters.invalidated_blocks;
}

void Runtime::code_written(uint64_t address)
{
	const uint64_t granule = address & ~uint64_t(memory.watch_granule() - 1);
	const auto found = blocks_by_granule.find(granule);
	if (found == blocks_by_granule.end())
		return;
	const std::vector<uint64_t> starts = std::move(found->second);
	blocks_by_granule.erase(found);
	for (uint64_t start : starts) {
		const auto block = blocks.find(start);
		if (block == blocks.end())
			continue;
		invalidate(*block->second);
		// The code stays in the cache until it is reset: the guest may still be running it on its way out.
		blocks.erase(block);
	}
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
