#pragma once

#include <waits.h>

#include <cstdio>
#include <cstdlib>

namespace raven {

// The process's table of waits on the kernel objects only it holds (native/waits): events, mutexes and semaphores live there until
// another process shares them.
inline const RavenWaits *kernel_waits()
{
	static RavenWaits *const table = [] {
		RavenWaits *made = raven_waits_new();
		if (std::getenv("RAVEN_TRACE_WAITS"))
			std::fprintf(stderr, "waits: %s\n", raven_waits_kernel(made) ? "kernel objects through /dev/ntsync" : "the host's own table");
		return made;
	}();
	return table;
}

}
