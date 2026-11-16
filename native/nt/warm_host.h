#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace raven {

// A host kept warm for the next start of the same program, as the Windows half of a process takes most of its start to make and is the
// same each time: the system libraries' images, their exports and the program's graph bound to them. A start that finds no warm host
// leaves one listening behind it; the next start with the same program, arguments, directory and environment is prepared there, and the
// host stops just before the guest process is made. Every start after that is a fork of the stopped host, which goes on from there with
// the client's standard streams. Only a start with nothing open but its standard streams takes part: what else a parent hands down
// cannot be passed along. RAVEN_WARM=0 turns it off, and RAVEN_WARM_SECONDS is how long a host waits for a start before it ends (10).

// Runs a start on the warm host that listens for its identity (see warm_wire.h): the exit status the start ended with, or nothing when it
// must run here. The raven launcher asks before the runtime host is even loaded.
std::optional<int> run_on_warm_host(const std::string &identity);
// Leaves a host listening for the next start like this one; returns at once here, and again in the process that prepares a start.
void start_warm_host(int argc, char **argv);
// Where a prepared host stops: in an ordinary start it returns at once. In the host it returns only in each process forked for a start,
// after `detach` gave that process its own copies of what it will write to. The host forks each start before its request comes.
void warm_point(const std::function<void()> &detach);
// Where a start forked ahead waits for its request, just before its guest runs: it returns true with the request's streams in place, after
// the work warm_when_ready put off until then, or ends the process when the host ended without one. In any other start it returns false
// at once.
bool warm_ready();
// Whether this is a start forked ahead that has not had its request yet.
bool warm_waiting();
// Work that needs a start's own standard streams: done at once, or in a start forked ahead, once its request's streams are in place.
void warm_when_ready(std::function<void()> work);
// Work each start must do in its own process, as confining it: done at once in an ordinary start, and in a process that prepares a host,
// in each start forked from it at the warm point.
void warm_defer(std::function<void()> work);
// A start that makes its guest process without stopping at the warm point (a 32-bit one) leaves no host behind, and does the deferred work
// itself before the process exists.
void warm_settle();
// A file or folder whose contents decide what a prepared host holds: a start finds the host stale once one of them changed.
void warm_watch(const std::filesystem::path &path);

}
