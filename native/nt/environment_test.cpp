#include "environment.h"
#include "providers.h"
#include "module_api.h"
#include "loader.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <sys/mman.h>
#include <thread>

static std::vector<uint8_t> read_file(const char *path)
{
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file || file.tellg() <= 0 || file.tellg() > 1024 * 1024)
		throw std::runtime_error("cannot read environment fixture");
	std::vector<uint8_t> bytes(static_cast<size_t>(file.tellg()));
	file.seekg(0);
	if (!file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
		throw std::runtime_error("truncated environment fixture");
	return bytes;
}

static void check(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

int main(int argc, char **argv)
{
	try {
		if (argc != 4)
			throw std::runtime_error("bootstrap, client and environment probe required");
		raven::ModuleSet modules;
		raven::PEModule *entry = nullptr;
		for (int index = 2; index < argc; ++index) {
			auto bytes = read_file(argv[index]);
			const auto base = raven::PEImage(bytes).image_base;
			auto &module = modules.add(std::filesystem::path(argv[index]).filename().string(), std::move(bytes), base);
			if (!entry)
				entry = &module;
		}
		check(modules.bind_imports() == 4, "environment fixture import count differs");
		std::vector<raven::LoadedImage *> images;
		for (const auto &[name, module] : modules.all())
			images.push_back(&module->loaded);
		raven::NativeProcess process(images, argv[1], std::chrono::seconds(2));
		raven::NativeEnvironment environment(process, entry->loaded.base(), 73, 91);
		const auto loader = raven::initialize_loader_data(process, environment.peb(), modules, *entry);
		check(loader != 0, "loader graph was not published");
		bool loader_rejected = false;
		try { raven::initialize_loader_data(process, environment.peb(), modules, *entry); }
		catch (const std::runtime_error &) { loader_rejected = true; }
		check(loader_rejected, "duplicate loader graph replaced the original");
		const auto wrong_peb = process.map_memory(4096, PROT_READ | PROT_WRITE);
		loader_rejected = false;
		try { raven::initialize_loader_data(process, wrong_peb, modules, *entry); }
		catch (const std::runtime_error &) { loader_rejected = true; }
		check(loader_rejected && process.read_memory(wrong_peb + 0x18, 8) == std::vector<uint8_t>(8),
			"mismatched PEB received a loader graph");
		process.unmap_memory(wrong_peb);
		// KUSER_SHARED_DATA: Windows 10.0.19045, a workstation, read-only to the program.
		const auto shared = process.read_memory(0x7ffe0260, 20);
		check(shared[0] == 0x65 && shared[1] == 0x4a && shared[4] == 1 && shared[12] == 10 && shared[16] == 0 &&
			process.has_protection(0x7ffe0000, 4096, PROT_READ), "shared user data does not name Windows 10 22H2");
		// Each host thread names the guest thread it works for: another host thread that names its own changes nothing here.
		{
			const auto first = environment.teb();
			const auto second = environment.create_thread_block(0x7000000, 0x6ff0000, 92);
			uint64_t named = 0;
			uint32_t named_id = 0;
			std::thread([&] {
				environment.set_current(second);
				named = environment.teb();
				named_id = environment.thread_id();
			}).join();
			check(named == second && named_id == 92, "a host thread did not work for the thread it named");
			check(environment.teb() == first && environment.thread_id() == 91, "another host thread changed the thread this one works for");
			environment.release_thread_block(second);
		}
		check(environment.last_error() == 0, "initial LastError is not zero");
		environment.set_last_error(55);
		const auto heap_list=environment.prepare_heaps();
		check(heap_list && !environment.process_heap() && process.read_memory(heap_list,4096)==std::vector<uint8_t>(4096),
			"initial process heap list is not owned zeroed storage");
		bool heap_rejected=false;
		try { environment.prepare_heaps(); } catch (const std::runtime_error &) { heap_rejected=true; }
		check(heap_rejected,"duplicate heap storage replaced the owner");
		heap_rejected=false;
		try { environment.publish_process_heap(heap_list); } catch (const std::runtime_error &) { heap_rejected=true; }
		check(heap_rejected && !environment.process_heap(),"unregistered heap was published");
		check(environment.last_error() == 55, "host LastError state was not retained");
		check(!environment.dispatch("NotImplemented"), "unknown API returned apparent success");
		bool rejected = false;
		try { environment.dispatch("GetLastError"); } catch (const std::runtime_error &) { rejected = true; }
		check(rejected, "API dispatch outside a pending call was accepted");
		rejected = false;
		try { raven::NativeEnvironment duplicate(process, entry->loaded.base(), 73, 91); }
		catch (const std::runtime_error &) { rejected = true; }
		check(rejected, "duplicate thread environment was accepted");
		auto saved = process.context();
		auto wrong = saved;
		wrong.registers.gs_base = 0;
		process.set_context(wrong);
		rejected = false;
		try { environment.set_last_error(99); } catch (const std::runtime_error &) { rejected = true; }
		process.set_context(saved);
		check(rejected && environment.last_error() == 55, "wrong-thread LastError write was accepted");
		std::map<uint64_t, std::string> traps;
		for (const char *name : {"GetLastError", "SetLastError", "GetCurrentProcessId", "GetCurrentThreadId"}) {
			const auto address = modules.resolve({"kernel32.dll", name, 0, false, 0});
			process.add_api_trap(address);
			traps.emplace(address, name);
		}
		auto result = process.call(entry->loaded.base() + entry->image.entry, {}, std::chrono::seconds(2));
		size_t calls = 0;
		while (result.stop == raven::NativeStop::api) {
			check(++calls <= 32, "environment fixture exceeded API limit");
			const auto found = traps.find(result.value);
			check(found != traps.end() && environment.dispatch(found->second), "unrecognized environment API stop");
			result = process.resume(std::chrono::seconds(2));
		}
		check(result.stop == raven::NativeStop::returned && result.value == 0 && calls == 8,
			"compiled TEB/PEB and LastError fixture failed");
		check(environment.last_error() == 0, "compiled final LastError differs");
		raven::NativeProcess other(images, argv[1], std::chrono::seconds(2));
		raven::NativeEnvironment isolated(other, entry->loaded.base(), 101, 103);
		isolated.set_last_error(123);
		check(isolated.last_error() == 123 && environment.last_error() == 0, "independent guest LastError states interfered");
		raven::NativeModuleAPI module_api(process, environment, modules, *entry);
		const auto proxy = modules.resolve({"kernel32.dll", "GetLastError", 0, false, 0});
		const auto text = process.map_memory(4096, PROT_READ | PROT_WRITE);
		const auto invoke = [&](std::string_view name, std::span<const uint64_t> arguments) {
			check(process.call(proxy, arguments, std::chrono::seconds(2)).stop == raven::NativeStop::api, "module API test did not stop");
			check(module_api.dispatch(name), "module API dispatch was not recognized");
			return process.resume(std::chrono::seconds(2)).value;
		};
		const uint64_t null_name[] = {0};
		environment.set_last_error(77);
		check(invoke("GetModuleHandleA", null_name) == entry->loaded.base() && environment.last_error() == 77,
			"null module lookup or LastError preservation differs");
		const uint8_t kernel_name[] = {'K','E','R','N','E','L','3','2',0};
		process.write_memory(text, kernel_name);
		const uint64_t name_pointer[] = {text};
		const auto kernel = modules.all().at("kernel32.dll")->loaded.base();
		check(invoke("GetModuleHandleA", name_pointer) == kernel, "case-insensitive default DLL extension lookup differs");
		const uint8_t export_name[] = {'G','e','t','L','a','s','t','E','r','r','o','r',0};
		process.write_memory(text, export_name);
		const uint64_t procedure_name[] = {kernel, text};
		check(invoke("GetProcAddress", procedure_name) == proxy, "dynamic named export lookup differs");
		const uint64_t ordinal[] = {kernel, 1};
		check(invoke("GetProcAddress", ordinal) == proxy, "dynamic ordinal export lookup differs");
		const uint8_t missing[] = {'m','i','s','s','i','n','g',0};
		process.write_memory(text, missing);
		check(invoke("GetModuleHandleA", name_pointer) == 0 && environment.last_error() == 126, "missing module error differs");
		check(invoke("GetProcAddress", procedure_name) == 0 && environment.last_error() == 127, "missing export error differs");
		const uint8_t unicode_name[] = {0xe9, 0, 0, 0};
		process.write_memory(text, unicode_name);
		check(invoke("GetModuleHandleW", name_pointer) == 0 && environment.last_error() == 120, "unsupported Unicode lookup reported success");
		const uint8_t path_name[] = {'C',':','\\','k','e','r','n','e','l','3','2','.','d','l','l',0};
		process.write_memory(text, path_name);
		check(invoke("GetModuleHandleA", name_pointer) == 0 && environment.last_error() == 120, "unsupported path lookup matched a basename");
		const uint8_t trailing_dot[] = {'k','e','r','n','e','l','3','2','.',0};
		process.write_memory(text, trailing_dot);
		check(invoke("GetModuleHandleA", name_pointer) == 0 && environment.last_error() == 126, "trailing dot incorrectly appended DLL extension");
		const uint64_t invalid_pointer[] = {UINT64_MAX};
		check(process.call(proxy, invalid_pointer, std::chrono::seconds(2)).stop == raven::NativeStop::api, "invalid pointer test did not stop");
		rejected = false;
		try { module_api.dispatch("GetModuleHandleW"); } catch (const std::runtime_error &) { rejected = true; }
		check(rejected, "module API accepted unreadable guest memory");
		process.return_api(0);
		process.resume(std::chrono::seconds(2));
		const auto system=[&](std::initializer_list<uint64_t> args) {
			check(process.call(proxy,std::span(args.begin(),args.size()),std::chrono::seconds(1)).stop==raven::NativeStop::api &&
				environment.dispatch("NtQuerySystemInformation"),"system information did not dispatch");
			const auto result=process.resume(std::chrono::seconds(1));
			check(result.stop==raven::NativeStop::returned,"system information did not return");
			return result.value;
		};
		environment.set_last_error(71);
		check(system({0,text,64,text+128})==0 && environment.last_error()==71 && process.read_memory(text+128,4)[0]==64,
			"basic system information length or LastError differs");
		const auto basic=process.read_memory(text,64);
		check(basic[8]==0 && basic[9]==16 && basic[24]==0 && basic[25]==0 && basic[26]==1 && basic[56] && basic[56]<=64,
			"basic system geometry or processor group differs");
		check(system({0,text,63,text+128})==0xc0000004U && system({0,text,65,0})==0xc0000004U &&
			system({1,text,64,0})==0xc0000002U && process.read_memory(text,64)==basic,"unsupported system query changed output");
		check(system({0,0x400000000000ULL,64,0})==0xc0000005U &&
			system({0,text,64,0x400000000000ULL})==0xc0000005U && process.read_memory(text,64)==basic,
			"bad system query output succeeded or changed data");
		process.protect_memory(text,4096,PROT_READ);
		check(system({0,text,64,0})==0xc0000005U && environment.last_error()==71,"readonly system query succeeded");
		process.protect_memory(text,4096,PROT_READ | PROT_WRITE);
		process.unmap_memory(text);
		raven::ModuleSet runtime;
		const std::span<const uint8_t> entry_bytes = entry->bytes;
		auto &runtime_entry = runtime.add(entry->name, std::vector<uint8_t>(entry_bytes.begin(), entry_bytes.end()), entry->loaded.base());
		for (size_t run = 0; run < 2; ++run) {
			const auto executed = raven::run_native_process(runtime, runtime_entry, argv[1], std::chrono::seconds(2));
			check(executed.stop == raven::NativeStop::returned && executed.value == 0,
				"runtime-owned API providers did not survive module graph reuse");
		}
		std::cout << "Compiled PE64 GS/TEB/PEB, four environment APIs, guest LastError writes and independent guest state passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
