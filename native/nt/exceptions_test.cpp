#include "exceptions.h"
#include "providers.h"

#include <iostream>
#include <optional>
#include <stdexcept>
#include <sys/mman.h>

static void check(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

int main(int argc, char **argv)
{
	try {
		if (argc != 2)
			throw std::runtime_error("bootstrap required");
		for (const auto mode : {0, 1, 2, 3, 4}) {
			raven::ModuleSet modules;
			auto &module = modules.add("kernel32.dll", raven::environment_provider("kernel32.dll", 0x180000000));
			raven::LoadedImage *images[] = {&module.loaded};
			raven::NativeProcess process(images, argv[1], std::chrono::seconds(2));
			const auto api = modules.resolve({"kernel32.dll", "RaiseException", 0, false, 0});
			process.add_api_trap(api);
			std::vector<uint8_t> bytes(15 * 8);
			for (size_t index = 0; index < bytes.size(); ++index)
				bytes[index] = static_cast<uint8_t>(index * 7);
			const auto data = process.map_memory(4096, PROT_READ | PROT_WRITE);
			process.write_memory(data, bytes);
			const uint64_t arguments[] = {0x12345678e0123456ULL, 0x1234567800000001ULL,
				mode == 0 || mode == 4 ? 15U : mode == 3 ? 0U : 16U,
				mode == 2 ? 0 : mode == 3 || mode == 4 ? 1 : data};
			check(process.call(api, arguments, std::chrono::seconds(1)).stop == raven::NativeStop::api, "exception fixture did not suspend at the API");
			bool failed = false;
			std::optional<raven::NativeResult> captured;
			try {
				captured = raven::native_raise_exception(process);
			} catch (const std::runtime_error &) {
				failed = true;
			}
			check(failed == (mode == 1 || mode == 4), "software exception bounds or pointer rejection differs");
			if (captured) {
				const auto &result = *captured;
				check(result.stop == raven::NativeStop::exception && result.value == 0xe0123456 &&
					result.exception_flags == 1 && result.instruction == api, "software exception fields differ");
				check(result.exception_information.size() == (mode == 0 ? 15U : 0U), "software exception parameter count differs");
				if (!mode)
					for (size_t index = 0; index < bytes.size(); ++index)
						check(static_cast<uint8_t>(result.exception_information[index / 8] >> ((index % 8) * 8)) == bytes[index], "software exception parameter bytes differ");
			}
			const auto pending = process.api_arguments(4);
			check(std::equal(pending.begin(), pending.end(), std::begin(arguments)), "exception capture completed or altered the pending caller");
		}
		for (const auto mode : {0,1,2,3}) {
			raven::ModuleSet modules;
			auto &module=modules.add("kernel32.dll",raven::environment_provider("kernel32.dll",0x180000000));
			raven::LoadedImage *images[]{&module.loaded};
			raven::NativeProcess process(images,argv[1],std::chrono::seconds(2));
			const auto api=modules.resolve({"kernel32.dll","RaiseException",0,false,0});
			process.add_api_trap(api);
			std::vector<uint8_t> record(152);
			const auto put=[&](size_t at,uint64_t value,size_t width) {
				for (size_t byte=0; byte<width; ++byte) record[at+byte]=uint8_t(value>>(byte*8));
			};
			put(0,0xe0123456,4); put(4,1,4); put(16,0x190001234ULL,8); put(24,mode==2 ? 16 : 2,4);
			put(32,0x1122334455667788ULL,8); put(40,0x8877665544332211ULL,8);
			// The page after the record's is kept unreadable, so the record that crosses into it (mode 3) cannot be read whatever the
			// host maps next to it, as the page of gates of a parallel guest.
			const auto data=process.map_memory(8192,PROT_READ | PROT_WRITE);
			process.protect_memory(data+4096,4096,PROT_NONE);
			process.write_memory(data,record);
			const uint64_t arguments[]{mode==0 ? 0x12345678e0123456ULL : mode==3 ? data+4096-128 : data};
			check(process.call(api,arguments,std::chrono::seconds(1)).stop==raven::NativeStop::api,"RTL exception did not reach API boundary");
			bool failed=false;
			std::optional<raven::NativeResult> result;
			try { result=raven::native_raise_rtl_exception(process,mode==0); }
			catch (const std::runtime_error &) { failed=true; }
			check(failed==(mode>=2),"RTL exception count/pointer bounds differ");
			if (result) {
				check(result->stop==raven::NativeStop::exception && result->value==0xe0123456 && result->exception_flags==1,
					"RTL exception code/flags differ");
				check(result->instruction==(mode==0 ? api : 0x190001234ULL),"RTL exception address differs");
				check(result->exception_information==(mode==0 ? std::vector<uint64_t>{} :
					std::vector<uint64_t>{0x1122334455667788ULL,0x8877665544332211ULL}),"RTL exception information differs");
			}
			check(process.api_arguments(1)[0]==arguments[0],"RTL exception capture changed the pending caller");
		}
		std::cout << "Win32/RTL exception capture, DWORD ABI, record bounds and preserved pending callers passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
