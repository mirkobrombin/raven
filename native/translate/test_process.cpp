#include "test_process.h"

#include "pe/pe.h"
#include "runtime.h"

#include <csignal>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>

namespace raven::translate {

namespace {

TestProcess *active = nullptr;
struct sigaction previous_segv {}, previous_bus {};

size_t host_page()
{
	return static_cast<size_t>(getpagesize());
}

void on_fault(int signal, siginfo_t *info, void *context)
{
	if (active && active->code_write(reinterpret_cast<uint64_t>(info->si_addr)))
		return;
	const struct sigaction &previous = signal == SIGBUS ? previous_bus : previous_segv;
	if (previous.sa_flags & SA_SIGINFO && previous.sa_sigaction) {
		previous.sa_sigaction(signal, info, context);
		return;
	}
	std::signal(signal, SIG_DFL);
	std::raise(signal);
}

uint32_t read32(const uint8_t *at)
{
	uint32_t value;
	std::memcpy(&value, at, 4);
	return value;
}

}

std::vector<std::pair<uint64_t, uint64_t>> lay_out_image(const PEImage &pe, std::span<const uint8_t> file, uint8_t *bytes, uint64_t base)
{
	std::vector<std::pair<uint64_t, uint64_t>> code_ranges;
	std::memcpy(bytes, file.data(), std::min<size_t>(pe.headers_size, file.size()));
	for (const PESection &section : pe.sections) {
		const uint32_t size = std::min(section.file_size, section.virtual_size ? section.virtual_size : section.file_size);
		if (size) {
			const auto data = pe.file_span(section.address, size);
			std::memcpy(bytes + section.address, data.data(), data.size());
		}
		if (section.characteristics & 0x20000000u)
			code_ranges.emplace_back(base + section.address, base + section.address + section.virtual_size);
	}
	return code_ranges;
}

TestProcess::TestProcess()
{
	if (active)
		throw std::logic_error("one test process at a time");
	active = this;
	struct sigaction action {};
	action.sa_sigaction = on_fault;
	action.sa_flags = SA_SIGINFO;
	sigemptyset(&action.sa_mask);
	sigaction(SIGSEGV, &action, &previous_segv);
	sigaction(SIGBUS, &action, &previous_bus);
}

TestProcess::~TestProcess()
{
	sigaction(SIGSEGV, &previous_segv, nullptr);
	sigaction(SIGBUS, &previous_bus, nullptr);
	for (const Image &image : images)
		munmap(reinterpret_cast<void *>(image.base), image.size);
	if (stack)
		munmap(stack, stack_size);
	if (teb)
		munmap(teb, host_page());
	active = nullptr;
}

uint64_t TestProcess::load(const std::filesystem::path &path)
{
	std::ifstream input(path, std::ios::binary);
	if (!input)
		throw std::runtime_error("cannot open " + path.string());
	Image image;
	image.file.assign(std::istreambuf_iterator<char>(input), {});
	const PEImage pe(image.file);
	if (!pe.is_64 || pe.machine != 0x8664)
		throw std::runtime_error(path.string() + " is not an x64 image");
	const size_t page = host_page();
	image.size = (uint64_t(pe.image_size) + page - 1) & ~uint64_t(page - 1);
	// A hint, never MAP_FIXED: on macOS the preferred base may hold the shared cache, and MAP_FIXED would replace it.
	void *memory = mmap(reinterpret_cast<void *>(pe.image_base), image.size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (memory == MAP_FAILED)
		throw std::runtime_error("cannot map " + path.string());
	image.base = reinterpret_cast<uint64_t>(memory);
	auto *bytes = static_cast<uint8_t *>(memory);
	image.code_ranges = lay_out_image(pe, image.file, bytes, image.base);
	if (image.base != pe.image_base) {
		// IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE, at offset 70 of the optional header: the image may move, and its base relocations, if
		// any, say what to fix. IMAGE_FILE_RELOCS_STRIPPED says it cannot.
		uint16_t dll_characteristics;
		std::memcpy(&dll_characteristics, image.file.data() + read32(image.file.data() + 0x3c) + 24 + 70, 2);
		if (!(dll_characteristics & 0x40) || (pe.characteristics & 1)) {
			munmap(memory, image.size);
			throw std::runtime_error(path.string() + " cannot move from its taken base");
		}
		const PEDirectory relocations = pe.directories[5];
		const int64_t delta = static_cast<int64_t>(image.base - pe.image_base);
		for (uint32_t at = 0; at + 8 <= relocations.size;) {
			const uint8_t *block = bytes + relocations.address + at;
			const uint32_t page_rva = read32(block), block_size = read32(block + 4);
			if (block_size < 8)
				break;
			for (uint32_t entry = 8; entry + 2 <= block_size; entry += 2) {
				uint16_t value;
				std::memcpy(&value, block + entry, 2);
				if (value >> 12 == 10) {
					uint64_t target;
					std::memcpy(&target, bytes + page_rva + (value & 0xfff), 8);
					target += static_cast<uint64_t>(delta);
					std::memcpy(bytes + page_rva + (value & 0xfff), &target, 8);
				} else if (value >> 12 != 0)
					throw std::runtime_error("unsupported base relocation");
			}
			at += block_size;
		}
	}
	image.entry = pe.entry;
	images.push_back(std::move(image));
	return images.back().base;
}

void TestProcess::bind_imports(const std::map<std::string, Native> &table)
{
	for (const Image &image : images) {
		const PEImage pe(image.file);
		for (const PEImport &import : pe.imports()) {
			const auto found = table.find(import.name);
			if (found == table.end())
				throw std::runtime_error("no native function for " + import.module + "!" + import.name);
			const uint64_t address = reinterpret_cast<uint64_t>(found->second.function);
			std::memcpy(reinterpret_cast<void *>(image.base + import.iat_address), &address, 8);
			natives[address] = found->second;
			runtime->bind_native(address, found->second.function, found->second.integer_arguments);
		}
	}
}

void TestProcess::attach(Runtime &attached)
{
	runtime = &attached;
}

uint64_t TestProcess::entry(uint64_t base) const
{
	for (const Image &image : images)
		if (image.base == base)
			return image.base + image.entry;
	throw std::runtime_error("no image at that base");
}

const std::vector<uint8_t> &TestProcess::file(uint64_t base) const
{
	for (const Image &image : images)
		if (image.base == base)
			return image.file;
	throw std::runtime_error("no image at that base");
}

void TestProcess::prepare_thread()
{
	if (!stack) {
		stack_size = 1 << 20;
		void *memory = mmap(nullptr, stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
		void *block = mmap(nullptr, host_page(), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
		if (memory == MAP_FAILED || block == MAP_FAILED)
			throw std::runtime_error("cannot map the guest stack");
		stack = static_cast<uint8_t *>(memory);
		teb = static_cast<uint8_t *>(block);
		// NT_TIB: stack base and limit, and the TEB's own address, which gs:[0x30] reads.
		const uint64_t top = reinterpret_cast<uint64_t>(stack + stack_size), bottom = reinterpret_cast<uint64_t>(stack);
		const uint64_t self = reinterpret_cast<uint64_t>(teb);
		std::memcpy(teb + 0x8, &top, 8);
		std::memcpy(teb + 0x10, &bottom, 8);
		std::memcpy(teb + 0x30, &self, 8);
	}
	CpuState &state = runtime->state();
	state.gpr[rsp] = reinterpret_cast<uint64_t>(stack + stack_size);
	state.gs_base = reinterpret_cast<uint64_t>(teb);
}

std::span<const uint8_t> TestProcess::code(uint64_t address) const
{
	for (const Image &image : images)
		for (const auto &[start, end] : image.code_ranges)
			if (address >= start && address < end)
				return {reinterpret_cast<const uint8_t *>(address), static_cast<size_t>(end - address)};
	return {};
}

bool TestProcess::is_native(uint64_t address) const
{
	return natives.count(address) != 0;
}

void TestProcess::write(uint64_t address, const void *bytes, size_t size)
{
	std::memcpy(reinterpret_cast<void *>(address), bytes, size);
}

void TestProcess::read(uint64_t address, void *bytes, size_t size) const
{
	std::memcpy(bytes, reinterpret_cast<const void *>(address), size);
}

void TestProcess::watch_code(uint64_t address, size_t size, bool watch)
{
	const uint64_t page = host_page();
	for (uint64_t at = address & ~(page - 1); at < address + size; at += page) {
		unsigned &count = watched[at];
		if (watch && count++ == 0)
			mprotect(reinterpret_cast<void *>(at), page, PROT_READ);
		else if (!watch && count && --count == 0) {
			mprotect(reinterpret_cast<void *>(at), page, PROT_READ | PROT_WRITE);
			watched.erase(at);
		}
	}
}

size_t TestProcess::watch_granule() const
{
	return host_page();
}

bool TestProcess::code_write(uint64_t address)
{
	const uint64_t page = address & ~uint64_t(host_page() - 1);
	const auto found = watched.find(page);
	if (found == watched.end())
		return false;
	runtime->code_written(address);
	// Every translation of the page is gone, so nothing watches it now; make sure the write can go through.
	if (watched.erase(page))
		mprotect(reinterpret_cast<void *>(page), host_page(), PROT_READ | PROT_WRITE);
	return true;
}

}
