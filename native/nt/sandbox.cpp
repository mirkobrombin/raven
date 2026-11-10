#include "sandbox.h"
#include "text.h"

#include <cctype>
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
	}
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
