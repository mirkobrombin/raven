#include "shared_gpu_memory.h"
#include "cpu/native_cpu.h"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <memory>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <vector>

namespace raven {

namespace {

bool drm_node(const struct stat &status)
{
	return S_ISCHR(status.st_mode) && major(status.st_rdev) == 226;
}

// NVIDIA's driver maps through its own nodes, /dev/nvidiactl and /dev/nvidia0, one open description for each mapping.
bool nvidia_node(const struct stat &status)
{
	return S_ISCHR(status.st_mode) && major(status.st_rdev) == 195;
}

// Many descriptions of one NVIDIA node share its inode, and each answers only for the mapping it was set up for: a byte written
// through the trial mapping must come back through the driver's own.
bool same_memory(void *alias, void *host)
{
	auto *mine = static_cast<volatile uint8_t *>(alias);
	const auto *theirs = static_cast<volatile const uint8_t *>(host);
	const uint8_t original = *theirs;
	*mine = uint8_t(original ^ 0xa5);
	std::atomic_thread_fence(std::memory_order_seq_cst);
	const bool same = *theirs == uint8_t(original ^ 0xa5);
	*mine = original;
	std::atomic_thread_fence(std::memory_order_seq_cst);
	return same && *theirs == original;
}

}

std::optional<GpuMapping> map_gpu_memory(NativeProcess &process, void *host, size_t size)
{
	const uint64_t address = reinterpret_cast<uintptr_t>(host);
	if (!size || size > UINT64_MAX - address)
		return std::nullopt;
	std::ifstream maps("/proc/self/maps");
	std::string line;
	uint64_t first = 0, backing = 0, inode = 0, last_of = 0;
	unsigned device_major = 0, device_minor = 0;
	bool found = false, drm = false, node = false, nvidia = false;
	while (std::getline(maps, line)) {
		uint64_t last = 0;
		char access[5]{};
		int path = 0;
		if (std::sscanf(line.c_str(), "%" SCNx64 "-%" SCNx64 " %4s %" SCNx64 " %x:%x %" SCNu64 " %n",
			&first, &last, access, &backing, &device_major, &device_minor, &inode, &path) != 7 ||
			first > address || last < address + size)
			continue;
		drm = path > 0 && std::string_view(line).substr(size_t(path)) == "anon_inode:i915.gem";
		if (std::string_view(access) != "rw-s" || (!drm && !inode))
			return std::nullopt;
		found = true;
		last_of = last;
		break;
	}
	if (!found)
		return std::nullopt;
	std::unique_ptr<DIR, int (*)(DIR *)> directory(opendir("/proc/self/fd"), closedir);
	if (!directory)
		return std::nullopt;
	std::optional<dev_t> device;
	std::vector<int> candidates;
	while (auto *entry = readdir(directory.get())) {
		char *end = nullptr;
		const auto number = std::strtol(entry->d_name, &end, 10);
		if (*end || number < 0 || number > INT32_MAX)
			continue;
		struct stat status{};
		if (fstat(int(number), &status) != 0)
			continue;
		if (drm) {
			if (!drm_node(status))
				continue;
			// DRM offsets belong to the open file description. Multiple nodes leave the owner ambiguous.
			if (device && *device != status.st_rdev)
				return std::nullopt;
			device = status.st_rdev;
		} else if ((!S_ISREG(status.st_mode) && !drm_node(status) && !nvidia_node(status)) || status.st_dev != makedev(device_major, device_minor) || status.st_ino != inode) {
			continue;
		} else {
			// Other DRM drivers, amdgpu among them, map through the device node itself. Its offsets too belong to one open description of
			// it, and only that one maps them in the trial below.
			nvidia = nvidia_node(status);
			node = drm_node(status) || nvidia;
		}
		candidates.push_back(int(number));
	}
	// The node finds an object only by the offset of its first page, so the guest's view starts where the driver's own mapping does.
	const uint64_t from = node ? first : address & ~uint64_t(4095), delta = address - from;
	if (backing > UINT64_MAX - (from - first) || size > SIZE_MAX - delta - 4095)
		return std::nullopt;
	const uint64_t offset = backing + (from - first);
	// It also checks a mapping against the one it set up, so the guest's view spans the driver's whole mapping.
	const size_t length = nvidia ? size_t(last_of - first) : (size + size_t(delta) + 4095) & ~size_t(4095);
	if (offset > INT64_MAX || length > INT64_MAX - offset)
		return std::nullopt;
	for (const auto candidate : candidates) {
		const int file = fcntl(candidate, F_DUPFD_CLOEXEC, 0);
		if (file < 0)
			continue;
		struct stat status{};
		// A device node has no size to check the range against; the driver refuses a range its object does not cover.
		if (fstat(file, &status) != 0 || (drm ? !drm_node(status) || !device || status.st_rdev != *device :
			status.st_dev != makedev(device_major, device_minor) || status.st_ino != inode || (!drm_node(status) && !nvidia_node(status) && (!S_ISREG(status.st_mode) ||
			status.st_size < 0 || offset > uint64_t(status.st_size) || length > uint64_t(status.st_size) - offset)))) {
			close(file);
			continue;
		}
		void *alias = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, file, off_t(offset));
		if (alias == MAP_FAILED) {
			close(file);
			continue;
		}
		const bool matches = !nvidia || same_memory(static_cast<uint8_t *>(alias) + delta, host);
		munmap(alias, length);
		if (!matches) {
			close(file);
			continue;
		}
		try {
			const auto base = process.map_descriptor(file, length, offset);
			close(file);
			return GpuMapping{base + delta, base};
		} catch (...) {
			close(file);
			throw;
		}
	}
	return std::nullopt;
}

}
