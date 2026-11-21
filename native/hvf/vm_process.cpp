#include "vm_process.h"

#include "runtime.h"
#include "test_process.h"
#include "vcpu.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace raven::hvf {

VmProcess::VmProcess(Vm &vm) : vm(vm)
{
	vm.write_fault = [this](uint64_t address) { return code_write(address); };
	vm.emulate_store = [this](uint64_t address, const void *bytes, size_t size) { write(address, bytes, size); };
}

VmProcess::~VmProcess()
{
	vm.write_fault = nullptr;
	vm.emulate_store = nullptr;
}

void VmProcess::attach(translate::Runtime &runtime)
{
	std::lock_guard guard(lock);
	if (std::find(attached.begin(), attached.end(), &runtime) == attached.end())
		attached.push_back(&runtime);
}

void VmProcess::detach(translate::Runtime &runtime)
{
	std::lock_guard guard(lock);
	std::erase(attached, &runtime);
}

void VmProcess::drop_translations(uint64_t address)
{
	std::vector<translate::Runtime *> runtimes;
	{
		std::lock_guard guard(lock);
		runtimes = attached;
	}
	// Outside the lock: code_written takes each runtime's lock and calls watch_code.
	for (translate::Runtime *runtime : runtimes)
		runtime->code_written(address);
}

void VmProcess::watch_code(uint64_t address, size_t size, bool watch)
{
	std::lock_guard guard(lock);
	if (watch && attached.empty())
		throw std::logic_error("translating code of a VmProcess no runtime is attached to");
	for (uint64_t at = address & ~(Vm::page_size - 1); at < address + size; at += Vm::page_size) {
		unsigned &count = watched[at];
		if (watch && count++ == 0)
			vm.protect(at, false);
		else if (!watch && count && --count == 0) {
			vm.protect(at, true);
			watched.erase(at);
		} else if (!count)
			watched.erase(at);
	}
}

bool VmProcess::code_write(uint64_t address)
{
	const uint64_t page = address & ~(Vm::page_size - 1);
	if (!vm.host(address))
		return false;
	{
		std::lock_guard guard(lock);
		// Another store's fault on this page was handled first and unwatched it: this store just runs again.
		if (!watched.count(page))
			return true;
	}
	drop_translations(address);
	if (after_code_written)
		after_code_written(address);
	// code_written unwatched the page through watch_code. If a vCPU translated it again meanwhile, it is watched again and the retried
	// store faults again, dropping that translation too: making the page writable here would let the store miss it.
	return true;
}

namespace {

uint32_t read32(const uint8_t *at)
{
	uint32_t value;
	std::memcpy(&value, at, 4);
	return value;
}

bool same_name(std::string_view a, std::string_view b)
{
	return std::equal(a.begin(), a.end(), b.begin(), b.end(),
		[](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
}

}

uint64_t VmProcess::load(const std::filesystem::path &path)
{
	std::ifstream input(path, std::ios::binary);
	if (!input)
		throw std::runtime_error("cannot open " + path.string());
	Image image;
	image.name = path.filename().string();
	image.file.assign(std::istreambuf_iterator<char>(input), {});
	const PEImage pe(image.file);
	// ARM64EC images are x64 images to every loader that does not read their CHPE metadata.
	if (!pe.is_64 || pe.machine != 0x8664)
		throw std::runtime_error(path.string() + " is not an x64 image");
	// The VM's address space is the guest's own, so the image always gets its preferred base and needs no relocation.
	uint8_t *bytes = vm.map(pe.image_base, pe.image_size, Access::read_write);
	image.base = pe.image_base;
	image.entry = pe.entry;
	image.code_ranges = translate::lay_out_image(pe, image.file, bytes, pe.image_base);
	// IMAGE_LOAD_CONFIG_DIRECTORY64.CHPEMetadataPointer, at 0xc8; IMAGE_ARM64EC_METADATA's code map: RVA and count at 4 and 8, entries
	// of a start RVA whose low two bits are the kind (0 ARM64, 1 ARM64EC, 2 x64) and a length.
	const PEDirectory config = pe.directories[10];
	const auto inside = [&](uint64_t rva, uint64_t size) { return rva <= pe.image_size && size <= pe.image_size - rva; };
	if (config.address && config.size >= 0xd0 && inside(config.address, 0xd0) && read32(bytes + config.address) >= 0xd0) {
		uint64_t metadata;
		std::memcpy(&metadata, bytes + config.address + 0xc8, 8);
		if (metadata) {
			if (metadata < pe.image_base || !inside(metadata - pe.image_base, 80))
				throw std::runtime_error(path.string() + ": CHPE metadata outside the image");
			image.arm64ec_metadata = metadata;
			const uint8_t *chpe = bytes + (metadata - pe.image_base);
			const uint32_t count = read32(chpe + 8);
			if (!inside(read32(chpe + 4), uint64_t(count) * 8))
				throw std::runtime_error(path.string() + ": code map outside the image");
			const uint8_t *map = bytes + read32(chpe + 4);
			image.code_ranges.clear();
			for (uint32_t i = 0; i < count; ++i) {
				const uint32_t start = read32(map + 8 * i), length = read32(map + 8 * i + 4);
				if (!inside(start & ~3u, length))
					throw std::runtime_error(path.string() + ": code map range outside the image");
				const std::pair<uint64_t, uint64_t> range{image.base + (start & ~3u), image.base + (start & ~3u) + length};
				if ((start & 3) == 2)
					image.code_ranges.push_back(range);
				else {
					image.native_ranges.push_back(range);
					vm.set_access(range.first, length, Access::read_execute);
					vm.code_written(vm.host(range.first), length);
				}
			}
		}
	}
	images.push_back(std::move(image));
	return pe.image_base;
}

void VmProcess::prepare_thread(translate::CpuState &state, Vcpu &vcpu)
{
	prepare_thread(state);
	vcpu.set_reg(HV_REG_X18, state.gs_base);
}

void VmProcess::bind_imports()
{
	for (const Image &image : images) {
		const PEImage pe(image.file);
		for (const PEImport &import : pe.imports()) {
			const auto from = std::find_if(images.begin(), images.end(),
				[&](const Image &loaded) { return same_name(loaded.name, import.module); });
			if (from == images.end())
				throw std::runtime_error("no loaded image for " + import.module);
			uint64_t address = 0;
			for (const PEExport &exported : PEImage(from->file).exports())
				if ((import.by_ordinal && exported.ordinal == import.ordinal) ||
					(!import.by_ordinal && std::find(exported.names.begin(), exported.names.end(), import.name) != exported.names.end()))
					address = from->base + exported.address;
			if (!address)
				throw std::runtime_error("no export for " + import.module + "!" + import.name);
			vm.write(image.base + import.iat_address, &address, 8);
		}
	}
}

void VmProcess::link_arm64ec(translate::Runtime &runtime)
{
	std::vector<std::pair<uint64_t, uint64_t>> native;
	for (const Image &image : images)
		native.insert(native.end(), image.native_ranges.begin(), image.native_ranges.end());
	const translate::Runtime::Arm64ecRoutines routines = runtime.arm64ec_routines(native);
	for (const Image &image : images) {
		if (!image.arm64ec_metadata)
			continue;
		// IMAGE_ARM64EC_METADATA: the RVAs of the slots, each a pointer the code loads before branching.
		struct Slot {
			uint32_t offset;
			uint64_t routine;
		};
		const Slot slots[] = {
			{20, routines.dispatch_call}, // __os_arm64x_dispatch_call_no_redirect
			{24, routines.dispatch_ret},
			{28, routines.check_icall}, // __os_arm64x_dispatch_call, the code's __os_arm64x_check_call: x11 and x10 as check_icall
			{32, routines.check_icall}, // __os_arm64x_dispatch_icall, the code's __os_arm64x_check_icall
			{36, routines.check_icall}, // the same with a CFG check
			{56, routines.unsupported}, // GetX64InformationFunctionPointer
			{60, routines.unsupported}, // SetX64InformationFunctionPointer
			{72, routines.unsupported}, // __os_arm64x_dispatch_fptr
		};
		const uint8_t *chpe = vm.host(image.arm64ec_metadata);
		for (const Slot &slot : slots)
			if (const uint32_t rva = read32(chpe + slot.offset))
				vm.write(image.base + rva, &slot.routine, 8);
	}
}

bool VmProcess::is_native(uint64_t address) const
{
	for (const Image &image : images)
		for (const auto &[start, end] : image.native_ranges)
			if (address >= start && address < end)
				return true;
	return false;
}

bool VmProcess::native_return(uint64_t address) const
{
	if (!is_native(address) || !is_native(address - 4))
		return false;
	uint32_t word;
	vm.read(address - 4, &word, 4);
	return (word & 0xfffffc1fu) == 0xd63f0000u;
}

uint64_t VmProcess::entry_thunk(uint64_t address) const
{
	if (!is_native(address - 4))
		return 0;
	uint32_t word;
	vm.read(address - 4, &word, 4);
	const uint64_t thunk = address + static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(word & ~3u)));
	return word & 1 && is_native(thunk) ? thunk : 0;
}

uint64_t VmProcess::entry(uint64_t base) const
{
	for (const Image &image : images)
		if (image.base == base)
			return image.base + image.entry;
	throw std::runtime_error("no image at that base");
}

void VmProcess::prepare_thread(translate::Runtime &runtime)
{
	prepare_thread(runtime.state());
}

void VmProcess::prepare_thread(translate::CpuState &state)
{
	constexpr size_t stack_size = 1 << 20;
	const uint64_t stack = vm.map_anywhere(stack_size, Access::read_write);
	const uint64_t teb = vm.map_anywhere(Vm::page_size, Access::read_write);
	// NT_TIB: stack base and limit, and the TEB's own address, which gs:[0x30] reads.
	const uint64_t top = stack + stack_size;
	vm.write(teb + 0x8, &top, 8);
	vm.write(teb + 0x10, &stack, 8);
	vm.write(teb + 0x30, &teb, 8);
	state.gpr[translate::rsp] = top;
	state.gs_base = teb;
}

std::span<const uint8_t> VmProcess::code(uint64_t address) const
{
	for (const Image &image : images)
		for (const auto &[start, end] : image.code_ranges)
			if (address >= start && address < end)
				return {vm.host(address), static_cast<size_t>(end - address)};
	return {};
}

void VmProcess::read(uint64_t address, void *bytes, size_t size) const
{
	vm.read(address, bytes, size);
}

void VmProcess::write(uint64_t address, const void *bytes, size_t size)
{
	// Through the alias, past stage 2: a write the runtime makes to watched code drops its translations as a guest store would.
	vm.write(address, bytes, size);
	for (uint64_t page = address & ~(Vm::page_size - 1); page < address + size; page += Vm::page_size) {
		bool code;
		{
			std::lock_guard guard(lock);
			code = watched.count(page) != 0;
		}
		if (code)
			drop_translations(page);
	}
}

VmCodeCache::VmCodeCache(Vm &vm, size_t code_capacity, size_t data_capacity)
	: vm(vm), code_base(vm.map_anywhere(code_capacity, Access::read_execute)), code_alias(vm.host(code_base)),
	  code_size(code_capacity), data_base(vm.map_anywhere(data_capacity, Access::read_write)), data_alias(vm.host(data_base)),
	  data_size(data_capacity)
{
}

uint8_t *VmCodeCache::alias(const uint8_t *at) const
{
	return code_alias + (reinterpret_cast<uint64_t>(at) - code_base);
}

uint8_t *VmCodeCache::allocate(size_t size)
{
	const size_t start = (next + 15) & ~size_t(15);
	if (start + size > code_size)
		return nullptr;
	next = start + size;
	return reinterpret_cast<uint8_t *>(code_base + start);
}

void VmCodeCache::write(uint8_t *at, const void *bytes, size_t size)
{
	std::memcpy(alias(at), bytes, size);
	vm.code_written(alias(at), size);
}

void VmCodeCache::read(const uint8_t *at, void *bytes, size_t size) const
{
	std::memcpy(bytes, alias(at), size);
}

translate::CodeAllocator::Data VmCodeCache::allocate_data(size_t size)
{
	size = (size + 15) & ~size_t(15);
	size_t start;
	if (const auto reused = data_free.find(size); reused != data_free.end()) {
		start = reused->second;
		data_free.erase(reused);
		std::memset(data_alias + start, 0, size);
	} else {
		if (size > data_size - data_next)
			throw std::runtime_error("the VM's runtime data is full");
		start = data_next;
		data_next += size;
	}
	data_used[start] = size;
	return {data_alias + start, data_base + start};
}

void VmCodeCache::free_data(Data data)
{
	const auto used = data_used.find(data.address - data_base);
	if (used == data_used.end())
		return;
	data_free.emplace(used->second, used->first);
	data_used.erase(used);
}

bool VmCodeCache::contains(const uint8_t *address) const
{
	const uint64_t at = reinterpret_cast<uint64_t>(address);
	return at >= code_base && at < code_base + code_size;
}

}
