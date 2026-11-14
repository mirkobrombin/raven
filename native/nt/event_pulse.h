#pragma once

#include "api.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace raven {

struct KernelContext;
class EventObject;
struct GuestThread;

// PulseEvent as Windows does it: every thread waiting on a manual-reset event, or one thread waiting on an auto-reset event, is released
// and the event ends nonsignaled. A cooperative wait only sees its event when the scheduler next looks at that thread, so each released
// thread gets a pulse of its own that lasts until its wait takes it.
class EventPulses {
public:
	explicit EventPulses(KernelContext &context) : context(context) {}
	// False when the handle is not an event.
	bool pulse(uint64_t handle);
	// Revokes the pulses of threads that left their wait another way; the scheduler's services and pumps call it.
	void settle();

private:
	struct Pulse {
		std::shared_ptr<EventObject> event;
		std::shared_ptr<GuestThread> thread;
		std::chrono::steady_clock::time_point since;
	};
	KernelContext &context;
	std::vector<Pulse> pulses;
};

void register_pulse_event(ApiRegistry &registry, KernelContext &context);

}
