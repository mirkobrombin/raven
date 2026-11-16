#pragma once

#include <waits.h>

namespace raven {

// The process's table of waits on the kernel objects only it holds (native/waits): events, mutexes and semaphores live there until
// another process shares them.
inline const RavenWaits *kernel_waits()
{
	static RavenWaits *const table = raven_waits_new();
	return table;
}

}
