#include "clock_calibration.h"

#include <array>
#include <cstdlib>
#include <iostream>

namespace {

constexpr uint64_t frequency = 3000000000ULL, millisecond = 1000000, hour = 3600000 * millisecond;

void check(bool condition, const char *message)
{
	if (!condition) {
		std::cerr << message << '\n';
		std::exit(1);
	}
}

struct Machine {
	uint64_t counter = frequency * 100, nanoseconds = 100000 * millisecond, slept = 0;

	void run(uint64_t span)
	{
		counter += span * 3;
		nanoseconds += span;
	}
	// A suspended machine's monotonic clock stands still; its counter goes on, or starts again from zero.
	void suspend(bool restarts)
	{
		slept += hour;
		counter = restarts ? frequency : counter + hour * 3;
	}
	raven::ClockSample sample() const { return {counter, nanoseconds, slept}; }
};

// The count as the preludes compute it.
uint64_t count(const uint64_t *block, uint64_t counter)
{
	const uint64_t *slot = block + block[raven::ClockCalibration::named / 8] / 8;
	return slot[1] + uint64_t((static_cast<unsigned __int128>(counter - slot[0]) * slot[2]) >> 32);
}

bool close_to(uint64_t value, uint64_t wanted)
{
	return value + 10 >= wanted && value <= wanted + 10;
}

void check_rate(const uint64_t *block, const Machine &machine, const char *message)
{
	check(close_to(count(block, machine.counter + frequency) - count(block, machine.counter), 10000000), message);
}

}

int main()
{
	using raven::ClockCalibration;
	const auto named = [](const auto &block) { return block[ClockCalibration::named / 8]; };
	{
		std::array<uint64_t, 512> block{};
		Machine machine;
		ClockCalibration calibration(machine.sample());
		machine.run(5 * millisecond);
		calibration.publish(block.data(), machine.sample());
		check(named(block) == ClockCalibration::first, "the first calibration was not published");
		check(close_to(count(block.data(), machine.counter), machine.nanoseconds / 100), "the count does not go on from the monotonic clock");
		check_rate(block.data(), machine, "the first calibration has the wrong rate");
		machine.run(300 * millisecond);
		const uint64_t before = count(block.data(), machine.counter);
		calibration.publish(block.data(), machine.sample());
		check(calibration.done() && named(block) == ClockCalibration::second, "the second calibration was not published");
		check(close_to(count(block.data(), machine.counter), before), "the second calibration steps");
		check_rate(block.data(), machine, "the second calibration has the wrong rate");
	}
	// A start forked from a warm host has the host's origin, and the machine may have been suspended since.
	for (const bool restarts : {false, true}) {
		std::array<uint64_t, 512> block{};
		Machine machine;
		ClockCalibration calibration(machine.sample());
		machine.suspend(restarts);
		machine.run(5 * millisecond);
		calibration.publish(block.data(), machine.sample());
		check(!named(block), "calibrated over a time the machine was suspended in");
		machine.run(2 * millisecond);
		calibration.publish(block.data(), machine.sample());
		check(named(block) == ClockCalibration::first, "not calibrated again after a suspension");
		check_rate(block.data(), machine, "the calibration after a suspension has the wrong rate");
	}
	for (const bool restarts : {false, true}) {
		std::array<uint64_t, 512> block{};
		Machine machine;
		ClockCalibration calibration(machine.sample());
		machine.run(5 * millisecond);
		calibration.publish(block.data(), machine.sample());
		machine.suspend(restarts);
		machine.run(300 * millisecond);
		calibration.publish(block.data(), machine.sample());
		check(!calibration.done(), "refined over a time the machine was suspended in");
		machine.run(250 * millisecond);
		calibration.publish(block.data(), machine.sample());
		check(calibration.done(), "not refined again after a suspension");
		check_rate(block.data(), machine, "the refinement after a suspension has the wrong rate");
	}
	return 0;
}
