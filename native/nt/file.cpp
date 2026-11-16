#include "object_api.h"

#include "console.h"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdexcept>

namespace raven {

NativeObjects::File::~File()
{
	::close(descriptor);
}

NativeObjectResult NativeObjects::inherit_file(int descriptor)
{
	const auto copy=fcntl(descriptor,F_DUPFD_CLOEXEC,3);
	if (copy<0) return {0,errno==EBADF ? 0xc0000008U : 0xc000009aU};
	struct stat metadata{};
	if (fstat(copy,&metadata)) { ::close(copy);return {0,0xc0000008U}; }
	const uint32_t type=S_ISREG(metadata.st_mode) || S_ISDIR(metadata.st_mode) ? 1 :
		S_ISCHR(metadata.st_mode) ? 2 : S_ISFIFO(metadata.st_mode) || S_ISSOCK(metadata.st_mode) ? 3 : 0;
	if (!type) { ::close(copy);return {0,0xc00000bbU}; }
	std::shared_ptr<File> object;
	try { object=std::make_shared<File>(copy,type); }
	catch (...) { ::close(copy);throw; }
	std::lock_guard lock(mutex);
	if (handles.size()>=limit || next>UINT32_MAX-4) return {0,0xc000009aU};
	const auto handle=take();
	handles.emplace(handle,Handle{{},0,std::move(object)});
	return {handle};
}

uint32_t NativeObjects::refresh_file(uint64_t handle, int descriptor)
{
	const auto copy=fcntl(descriptor,F_DUPFD_CLOEXEC,3);
	if (copy<0) return errno==EBADF ? 0xc0000008U : 0xc000009aU;
	struct stat metadata{};
	if (fstat(copy,&metadata)) { ::close(copy);return 0xc0000008U; }
	const uint32_t type=S_ISREG(metadata.st_mode) || S_ISDIR(metadata.st_mode) ? 1 :
		S_ISCHR(metadata.st_mode) ? 2 : S_ISFIFO(metadata.st_mode) || S_ISSOCK(metadata.st_mode) ? 3 : 0;
	if (!type) { ::close(copy);return 0xc00000bbU; }
	std::shared_ptr<File> object;
	try { object=std::make_shared<File>(copy,type); }
	catch (...) { ::close(copy);throw; }
	std::lock_guard lock(mutex);
	const auto found=handles.find(handle);
	if (found==handles.end() || !found->second.file) return 0xc0000008U;
	found->second.file=std::move(object);
	return 0;
}

std::shared_ptr<NativeObjects::File> NativeObjects::file(uint64_t handle) const
{
	std::lock_guard lock(mutex);
	const auto found=handles.find(handle);
	return found==handles.end() ? nullptr : found->second.file;
}

int NativeObjects::descriptor(uint64_t handle) const
{
	const auto object=file(handle);
	return object ? object->descriptor : -1;
}

NativeObjectResult NativeObjects::file_type(uint64_t handle) const
{
	const auto object=file(handle);
	return object ? NativeObjectResult{object->type} : NativeObjectResult{0,0xc0000008U};
}

NativeObjectResult NativeObjects::read_file(uint64_t handle,std::span<uint8_t> into)
{
	const auto object=file(handle);
	if (!object) return {0,0xc0000008U};
	ssize_t count;
	do { count=::read(object->descriptor,into.data(),into.size()); } while (count<0 && errno==EINTR);
	if (count>=0) return {uint64_t(count)};
	if (errno==EBADF || errno==EACCES || errno==EISDIR) return {0,0xc0000022U};
	return {0,0xc0000001U};
}

NativeObjectResult NativeObjects::write_file(uint64_t handle,std::span<const uint8_t> bytes)
{
	const auto object=file(handle);
	if (!object) return {0,0xc0000008U};
	if (bytes.size()>1024*1024) return {0,0xc00000bbU};
	ssize_t written;
	if (::isatty(object->descriptor))
		written=TerminalConsole::instance().write_text(object->descriptor,std::string_view(reinterpret_cast<const char *>(bytes.data()),bytes.size())) ?
			ssize_t(bytes.size()) : -1;
	else
		do { written=::write(object->descriptor,bytes.data(),bytes.size()); } while (written<0 && errno==EINTR);
	if (written>=0) return {uint64_t(written)};
	if (errno==EBADF || errno==EACCES || errno==EISDIR) return {0,0xc0000022U};
	if (errno==ENOSPC || errno==EDQUOT) return {0,0xc000007fU};
	return {0,0xc0000001U};
}

bool NativeObjectAPI::file_io(std::string_view name)
{
	if (name!="GetFileType" && name!="WriteFile") return false;
	// Files opened through CreateFile are generic kernel objects served by the file API registry.
	if (objects.object(process.api_arguments(1)[0])) return false;
	const bool writing=name=="WriteFile";
	const auto args=process.api_arguments(writing ? 5 : 1);
	NativeObjectResult result;
	const auto count=[&](uint32_t value) {
		std::array<uint8_t,4> bytes{};
		for (size_t i=0;i<4;++i) bytes[i]=uint8_t(value>>(i*8));
		process.write_memory(args[3],bytes);
	};
	try {
		if (!writing) result=objects.file_type(args[0]);
		else if (args[4] || uint32_t(args[2])>1024*1024) result.status=0xc00000bbU;
		else {
			if (args[3]) count(0);
			if (std::chrono::steady_clock::now()>=deadline)
				throw NativeResult{NativeStop::timeout,0,process.context().registers.rip,0};
			const auto bytes=process.read_memory(args[1],uint32_t(args[2]));
			result=objects.write_file(args[0],bytes);
			if (!result.status && args[3]) count(uint32_t(result.value));
		}
	} catch (const std::runtime_error &failure) {
		const std::string_view message=failure.what();
		if (message.find("readable")==message.npos && message.find("writable")==message.npos) throw;
		result.status=0xc0000005U;
	}
	if (result.status) {
		const auto status=result.status;
		environment.set_last_error(status==0xc0000008U ? 6 : status==0xc0000022U ? 5 :
			status==0xc00000bbU ? 120 : status==0xc0000005U ? 998 : status==0xc000007fU ? 112 : 31);
	}
	process.return_api(writing ? uint64_t(!result.status) : result.value);
	return true;
}

}
