#include "object_api.h"
#include "providers.h"
#include "event.h"
#include "mutex.h"

#include <array>
#include <future>
#include <iostream>
#include <stdexcept>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

static void check(bool condition, const char *message)
{
	if (!condition) throw std::runtime_error(message);
}

static void queued(raven::NativeObjects &objects, uint64_t handle, size_t count)
{
	const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
	while (objects.waiting(handle)!=count && std::chrono::steady_clock::now()<end) std::this_thread::yield();
	check(objects.waiting(handle)==count,"event waiters did not register");
}

int main(int argc, char **argv)
{
	try {
		check(argc==2,"bootstrap required");
		{
			auto event = std::make_shared<raven::EventObject>(1, true);
			raven::InheritedHandle record;
			check(event->describe(record) && record.kind == "event", "event descriptor transfer failed");
			record.descriptor = ::dup(record.descriptor);
			auto imported = raven::object_from_record(record);
			check(imported && imported->try_consume(1) && !event->try_consume(1), "event transfer copied its signaled state");
			for (unsigned index = 0; index < 100; ++index) {
				event->set_state(true);
				auto first = std::async(std::launch::async, [&] { return event->try_consume(1); });
				auto second = std::async(std::launch::async, [&] { return imported->try_consume(2); });
				check(unsigned(first.get()) + unsigned(second.get()) == 1, "shared auto-reset event had multiple consumers");
			}
			event.reset();
			check(imported->set_state(true) == 0 && imported->try_consume(1), "event died with the exporting owner");
			raven::NativeObjects receiver;
			record.handle = 0x1000;
			check(imported->describe(record), "event retransmission failed");
			record.descriptor = ::dup(record.descriptor);
			raven::adopt_inherited(receiver, {record});
			check(!receiver.access_of(record.handle) && receiver.set_event(record.handle,true).status == 0xc0000022U &&
				receiver.query_event(record.handle).status == 0xc0000022U && receiver.signaled(record.handle,1) == -3,
				"event transfer granted rights to a zero-access handle");
		}
		{
			auto mutex = std::make_shared<raven::MutexObject>();
			raven::InheritedHandle record;
			check(mutex->try_consume(7) && mutex->try_consume(7) && mutex->describe(record), "owned mutex transfer failed");
			record.descriptor = ::dup(record.descriptor);
			auto imported = std::dynamic_pointer_cast<raven::MutexObject>(raven::object_from_record(record));
			check(imported && !imported->try_consume(8) && !imported->release(8) && mutex->release(7) &&
				!imported->try_consume(8) && imported->release(7) && imported->try_consume(8), "mutex transfer lost ownership or recursion");
			mutex.reset();
			check(imported->release(8), "mutex died with exporting handle");
		}
		raven::NativeObjects objects;
		const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(4);
		const auto manual=objects.create_event(0x1f0003,0,false), automatic=objects.create_event(0x1f0003,1,true);
		check(!manual.status && !automatic.status && manual.value!=automatic.value,"event identity creation failed");
		check(objects.wait(manual.value,0,end).status==0x102 && !objects.wait(automatic.value,0,end).status &&
			objects.wait(automatic.value,0,end).status==0x102,"poll or auto-reset semantics differ");
		const auto restricted=objects.create_event(1,0,false);
		check(!restricted.status && objects.set_event(restricted.value,true).status==0xc0000022U &&
			objects.wait(restricted.value,0,end).status==0xc0000022U,"event access mask was bypassed");
		check(objects.create_event(0x1000000,0,false).status && objects.create_event(0,2,false).status,"unsupported event contract succeeded");
		auto first=std::async(std::launch::async,[&] { return objects.wait(manual.value,std::nullopt,end); });
		auto second=std::async(std::launch::async,[&] { return objects.wait(manual.value,std::nullopt,end); });
		queued(objects,manual.value,2);
		check(!objects.set_event(manual.value,true).status && !objects.set_event(manual.value,false).status,
			"manual signal/reset failed");
		const auto released_first=first.get(), released_second=second.get();
		check(!released_first.status && !released_first.runtime_timeout && !released_second.status && !released_second.runtime_timeout &&
			objects.wait(manual.value,0,end).status==0x102,
			"reset canceled already released manual waiters");
		auto waiting=std::async(std::launch::async,[&] { return objects.wait(automatic.value,std::nullopt,end); });
		queued(objects,automatic.value,1);
		objects.set_event(automatic.value,true);
		const auto released=waiting.get();
		check(!released.status && !released.runtime_timeout && objects.wait(automatic.value,0,end).status==0x102,
			"auto-reset delivered more than one acquisition");
		auto auto_first=std::async(std::launch::async,[&] { return objects.wait(automatic.value,std::nullopt,end); });
		auto auto_second=std::async(std::launch::async,[&] { return objects.wait(automatic.value,std::nullopt,end); });
		queued(objects,automatic.value,2);
		objects.set_event(automatic.value,true);
		check(objects.waiting(automatic.value)==1,"one auto-reset signal released multiple waiters");
		objects.set_event(automatic.value,true);
		const auto auto_released_first=auto_first.get(), auto_released_second=auto_second.get();
		check(!auto_released_first.status && !auto_released_first.runtime_timeout && !auto_released_second.status &&
			!auto_released_second.runtime_timeout && objects.wait(automatic.value,0,end).status==0x102,
			"two auto-reset signals did not release exactly two queued waits");
		const auto started=std::chrono::steady_clock::now();
		check(objects.wait(manual.value,-50000,end).status==0x102 && std::chrono::steady_clock::now()-started>=std::chrono::milliseconds(5),
			"relative event wait reported an early timeout");
		check(objects.wait(manual.value,std::nullopt,std::chrono::steady_clock::now()+std::chrono::milliseconds(2)).runtime_timeout,
			"runtime wait limit was reported as Windows success");
		check(!objects.close(manual.value) && objects.close(manual.value)==0xc0000008U &&
			objects.wait(manual.value,0,end).status==0xc0000008U && !objects.close(automatic.value) &&
			!objects.close(restricted.value) && !objects.count(),"event close/stale lifetime differs");
		raven::NativeObjects limited(1);
		const auto retained=limited.create_event(0,0,false);
		check(!retained.status && limited.create_event(0,0,false).status==0xc000009aU && limited.count()==1,"handle budget changed live state");
		limited.close(retained.value);
		check(limited.create_event(0,0,false).value!=retained.value,"closed handle identity was reused");
		raven::ModuleSet modules;
		auto &entry=modules.add("entry.exe",raven::environment_provider("kernel32.dll",0x180000000ULL));
		raven::LoadedImage *images[]{&entry.loaded};
		raven::NativeProcess process(images,argv[1],std::chrono::seconds(2));
		raven::NativeEnvironment environment(process,entry.loaded.base(),1,1);
		raven::NativeObjectAPI api(process,environment,objects,std::chrono::steady_clock::now()+std::chrono::seconds(2));
		const auto proxy=modules.resolve({"entry.exe","GetLastError",0,false,0}); process.add_api_trap(proxy);
		const auto invoke=[&](std::string_view name,std::initializer_list<uint64_t> args) {
			check(process.call(proxy,std::span(args.begin(),args.size()),std::chrono::seconds(1)).stop==raven::NativeStop::api && api.dispatch(name),
				"object API did not dispatch");
			const auto result=process.resume(std::chrono::seconds(1));
			check(result.stop==raven::NativeStop::returned,"object API did not return"); return result.value;
		};
		const auto output=process.map_memory(4096,PROT_READ | PROT_WRITE);
		check(invoke("NtCreateEvent",{0x400000000000ULL,0x1f0003,0,0,0})==0xc0000005U && !objects.count(),"bad event output allocated state");
		process.protect_memory(output,4096,PROT_READ);
		check(invoke("NtCreateEvent",{output,0x1f0003,0,0,0})==0xc0000005U && !objects.count(),"readonly event output allocated state");
		process.protect_memory(output,4096,PROT_READ | PROT_WRITE);
		check(invoke("NtCreateEvent",{output,0x1f0003,1,0,0})==0xc0000005U && !objects.count(),"unreadable attributes created state");
		// OBJECT_ATTRIBUTES without a name make an unnamed event, inheritable with OBJ_INHERIT; a named one is not made here.
		const auto attributes=process.map_memory(4096,PROT_READ | PROT_WRITE);
		std::array<uint8_t,48> unnamed{}; unnamed[0]=48; unnamed[24]=2;
		process.write_memory(attributes,unnamed);
		check(invoke("NtCreateEvent",{output,0x1f0003,attributes,0,0})==0 && objects.count()==1,"unnamed attributes made no event");
		const auto inheritable=process.read_memory(output,8);
		uint64_t made_event=0; for (size_t byte=0; byte<8; ++byte) made_event|=uint64_t(inheritable[byte])<<(byte*8);
		check(objects.inherit(made_event) && invoke("CloseHandle",{made_event}) && !objects.count(),"OBJ_INHERIT was not kept");
		unnamed[16]=0x10; process.write_memory(attributes,unnamed);
		check(invoke("NtCreateEvent",{output,0x1f0003,attributes,0,0})==0xc00000bbU && !objects.count(),"named native event created state");
		const auto live=invoke("CreateEventW",{0,1,0,0});
		check(live && objects.count()==1 && invoke("NtSetEvent",{live,0x400000000000ULL})==0xc0000005U &&
			objects.query_event(live).value==0,"bad previous-state output signaled an event");
		check(invoke("NtQueryEvent",{live,1,output,8,0})==0xc00000bbU,"unknown query behavior succeeded");
		check(invoke("CloseHandle",{live}) && !objects.count() && !api.dispatch("Unimplemented"),"object API leaked state or accepted an unknown name");
		const auto blocked=objects.create_event(0x100000,0,false);
		raven::NativeObjectAPI expired(process,environment,objects,std::chrono::steady_clock::now());
		const uint64_t wait_args[]{blocked.value,UINT32_MAX};
		check(process.call(proxy,wait_args,std::chrono::seconds(1)).stop==raven::NativeStop::api,"deadline test did not reach API boundary");
		bool stopped=false;
		try { expired.dispatch("WaitForSingleObject"); }
		catch (const raven::NativeResult &result) { stopped=result.stop==raven::NativeStop::timeout; }
		check(stopped && objects.waiting(blocked.value)==0,"runtime deadline returned a successful guest wait or leaked a waiter");
		objects.close(blocked.value);
		std::cout << "Native event rights, signaling, wait delivery, timeouts, handles and checked guest API contracts passed\n";
		return 0;
	} catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
