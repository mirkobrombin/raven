#include "providers.h"
#include "nls.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

static void check(bool value,const char *message)
{
	if (!value) throw std::runtime_error(message);
}

static std::vector<uint8_t> read_file(const std::filesystem::path &path)
{
	std::ifstream file(path,std::ios::binary | std::ios::ate);
	check(bool(file) && file.tellg()>0 && file.tellg()<1024*1024,"invalid process parameter fixture");
	std::vector<uint8_t> bytes(size_t(file.tellg())); file.seekg(0);
	check(bool(file.read(reinterpret_cast<char *>(bytes.data()),std::streamsize(bytes.size()))),"short process parameter fixture");
	return bytes;
}

static uint64_t number(raven::NativeProcess &process,uint64_t address,size_t width)
{
	const auto bytes=process.read_memory(address,width);
	uint64_t result=0;
	for (size_t i=0;i<width;++i) result|=uint64_t(bytes[i])<<(i*8);
	return result;
}

int main(int argc,char **argv)
{
	try {
		check(argc==3,"bootstrap and process parameter fixture directory required");
		const std::filesystem::path directory(argv[2]);
		raven::ModuleSet modules;
		auto &entry=modules.add("parameters.exe",read_file(directory/"parameters.exe"));
		modules.add("parameters.dll",read_file(directory/"parameters.dll"));
		for (const unsigned mode : {0U,1U,2U}) {
			const bool configured=mode!=0;
			std::optional<raven::NativeLaunch> launch=configured ?
				std::optional(raven::NativeLaunch{u"R:\\apps\\parameter.exe",u"\"R:\\apps\\parameter.exe\" \u00e9 \u4e2d \U0001f642"}) : std::nullopt;
			if (mode==2) launch->command_line.clear();
			const auto result=raven::run_native_process(modules,entry,argv[1],std::chrono::seconds(5),{},{},{},launch);
			check(result.stop==raven::NativeStop::returned && !result.value,"parameters unavailable during DLL TLS, DllMain or executable TLS/entry");
			uint64_t digest=14695981039346656037ULL;
			for (const auto character : configured ? launch->command_line : std::u16string(u"\"parameters.exe\""))
				digest=(digest^uint16_t(character))*1099511628211ULL;
			const auto offset=modules.resolve({"parameters.exe","command_digest",0,false,0})-entry.loaded.base();
			const auto bytes=entry.loaded.data().subspan(size_t(offset),8);
			uint64_t observed=0;
			for (size_t i=0;i<8;++i) observed|=uint64_t(bytes[i])<<(i*8);
			check(observed==digest,"guest command line lost launch data or UTF-16 surrogate pairs");
			digest=14695981039346656037ULL;
			const auto ansi=raven::encode_ansi(configured ? launch->command_line : std::u16string(u"\"parameters.exe\""),1252);
			for (const char byte : ansi) digest=(digest^static_cast<unsigned char>(byte))*1099511628211ULL;
			const auto ansi_offset=modules.resolve({"parameters.exe","ansi_digest",0,false,0})-entry.loaded.base();
			const auto ansi_bytes=entry.loaded.data().subspan(size_t(ansi_offset),8);
			observed=0;
			for (size_t i=0;i<8;++i) observed|=uint64_t(ansi_bytes[i])<<(i*8);
			check(observed==digest,"guest ANSI command line differs from launch conversion");
		}
		std::vector<raven::LoadedImage *> images;
		for (const auto &[name,module] : modules.all()) images.push_back(&module->loaded);
		raven::NativeProcess process(images,argv[1],std::chrono::seconds(5));
		raven::NativeEnvironment environment(process,entry.loaded.base(),1,1);
		for (const raven::NativeLaunch launch : {raven::NativeLaunch{},
			raven::NativeLaunch{std::u16string(32767,u'x'),u"command"},
			raven::NativeLaunch{u"image",std::u16string(32767,u'x')},
			raven::NativeLaunch{std::u16string(u"a\0b",3),u"command"},
			raven::NativeLaunch{u"image",std::u16string(u"a\0b",3)},
			raven::NativeLaunch{u"image",u"command",65001},raven::NativeLaunch{u"image",u"command",1361}}) {
			bool rejected=false;
			try { environment.prepare_parameters(launch); } catch (const std::runtime_error &) { rejected=true; }
			check(rejected && !number(process,environment.peb()+0x20,8),"invalid parameters published a process block");
		}
		const auto proxy=modules.resolve({"kernel32.dll","GetCommandLineW",0,false,0});
		const auto &kernel=*modules.all().at("kernel32.dll");
		process.add_api_trap(raven::api_trap_address(kernel,"kernel32.dll","GetCommandLineW",uint32_t(proxy-kernel.loaded.base())));
		check(process.call(proxy,{},std::chrono::seconds(2)).stop==raven::NativeStop::api,"command line test did not stop at its API");
		bool rejected=false;
		try { environment.prepare_parameters({u"image",u"command"}); } catch (const std::runtime_error &) { rejected=true; }
		check(rejected && !number(process,environment.peb()+0x20,8),"running process accepted startup parameters");
		rejected=false;
		try { environment.dispatch("GetCommandLineW"); } catch (const std::runtime_error &) { rejected=true; }
		check(rejected,"missing process parameters produced apparent command line success");
		for (const auto name : {"GetCommandLineA","GetACP"}) {
			rejected=false;
			try { environment.dispatch(name); } catch (const std::runtime_error &) { rejected=true; }
			check(rejected,"missing ANSI parameters produced apparent success");
		}
		process.return_api(0);
		check(process.resume(std::chrono::seconds(2)).stop==raven::NativeStop::returned,"negative startup test did not finish");
		const raven::NativeLaunch launch{u"image.exe",std::u16string(32766,u'\u4e2d')};
		environment.set_last_error(91);
		environment.prepare_parameters(launch);
		const auto parameters=number(process,environment.peb()+0x20,8);
		check(parameters && number(process,parameters+8,4)==1 && environment.last_error()==91,"normalized process parameters or LastError differs");
		check(number(process,parameters,4)>=number(process,parameters+4,4),"parameter block capacity is smaller than length");
		for (const auto &[offset,text] : {std::pair<size_t,std::u16string_view>{0x60,launch.image_path}, {0x70,launch.command_line}}) {
			const auto buffer=number(process,parameters+offset+8,8);
			check(number(process,parameters+offset,2)==text.size()*2 && number(process,parameters+offset+2,2)==(text.size()+1)*2,
				"UNICODE_STRING byte lengths differ");
			const auto bytes=process.read_memory(buffer,(text.size()+1)*2);
			for (size_t i=0;i<text.size();++i)
				check(bytes[i*2]==uint8_t(text[i]) && bytes[i*2+1]==uint8_t(text[i]>>8),"UTF-16 launch data changed");
			check(!bytes[text.size()*2] && !bytes[text.size()*2+1],"parameter string lacks its terminator");
		}
		for (unsigned repeat=0;repeat<2;++repeat) {
			check(process.call(proxy,{},std::chrono::seconds(2)).stop==raven::NativeStop::api && environment.dispatch("GetCommandLineW"),
				"command line did not dispatch");
			const auto result=process.resume(std::chrono::seconds(2));
			check(result.stop==raven::NativeStop::returned && result.value==number(process,parameters+0x78,8) && environment.last_error()==91,
				"command line pointer identity or LastError preservation differs");
		}
		uint64_t ansi_pointer=0;
		for (unsigned repeat=0;repeat<2;++repeat) {
			check(process.call(proxy,{},std::chrono::seconds(2)).stop==raven::NativeStop::api && environment.dispatch("GetCommandLineA"),
				"ANSI command line did not dispatch");
			const auto result=process.resume(std::chrono::seconds(2));
			check(result.stop==raven::NativeStop::returned && result.value && (!ansi_pointer || result.value==ansi_pointer) && environment.last_error()==91,
				"ANSI command line identity or LastError differs");
			ansi_pointer=result.value;
		}
		const auto ansi=process.read_memory(ansi_pointer,launch.command_line.size()+1);
		check(ansi.back()==0,"ANSI command line lacks a terminator");
		for (size_t i=0;i<launch.command_line.size();++i) check(ansi[i]=='?',"unrepresentable ANSI character differs");
		rejected=false;
		try { environment.prepare_parameters({u"replacement",u"replacement"}); } catch (const std::runtime_error &) { rejected=true; }
		check(rejected && number(process,environment.peb()+0x20,8)==parameters,"duplicate startup replaced process parameters");
		const auto published=process.read_memory(environment.peb()+0x20,8);
		process.write_memory(environment.peb()+0x20,std::vector<uint8_t>(8));
		rejected=false;
		try { environment.prepare_parameters({u"replacement",u"replacement"}); } catch (const std::runtime_error &) { rejected=true; }
		check(rejected && !number(process,environment.peb()+0x20,8),"cleared guest PEB bypassed the parameter owner");
		process.write_memory(environment.peb()+0x20,published);
		const auto environment_pointer=number(process,parameters+0x80,8);
		check(process.read_memory(environment_pointer,4)==std::vector<uint8_t>(4) && number(process,parameters+0x3f0,8)==4,
			"empty environment is not double terminated");
		for (const size_t offset : {0x20U,0x28U,0x30U})
			check(number(process,parameters+offset,8)==UINT64_MAX,"startup invented a standard handle");
		std::cout<<"Process-owned UTF-16 parameters before DLL/EXE callbacks, normalized pointers, bounds and duplicate rejection passed\n";
	} catch (const std::exception &error) { std::cerr<<error.what()<<'\n'; return 1; }
	return 0;
}
