#include "environment.h"

#include <algorithm>
#include <array>
#include <sched.h>
#include <stdexcept>
#include <time.h>
#include <unistd.h>

namespace raven {

std::span<const std::string_view> NativeEnvironment::nt_exports()
{
	static constexpr std::array<std::string_view,1> names{"NtQuerySystemInformation"};
	return names;
}

static void put(std::span<uint8_t> bytes, size_t at, uint64_t value, size_t width)
{
	for (size_t byte=0; byte<width; ++byte)
		bytes[at+byte]=uint8_t(value>>(byte*8));
}

static std::array<uint8_t,64> basic_information()
{
	const auto pages=sysconf(_SC_PHYS_PAGES), page_size=sysconf(_SC_PAGESIZE);
	cpu_set_t affinity;
	CPU_ZERO(&affinity);
	timespec resolution{};
	if (pages<=0 || uint64_t(pages)>UINT32_MAX || page_size!=4096 ||
		sched_getaffinity(0,sizeof(affinity),&affinity)!=0 || clock_getres(CLOCK_MONOTONIC_COARSE,&resolution)!=0)
		throw std::runtime_error("native basic system information cannot describe the host resources");
	const auto processors=std::min(CPU_COUNT(&affinity),64);
	const auto tick=(uint64_t(resolution.tv_sec)*1000000000+uint64_t(resolution.tv_nsec)+99)/100;
	if (processors<=0 || !tick || tick>UINT32_MAX)
		throw std::runtime_error("native basic system information exceeds the initial group/timer contract");
	std::array<uint8_t,64> result{};
	put(result,4,tick,4); put(result,8,4096,4); put(result,12,uint64_t(pages),4);
	// Logical guest physical numbering; physical-page mapping APIs remain unsupported.
	put(result,20,uint64_t(pages)-1,4); put(result,24,65536,4);
	put(result,32,65536,8); put(result,40,native_user_limit-1,8);
	put(result,48,processors==64 ? UINT64_MAX : (uint64_t(1)<<processors)-1,8);
	result[56]=uint8_t(processors);
	return result;
}

bool NativeEnvironment::system_information()
{
	require_current();
	const auto args=process.api_arguments(4);
	// The list of processes is the API layer's, which knows the other instances of the runtime.
	if (uint32_t(args[0])==5)
		return false;
	uint32_t status=0;
	try {
		if (uint32_t(args[0])!=0)
			status=0xc0000002U;
		else {
			if (args[3]) {
				std::array<uint8_t,4> length{}; put(length,0,64,4); process.write_memory(args[3],length);
			}
			if (uint32_t(args[2])!=64)
				status=0xc0000004U;
			else
				process.write_memory(args[1],basic_information());
		}
	} catch (const std::runtime_error &error) {
		const std::string_view message=error.what();
		if (message.find("readable")==message.npos && message.find("writable")==message.npos)
			throw;
		status=0xc0000005U;
	}
	process.return_api(status);
	return true;
}

}
