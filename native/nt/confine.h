#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <utility>
#include <vector>

namespace raven {

// The runtime confined by the host kernel (Landlock), beneath the checks of the NT layer: should a program find its way past those, the
// runtime itself still reaches only the prefix, its own files, the system's libraries and devices, and the host paths granted when it
// started. What the confinement cannot know in advance goes through a broker, a process started before the confinement that the runtime
// asks: it opens a host path granted later (by the file dialog, while the program runs) after checking the grant itself, starts the
// runtime's trusted programs, which see the whole host, and opens web addresses with the host's handler.
//
// Confines this runtime and the programs it starts; true when the kernel took the confinement. RAVEN_NO_CONFINE=1 leaves it out. Abstract
// unix sockets outside the confinement are closed too where the kernel can, unless `abstract_sockets` keeps them for an X11 display,
// which the X library reaches through one.
bool confine(const std::filesystem::path &prefix, bool abstract_sockets = false);
bool confined();

// A host path the confinement keeps out but the sandbox grants, opened by the broker; -1 with errno set when the broker refuses it.
int brokered_open(const std::filesystem::path &host, int flags, mode_t mode);
// Starts a trusted program of the runtime through the broker, with the descriptors given at the numbers given; its process id and a
// process descriptor for it, or {-1, -1} with errno set.
std::pair<pid_t, int> brokered_spawn(const std::vector<std::string> &arguments, const std::vector<std::pair<int, int>> &descriptors);
// mkdir, rmdir or unlink, and rename on host paths, as the system calls do them (0, or -1 with errno set): when the confinement refuses one
// on a path the sandbox granted for writing after it started, the broker does it.
int host_mkdir(const std::filesystem::path &host);
int host_remove(const std::filesystem::path &host, bool directory);
int host_rename(const std::filesystem::path &from, const std::filesystem::path &to);
// Opens a web or mail address through the broker.
bool brokered_address(std::string_view address);
// Shows a notification on the Linux desktop through the broker.
bool brokered_notice(std::string_view application, std::string_view title, std::string_view text);
// Opens host files and addresses with an exposed Linux program through the broker.
bool brokered_host(std::string_view name, const std::vector<std::string> &targets);

// A runtime started by a confined runtime runs inside that confinement too, whatever its own, and so does any broker it starts: what only
// the host can do, starting a Linux program or opening an address, goes to the broker of the outermost confined runtime instead, over a
// channel each runtime hands the runtimes it starts. A new channel to that broker for a runtime about to start, or -1 when this runtime
// runs in no confinement, so that what it starts reaches the host itself.
int host_broker_channel();
// Takes the channel the runtime that started this one handed it.
void adopt_host_broker(int descriptor);
bool host_brokered();
// Through that channel: opens host files and addresses with an exposed Linux program for the program `caller` (UTF-8), which the broker
// checks the targets against together with its own program, and opens a web or mail address.
bool forwarded_host(std::string_view name, const std::vector<std::string> &targets, std::string_view caller);
bool forwarded_address(std::string_view address);
bool forwarded_notice(std::string_view application, std::string_view title, std::string_view text);

// Whether a program image is one of the runtime's trusted programs: its control panel or its file dialog, in the runtime's own folder.
bool trusted_image(const std::filesystem::path &image);

}
