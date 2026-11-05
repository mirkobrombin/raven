#include "nt/providers.h"
#include "desktop/foreign_surface.h"

#include <cerrno>
#include <atomic>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

static void check(bool value,const char *message)
{
	if (!value) throw std::runtime_error(message);
}

int main(int argc,char **argv)
{
	uint64_t window=0;
	pid_t child=0;
	try {
		check(argc==3,"bootstrap and foreign-present fixture required");
		std::ifstream file(argv[2],std::ios::binary | std::ios::ate);
		check(bool(file) && file.tellg()>0 && file.tellg()<1024*1024,"invalid foreign-present fixture");
		std::vector<uint8_t> bytes(size_t(file.tellg())); file.seekg(0);
		check(bool(file.read(reinterpret_cast<char *>(bytes.data()),std::streamsize(bytes.size()))),"short foreign-present fixture");
		raven::ModuleSet modules; auto &entry=modules.add("foreign-present.exe",std::move(bytes));
		window=raven::window_handle_base()+1;
		raven::publish_window(window,64,48);
		const auto target=modules.resolve({"foreign-present.exe","target_window",0,false,0})-entry.loaded.base();
		std::memcpy(entry.loaded.data().data()+target,&window,sizeof window);
		const auto control=modules.resolve({"foreign-present.exe","control",0,false,0})-entry.loaded.base();
		auto &command=*reinterpret_cast<uint32_t *>(entry.loaded.data().data()+control);
		child=::fork();
		check(child>=0,"foreign-present fork failed");
		if (!child) {
			try {
				const auto result=entry.image.is_64 ?
					raven::run_native_process(modules,entry,argv[1],std::chrono::seconds(30)) :
					raven::run_native_process_x86(modules,entry,argv[1],std::chrono::seconds(30));
				std::cerr<<"Foreign presentation returned "<<result.value<<'\n';
				::_exit(result.stop==raven::NativeStop::returned ? int(result.value) : 100);
			} catch (const std::exception &error) { std::cerr<<error.what()<<'\n'; ::_exit(101); }
		}
		uint32_t seen=0;
		// Generous: in a parallel suite the software Vulkan driver shares the processors with other GPU tests.
		const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
		for (uint32_t frame=0;frame<2;++frame) {
			bool received=false;
			while (!received && std::chrono::steady_clock::now()<deadline) {
				received=raven::take_foreign_frame(window,seen,[&](const uint8_t *pixels,uint32_t width,uint32_t height) {
					check(width==(frame ? 80U : 64U) && height==(frame ? 40U : 48U),"foreign frame extent differs");
					for (size_t i=0;i<size_t(width)*height;++i)
						check(pixels[i*4]==(frame ? 0 : 191) && pixels[i*4+1]==(frame ? 32 : 128) &&
							pixels[i*4+2]==(frame ? 255 : 64) && pixels[i*4+3]==255,"foreign GPU texels differ");
				});
				if (!received) {
					int status=0;
					const auto ended=::waitpid(child,&status,WNOHANG);
					if (ended==child) { child=0; throw std::runtime_error("foreign Vulkan child ended before its frame"); }
					::usleep(1000);
				}
			}
			check(received,"foreign GPU frame was not published");
			if (!frame) raven::publish_window(window,80,40);
			else raven::withdraw_window(window);
			std::atomic_ref<uint32_t>(command).store(frame+1,std::memory_order_release);
		}
		int status=0;
		while (::waitpid(child,&status,0)<0) check(errno==EINTR,"foreign-present wait failed");
		child=0;
		check(WIFEXITED(status) && !WEXITSTATUS(status),"foreign Vulkan child failed");
		raven::withdraw_window(window);
		std::cout<<"Real foreign Vulkan frame reached its owner\n";
	} catch (const std::exception &error) {
		if (child>0) { ::kill(child,SIGKILL); while (::waitpid(child,nullptr,0)<0 && errno==EINTR) {} }
		if (window) raven::withdraw_window(window);
		std::cerr<<error.what()<<'\n'; return 1;
	}
	return 0;
}
