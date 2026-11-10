#pragma once

#include <ctime>
#include <sys/types.h>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace raven {

// What a program may reach of the host's files: nothing but the prefix, unless the person running it granted a file or a folder. The
// program's own folder and the host paths named on the command line are granted at launch; the file dialog, the control panel and the
// prefix's sandbox.conf grant the rest. Granted paths appear under Z: at their host spelling, and the folders above them only lead to
// them: a folder that leads to a grant lists nothing else.
//
// The runtime's own control panel and file dialog are trusted: they come from the runtime's folder, which no program can change, they see
// every host file, and only they may grant one. No other program may write into them, start threads in them or drive their windows.
class Sandbox {
public:
	enum class Access { none, traverse, read, write };
	struct Grant {
		std::filesystem::path host;
		bool write = false;
		// The program the grant is for, by the Windows path of its executable; empty for every program of the prefix.
		std::u16string program;
	};

	// Reads the prefix's sandbox.conf, and again whenever it changes. A line "read PATH" or "write PATH" grants a path to every program,
	// "app-read PROGRAM<tab>PATH" and "app-write PROGRAM<tab>PATH" to one program, and "network off" or "app-network-off PROGRAM" take
	// the network away.
	void load(const std::filesystem::path &prefix);
	// The program this runtime runs, by the Windows path of its executable, which the grants of one program are matched against.
	void set_program(std::u16string program);
	void set_trusted(bool value) { trusted_program = value; }
	// The program whose runtime started this one, as that runtime names it: the program a trusted file dialog serves.
	void set_parent(std::u16string value) { parent_program = std::move(value); }
	const std::u16string &parent() const { return parent_program; }
	bool trusted() const { return trusted_program; }
	// A grant for this run, handed over at launch and to the programs it starts.
	void grant(const std::filesystem::path &host, bool write);
	// A host folder every program reads as part of the system: the runtime's own files and the host's fonts, which stand for the ones
	// Windows ships. Each runtime adds these itself.
	void allow_reading(const std::filesystem::path &host);
	void allow_system_reading();
	// Records a lasting grant in sandbox.conf; only the control panel and the file dialog may.
	bool record(const std::filesystem::path &host, bool write, std::u16string_view program);
	std::vector<Grant> grants() const;
	bool network() const;
	// The access the program has to a host path, its symbolic links followed: a granted path or anything under it, a folder that leads to
	// a grant, or nothing.
	Access access(const std::filesystem::path &host) const;
	// Notes a refused path in the prefix's sandbox.log, which the control panel shows.
	void refuse(const std::filesystem::path &host, std::string_view operation) const;
	// Opens a web or mail address with the host's handler, through the desktop portal when there is one: the only addresses that leave
	// the sandbox are http, https, ftp and mailto ones, and only whole, unbroken ones. False when the address is refused or nothing opens it.
	bool open_address(std::string_view address) const;
	// The launch options a child runtime needs to keep the grants of this run.
	std::vector<std::string> child_options() const;

private:
	void refresh() const;
	std::filesystem::path directory;
	std::u16string program, parent_program;
	bool trusted_program = false;
	std::vector<Grant> session, system;
	mutable std::vector<Grant> configured;
	mutable bool network_allowed = true;
	mutable timespec configuration_time{};
	mutable off_t configuration_size = -1;
};

Sandbox &sandbox();

}
