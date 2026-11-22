#include "api.h"
#include "windows.h"
#include "nt/providers.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>

static void check(bool value,const char *message)
{
	if (!value) throw std::runtime_error(message);
}

int main(int argc,char **argv)
{
	try {
		check(argc==3 || argc==4,"bootstrap, color fixture and optional output prefix required");
		std::ifstream file(argv[2],std::ios::binary|std::ios::ate);check(bool(file) && file.tellg()>0,"cannot read color fixture");
		std::vector<uint8_t> bytes(size_t(file.tellg()));file.seekg(0);
		check(bool(file.read(reinterpret_cast<char *>(bytes.data()),std::streamsize(bytes.size()))),"short color fixture");
		raven::ModuleSet modules;auto &entry=modules.add("color.exe",bytes);
		const auto run=raven::run_native_process(modules,entry,argv[1],std::chrono::seconds(15));
		if(run.stop!=raven::NativeStop::returned || run.value)std::cerr<<"color SDK stop="<<int(run.stop)<<" value="<<run.value<<'\n';
		check(run.stop==raven::NativeStop::returned && !run.value,"color/frame SDK contract differs");
		if(argc==4) {
			const auto save=[&](std::string_view name,size_t size) {
				const auto found=std::find_if(entry.exports.begin(),entry.exports.end(),[&](const auto &symbol) {
					return std::find(symbol.names.begin(),symbol.names.end(),name)!=symbol.names.end();
				});check(found!=entry.exports.end(),"color evidence export missing");
				const auto data=entry.loaded.data().subspan(found->address,size);
				std::ofstream output(std::string(argv[3])+"."+std::string(name),std::ios::binary);
				output.write(reinterpret_cast<const char *>(data.data()),std::streamsize(data.size()));check(bool(output),"cannot save color evidence");
			};
			save("color_rows",31*16);save("color_pixels",31*16*12*4);save("frame_rows",32*24);save("frame_pixels",32*16*12*4);
		}
		auto &user=*modules.all().at("user32.dll");raven::LoadedImage *images[]={&entry.loaded,&user.loaded};
		raven::NativeProcess process(images,argv[1],std::chrono::seconds(3));
		for(const auto &symbol:user.exports)
			if(!symbol.names.empty() && symbol.forwarder.empty())
				process.add_api_trap(raven::api_trap_address(user,"user32.dll",symbol.names.at(0),symbol.address));
		raven::NativeEnvironment environment(process,entry.loaded.base(),1,1);raven::NativeResources resources(process,environment,modules,entry);
		auto palette=raven::desktop_colors;palette[5]=0x112233;palette[8]=0x445566;palette[15]=0x778899;
		raven::DesktopObjects objects(256*1024*1024,{}, {},palette),defaults;
		raven::NativeDesktop desktop(process,environment,objects,resources),other(process,environment,defaults,resources);
		const auto call=[&](auto &owner,const char *name,std::initializer_list<uint64_t> args) {
			const auto function=modules.resolve({"user32.dll",name,0,false,0});
			check(process.call(function,std::span(args.begin(),args.size()),std::chrono::seconds(1)).stop==raven::NativeStop::api,"color API did not trap");
			check(owner.dispatch(name),"color API not dispatched");const auto result=process.resume(std::chrono::seconds(1));
			check(result.stop==raven::NativeStop::returned,"color API did not return");return result.value;
		};
		check(call(desktop,"GetSysColor",{5})==0x112233 && call(other,"GetSysColor",{5})==0xffffff,"system colors use global state");
		const auto brush=call(desktop,"GetSysColorBrush",{5});const auto count=objects.count(),used=objects.bytes();
		check(brush && objects.brush(brush)->color==0x112233 && objects.delete_brush(brush) && call(desktop,"GetSysColorBrush",{5})==brush &&
			objects.count()==count && objects.bytes()==used && objects.system_color(31)==0,"system brush ownership/cache differs");
		const auto atom=objects.register_class({"color owner",1,1});const auto window=objects.add(raven::DesktopWindow{atom,1,u"",0,0,16,12});
		const auto dc=objects.add(raven::DesktopDC{0,0});raven::NativeWindows windows(process,environment,objects);
		check(call(windows,"DefWindowProcW",{window,0x138,dc,window})==objects.system_brush(15) && objects.dc(dc)->text_color==0x445566 &&
			objects.dc(dc)->background_color==0x778899,"control colors ignored their desktop owner");
		environment.set_last_error(71);
		check(!call(windows,"DefWindowProcW",{window,0x137,dc,window}) && environment.last_error()==120,"unimplemented scrollbar pattern returned a solid brush");
		palette[0]=0xff000000;bool rejected=false;
		try {raven::DesktopObjects invalid(1024*1024,{}, {},palette);}catch(const std::runtime_error &){rejected=true;}
		check(rejected,"invalid palette accepted");
	} catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}
	return 0;
}
