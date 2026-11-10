#include "sandbox.h"

#include <cctype>
#include <ctime>
#include <fstream>
#include <mutex>
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

}

Sandbox &sandbox()
{
	static Sandbox instance;
	return instance;
}

void Sandbox::load(const std::filesystem::path &prefix)
{
	log_file = prefix / "sandbox.log";
	std::ifstream input(prefix / "sandbox.conf");
	for (std::string line; std::getline(input, line);) {
		const auto space = line.find(' ');
		const auto word = line.substr(0, space);
		const auto value = space == line.npos ? std::string() : line.substr(space + 1);
		if ((word == "read" || word == "write") && !value.empty() && value.front() == '/')
			grant(value, word == "write");
		else if (word == "network")
			network_allowed = value != "off";
	}
}

void Sandbox::grant(const std::filesystem::path &host, bool write)
{
	const auto path = real(host);
	for (auto &existing : granted)
		if (existing.host == path) {
			existing.write = existing.write || write;
			return;
		}
	granted.push_back({path, write});
}

Sandbox::Access Sandbox::access(const std::filesystem::path &host) const
{
	const auto path = real(host);
	Access result = Access::none;
	for (const auto &grant : granted) {
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
	if (log_file.empty())
		return;
	static std::mutex lock;
	std::lock_guard guard(lock);
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
	output << stamp << ' ' << ::getpid() << ' ' << operation << ' ' << host.string() << '\n';
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
	for (const auto &grant : granted)
		options.push_back(std::string(grant.write ? "--grant-write=" : "--grant-read=") + grant.host.string());
	return options;
}

}
