#include "environment.h"
#include "nls.h"

#include <array>
#include <atomic>
#include <thread>
#include <chrono>
#include <string_view>
#include <algorithm>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>

namespace raven {

static void field(NativeProcess &process, uint64_t address, uint64_t value, size_t size)
{
	std::array<uint8_t, 8> bytes{};
	for (size_t index = 0; index < size; ++index)
		bytes[index] = static_cast<uint8_t>(value >> (index * 8));
	process.write_memory(address, std::span<const uint8_t>(bytes).first(size));
}

static uint32_t value(NativeProcess &process, uint64_t address)
{
	const auto bytes = process.read_memory(address, 4);
	uint32_t result = 0;
	for (size_t index = 0; index < bytes.size(); ++index)
		result |= uint32_t(bytes[index]) << (index * 8);
	return result;
}

// KUSER_SHARED_DATA, the page Windows maps read-only at 0x7ffe0000 in every process: programs and installers read the version of
// Windows, the system folder and the processor count straight from it, and Go reads its clocks there. The values agree with what
// GetVersionEx answers. The page is shared with the host, which keeps the clock fields current as Windows does at each clock tick.
namespace {

constexpr uint64_t shared_user_data = 0x7ffe0000;

// A KSYSTEM_TIME: the low word and two copies of the high word. Readers take the first high copy, the low word and the second copy,
// and read again when the copies differ, so the second copy is written first and the first copy last.
void store_system_time(uint8_t *page, size_t at, uint64_t value)
{
	const auto word = [&](size_t offset) { return std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t *>(page + at + offset)); };
	word(8).store(uint32_t(value >> 32), std::memory_order_release);
	word(0).store(uint32_t(value), std::memory_order_release);
	word(4).store(uint32_t(value >> 32), std::memory_order_release);
}

// The clock fields: the tick count (in ticks of 15.625 ms, the TickCountMultiplier the page declares), the interrupt time since boot
// and the system time since 1601, both in 100-nanosecond units.
void store_clocks(uint8_t *page)
{
	using namespace std::chrono;
	const auto interrupt_time = uint64_t(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count() / 100);
	const auto system_time = uint64_t(duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count() / 100) + 116444736000000000ULL;
	const uint64_t ticks = interrupt_time / 156250;
	std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t *>(page)).store(uint32_t(ticks), std::memory_order_release);
	store_system_time(page, 0x08, interrupt_time);
	store_system_time(page, 0x14, system_time);
	store_system_time(page, 0x320, ticks);
}

}

static void map_shared_user_data(NativeProcess &process)
{
	try {
		process.read_memory(shared_user_data, 4);
		return;
	} catch (...) {}
	const int descriptor = ::memfd_create("raven-shared-user-data", MFD_CLOEXEC);
	if (descriptor < 0 || ::ftruncate(descriptor, 4096)) {
		if (descriptor >= 0)
			::close(descriptor);
		throw std::runtime_error("cannot create the shared user data page");
	}
	auto *page = static_cast<uint8_t *>(::mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0));
	if (page == MAP_FAILED) {
		::close(descriptor);
		throw std::runtime_error("cannot map the shared user data page");
	}
	try {
		process.map_shared(descriptor, 4096, 0, shared_user_data);
	} catch (...) {
		::munmap(page, 4096);
		::close(descriptor);
		throw;
	}
	::close(descriptor);
	const uint64_t base = shared_user_data;
	field(process, base + 0x04, 0x0fa00000, 4);
	store_clocks(page);
	field(process, base + 0x2c, 0x14c, 2);
	field(process, base + 0x2e, 0x8664, 2);
	const std::u16string_view root = u"C:\\Windows";
	for (size_t index = 0; index < root.size(); ++index)
		field(process, base + 0x30 + index * 2, root[index], 2);
	field(process, base + 0x260, 19045, 4);
	field(process, base + 0x264, 1, 4);
	field(process, base + 0x268, 1, 1);
	field(process, base + 0x26c, 10, 4);
	field(process, base + 0x270, 0, 4);
	field(process, base + 0x2d0, 0x100, 4);
	field(process, base + 0x2e8, sysconf(_SC_PHYS_PAGES), 4);
	field(process, base + 0x3c0, std::max<long>(1, sysconf(_SC_NPROCESSORS_ONLN)), 4);
	process.protect_memory(base, 4096, PROT_READ);
	// The host's view of the page lives as long as the process; the thread updates it every millisecond, finer than the 15.625 ms
	// clock tick Windows runs at by default.
	std::thread([page] {
		for (;;) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			store_clocks(page);
		}
	}).detach();
}

NativeEnvironment::NativeEnvironment(NativeProcess &process, uint64_t image_base,
	uint32_t process_id, uint32_t thread_id) : process(process)
{
	if (process.mode() == NativeMode::x86) {
		x86_constructor(image_base, process_id, thread_id);
		return;
	}
	auto context = process.context();
	if (process.active() || context.registers.gs_base || !process_id || !thread_id || !image_base || image_base % 65536)
		throw std::runtime_error("invalid initial native environment or existing GS environment");
	if (process.read_memory(image_base, 2) != std::vector<uint8_t>{0x4d, 0x5a})
		throw std::runtime_error("native environment requires an image header");
	const auto [stack_limit, stack_base] = process.stack_bounds();
	try {
		map_shared_user_data(process);
		process_block = process.map_memory(4096, PROT_READ | PROT_WRITE);
		thread_block = process.map_memory(8192, PROT_READ | PROT_WRITE);
		// x64 NT_TIB, CLIENT_ID and the initial TEB/PEB contract fields.
		field(process, thread_block, UINT64_MAX, 8);
		field(process, thread_block + 0x08, stack_base, 8);
		field(process, thread_block + 0x10, stack_limit, 8);
		// DeallocationStack: the bottom of the stack's allocation, where a runtime that measures its stack finds its end.
		field(process, thread_block + 0x1478, stack_limit, 8);
		field(process, thread_block + 0x30, thread_block, 8);
		field(process, thread_block + 0x40, process_id, 8);
		field(process, thread_block + 0x48, thread_id, 8);
		field(process, thread_block + 0x60, process_block, 8);
		field(process, process_block + 0x10, image_base, 8);
		// How long a thread waits for a critical section before the runtime library reports a deadlock: thirty days, relative, in units of 100 ns.
		field(process, process_block + 0xc0, uint64_t(-int64_t(2592000) * 10000000), 8);
		// The loader lock: a critical section in the rest of the PEB page, free (lock count -1, no owner), named at PEB+0x110.
		field(process, process_block + 0x800 + 0x08, UINT32_MAX, 4);
		field(process, process_block + 0x110, process_block + 0x800, 8);
		// The PEB version agrees with KUSER_SHARED_DATA and GetVersionEx.
		field(process, process_block + 0x118, 10, 4);
		field(process, process_block + 0x11c, 0, 4);
		field(process, process_block + 0x120, 19045, 2);
		field(process, process_block + 0x124, 2, 4);
		context.registers.gs_base = thread_block;
		process.set_context(context);
		thread_ids[thread_block] = thread_id;
	} catch (...) {
		try {
			if (thread_block)
				process.unmap_memory(thread_block);
			if (process_block)
				process.unmap_memory(process_block);
		} catch (...) {}
		throw;
	}
}

uint64_t NativeEnvironment::create_thread_block(uint64_t stack_base, uint64_t stack_limit, uint32_t thread_id)
{
	if (!thread_id)
		throw std::runtime_error("native guest threads need a thread identity");
	if (process.mode() == NativeMode::x86) {
		const auto block = process.map_memory(8192, PROT_READ | PROT_WRITE);
		try {
			field(process, block, UINT32_MAX, 4);
			field(process, block + 0x04, stack_base, 4);
			field(process, block + 0x08, stack_limit, 4);
			// DeallocationStack: a thread's stack is allocated with one guard page below its limit.
			field(process, block + 0xe0c, stack_limit - 4096, 4);
			field(process, block + 0x18, block, 4);
			field(process, block + 0x20, value(process, thread_block + 0x20), 4);
			field(process, block + 0x24, thread_id, 4);
			field(process, block + 0x30, process_block, 4);
		} catch (...) {
			process.unmap_memory(block);
			throw;
		}
		thread_ids[block] = thread_id;
		return block;
	}
	const auto teb = process.map_memory(8192, PROT_READ | PROT_WRITE);
	try {
		field(process, teb, UINT64_MAX, 8);
		field(process, teb + 0x08, stack_base, 8);
		field(process, teb + 0x10, stack_limit, 8);
		field(process, teb + 0x1478, stack_limit - 4096, 8);
		field(process, teb + 0x30, teb, 8);
		field(process, teb + 0x40, value(process, thread_block + 0x40), 8);
		field(process, teb + 0x48, thread_id, 8);
		field(process, teb + 0x60, process_block, 8);
	} catch (...) {
		process.unmap_memory(teb);
		throw;
	}
	thread_ids[teb] = thread_id;
	return teb;
}

void NativeEnvironment::release_thread_block(uint64_t teb)
{
	if (teb == thread_block)
		throw std::runtime_error("the current native thread block cannot be released");
	thread_ids.erase(teb);
	process.unmap_memory(teb);
}

void NativeEnvironment::x86_constructor(uint64_t image_base, uint32_t process_id, uint32_t thread_id)
{
	if (process.active() || !process_id || !thread_id || !image_base || image_base % 65536 || image_base > UINT32_MAX)
		throw std::runtime_error("invalid initial native environment");
	if (process.read_memory(image_base, 2) != std::vector<uint8_t>{0x4d, 0x5a})
		throw std::runtime_error("native environment requires an image header");
	const auto [stack_limit, stack_base] = process.stack_bounds();
	try {
		map_shared_user_data(process);
		process_block = process.map_memory(4096, PROT_READ | PROT_WRITE);
		thread_block = process.map_memory(8192, PROT_READ | PROT_WRITE);
		// x86 NT_TIB, CLIENT_ID and the initial TEB/PEB contract fields.
		field(process, thread_block, UINT32_MAX, 4);
		field(process, thread_block + 0x04, stack_base, 4);
		field(process, thread_block + 0x08, stack_limit, 4);
		field(process, thread_block + 0xe0c, stack_limit, 4);
		field(process, thread_block + 0x18, thread_block, 4);
		field(process, thread_block + 0x20, process_id, 4);
		field(process, thread_block + 0x24, thread_id, 4);
		thread_ids[thread_block] = thread_id;
		field(process, thread_block + 0x30, process_block, 4);
		field(process, process_block + 0x08, image_base, 4);
		field(process, process_block + 0x70, uint64_t(-int64_t(2592000) * 10000000), 8);
		// The loader lock, as for x64: a free critical section in the PEB page, named at PEB+0xa0.
		field(process, process_block + 0x800 + 0x04, UINT32_MAX, 4);
		field(process, process_block + 0xa0, process_block + 0x800, 4);
		field(process, process_block + 0xa4, 10, 4);
		field(process, process_block + 0xa8, 0, 4);
		field(process, process_block + 0xac, 19045, 2);
		field(process, process_block + 0xb0, 2, 4);
		process.set_teb(thread_block);
	} catch (...) {
		try {
			if (thread_block)
				process.unmap_memory(thread_block);
			if (process_block)
				process.unmap_memory(process_block);
		} catch (...) {}
		throw;
	}
}

void NativeEnvironment::require_current() const
{
	if (process.mode() == NativeMode::x86)
		return;
	const auto serving = process.serving_teb();
	if ((serving ? serving : process.context().registers.gs_base) != thread_block)
		throw std::runtime_error("native environment is not the current guest thread");
}

uint32_t NativeEnvironment::last_error() const
{
	require_current();
	return value(process, thread_block + (process.mode() == NativeMode::x86 ? 0x34 : 0x68));
}

void NativeEnvironment::set_last_error(uint32_t error)
{
	require_current();
	field(process, thread_block + (process.mode() == NativeMode::x86 ? 0x34 : 0x68), error, 4);
}

uint64_t NativeEnvironment::prepare_heaps()
{
	require_current();
	if (process.active()) throw std::runtime_error("native heap storage requires process startup");
	const auto previous=process.read_memory(process_block+0xe8,16);
	if (heap_storage || previous!=std::vector<uint8_t>(16) || process_heap())
		throw std::runtime_error("native heap storage is already initialized");
	process.write_memory(process_block+0xe8,previous);
	heap_storage=process.map_memory(4096,PROT_READ | PROT_WRITE);
	field(process,process_block+0xec,heap_capacity,4);
	field(process,process_block+0xf0,heap_storage,8);
	return heap_storage;
}

void NativeEnvironment::publish_process_heap(uint64_t heap)
{
	require_current();
	if (process.active() || !heap_storage || !heap || process_heap() || value(process,process_block+0xe8)!=1 ||
		value(process,process_block+0xec)!=heap_capacity)
		throw std::runtime_error("invalid native process heap publication");
	const auto list=process.read_memory(process_block+0xf0,8);
	for (size_t index=0; index<8; ++index)
		if (list[index]!=uint8_t(heap_storage>>(index*8)))
			throw std::runtime_error("native heap list owner changed during initialization");
	const auto bytes=process.read_memory(heap_storage,8);
	for (size_t index=0; index<8; ++index)
		if (bytes[index]!=uint8_t(heap>>(index*8)))
			throw std::runtime_error("native process heap does not belong to the shared heap list");
	process.read_memory(heap,16);
	field(process,process_block+0x30,heap,8);
}

uint64_t NativeEnvironment::process_heap() const
{
	require_current();
	const auto bytes=process.read_memory(process_block+0x30,8);
	uint64_t heap=0;
	for (size_t index=0; index<8; ++index) heap|=uint64_t(bytes[index])<<(index*8);
	return heap;
}

uint64_t NativeEnvironment::pointer(uint64_t address) const
{
	const auto width = process.mode() == NativeMode::x86 ? 4 : 8;
	const auto bytes = process.read_memory(address, width);
	uint64_t result = 0;
	for (size_t index = 0; index < bytes.size(); ++index)
		result |= uint64_t(bytes[index]) << (index * 8);
	return result;
}

void NativeEnvironment::prepare_parameters_x86(const NativeLaunch &launch,std::array<uint64_t,3> standard_handles)
{
	if (process.active() || command_line_ansi || process.read_memory(process_block+0x10,4)!=std::vector<uint8_t>(4))
		throw std::runtime_error("native process parameters are already initialized or running");
	if (launch.image_path.empty() || launch.image_path.size()>32766 || launch.command_line.size()>32766 ||
		launch.image_path.find(u'\0')!=launch.image_path.npos || launch.command_line.find(u'\0')!=launch.command_line.npos)
		throw std::runtime_error("invalid native process image or command line");
	const auto &title=launch.startup.title ? *launch.startup.title : launch.image_path;
	if (title.size()>32766 || title.find(u'\0')!=title.npos || launch.startup.runtime_data.size()>65535)
		throw std::runtime_error("native startup title or runtime data exceeds its ABI bound");
	// Normalized x86 RTL_USER_PROCESS_PARAMETERS. Empty fields do not grant host resources.
	const auto ansi=encode_ansi(launch.command_line,launch.ansi_code_page);
	const auto ansi_title=encode_ansi(title,launch.ansi_code_page);
	const size_t length=0x400+(launch.image_path.size()+1+launch.command_line.size()+1+title.size()+1)*2+
		4+ansi.size()+1+ansi_title.size()+1+1+launch.startup.runtime_data.size()+7+68;
	const size_t capacity=(length+4095)&~size_t(4095);
	std::vector<uint8_t> bytes(capacity);
	const auto address=process.map_memory(capacity,PROT_READ | PROT_WRITE);
	try {
		const auto put=[&](size_t offset,uint64_t number,size_t width) {
			for (size_t i=0;i<width;++i) bytes.at(offset+i)=uint8_t(number>>(i*8));
		};
		put(0,capacity,4); put(4,length,4); put(8,1,4);
		for (size_t i=0;i<3;++i) put(0x18+i*4,standard_handles[i],4);
		size_t cursor=0x400;
		const auto text=[&](size_t offset,std::u16string_view string) {
			put(offset,string.size()*2,2); put(offset+2,(string.size()+1)*2,2);
			put(offset+4,address+cursor,4);
			for (const auto character : string) { put(cursor,character,2); cursor+=2; }
			cursor+=2;
		};
		text(0x38,launch.image_path); text(0x40,launch.command_line);
		text(0x70,title);
		for (const size_t offset : {0x24U,0x30U}) {
			put(offset+2,2,2); put(offset+4,address+cursor,4);
		}
		put(0x7c,address+cursor,4);
		cursor+=4;
		const auto command_offset=cursor;
		std::copy(ansi.begin(),ansi.end(),bytes.begin()+static_cast<ptrdiff_t>(cursor));
		cursor+=ansi.size()+1;
		const auto title_offset=cursor;
		std::copy(ansi_title.begin(),ansi_title.end(),bytes.begin()+static_cast<ptrdiff_t>(cursor));
		cursor+=ansi_title.size()+1;
		const auto desktop_offset=cursor++;
		const auto runtime=launch.startup.runtime_data.empty() ? 0 : address+cursor;
		std::copy(launch.startup.runtime_data.begin(),launch.startup.runtime_data.end(),bytes.begin()+static_cast<ptrdiff_t>(cursor));
		cursor+=launch.startup.runtime_data.size();
		put(0x88,launch.startup.runtime_data.size(),2); put(0x8a,launch.startup.runtime_data.size(),2); put(0x8c,runtime,4);
		for (size_t i=0;i<7;++i) put(0x4c+i*4,launch.startup.geometry[i],4);
		put(0x68,launch.startup.flags,4); put(0x6c,launch.startup.show_window,4);
		cursor=(cursor+7)&~size_t(7);
		const auto startup_offset=cursor;
		// STARTUPINFOA.
		put(cursor,68,4); put(cursor+8,address+desktop_offset,4); put(cursor+12,address+title_offset,4);
		for (size_t i=0;i<7;++i) put(cursor+16+i*4,launch.startup.geometry[i],4);
		put(cursor+44,launch.startup.flags,4); put(cursor+48,launch.startup.show_window,2);
		put(cursor+50,launch.startup.runtime_data.size(),2); put(cursor+52,runtime,4);
		for (size_t i=0;i<3;++i) put(cursor+56+i*4,standard_handles[i],4);
		process.write_memory(address,bytes);
		field(process,process_block+0x10,address,4);
		command_line_ansi=address+command_offset;
		ansi_code_page=launch.ansi_code_page;
		startup_info_ansi=address+startup_offset;
	} catch (...) {
		process.unmap_memory(address);
		throw;
	}
}

void NativeEnvironment::prepare_parameters(const NativeLaunch &launch,std::array<uint64_t,3> standard_handles)
{
	if (process.mode() == NativeMode::x86) {
		prepare_parameters_x86(launch,standard_handles);
		return;
	}
	require_current();
	if (process.active() || command_line_ansi || process.read_memory(process_block+0x20,8)!=std::vector<uint8_t>(8))
		throw std::runtime_error("native process parameters are already initialized or running");
	if (launch.image_path.empty() || launch.image_path.size()>32766 || launch.command_line.size()>32766 ||
		launch.image_path.find(u'\0')!=launch.image_path.npos || launch.command_line.find(u'\0')!=launch.command_line.npos)
		throw std::runtime_error("invalid native process image or command line");
	const auto &title=launch.startup.title ? *launch.startup.title : launch.image_path;
	if (title.size()>32766 || title.find(u'\0')!=title.npos || launch.startup.runtime_data.size()>65535)
		throw std::runtime_error("native startup title or runtime data exceeds its ABI bound");
	// Normalized x64 RTL_USER_PROCESS_PARAMETERS. Empty fields do not grant host resources.
	const auto ansi=encode_ansi(launch.command_line,launch.ansi_code_page);
	const auto ansi_title=encode_ansi(title,launch.ansi_code_page);
	const size_t length=0x400+(launch.image_path.size()+1+launch.command_line.size()+1+title.size()+1)*2+
		4+ansi.size()+1+ansi_title.size()+1+1+launch.startup.runtime_data.size()+7+104;
	const size_t capacity=(length+4095)&~size_t(4095);
	std::vector<uint8_t> bytes(capacity);
	const auto address=process.map_memory(capacity,PROT_READ | PROT_WRITE);
	try {
		const auto put=[&](size_t offset,uint64_t number,size_t width) {
			for (size_t i=0;i<width;++i) bytes.at(offset+i)=uint8_t(number>>(i*8));
		};
		put(0,capacity,4); put(4,length,4); put(8,1,4);
		for (size_t i=0;i<3;++i) put(0x20+i*8,standard_handles[i],8);
		size_t cursor=0x400;
		const auto text=[&](size_t offset,std::u16string_view string) {
			put(offset,string.size()*2,2); put(offset+2,(string.size()+1)*2,2);
			put(offset+8,address+cursor,8);
			for (const auto character : string) { put(cursor,character,2); cursor+=2; }
			cursor+=2;
		};
		text(0x60,launch.image_path); text(0x70,launch.command_line);
		text(0xb0,title);
		for (const size_t offset : {0x38U,0x50U}) {
			put(offset+2,2,2); put(offset+8,address+cursor,8);
		}
		put(0x80,address+cursor,8); put(0x3f0,4,8);
		cursor+=4;
		const auto command_offset=cursor;
		std::copy(ansi.begin(),ansi.end(),bytes.begin()+static_cast<ptrdiff_t>(cursor));
		cursor+=ansi.size()+1;
		const auto title_offset=cursor;
		std::copy(ansi_title.begin(),ansi_title.end(),bytes.begin()+static_cast<ptrdiff_t>(cursor));
		cursor+=ansi_title.size()+1;
		const auto desktop_offset=cursor++;
		const auto runtime=launch.startup.runtime_data.empty() ? 0 : address+cursor;
		std::copy(launch.startup.runtime_data.begin(),launch.startup.runtime_data.end(),bytes.begin()+static_cast<ptrdiff_t>(cursor));
		cursor+=launch.startup.runtime_data.size();
		put(0xe0,launch.startup.runtime_data.size(),2); put(0xe2,launch.startup.runtime_data.size(),2); put(0xe8,runtime,8);
		for (size_t i=0;i<7;++i) put(0x88+i*4,launch.startup.geometry[i],4);
		put(0xa4,launch.startup.flags,4); put(0xa8,launch.startup.show_window,4);
		cursor=(cursor+7)&~size_t(7);
		const auto startup_offset=cursor;
		put(cursor,104,4); put(cursor+16,address+desktop_offset,8); put(cursor+24,address+title_offset,8);
		for (size_t i=0;i<7;++i) put(cursor+32+i*4,launch.startup.geometry[i],4);
		put(cursor+60,launch.startup.flags,4); put(cursor+64,launch.startup.show_window,2);
		put(cursor+66,launch.startup.runtime_data.size(),2); put(cursor+72,runtime,8);
		for (size_t i=0;i<3;++i) put(cursor+80+i*8,standard_handles[i],8);
		process.write_memory(address,bytes);
		field(process,process_block+0x20,address,8);
		command_line_ansi=address+command_offset;
		ansi_code_page=launch.ansi_code_page;
		startup_info_ansi=address+startup_offset;
	} catch (...) {
		process.unmap_memory(address);
		throw;
	}
}

bool NativeEnvironment::dispatch(std::string_view name)
{
	if (name=="GetStdHandle") {
		require_current();
		const auto selector=uint32_t(process.api_arguments(1)[0]);
		if (selector<uint32_t(-12) || selector>uint32_t(-10)) {
			set_last_error(6);process.return_api(UINT64_MAX);return true;
		}
		const bool narrow=process.mode()==NativeMode::x86;
		const auto parameters=pointer(process_block+(narrow ? 0x10 : 0x20));
		if (!parameters) throw std::runtime_error("native process parameters are not initialized");
		const auto handle=pointer(parameters+(narrow ? 0x18+(uint32_t(-10)-selector)*4 : 0x20+(uint32_t(-10)-selector)*8));
		process.return_api(handle);return true;
	}
	if (multibyte_information(name)) return true;
	if (name=="NtQuerySystemInformation")
		return system_information();
	if (name=="GetStartupInfoA" || name=="GetStartupInfoW")
		return startup_information(name);
	if (name=="GetCommandLineA" || name=="GetACP") {
		require_current();
		process.api_arguments(0);
		if (!command_line_ansi) throw std::runtime_error("native ANSI process parameters are not initialized");
		process.return_api(name=="GetCommandLineA" ? command_line_ansi : ansi_code_page);
		return true;
	}
	if (name=="GetCommandLineW") {
		require_current();
		process.api_arguments(0);
		const bool narrow=process.mode()==NativeMode::x86;
		const auto parameters=pointer(process_block+(narrow ? 0x10 : 0x20));
		if (!parameters) throw std::runtime_error("native process parameters are not initialized");
		process.return_api(pointer(parameters+(narrow ? 0x44 : 0x78)));
		return true;
	}
	if (name != "GetLastError" && name != "SetLastError" && name != "GetCurrentProcessId" && name != "GetCurrentThreadId")
		return false;
	require_current();
	const auto arguments = process.api_arguments(name == "SetLastError" ? 1 : 0);
	uint32_t result = 0;
	if (name == "SetLastError")
		set_last_error(static_cast<uint32_t>(arguments[0]));
	else if (name == "GetLastError")
		result = last_error();
	else
		result = value(process, thread_block + (process.mode() == NativeMode::x86 ? (name == "GetCurrentProcessId" ? 0x20 : 0x24) :
			(name == "GetCurrentProcessId" ? 0x40 : 0x48)));
	process.return_api(result);
	return true;
}

}
