#pragma once

#include <x86intrin.h>
#include <algorithm>
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

// What the calls through served stubs cost, in time stamp counter cycles, when RAVEN_CYCLES is set to the seconds between reports. A call is cut in three: the boundary, from the stub taking its arguments to the runtime seeing the call; the service, from
// there to the runtime answering; the return, from the answer to the guest going on with it. The stub stamps the two guest ends in the
// thread's block (gates.S), the runtime the two in between; the return of a call is known when the next call on the same block is seen.
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
	// The runtime sees the call `stub` in `block` at `now`: the cycles it returns are the time it was seen, for answered(). A call answered
	// beside the turn is counted once it is known to be answered there, with the time it was seen.
	uint64_t seen(const uint64_t *block, uint64_t stub, uint64_t now = __rdtsc())
	{
		if (!enabled())
			return now;
		const uint64_t entered = block[served_stub::entered], last = block[served_stub::last_stub],
			answered = block[served_stub::answered], resumed = block[served_stub::resumed];
		std::lock_guard lock(mutex);
		auto &entry = calls[stub];
		++entry.count;
		if (entered && now >= entered && now - entered < limit)
			entry.boundary += now - entered;
		if (last && answered && resumed >= answered && resumed - answered < limit) {
			auto &previous = calls[last];
			previous.back += resumed - answered;
			++previous.returns;
		}
		return now;
	}
	// The runtime answers the call `stub` it saw at `since`; it stamps the block for the next call to find.
	void answered(uint64_t *block, uint64_t stub, uint64_t since)
	{
		const uint64_t now = __rdtsc();
		if (!enabled())
			return;
		block[served_stub::last_stub] = stub;
		block[served_stub::answered] = now;
		std::lock_guard lock(mutex);
		if (now >= since)
			calls[stub].service += now - since;
		report();
	}

private:
	struct Entry {
		uint64_t count = 0, returns = 0, boundary = 0, service = 0, back = 0;
	};
	// A stretch longer than this many cycles (about a second) is a guest that was stopped or descheduled, not the cost of a call.
	static constexpr uint64_t limit = 4'000'000'000ULL;
	std::chrono::seconds interval{std::getenv("RAVEN_CYCLES") ? std::max(1, std::atoi(std::getenv("RAVEN_CYCLES"))) : 0};
	std::chrono::steady_clock::time_point last_report = std::chrono::steady_clock::now();
	std::mutex mutex;
	std::map<uint64_t, std::string> names;
	std::map<uint64_t, Entry> calls;

	void report()
	{
		const auto now = std::chrono::steady_clock::now();
		if (now - last_report < interval)
			return;
		last_report = now;
		std::vector<std::pair<uint64_t, uint64_t>> order;
		uint64_t count = 0, boundary = 0, service = 0, back = 0, returns = 0;
		for (const auto &[stub, entry] : calls) {
			order.emplace_back(entry.boundary + entry.service + entry.back, stub);
			count += entry.count;
			boundary += entry.boundary;
			service += entry.service;
			back += entry.back;
			returns += entry.returns;
		}
		std::sort(order.rbegin(), order.rend());
		std::fprintf(stderr, "cycles: %llu calls, per call boundary %llu service %llu return %llu\n", (unsigned long long)count,
			(unsigned long long)(count ? boundary / count : 0), (unsigned long long)(count ? service / count : 0),
			(unsigned long long)(returns ? back / returns : 0));
		for (size_t index = 0; index < order.size() && index < 12; ++index) {
			const auto &entry = calls[order[index].second];
			const auto name = names.find(order[index].second);
			std::fprintf(stderr, "cycles:   %-32s %8llu calls  boundary %7llu  service %9llu  return %7llu\n",
				name == names.end() ? "?" : name->second.c_str(), (unsigned long long)entry.count,
				(unsigned long long)(entry.count ? entry.boundary / entry.count : 0), (unsigned long long)(entry.count ? entry.service / entry.count : 0),
				(unsigned long long)(entry.returns ? entry.back / entry.returns : 0));
		}
		calls.clear();
	}
};

}
