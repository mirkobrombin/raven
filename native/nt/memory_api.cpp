#include "host_names.h"
#include "firmware.h"
#include "kernel_context.h"
#include "section.h"
#include "security_attributes.h"
#include "text.h"

#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace raven {

namespace {

struct View {
	std::shared_ptr<SectionObject> mapping;
	uint64_t offset = 0, size = 0;
	bool writable = false;
};

// A section made with SEC_IMAGE maps its file as the loader lays a program out: the headers, then each section at its own address with
// the access its characteristics give, relocated by the difference from the base the image asks for. Nothing in it runs; a runtime that
// reads its libraries this way only finds its data where the headers say. Returns the view's size, or 0 when the file is no image.
uint64_t map_image(ApiCall &call, NativeMemory &memory, int descriptor, uint64_t &address)
{
	struct stat status{};
	if (::fstat(descriptor, &status) != 0)
		return 0;
	const uint64_t file_size = uint64_t(status.st_size);
	const auto read_at = [&](uint64_t offset, size_t size) {
		std::vector<uint8_t> bytes(size);
		if (offset > file_size || size > file_size - offset || ::pread(descriptor, bytes.data(), size, off_t(offset)) != ssize_t(size))
			bytes.clear();
		return bytes;
	};
	const auto u16 = [](const std::vector<uint8_t> &bytes, size_t at) { return uint32_t(bytes[at] | bytes[at + 1] << 8); };
	const auto u32 = [](const std::vector<uint8_t> &bytes, size_t at) {
		return uint32_t(bytes[at]) | uint32_t(bytes[at + 1]) << 8 | uint32_t(bytes[at + 2]) << 16 | uint32_t(bytes[at + 3]) << 24;
	};
	const auto dos = read_at(0, 64);
	if (dos.empty() || dos[0] != 'M' || dos[1] != 'Z')
		return 0;
	const uint64_t header = u32(dos, 0x3c);
	const auto nt = read_at(header, 24);
	if (nt.empty() || u32(nt, 0) != 0x4550)
		return 0;
	const uint32_t count = u16(nt, 6), optional_size = u16(nt, 20);
	const auto optional = read_at(header + 24, optional_size);
	if (optional.size() < 96)
		return 0;
	const bool wide = u16(optional, 0) == 0x20b;
	if (!wide && u16(optional, 0) != 0x10b)
		return 0;
	const uint64_t preferred = wide ? uint64_t(u32(optional, 24)) | uint64_t(u32(optional, 28)) << 32 : u32(optional, 28);
	const uint64_t image_size = (uint64_t(u32(optional, 56)) + 4095) & ~uint64_t(4095);
	const uint64_t headers_size = std::min<uint64_t>(u32(optional, 60), file_size);
	const size_t directories = wide ? 112 : 96;
	const uint32_t relocations = optional.size() >= directories + 48 ? u32(optional, directories + 40) : 0;
	const uint32_t relocations_size = optional.size() >= directories + 48 ? u32(optional, directories + 44) : 0;
	const auto table = read_at(header + 24 + optional_size, size_t(count) * 40);
	if (!image_size || (count && table.empty()))
		return 0;
	const auto range = memory.allocate(0, image_size, 0x3000, 4);
	if (range.status)
		return 0;
	address = range.address;
	call.write(address, read_at(0, headers_size));
	for (uint32_t index = 0; index < count; ++index) {
		const size_t entry = size_t(index) * 40;
		const uint64_t virtual_address = u32(table, entry + 12), raw_size = u32(table, entry + 16), raw = u32(table, entry + 20);
		const uint64_t virtual_size = u32(table, entry + 8) ? u32(table, entry + 8) : raw_size;
		const uint64_t length = std::min({raw_size, virtual_size, file_size > raw ? file_size - raw : 0});
		if (virtual_address < image_size && length)
			call.write(address + virtual_address, read_at(raw, std::min(length, image_size - virtual_address)));
	}
	// Base relocations: blocks of a page address, their size and 16-bit entries whose top four bits name the kind.
	const uint64_t delta = address - preferred;
	if (delta && relocations && relocations_size && relocations < image_size && relocations_size <= image_size - relocations) {
		const auto blocks = call.read(address + relocations, relocations_size);
		for (size_t at = 0; at + 8 <= blocks.size();) {
			const uint32_t page = u32(blocks, at), size = u32(blocks, at + 4);
			if (size < 8 || size > blocks.size() - at)
				break;
			for (size_t item = at + 8; item + 2 <= at + size; item += 2) {
				const uint32_t value = u16(blocks, item), kind = value >> 12;
				const uint64_t target = address + page + (value & 0xfff);
				if (kind == 10 && page + (value & 0xfff) + 8 <= image_size)
					call.store<uint64_t>(target, call.load<uint64_t>(target) + delta);
				else if (kind == 3 && page + (value & 0xfff) + 4 <= image_size)
					call.store<uint32_t>(target, uint32_t(call.load<uint32_t>(target) + delta));
			}
			at += size;
		}
	}
	memory.protect(address, (headers_size + 4095) & ~uint64_t(4095), 2);
	for (uint32_t index = 0; index < count; ++index) {
		const size_t entry = size_t(index) * 40;
		const uint64_t virtual_address = u32(table, entry + 12);
		const uint64_t virtual_size = u32(table, entry + 8) ? u32(table, entry + 8) : u32(table, entry + 16);
		const uint32_t characteristics = u32(table, entry + 36);
		const bool execute = characteristics & 0x20000000U, write = characteristics & 0x80000000U;
		const uint32_t access = execute ? (write ? 0x40 : 0x20) : write ? 0x04 : 0x02;
		if (virtual_address < image_size && virtual_size)
			memory.protect(address + virtual_address, std::min((virtual_size + 4095) & ~uint64_t(4095), image_size - virtual_address), access);
	}
	return image_size;
}

// Where the named sections of the prefix live: one host file each, held by every process that has the section open.
std::filesystem::path section_directory()
{
	std::filesystem::path base;
	if (const char *runtime = std::getenv("XDG_RUNTIME_DIR"); runtime && *runtime)
		base = runtime;
	else
		base = NativePrefix::from_environment().root();
	auto directory = base / ("raven-sections-" + prefix_tag());
	std::error_code error;
	std::filesystem::create_directories(directory, error);
	return directory;
}

std::string upper_utf8(const std::u16string &name)
{
	auto text = to_utf8(name);
	for (auto &character : text)
		character = char(std::toupper(static_cast<unsigned char>(character)));
	return text;
}

// Opens the host file of a named section and holds a shared lock on it. A creator that finds the file free makes it new; one that finds
// it held opens the section that exists. The lock is what tells a live section from the file a past run left.
int open_named_section(const std::u16string &name, uint64_t size, bool create, bool &existed, std::string &path, uint32_t &error)
{
	existed = false;
	const auto directory = section_directory();
	path = (directory / hex_text(name_hash(upper_utf8(name)))).string();
	// Opening and creating take the directory's lock, as SharedFile does: the change of a creator's lock from exclusive to shared
	// would otherwise let another process find the section unheld and truncate it.
	const int guard = lock_directory(directory);
	struct Release {
		int descriptor;
		~Release() { if (descriptor >= 0) ::close(descriptor); }
	} release{guard};
	static std::once_flag swept;
	std::call_once(swept, [&] { sweep_directory(directory); });
	const int descriptor = ::open(path.c_str(), (create ? O_CREAT : 0) | O_RDWR | O_CLOEXEC, 0600);
	if (descriptor < 0) {
		error = errno == ENOENT ? 2 : 5;
		return -1;
	}
	if (::flock(descriptor, LOCK_EX | LOCK_NB) == 0) {
		if (create) {
			if (::ftruncate(descriptor, 0) || ::ftruncate(descriptor, off_t(size))) {
				::close(descriptor);
				error = 8;
				return -1;
			}
			::flock(descriptor, LOCK_SH);
			return descriptor;
		}
		// Nobody holds the file: the section is gone.
		::close(descriptor);
		error = 2;
		return -1;
	}
	::flock(descriptor, LOCK_SH);
	existed = true;
	return descriptor;
}

}

void register_memory_api(ApiRegistry &registry, KernelContext &context)
{
	auto &allocator = context.allocator;
	auto &memory = context.memory;
	auto &objects = context.objects;
	constexpr uint32_t zero_flag = 0x40, movable_flag = 2, modify_flag = 0x80;
	const auto allocate_block = [&](ApiCall &call) {
		const bool zero = call.u32(0) & zero_flag;
		uint64_t address;
		if (call.u32(0) & movable_flag) {
			address = allocator.allocate_movable(call[1], zero);
		} else {
			address = allocator.allocate(call[1], zero);
			if (address && !call[1])
				allocator.mark_empty(address);
		}
		if (!address) {
			call.fail(8);
			return;
		}
		call.ret(address);
	};
	registry.add("LocalAlloc", 2, allocate_block);
	registry.add("GlobalAlloc", 2, allocate_block);
	const auto free_block = [&](ApiCall &call) {
		if (call[0] && !allocator.release(call[0])) {
			call.fail(6, call[0]);
			return;
		}
		call.ret(0);
	};
	registry.add("LocalFree", 1, free_block);
	registry.add("GlobalFree", 1, free_block);
	const auto lock_block = [&](ApiCall &call) {
		const auto block = allocator.data(call[0]);
		const auto size = allocator.size(block);
		if (size == UINT64_MAX) {
			call.fail(6);
			return;
		}
		// A movable block without bytes is what Windows calls discarded: it has a handle, and nothing to lock.
		if (!size && allocator.movable(call[0])) {
			call.fail(157);
			return;
		}
		call.ret(block);
	};
	registry.add("LocalLock", 1, lock_block);
	registry.add("GlobalLock", 1, lock_block);
	const auto unlock_block = [](ApiCall &call) { call.set_last_error(0); };
	registry.add("LocalUnlock", 1, unlock_block);
	registry.add("GlobalUnlock", 1, unlock_block);
	const auto size_block = [&](ApiCall &call) {
		const auto size = allocator.size(allocator.data(call[0]));
		if (size == UINT64_MAX) {
			call.fail(6);
			return;
		}
		call.ret(size);
	};
	registry.add("LocalSize", 1, size_block);
	registry.add("GlobalSize", 1, size_block);
	const auto handle_block = [&](ApiCall &call) {
		if (allocator.size(call[0]) == UINT64_MAX) {
			call.fail(6);
			return;
		}
		call.ret(allocator.handle(call[0]));
	};
	registry.add("LocalHandle", 1, handle_block);
	registry.add("GlobalHandle", 1, handle_block);
	const auto resize_block = [&](ApiCall &call) {
		const auto block = allocator.data(call[0]);
		const auto old = allocator.size(block);
		if (old == UINT64_MAX) {
			call.fail(6);
			return;
		}
		// LMEM_MODIFY changes what kind of block this is, not its bytes.
		if (call.u32(2) & modify_flag) {
			if (call.u32(2) & movable_flag)
				allocator.make_movable(call[0]);
			call.ret(call[0]);
			return;
		}
		const auto address = allocator.allocate(call[1], call.u32(2) & zero_flag);
		if (!address) {
			call.fail(8);
			return;
		}
		if (const auto kept = std::min<uint64_t>(old, call[1]))
			call.write(address, call.read(block, kept));
		if (!call[1])
			allocator.mark_empty(address);
		if (allocator.movable(call[0])) {
			allocator.move(call[0], address);
			call.ret(call[0]);
			return;
		}
		allocator.release(call[0]);
		call.ret(address);
	};
	registry.add("LocalReAlloc", 3, resize_block);
	registry.add("GlobalReAlloc", 3, resize_block);
	registry.add("GlobalMemoryStatusEx", 1, [](ApiCall &call) {
		std::vector<uint8_t> bytes(64);
		const uint32_t length = 64, load = 20;
		std::memcpy(bytes.data(), &length, 4);
		std::memcpy(bytes.data() + 4, &load, 4);
		const uint64_t values[] = {16ULL << 30, 8ULL << 30, 32ULL << 30, 16ULL << 30, 0x7fffffe0000ULL, 0x7fffff00000ULL, 0};
		std::memcpy(bytes.data() + 8, values, 56);
		call.write(call[0], bytes);
		call.ret(1);
	});
	registry.add("GetPhysicallyInstalledSystemMemory", 1, [](ApiCall &call) {
		if (!call[0]) {
			call.fail(87);
			return;
		}
		std::ifstream file("/sys/firmware/dmi/tables/DMI", std::ios::binary);
		if (!file) {
			call.fail(50);
			return;
		}
		const std::vector<uint8_t> table(std::istreambuf_iterator<char>(file), {});
		const uint64_t memory = smbios_memory(table);
		const long pages = sysconf(_SC_PHYS_PAGES), page_size = sysconf(_SC_PAGESIZE);
		if (file.bad() || !memory || pages <= 0 || page_size <= 0 || memory < uint64_t(pages) * uint64_t(page_size) / 1024) {
			call.fail(13);
			return;
		}
		call.store<uint64_t>(call[0], memory);
		call.ret(1);
	});
	// MEMORYSTATUS: the same figures in SIZE_T fields; a 32-bit program sees them capped as Windows caps them, at 4 GiB less a byte for
	// memory and at its 2 GiB address space.
	registry.add("GlobalMemoryStatus", 1, [](ApiCall &call) {
		const size_t width = call.pointer_width();
		const uint64_t cap = width == 4 ? 0xffffffffULL : UINT64_MAX;
		const uint64_t values[] = {16ULL << 30, 8ULL << 30, 32ULL << 30, 16ULL << 30,
			width == 4 ? 0x7ffe0000ULL : 0x7fffffe0000ULL, width == 4 ? 0x7fe00000ULL : 0x7fffff00000ULL};
		std::vector<uint8_t> bytes(8 + 6 * width);
		const uint32_t length = uint32_t(bytes.size()), load = 20;
		std::memcpy(bytes.data(), &length, 4);
		std::memcpy(bytes.data() + 4, &load, 4);
		for (size_t index = 0; index < 6; ++index) {
			const uint64_t value = std::min(values[index], cap);
			std::memcpy(bytes.data() + 8 + index * width, &value, width);
		}
		call.write(call[0], bytes);
	});
	registry.add("HeapSetInformation", 4, [](ApiCall &call) { call.ret(1); });
	// Only HeapCompatibilityInformation is answered: every heap is a low-fragmentation heap (2).
	registry.add("HeapQueryInformation", 5, [](ApiCall &call) {
		if (call.u32(1) != 0) {
			call.fail(87);
			return;
		}
		if (call[4]) call.store_pointer(call[4], 4);
		if (call.u32(3) < 4 || !call[2]) {
			call.fail(24);
			return;
		}
		call.store<uint32_t>(call[2], 2);
		call.ret(1);
	});
	registry.add("HeapValidate", 3, [](ApiCall &call) { call.ret(1); });
	registry.add("HeapCompact", 2, [](ApiCall &call) { call.ret(0x10000); });

	registry.add("CreateFileMappingW", 6, [&](ApiCall &call) {
		auto mapping = std::make_shared<SectionObject>();
		const auto name = call[5] ? call.wide(call[5], 260) : std::u16string();
		bool existed = false;
		if (call[0] != UINT64_MAX) {
			mapping->file = objects.object_as<FileObject>(call[0]);
			if (!mapping->file) {
				call.fail(6);
				return;
			}
			struct stat metadata{};
			::fstat(mapping->file->descriptor, &metadata);
			const uint64_t maximum = uint64_t(call.u32(3)) << 32 | call.u32(4);
			mapping->size = maximum ? maximum : uint64_t(metadata.st_size);
		} else {
			mapping->size = uint64_t(call.u32(3)) << 32 | call.u32(4);
			if (!mapping->size) {
				call.fail(87);
				return;
			}
			if (name.empty()) {
				mapping->descriptor = ::memfd_create("raven-section", MFD_CLOEXEC);
				if (mapping->descriptor < 0 || ::ftruncate(mapping->descriptor, off_t(mapping->size))) {
					call.fail(8);
					return;
				}
			} else {
				uint32_t error = 0;
				mapping->descriptor = open_named_section(name, mapping->size, true, existed, mapping->backing, error);
				if (mapping->descriptor < 0) {
					call.fail(error);
					return;
				}
				mapping->name = name;
				if (existed) {
					struct stat metadata{};
					::fstat(mapping->descriptor, &metadata);
					mapping->size = uint64_t(metadata.st_size);
				}
			}
		}
		mapping->protection = call.u32(2);
		const auto made = objects.add_object(mapping);
		if (made.status) {
			call.fail(8);
			return;
		}
		// SECTION_ALL_ACCESS: the rights of a section are kept per handle so a copy can give some up.
		objects.set_access(made.value, 0xf001f);
		if (inheritable_handle(call, call[1]))
			objects.set_inherit(made.value, true);
		call.set_last_error(existed ? 183 : 0);
		call.ret(made.value);
	});
	registry.add("OpenFileMappingW", 3, [&](ApiCall &call) {
		if (!call[2]) {
			call.fail(87);
			return;
		}
		auto mapping = std::make_shared<SectionObject>();
		mapping->name = call.wide(call[2], 260);
		bool existed = false;
		uint32_t error = 0;
		mapping->descriptor = open_named_section(mapping->name, 0, false, existed, mapping->backing, error);
		if (mapping->descriptor < 0) {
			call.fail(error);
			return;
		}
		struct stat metadata{};
		::fstat(mapping->descriptor, &metadata);
		mapping->size = uint64_t(metadata.st_size);
		mapping->protection = 4;
		const auto made = objects.add_object(mapping);
		if (made.status) {
			call.fail(8);
			return;
		}
		objects.set_inherit(made.value, call.u32(1) != 0);
		// An opened section carries the rights the caller asked for.
		objects.set_access(made.value, call.u32(0) & ~0xf0000000U ? (call.u32(0) & ~0xf0000000U) : 0xf001f);
		call.ret(made.value);
	});
	// atlthunk: a thunk is a few instructions in executable memory that call a window procedure with the window handle replaced by a
	// value of the program's (the object behind the window). Thunks come from pages kept for them; a freed one is used again.
	struct ThunkPool {
		std::vector<uint64_t> free;
	};
	auto thunks = std::make_shared<ThunkPool>();
	constexpr uint64_t thunk_size = 32;
	registry.add("AtlThunk_AllocateData", 0, [&memory, thunks](ApiCall &call) {
		if (thunks->free.empty()) {
			const auto page = memory.allocate(0, 0x10000, 0x3000, 0x40);
			if (page.status) {
				call.fail(8, 0);
				return;
			}
			for (uint64_t at = page.address + 0x10000; at > page.address; at -= thunk_size)
				thunks->free.push_back(at - thunk_size);
		}
		const auto thunk = thunks->free.back();
		thunks->free.pop_back();
		call.ret(thunk);
	});
	registry.add("AtlThunk_InitData", 3, [](ApiCall &call) {
		std::vector<uint8_t> code;
		const auto add = [&](std::initializer_list<uint8_t> bytes) { code.insert(code.end(), bytes); };
		const auto add_word = [&](uint64_t value, size_t size) {
			for (size_t byte = 0; byte < size; ++byte)
				code.push_back(uint8_t(value >> (byte * 8)));
		};
		if (call.pointer_width() == 8) {
			// mov rcx, value; mov rax, procedure; jmp rax
			add({0x48, 0xb9});
			add_word(call[2], 8);
			add({0x48, 0xb8});
			add_word(call[1], 8);
			add({0xff, 0xe0});
		} else {
			// mov dword [esp+4], value; mov eax, procedure; jmp eax
			add({0xc7, 0x44, 0x24, 0x04});
			add_word(call[2], 4);
			add({0xb8});
			add_word(call[1], 4);
			add({0xff, 0xe0});
		}
		call.write(call[0], code);
	});
	registry.add("AtlThunk_DataToCode", 1, [](ApiCall &call) { call.ret(call[0]); });
	registry.add("AtlThunk_FreeData", 1, [thunks](ApiCall &call) {
		if (call[0])
			thunks->free.push_back(call[0]);
	});
	// The guest runs as native code on a processor whose instruction cache follows stores, and nothing here caches translated code: a
	// flush has nothing to do and succeeds.
	registry.add("FlushInstructionCache", 3, [](ApiCall &call) { call.ret(1); });
	registry.add("NtFlushInstructionCache", 3, [](ApiCall &call) { call.ret(0); });
	// SectionBasicInformation: the base address, the allocation attributes and the maximum size of the section.
	// The PE headers of an image section's file: the machine, the subsystem and the rest SectionImageInformation reports.
	const auto image_headers = [](const SectionObject &mapping) -> std::optional<std::vector<uint8_t>> {
		if (!mapping.file)
			return std::nullopt;
		std::vector<uint8_t> head(4096);
		const ssize_t count = ::pread(mapping.file->descriptor, head.data(), head.size(), 0);
		if (count < 64 || head[0] != 'M' || head[1] != 'Z')
			return std::nullopt;
		head.resize(size_t(count));
		uint32_t offset;
		std::memcpy(&offset, head.data() + 60, 4);
		if (uint64_t(offset) + 24 + 96 > head.size() || std::memcmp(head.data() + offset, "PE\0\0", 4))
			return std::nullopt;
		return std::vector<uint8_t>(head.begin() + offset, head.end());
	};
	// A section over a file or over memory; SEC_IMAGE (0x1000000) asks for a file holding a PE image.
	registry.add("NtCreateSection", 7, [&, image_headers](ApiCall &call) {
		constexpr uint32_t status_invalid_handle = 0xc0000008U, status_no_memory = 0xc0000017U, status_invalid_parameter = 0xc000000dU,
			status_not_mz = 0xc000012fU, sec_image = 0x1000000;
		auto mapping = std::make_shared<SectionObject>();
		const uint64_t maximum = call[3] ? call.load<uint64_t>(call[3]) : 0;
		if (call[6]) {
			mapping->file = objects.object_as<FileObject>(call[6]);
			if (!mapping->file) {
				call.ret(status_invalid_handle);
				return;
			}
			struct stat metadata{};
			::fstat(mapping->file->descriptor, &metadata);
			mapping->size = maximum ? maximum : uint64_t(metadata.st_size);
			mapping->image = (call.u32(5) & sec_image) != 0;
			if (mapping->image && !image_headers(*mapping)) {
				call.ret(status_not_mz);
				return;
			}
		} else {
			mapping->size = maximum;
			if (!mapping->size || (call.u32(5) & sec_image)) {
				call.ret(status_invalid_parameter);
				return;
			}
			mapping->descriptor = ::memfd_create("raven-section", MFD_CLOEXEC);
			if (mapping->descriptor < 0 || ::ftruncate(mapping->descriptor, off_t(mapping->size))) {
				call.ret(status_no_memory);
				return;
			}
		}
		mapping->protection = call.u32(4);
		const auto made = objects.add_object(mapping);
		if (made.status) {
			call.ret(status_no_memory);
			return;
		}
		objects.set_access(made.value, call.u32(1) ? call.u32(1) : 0xf001f);
		call.store_pointer(call[0], made.value);
		call.ret(0);
	});
	registry.add("NtQuerySection", 5, [&, image_headers](ApiCall &call) {
		constexpr uint32_t status_invalid_handle = 0xc0000008U, status_invalid_info_class = 0xc0000003U, status_info_length_mismatch = 0xc0000004U, sec_commit = 0x8000000,
			status_section_not_image = 0xc0000049U;
		const auto mapping = objects.object_as<SectionObject>(call[0]);
		if (!mapping) {
			call.ret(status_invalid_handle);
			return;
		}
		// SectionImageInformation: the entry point, the stack sizes, the subsystem, the characteristics and the machine of the image.
		if (call.u32(1) == 1) {
			const auto headers = mapping->image ? image_headers(*mapping) : std::nullopt;
			if (!headers) {
				call.ret(status_section_not_image);
				return;
			}
			const size_t width = call.pointer_width(), layout = width == 8 ? 0x40 : 0x30;
			if (call.u32(3) < layout) {
				call.ret(status_info_length_mismatch);
				return;
			}
			const auto &pe = *headers;
			const auto read16 = [&](size_t at) { uint16_t value; std::memcpy(&value, pe.data() + at, 2); return value; };
			const auto read32 = [&](size_t at) { uint32_t value; std::memcpy(&value, pe.data() + at, 4); return value; };
			const auto read64 = [&](size_t at) { uint64_t value; std::memcpy(&value, pe.data() + at, 8); return value; };
			const size_t optional = 24;
			const bool wide = read16(optional) == 0x20b;
			const uint64_t base = wide ? read64(optional + 24) : read32(optional + 28);
			const uint64_t reserve = wide ? read64(optional + 72) : read32(optional + 72);
			const uint64_t commit = wide ? read64(optional + 80) : read32(optional + 76);
			std::vector<uint8_t> bytes(layout, 0);
			const auto put = [&](size_t at, uint64_t value, size_t size) { std::memcpy(bytes.data() + at, &value, size); };
			put(0, base + read32(optional + 16), width);
			put(width * 2, reserve, width);
			put(width * 3, commit, width);
			const size_t after = width * 4;
			put(after, read16(optional + 68), 4);
			put(after + 4, read16(optional + 50), 2);
			put(after + 6, read16(optional + 48), 2);
			put(after + 12, read16(22), 2);
			put(after + 14, read16(optional + 70), 2);
			put(after + 16, read16(4), 2);
			put(after + 18, read32(optional + 4) != 0, 1);
			put(after + 24, mapping->size, 4);
			put(after + 28, read32(optional + 64), 4);
			call.write(call[2], bytes);
			if (call[4])
				call.store<uint32_t>(call[4], uint32_t(layout));
			call.ret(0);
			return;
		}
		if (call.u32(1) != 0) {
			call.ret(status_invalid_info_class);
			return;
		}
		const size_t width = call.pointer_width();
		const size_t layout = width == 8 ? 24 : 16;
		if (call.u32(3) < layout) {
			call.ret(status_info_length_mismatch);
			return;
		}
		std::vector<uint8_t> bytes(layout, 0);
		const uint32_t attributes = sec_commit;
		std::memcpy(bytes.data() + width, &attributes, 4);
		std::memcpy(bytes.data() + layout - 8, &mapping->size, 8);
		call.write(call[2], bytes);
		if (call[4])
			call.store<uint32_t>(call[4], uint32_t(layout));
		call.ret(0);
	});
	const auto views_owner = std::make_shared<std::map<uint64_t, View>>();
	auto &views = *views_owner;
	registry.add("MapViewOfFile", 5, [&, views_owner](ApiCall &call) {
		const auto mapping = objects.object_as<SectionObject>(call[0]);
		if (!mapping) {
			call.fail(6);
			return;
		}
		// The handle must carry the right for the kind of view: writing needs the map-write right, reading the map-read right.
		if (mapping->descriptor >= 0) {
			const uint32_t need = (call.u32(1) & 0x2) ? 0x2U : 0x4U;
			if (!(objects.access_of(call[0]) & need)) {
				call.fail(5);
				return;
			}
		}
		const uint64_t offset = uint64_t(call.u32(2)) << 32 | call.u32(3);
		const uint64_t size = call[4] ? call[4] : mapping->size - offset;
		if (offset > mapping->size || size > mapping->size - offset) {
			call.fail(87);
			return;
		}
		// A view of shared memory maps the host file itself; the other kind is an allocation the file is read into.
		if (mapping->descriptor >= 0) {
			const uint32_t rights = call.u32(1);
			const uint32_t access = rights & 0x2 ? ((rights & 0x20) ? 0x40 : 0x04) : rights & 0x1 ? 0x08 : (rights & 0x20) ? 0x20 : 0x02;
			const auto view = memory.map_view(mapping->descriptor, offset, size, access);
			if (view.status) {
				call.fail(view.status == 0xc0000017U ? 8 : 87);
				return;
			}
			views[view.address] = {mapping, offset, size, bool(rights & 2)};
			call.ret(view.address);
			return;
		}
		if (mapping->file && (mapping->protection & 0x1000000U)) {
			uint64_t address = 0;
			const uint64_t image_size = map_image(call, memory, mapping->file->descriptor, address);
			if (!image_size) {
				call.fail(193);
				return;
			}
			views[address] = {mapping, 0, image_size, false};
			call.ret(address);
			return;
		}
		// A view that can only be read is the file itself, as on Windows: a program that maps a library of hundreds of megabytes to
		// read it would otherwise have the whole file copied into its memory. A view the program may write, or one past the end of the
		// file, is copied as before.
		if (mapping->file && !(call.u32(1) & 0x3)) {
			struct stat status{};
			if (::fstat(mapping->file->descriptor, &status) == 0 && offset + size <= uint64_t(status.st_size)) {
				try {
					const auto view = memory.map_view(mapping->file->descriptor, offset, size, call.u32(1) & 0x20 ? 0x20 : 0x02);
					if (!view.status) {
						views[view.address] = {mapping, offset, size, false};
						call.ret(view.address);
						return;
					}
				} catch (const std::runtime_error &) {
				}
			}
		}
		const auto range = memory.allocate(0, (size + 4095) & ~uint64_t(4095), 0x3000, 4);
		if (range.status) {
			call.fail(8);
			return;
		}
		if (mapping->file) {
			std::vector<uint8_t> bytes(size);
			const auto count = ::pread(mapping->file->descriptor, bytes.data(), size, off_t(offset));
			if (count > 0)
				call.write(range.address, std::span<const uint8_t>(bytes.data(), size_t(count)));
		}
		views[range.address] = {mapping, offset, size, bool(call.u32(1) & 2)};
		call.ret(range.address);
	});
	// A view at an address of the caller's choosing is not offered, as NtMapViewOfSection below: the request fails as on Windows when
	// the address is taken (ERROR_INVALID_ADDRESS), and a program that asked for a preferred address maps the view anywhere instead.
	registry.add("MapViewOfFileEx", 6, [&registry](ApiCall &call) {
		if (call[5]) {
			call.fail(487);
			return;
		}
		const auto map = registry.find("MapViewOfFile");
		ApiCall inner(call.process, call.environment, {call[0], call[1], call[2], call[3], call[4]});
		map->second(inner);
		call.ret(inner.result);
	});
	const auto write_back = [&](uint64_t address, ApiCall &call) {
		const auto found = views.find(address);
		if (found == views.end() || !found->second.writable || !found->second.mapping->file)
			return;
		const auto bytes = call.read(address, found->second.size);
		::pwrite(found->second.mapping->file->descriptor, bytes.data(), bytes.size(), off_t(found->second.offset));
	};
	registry.add("FlushViewOfFile", 2, [write_back](ApiCall &call) {
		write_back(call[0], call);
		call.ret(1);
	});
	registry.add("UnmapViewOfFile", 1, [&, write_back](ApiCall &call) {
		if (!views.contains(call[0])) {
			call.fail(998);
			return;
		}
		write_back(call[0], call);
		views.erase(call[0]);
		memory.free(call[0], 0, 0x8000);
		call.ret(1);
	});
	// The native forms map and unmap views of this process through the calls above; the page protection names the view's access.
	registry.add("NtMapViewOfSection", 10, [&registry, &objects](ApiCall &call) {
		const auto mapping = objects.object_as<SectionObject>(call[0]);
		const auto map = registry.find("MapViewOfFile");
		const bool self = call[1] == UINT64_MAX || (call.pointer_width() == 4 && call[1] == UINT32_MAX);
		if (!mapping || !map || !call[2] || !call[6]) {
			call.ret(mapping ? 0xc000000dU : 0xc0000008U);
			return;
		}
		if (!self) {
			call.ret(0xc0000022U);
			return;
		}
		if (call.load_pointer(call[2])) {
			// A view at an address of the caller's choosing is not offered.
			call.ret(0xc0000018U);
			return;
		}
		const uint64_t offset = call[5] ? call.load<uint64_t>(call[5]) : 0;
		const uint64_t size = call.load_pointer(call[6]);
		const uint32_t protect = call.u32(9);
		const uint32_t rights = (protect & 0x44 ? 0x6U : protect & 0x88 ? 0x1U : 0x4U) | (protect & 0xf0 ? 0x20U : 0U);
		ApiCall inner(call.process, call.environment, {call[0], rights, offset >> 32, offset & 0xffffffffU, size});
		const uint32_t before = call.last_error();
		map->second(inner);
		if (!inner.result) {
			const uint32_t error = call.last_error();
			call.set_last_error(before);
			call.ret(error == 5 ? 0xc0000022U : error == 8 ? 0xc0000017U : error == 6 ? 0xc0000008U : 0xc000000dU);
			return;
		}
		call.store_pointer(call[2], inner.result);
		call.store_pointer(call[6], size ? size : mapping->size - offset);
		call.ret(0);
	});
	registry.add("NtUnmapViewOfSection", 2, [&registry](ApiCall &call) {
		const auto unmap = registry.find("UnmapViewOfFile");
		if (!unmap || !(call[0] == UINT64_MAX || (call.pointer_width() == 4 && call[0] == UINT32_MAX))) {
			call.ret(0xc0000022U);
			return;
		}
		ApiCall inner(call.process, call.environment, {call[1]});
		const uint32_t before = call.last_error();
		unmap->second(inner);
		call.set_last_error(before);
		call.ret(inner.result ? 0 : 0xc0000019U);
	});
}

}
