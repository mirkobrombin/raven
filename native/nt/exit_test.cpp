#include "providers.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

static void check(bool value,const char *message)
{
	if (!value) throw std::runtime_error(message);
}
static std::vector<uint8_t> read_file(const std::filesystem::path &path)
{
	std::ifstream file(path,std::ios::binary | std::ios::ate);
	check(bool(file) && file.tellg()>0 && file.tellg()<1024*1024,"invalid exit fixture");
	std::vector<uint8_t> bytes(size_t(file.tellg())); file.seekg(0);
	check(bool(file.read(reinterpret_cast<char *>(bytes.data()),std::streamsize(bytes.size()))),"short exit fixture"); return bytes;
}
static uint32_t field(raven::ModuleSet &modules,const char *module,const char *name)
{
	auto &loaded=modules.all().at(module)->loaded;
	const auto offset=modules.resolve({module,name,0,false,0})-loaded.base();
	const auto bytes=loaded.data().subspan(size_t(offset),4);
	uint32_t value=0; for (unsigned i=0;i<4;++i) value|=uint32_t(bytes[i])<<(i*8); return value;
}
int main(int argc,char **argv)
{
	try {
		check(argc==3,"bootstrap and exit fixture directory required");
		const std::filesystem::path directory(argv[2]);
		for (unsigned mode=0;mode<9;++mode) {
			raven::ModuleSet modules;
			auto &entry=modules.add("client.exe",read_file(directory/("exit-"+std::to_string(mode)+".exe")));
			unsigned loads=0,frames=0,removed=0; uint64_t window=0;
			const auto source=[&](std::string_view name)->std::optional<std::vector<uint8_t>> { ++loads; return read_file(directory/name); };
			raven::DesktopPresentation presentation;
			presentation.present=[&](uint64_t handle,std::string_view title,uint32_t width,uint32_t height,std::span<const uint8_t> pixels) {
				check(handle && (!window || window==handle) && title=="Raven process exit" && width==8 && height==8 && pixels.size()==256,"exit callback surface differs"); window=handle;
				// The window shows its color before it paints and after it paints white.
				for (size_t at=0;at<pixels.size();at+=4) check(pixels[at]==255 && pixels[at+1]==pixels[at] && pixels[at+2]==pixels[at],"exit callback paint differs"); ++frames;
			};
			presentation.remove=[&](uint64_t handle) { check(handle==window,"removed wrong exit window"); ++removed; };
			const auto result=raven::run_native_process(modules,entry,argv[1],std::chrono::seconds(5),source,{},presentation);
			const uint32_t expected=mode==4 || mode==5 ? 9 : mode==6 ? 0 : mode==7 ? 0x12345600 : 0x12345678;
			check(result.stop==(mode==8 ? raven::NativeStop::exception : raven::NativeStop::exited) && result.value==(mode==8 ? 0xe0000066 : expected),"guest exit status or cleanup fault differs");
			check(!field(modules,"client.exe","after_exit") && field(modules,"client.exe","phase")==((mode==2 || mode==3) ? 1U : 2U) && loads==1,"exit resumed caller or initialization");
			if (mode!=2 && mode!=3) check(field(modules,"client.exe","sequence")==1234,"exit notification order or repeated cleanup differs");
			else check(field(modules,mode==2 ? "exit-attach.dll" : "exit-tls.dll","exit_trace")== (mode==2 ? 12U : 1U),"partially initialized module received detach");
			check(mode==1 ? frames==2 && removed==1 : !frames && !removed,"process exit retained presentation resources");
			std::ifstream children("/proc/self/task/"+std::to_string(getpid())+"/children"); long child=0;
			check(!(children>>child),"process exit retained native child");
		}
	} catch (const std::exception &error) { std::cerr<<error.what()<<'\n'; return 1; }
	return 0;
}
