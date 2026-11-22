#pragma once

#include <cstddef>
#include <cstdint>

namespace raven {

// The time stamp counter beside the monotonic clock it was read with, and how long the machine had been suspended by then, which the
// monotonic clock leaves out and the counter may not. clock_origin is the sample the process took as it started.
struct ClockSample {
	uint64_t counter, nanoseconds, slept;
};
ClockSample clock_sample();
const ClockSample &clock_origin();

// How the guest's clock preludes turn the time stamp counter into the performance counter, as the runtime publishes it in the served
// block: the offset of the slot to read, written last, and two slots, each the counter value at the base, the count at the base, and the
// multiplier, a 32.32 fixed-point count per tick of the counter. The first calibration is over the time since the runtime started, the
// second over 200 ms once those have passed. The second goes to a slot of its own and is named last, so a reader has one or the other
// whole; it goes on from the count of the first, so the clock does not step. A slot's base lies 2^30 counts back: a reader whose counter
// was read just before the slot was named is not before the base.
class ClockCalibration {
public:
	static constexpr size_t named = 0x800, first = 0x808, second = 0x820;

	explicit ClockCalibration(const ClockSample &started) : origin(started) {}
	bool done() const { return slot == second; }
	void publish(uint64_t *block, const ClockSample &now)
	{
		if (done())
			return;
		if (now.counter <= origin.counter || now.nanoseconds - origin.nanoseconds < (slot ? 200000000ULL : 1000000ULL))
			return;
		const auto multiplier = uint64_t((static_cast<unsigned __int128>(now.nanoseconds - origin.nanoseconds) << 32) /
			(static_cast<unsigned __int128>(now.counter - origin.counter) * 100));
		uint64_t count = now.nanoseconds / 100;
		if (slot) {
			const uint64_t *last = block + slot / 8;
			count = last[1] + uint64_t((static_cast<unsigned __int128>(now.counter - last[0]) * last[2]) >> 32);
		}
		const uint64_t back = 1ULL << 30;
		const size_t next = slot ? second : first;
		uint64_t *published = block + next / 8;
		published[0] = now.counter - back;
		published[1] = count - uint64_t((static_cast<unsigned __int128>(back) * multiplier) >> 32);
		published[2] = multiplier;
		__atomic_store_n(&block[named / 8], uint64_t(next), __ATOMIC_RELEASE);
		slot = next;
	}

private:
	ClockSample origin;
	size_t slot = 0;
};

}
