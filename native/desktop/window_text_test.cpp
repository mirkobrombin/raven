#include "windows.h"
#include "nt/providers.h"
#include "nt/nls.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/mman.h>

static void check(bool value,const char *message)
{
	if (!value) throw std::runtime_error(message);
}

int main(int argc,char **argv)
{
	try {
		check(argc==3 || argc==4,"bootstrap, window text fixture and optional output prefix required");
		std::ifstream file(argv[2],std::ios::binary|std::ios::ate);check(bool(file) && file.tellg()>0,"cannot read window text fixture");
		std::vector<uint8_t> bytes(size_t(file.tellg()));file.seekg(0);
		check(bool(file.read(reinterpret_cast<char *>(bytes.data()),std::streamsize(bytes.size()))),"short window text fixture");
		raven::ModuleSet modules;auto &entry=modules.add("text.exe",bytes);
		const auto run=raven::run_native_process(modules,entry,argv[1],std::chrono::seconds(15));
		if(run.stop!=raven::NativeStop::returned || run.value)std::cerr<<"window text SDK stop="<<int(run.stop)<<" value="<<run.value<<'\n';
		check(run.stop==raven::NativeStop::returned && !run.value,"window text SDK contract differs");
		if(argc==4) {
			const auto found=std::find_if(entry.exports.begin(),entry.exports.end(),[](const auto &symbol) {
				return std::find(symbol.names.begin(),symbol.names.end(),"text_rows")!=symbol.names.end();
			});check(found!=entry.exports.end(),"window text evidence export missing");
			const auto data=entry.loaded.data().subspan(found->address,512*24*4);
			std::ofstream output(std::string(argv[3])+".text-rows",std::ios::binary);
			output.write(reinterpret_cast<const char *>(data.data()),std::streamsize(data.size()));check(bool(output),"cannot save window text evidence");
		}
		auto &user=*modules.all().at("user32.dll");raven::LoadedImage *images[]={&entry.loaded,&user.loaded};
		raven::NativeProcess process(images,argv[1],std::chrono::seconds(3));
		for(const auto &symbol:user.exports)
			if(!symbol.names.empty() && symbol.forwarder.empty())
				process.add_api_trap(raven::api_trap_address(user,"user32.dll",symbol.names.at(0),symbol.address));
		raven::NativeEnvironment environment(process,entry.loaded.base(),1,1);raven::DesktopObjects objects;
		raven::NativeWindows windows(process,environment,objects);
		const auto atom=objects.register_class({"text owner",1,1});
		const auto window=objects.add(raven::DesktopWindow{atom,1,u"caf\u00e9-\u4e2d-\U0001f680",0,0,16,12});
		check(window,"cannot allocate owned Unicode title");objects.window(window)->visible=true;
		unsigned frames=0;std::string title;
		raven::DesktopPresentation presentation;
		presentation.present=[&](uint64_t handle,std::string_view text,uint32_t width,uint32_t height,std::span<const uint8_t> pixels) {
			check(handle==window && width==16 && height==12 && pixels.size()==16*12*4,"Unicode title changed surface ownership");
			title=text;++frames;
		};
		windows.set_presentation(presentation);windows.drawing(window);windows.flush_dirty();
		check(frames==1 && title=="caf\xc3\xa9-\xe4\xb8\xad-\xf0\x9f\x9a\x80","host title is not UTF-8");
		const auto call=[&](uint32_t message,uint64_t wparam,uint64_t lparam) {
			const auto function=modules.resolve({"user32.dll","DefWindowProcW",0,false,0});
			const uint64_t args[]{window,message,wparam,lparam};
			check(process.call(function,args,std::chrono::seconds(1)).stop==raven::NativeStop::api,"text API did not trap");
			check(windows.dispatch("DefWindowProcW"),"text API not dispatched");const auto result=process.resume(std::chrono::seconds(1));
			check(result.stop==raven::NativeStop::returned,"text API did not return");return result.value;
		};
		const auto input=process.map_memory(65536,PROT_READ | PROT_WRITE);
		const std::array<uint8_t,8> invalid{0x00,0xd8,0x78,0x00,0x00,0xdc,0,0};process.write_memory(input,invalid);
		check(call(0xc,0,input)==1 && frames==2 && title=="\xef\xbf\xbdx\xef\xbf\xbd" &&
			objects.window(window)->title==std::u16string({0xd800,u'x',0xdc00}),"host replacement damaged guest UTF-16");
		check(call(0xd,3,input+64)==2,"UTF-16 truncation count differs");
		const auto copied=process.read_memory(input+64,6);
		check(copied==std::vector<uint8_t>({0,0xd8,0x78,0,0,0}),"UTF-16 truncation changed guest code units");
		const auto previous=objects.window(window)->title;
		std::vector<uint8_t> too_long(32767*2,1);process.write_memory(input,too_long);environment.set_last_error(71);
		check(!call(0xc,0,input) && environment.last_error()==120 && objects.window(window)->title==previous && frames==2,
			"unqualified title length mutated window state");
		bool rejected=false;try {raven::encode_utf8(previous);}catch(uint32_t error){rejected=error==87;}
		check(rejected,"presentation replacement weakened strict filename encoding");
		windows.shutdown();process.unmap_memory(input);
	} catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}
	return 0;
}
