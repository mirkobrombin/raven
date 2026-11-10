#include "sandbox.h"
#include "confine.h"
#include "text.h"

#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace raven {

namespace {

// The real spelling of a path, links followed as far as the path exists: grants and requests compare in this form, so a link cannot lead
// out of a grant and /home names the same folders as /var/home.
std::filesystem::path real(const std::filesystem::path &host)
{
	std::error_code error;
	auto result = std::filesystem::weakly_canonical(host, error);
	return error ? host.lexically_normal() : result;
}

// True when `inner` is `outer` or lies under it.
bool within(const std::filesystem::path &inner, const std::filesystem::path &outer)
{
	const auto &text = inner.native(), &base = outer.native();
	if (base == "/")
		return true;
	return text.size() >= base.size() && text.compare(0, base.size(), base) == 0 && (text.size() == base.size() || text[base.size()] == '/');
}

std::mutex &configuration_lock()
{
	static std::mutex lock;
	return lock;
}

}

Sandbox &sandbox()
{
	static Sandbox instance;
	return instance;
}

void Sandbox::load(const std::filesystem::path &prefix)
{
	directory = prefix;
	configuration_size = -2;
	refresh();
}

void Sandbox::set_program(std::u16string name)
{
	program = std::move(name);
	configuration_size = -2;
	refresh();
}

void Sandbox::refresh() const
{
	if (directory.empty())
		return;
	std::lock_guard guard(configuration_lock());
	const auto file = directory / "sandbox.conf";
	struct stat status{};
	const bool present = ::stat(file.c_str(), &status) == 0;
	// A change is seen by its time and its size: two grants recorded within one tick of the clock still differ in size.
	if (present ? status.st_mtim.tv_sec == configuration_time.tv_sec && status.st_mtim.tv_nsec == configuration_time.tv_nsec &&
			status.st_size == configuration_size : configuration_size == -1)
		return;
	configuration_time = present ? status.st_mtim : timespec{};
	configuration_size = present ? status.st_size : -1;
	configured.clear();
	exposed.clear();
	handlers.clear();
	network_allowed = true;
	std::ifstream input(file);
	for (std::string line; std::getline(input, line);) {
		const auto space = line.find(' ');
		const auto word = line.substr(0, space);
		auto value = space == line.npos ? std::string() : line.substr(space + 1);
		std::u16string owner;
		if (word.starts_with("app-")) {
			const auto tab = value.find('\t');
			owner = from_utf8(value.substr(0, tab));
			value = tab == value.npos ? std::string() : value.substr(tab + 1);
			if (!equal_fold(owner, program))
				continue;
		}
		if ((word == "read" || word == "write" || word == "app-read" || word == "app-write") && !value.empty() && value.front() == '/')
			configured.push_back({real(value), word.ends_with("write"), owner});
		else if (word == "network")
			network_allowed = value != "off";
		else if (word == "app-network-off")
			network_allowed = false;
		else if (word == "host-app" || word == "handler") {
			const auto tab = value.find('\t');
			if (tab == value.npos || !tab || tab + 1 == value.size())
				continue;
			if (word == "host-app")
				exposed.push_back({value.substr(0, tab), value.substr(tab + 1)});
			else
				handlers.emplace_back(value.substr(0, tab), value.substr(tab + 1));
		}
	}
}

std::vector<Sandbox::HostApp> Sandbox::host_apps() const
{
	refresh();
	std::lock_guard guard(configuration_lock());
	return exposed;
}

std::string Sandbox::handler(std::string_view kind) const
{
	refresh();
	std::lock_guard guard(configuration_lock());
	for (const auto &[which, choice] : handlers)
		if (equal_fold(from_utf8(which), from_utf8(kind)))
			return choice;
	return {};
}

Sandbox::Access Sandbox::access_as(const std::filesystem::path &host, std::u16string_view other) const
{
	Sandbox as;
	as.session = session;
	as.system = system;
	as.set_program(std::u16string(other));
	as.load(directory);
	return as.access(host);
}

bool Sandbox::open_on_host(std::string_view name, const std::vector<std::string> &targets) const
{
	// A confined runtime would confine the Linux program too: the broker starts it, after the same checks.
	if (confined())
		return brokered_host(name, targets);
	std::string desktop;
	for (const auto &app : host_apps())
		if (equal_fold(from_utf8(app.name), from_utf8(name)))
			desktop = app.desktop;
	// The desktop entry is found where the desktop finds it; its name is a plain file name.
	if (desktop.empty() || desktop.find('/') != desktop.npos)
		return false;
	if (!desktop.ends_with(".desktop"))
		desktop += ".desktop";
	std::vector<std::string> folders;
	const char *home = std::getenv("HOME"), *data_home = std::getenv("XDG_DATA_HOME"), *data = std::getenv("XDG_DATA_DIRS");
	folders.push_back(data_home && *data_home ? std::string(data_home) : std::string(home ? home : "") + "/.local/share");
	for (std::string_view rest = data && *data ? data : "/usr/local/share:/usr/share"; !rest.empty();) {
		const auto colon = rest.find(':');
		folders.emplace_back(rest.substr(0, colon));
		rest = colon == rest.npos ? std::string_view() : rest.substr(colon + 1);
	}
	std::filesystem::path entry;
	std::error_code error;
	for (const auto &folder : folders)
		if (std::filesystem::is_regular_file(std::filesystem::path(folder) / "applications" / desktop, error)) {
			entry = std::filesystem::path(folder) / "applications" / desktop;
			break;
		}
	if (entry.empty())
		return false;
	// Every path must be one the program that asked reaches; an address must be a web or mail one.
	std::vector<std::string> arguments{"gio", "launch", entry.string()};
	for (const auto &target : targets) {
		const auto colon = target.find(':');
		if (colon != target.npos && colon > 1 && target.front() != '/') {
			const auto scheme = target.substr(0, colon);
			if (scheme != "http" && scheme != "https" && scheme != "mailto" && scheme != "ftp")
				return false;
		} else {
			const auto reach = target.empty() || target.front() != '/' ? Access::none : access_as(target, parent_program);
			const bool in_prefix = !target.empty() && std::string_view(target).starts_with(directory.string() + "/");
			if (!in_prefix && reach != Access::read && reach != Access::write) {
				refuse(target, "open-on-host");
				return false;
			}
		}
		arguments.push_back(target);
	}
	// gio starts the program apart from the runtime, as the desktop would.
	const pid_t child = ::fork();
	if (child == 0) {
		if (::fork() == 0) {
			::setsid();
			std::vector<char *> argv;
			for (auto &argument : arguments)
				argv.push_back(argument.data());
			argv.push_back(nullptr);
			::execvp("gio", argv.data());
			::_exit(127);
		}
		::_exit(0);
	}
	if (child < 0)
		return false;
	int status = 0;
	while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
	return true;
}

void Sandbox::grant(const std::filesystem::path &host, bool write)
{
	const auto path = real(host);
	for (auto &existing : session)
		if (existing.host == path) {
			existing.write = existing.write || write;
			return;
		}
	session.push_back({path, write, {}});
}

void Sandbox::allow_reading(const std::filesystem::path &host)
{
	std::error_code error;
	if (std::filesystem::exists(host, error))
		system.push_back({real(host), false, {}});
}

void Sandbox::allow_system_reading()
{
	std::error_code error;
	allow_reading(std::filesystem::read_symlink("/proc/self/exe", error).parent_path());
	for (const char *fonts : {"/usr/share/fonts", "/usr/local/share/fonts"})
		allow_reading(fonts);
#ifdef RAVEN_FONT_DIRECTORY
	allow_reading(RAVEN_FONT_DIRECTORY);
#endif
}

bool Sandbox::record(const std::filesystem::path &host, bool write, std::u16string_view owner)
{
	if (!trusted_program || directory.empty() || !host.is_absolute())
		return false;
	{
		std::lock_guard guard(configuration_lock());
		std::ofstream output(directory / "sandbox.conf", std::ios::app);
		if (owner.empty())
			output << (write ? "write " : "read ") << real(host).string() << '\n';
		else
			output << (write ? "app-write " : "app-read ") << to_utf8(owner) << '\t' << real(host).string() << '\n';
		if (!output)
			return false;
	}
	refresh();
	return true;
}

std::vector<Sandbox::Grant> Sandbox::grants() const
{
	refresh();
	std::lock_guard guard(configuration_lock());
	auto all = session;
	all.insert(all.end(), configured.begin(), configured.end());
	all.insert(all.end(), system.begin(), system.end());
	return all;
}

bool Sandbox::network() const
{
	refresh();
	return network_allowed;
}

bool Sandbox::reaches(const sockaddr *address, socklen_t length) const
{
	if (!address || length < socklen_t(sizeof(sa_family_t)) || network())
		return true;
	char text[INET6_ADDRSTRLEN] = {};
	if (address->sa_family == AF_INET && length >= socklen_t(sizeof(sockaddr_in))) {
		const auto *ip = reinterpret_cast<const sockaddr_in *>(address);
		if ((ntohl(ip->sin_addr.s_addr) >> 24) == 127)
			return true;
		::inet_ntop(AF_INET, &ip->sin_addr, text, sizeof(text));
	} else if (address->sa_family == AF_INET6 && length >= socklen_t(sizeof(sockaddr_in6))) {
		const auto *ip = reinterpret_cast<const sockaddr_in6 *>(address);
		if (IN6_IS_ADDR_LOOPBACK(&ip->sin6_addr) || (IN6_IS_ADDR_V4MAPPED(&ip->sin6_addr) && ip->sin6_addr.s6_addr[12] == 127))
			return true;
		::inet_ntop(AF_INET6, &ip->sin6_addr, text, sizeof(text));
	} else {
		// A local socket stays on the computer; any other family is a network.
		if (address->sa_family == AF_UNIX)
			return true;
		std::snprintf(text, sizeof(text), "family %d", int(address->sa_family));
	}
	refuse(text, "network");
	return false;
}

Sandbox::Access Sandbox::access(const std::filesystem::path &host) const
{
	if (trusted_program)
		return Access::write;
	const auto path = real(host);
	Access result = Access::none;
	for (const auto &grant : grants()) {
		if (within(path, grant.host)) {
			if (grant.write)
				return Access::write;
			result = Access::read;
		} else if (result == Access::none && within(grant.host, path)) {
			result = Access::traverse;
		}
	}
	return result;
}

void Sandbox::refuse(const std::filesystem::path &host, std::string_view operation) const
{
	if (directory.empty())
		return;
	static std::mutex lock;
	std::lock_guard guard(lock);
	const auto log_file = directory / "sandbox.log";
	std::error_code error;
	// The log keeps the latest refusals: past a megabyte it starts again.
	if (std::filesystem::file_size(log_file, error) > (1u << 20) && !error)
		std::filesystem::remove(log_file, error);
	std::ofstream output(log_file, std::ios::app);
	char stamp[32];
	const auto now = std::time(nullptr);
	std::tm parts{};
	::localtime_r(&now, &parts);
	std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &parts);
	output << stamp << '\t' << (program.empty() ? "-" : to_utf8(program)) << '\t' << operation << '\t' << host.string() << '\n';
}

bool Sandbox::open_address(std::string_view address) const
{
	// A confined runtime cannot reach the desktop's handlers: the broker opens the address, after the same checks.
	if (confined())
		return brokered_address(address);
	const auto scheme = address.substr(0, address.find(':'));
	std::string lower(scheme);
	for (auto &character : lower)
		character = char(std::tolower(static_cast<unsigned char>(character)));
	bool whole = scheme.size() < address.size();
	for (const char character : address)
		whole = whole && static_cast<unsigned char>(character) > ' ' && character != 0x7f;
	if (!whole || (lower != "http" && lower != "https" && lower != "ftp" && lower != "mailto")) {
		refuse(std::string(address), "open-address");
		return false;
	}
	// The portal takes the address as a GVariant string: quoted, its quotes and backslashes escaped.
	std::string quoted = "'";
	for (const char character : address) {
		if (character == '\\' || character == '\'')
			quoted += '\\';
		quoted += character;
	}
	quoted += '\'';
	const std::string text(address);
	const pid_t child = ::fork();
	if (child == 0) {
		::setsid();
		if (::fork() == 0) {
			::execlp("gdbus", "gdbus", "call", "--session", "--dest", "org.freedesktop.portal.Desktop", "--object-path",
				"/org/freedesktop/portal/desktop", "--method", "org.freedesktop.portal.OpenURI.OpenURI", "", quoted.c_str(), "{}",
				static_cast<char *>(nullptr));
			::_exit(127);
		}
		int status = 0;
		::wait(&status);
		if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
			::_exit(0);
		::execlp("xdg-open", "xdg-open", text.c_str(), static_cast<char *>(nullptr));
		::_exit(127);
	}
	return child > 0;
}

std::vector<std::string> Sandbox::child_options() const
{
	std::vector<std::string> options;
	for (const auto &grant : session)
		options.push_back(std::string(grant.write ? "--grant-write=" : "--grant-read=") + grant.host.string());
	if (!program.empty())
		options.push_back("--parent-program=" + to_utf8(program));
	return options;
}

}
