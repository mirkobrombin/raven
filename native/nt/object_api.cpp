#include "object_api.h"
#include "threads.h"

#include <array>
#include <new>
#include <stdexcept>

namespace raven {

NativeObjectAPI::NativeObjectAPI(NativeProcess &process, NativeEnvironment &environment, NativeObjects &objects,
	std::chrono::steady_clock::time_point deadline)
	: process(process), environment(environment), objects(objects), deadline(deadline) {}

std::span<const std::string_view> NativeObjectAPI::nt_exports()
{
	static constexpr std::array<std::string_view,6> names{
		"NtClose","NtCreateEvent","NtQueryEvent","NtResetEvent","NtSetEvent","NtWaitForSingleObject"};
	return names;
}

static void put(std::span<uint8_t> bytes, size_t at, uint64_t value, size_t width)
{
	for (size_t byte=0; byte<width; ++byte) bytes[at+byte]=uint8_t(value>>(byte*8));
}

static uint32_t error(uint32_t status)
{
	if (status==0xc0000008U) return 6;
	if (status==0xc0000024U) return 6;
	if (status==0xc0000022U) return 5;
	if (status==0xc0000005U) return 998;
	if (status==0xc00000bbU) return 120;
	if (status==0xc000009aU) return 8;
	return 87;
}

void NativeObjectAPI::register_served(ApiRegistry &registry, NativeObjects &objects)
{
	registry.add("CloseHandle", 1, [&objects](ApiCall &call) {
		const uint32_t status=objects.close(call[0]);
		if (status) call.fail(error(status), 0);
		else call.ret(1);
	});
	for (const bool set : {true, false})
		registry.add(set ? "SetEvent" : "ResetEvent", 1, [&objects, set](ApiCall &call) {
			const uint32_t status=objects.set_event(call[0], set).status;
			if (status) call.fail(error(status), 0);
			else call.ret(1);
		});
}

bool NativeObjectAPI::dispatch(std::string_view name)
{
	if (file_io(name)) return true;
	const bool nt=name.starts_with("Nt");
	const bool create=name=="NtCreateEvent" || name=="CreateEventA" || name=="CreateEventW";
	// An event with a name, whether made or opened, is a shared object that the registry handlers keep.
	if (!nt && create && process.api_arguments(4)[3]) return false;
	const bool close=name=="NtClose" || name=="CloseHandle" || name=="_lclose" || name=="LZClose";
	const bool set=name=="NtSetEvent" || name=="SetEvent" || name=="NtResetEvent" || name=="ResetEvent";
	const bool query=name=="NtQueryEvent";
	// The native waits are the scheduler's (thread_api), which also runs APCs for an alertable one.
	const bool wait=!cooperative && name=="WaitForSingleObject";
	if (!create && !close && !set && !query && !wait) return false;
	const auto args=process.api_arguments(create ? (nt ? 5 : 4) : close ? 1 : set ? (nt ? 2 : 1) : query ? 5 : 2);
	uint32_t status=0;
	uint64_t result=0;
	const auto writable=[&](uint64_t address,size_t size) {
		const auto previous=process.read_memory(address,size); process.write_memory(address,previous);
	};
	try {
		if (create) {
			if (nt) writable(args[0],8);
			// OBJECT_ATTRIBUTES without a name (the Cygwin runtime passes them for inheritance and security) make an unnamed event,
			// inheritable with OBJ_INHERIT; a named native event is not made here.
			bool named=false,inherit=false;
			if (nt && args[2]) {
				const auto attributes=process.read_memory(args[2],32);
				for (size_t byte=16; byte<24; ++byte) named|=attributes[byte]!=0;
				inherit=attributes[24] & 2;
			}
			if (named) status=0xc00000bbU;
			else {
				const auto made=objects.create_event(nt ? uint32_t(args[1]) : 0x1f0003,
					nt ? uint32_t(args[3]) : uint32_t(args[1]) ? 0 : 1,nt ? uint8_t(args[4])!=0 : uint32_t(args[2])!=0);
				status=made.status; result=made.value;
				if (!status && inherit) objects.set_inherit(result,true);
				if (nt && !status) {
					std::array<uint8_t,8> output{}; put(output,0,result,8);
					try { process.write_memory(args[0],output); }
					catch (...) { objects.close(result); throw; }
				}
			}
		} else if (close) {
			status=objects.close(args[0]); result=1;
		} else if (set) {
			if (nt && args[1]) writable(args[1],4);
			const auto changed=objects.set_event(args[0],name=="NtSetEvent" || name=="SetEvent");
			status=changed.status; result=1;
			if (nt && args[1] && !status) { std::array<uint8_t,4> output{}; put(output,0,changed.value,4); process.write_memory(args[1],output); }
		} else if (query) {
			if (uint32_t(args[1])!=0) status=0xc00000bbU;
			else {
				if (args[4]) { std::array<uint8_t,4> length{}; put(length,0,8,4); process.write_memory(args[4],length); }
				if (uint32_t(args[3])<8) status=0xc0000004U;
				else {
					const auto state=objects.query_event(args[0]); status=state.status;
					if (!status) { std::array<uint8_t,8> output{}; put(output,0,state.value,8); process.write_memory(args[2],output); }
				}
			}
		} else {
			std::optional<int64_t> timeout;
			if (uint32_t(args[1])!=UINT32_MAX) timeout=-int64_t(uint32_t(args[1]))*10000;
			const auto waited=objects.wait(args[0],timeout,deadline);
			if (waited.runtime_timeout) throw NativeResult{NativeStop::timeout,0,process.context().registers.rip,0};
			status=waited.status;
		}
	} catch (const std::bad_alloc &) { status=0xc000009aU; }
	catch (const std::runtime_error &failure) {
		const std::string_view message=failure.what();
		if (message.find("readable")==message.npos && message.find("writable")==message.npos) throw;
		status=0xc0000005U;
	}
	if (!nt && status && !(wait && status==0x102)) environment.set_last_error(error(status));
	if (name=="_lclose" || name=="LZClose") { process.return_api(status && name=="_lclose" ? 0xffffffffU : 0U); return true; }
	process.return_api(nt ? status : wait ? status==0 ? 0 : status==0x102 ? 0x102 : UINT32_MAX : status ? 0 : result);
	return true;
}

}
