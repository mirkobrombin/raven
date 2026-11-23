#pragma once

#include "objects.h"

#include <fcntl.h>
#include <new>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace raven {

// The descriptor gives a detached visual its identity before a compositor opens it. It survives a transfer to another process.
class CompositionVisual : public KernelObject {
public:
	CompositionVisual()
	{
		file = ::memfd_create("raven-composition", MFD_CLOEXEC | MFD_ALLOW_SEALING);
		if (file < 0)
			throw std::bad_alloc();
		if (::fcntl(file, F_ADD_SEALS, seals) < 0) {
			::close(file);
			throw std::bad_alloc();
		}
	}
	explicit CompositionVisual(int descriptor) : file(descriptor) {}
	~CompositionVisual() override { ::close(file); }
	std::u16string_view type_name() const override { return u"Composition"; }
	uint32_t default_access() const override { return 1; }
	bool describe(InheritedHandle &record) const override
	{
		record.kind = "composition";
		record.descriptor = file;
		return true;
	}
	static bool can_adopt(int descriptor)
	{
		const int applied = ::fcntl(descriptor, F_GET_SEALS);
		struct stat info{};
		return applied >= 0 && (applied & seals) == seals && !::fstat(descriptor, &info) && info.st_size == 0;
	}

private:
	static constexpr int seals = F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;
	int file;
};

}
