#include "x86_system_api.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>

namespace raven {

namespace {

using Export = NativeX86System::Export;

constexpr std::array exported{
	Export{"kernel32.dll", "AcquireSRWLockExclusive", 1}, Export{"kernel32.dll", "AcquireSRWLockShared", 1},
	Export{"kernel32.dll", "ReleaseSRWLockExclusive", 1}, Export{"kernel32.dll", "ReleaseSRWLockShared", 1},
	Export{"kernel32.dll", "InitializeSRWLock", 1}, Export{"kernel32.dll", "TryAcquireSRWLockExclusive", 1},
	Export{"kernel32.dll", "TryAcquireSRWLockShared", 1}, Export{"kernel32.dll", "InitializeConditionVariable", 1},
	Export{"kernel32.dll", "SleepConditionVariableSRW", 4}, Export{"kernel32.dll", "WakeConditionVariable", 1},
	Export{"kernel32.dll", "WakeAllConditionVariable", 1}, Export{"kernel32.dll", "WaitOnAddress", 4}, Export{"kernel32.dll", "WakeByAddressSingle", 1},
	Export{"kernel32.dll", "WakeByAddressAll", 1}, Export{"kernel32.dll", "TlsAlloc", 0}, Export{"kernel32.dll", "TlsFree", 1},
	Export{"kernel32.dll", "TlsGetValue", 1}, Export{"kernel32.dll", "TlsSetValue", 2}, Export{"kernel32.dll", "CreateSemaphoreA", 4},
	Export{"kernel32.dll", "CreateSemaphoreW", 4}, Export{"kernel32.dll", "ReleaseSemaphore", 3},
	Export{"kernel32.dll", "WaitForMultipleObjects", 4},
	Export{"kernel32.dll", "GetCurrentThread", 0}, Export{"kernel32.dll", "GetThreadPriority", 1}, Export{"kernel32.dll", "SuspendThread", 1},
	Export{"kernel32.dll", "GetThreadContext", 2}, Export{"kernel32.dll", "SetThreadContext", 2},
	Export{"kernel32.dll", "DeviceIoControl", 8}, Export{"kernel32.dll", "GetProcessAffinityMask", 3},
	Export{"kernel32.dll", "SetProcessAffinityMask", 2}, Export{"kernel32.dll", "GetSystemInfo", 1}, Export{"kernel32.dll", "VirtualQuery", 3},
	Export{"kernel32.dll", "DuplicateHandle", 7}, Export{"kernel32.dll", "VirtualQueryEx", 4}, Export{"kernel32.dll", "SetProcessDEPPolicy", 1},
	Export{"user32.dll", "ChangeDisplaySettingsExW", 5},
	Export{"user32.dll", "DisplayConfigGetDeviceInfo", 1}, Export{"user32.dll", "EnumDisplayDevicesA", 4},
	Export{"user32.dll", "SetProcessDPIAware", 0},
	Export{"gdi32.dll", "D3DKMTCloseAdapter", 1}, Export{"gdi32.dll", "D3DKMTCreateDCFromMemory", 1}, Export{"gdi32.dll", "D3DKMTCreateDevice", 1},
	Export{"gdi32.dll", "D3DKMTCreateKeyedMutex2", 1}, Export{"gdi32.dll", "D3DKMTDestroyAllocation", 1},
	Export{"gdi32.dll", "D3DKMTDestroyDCFromMemory", 1}, Export{"gdi32.dll", "D3DKMTDestroyDevice", 1},
	Export{"gdi32.dll", "D3DKMTDestroyKeyedMutex", 1}, Export{"gdi32.dll", "D3DKMTDestroySynchronizationObject", 1},
	Export{"gdi32.dll", "D3DKMTEscape", 1}, Export{"gdi32.dll", "D3DKMTOpenAdapterFromLuid", 1}, Export{"gdi32.dll", "D3DKMTOpenResource2", 1},
	Export{"gdi32.dll", "D3DKMTOpenResourceFromNtHandle", 1}, Export{"gdi32.dll", "D3DKMTOpenSyncObjectFromNtHandle", 1},
	Export{"gdi32.dll", "D3DKMTOpenSynchronizationObject", 1}, Export{"gdi32.dll", "D3DKMTQueryResourceInfo", 1},
	Export{"advapi32.dll", "AllocateLocallyUniqueId", 1}, Export{"advapi32.dll", "RegNotifyChangeKeyValue", 5},
	Export{"setupapi.dll", "SetupDiEnumDeviceInterfaces", 5}, Export{"setupapi.dll", "SetupDiGetClassDevsW", 4},
	Export{"setupapi.dll", "SetupDiGetDeviceInterfaceDetailW", 6}, Export{"setupapi.dll", "SetupDiOpenDevRegKey", 6},
};

class SemaphoreObject : public KernelObject {
public:
	std::u16string_view type_name() const override { return u"Semaphore"; }
	int signaled(uint32_t) const override { return count > 0; }
	void consume(uint32_t) override { --count; }
	int64_t count = 0, maximum = 0;
};

constexpr uint32_t wait_timeout = 0x102, wait_failed = UINT32_MAX, not_supported = 0xc00000bbU;
constexpr uint64_t current_thread_pseudo = 0xfffffffeULL;

}

NativeX86System::NativeX86System(NativeProcess &process, NativeEnvironment &environment, NativeObjects &objects, NativeThreads &threads,
	NativeMemory &memory)
	: process(process), environment(environment), objects(objects), threads(threads), memory(memory)
{
}

std::span<const NativeX86System::Export> NativeX86System::exports()
{
	return exported;
}

void NativeX86System::write32(uint64_t address, uint32_t value)
{
	const uint8_t bytes[4] = {uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16), uint8_t(value >> 24)};
	process.write_memory(address, bytes);
}

uint32_t NativeX86System::read32(uint64_t address)
{
	const auto bytes = process.read_memory(address, 4);
	return uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 | uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
}

uint32_t NativeX86System::wait(const std::vector<uint64_t> &handles, bool all, uint32_t milliseconds)
{
	const uint32_t id = threads.current_id();
	std::optional<std::chrono::steady_clock::time_point> wake;
	if (milliseconds != UINT32_MAX)
		wake = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
	WaitedObjects waited(objects, handles);
	const auto states = [&] {
		std::vector<int> result;
		for (size_t index = 0; index < handles.size(); ++index)
			result.push_back(waited.signaled(index, id));
		return result;
	};
	bool looked = false;
	for (;;) {
		const auto current = states();
		if (std::any_of(current.begin(), current.end(), [](int state) { return state < 0; })) {
			environment.set_last_error(6);
			return wait_failed;
		}
		if (all) {
			if (std::all_of(current.begin(), current.end(), [](int state) { return state == 1; })) {
				for (size_t index = 0; index < handles.size(); ++index)
					waited.consume(index, id);
				return 0;
			}
		} else {
			for (size_t index = 0; index < current.size(); ++index)
				if (current[index] == 1) {
					waited.consume(index, id);
					return uint32_t(index);
				}
		}
		const auto ready = [&] {
			const auto now = states();
			return all ? std::all_of(now.begin(), now.end(), [](int state) { return state == 1; })
					   : std::any_of(now.begin(), now.end(), [](int state) { return state == 1; });
		};
		if (milliseconds == 0 || (wake && std::chrono::steady_clock::now() >= *wake)) {
			// Once, a wait that has run out still takes in what the host finished meanwhile.
			if (!looked) {
				looked = true;
				if (threads.block(ready, std::chrono::steady_clock::now()))
					continue;
			}
			return wait_timeout;
		}
		// An object that turns signaled by itself, such as a timer, is looked at again when it says.
		auto until = wake;
		for (size_t index = 0; index < handles.size(); ++index)
			if (const auto due = waited.wakes_at(index); due && (!until || *due < *until))
				until = due;
		threads.block(ready, until);
	}
}

void NativeX86System::lock_exclusive(uint64_t address)
{
	auto &lock = locks[address];
	const uint32_t id = threads.current_id();
	if (lock.owner || lock.shared)
		threads.block([&lock] { return !lock.owner && !lock.shared; }, std::nullopt);
	lock.owner = id;
}

void NativeX86System::lock_shared(uint64_t address)
{
	auto &lock = locks[address];
	if (lock.owner)
		threads.block([&lock] { return !lock.owner; }, std::nullopt);
	++lock.shared;
}

bool NativeX86System::dispatch(std::string_view name)
{
	const auto known = std::find_if(exported.begin(), exported.end(), [&](const auto &item) { return item.name == name; });
	if (known == exported.end())
		return false;
	const auto args = process.api_arguments(known->words);
	const auto fail = [&](uint32_t error, uint64_t value = 0) {
		environment.set_last_error(error);
		process.return_api(value);
	};
	if (name == "InitializeSRWLock") {
		locks.erase(args[0]);
		process.return_api(0);
	} else if (name == "AcquireSRWLockExclusive") {
		lock_exclusive(args[0]);
		process.return_api(0);
	} else if (name == "AcquireSRWLockShared") {
		lock_shared(args[0]);
		process.return_api(0);
	} else if (name == "ReleaseSRWLockExclusive") {
		locks[args[0]].owner = 0;
		process.return_api(0);
	} else if (name == "ReleaseSRWLockShared") {
		auto &lock = locks[args[0]];
		if (lock.shared)
			--lock.shared;
		process.return_api(0);
	} else if (name == "TryAcquireSRWLockExclusive" || name == "TryAcquireSRWLockShared") {
		auto &lock = locks[args[0]];
		const bool exclusive = name == "TryAcquireSRWLockExclusive";
		const bool free = exclusive ? !lock.owner && !lock.shared : !lock.owner;
		if (free) {
			if (exclusive)
				lock.owner = threads.current_id();
			else
				++lock.shared;
		}
		process.return_api(free);
	} else if (name == "InitializeConditionVariable") {
		sleepers.erase(args[0]);
		process.return_api(0);
	} else if (name == "SleepConditionVariableSRW") {
		const bool shared = args[3] & 1;
		auto &lock = locks[args[1]];
		if (shared && lock.shared)
			--lock.shared;
		else
			lock.owner = 0;
		auto token = std::make_shared<bool>(false);
		sleepers[args[0]].push_back(token);
		std::optional<std::chrono::steady_clock::time_point> wake;
		if (uint32_t(args[2]) != UINT32_MAX)
			wake = std::chrono::steady_clock::now() + std::chrono::milliseconds(uint32_t(args[2]));
		const bool woken = threads.block([token] { return *token; }, wake);
		if (!woken) {
			auto &list = sleepers[args[0]];
			list.erase(std::remove(list.begin(), list.end(), token), list.end());
		}
		if (shared)
			lock_shared(args[1]);
		else
			lock_exclusive(args[1]);
		if (woken)
			process.return_api(1);
		else
			fail(1460);
	} else if (name == "WakeConditionVariable" || name == "WakeAllConditionVariable") {
		auto &list = sleepers[args[0]];
		const bool all = name == "WakeAllConditionVariable";
		while (!list.empty()) {
			*list.front() = true;
			list.erase(list.begin());
			if (!all)
				break;
		}
		process.return_api(0);
	} else if (name == "WaitOnAddress") {
		// The waiter wakes once the memory no longer holds the value it compared with, so the wake calls need do nothing.
		const auto size = uint32_t(args[2]);
		if (size != 1 && size != 2 && size != 4 && size != 8) {
			fail(87);
			return true;
		}
		const uint64_t address = uint32_t(args[0]);
		const auto expected = process.read_memory(uint32_t(args[1]), size);
		std::optional<std::chrono::steady_clock::time_point> wake;
		if (uint32_t(args[3]) != UINT32_MAX)
			wake = std::chrono::steady_clock::now() + std::chrono::milliseconds(uint32_t(args[3]));
		if (!threads.block([this, address, size, expected] { return process.read_memory(address, size) != expected; }, wake)) {
			fail(1460);
			return true;
		}
		process.return_api(1);
	} else if (name == "WakeByAddressSingle" || name == "WakeByAddressAll") {
		process.return_api(0);
	} else if (name == "TlsAlloc") {
		const auto free = std::find(tls_used.begin(), tls_used.end(), false);
		if (free == tls_used.end()) {
			fail(1168, UINT32_MAX);
		} else {
			*free = true;
			process.return_api(uint32_t(free - tls_used.begin()));
		}
	} else if (name == "TlsFree") {
		const auto index = uint32_t(args[0]);
		if (index >= tls_used.size() || !tls_used[index]) {
			fail(87);
		} else {
			tls_used[index] = false;
			for (auto &[thread, values] : tls_values)
				values.erase(index);
			if (index < 64) {
				const uint32_t zero = 0;
				process.write_memory(environment.teb() + 0xe10 + index * 4, std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(&zero), 4));
			}
			process.return_api(1);
		}
	} else if (name == "TlsGetValue") {
		const auto index = uint32_t(args[0]);
		if (index >= tls_used.size() || !tls_used[index]) {
			fail(87);
		} else {
			environment.set_last_error(0);
			const auto &values = tls_values[threads.current_id()];
			const auto found = values.find(index);
			process.return_api(found == values.end() ? 0 : found->second);
		}
	} else if (name == "TlsSetValue") {
		const auto index = uint32_t(args[0]);
		if (index >= tls_used.size() || !tls_used[index]) {
			fail(87);
		} else {
			tls_values[threads.current_id()][index] = uint32_t(args[1]);
			// The first 64 slots also live in the thread's TEB, TlsSlots at 0xe10, where code that reads its slot through fs: finds them, as
			// the .NET runtime's thread lookups do.
			if (index < 64) {
				const auto value = uint32_t(args[1]);
				process.write_memory(environment.teb() + 0xe10 + index * 4,
					std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(&value), 4));
			}
			process.return_api(1);
		}
	} else if (name == "CreateSemaphoreA" || name == "CreateSemaphoreW") {
		const int32_t initial = int32_t(args[1]), maximum = int32_t(args[2]);
		if (args[0] || maximum <= 0 || initial < 0 || initial > maximum) {
			fail(87);
		} else {
			auto semaphore = std::make_shared<SemaphoreObject>();
			semaphore->count = initial;
			semaphore->maximum = maximum;
			const auto made = objects.add_object(semaphore);
			if (made.status)
				fail(8);
			else
				process.return_api(made.value);
		}
	} else if (name == "ReleaseSemaphore") {
		const auto semaphore = objects.object_as<SemaphoreObject>(args[0]);
		const int32_t amount = int32_t(args[1]);
		if (!semaphore) {
			fail(6);
		} else if (amount <= 0 || semaphore->count + amount > semaphore->maximum) {
			fail(298);
		} else {
			try {
				if (args[2])
					write32(args[2], uint32_t(semaphore->count));
			} catch (const std::runtime_error &) {
				fail(998);
				return true;
			}
			semaphore->count += amount;
			process.return_api(1);
		}
	} else if (name == "WaitForMultipleObjects") {
		const uint32_t count = uint32_t(args[0]);
		if (count == 0 || count > 64 || !args[1]) {
			fail(87, wait_failed);
		} else {
			std::vector<uint64_t> handles(count);
			for (uint32_t index = 0; index < count; ++index)
				handles[index] = read32(args[1] + index * 4ULL);
			process.return_api(wait(handles, args[2] != 0, uint32_t(args[3])));
		}
	} else if (name == "GetCurrentThread") {
		process.return_api(current_thread_pseudo);
	} else if (name == "GetThreadPriority") {
		process.return_api(0);
	} else if (name == "SuspendThread") {
		const auto thread = objects.object_as<ThreadObject>(args[0]);
		if (!thread) {
			fail(6, UINT32_MAX);
		} else {
			const auto previous = threads.suspend(*thread->thread);
			if (previous == UINT32_MAX)
				fail(5, UINT32_MAX);
			else
				process.return_api(previous);
		}
	} else if (name == "GetThreadContext" || name == "SetThreadContext") {
		fail(50);
	} else if (name == "DeviceIoControl") {
		fail(1);
	} else if (name == "GetProcessAffinityMask") {
		if (args[1])
			write32(args[1], 0xf);
		if (args[2])
			write32(args[2], 0xf);
		process.return_api(1);
	} else if (name == "SetProcessAffinityMask") {
		process.return_api(1);
	} else if (name == "GetSystemInfo") {
		std::vector<uint8_t> info(36);
		const uint32_t fields[] = {0, 4096, 0x10000, 0x7ffeffff, 0xf, 4, 586, 65536, 0x0a000006};
		for (size_t index = 0; index < 9; ++index)
			for (size_t byte = 0; byte < 4; ++byte)
				info[index * 4 + byte] = uint8_t(fields[index] >> (byte * 8));
		process.write_memory(args[0], info);
		process.return_api(0);
	} else if (name == "VirtualQuery" || name == "VirtualQueryEx") {
		// The shared process API queries other processes through their owner's channel.
		const bool other = name == "VirtualQueryEx" && args[0] != 0xffffffffULL;
		if (other)
			return false;
		const auto address = name == "VirtualQuery" ? args[0] : args[1], buffer = name == "VirtualQuery" ? args[1] : args[2];
		const auto length = name == "VirtualQuery" ? args[2] : args[3];
		const auto region = memory.query(address);
		if (!region || length < 28) {
			fail(87);
		} else {
			const auto &raw = *region;
			const auto word = [&](size_t offset) { return uint32_t(raw[offset]) | uint32_t(raw[offset + 1]) << 8 | uint32_t(raw[offset + 2]) << 16 | uint32_t(raw[offset + 3]) << 24; };
			std::vector<uint8_t> info(28);
			const uint32_t fields[] = {word(0), word(8), word(16), word(24), word(32), word(36), word(40)};
			for (size_t index = 0; index < 7; ++index)
				for (size_t byte = 0; byte < 4; ++byte)
					info[index * 4 + byte] = uint8_t(fields[index] >> (byte * 8));
			process.write_memory(buffer, info);
			process.return_api(28);
		}
	} else if (name == "SetProcessDEPPolicy") {
		// Data execution prevention is always on for these programs, as on 64-bit Windows; asking for it succeeds.
		process.return_api(1);
	} else if (name == "DuplicateHandle") {
		// The current-thread pseudo handle duplicates into a real handle to the calling thread.
		const auto made = args[1] == 0xfffffffeULL ? objects.add_object(std::make_shared<ThreadObject>(threads.find(threads.current_id())))
												   : objects.duplicate(args[1]);
		if (made.status) {
			fail(6);
		} else {
			// The copy is inherited by child processes when asked, and DUPLICATE_CLOSE_SOURCE closes the original.
			if (args[5])
				objects.set_inherit(made.value, true);
			if ((args[6] & 1) && args[1] < 0xfffffffaULL)
				objects.close(args[1]);
			if (args[3])
				write32(args[3], uint32_t(made.value));
			process.return_api(1);
		}
	} else if (name == "ChangeDisplaySettingsExW") {
		// A mode change is accepted without changing anything: the host shows the program's window at its own size.
		process.return_api(0);
	} else if (name == "DisplayConfigGetDeviceInfo") {
		process.return_api(50);
	} else if (name == "EnumDisplayDevicesA") {
		// One adapter, and one monitor on it.
		if (uint32_t(args[1]) != 0 || !args[2] || read32(args[2]) != 424) {
			process.return_api(0);
		} else {
			std::vector<uint8_t> device(424);
			const auto text = [&](size_t offset, std::string_view value) { std::copy(value.begin(), value.end(), device.begin() + offset); };
			device[0] = 424 & 0xff;
			device[1] = 424 >> 8;
			if (args[0]) {
				text(4, "\\\\.\\DISPLAY1\\Monitor0");
				text(36, "Raven Monitor");
				device[164] = 3;
				text(168, "MONITOR\\RAVEN0000");
			} else {
				text(4, "\\\\.\\DISPLAY1");
				text(36, "Raven Display");
				device[164] = 5;
				text(168, "PCI\\VEN_1AF4&DEV_1050");
			}
			process.write_memory(args[2], device);
			process.return_api(1);
		}
	} else if (name == "SetProcessDPIAware") {
		process.return_api(1);
	} else if (name.starts_with("D3DKMT")) {
		process.return_api(not_supported);
	} else if (name == "AllocateLocallyUniqueId") {
		write32(args[0], unique_counter++);
		write32(args[0] + 4, 0);
		process.return_api(1);
	} else if (name == "RegNotifyChangeKeyValue") {
		process.return_api(0);
	} else if (name == "SetupDiEnumDeviceInterfaces") {
		fail(259);
	} else if (name == "SetupDiGetClassDevsW" || name == "SetupDiOpenDevRegKey") {
		fail(50, UINT32_MAX);
	} else {
		fail(50);
	}
	return true;
}

}
