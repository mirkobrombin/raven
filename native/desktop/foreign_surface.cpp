#include "foreign_surface.h"

#include "shared_pixels.h"
#include "nt/host_names.h"
#include "nt/prefix.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
		output << traits.style << ' ' << traits.ex_style << '\n' << traits.class_name;
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
	if (!(input >> traits.style >> traits.ex_style))
		return std::nullopt;
	input.ignore(1);
	std::getline(input, traits.class_name);
	return traits;
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
