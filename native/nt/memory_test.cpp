#include "loader.h"
#include "memory.h"
#include "providers.h"

#include <algorithm>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <sys/mman.h>

static void check(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

static void rejected(const std::function<void()> &operation, const char *message)
{
	std::string failure;
	try { operation(); } catch (const std::runtime_error &error) { failure=error.what(); }
	check(failure.find(message)!=failure.npos,"memory negative failed for another reason");
}

int main(int argc, char **argv)
{
	try {
		check(argc==2,"bootstrap path required");
		raven::ModuleSet modules;
		auto &entry=modules.add("entry.exe",raven::environment_provider("kernel32.dll",0x200000000ULL));
		const auto service=modules.resolve({"entry.exe","VirtualAlloc",0,false,0});
		const std::array<uint8_t,11> stub{0xb8,39,0,0,0,0x4c,0x8b,0xd1,0x0f,0x05,0xc3};
		std::copy(stub.begin(),stub.end(),entry.loaded.data().begin()+static_cast<ptrdiff_t>(service-entry.loaded.base()));
		raven::LoadedImage *images[]{&entry.loaded};
		raven::NativeProcess process(images,argv[1],std::chrono::seconds(2));
		raven::NativeEnvironment environment(process,entry.loaded.base(),1,1);
		raven::NativeMemory memory(process,environment,16384);
		rejected([&] { process.enter_nt_service(0,0); },"pending syscall");
		const auto reserved=memory.allocate(0,8192,0x2000,4);
		check(!reserved.status && reserved.address!=entry.loaded.base() && !(reserved.address%65536) &&
			reserved.size==8192 && memory.bytes()==8192,"reservation overlap, alignment or accounting differs");
		rejected([&] { process.read_memory(reserved.address,1); },"readable");
		check(memory.allocate(reserved.address,4096,0x2000,4).status==0xc0000018U,"reservation collision was accepted");
		check(memory.protect(reserved.address,4096,4).status==0xc000002dU,"reserved pages acquired protection without commit");
		const auto committed=memory.allocate(reserved.address+4101,1,0x1000,4);
		check(!committed.status && committed.address==reserved.address+4096 && committed.size==4096 &&
			process.read_memory(committed.address,1)==std::vector<uint8_t>{0},"partial commit geometry or zero data differs");
		process.write_memory(committed.address,std::array<uint8_t,1>{91});
		check(!memory.allocate(committed.address,1,0x1000,4).status && process.read_memory(committed.address,1)[0]==91,
			"repeated commit lost live data");
		const auto reset=memory.allocate(committed.address,4096,0x80000,1);
		check(!reset.status && reset.address==committed.address && reset.size==4096 && memory.bytes()==8192 &&
			!memory.allocate(committed.address,4096,0x1000000,4).status,"reset of committed pages failed or changed accounting");
		check(memory.allocate(reserved.address,8192,0x80000,4).status==0xc0000018U && memory.allocate(0,4096,0x80000,4).status &&
			memory.allocate(committed.address,4096,0x81000,4).status,"reset of reserved pages, of no address or with a commit was accepted");
		check(!memory.protect(committed.address+1,1,2).status,"readonly page protection failed");
		rejected([&] { process.write_memory(committed.address,std::array<uint8_t,1>{0}); },"writable");
		check(!memory.free(committed.address+7,1,0x4000).status,"decommit failed");
		rejected([&] { process.read_memory(committed.address,1); },"readable");
		check(!memory.allocate(committed.address,4096,0x1000,4).status && process.read_memory(committed.address,1)[0]==0,
			"recommit retained discarded bytes");
		check(memory.free(reserved.address,1,0x8000).status && memory.free(reserved.address+4096,0,0x8000).status &&
			memory.count()==1,"invalid release removed a reservation");
		check(!memory.free(reserved.address,0,0x4000).status && !memory.free(reserved.address,0,0x8000).status &&
			!memory.count() && !memory.bytes(),"whole decommit/release leaked state");
		check(memory.allocate(0,20000,0x3000,4).status==0xc0000017U && !memory.count(),"reservation limit created state");
		check(memory.allocate(0,0,0x3000,4).status && memory.allocate(UINT64_MAX,4096,0x3000,4).status &&
			memory.allocate(0,4096,0x403000,4).status && memory.allocate(0,4096,0x3000,0x104).status && !memory.count(),
			"invalid geometry or unsupported modifiers succeeded");
		// MEM_TOP_DOWN only states a preference for high addresses, which any free address satisfies.
		const auto top_down=memory.allocate(0,4096,0x103000,4);
		check(!top_down.status && !memory.free(top_down.address,0,0x8000).status && !memory.count(),"top-down allocation failed");
		const auto code=memory.allocate(0,4096,0x3000,4);
		check(!code.status,"executable test allocation failed");
		process.write_memory(code.address,std::array<uint8_t,6>{0xb8,42,0,0,0,0xc3});
		check(!memory.protect(code.address,4096,0x20).status,"execute-read protection failed");
		const auto executed=process.call(code.address,{},std::chrono::seconds(1));
		check(executed.stop==raven::NativeStop::returned && executed.value==42,"committed native instructions did not execute");
		check(!memory.free(code.address,0,0x8000).status,"executable allocation did not release");
		const auto proxy=modules.resolve({"entry.exe","GetLastError",0,false,0});
		process.add_api_trap(proxy);
		const auto api=[&](std::string_view name,std::initializer_list<uint64_t> args) {
			check(process.call(proxy,std::span(args.begin(),args.size()),std::chrono::seconds(1)).stop==raven::NativeStop::api &&
				memory.dispatch(name),"memory API did not dispatch");
			const auto result=process.resume(std::chrono::seconds(1));
			check(result.stop==raven::NativeStop::returned,"memory API did not return");
			return result.value;
		};
		const auto output=process.map_memory(4096,PROT_READ | PROT_WRITE);
		check(api("NtAllocateVirtualMemory",{1,output,0,output+8,0x3000,4})==0xc0000008U && !memory.count(),
			"foreign process memory operation succeeded");
		check(api("NtAllocateVirtualMemory",{UINT64_MAX,0,0,output+8,0x3000,4})==0xc0000005U && !memory.count(),
			"bad NT output pointer succeeded");
		process.protect_memory(output,4096,PROT_READ);
		check(!api("VirtualProtect",{entry.loaded.base(),4096,4,output}) && environment.last_error()==998 && !memory.count(),
			"readonly Win32 output modified memory state");
		process.protect_memory(output,4096,PROT_READ | PROT_WRITE);
		std::array<uint8_t,16> pointers{};
		pointers[8]=1;
		process.write_memory(output,pointers);
		const std::array<uint64_t,6> arguments{UINT64_MAX,output,0,output+8,0x3000,4};
		const auto stopped=process.call(service,arguments,std::chrono::seconds(1));
		check(stopped.stop==raven::NativeStop::syscall && stopped.value==39 && stopped.instruction==service+10,
			"verified service stub did not stop before Linux syscall execution");
		rejected([&] { process.enter_nt_service(stopped.instruction,40); },"does not match");
		process.enter_nt_service(stopped.instruction,39);
		check(process.api_arguments(6)==std::vector<uint64_t>(arguments.begin(),arguments.end()) &&
			memory.dispatch("NtAllocateVirtualMemory"),"NT syscall argument recovery differs");
		const auto returned=process.resume(std::chrono::seconds(1));
		check(returned.stop==raven::NativeStop::returned && returned.value==0 && memory.count()==1,
			"NT syscall service did not return to its caller");
		const auto bytes=process.read_memory(output,8);
		uint64_t allocated=0;
		for (size_t byte=0; byte<8; ++byte) allocated|=uint64_t(bytes[byte])<<(byte*8);
		process.write_memory(allocated,bytes);
		process.write_memory(output+8,std::array<uint8_t,8>{});
		check(api("NtFreeVirtualMemory",{UINT64_MAX,allocated,output+8,0x8000})==0xc00000bbU && memory.count()==1 &&
			process.read_memory(allocated,8)==bytes,"release output alias removed its own parameter storage");
		check(allocated && !memory.free(allocated,0,0x8000).status && !memory.count(),"NT syscall allocation was not owned or released");
		// The loader registers every module it maps, which VirtualQuery then reports as image memory based at the module.
		raven::register_images(memory,modules);
		const auto read_quad=[&](uint64_t at) {
			const auto value=process.read_memory(at,8);
			uint64_t result=0;
			for (size_t byte=0; byte<8; ++byte) result|=uint64_t(value[byte])<<(byte*8);
			return result;
		};
		check(api("VirtualQuery",{entry.loaded.base(),output,48})==48 && read_quad(output+8)==entry.loaded.base() &&
			uint32_t(read_quad(output+40))==0x1000000,"a mapped image was not reported as image memory");
		check(!memory.dispatch("Unimplemented"),"unimplemented memory function succeeded");
		rejected([&] { process.complete_debug_print(0); },"INT 2D");
		const auto debug=process.map_memory(4096,PROT_READ | PROT_WRITE);
		process.write_memory(debug,std::array<uint8_t,9>{0xb8,1,0,0,0,0xcd,0x2d,0xcc,0xc3});
		process.protect_memory(debug,4096,PROT_READ | PROT_EXEC);
		process.write_memory(output+128,std::array<uint8_t,6>{'t','r','a','c','e','\n'});
		const auto print=process.call(debug,std::array<uint64_t,2>{output+128,6},std::chrono::seconds(1));
		check(print.stop==raven::NativeStop::fault && print.instruction==debug+5 && print.value==1,
			"debug print service did not produce its captured fault");
		rejected([&] { process.complete_debug_print(print.instruction+1); },"INT 2D");
		const auto context=process.context().registers;
		check(process.read_memory(context.rcx,size_t(uint32_t(context.rdx)))==std::vector<uint8_t>{'t','r','a','c','e','\n'},
			"debug print payload capture differs");
		process.complete_debug_print(print.instruction);
		const auto completed=process.resume(std::chrono::seconds(1));
		check(completed.stop==raven::NativeStop::returned && completed.value==0,"debug print did not return to its caller");
		process.unmap_memory(debug);
		std::cout << "Native reservation, commit, decommit, protection, query, executable memory and negative contracts passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
