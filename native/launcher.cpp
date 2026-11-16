#include "nt/warm_host.h"
#include "nt/warm_wire.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

// The raven command. The runtime host links the graphics, sound, font and network libraries of the system, and loading them takes a
// start several milliseconds before it does anything; this launcher asks a warm host for the start first, with nothing but the C
// library loaded, and runs the host itself only when no warm host listens.
int main(int argc, char **argv)
{
	char self[4096];
	const auto length = ::readlink("/proc/self/exe", self, sizeof self - 1);
	if (length <= 0) {
		std::perror("raven: cannot find the runtime host");
		return 127;
	}
	std::string host(self, size_t(length));
	host = host.substr(0, host.rfind('/') + 1) + "raven-runtime";
	if (argc > 1 && std::strcmp(argv[1], "--run-process") == 0 && raven::warm::eligible()) {
		if (const auto status = raven::run_on_warm_host(raven::warm::identity_of(host, argc, argv)))
			return *status;
		// No host listened: the runtime leaves one behind for the next start like this one.
		::setenv("RAVEN_WARM_ASKED", "1", 1);
	}
	::execv(host.c_str(), argv);
	std::fprintf(stderr, "raven: cannot run %s: %s\n", host.c_str(), std::strerror(errno));
	return 127;
}
