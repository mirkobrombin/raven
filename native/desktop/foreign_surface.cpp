#include "foreign_surface.h"

#include "shared_pixels.h"
#include "nt/host_names.h"
#include "nt/prefix.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <vector>
#include <mutex>
#include <signal.h>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

namespace raven {

namespace {

constexpr size_t header_size = 4096;
constexpr uint32_t surface_magic = 0x52574653;

// The directory is the same for the whole life of the process; it is made the first time it is asked for.
const std::filesystem::path &window_directory()
{
	static const std::filesystem::path directory = [] {
		std::filesystem::path base;
		if (const char *runtime = std::getenv("XDG_RUNTIME_DIR"); runtime && *runtime)
			base = runtime;
		else
			base = NativePrefix::from_environment().root();
		auto made = base / ("raven-windows-" + prefix_tag());
		std::error_code error;
		std::filesystem::create_directories(made, error);
		return made;
	}();
	return directory;
}

std::string window_name(uint64_t window)
{
	char text[17];
	std::snprintf(text, sizeof(text), "%08llx", static_cast<unsigned long long>(window & 0xffffffffULL));
	return text;
}

}

uint64_t window_handle_base()
{
	// Sixteen bits of window number under a tag from the process id; the tag stays clear of the low handles other objects use.
	static const uint64_t base = uint64_t((uint32_t(::getpid()) % 0x7ef0) + 0x100) << 16;
	return base;
}

// Removes what processes that ended without withdrawing their windows left: a window whose owner is gone, and pixels without a window.
static void sweep_windows(const std::filesystem::path &directory)
{
	std::error_code error;
	std::vector<std::filesystem::path> stale;
	for (const auto &entry : std::filesystem::directory_iterator(directory, error)) {
		const auto path = entry.path();
		if (path.extension() == ".window") {
			std::ifstream input(path);
			long owner = 0;
			if ((input >> owner) && owner > 0 && !(::kill(pid_t(owner), 0) != 0 && errno == ESRCH))
				continue;
			stale.push_back(path);
			stale.push_back(std::filesystem::path(path).replace_extension(".pixels"));
			stale.push_back(std::filesystem::path(path).replace_extension(".traits"));
		} else if (path.extension() == ".pixels" && !std::filesystem::exists(std::filesystem::path(path).replace_extension(".window"), error)) {
			stale.push_back(path);
		} else if (path.extension() == ".post" || path.extension() == ".posting" || path.extension() == ".send" || path.extension() == ".sending" ||
			path.extension() == ".reply" || path.extension() == ".replying") {
			// Messages for a window no longer there, or half written by a poster that ended.
			const auto window = path.filename().string().substr(0, 8);
			if (!std::filesystem::exists(directory / (window + ".window"), error))
				stale.push_back(path);
		}
	}
	for (const auto &path : stale)
		std::filesystem::remove(path, error);
}

void publish_window(uint64_t window, uint32_t width, uint32_t height)
{
	const auto directory = window_directory();
	static std::once_flag swept;
	std::call_once(swept, [&] { sweep_windows(directory); });
	const auto final_path = directory / (window_name(window) + ".window");
	const auto temporary = directory / (window_name(window) + ".window." + std::to_string(::getpid()));
	{
		std::ofstream output(temporary, std::ios::trunc);
		output << ::getpid() << ' ' << width << ' ' << height << '\n';
	}
	std::error_code error;
	std::filesystem::rename(temporary, final_path, error);
}

void withdraw_window(uint64_t window)
{
	const auto directory = window_directory();
	std::error_code error;
	std::filesystem::remove(directory / (window_name(window) + ".window"), error);
	std::filesystem::remove(directory / (window_name(window) + ".pixels"), error);
	std::filesystem::remove(directory / (window_name(window) + ".traits"), error);
}

void publish_window_traits(uint64_t window, const ForeignWindowTraits &traits)
{
	const auto directory = window_directory();
	const auto final_path = directory / (window_name(window) + ".traits");
	const auto temporary = directory / (window_name(window) + ".traits." + std::to_string(::getpid()));
	{
		std::ofstream output(temporary, std::ios::trunc);
		output << traits.style << ' ' << traits.ex_style << ' ' << traits.parent << ' ' << (traits.visible ? 1 : 0) << '\n' << traits.class_name << '\n'
			<< traits.title;
	}
	std::error_code error;
	std::filesystem::rename(temporary, final_path, error);
}

struct ForeignRecord {
	long owner = 0;
	uint32_t width = 0, height = 0;
};

static std::optional<ForeignRecord> foreign_record(uint64_t window)
{
	std::ifstream input(window_directory() / (window_name(window) + ".window"));
	ForeignRecord record;
	if (!(input >> record.owner >> record.width >> record.height) || record.owner <= 0 || record.owner == ::getpid())
		return std::nullopt;
	if (::kill(pid_t(record.owner), 0) != 0 && errno == ESRCH)
		return std::nullopt;
	return record;
}

std::optional<std::pair<uint32_t, uint32_t>> foreign_window_size(uint64_t window)
{
	const auto record = foreign_record(window);
	if (!record)
		return std::nullopt;
	return std::make_pair(record->width, record->height);
}

std::optional<ForeignWindowTraits> foreign_window_traits(uint64_t window)
{
	if (!foreign_record(window))
		return std::nullopt;
	std::ifstream input(window_directory() / (window_name(window) + ".traits"));
	ForeignWindowTraits traits;
	std::string first;
	if (!std::getline(input, first))
		return std::nullopt;
	std::istringstream fields(first);
	int visible = 0;
	if (!(fields >> traits.style >> traits.ex_style >> traits.parent >> visible))
		return std::nullopt;
	traits.visible = visible != 0;
	std::getline(input, traits.class_name);
	// The title is the rest of the file, line breaks and all.
	traits.title.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
	return traits;
}

bool post_foreign_message(uint64_t window, uint32_t message, uint64_t wparam, uint64_t lparam)
{
	if (!foreign_record(window))
		return false;
	static std::atomic<uint64_t> sequence{0};
	char name[64];
	// Window, poster and a running count, all of fixed width, so the names of one poster sort in the order it posted.
	std::snprintf(name, sizeof(name), "%s.%08x.%016llx", window_name(window).c_str(), unsigned(::getpid()),
		static_cast<unsigned long long>(sequence++));
	const auto final_path = window_directory() / (std::string(name) + ".post");
	const auto temporary = window_directory() / (std::string(name) + ".posting");
	{
		std::ofstream output(temporary, std::ios::trunc);
		output << message << ' ' << wparam << ' ' << lparam << '\n';
		if (!output)
			return false;
	}
	std::error_code error;
	std::filesystem::rename(temporary, final_path, error);
	return !error;
}

void take_foreign_messages(const std::function<bool(uint64_t)> &owned, const std::function<void(uint64_t, uint32_t, uint64_t, uint64_t)> &use)
{
	std::vector<std::filesystem::path> posted;
	std::error_code error;
	for (const auto &entry : std::filesystem::directory_iterator(window_directory(), error))
		if (entry.path().extension() == ".post")
			posted.push_back(entry.path());
	std::sort(posted.begin(), posted.end());
	for (const auto &path : posted) {
		const auto name = path.filename().string();
		char *end = nullptr;
		const auto window = std::strtoull(name.substr(0, 8).c_str(), &end, 16);
		if (name.size() < 9 || name[8] != '.' || !end || *end || !owned(window))
			continue;
		std::ifstream input(path);
		uint32_t message = 0;
		uint64_t wparam = 0, lparam = 0;
		const bool read = bool(input >> message >> wparam >> lparam);
		input.close();
		std::filesystem::remove(path, error);
		if (read)
			use(window, message, wparam, lparam);
	}
}

std::optional<uint64_t> send_foreign_message(uint64_t window, uint32_t message, uint64_t wparam, uint64_t lparam, const std::vector<uint8_t> &data,
	std::chrono::milliseconds timeout)
{
	if (!foreign_record(window))
		return std::nullopt;
	static std::atomic<uint64_t> sequence{0};
	char name[64];
	std::snprintf(name, sizeof(name), "%s.%08x.%016llx", window_name(window).c_str(), unsigned(::getpid()),
		static_cast<unsigned long long>(sequence++));
	const auto request = window_directory() / (std::string(name) + ".send");
	const auto temporary = window_directory() / (std::string(name) + ".sending");
	const auto reply = window_directory() / (std::string(name) + ".reply");
	{
		std::ofstream output(temporary, std::ios::trunc | std::ios::binary);
		output << message << ' ' << wparam << ' ' << lparam << ' ' << data.size() << '\n';
		output.write(reinterpret_cast<const char *>(data.data()), std::streamsize(data.size()));
		if (!output)
			return std::nullopt;
	}
	std::error_code error;
	std::filesystem::rename(temporary, request, error);
	if (error)
		return std::nullopt;
	// The owner answers from its message loop; this thread looks for the answer until the time is up or the owner is gone.
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	for (unsigned round = 0;; ++round) {
		std::ifstream input(reply);
		uint64_t result = 0;
		if (input >> result) {
			input.close();
			std::filesystem::remove(reply, error);
			return result;
		}
		if (std::chrono::steady_clock::now() >= deadline || (round % 64 == 63 && !foreign_record(window)))
			break;
		::usleep(round < 100 ? 200 : 2000);
	}
	// A request the owner has not taken yet is taken back; one it took may still be answered, and the sweep removes that answer.
	std::filesystem::remove(request, error);
	return std::nullopt;
}

void take_foreign_sends(const std::function<bool(uint64_t)> &owned,
	const std::function<uint64_t(uint64_t, uint32_t, uint64_t, uint64_t, const std::vector<uint8_t> &)> &use)
{
	std::vector<std::filesystem::path> sent;
	std::error_code error;
	for (const auto &entry : std::filesystem::directory_iterator(window_directory(), error))
		if (entry.path().extension() == ".send")
			sent.push_back(entry.path());
	std::sort(sent.begin(), sent.end());
	for (const auto &path : sent) {
		const auto name = path.filename().string();
		char *end = nullptr;
		const auto window = std::strtoull(name.substr(0, 8).c_str(), &end, 16);
		if (name.size() < 9 || name[8] != '.' || !end || *end || !owned(window))
			continue;
		std::ifstream input(path, std::ios::binary);
		uint32_t message = 0;
		uint64_t wparam = 0, lparam = 0, size = 0;
		if (!(input >> message >> wparam >> lparam >> size) || size > (64U << 20)) {
			std::filesystem::remove(path, error);
			continue;
		}
		input.ignore(1);
		std::vector<uint8_t> data(size);
		input.read(reinterpret_cast<char *>(data.data()), std::streamsize(size));
		const bool complete = input.gcount() == std::streamsize(size);
		input.close();
		// Taking the request away first tells a sender that gave up that its answer is coming anyway.
		std::filesystem::remove(path, error);
		if (error || !complete)
			continue;
		const auto result = use(window, message, wparam, lparam, data);
		const auto stem = std::filesystem::path(path).replace_extension("");
		const auto temporary = std::filesystem::path(stem.string() + ".replying");
		{
			std::ofstream output(temporary, std::ios::trunc);
			output << result << '\n';
		}
		std::filesystem::rename(temporary, std::filesystem::path(stem.string() + ".reply"), error);
	}
}

std::vector<uint64_t> foreign_windows()
{
	std::vector<uint64_t> found;
	std::error_code error;
	for (const auto &entry : std::filesystem::directory_iterator(window_directory(), error)) {
		const auto path = entry.path();
		if (path.extension() != ".window")
			continue;
		const auto stem = path.stem().string();
		char *end = nullptr;
		const auto low = std::strtoull(stem.c_str(), &end, 16);
		if (stem.size() != 8 || !end || *end)
			continue;
		// A handle keeps its process tag in the bits above the low word the name carries; window handles fit in those 32 bits.
		if (foreign_record(low))
			found.push_back(low);
	}
	return found;
}

std::optional<uint32_t> foreign_window_owner(uint64_t window)
{
	const auto record = foreign_record(window);
	if (!record)
		return std::nullopt;
	return uint32_t(record->owner);
}

std::shared_ptr<ForeignSurface> ForeignSurface::open_for_drawing(uint64_t window, uint32_t width, uint32_t height)
{
	if (!width || !height || width > 16384 || height > 16384)
		return nullptr;
	const auto path = window_directory() / (window_name(window) + ".pixels");
	const int file = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (file < 0)
		return nullptr;
	const size_t bytes = (size_t(width) * height * 4 + 4095) & ~size_t(4095);
	struct stat status{};
	if (::fstat(file, &status) != 0 || (size_t(status.st_size) < header_size + bytes && ::ftruncate(file, off_t(header_size + bytes)) != 0)) {
		::close(file);
		return nullptr;
	}
	void *mapped = ::mmap(nullptr, header_size, PROT_READ | PROT_WRITE, MAP_SHARED, file, 0);
	if (mapped == MAP_FAILED) {
		::close(file);
		return nullptr;
	}
	auto surface = std::shared_ptr<ForeignSurface>(new ForeignSurface());
	surface->file = file;
	surface->header = static_cast<uint32_t *>(mapped);
	surface->width = width;
	surface->height = height;
	try {
		surface->pixels = std::make_shared<SharedPixels>(file, header_size, bytes);
	} catch (...) {
		return nullptr;
	}
	surface->header[1] = width;
	surface->header[2] = height;
	surface->header[0] = surface_magic;
	return surface;
}

ForeignSurface::~ForeignSurface()
{
	if (header)
		::munmap(header, header_size);
	if (file >= 0)
		::close(file);
}

void ForeignSurface::publish_frame()
{
	if (!header)
		return;
	header[1] = width;
	header[2] = height;
	std::atomic_ref<uint32_t>(header[3]).fetch_add(1, std::memory_order_release);
}

uint64_t foreign_frames_stamp()
{
	struct stat status{};
	if (::stat(window_directory().c_str(), &status) != 0)
		return 0;
	return uint64_t(status.st_mtim.tv_sec) * 1000000000ULL + uint64_t(status.st_mtim.tv_nsec);
}

int foreign_frames_descriptor()
{
	static const int descriptor = [] {
		const int watch = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
		if (watch >= 0 && ::inotify_add_watch(watch, window_directory().c_str(), IN_CREATE | IN_MOVED_TO | IN_DELETE) < 0) {
			::close(watch);
			return -1;
		}
		return watch;
	}();
	return descriptor;
}

void drain_foreign_frames_descriptor()
{
	alignas(inotify_event) char events[4096];
	const int descriptor = foreign_frames_descriptor();
	while (descriptor >= 0 && ::read(descriptor, events, sizeof events) > 0) {}
}

bool take_foreign_frame(uint64_t window, uint32_t &seen, const std::function<void(const uint8_t *, uint32_t, uint32_t)> &use)
{
	const auto path = window_directory() / (window_name(window) + ".pixels");
	const int file = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
	if (file < 0)
		return false;
	struct stat status{};
	bool taken = false;
	if (::fstat(file, &status) == 0 && size_t(status.st_size) > header_size) {
		void *mapped = ::mmap(nullptr, size_t(status.st_size), PROT_READ, MAP_SHARED, file, 0);
		if (mapped != MAP_FAILED) {
			const auto *header = static_cast<const uint32_t *>(mapped);
			const uint32_t sequence = std::atomic_ref<const uint32_t>(header[3]).load(std::memory_order_acquire);
			const uint32_t width = header[1], height = header[2];
			if (header[0] == surface_magic && sequence && sequence != seen && width && height &&
				header_size + size_t(width) * height * 4 <= size_t(status.st_size)) {
				use(static_cast<const uint8_t *>(mapped) + header_size, width, height);
				seen = sequence;
				taken = true;
			}
			::munmap(mapped, size_t(status.st_size));
		}
	}
	::close(file);
	return taken;
}

}
