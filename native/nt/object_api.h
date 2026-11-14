#pragma once

#include "environment.h"
#include "api.h"
#include "objects.h"

namespace raven {

class NativeObjectAPI {
public:
	NativeObjectAPI(NativeProcess &process, NativeEnvironment &environment, NativeObjects &objects,
		std::chrono::steady_clock::time_point deadline);
	bool dispatch(std::string_view name);
	// Waits belong to the thread scheduler once guest threads exist; only the NT variants stay here.
	void set_cooperative(bool value) { cooperative = value; }
	// With the scheduler, an NT wait that cannot be satisfied lets the other guest threads run until it can be, or its time is up.
	void set_threads(class NativeThreads *value) { threads = value; }
	static std::span<const std::string_view> nt_exports();
	// The calls a served stub answers without stopping the guest: closing a handle and setting or resetting an event.
	// Setting an event that a thread may wait for is not answered without stopping: it runs again stopped, where the thread it wakes can
	// run first.
	static void register_served(ApiRegistry &registry, NativeObjects &objects, class NativeThreads &threads);

private:
	NativeProcess &process;
	NativeEnvironment &environment;
	NativeObjects &objects;
	std::chrono::steady_clock::time_point deadline;
	bool cooperative = false;
	class NativeThreads *threads = nullptr;
	bool file_io(std::string_view name);
};

}
