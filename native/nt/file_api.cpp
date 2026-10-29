#include "child_process.h"
#include "console.h"
#include "event.h"
#include "mutex.h"
#include "threads.h"
#include "handle_channel.h"
#include "pipe.h"
#include "section.h"
#include "security_attributes.h"
#include "kernel_context.h"
#include "file_objects.h"
#include "restart.h"
#include "text.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <unistd.h>

namespace raven {

FileObject::~FileObject()
{
	if (descriptor >= 0)
		::close(descriptor);
	if (lock_descriptor >= 0)
		::close(lock_descriptor);
	if (flags & 0x04000000U) {
		if (directory)
			::rmdir(path.c_str());
		else
			::unlink(path.c_str());
	}
}

namespace {

constexpr uint64_t invalid_handle = UINT64_MAX;
constexpr uint32_t attribute_readonly = 0x1, attribute_hidden = 0x2, attribute_directory = 0x10, attribute_archive = 0x20,
	attribute_normal = 0x80, attribute_reparse = 0x400;

uint64_t filetime(const timespec &time)
{
	return (uint64_t(time.tv_sec) * 10000000ULL) + uint64_t(time.tv_nsec) / 100 + filetime_epoch_offset;
}

timespec timespec_from(uint64_t ticks)
{
	const uint64_t since_unix = ticks - filetime_epoch_offset;
	return {time_t(since_unix / 10000000ULL), long(since_unix % 10000000ULL * 100)};
}

uint32_t attributes_of(const struct stat &metadata, const std::filesystem::path &path)
{
	uint32_t attributes = S_ISDIR(metadata.st_mode) ? attribute_directory : attribute_archive;
	if (S_ISLNK(metadata.st_mode))
		attributes |= attribute_reparse;
	if (!(metadata.st_mode & 0222))
		attributes |= attribute_readonly;
	const auto name = path.filename().string();
	if (name.size() > 1 && name[0] == '.' && name != "..")
		attributes |= attribute_hidden;
	return attributes;
}

uint32_t error_of(int error, bool missing_parent = false)
{
	switch (error) {
	case ENOENT: return missing_parent ? 3 : 2;
	case ENOTDIR: return 3;
	case EACCES: case EPERM: case EROFS: case EISDIR: return 5;
	case EEXIST: return 80;
	case ENOTEMPTY: return 145;
	case ENOSPC: case EDQUOT: return 112;
	case EMFILE: case ENFILE: return 4;
	case ENAMETOOLONG: return 206;
	case EXDEV: return 17;
	case EBUSY: case ETXTBSY: return 32;
	case EINVAL: return 87;
	case EBADF: return 6;
	default: return 31;
	}
}

// Windows wildcard matching: * and ? against a file name, case-insensitively; "*.*" matches every name.
bool wildcard(std::u16string_view pattern, std::u16string_view name)
{
	if (pattern == u"*.*" || pattern == u"*")
		return true;
	size_t p = 0, n = 0, star = std::u16string_view::npos, mark = 0;
	while (n < name.size()) {
		if (p < pattern.size() && (pattern[p] == u'?' || pattern[p] == u'>' || equal_fold(pattern.substr(p, 1), name.substr(n, 1)))) {
			++p;
			++n;
		} else if (p < pattern.size() && (pattern[p] == u'*' || pattern[p] == u'<')) {
			star = p++;
			mark = n;
		} else if (star != std::u16string_view::npos) {
			p = star + 1;
			n = ++mark;
		} else {
			return false;
		}
	}
	while (p < pattern.size() && (pattern[p] == u'*' || pattern[p] == u'<' || pattern[p] == u'.'))
		++p;
	return p == pattern.size();
}

FindObject::Entry entry_of(const std::filesystem::path &path, const std::u16string &name)
{
	FindObject::Entry entry{name, attribute_normal, 0, 0, 0, 0};
	struct stat metadata{};
	if (::lstat(path.c_str(), &metadata)) {
		metadata = {};
		return entry;
	}
	struct stat target{};
	const bool link = S_ISLNK(metadata.st_mode);
	if (link && !::stat(path.c_str(), &target))
		metadata = target;
	// A link reports what it points at, marked as the reparse point it is.
	entry.attributes = attributes_of(metadata, path) | (link ? attribute_reparse : 0);
	entry.creation = filetime(metadata.st_ctim);
	entry.access = filetime(metadata.st_atim);
	entry.write = filetime(metadata.st_mtim);
	entry.size = S_ISDIR(metadata.st_mode) ? 0 : uint64_t(metadata.st_size);
	return entry;
}

void put32(std::vector<uint8_t> &bytes, size_t at, uint32_t value)
{
	std::memcpy(bytes.data() + at, &value, 4);
}

void put64(std::vector<uint8_t> &bytes, size_t at, uint64_t value)
{
	std::memcpy(bytes.data() + at, &value, 8);
}

// WIN32_FIND_DATAW is 592 bytes: attributes, three times, size, two reserved words, a 260-character name and a short name.
std::vector<uint8_t> find_data(const FindObject::Entry &entry)
{
	std::vector<uint8_t> bytes(592);
	put32(bytes, 0, entry.attributes);
	put64(bytes, 4, entry.creation);
	put64(bytes, 12, entry.access);
	put64(bytes, 20, entry.write);
	put32(bytes, 28, uint32_t(entry.size >> 32));
	put32(bytes, 32, uint32_t(entry.size));
	const auto name = entry.name.substr(0, 259);
	std::memcpy(bytes.data() + 44, name.data(), name.size() * 2);
	return bytes;
}

std::vector<uint8_t> attribute_data(const FindObject::Entry &entry)
{
	std::vector<uint8_t> bytes(36);
	put32(bytes, 0, entry.attributes);
	put64(bytes, 4, entry.creation);
	put64(bytes, 12, entry.access);
	put64(bytes, 20, entry.write);
	put32(bytes, 28, uint32_t(entry.size >> 32));
	put32(bytes, 32, uint32_t(entry.size));
	return bytes;
}

}

// A change notification handle: an inotify instance watching a directory, and the directories below it when the caller asked for the
// tree. It is signaled while changes wait to be taken in by FindNextChangeNotification. Nothing announces a change to a waiter, so a
// waiter looks again at a short interval.
class ChangeNotificationObject : public KernelObject {
public:
	explicit ChangeNotificationObject(int descriptor) : descriptor(descriptor) {}
	~ChangeNotificationObject() override { ::close(descriptor); }
	int signaled(uint32_t thread) const override
	{
		(void)thread;
		pollfd entry{descriptor, POLLIN, 0};
		return ::poll(&entry, 1, 0) > 0 ? 1 : 0;
	}
	std::optional<std::chrono::steady_clock::time_point> wakes_at() const override
	{
		return std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
	}
	void take_changes()
	{
		char buffer[4096];
		while (::read(descriptor, buffer, sizeof buffer) > 0) {
		}
	}
	const int descriptor;
};

void register_file_api(ApiRegistry &registry, KernelContext &context)
{
	auto &paths = context.paths;
	auto &objects = context.objects;
	const auto file_of = [&](uint64_t handle) { return objects.object_as<FileObject>(handle); };
	// Host errors become Windows last errors; a missing parent directory reads as path-not-found.
	const auto resolve = [&](ApiCall &call, std::u16string_view name) -> std::optional<NativePaths::Resolved> {
		const auto resolved = paths.resolve(name);
		if (!resolved)
			call.set_last_error(name.empty() ? 2 : 123);
		return resolved;
	};
	const auto missing = [&](const NativePaths::Resolved &resolved) { return resolved.parent_exists ? 2U : 3U; };

	// CreateFileW and CreateFile2 open the same way; each reads its own arguments, the security attributes among them.
	const auto create_file = [&, resolve, missing](ApiCall &call, const std::u16string &name, uint32_t access, uint32_t disposition, uint32_t flags,
		uint64_t security) {
		if (open_pipe_client(context, call, name, access, flags))
			return;
		call.ret(invalid_handle);
		if (disposition < 1 || disposition > 5) {
			call.set_last_error(87);
			return;
		}
		if (NativePaths::device(name)) {
			if (open_console_device(context, call, name, access, inheritable_handle(call, security)))
				return;
			const int descriptor = ::open("/dev/null", O_RDWR | O_CLOEXEC);
			const auto made = objects.add_object(std::make_shared<FileObject>(descriptor, access, false, "/dev/null", u"NUL", 0));
			{ if (!made.status && inheritable_handle(call, security)) objects.set_inherit(made.value, true); call.ret(made.status ? invalid_handle : made.value); }
			return;
		}
		const auto resolved = resolve(call, name);
		if (!resolved)
			return;
		const bool want_write = access & (0x40000000U | 0x10000000U | 0x2U | 0x4U | 0x10000U);
		const bool want_read = access & (0x80000000U | 0x10000000U | 0x1U);
		struct stat metadata{};
		const bool exists = resolved->exists && !::stat(resolved->host.c_str(), &metadata);
		if (exists && S_ISDIR(metadata.st_mode)) {
			if (!(flags & 0x02000000U) || disposition == 1 || disposition == 2 || disposition == 5) {
				call.set_last_error(disposition == 1 ? 80 : 5);
				return;
			}
			const int descriptor = ::open(resolved->host.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
			if (descriptor < 0) {
				call.set_last_error(error_of(errno));
				return;
			}
			const auto made = objects.add_object(std::make_shared<FileObject>(descriptor, access, true, resolved->host, paths.full(name), flags));
			{ if (!made.status && inheritable_handle(call, security)) objects.set_inherit(made.value, true); call.ret(made.status ? invalid_handle : made.value); }
			return;
		}
		int mode = want_read && want_write ? O_RDWR : want_write ? O_WRONLY : O_RDONLY;
		// FILE_APPEND_DATA without a right to write at a position means every write goes to the end of the file.
		if ((access & 0x4U) && !(access & (0x40000000U | 0x10000000U | 0x2U)))
			mode |= O_APPEND;
		if (disposition == 1) {
			if (exists) {
				call.set_last_error(80);
				return;
			}
			mode |= O_CREAT | O_EXCL;
		} else if (disposition == 2) {
			mode |= O_CREAT;
			if (want_write)
				mode |= O_TRUNC;
		} else if (disposition == 4) {
			mode |= O_CREAT;
		} else if (disposition == 5) {
			if (!want_write) {
				call.set_last_error(87);
				return;
			}
			if (!exists) {
				call.set_last_error(missing(*resolved));
				return;
			}
			mode |= O_TRUNC;
		} else if (!exists) {
			call.set_last_error(missing(*resolved));
			return;
		}
		if (!exists && !resolved->parent_exists) {
			call.set_last_error(3);
			return;
		}
		const int descriptor = ::open(resolved->host.c_str(), mode | O_CLOEXEC, 0666);
		if (descriptor < 0) {
			call.set_last_error(error_of(errno, !resolved->parent_exists));
			return;
		}
		// CREATE_ALWAYS and OPEN_ALWAYS report an existing file through a successful call with ERROR_ALREADY_EXISTS.
		call.set_last_error(exists && (disposition == 2 || disposition == 4) ? 183 : 0);
		if (disposition == 2 && exists && !want_write)
			::ftruncate(descriptor, 0);
		const auto made = objects.add_object(std::make_shared<FileObject>(descriptor, access, false, resolved->host, paths.full(name), flags));
		{ if (!made.status && inheritable_handle(call, security)) objects.set_inherit(made.value, true); call.ret(made.status ? invalid_handle : made.value); }
	};
	registry.add("CreateFileW", 7, [create_file](ApiCall &call) {
		create_file(call, call.wide(call[0]), call.u32(1), call.u32(4), call.u32(5), optional_argument(call, 3));
	});
	// The extended parameters hold the attributes, the flags and the quality-of-service bits that CreateFileW takes in one word, and the
	// security attributes.
	registry.add("CreateFile2", 5, [create_file](ApiCall &call) {
		uint32_t flags = 0;
		uint64_t security = 0;
		if (call[4]) {
			flags = call.load<uint32_t>(call[4] + 4) | call.load<uint32_t>(call[4] + 8) | call.load<uint32_t>(call[4] + 12);
			security = call.pointer_width() == 8 ? call.load<uint64_t>(call[4] + 16) : call.load<uint32_t>(call[4] + 16);
		}
		create_file(call, call.wide(call[0]), call.u32(1), call.u32(3), flags, security);
	});
	registry.add("ReadFile", 5, [&, resolve, missing, file_of](ApiCall &call) {
		if (transfer_pipe(context, call, true) || read_console_file(context, call))
			return;
		// The standard handles inherited from the host read straight from their descriptors.
		if (!file_of(call[0])) {
			std::vector<uint8_t> buffer(std::min<uint32_t>(call.u32(2), 1 << 20));
			const auto result = objects.read_file(call[0], buffer);
			if (!result.status && !call[4]) {
				call.write(call[1], std::span<const uint8_t>(buffer.data(), size_t(result.value)));
				if (call[3]) call.store<uint32_t>(call[3], uint32_t(result.value));
				call.ret(1);
				return;
			}
		}
		const auto file = file_of(call[0]);
		if (!file || file->directory || !file->readable()) {
			call.fail(file && file->directory ? 5 : file ? 5 : 6);
			return;
		}
		if (call[3]) call.store<uint32_t>(call[3], 0);
		else if (!call[4]) {
			call.fail(87);
			return;
		}
		std::vector<uint8_t> buffer(call.u32(2));
		ssize_t count;
		if (call[4]) {
			// An OVERLAPPED offset applies to synchronous handles too, and moves their file pointer after the transfer.
			const auto offset = call.load<uint64_t>(call[4] + 2 * call.pointer_width());
			count = ::pread(file->descriptor, buffer.data(), buffer.size(), off_t(offset));
			if (count >= 0 && !(file->flags & 0x40000000U))
				::lseek(file->descriptor, off_t(offset + uint64_t(count)), SEEK_SET);
		} else {
			do count = ::read(file->descriptor, buffer.data(), buffer.size()); while (count < 0 && errno == EINTR);
		}
		if (count < 0) {
			call.fail(error_of(errno));
			return;
		}
		call.write(call[1], std::span<const uint8_t>(buffer.data(), size_t(count)));
		if (std::getenv("RAVEN_TRACE_IO")) std::fprintf(stderr, "read %llx asked %x got %zd\n", static_cast<unsigned long long>(call[0]), call.u32(2), count);
		if (call[3]) call.store<uint32_t>(call[3], uint32_t(count));
		if (call[4] && (file->flags & 0x40000000U)) {
			complete_file_io(context, *file, call[4], call.pointer_width(), 0, uint64_t(count));
		} else if (call[4]) {
			call.store_pointer(call[4], 0);
			call.store_pointer(call[4] + call.pointer_width(), uint64_t(count));
		}
		call.ret(1);
	});
	registry.add("WriteFile", 5, [&, resolve, missing, file_of](ApiCall &call) {
		if (transfer_pipe(context, call, false))
			return;
		const auto file = file_of(call[0]);
		if (!file || file->directory || !file->writable()) {
			call.fail(file ? 5 : 6);
			return;
		}
		if (call[3]) call.store<uint32_t>(call[3], 0);
		else if (!call[4]) {
			call.fail(87);
			return;
		}
		const auto bytes = call.read(call[1], call.u32(2));
		ssize_t count;
		if (call[4]) {
			const auto offset = call.load<uint64_t>(call[4] + 2 * call.pointer_width());
			count = offset == UINT64_MAX ? ::write(file->descriptor, bytes.data(), bytes.size())
				: ::pwrite(file->descriptor, bytes.data(), bytes.size(), off_t(offset));
			if (count >= 0 && offset != UINT64_MAX && !(file->flags & 0x40000000U))
				::lseek(file->descriptor, off_t(offset + uint64_t(count)), SEEK_SET);
		} else if (::isatty(file->descriptor)) {
			count = TerminalConsole::instance().write_text(file->descriptor, std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size())) ?
				ssize_t(bytes.size()) : -1;
		} else {
			do count = ::write(file->descriptor, bytes.data(), bytes.size()); while (count < 0 && errno == EINTR);
		}
		if (count < 0) {
			call.fail(error_of(errno));
			return;
		}
		if (call[3]) call.store<uint32_t>(call[3], uint32_t(count));
		if (call[4] && (file->flags & 0x40000000U)) {
			complete_file_io(context, *file, call[4], call.pointer_width(), 0, uint64_t(count));
		} else if (call[4]) {
			call.store_pointer(call[4], 0);
			call.store_pointer(call[4] + call.pointer_width(), uint64_t(count));
		}
		call.ret(1);
	});
	// The completion forms: the transfer is done at once and the routine runs as an APC of the calling thread at its next alertable wait,
	// with the error, the byte count and the OVERLAPPED. A transfer that fails at once queues nothing.
	const auto transfer_ex = [&registry, &context](ApiCall &call, const char *name) {
		const auto transfer = registry.find(name);
		ApiCall inner(call.process, call.environment, {call[0], call[1], call[2], 0, call[3]});
		transfer->second(inner);
		if (!inner.result) {
			call.ret(0);
			return;
		}
		const auto bytes = call.load_pointer(call[3] + call.pointer_width());
		context.threads.current().apcs.push_back({call[4], 0, {0, bytes, call[3]}});
		call.set_last_error(0);
		call.ret(1);
	};
	registry.add("ReadFileEx", 5, [transfer_ex](ApiCall &call) { transfer_ex(call, "ReadFile"); });
	registry.add("WriteFileEx", 5, [transfer_ex](ApiCall &call) { transfer_ex(call, "WriteFile"); });
	// The native transfers: ReadFile or WriteFile does the work, with an OVERLAPPED that carries the offset when the caller names one,
	// and the outcome goes to the IO_STATUS_BLOCK as an NTSTATUS. An offset of -1 writes at the end of the file; -2 or none, at the file
	// pointer. A pipe answers with its own transfers, which may stay pending until the event the caller named is set.
	const auto native_transfer = [&registry, &context, file_of](ApiCall &call, bool reading) {
		if (native_pipe_transfer(context, call, reading))
			return;
		const auto write = registry.find(reading ? "ReadFile" : "WriteFile");
		if (!write || !call[4]) {
			call.ret(0xc000000dU);
			return;
		}
		const size_t width = call.pointer_width();
		uint64_t offset = UINT64_MAX;
		if (call[7]) {
			const auto requested = call.load<int64_t>(call[7]);
			if (requested == -1 && !reading) {
				const auto file = file_of(call[0]);
				offset = file ? uint64_t(::lseek(file->descriptor, 0, SEEK_END)) : UINT64_MAX;
			} else if (requested >= 0) {
				offset = uint64_t(requested);
			}
		}
		const uint64_t scratch = call.process.map_memory(4096, PROT_READ | PROT_WRITE);
		struct Unmap {
			NativeProcess &process;
			uint64_t address;
			~Unmap() { try { process.unmap_memory(address); } catch (...) {} }
		} unmap{call.process, scratch};
		const uint64_t written = scratch, overlapped = scratch + 64;
		call.store<uint32_t>(written, 0);
		// The OVERLAPPED: status and count words, then the offset, then the event, which stays empty.
		std::vector<uint8_t> block(width * 3 + 8);
		std::memcpy(block.data() + 2 * width, &offset, 8);
		call.write(overlapped, block);
		const bool positioned = offset != UINT64_MAX;
		ApiCall inner(call.process, call.environment, {call[0], call[5], call[6], positioned ? 0 : written, positioned ? overlapped : 0});
		const uint32_t before = call.last_error();
		write->second(inner);
		const uint64_t count = positioned ? call.load_pointer(overlapped + width) : call.load<uint32_t>(written);
		uint32_t status = 0;
		if (!inner.result) {
			switch (call.last_error()) {
			case 6: status = 0xc0000008U; break;
			case 5: status = 0xc0000022U; break;
			case 87: status = 0xc000000dU; break;
			case 112: status = 0xc000007fU; break;
			case 109: case 232: status = 0xc000014bU; break;
			default: status = 0xc0000001U; break;
			}
			call.set_last_error(before);
		}
		call.store_pointer(call[4], status);
		call.store_pointer(call[4] + width, inner.result ? count : 0);
		if (call[1] && inner.result)
			if (const auto set = registry.find("SetEvent")) {
				ApiCall signal(call.process, call.environment, {call[1]});
				set->second(signal);
			}
		call.ret(status);
	};
	registry.add("NtWriteFile", 9, [native_transfer](ApiCall &call) { native_transfer(call, false); });
	registry.add("NtReadFile", 9, [native_transfer](ApiCall &call) { native_transfer(call, true); });
	const auto query_directory = [&objects, file_of](ApiCall &call) {
		try {
			if (!call[4]) { call.ret(0xc000000dU); return; }
			const auto finish = [&](uint32_t status, size_t count = 0) {
				call.store_pointer(call[4], status);
				call.store_pointer(call[4] + call.pointer_width(), count);
				call.ret(status);
			};
			finish(0);
			const auto file = file_of(call[0]);
			if (!file) { finish(0xc0000008U); return; }
			if (!file->directory) { finish(0xc0000103U); return; }
			if (!(objects.access_of(call[0]) & (0x80000000U | 0x10000000U | 1U))) { finish(0xc0000022U); return; }
			if (call[1] || call[2] || call[3] || file->port) { finish(0xc00000bbU); return; }
			const auto information = call.u32(7), capacity = call.u32(6);
			const size_t header = information == 1 ? 64 : information == 2 ? 68 : information == 3 ? 94 : information == 12 ? 12 : 0;
			if (!header) { finish(0xc0000003U); return; }
			if (capacity < header) { finish(0xc0000023U); return; }
			if (!call[5]) { finish(0xc0000005U); return; }
			std::lock_guard lock(file->directory_mutex);
			const bool first = !file->directory_scan;
			if (first) {
				std::u16string pattern = u"*";
				if (call[9]) {
					const auto length = call.load<uint16_t>(call[9]);
					if ((length & 1) || length > call.load<uint16_t>(call[9] + 2)) { finish(0xc000000dU); return; }
					const auto bytes = call.read(call.load_pointer(call[9] + call.pointer_width()), length);
					pattern.resize(length / 2);
					if (length) std::memcpy(pattern.data(), bytes.data(), length);
					if (pattern.find_first_of(u"\\/:") != pattern.npos) { finish(0xc000000dU); return; }
				}
				const int descriptor = ::openat(file->descriptor, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
				if (descriptor < 0) { finish(errno == EACCES ? 0xc0000022U : 0xc0000001U); return; }
				DIR *stream = ::fdopendir(descriptor);
				if (!stream) { ::close(descriptor); finish(0xc000009aU); return; }
				struct CloseDirectory { DIR *stream; ~CloseDirectory() { ::closedir(stream); } } close{stream};
				auto scan = std::make_shared<FindObject>();
				for (;;) {
					errno = 0;
					const auto *item = ::readdir(stream);
					if (!item) {
						if (errno) { finish(0xc0000001U); return; }
						break;
					}
					const auto name = from_utf8(item->d_name);
					if (!wildcard(pattern, name)) continue;
					struct stat metadata{};
					if (::fstatat(file->descriptor, item->d_name, &metadata, 0)) continue;
					scan->entries.push_back({name, attributes_of(metadata, item->d_name), filetime(metadata.st_ctim),
						filetime(metadata.st_atim), filetime(metadata.st_mtim), S_ISDIR(metadata.st_mode) ? 0 : uint64_t(metadata.st_size),
						uint64_t(metadata.st_blocks) * 512});
				}
				file->directory_pattern = std::move(pattern);
				file->directory_scan = std::move(scan);
			}
			const auto &scan = file->directory_scan;
			// ReturnSingleEntry and RestartScan are BOOLEANs: only the low byte of their argument words is set by the caller.
			size_t next = uint8_t(call[10]) ? 0 : scan->next;
			if (next == scan->entries.size()) { finish(first ? 0xc000000fU : 0x80000006U); return; }
			std::vector<uint8_t> bytes;
			size_t previous = 0;
			uint32_t status = 0;
			while (next < scan->entries.size()) {
				const auto &entry = scan->entries[next];
				const size_t start = bytes.empty() ? 0 : (bytes.size() + 7) & ~size_t(7);
				const size_t size = header + entry.name.size() * 2;
				if (start + size > capacity && (!bytes.empty() || !first)) break;
				const size_t copied = std::min(size, capacity - start);
				bytes.resize(start + copied);
				if (start) put32(bytes, previous, uint32_t(start - previous));
				put32(bytes, start + (information == 12 ? 8 : 60), uint32_t(entry.name.size() * 2));
				if (information != 12) {
					put64(bytes, start + 8, entry.creation);
					put64(bytes, start + 16, entry.access);
					put64(bytes, start + 24, entry.write);
					put64(bytes, start + 32, entry.creation);
					put64(bytes, start + 40, entry.size);
					put64(bytes, start + 48, entry.allocation);
					put32(bytes, start + 56, entry.attributes);
				}
				std::memcpy(bytes.data() + start + header, entry.name.data(), copied - header);
				if (copied < size) { status = 0x80000005U; break; }
				++next;
				previous = start;
				if (uint8_t(call[8])) break;
			}
			call.write(call[5], bytes);
			finish(status, bytes.size());
			scan->next = next;
		} catch (const ApiFault &) { call.ret(0xc0000005U); }
	};
	registry.add("NtQueryDirectoryFile", 11, query_directory);
	registry.add("ZwQueryDirectoryFile", 11, query_directory);
	// No file is encrypted: decrypting one that exists has nothing to do, and encryption is not offered.
	const auto decrypt = [&, resolve](ApiCall &call, bool wide) {
		const auto name = wide ? call.wide(call[0]) : from_utf8(call.narrow(call[0]));
		const auto resolved = resolve(call, name);
		if (!resolved)
			return;
		if (!resolved->exists) {
			call.fail(2);
			return;
		}
		call.ret(1);
	};
	registry.add("DecryptFileW", 2, [decrypt](ApiCall &call) { decrypt(call, true); });
	registry.add("DecryptFileA", 2, [decrypt](ApiCall &call) { decrypt(call, false); });
	registry.add("EncryptFileW", 1, [](ApiCall &call) { call.fail(50); });
	registry.add("EncryptFileA", 1, [](ApiCall &call) { call.fail(50); });
	registry.add("SetFilePointerEx", 4, [&, resolve, missing, file_of](ApiCall &call) {
		const auto file = file_of(call[0]);
		if (!file) {
			call.fail(6);
			return;
		}
		const auto method = call.u32(3);
		if (method > 2) {
			call.fail(87);
			return;
		}
		const off_t position = ::lseek(file->descriptor, off_t(int64_t(call[1])), method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END);
		if (position < 0) {
			call.fail(errno == EINVAL ? 131 : error_of(errno));
			return;
		}
		if (call[2]) call.store<int64_t>(call[2], position);
		call.ret(1);
	});
	// The older form splits the distance in two words; the answer is the low word of the position, and 0xffffffff with a last error on failure.
	registry.add("SetFilePointer", 4, [&, resolve, missing, file_of](ApiCall &call) {
		const auto file = file_of(call[0]);
		if (!file) {
			call.fail(6, 0xffffffffU);
			return;
		}
		const auto method = call.u32(3);
		if (method > 2) {
			call.fail(87, 0xffffffffU);
			return;
		}
		const int64_t distance = call[2] ? int64_t(uint64_t(uint32_t(call.load<int32_t>(call[2]))) << 32 | call.u32(1)) : int64_t(call.i32(1));
		const off_t position = ::lseek(file->descriptor, off_t(distance), method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END);
		if (position < 0) {
			call.fail(errno == EINVAL ? 131 : error_of(errno), 0xffffffffU);
			return;
		}
		if (call[2]) call.store<int32_t>(call[2], int32_t(uint64_t(position) >> 32));
		call.set_last_error(0);
		call.ret(uint32_t(position));
	});
	// Byte-range locks map to open-file-description locks, so they conflict between handles the way Windows locks do.
	const auto lock_range = [file_of](ApiCall &call, int kind, uint64_t offset, uint64_t length, bool wait) {
		const auto file = file_of(call[0]);
		if (!file) {
			call.fail(6);
			return;
		}
		// Windows locks a range exclusively through a handle opened only for reading; the host takes a write lock only on a writable
		// description, so such a handle locks through one of its own, and where none can be opened an exclusive lock becomes a shared one.
		int target = file->descriptor;
		if (const int mode = ::fcntl(target, F_GETFL); mode >= 0 && (mode & O_ACCMODE) == O_RDONLY) {
			if (file->lock_descriptor < 0)
				file->lock_descriptor = ::open(("/proc/self/fd/" + std::to_string(file->descriptor)).c_str(), O_RDWR | O_CLOEXEC);
			if (file->lock_descriptor >= 0)
				target = file->lock_descriptor;
			else if (kind == F_WRLCK)
				kind = F_RDLCK;
		}
		struct flock request{};
		request.l_type = kind;
		request.l_whence = SEEK_SET;
		request.l_start = off_t(offset);
		// A range that reaches past what the host can count (programs lock "everything" with all bits set) runs to the end of the file
		// and beyond, which is what a zero length asks of the host.
		request.l_len = offset > uint64_t(INT64_MAX) || length > uint64_t(INT64_MAX) - offset ? 0 : off_t(length);
		if (offset > uint64_t(INT64_MAX)) {
			call.fail(87);
			return;
		}
		// An empty range locks nothing and conflicts with nothing.
		if (!length) {
			call.ret(1);
			return;
		}
		if (::fcntl(target, wait ? F_OFD_SETLKW : F_OFD_SETLK, &request) < 0) {
			call.fail(errno == EAGAIN || errno == EACCES ? 33 : errno == EINTR ? 995 : error_of(errno));
			return;
		}
		call.ret(1);
	};
	registry.add("LockFileEx", 6, [lock_range](ApiCall &call) {
		const uint64_t offset = call[5] ? call.load<uint64_t>(call[5] + 2 * call.pointer_width()) : 0;
		lock_range(call, (call.u32(1) & 2) ? F_WRLCK : F_RDLCK, offset, call.u32(3) | (uint64_t(call.u32(4)) << 32), !(call.u32(1) & 1));
	});
	registry.add("UnlockFileEx", 5, [lock_range](ApiCall &call) {
		const uint64_t offset = call[4] ? call.load<uint64_t>(call[4] + 2 * call.pointer_width()) : 0;
		lock_range(call, F_UNLCK, offset, call.u32(2) | (uint64_t(call.u32(3)) << 32), false);
	});
	registry.add("LockFile", 5, [lock_range](ApiCall &call) {
		lock_range(call, F_WRLCK, call.u32(1) | (uint64_t(call.u32(2)) << 32), call.u32(3) | (uint64_t(call.u32(4)) << 32), false);
	});
	registry.add("UnlockFile", 5, [lock_range](ApiCall &call) {
		lock_range(call, F_UNLCK, call.u32(1) | (uint64_t(call.u32(2)) << 32), call.u32(3) | (uint64_t(call.u32(4)) << 32), false);
	});
	registry.add("GetFileSizeEx", 2, [&, resolve, missing, file_of](ApiCall &call) {
		const auto file = file_of(call[0]);
		struct stat metadata{};
		if (!file || ::fstat(file->descriptor, &metadata)) {
			call.fail(6);
			return;
		}
		call.store<int64_t>(call[1], metadata.st_size);
		call.ret(1);
	});
	registry.add("GetFileSize", 2, [&, resolve, missing, file_of](ApiCall &call) {
		const auto file = file_of(call[0]);
		struct stat metadata{};
		if (!file || ::fstat(file->descriptor, &metadata)) {
			call.fail(6, 0xffffffffU);
			return;
		}
		if (call[1]) call.store<uint32_t>(call[1], uint32_t(uint64_t(metadata.st_size) >> 32));
		call.set_last_error(0);
		call.ret(uint32_t(metadata.st_size));
	});
	registry.add("GetFileType", 1, [&, resolve, missing, file_of](ApiCall &call) {
		const auto file = file_of(call[0]);
		if (!file) {
			call.fail(6);
			return;
		}
		call.ret(objects.object_as<PipeObject>(call[0]) ? 3 : ::isatty(file->descriptor) ? 2 : 1);
	});
	registry.add("SetEndOfFile", 1, [&, resolve, missing, file_of](ApiCall &call) {
		const auto file = file_of(call[0]);
		if (!file || !file->writable()) {
			call.fail(file ? 5 : 6);
			return;
		}
		const off_t position = ::lseek(file->descriptor, 0, SEEK_CUR);
		if (position < 0 || ::ftruncate(file->descriptor, position)) {
			call.fail(error_of(errno));
			return;
		}
		call.ret(1);
	});
	registry.add("FlushFileBuffers", 1, [&, resolve, missing, file_of](ApiCall &call) {
		const auto file = file_of(call[0]);
		if (!file || !file->writable()) {
			call.fail(file ? 5 : 6);
			return;
		}
		::fsync(file->descriptor);
		call.ret(1);
	});
	// Reparse points are the host's symbolic links. FSCTL_SET_REPARSE_POINT makes an empty directory a mount point (a junction) or
	// a file or directory a symbolic link to the target its REPARSE_DATA_BUFFER names; FSCTL_GET_REPARSE_POINT describes a link,
	// as a mount point when it holds an absolute path to a directory (what a junction is) and as a symbolic link otherwise; and
	// FSCTL_DELETE_REPARSE_POINT removes the link, leaving an empty directory or file. Other control codes are not supported by
	// the host file system, as from a volume whose driver lacks them, and nothing comes back.
	constexpr uint32_t fsctl_set_reparse_point = 0x900a4, fsctl_get_reparse_point = 0x900a8, fsctl_delete_reparse_point = 0x900ac;
	constexpr uint32_t tag_mount_point = 0xa0000003U, tag_symbolic_link = 0xa000000cU;
	registry.add("DeviceIoControl", 8, [&, file_of](ApiCall &call) {
		const uint32_t code = call.u32(1);
		if (call[6])
			call.store<uint32_t>(call[6], 0);
		const auto file = file_of(call[0]);
		if (code != fsctl_set_reparse_point && code != fsctl_get_reparse_point && code != fsctl_delete_reparse_point) {
			call.fail(1);
			return;
		}
		if (!file) {
			call.fail(6);
			return;
		}
		struct stat own{};
		if (::lstat(file->path.c_str(), &own)) {
			call.fail(error_of(errno));
			return;
		}
		const bool link = S_ISLNK(own.st_mode);
		if (code == fsctl_get_reparse_point) {
			if (!link) {
				call.fail(4390);
				return;
			}
			std::error_code error;
			const auto target = std::filesystem::read_symlink(file->path, error);
			if (error) {
				call.fail(error_of(error.value()));
				return;
			}
			const bool absolute = target.is_absolute();
			const bool mount_point = absolute && std::filesystem::is_directory(file->path, error);
			const auto print = absolute ? paths.dos(target) : [&] {
				auto text = from_utf8(target.string());
				for (auto &character : text)
					if (character == u'/')
						character = u'\\';
				return text;
			}();
			const auto substitute = absolute ? u"\\??\\" + print : print;
			// The header, the offsets and lengths of the two names, the flags of a symbolic link, then the names one after the other.
			const size_t names_at = mount_point ? 16 : 20;
			std::vector<uint8_t> bytes(names_at + (substitute.size() + print.size() + 2) * 2, 0);
			put32(bytes, 0, mount_point ? tag_mount_point : tag_symbolic_link);
			const auto put16 = [&](size_t at, size_t value) { const uint16_t word = uint16_t(value); std::memcpy(bytes.data() + at, &word, 2); };
			put16(4, bytes.size() - 8);
			put16(8, 0);
			put16(10, substitute.size() * 2);
			put16(12, (substitute.size() + 1) * 2);
			put16(14, print.size() * 2);
			if (!mount_point)
				put32(bytes, 16, absolute ? 0 : 1);
			std::memcpy(bytes.data() + names_at, substitute.data(), substitute.size() * 2);
			std::memcpy(bytes.data() + names_at + (substitute.size() + 1) * 2, print.data(), print.size() * 2);
			if (call.u32(5) < bytes.size()) {
				if (call.u32(5) >= 8 && call[4])
					call.write(call[4], std::span<const uint8_t>(bytes.data(), 8));
				call.fail(call.u32(5) >= 8 ? 234 : 122);
				return;
			}
			call.write(call[4], bytes);
			if (call[6])
				call.store<uint32_t>(call[6], uint32_t(bytes.size()));
			call.ret(1);
			return;
		}
		if (code == fsctl_delete_reparse_point) {
			if (!link) {
				call.fail(4390);
				return;
			}
			std::error_code error;
			const bool directory = std::filesystem::is_directory(file->path, error);
			if (::unlink(file->path.c_str()) || (directory ? ::mkdir(file->path.c_str(), 0777) : ::close(::open(file->path.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0666)))) {
				call.fail(error_of(errno));
				return;
			}
			call.ret(1);
			return;
		}
		// FSCTL_SET_REPARSE_POINT: the tag, the data length, then the names' offsets and lengths (and a symbolic link's flags).
		if (!call[2] || call.u32(3) < 16) {
			call.fail(4392);
			return;
		}
		const auto header = call.read(call[2], 20);
		uint32_t tag;
		std::memcpy(&tag, header.data(), 4);
		if (tag != tag_mount_point && tag != tag_symbolic_link) {
			call.fail(4394);
			return;
		}
		const auto word = [&](size_t at) { uint16_t value; std::memcpy(&value, header.data() + at, 2); return size_t(value); };
		const size_t names_at = tag == tag_mount_point ? 16 : 20;
		uint32_t flags = 0;
		if (tag == tag_symbolic_link)
			std::memcpy(&flags, header.data() + 16, 4);
		if (word(10) == 0 || names_at + word(8) + word(10) > call.u32(3)) {
			call.fail(4392);
			return;
		}
		const auto bytes = call.read(call[2] + names_at + word(8), word(10));
		std::u16string substitute(bytes.size() / 2, u'\0');
		std::memcpy(substitute.data(), bytes.data(), substitute.size() * 2);
		const bool relative = tag == tag_symbolic_link && (flags & 1);
		if (!relative && substitute.starts_with(u"\\??\\"))
			substitute = substitute.substr(4);
		std::filesystem::path content;
		if (relative) {
			auto text = substitute;
			for (auto &character : text)
				if (character == u'\\')
					character = u'/';
			content = to_utf8(text);
		} else {
			const auto target = paths.resolve(substitute);
			if (!target) {
				call.fail(123);
				return;
			}
			content = target->host;
		}
		if (link) {
			call.fail(4391);
			return;
		}
		// A mount point goes on an empty directory, a symbolic link on any file or directory that is empty.
		std::error_code error;
		const bool directory = S_ISDIR(own.st_mode);
		if (tag == tag_mount_point && !directory) {
			call.fail(267);
			return;
		}
		if (directory ? !std::filesystem::is_empty(file->path, error) : own.st_size != 0) {
			call.fail(145);
			return;
		}
		if ((directory ? ::rmdir(file->path.c_str()) : ::unlink(file->path.c_str())) || ::symlink(content.c_str(), file->path.c_str())) {
			call.fail(error_of(errno));
			return;
		}
		call.ret(1);
	});
	registry.add("GetFileTime", 4, [&, resolve, missing, file_of](ApiCall &call) {
		const auto file = file_of(call[0]);
		struct stat metadata{};
		if (!file || ::fstat(file->descriptor, &metadata)) {
			call.fail(6);
			return;
		}
		if (call[1]) call.store<uint64_t>(call[1], filetime(metadata.st_ctim));
		if (call[2]) call.store<uint64_t>(call[2], filetime(metadata.st_atim));
		if (call[3]) call.store<uint64_t>(call[3], filetime(metadata.st_mtim));
		call.ret(1);
	});
	registry.add("SetFileTime", 4, [&, resolve, missing, file_of](ApiCall &call) {
		const auto file = file_of(call[0]);
		if (!file || !file->writable()) {
			call.fail(file ? 5 : 6);
			return;
		}
		timespec times[2]{{0, UTIME_OMIT}, {0, UTIME_OMIT}};
		if (call[2]) if (const auto value = call.load<uint64_t>(call[2])) times[0] = timespec_from(value);
		if (call[3]) if (const auto value = call.load<uint64_t>(call[3])) times[1] = timespec_from(value);
		if (::futimens(file->descriptor, times)) {
			call.fail(error_of(errno));
			return;
		}
		call.ret(1);
	});
	registry.add("GetFileInformationByHandle", 2, [&, resolve, missing, file_of](ApiCall &call) {
		const auto file = file_of(call[0]);
		struct stat metadata{};
		if (!file || ::fstat(file->descriptor, &metadata)) {
			call.fail(6);
			return;
		}
		std::vector<uint8_t> bytes(52);
		put32(bytes, 0, attributes_of(metadata, file->path));
		put64(bytes, 4, filetime(metadata.st_ctim));
		put64(bytes, 12, filetime(metadata.st_atim));
		put64(bytes, 20, filetime(metadata.st_mtim));
		put32(bytes, 28, uint32_t(metadata.st_dev));
		put32(bytes, 32, uint32_t(uint64_t(metadata.st_size) >> 32));
		put32(bytes, 36, uint32_t(metadata.st_size));
		put32(bytes, 40, uint32_t(metadata.st_nlink));
		put32(bytes, 44, uint32_t(uint64_t(metadata.st_ino) >> 32));
		put32(bytes, 48, uint32_t(metadata.st_ino));
		call.write(call[1], bytes);
		call.ret(1);
	});
	// The host file systems keep no 8.3 names: a valid request on an open file is accepted and leaves nothing to record, as a volume with
	// short name generation turned off does.
	registry.add("SetFileShortNameW", 2, [file_of](ApiCall &call) {
		if (!file_of(call[0])) {
			call.fail(6);
			return;
		}
		if (!call[1] || call.wide(call[1], 13).size() > 12) {
			call.fail(87);
			return;
		}
		call.ret(1);
	});
	// The information classes programs set on an open file: times, renaming, deletion on close, the file's length and I/O hints.
	registry.add("SetFileInformationByHandle", 4, [&, resolve, missing, file_of](ApiCall &call) {
		const auto file = file_of(call[0]);
		if (!file || !call[2]) {
			call.fail(file ? 87 : 6);
			return;
		}
		const uint32_t size = call.u32(3);
		const size_t width = call.pointer_width();
		switch (call.u32(1)) {
		case 0: {
			if (size < 36) {
				call.fail(24);
				return;
			}
			// Zero leaves a time as it is; the change time is the host's to keep.
			const auto bytes = call.read(call[2], 36);
			const auto time_at = [&](size_t at) { int64_t value; std::memcpy(&value, bytes.data() + at, 8); return value; };
			timespec times[2] = {{0, UTIME_OMIT}, {0, UTIME_OMIT}};
			const auto convert = [](int64_t value, timespec &out) {
				if (value <= 0)
					return;
				const uint64_t unix = uint64_t(value) - filetime_epoch_offset;
				out = {time_t(unix / 10000000ULL), long(unix % 10000000ULL) * 100};
			};
			convert(time_at(8), times[0]);
			convert(time_at(16), times[1]);
			if ((times[0].tv_nsec != UTIME_OMIT || times[1].tv_nsec != UTIME_OMIT) && ::futimens(file->descriptor, times)) {
				call.fail(error_of(errno));
				return;
			}
			call.ret(1);
			return;
		}
		case 3: case 22: {
			// FILE_RENAME_INFO: the replace flag (a byte, or flags for the Ex class), a root directory handle and the counted new name.
			const size_t header = width == 4 ? 12 : 20;
			if (size < header) {
				call.fail(24);
				return;
			}
			const uint32_t flags = call.u32(1) == 22 ? call.load<uint32_t>(call[2]) : call.load<uint8_t>(call[2]) ? 1U : 0U;
			const uint32_t length = call.load<uint32_t>(call[2] + (width == 4 ? 8 : 16));
			if (call.load_pointer(call[2] + width) || length % 2 || header + length > size) {
				call.fail(87);
				return;
			}
			const auto bytes = call.read(call[2] + header, length);
			std::u16string name(length / 2, u'\0');
			std::memcpy(name.data(), bytes.data(), length);
			const auto target = resolve(call, name);
			if (!target)
				return;
			if (target->exists && !(flags & 1)) {
				call.fail(183);
				return;
			}
			if (::rename(file->path.c_str(), target->host.c_str())) {
				call.fail(error_of(errno, !target->parent_exists));
				return;
			}
			file->path = target->host;
			file->dos_path = paths.dos(target->host);
			call.ret(1);
			return;
		}
		case 4: case 21: {
			// Deletion when the last handle closes; POSIX semantics take the name away at once.
			if (size < (call.u32(1) == 21 ? 4U : 1U)) {
				call.fail(24);
				return;
			}
			const uint32_t flags = call.u32(1) == 21 ? call.load<uint32_t>(call[2]) : call.load<uint8_t>(call[2]) ? 1U : 0U;
			if ((flags & 1) && file->directory && !std::filesystem::is_empty(file->path)) {
				call.fail(145);
				return;
			}
			if ((flags & 3) == 3) {
				if ((file->directory ? ::rmdir(file->path.c_str()) : ::unlink(file->path.c_str())) && errno != ENOENT) {
					call.fail(error_of(errno));
					return;
				}
				file->flags &= ~0x04000000U;
			} else if (flags & 1) {
				file->flags |= 0x04000000U;
			} else {
				file->flags &= ~0x04000000U;
			}
			call.ret(1);
			return;
		}
		case 5: case 6: {
			if (size < 8) {
				call.fail(24);
				return;
			}
			const auto requested = call.load<int64_t>(call[2]);
			struct stat metadata{};
			if (requested < 0 || !file->writable() || ::fstat(file->descriptor, &metadata)) {
				call.fail(requested < 0 ? 87 : 5);
				return;
			}
			// An allocation size only shortens a file that is longer; the end of file sets the length either way.
			if ((call.u32(1) == 6 || requested < metadata.st_size) && ::ftruncate(file->descriptor, off_t(requested))) {
				call.fail(error_of(errno));
				return;
			}
			call.ret(1);
			return;
		}
		case 12:
			call.ret(1);
			return;
		default:
			call.fail(87);
			return;
		}
	});
	// The volume keeps no code integrity signing levels with its files, as one without extended attributes: a program that caches a level
	// it checked is told the file system cannot, and checks the signature again next time.
	const auto signing_level = [file_of](ApiCall &call) {
		call.fail(file_of(call[0]) ? 50 : 6);
	};
	registry.add("GetCachedSigningLevel", 6, signing_level);
	registry.add("SetCachedSigningLevel", 4, [file_of](ApiCall &call) {
		call.fail(file_of(call[3]) ? 50 : 6);
	});
	registry.add("GetFileInformationByHandleEx", 4, [&, resolve, missing, file_of](ApiCall &call) {
		const auto file = file_of(call[0]);
		struct stat metadata{};
		if (!file || ::fstat(file->descriptor, &metadata)) {
			call.fail(6);
			return;
		}
		std::vector<uint8_t> bytes;
		switch (call.u32(1)) {
		case 0:
			bytes.assign(40, 0);
			put64(bytes, 0, filetime(metadata.st_ctim));
			put64(bytes, 8, filetime(metadata.st_atim));
			put64(bytes, 16, filetime(metadata.st_mtim));
			put64(bytes, 24, filetime(metadata.st_ctim));
			put32(bytes, 32, attributes_of(metadata, file->path));
			break;
		case 1:
			bytes.assign(24, 0);
			put64(bytes, 0, uint64_t(metadata.st_blocks) * 512);
			put64(bytes, 8, uint64_t(metadata.st_size));
			put32(bytes, 16, uint32_t(metadata.st_nlink));
			bytes[20] = (file->flags & 0x04000000U) ? 1 : 0;
			bytes[21] = S_ISDIR(metadata.st_mode) ? 1 : 0;
			break;
		case 2: {
			// The name without its drive, as the volume knows it.
			const auto name = file->dos_path.size() >= 2 && file->dos_path[1] == u':' ? file->dos_path.substr(2) : file->dos_path;
			bytes.assign(4 + name.size() * 2, 0);
			put32(bytes, 0, uint32_t(name.size() * 2));
			std::memcpy(bytes.data() + 4, name.data(), name.size() * 2);
			if (bytes.size() > call.u32(3)) {
				if (call.u32(3) >= 4)
					call.write(call[2], std::span<const uint8_t>(bytes.data(), call.u32(3)));
				call.fail(234);
				return;
			}
			break;
		}
		case 9:
			bytes.assign(8, 0);
			put32(bytes, 0, attributes_of(metadata, file->path));
			break;
		case 18:
			bytes.assign(24, 0);
			put64(bytes, 0, uint64_t(metadata.st_dev));
			put64(bytes, 8, uint64_t(metadata.st_ino));
			break;
		default:
			call.fail(87);
			return;
		}
		if (bytes.size() > call.u32(3)) {
			call.fail(24);
			return;
		}
		call.write(call[2], bytes);
		call.ret(1);
	});
	registry.add("GetFinalPathNameByHandleW", 4, [&, resolve, missing, file_of](ApiCall &call) {
		const auto file = file_of(call[0]);
		if (!file) {
			call.fail(6);
			return;
		}
		const auto text = u"\\\\?\\" + file->dos_path;
		const size_t capacity = call.u32(2);
		const size_t length = call.put_wide(call[1], text, capacity);
		call.ret(capacity > length ? length : length + 1);
	});
	registry.add("GetFileAttributesW", 1, [&, resolve, missing, file_of](ApiCall &call) {
		call.ret(UINT32_MAX);
		const auto name = call.wide(call[0]);
		const auto resolved = resolve(call, name);
		struct stat metadata{};
		if (!resolved)
			return;
		if (NativePaths::device(name)) {
			call.ret(attribute_archive);
			return;
		}
		struct stat own{};
		if (!resolved->exists || ::lstat(resolved->host.c_str(), &own)) {
			call.set_last_error(missing(*resolved));
			return;
		}
		const bool link = S_ISLNK(own.st_mode);
		if (!link || ::stat(resolved->host.c_str(), &metadata))
			metadata = own;
		call.set_last_error(0);
		call.ret(attributes_of(metadata, resolved->host) | (link ? attribute_reparse : 0));
	});
	registry.add("GetFileAttributesExW", 3, [&, resolve, missing, file_of](ApiCall &call) {
		const auto name = call.wide(call[0]);
		if (call.u32(1) != 0) {
			call.fail(87);
			return;
		}
		const auto resolved = resolve(call, name);
		if (!resolved)
			return;
		if (!resolved->exists) {
			call.set_last_error(missing(*resolved));
			return;
		}
		call.write(call[2], attribute_data(entry_of(resolved->host, u"")));
		call.ret(1);
	});
	registry.add("SetFileAttributesW", 2, [&, resolve, missing, file_of](ApiCall &call) {
		const auto resolved = resolve(call, call.wide(call[0]));
		if (!resolved)
			return;
		struct stat metadata{};
		if (!resolved->exists || ::stat(resolved->host.c_str(), &metadata)) {
			call.set_last_error(missing(*resolved));
			return;
		}
		// The read-only flag of a folder does not stop files being made inside it: the shell sets it on the folders it customizes, so a
		// folder keeps its permissions.
		if (S_ISDIR(metadata.st_mode)) {
			call.ret(1);
			return;
		}
		mode_t mode = metadata.st_mode & 07777;
		mode = call.u32(1) & attribute_readonly ? mode & ~0222 : mode | 0200;
		if (::chmod(resolved->host.c_str(), mode)) {
			call.fail(error_of(errno));
			return;
		}
		call.ret(1);
	});
	registry.add("CreateDirectoryW", 2, [&, resolve, missing, file_of](ApiCall &call) {
		const auto resolved = resolve(call, call.wide(call[0]));
		if (!resolved)
			return;
		if (resolved->exists) {
			call.set_last_error(183);
			return;
		}
		if (::mkdir(resolved->host.c_str(), 0777)) {
			call.fail(error_of(errno, !resolved->parent_exists));
			return;
		}
		call.ret(1);
	});
	registry.add("RemoveDirectoryW", 1, [&, resolve, missing, file_of](ApiCall &call) {
		const auto resolved = resolve(call, call.wide(call[0]));
		if (!resolved)
			return;
		if (::rmdir(resolved->host.c_str())) {
			call.fail(error_of(errno, !resolved->parent_exists));
			return;
		}
		call.ret(1);
	});
	registry.add("DeleteFileW", 1, [&, resolve, missing, file_of](ApiCall &call) {
		const auto resolved = resolve(call, call.wide(call[0]));
		if (!resolved)
			return;
		if (::unlink(resolved->host.c_str())) {
			call.fail(errno == EISDIR ? 5 : error_of(errno, !resolved->parent_exists));
			return;
		}
		call.ret(1);
	});
	// A symbolic link becomes a host one. A relative target stays relative to the link's directory, so the tree it lives in can move;
	// the target is spelled as the host names it, since the host lookup through the link does not ignore case.
	registry.add("CreateSymbolicLinkW", 3, [&, resolve](ApiCall &call) {
		const auto link_name = paths.full(call.wide(call[0]));
		const auto target_name = call.wide(call[1]);
		const auto link = link_name.empty() ? std::nullopt : resolve(call, link_name);
		if (!link)
			return;
		if (target_name.empty()) {
			call.fail(87);
			return;
		}
		const bool relative = !(target_name.size() > 1 && target_name[1] == u':') && target_name[0] != u'\\';
		const auto separator = link_name.find_last_of(u'\\');
		const auto target = resolve(call, relative ? paths.full(link_name.substr(0, separator + 1) + target_name) : target_name);
		if (!target)
			return;
		if (link->exists) {
			call.set_last_error(183);
			return;
		}
		const auto content = relative ? target->host.lexically_relative(link->host.parent_path()) : target->host;
		if (::symlink(content.c_str(), link->host.c_str())) {
			call.fail(error_of(errno, !link->parent_exists));
			return;
		}
		call.ret(1);
	});
	registry.add("CreateHardLinkW", 3, [&, resolve, missing](ApiCall &call) {
		const auto link = resolve(call, call.wide(call[0]));
		const auto existing = link ? resolve(call, call.wide(call[1])) : std::nullopt;
		if (!link || !existing)
			return;
		if (!existing->exists) {
			call.set_last_error(missing(*existing));
			return;
		}
		if (link->exists) {
			call.set_last_error(183);
			return;
		}
		if (::link(existing->host.c_str(), link->host.c_str())) {
			call.fail(error_of(errno, !link->parent_exists));
			return;
		}
		call.ret(1);
	});
	const auto copy_file = [&, missing](ApiCall &call, const NativePaths::Resolved &from, const NativePaths::Resolved &to, bool fail_if_exists) {
		if (!from.exists) {
			call.set_last_error(missing(from));
			return;
		}
		if (to.exists && fail_if_exists) {
			call.set_last_error(80);
			return;
		}
		const int source = ::open(from.host.c_str(), O_RDONLY | O_CLOEXEC);
		if (source < 0) {
			call.fail(error_of(errno));
			return;
		}
		struct stat metadata{};
		::fstat(source, &metadata);
		const int target = ::open(to.host.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, metadata.st_mode & 0777);
		if (target < 0) {
			const int error = errno;
			::close(source);
			call.fail(error_of(error, !to.parent_exists));
			return;
		}
		std::vector<char> buffer(1 << 16);
		bool good = true;
		for (;;) {
			const ssize_t count = ::read(source, buffer.data(), buffer.size());
			if (count <= 0) {
				good = count == 0;
				break;
			}
			if (::write(target, buffer.data(), size_t(count)) != count) {
				good = false;
				break;
			}
		}
		if (good) {
			const timespec times[]{metadata.st_atim, metadata.st_mtim};
			good = ::futimens(target, times) == 0;
		}
		const int error = errno;
		::close(source);
		::close(target);
		if (!good) {
			call.fail(error_of(error));
			return;
		}
		call.ret(1);
	};
	registry.add("CopyFileW", 3, [&, resolve, missing, file_of, copy_file](ApiCall &call) {
		const auto from = resolve(call, call.wide(call[0]));
		const auto to = from ? resolve(call, call.wide(call[1])) : std::nullopt;
		if (from && to)
			copy_file(call, *from, *to, call.u32(2));
	});
	registry.add("CopyFileExW", 6, [&, resolve, missing, file_of, copy_file](ApiCall &call) {
		const auto from = resolve(call, call.wide(call[0]));
		const auto to = from ? resolve(call, call.wide(call[1])) : std::nullopt;
		if (from && to)
			copy_file(call, *from, *to, call.u32(5) & 1);
	});
	registry.add("MoveFileExW", 3, [&, resolve, missing, file_of, copy_file](ApiCall &call) {
		const auto flags = call.u32(2);
		// MOVEFILE_DELAY_UNTIL_REBOOT: the move, or the deletion when no target is named, waits for the next start of Windows, which
		// `raven --reboot` stands for.
		if (flags & 4) {
			const auto from_name = paths.full(call.wide(call[0]));
			const auto to_name = call[1] ? paths.full(call.wide(call[1])) : std::u16string();
			if (from_name.empty() || (call[1] && to_name.empty())) {
				call.fail(123);
				return;
			}
			if (!queue_pending_rename(NativePrefix::from_environment(), from_name, to_name, flags & 1)) {
				call.fail(5);
				return;
			}
			call.ret(1);
			return;
		}
		const auto from = resolve(call, call.wide(call[0]));
		if (!from)
			return;
		if (!call[1]) {
			call.fail(87);
			return;
		}
		const auto to = resolve(call, call.wide(call[1]));
		if (!to)
			return;
		if (!from->exists) {
			call.set_last_error(missing(*from));
			return;
		}
		if (to->exists && !(flags & 1)) {
			call.set_last_error(183);
			return;
		}
		if (::rename(from->host.c_str(), to->host.c_str())) {
			if (errno == EXDEV && (flags & 2)) {
				copy_file(call, *from, *to, false);
				if (call.result)
					::unlink(from->host.c_str());
				return;
			}
			call.fail(error_of(errno, !to->parent_exists));
			return;
		}
		call.ret(1);
	});
	registry.add("MoveFileW", 2, [&, resolve, missing, file_of, copy_file](ApiCall &call) {
		const auto from = resolve(call, call.wide(call[0]));
		const auto to = from ? resolve(call, call.wide(call[1])) : std::nullopt;
		if (!from || !to)
			return;
		if (!from->exists) {
			call.set_last_error(missing(*from));
			return;
		}
		if (to->exists) {
			call.set_last_error(183);
			return;
		}
		if (::rename(from->host.c_str(), to->host.c_str())) {
			call.fail(error_of(errno, !to->parent_exists));
			return;
		}
		call.ret(1);
	});
	registry.add("ReplaceFileW", 6, [&, resolve, missing, file_of, copy_file](ApiCall &call) {
		const auto replaced = resolve(call, call.wide(call[0]));
		const auto replacement = resolve(call, call.wide(call[1]));
		if (!replaced || !replacement)
			return;
		if (!replaced->exists) {
			call.set_last_error(1176);
			return;
		}
		if (!replacement->exists) {
			call.set_last_error(1177);
			return;
		}
		if (call[2]) {
			const auto backup = resolve(call, call.wide(call[2]));
			if (!backup)
				return;
			copy_file(call, *replaced, *backup, false);
			if (!call.result) {
				call.set_last_error(1175);
				return;
			}
			call.result = 0;
		}
		if (::rename(replacement->host.c_str(), replaced->host.c_str())) {
			call.fail(1175);
			return;
		}
		call.ret(1);
	});
	registry.add("GetCurrentDirectoryW", 2, [&, resolve, missing, file_of, copy_file](ApiCall &call) {
		const auto &text = paths.current();
		const size_t capacity = call.u32(0);
		const size_t length = text.size() > 3 ? text.size() : 3;
		call.put_wide(call[1], text.size() == 2 ? text + u"\\" : text, capacity);
		call.ret(capacity > length ? length : length + 1);
	});
	registry.add("SetCurrentDirectoryW", 1, [&, resolve, missing, file_of, copy_file](ApiCall &call) {
		if (!paths.set_current(call.wide(call[0]))) {
			call.fail(2);
			return;
		}
		call.ret(1);
	});
	registry.add("GetFullPathNameW", 4, [&, resolve, missing, file_of, copy_file](ApiCall &call) {
		const auto name = call.wide(call[0]);
		if (name.empty()) {
			call.fail(87);
			return;
		}
		const auto text = paths.full(name);
		if (text.empty()) {
			call.fail(123);
			return;
		}
		const size_t capacity = call.u32(1);
		if (capacity <= text.size()) {
			call.ret(text.size() + 1);
			return;
		}
		call.put_wide(call[2], text, capacity);
		if (call[3]) {
			const auto slash = text.find_last_of(u'\\');
			call.store_pointer(call[3], slash == text.npos || slash + 1 == text.size() ? 0 : call[2] + (slash + 1) * 2);
		}
		call.ret(text.size());
	});
	registry.add("GetLongPathNameW", 3, [&, resolve, missing, file_of, copy_file](ApiCall &call) {
		const auto name = call.wide(call[0]);
		const auto resolved = resolve(call, name);
		if (!resolved)
			return;
		if (!resolved->exists) {
			call.set_last_error(missing(*resolved));
			return;
		}
		const auto text = paths.dos(resolved->host);
		const size_t capacity = call.u32(2);
		call.put_wide(call[1], text, capacity);
		call.ret(capacity > text.size() ? text.size() : text.size() + 1);
	});
	// GetTempPath2W differs only for SYSTEM processes, which get a folder of their own; a program never runs as one here.
	const auto temporary_path = [&](ApiCall &call) {
		auto text = paths.temporary();
		const size_t capacity = call.u32(0);
		call.put_wide(call[1], text, capacity);
		call.ret(capacity > text.size() ? text.size() : text.size() + 1);
	};
	registry.add("GetTempPathW", 2, temporary_path);
	registry.add("GetTempPath2W", 2, temporary_path);
	// The classic form reports clusters of eight 512-byte sectors, each count clamped to what a 32-bit field holds.
	registry.add("GetDiskFreeSpaceW", 5, [&, resolve, missing, file_of, copy_file](ApiCall &call) {
		const auto resolved = call[0] ? resolve(call, call.wide(call[0])) : paths.resolve(paths.current());
		if (!resolved)
			return;
		struct statvfs info{};
		auto path = resolved->host;
		while (!std::filesystem::exists(path) && path.has_parent_path() && path != path.parent_path())
			path = path.parent_path();
		if (::statvfs(path.c_str(), &info)) {
			call.fail(error_of(errno));
			return;
		}
		constexpr uint64_t cluster = 8 * 512;
		const auto clamp = [](uint64_t value) { return uint32_t(std::min<uint64_t>(value, UINT32_MAX)); };
		if (call[1]) call.store<uint32_t>(call[1], 8);
		if (call[2]) call.store<uint32_t>(call[2], 512);
		if (call[3]) call.store<uint32_t>(call[3], clamp(uint64_t(info.f_bavail) * info.f_frsize / cluster));
		if (call[4]) call.store<uint32_t>(call[4], clamp(uint64_t(info.f_blocks) * info.f_frsize / cluster));
		call.ret(1);
	});
	// The volume of an open file, as GetVolumeInformationW and GetDiskFreeSpaceW describe it: one NTFS disk volume without a label.
	registry.add("NtQueryVolumeInformationFile", 5, [file_of](ApiCall &call) {
		constexpr uint32_t volume_class = 1, size_class = 3, device_class = 4, attribute_class = 5, full_size_class = 7;
		constexpr uint32_t invalid_handle = 0xc0000008U, length_mismatch = 0xc0000004U, buffer_overflow = 0x80000005U, invalid_class = 0xc0000003U;
		const size_t width = call.pointer_width();
		const auto finish = [&](uint32_t status, uint64_t information) {
			call.store_pointer(call[1], status);
			call.store_pointer(call[1] + width, information);
			call.ret(status);
		};
		const auto file = file_of(call[0]);
		if (!file) {
			call.ret(invalid_handle);
			return;
		}
		const uint32_t capacity = call.u32(3), kind = call.u32(4);
		struct statvfs info{};
		if ((kind == size_class || kind == full_size_class) && ::fstatvfs(file->descriptor, &info)) {
			finish(0xc0000001U, 0);
			return;
		}
		constexpr uint64_t cluster = 8 * 512;
		const uint64_t total = uint64_t(info.f_blocks) * info.f_frsize / cluster, available = uint64_t(info.f_bavail) * info.f_frsize / cluster;
		std::vector<uint8_t> bytes;
		size_t fixed = 0;
		const auto put = [&bytes](size_t at, const auto value) {
			if (bytes.size() < at + sizeof(value)) bytes.resize(at + sizeof(value));
			std::memcpy(bytes.data() + at, &value, sizeof(value));
		};
		const auto put_name = [&bytes](size_t at, std::u16string_view name) {
			bytes.resize(at + name.size() * 2);
			std::memcpy(bytes.data() + at, name.data(), name.size() * 2);
		};
		switch (kind) {
		case volume_class:
			// Creation time, serial number, label length, object support, then the label.
			fixed = 18;
			put(0, uint64_t(0));
			put(8, uint32_t(0x52415645));
			put(12, uint32_t(0));
			put(16, uint8_t(0));
			bytes.resize(fixed);
			break;
		case size_class:
			fixed = 24;
			put(0, total);
			put(8, available);
			put(16, uint32_t(8));
			put(20, uint32_t(512));
			break;
		case device_class:
			// FILE_DEVICE_DISK, no characteristics.
			fixed = 8;
			put(0, uint32_t(7));
			put(4, uint32_t(0));
			break;
		case attribute_class:
			fixed = 12;
			put(0, uint32_t(0x0000000f));
			put(4, int32_t(255));
			put(8, uint32_t(8));
			put_name(12, u"NTFS");
			break;
		case full_size_class:
			fixed = 32;
			put(0, total);
			put(8, available);
			put(16, available);
			put(24, uint32_t(8));
			put(28, uint32_t(512));
			break;
		default:
			call.ret(invalid_class);
			return;
		}
		if (capacity < fixed) {
			finish(length_mismatch, 0);
			return;
		}
		// A name that does not fit is cut at the buffer's end.
		const size_t written = std::min<size_t>(bytes.size(), capacity);
		call.write(call[2], std::span<const uint8_t>(bytes.data(), written));
		finish(written < bytes.size() ? buffer_overflow : 0, written);
	});
	// GetTempFileName: a zero unique value picks a free name and creates the file; any other value only forms the name.
	registry.add("GetTempFileNameW", 4, [&, resolve, missing, file_of, copy_file](ApiCall &call) {
		const auto directory = call.wide(call[0]);
		auto prefix = call.wide(call[1]);
		if (prefix.size() > 3)
			prefix.resize(3);
		const uint32_t requested = call.u32(2) & 0xffff;
		const auto name_for = [&](uint32_t unique) {
			static constexpr char digits[] = "0123456789abcdef";
			std::u16string name = directory;
			if (name.empty() || (name.back() != u'\\' && name.back() != u'/'))
				name.push_back(u'\\');
			name += prefix;
			bool started = false;
			for (int shift = 12; shift >= 0; shift -= 4) {
				const auto digit = (unique >> shift) & 15;
				if (digit || started || shift == 0) {
					name.push_back(char16_t(digits[digit]));
					started = true;
				}
			}
			return name + u".tmp";
		};
		uint32_t unique = requested;
		std::u16string name;
		if (requested) {
			name = name_for(unique);
		} else {
			static uint32_t counter = 0;
			for (unsigned tries = 0; tries < 0x10000; ++tries) {
				unique = ((::getpid() * 7919U) + (++counter) * 31U + tries) & 0xffff;
				if (!unique)
					continue;
				name = name_for(unique);
				const auto resolved = paths.resolve(name);
				if (!resolved) {
					call.fail(123);
					return;
				}
				if (!resolved->parent_exists) {
					call.fail(3);
					return;
				}
				const int descriptor = ::open(resolved->host.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
				if (descriptor >= 0) {
					::close(descriptor);
					break;
				}
				if (errno != EEXIST) {
					call.fail(error_of(errno));
					return;
				}
				name.clear();
			}
			if (name.empty()) {
				call.fail(80);
				return;
			}
		}
		if (name.size() >= 260) {
			call.fail(111);
			return;
		}
		call.put_wide(call[3], name, 260);
		call.ret(unique);
	});
	// There are no 8.3 aliases in the prefix, so the short form of an existing path is its own long form.
	registry.add("GetShortPathNameW", 3, [&, resolve, missing, file_of, copy_file](ApiCall &call) {
		const auto text = call.wide(call[0]);
		const auto resolved = resolve(call, text);
		if (!resolved)
			return;
		if (!resolved->exists) {
			call.fail(missing(*resolved));
			return;
		}
		const auto full = paths.full(text);
		const size_t capacity = call.u32(2);
		call.put_wide(call[1], full, capacity);
		call.ret(capacity > full.size() ? full.size() : full.size() + 1);
	});
	// SearchPath: the program's directory, the current directory, the system directories, then the path named by the caller.
	registry.add("SearchPathW", 6, [&, resolve, missing, file_of, copy_file](ApiCall &call) {
		auto file = call.wide(call[1]);
		if (file.empty()) {
			call.fail(87);
			return;
		}
		if (call[2] && file.find(u'.') == std::u16string::npos)
			file += call.wide(call[2]);
		std::vector<std::u16string> directories;
		if (call[0]) {
			directories.push_back(call.wide(call[0]));
		} else {
			const auto &image = context.launch.image_path;
			const auto slash = image.find_last_of(u'\\');
			if (slash != std::u16string::npos)
				directories.push_back(image.substr(0, slash));
			directories.push_back(paths.current());
			directories.push_back(u"C:\\Windows\\System32");
			directories.push_back(u"C:\\Windows");
		}
		for (const auto &directory : directories) {
			std::u16string candidate = directory;
			if (candidate.empty() || candidate.back() != u'\\')
				candidate.push_back(u'\\');
			candidate += file;
			const auto resolved = paths.resolve(candidate);
			if (!resolved || !resolved->exists || !std::filesystem::is_regular_file(resolved->host))
				continue;
			const auto full = paths.full(candidate);
			const size_t capacity = call.u32(3);
			if (capacity > full.size()) {
				call.put_wide(call[4], full, capacity);
				if (call[5]) {
					const auto slash = full.find_last_of(u'\\');
					call.store_pointer(call[5], call[4] + (slash == std::u16string::npos ? 0 : slash + 1) * 2);
				}
			}
			call.ret(capacity > full.size() ? full.size() : full.size() + 1);
			return;
		}
		call.fail(2);
	});
	registry.add("GetDiskFreeSpaceExW", 4, [&, resolve, missing, file_of, copy_file](ApiCall &call) {
		const auto resolved = call[0] ? resolve(call, call.wide(call[0])) : paths.resolve(paths.current());
		if (!resolved)
			return;
		struct statvfs info{};
		auto path = resolved->host;
		while (!std::filesystem::exists(path) && path.has_parent_path() && path != path.parent_path())
			path = path.parent_path();
		if (::statvfs(path.c_str(), &info)) {
			call.fail(error_of(errno));
			return;
		}
		if (call[1]) call.store<uint64_t>(call[1], uint64_t(info.f_bavail) * info.f_frsize);
		if (call[2]) call.store<uint64_t>(call[2], uint64_t(info.f_blocks) * info.f_frsize);
		if (call[3]) call.store<uint64_t>(call[3], uint64_t(info.f_bfree) * info.f_frsize);
		call.ret(1);
	});
	const auto find_first = [&](ApiCall &call, uint64_t name_argument, uint64_t data_argument) {
		call.ret(invalid_handle);
		const auto pattern_text = call.wide(call[name_argument]);
		const auto path = paths.full(pattern_text);
		if (path.empty()) {
			call.set_last_error(123);
			return;
		}
		const auto slash = path.find_last_of(u'\\');
		const auto directory = path.substr(0, slash == 2 ? 3 : slash);
		const auto pattern = path.substr(slash + 1);
		const auto resolved = paths.resolve(directory);
		if (!resolved || !resolved->exists) {
			call.set_last_error(3);
			return;
		}
		auto found = std::make_shared<FindObject>();
		const bool wild = pattern.find_first_of(u"*?<>") != pattern.npos;
		std::vector<std::u16string> names;
		if (wild) {
			DIR *handle = ::opendir(resolved->host.c_str());
			if (!handle) {
				call.set_last_error(error_of(errno));
				return;
			}
			while (const auto *item = ::readdir(handle)) {
				const auto name = from_utf8(item->d_name);
				if (directory.size() <= 3 && (name == u"." || name == u".."))
					continue;
				if (wildcard(pattern, name))
					names.push_back(name);
			}
			::closedir(handle);
		} else {
			const auto target = paths.resolve(path);
			if (target && target->exists)
				names.push_back(from_utf8(target->host.filename().string()));
		}
		std::sort(names.begin(), names.end(), [](const auto &a, const auto &b) { return compare_fold(a, b) < 0; });
		for (const auto &name : names)
			found->entries.push_back(entry_of(resolved->host / to_utf8(name), name));
		if (found->entries.empty()) {
			call.set_last_error(wild ? 2 : (paths.resolve(path) && paths.resolve(path)->parent_exists ? 2 : 3));
			return;
		}
		call.write(call[data_argument], find_data(found->entries[0]));
		found->next = 1;
		// Search handles take no security attributes; the fourth argument of FindFirstFileExW is the search operation.
		const auto made = objects.add_object(found);
		call.ret(made.status ? invalid_handle : made.value);
		call.set_last_error(0);
	};
	registry.add("FindFirstFileW", 2, [find_first](ApiCall &call) { find_first(call, 0, 1); });
	registry.add("FindFirstFileExW", 6, [find_first](ApiCall &call) {
		if (call.u32(1) > 1 || call.u32(3) > 2) {
			call.fail(87, invalid_handle);
			return;
		}
		find_first(call, 0, 2);
	});
	registry.add("FindNextFileW", 2, [&, resolve, missing, file_of, copy_file, find_first](ApiCall &call) {
		const auto found = objects.object_as<FindObject>(call[0]);
		if (!found) {
			call.fail(6);
			return;
		}
		if (found->next >= found->entries.size()) {
			call.fail(18);
			return;
		}
		call.write(call[1], find_data(found->entries[found->next++]));
		call.ret(1);
	});
	registry.add("FindClose", 1, [&, resolve, missing, file_of, copy_file, find_first](ApiCall &call) {
		if (!objects.object_as<FindObject>(call[0])) {
			call.fail(6);
			return;
		}
		objects.close(call[0]);
		call.ret(1);
	});
	// FILE_NOTIFY_CHANGE_* filters: names (1, 2), attributes (4), size (8), last write (0x10), last access (0x20), creation (0x40) and
	// security (0x100).
	registry.add("FindFirstChangeNotificationW", 3, [&, resolve, missing](ApiCall &call) {
		const uint32_t filter = call.u32(2);
		call.ret(invalid_handle);
		if (!filter || (filter & ~0x17fU)) {
			call.set_last_error(87);
			return;
		}
		const auto resolved = resolve(call, call.wide(call[0]));
		if (!resolved)
			return;
		std::error_code error;
		if (!std::filesystem::is_directory(resolved->host, error)) {
			call.set_last_error(resolved->exists ? 267 : missing(*resolved));
			return;
		}
		uint32_t mask = IN_ONLYDIR;
		if (filter & 3)
			mask |= IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF;
		if (filter & 0x104)
			mask |= IN_ATTRIB;
		if (filter & 0x18)
			mask |= IN_MODIFY | IN_CLOSE_WRITE;
		if (filter & 0x20)
			mask |= IN_ACCESS;
		if (filter & 0x40)
			mask |= IN_CREATE;
		const int descriptor = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
		if (descriptor < 0) {
			call.set_last_error(8);
			return;
		}
		auto object = std::make_shared<ChangeNotificationObject>(descriptor);
		if (::inotify_add_watch(descriptor, resolved->host.c_str(), mask) < 0) {
			call.set_last_error(error_of(errno, false));
			return;
		}
		// inotify watches one directory: the tree is the directories found below it now, as many as the host's watch limit leaves room for.
		if (uint32_t(call[1]) & 0xff) {
			for (std::filesystem::recursive_directory_iterator walk(resolved->host, std::filesystem::directory_options::skip_permission_denied, error), end;
				!error && walk != end; walk.increment(error))
				if (walk->is_directory(error) && !walk->is_symlink(error) && ::inotify_add_watch(descriptor, walk->path().c_str(), mask) < 0)
					break;
		}
		const auto made = objects.add_object(object);
		if (made.status) {
			call.set_last_error(4);
			return;
		}
		call.ret(made.value);
	});
	registry.add("FindNextChangeNotification", 1, [&](ApiCall &call) {
		const auto object = objects.object_as<ChangeNotificationObject>(call[0]);
		if (!object) {
			call.fail(6);
			return;
		}
		object->take_changes();
		call.ret(1);
	});
	registry.add("FindCloseChangeNotification", 1, [&](ApiCall &call) {
		if (!objects.object_as<ChangeNotificationObject>(call[0])) {
			call.fail(6);
			return;
		}
		objects.close(call[0]);
		call.ret(1);
	});
	registry.add("GetDriveTypeW", 1, [&, resolve, missing, file_of, copy_file, find_first](ApiCall &call) {
		// DRIVE_FIXED for the prefix's drives, DRIVE_NO_ROOT_DIR for letters without a mapping. Z: shows the host's whole file system, a
		// share rather than a disk of this machine: DRIVE_REMOTE, so installers looking for the fixed disk with the most room stay off it.
		const auto name = call[0] ? paths.full(call.wide(call[0])) : paths.current();
		const auto root = paths.resolve(name);
		call.ret(!root || !root->exists ? 1 : !name.empty() && upper_case(name[0]) == u'Z' ? 4 : 3);
	});
	registry.add("DuplicateHandle", 7, [&, resolve, missing, file_of, find_first, copy_file](ApiCall &call) {
		constexpr uint64_t current = UINT64_MAX;
		constexpr uint32_t same_access = 0x2, close_source = 0x1;
		const auto source_process = call[0] == current ? nullptr : objects.object(call[0]);
		const auto target_process = call[2] == current ? nullptr : objects.object(call[2]);
		for (const auto handle : {call[0], call[2]}) {
			if (handle == current) continue;
			const auto &object = handle == call[0] ? source_process : target_process;
			if (!object || object->type_name() != u"Process") { call.fail(6); return; }
			if (!(objects.access_of(handle) & 0x40)) { call.fail(5); return; }
		}
		if (call[3]) {
			const auto previous = call.read(call[3], call.pointer_width());
			call.write(call[3], previous);
		}
		const bool from_self = call[0] == current || objects.object_as<SelfProcessObject>(call[0]);
		const bool to_self = call[2] == current || objects.object_as<SelfProcessObject>(call[2]);
		std::optional<uint32_t> typed_rights;
		const auto source = from_self ? objects.object(call[1]) : nullptr;
		const bool token = std::dynamic_pointer_cast<AccessToken>(source) != nullptr;
		const bool event = source && source->type_name() == u"Event";
		const bool mutant = source && source->type_name() == u"Mutant";
		if ((token || event || mutant) && !(call.u32(6) & same_access)) {
			const auto requested = token ? token_access(call.u32(4)) : event ? event_access(call.u32(4)) : mutex_access(call.u32(4));
			const uint32_t held = objects.access_of(call[1]);
			if (!requested) {
				call.fail(87);
				return;
			}
			typed_rights = call.u32(4) & 0x02000000U ? held : *requested;
		}
		// The channel to the process a handle names: a child of this one, or the parent that started it.
		const auto channel_of = [&](uint64_t handle) -> HandleChannel * {
			const auto process = objects.object_as<ProcessObject>(handle);
			if (!process)
				return nullptr;
			if (process->channel)
				return process->channel.get();
			return process->foreign && process->pid == ::getppid() ? parent_channel() : nullptr;
		};
		if (from_self && !to_self) {
			// A handle for another process travels over the channel to it and has a value there that the sender chose.
			auto *channel = channel_of(call[2]);
			const auto object = objects.object(call[1]);
			InheritedHandle record;
			if (!channel || !object || !object->describe(record)) {
				call.fail(50);
				return;
			}
			record.handle_access = typed_rights.value_or(objects.access_of(call[1]));
			const auto made = channel->send(objects, context.threads, std::move(record), call.u32(5));
			if (made.status || !made.value) {
				call.fail(made.status == 0xc000009aU ? 8 : made.status == 0xc0000008U ? 6 : 50);
				return;
			}
			if (call[3])
				call.store_pointer(call[3], made.value);
			if ((call.u32(6) & close_source) && call[1] != UINT64_MAX && call[1] != UINT64_MAX - 1)
				objects.close(call[1]);
			call.ret(1);
			return;
		}
		if (!from_self) {
			// A handle of another process is asked for over the channel to it; it can only come to this process.
			auto *channel = channel_of(call[0]);
			if (!channel || !to_self) {
				call.fail(50);
				return;
			}
			uint32_t access = 0;
			const auto object = channel->fetch(objects, context.threads, call[1], call.u32(6) & close_source, access);
			if (!object) {
				call.fail(6);
				return;
			}
			uint32_t granted = (call.u32(6) & same_access) || !call.u32(4) ? access : call.u32(4);
			if ((std::dynamic_pointer_cast<AccessToken>(object) || object->type_name() == u"Event" || object->type_name() == u"Mutant") && !(call.u32(6) & same_access)) {
				const auto requested = object->type_name() == u"Event" ? event_access(call.u32(4)) :
					object->type_name() == u"Mutant" ? mutex_access(call.u32(4)) : token_access(call.u32(4));
				if (!requested) {
					call.fail(87);
					return;
				}
				granted = call.u32(4) & 0x02000000U ? access : *requested;
			}
			const auto made = objects.add_object(object);
			if (made.status) {
				call.fail(4);
				return;
			}
			objects.set_access(made.value, granted);
			if (call.u32(5))
				objects.set_inherit(made.value, true);
			if (call[3])
				call.store_pointer(call[3], made.value);
			call.ret(1);
			return;
		}
		if (!call[3] && !(call.u32(6) & close_source)) {
			call.fail(87);
			return;
		}
		// The current-thread pseudo handle duplicates into a real handle to the calling thread; the current-process one into the process itself.
		std::optional<uint32_t> narrowed = typed_rights;
		const bool section = objects.object_as<SectionObject>(call[1]) != nullptr;
		if (section && !(call.u32(6) & same_access)) {
			// A copy cannot carry rights the original lacks; asking for them is refused.
			const uint32_t held = objects.access_of(call[1]);
			uint32_t wanted = call.u32(4);
			if (wanted & (0x10000000U | 0x02000000U))
				wanted = held;
			if (wanted & ~held) {
				if (std::getenv("RAVEN_TRACE_FAIL"))
					std::fprintf(stderr, "raven: section handle %llx holds %x, a copy asked for %x\n", (unsigned long long)call[1], held, wanted);
				call.fail(5);
				return;
			}
			narrowed = wanted;
		} else if (!typed_rights && !(call.u32(6) & same_access) && call[1] < UINT64_MAX - 1) {
			// Any other copy holds the rights it asks for; asking for everything gives what the original holds.
			const uint32_t wanted = call.u32(4);
			narrowed = wanted & (0x10000000U | 0x02000000U) ? objects.access_of(call[1]) : wanted;
		}
		const auto made = call[1] == UINT64_MAX - 1 && call[0] == UINT64_MAX ?
			objects.add_object(std::make_shared<ThreadObject>(context.threads.find(context.threads.current_id()))) :
			call[1] == UINT64_MAX ? objects.add_object(std::make_shared<SelfProcessObject>()) : objects.duplicate(call[1], narrowed);
		if (made.status) {
			call.fail(6);
			return;
		}
		if (call.u32(5))
			objects.set_inherit(made.value, true);
		if (call[3])
			call.store_pointer(call[3], made.value);
		// DUPLICATE_CLOSE_SOURCE closes the original whatever else happens.
		if ((call.u32(6) & close_source) && call[1] != UINT64_MAX && call[1] != UINT64_MAX - 1)
			objects.close(call[1]);
		call.ret(1);
	});
	// NtDuplicateObject takes the arguments of DuplicateHandle, with OBJ_INHERIT among the attributes, and answers with a status.
	registry.add("NtDuplicateObject", 7, [duplicate = registry.find("DuplicateHandle")->second](ApiCall &call) {
		ApiCall inner(call.process, call.environment, {call[0], call[1], call[2], call[3], call[4], (call.u32(5) & 0x2) ? 1u : 0u, call[6]});
		duplicate(inner);
		if (inner.result) {
			call.ret(0);
			return;
		}
		switch (inner.last_error()) {
		case 5: call.ret(0xc0000022U); break;
		case 6: call.ret(0xc0000008U); break;
		case 8: call.ret(0xc000009aU); break;
		case 87: call.ret(0xc000000dU); break;
		case 50: call.ret(0xc00000bbU); break;
		default: call.ret(0xc0000001U); break;
		}
	});
	// Power requests keep the machine awake while a program works, as during a download; the host's own power management decides here, so a
	// request is a handle that setting and clearing accept.
	registry.add("PowerCreateRequest", 1, [&objects](ApiCall &call) {
		const auto made = objects.create_event(0x1f0003, 0, false);
		if (made.status) {
			call.fail(8, UINT64_MAX);
			return;
		}
		call.ret(made.value);
	});
	for (const char *name : {"PowerSetRequest", "PowerClearRequest"})
		registry.add(name, 2, [](ApiCall &call) { call.ret(1); });
	// Kernel transactions: a transaction is a handle the transacted file calls name, committed or rolled back at the end. The calls here
	// take effect at once, as they do when no transaction is given, so committing succeeds and rolling back leaves what was done.
	registry.add("CreateTransaction", 7, [&objects](ApiCall &call) {
		const auto made = objects.create_event(0x1f0003, 0, false);
		if (made.status) {
			call.fail(8, UINT64_MAX);
			return;
		}
		call.ret(made.value);
	});
	for (const char *name : {"CommitTransaction", "CommitTransactionAsync", "RollbackTransaction", "RollbackTransactionAsync"})
		registry.add(name, 1, [](ApiCall &call) { call.ret(1); });
	// Each transacted file call is its plain form without the transaction: (name, plain form, its arguments taken from the first ones).
	struct Transacted {
		const char *name, *plain;
		size_t arguments;
		std::vector<size_t> from;
	};
	const Transacted transacted[] = {
		{"CreateFileTransactedW", "CreateFileW", 10, {0, 1, 2, 3, 4, 5, 6}},
		{"DeleteFileTransactedW", "DeleteFileW", 2, {0}},
		{"MoveFileTransactedW", "MoveFileExW", 6, {0, 1, 4}},
		{"CopyFileTransactedW", "CopyFileExW", 7, {0, 1, 2, 3, 4, 5}},
		{"CreateDirectoryTransactedW", "CreateDirectoryW", 4, {1, 2}},
		{"RemoveDirectoryTransactedW", "RemoveDirectoryW", 2, {0}},
		{"GetFileAttributesTransactedW", "GetFileAttributesExW", 4, {0, 1, 2}},
		{"SetFileAttributesTransactedW", "SetFileAttributesW", 3, {0, 1}},
	};
	for (const auto &entry : transacted) {
		const auto plain = registry.find(entry.plain);
		if (!plain)
			continue;
		registry.add(entry.name, entry.arguments, [handler = plain->second, from = entry.from](ApiCall &call) {
			std::vector<uint64_t> words;
			for (const auto index : from)
				words.push_back(call[index]);
			ApiCall inner(call.process, call.environment, words);
			handler(inner);
			call.ret(inner.result);
		});
	}
}

}
