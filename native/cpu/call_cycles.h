#pragma once

#include <x86intrin.h>
#include <algorithm>
#include <utility>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "served_stub.h"

namespace raven {

// What the calls through served stubs cost, and the NT system calls of threads in the agent's hands (timed in its handler, agent.S), in
// time stamp counter cycles, when RAVEN_CYCLES is set to the seconds between reports. A call is cut in four: the boundary, from the stub
// taking its arguments to the runtime seeing the call, less the turn; the turn, the part of the boundary its host thread waited for the
// runtime while other threads had it; the service, from the call seen to the runtime answering; the return, from the answer to the guest
// going on with it. The stub stamps the two guest ends in the thread's block (gates.S), the runtime the two in between; the return of a
// call is known when the next call on the same block is seen. Calls answered beside the turn (see NativeProcess::serve_alone) are counted
// apart as well, as they have no turn to wait for. Apart again, the part of the service a call spent waiting for the desktop's lock while
// another thread held it (see DesktopLock). And the times the host had the kernel copy guest memory for the calls, which costs far
// more than memory both sides map.
class CallCycles {
public:
	static CallCycles &instance()
	{
		static CallCycles cycles;
		return cycles;
	}
	bool enabled() const { return interval.count() > 0; }
	void set_names(std::map<uint64_t, std::string> stubs)
	{
		std::lock_guard lock(mutex);
		names = std::move(stubs);
	}
	// A system call is counted by its number, which no stub's address equals; its name is known once it is served.
	void name_service(uint64_t number, const std::string &name)
	{
		if (!enabled())
			return;
		std::lock_guard lock(mutex);
		names.try_emplace(number, name);
	}
	// The runtime sees the call `stub` in `block` at `now`: the cycles it returns are the time it was seen, for answered(). A call answered
	// beside the turn is counted once it is known to be answered there, with the time it was seen.
	uint64_t seen(const uint64_t *block, uint64_t stub, uint64_t now = __rdtsc())
	{
		if (!enabled())
			return now;
		const uint64_t entered = block[served_stub::entered], last = block[served_stub::last_stub],
			answered = block[served_stub::answered], resumed = block[served_stub::resumed];
		const uint64_t turn = std::exchange(waited, 0);
		std::lock_guard lock(mutex);
		auto &entry = calls[stub];
		++entry.count;
		if (entered && now >= entered && now - entered < limit) {
			const uint64_t part = std::min(turn, now - entered);
			entry.boundary += now - entered - part;
			entry.turn += part;
		}
		if (last && answered && resumed >= answered && resumed - answered < limit) {
			auto &previous = calls[last];
			previous.back += resumed - answered;
			++previous.returns;
		}
		return now;
	}
	// The runtime answers the call `stub` it saw at `since`, beside the turn or in it; it stamps the block for the next call to find.
	void answered(uint64_t *block, uint64_t stub, uint64_t since, bool beside = false)
	{
		const uint64_t now = __rdtsc();
		if (!enabled())
			return;
		block[served_stub::last_stub] = stub;
		block[served_stub::answered] = now;
		std::lock_guard lock(mutex);
		auto &entry = calls[stub];
		if (now >= since)
			entry.service += now - since;
		if (beside)
			++entry.beside;
		entry.locked += std::exchange(locked, 0);
		entry.copies += std::exchange(copies, 0);
		// A call that went to the turn after all counts only the turn's answer.
		entry.whole += std::exchange(whole, 0) * beside;
		entry.desktop += std::exchange(desktop, 0) * !beside;
		report();
	}
	// The host thread of the calling thread waited `cycles` for the runtime's turn before it saw the call it is about to see; 0 when it
	// sees the call without having waited.
	void waited_turn(uint64_t cycles)
	{
		if (enabled())
			waited = cycles;
	}
	// A system call the agent took, from a guest whose served block is `served`: what the signal took from the guest's stamp to the
	// handler's (`stamps`, see agent.h) and, for the call answered before it, from the handler letting the guest go to the guest's stamp
	// at the landing. Each stamp counts once.
	void signalled(uint64_t *served, const uint64_t *stamps, uint64_t number)
	{
		if (!enabled() || !served)
			return;
		const uint64_t asked = std::exchange(served[served_stub::service_asked], 0), entered = stamps[served_stub::entered];
		const uint64_t landed = std::exchange(served[served_stub::service_resumed], 0), left = stamps[served_stub::resumed];
		std::lock_guard lock(mutex);
		if (asked && entered >= asked && entered - asked < limit) {
			auto &entry = calls[number];
			entry.delivery += entered - asked;
			++entry.deliveries;
		}
		if (landed && left && landed >= left && landed - left < limit) {
			auto &entry = calls[served[served_stub::service_last]];
			entry.sigreturn += landed - left;
			++entry.sigreturns;
		}
	}
	// The host thread waited `cycles` for a lock another thread held, in the call it serves.
	void waited_lock(uint64_t cycles)
	{
		if (enabled())
			locked += cycles;
	}
	// The host thread had the kernel copy guest memory, in the call it serves.
	void copied()
	{
		if (enabled())
			++copies;
	}
	// The host thread took the desktop lock whole for a call it answers beside the turn, which stops every other thread's call on the
	// desktop until it is done.
	void took_whole()
	{
		if (enabled())
			whole = 1;
	}
	// The turn took the desktop lock for the call it answers.
	void took_desktop()
	{
		if (enabled())
			desktop = 1;
	}
	~CallCycles()
	{
		// What a run shorter than one report's interval cost is told as it ends.
		std::lock_guard lock(mutex);
		if (!calls.empty())
			report(true);
	}

private:
	struct Entry {
		uint64_t count = 0, returns = 0, beside = 0, boundary = 0, turn = 0, service = 0, back = 0, locked = 0, copies = 0, whole = 0, desktop = 0;
		uint64_t delivery = 0, deliveries = 0, sigreturn = 0, sigreturns = 0;
	};
	// A stretch longer than this many cycles (about a second) is a guest that was stopped or descheduled, not the cost of a call.
	static constexpr uint64_t limit = 4'000'000'000ULL;
	std::chrono::seconds interval{std::getenv("RAVEN_CYCLES") ? std::max(1, std::atoi(std::getenv("RAVEN_CYCLES"))) : 0};
	std::chrono::steady_clock::time_point last_report = std::chrono::steady_clock::now();
	std::mutex mutex;
	std::map<uint64_t, std::string> names;
	std::map<uint64_t, Entry> calls;
	// The turn the host thread waited for, kept until the call it waited for is seen.
	static inline thread_local uint64_t waited = 0;
	// The lock the host thread waited for in the call it serves, kept until it answers it.
	static inline thread_local uint64_t locked = 0;
	// The kernel's copies of guest memory, kept until the call they were made for is answered.
	static inline thread_local uint64_t copies = 0;
	// Whether the call took the desktop lock whole beside the turn, kept until it is answered.
	static inline thread_local uint64_t whole = 0;
	// Whether the turn took the desktop lock for the call, kept until it is answered.
	static inline thread_local uint64_t desktop = 0;

	void report(bool ending = false)
	{
		const auto now = std::chrono::steady_clock::now();
		if (!ending && now - last_report < interval)
			return;
		last_report = now;
		std::vector<std::pair<uint64_t, uint64_t>> order;
		uint64_t count = 0, beside = 0, boundary = 0, turn = 0, service = 0, back = 0, returns = 0;
		for (const auto &[stub, entry] : calls) {
			order.emplace_back(entry.boundary + entry.turn + entry.service + entry.back, stub);
			count += entry.count;
			beside += entry.beside;
			boundary += entry.boundary;
			turn += entry.turn;
			service += entry.service;
			back += entry.back;
			returns += entry.returns;
		}
		std::sort(order.rbegin(), order.rend());
		const auto per = [](uint64_t total, uint64_t count) { return (unsigned long long)(count ? total / count : 0); };
		std::fprintf(stderr, "cycles: %llu calls, %llu beside the turn, per call boundary %llu turn %llu service %llu return %llu\n",
			(unsigned long long)count, (unsigned long long)beside, per(boundary, count), per(turn, count), per(service, count), per(back, returns));
		// The costliest dozen while the program runs; every call in the report the run ends with.
		for (size_t index = 0; index < order.size() && (ending || index < 12); ++index) {
			const auto &entry = calls[order[index].second];
			const auto name = names.find(order[index].second);
			// A system call has the signal's way to the agent and back besides: the kernel's part of its boundary and of its return.
			std::fprintf(stderr, "cycles:   %-32s %8llu calls %8llu beside  boundary %7llu  turn %9llu  service %9llu  return %7llu  lock %9llu  copies %8llu"
				"  signal %7llu  sigreturn %7llu  whole %8llu  desktop %8llu\n",
				name == names.end() ? "?" : name->second.c_str(), (unsigned long long)entry.count, (unsigned long long)entry.beside,
				per(entry.boundary, entry.count), per(entry.turn, entry.count), per(entry.service, entry.count), per(entry.back, entry.returns),
				per(entry.locked, entry.count), (unsigned long long)entry.copies, per(entry.delivery, entry.deliveries),
				per(entry.sigreturn, entry.sigreturns), (unsigned long long)entry.whole,
				(unsigned long long)entry.desktop);
		}
		calls.clear();
	}
};

}
