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
// Confines this runtime and the programs it starts; true when the kernel took the confinement. RAVEN_NO_CONFINE=1 leaves it out.
bool confine(const std::filesystem::path &prefix);
bool confined();

// A host path the confinement keeps out but the sandbox grants, opened by the broker; -1 with errno set when the broker refuses it.
int brokered_open(const std::filesystem::path &host, int flags, mode_t mode);
// Starts a trusted program of the runtime through the broker, with the descriptors given at the numbers given; its process id and a
// process descriptor for it, or {-1, -1} with errno set.
std::pair<pid_t, int> brokered_spawn(const std::vector<std::string> &arguments, const std::vector<std::pair<int, int>> &descriptors);
// Opens a web or mail address through the broker.
bool brokered_address(std::string_view address);
// Opens host files and addresses with an exposed Linux program through the broker.
bool brokered_host(std::string_view name, const std::vector<std::string> &targets);

// Whether a program image is one of the runtime's trusted programs: its control panel or its file dialog, in the runtime's own folder.
bool trusted_image(const std::filesystem::path &image);

}
