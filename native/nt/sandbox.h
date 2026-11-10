#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace raven {

// What a program may reach of the host's files: nothing but the prefix, unless the person running it granted a file or a folder. The
// program's own folder and the host paths named on the command line are granted at launch; the file dialog, the control panel and the
// prefix's sandbox.conf grant the rest. Granted paths appear under Z: at their host spelling, and the folders above them only lead to
// them: a folder that leads to a grant lists nothing else.
class Sandbox {
public:
	enum class Access { none, traverse, read, write };
	struct Grant {
		std::filesystem::path host;
		bool write = false;
	};

	// Reads the prefix's sandbox.conf: a line "read PATH" or "write PATH" grants a path, "network off" takes the network away.
	void load(const std::filesystem::path &prefix);
	void grant(const std::filesystem::path &host, bool write);
	const std::vector<Grant> &grants() const { return granted; }
	bool network() const { return network_allowed; }
	// The access the program has to a host path, its symbolic links followed: a granted path or anything under it, a folder that leads to
	// a grant, or nothing.
	Access access(const std::filesystem::path &host) const;
	// Notes a refused path in the prefix's sandbox.log, which the control panel shows.
	void refuse(const std::filesystem::path &host, std::string_view operation) const;
	// Opens a web or mail address with the host's handler, through the desktop portal when there is one: the only addresses that leave
	// the sandbox are http, https, ftp and mailto ones, and only whole, unbroken ones. False when the address is refused or nothing opens it.
	bool open_address(std::string_view address) const;
	// The launch options a child runtime needs to keep the grants of this one.
	std::vector<std::string> child_options() const;

private:
	std::filesystem::path log_file;
	std::vector<Grant> granted;
	bool network_allowed = true;
};

Sandbox &sandbox();

}
