#pragma once

#include <ctime>
#include <sys/socket.h>
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
	const std::u16string &running() const { return program; }
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
	// Whether a program may send to an address: any address while the network is on, and otherwise only the computer's own (loopback
	// and local sockets). A refused address is noted in the log.
	bool reaches(const sockaddr *address, socklen_t length) const;
	// The access the program has to a host path, its symbolic links followed: a granted path or anything under it, a folder that leads to
	// a grant, or nothing.
	Access access(const std::filesystem::path &host) const;
	// Notes a refused path in the prefix's sandbox.log, which the control panel shows.
	void refuse(const std::filesystem::path &host, std::string_view operation) const;
	// Opens a web or mail address with the host's handler, through the desktop portal when there is one: the only addresses that leave
	// the sandbox are http, https, ftp and mailto ones, and only whole, unbroken ones. False when the address is refused or nothing opens it.
	bool open_address(std::string_view address) const;
	// The Linux programs the prefix shows its programs, each as C:\Program Files\Raven\Host\NAME.exe: a line "host-app NAME<tab>ID" of
	// sandbox.conf names the program by its desktop entry.
	struct HostApp {
		std::string name, desktop;
	};
	std::vector<HostApp> host_apps() const;
	// Who opens a kind of file (".txt") or of address ("mailto"): a line "handler KIND<tab>windows", "handler KIND<tab>linux:NAME" or
	// "handler KIND<tab>ask"; empty when no line names it, and a program of the prefix opens it then.
	std::string handler(std::string_view kind) const;
	// The access another program of the prefix has: the program that started this one, whose files a bridge to a Linux program passes on.
	Access access_as(const std::filesystem::path &host, std::u16string_view other) const;
	// Opens host files and addresses with a Linux program the prefix exposes, for the program that started this one: only what that
	// program reaches. False when the program is not exposed or a path is refused.
	bool open_on_host(std::string_view name, const std::vector<std::string> &targets) const;
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
	mutable std::vector<HostApp> exposed;
	mutable std::vector<std::pair<std::string, std::string>> handlers;
	mutable timespec configuration_time{};
	mutable off_t configuration_size = -1;
};

Sandbox &sandbox();

}
