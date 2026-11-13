#include "windows.h"
#include "api.h"
#include "nt/providers.h"

#include <fstream>
#include <cerrno>
#include <iostream>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static void check(bool condition,const char *message)
{
	if (!condition) throw std::runtime_error(message);
}

struct ForeignWindow {
	uint64_t handle=0;
	pid_t owner=0;
	int channel=-1;
	ForeignWindow()
	{
		int sockets[2];
		check(::socketpair(AF_UNIX,SOCK_STREAM | SOCK_CLOEXEC,0,sockets)==0,"publisher socket failed");
		owner=::fork();
		if (owner<0) { ::close(sockets[0]); ::close(sockets[1]); throw std::runtime_error("publisher fork failed"); }
		if (!owner) {
			::close(sockets[0]);
			const auto window=raven::window_handle_base()+1;
			raven::publish_window(window,801,603);
			raven::publish_window(window+1,0,0);
			if (::write(sockets[1],&window,sizeof window)!=sizeof window) ::_exit(1);
			char release=0;
			while (::read(sockets[1],&release,1)<0 && errno==EINTR) {}
			::_exit(0);
		}
		::close(sockets[1]);
		channel=sockets[0];
		const auto received=::read(channel,&handle,sizeof handle);
		if (received!=sizeof handle) { stop(); throw std::runtime_error("publisher did not publish a window"); }
	}
	void stop()
	{
		if (channel>=0) { ::close(channel); channel=-1; }
		if (owner>0) { while (::waitpid(owner,nullptr,0)<0 && errno==EINTR) {} owner=0; }
	}
	~ForeignWindow()
	{
		stop();
		if (handle) raven::withdraw_window(handle);
		if (handle) raven::withdraw_window(handle+1);
	}
};

int main(int argc,char **argv)
{
	try {
		check(argc==3,"bootstrap and coordinate fixture required");
		ForeignWindow foreign;
		std::ifstream file(argv[2],std::ios::binary | std::ios::ate);
		check(bool(file) && file.tellg()>0 && file.tellg()<1024*1024*1024,"invalid coordinate fixture");
		std::vector<uint8_t> bytes(static_cast<size_t>(file.tellg()));
		file.seekg(0);
		check(bool(file.read(reinterpret_cast<char *>(bytes.data()),static_cast<std::streamsize>(bytes.size()))),"cannot read coordinate fixture");
		raven::ModuleSet modules;
		auto &entry=modules.add("client.exe",std::move(bytes));
		auto &user=modules.add("user32.dll",raven::runtime_provider("user32.dll",0x180000000ULL,raven::NativeDesktop::user_exports()));
		raven::LoadedImage *images[]={&entry.loaded,&user.loaded};
		raven::NativeProcess process(images,argv[1],std::chrono::seconds(2));
		for (const auto &symbol:user.exports)
			if (!symbol.names.empty() && symbol.forwarder.empty())
				process.add_api_trap(raven::api_trap_address(user,"user32.dll",symbol.names.at(0),symbol.address));
		raven::NativeEnvironment environment(process,entry.loaded.base(),1,1);
		raven::DesktopObjects objects;
		raven::NativeWindows windows(process,environment,objects);
		const auto window=objects.add(raven::DesktopWindow{0,0,u"",20,30,320,240});
		check(window,"window allocation failed");
		auto *record=objects.window(window);
		record->x=27; record->y=41; record->width=300; record->height=212;
		const auto call=[&](const char *name,std::initializer_list<uint64_t> args) {
			const auto function=modules.resolve({"user32.dll",name,0,false,0});
			check(process.call(function,std::span(args.begin(),args.size()),std::chrono::seconds(1)).stop==raven::NativeStop::api,"coordinate API did not trap");
			check(windows.dispatch(name),"coordinate API not dispatched");
			const auto result=process.resume(std::chrono::seconds(1));
			check(result.stop==raven::NativeStop::returned,"coordinate API did not return");
			return result.value;
		};
		const auto page=process.map_memory(8192,PROT_READ | PROT_WRITE);
		process.protect_memory(page+4096,4096,PROT_NONE);
		const auto output=page+4096-16;
		const auto words=[&](uint64_t address,size_t count) {
			const auto data=process.read_memory(address,count*4);
			std::vector<int32_t> result(count);
			for (size_t i=0;i<count;++i) {
				uint32_t value=0;
				for (unsigned k=0;k<4;++k) value|=uint32_t(data[i*4+k])<<(k*8);
				result[i]=int32_t(value);
			}
			return result;
		};
		check(call("GetWindowRect",{window,output})==1 && words(output,4)==std::vector<int32_t>{20,30,340,270},"outer rectangle followed client geometry");
		check(call("GetClientRect",{window,output})==1 && words(output,4)==std::vector<int32_t>{0,0,300,212},"client extent differs");
		environment.set_last_error(71);
		check(call("IsWindow",{foreign.handle})==1 && environment.last_error()==71,"live foreign window was rejected");
		check(call("GetClientRect",{foreign.handle,output})==1 && words(output,4)==std::vector<int32_t>{0,0,801,603} && environment.last_error()==71,"live foreign client extent differs");
		check(call("GetClientRect",{foreign.handle+1,output})==1 && words(output,4)==std::vector<int32_t>{0,0,0,0},"empty foreign client extent differs");
		check(call("GetClientRect",{foreign.handle,page+4096})==0 && environment.last_error()==998,"foreign rectangle accepted unwritable output");
		foreign.stop();
		check(call("IsWindow",{foreign.handle})==0,"dead foreign window was accepted");
		check(call("GetClientRect",{foreign.handle,output})==0 && environment.last_error()==1400 && words(output,4)==std::vector<int32_t>{0,0,0,0},"dead owner's rectangle was accepted or modified");
		const auto point=page+4096-8;
		const std::array<uint8_t,8> zero{};
		process.write_memory(point,zero);
		environment.set_last_error(71);
		check(call("ClientToScreen",{window,point})==1 && words(point,2)==std::vector<int32_t>{27,41} && environment.last_error()==71,"client origin did not use live window state");
		check(call("ScreenToClient",{window,point})==1 && words(point,2)==std::vector<int32_t>{0,0},"inverse coordinate mapping differs");
		check(call("MapWindowPoints",{window,0,0x400000000000ULL,0})==(27U | (41U<<16)),"zero-point mapping accessed the buffer");
		check(call("MapWindowPoints",{0x12345,window,0x400000000000ULL,0})==0 && environment.last_error()==1400,"invalid zero-point source accepted");
		check(call("MapWindowPoints",{0,window,point,65537})==0 && environment.last_error()==120 && words(point,2)==std::vector<int32_t>{0,0},"unqualified point count mutated memory");
		check(call("MapWindowPoints",{0,window,0x400000000000ULL,1})==0 && environment.last_error()==998,"unreadable points accepted");
		record=objects.window(window); record->visible=true;
		raven::DesktopWindow child{0,1,u"",40,60,40,30}; child.parent=window; child.visible=true;
		const auto target=objects.add(child);
		bool injected=false,hit=false;
		windows.set_call([&](uint64_t procedure,std::span<const uint64_t> args) {
			check(procedure==1 && args.size()==4 && args[0]==target && args[1]==0x84,"unexpected pointer callback");
			hit=true;
			check(objects.erase(target),"hit-test callback could not destroy its window");
			return raven::NativeResult{raven::NativeStop::returned,1};
		});
		raven::DesktopPresentation presentation;
		presentation.poll=[&](std::chrono::milliseconds) {
			std::vector<raven::DesktopEvent> events;
			if (!injected) {
				injected=true;
				events.push_back({window,0,0,0,raven::DesktopEventKind::pointer_motion,25,35});
			}
			return events;
		};
		windows.set_presentation(presentation);
		check(call("PeekMessageW",{page+128,0,0x200,0x200,1})==0 && hit && !objects.window(target),"destroyed hit-test target retained pointer input");
		injected=false;
		check(call("PeekMessageW",{page+128,0,0x200,0x200,1})==1 &&
			words(page+128,2)==std::vector<int32_t>{int32_t(window),int32_t(window>>32)},"surviving window lost pointer input");
		// A move whose cursor request lets the program read its queue is still queued before the press that came after it.
		raven::DesktopWindow asked{0,1,u"",40,60,40,30}; asked.parent=window; asked.visible=true;
		check(objects.add(asked),"a child window could not be added");
		bool reading=false;
		int step=0;
		windows.set_call([&](uint64_t,std::span<const uint64_t> args) {
			if (args[1]==0x84) return raven::NativeResult{raven::NativeStop::returned,1};
			if (args[1]==0x20 && !reading) {
				// The program asks from its cursor handler whether input waits, which takes in what the host has.
				reading=true;
				step=1;
				windows.input_ready();
			}
			return raven::NativeResult{raven::NativeStop::returned,0};
		});
		presentation.poll=[&](std::chrono::milliseconds) {
			std::vector<raven::DesktopEvent> events;
			if (step==0) {
				step=-1;
				events.push_back({window,0,0,0,raven::DesktopEventKind::pointer_motion,26,36});
			} else if (step==1) {
				step=2;
				events.push_back({window,0x110,1,0,raven::DesktopEventKind::pointer_button,26,36});
			}
			return events;
		};
		windows.set_presentation(presentation);
		while (call("PeekMessageW",{page+128,0,0,0,1}) && words(page+128,3)[2]!=0x200 && words(page+128,3)[2]!=0x201) {}
		check(reading,"a move did not ask its window for the cursor");
		check(words(page+128,3)[2]==0x200,"a press reached the program before the move that came before it");
		while (call("PeekMessageW",{page+128,0,0,0,1}) && words(page+128,3)[2]!=0x201) {}
		check(words(page+128,3)[2]==0x201,"the press that followed a move was lost");
		process.protect_memory(page,4096,PROT_READ);
		check(call("ClientToScreen",{window,point})==0 && environment.last_error()==998 && words(point,2)==std::vector<int32_t>{0,0},"readonly point was modified");
		std::cout<<"Owned outer/client geometry, live mapping and guard-page negatives passed\n";
		return 0;
	} catch (const std::exception &error) { std::cerr<<error.what()<<'\n'; return 1; }
}
