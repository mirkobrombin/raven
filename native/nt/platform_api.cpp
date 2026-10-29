#include <array>
#include <atomic>
#include <cstdio>
#include "elevation.h"
#include "hash.h"
#include "host_names.h"
#include "file_objects.h"
#include "kernel_context.h"
#include "system_exports.h"

#include <sys/random.h>
#include <sys/sysinfo.h>
#include "text.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <unistd.h>

namespace raven {

namespace {

constexpr uint32_t e_fail = 0x80004005U, e_handle = 0x80070006U, e_invalidarg = 0x80070057U, e_notimpl = 0x80004001U, s_ok = 0, s_false = 1;
constexpr uint32_t class_not_registered = 0x80040154U, class_string = 0x800401f3U;

// A CryptoAPI provider or hash handle is a small integer into these tables.
struct CryptState {
	struct Key {
		std::vector<uint32_t> modulus;
		uint32_t exponent;
		size_t length;
		std::vector<uint8_t> blob;
	};
	std::map<uint64_t, bool> providers;
	std::map<uint64_t, std::unique_ptr<Digest>> hashes;
	std::map<uint64_t, Key> keys;
	// Where each provider's enumeration of algorithms has got to.
	std::map<uint64_t, size_t> enumeration;
	uint64_t next = 0x1000;
};

// RSA public keys and signature checks for CryptImportKey and CryptVerifySignature. Numbers are little-endian 32-bit words; the only
// operation needed is s^e mod n with the public exponent, so multiplication and a shift-and-subtract remainder are enough.
using Words = std::vector<uint32_t>;

Words words_from_bytes(std::span<const uint8_t> little_endian)
{
	Words value((little_endian.size() + 3) / 4);
	for (size_t at = 0; at < little_endian.size(); ++at)
		value[at / 4] |= uint32_t(little_endian[at]) << (at % 4 * 8);
	return value;
}

// The remainder of value by modulus, one bit of the quotient at a time.
Words remainder(const Words &value, const Words &modulus)
{
	size_t bits = modulus.size() * 32;
	while (bits && !(modulus[(bits - 1) / 32] >> ((bits - 1) % 32) & 1))
		--bits;
	Words rest(modulus.size() + 1);
	const auto less = [&](const Words &left) {
		for (size_t at = left.size(); at-- > 0;) {
			const uint32_t right = at < modulus.size() ? modulus[at] : 0;
			if (left[at] != right)
				return left[at] < right;
		}
		return false;
	};
	for (size_t bit = value.size() * 32; bit-- > 0;) {
		uint32_t carry = value[bit / 32] >> (bit % 32) & 1;
		for (auto &word : rest) {
			const uint32_t next = word >> 31;
			word = word << 1 | carry;
			carry = next;
		}
		if (!less(rest)) {
			uint64_t borrow = 0;
			for (size_t at = 0; at < rest.size(); ++at) {
				const uint64_t difference = uint64_t(rest[at]) - (at < modulus.size() ? modulus[at] : 0) - borrow;
				rest[at] = uint32_t(difference);
				borrow = difference >> 63;
			}
		}
	}
	rest.resize(modulus.size());
	return rest;
}

Words multiply_mod(const Words &left, const Words &right, const Words &modulus)
{
	Words product(left.size() + right.size());
	for (size_t i = 0; i < left.size(); ++i) {
		uint64_t carry = 0;
		for (size_t j = 0; j < right.size(); ++j) {
			const uint64_t sum = uint64_t(left[i]) * right[j] + product[i + j] + carry;
			product[i + j] = uint32_t(sum);
			carry = sum >> 32;
		}
		product[i + right.size()] = uint32_t(carry);
	}
	return remainder(product, modulus);
}

Words power_mod(const Words &base, uint32_t exponent, const Words &modulus)
{
	Words result(modulus.size());
	result[0] = 1;
	result = remainder(result, modulus);
	for (int bit = 31; bit >= 0; --bit) {
		result = multiply_mod(result, result, modulus);
		if (exponent >> bit & 1)
			result = multiply_mod(result, base, modulus);
	}
	return result;
}

// The DER prefix of a PKCS #1 DigestInfo for a digest of each length: MD5, SHA-1, SHA-256, SHA-384 and SHA-512.
std::vector<uint8_t> digest_info(size_t size)
{
	switch (size) {
	case 16: return {0x30, 0x20, 0x30, 0x0c, 0x06, 0x08, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x02, 0x05, 0x05, 0x00, 0x04, 0x10};
	case 20: return {0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e, 0x03, 0x02, 0x1a, 0x05, 0x00, 0x04, 0x14};
	case 32: return {0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20};
	case 48: return {0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02, 0x05, 0x00, 0x04, 0x30};
	case 64: return {0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03, 0x05, 0x00, 0x04, 0x40};
	default: return {};
	}
}


// COM per thread: how often it was initialized and in which apartment, single-threaded or the multithreaded one.
struct ComState {
	struct Thread {
		int count = 0;
		bool multithreaded = false;
	};
	std::map<uint64_t, Thread> threads;
	uint64_t main_sta = 0;
	int mta_threads = 0;
};

}

void register_platform_api(ApiRegistry &registry, KernelContext &context)
{
	// System file protection: the system's own libraries in the system directories are protected, as Windows protects them, and
	// installers leave them alone; the runtime serves them whatever file is there.
	registry.add("SfcIsFileProtected", 2, [](ApiCall &call) {
		std::u16string path = call[1] ? call.wide(call[1]) : u"";
		for (auto &character : path)
			character = character == u'/' ? u'\\' : character >= u'A' && character <= u'Z' ? char16_t(character + 32) : character;
		const auto split = path.find_last_of(u'\\');
		const auto folder = split == path.npos ? std::u16string() : path.substr(0, split);
		std::string name;
		for (const auto character : path.substr(split == path.npos ? 0 : split + 1))
			name.push_back(character < 0x80 ? char(character) : '?');
		const bool system = folder.ends_with(u"\\windows\\system32") || folder.ends_with(u"\\windows\\syswow64");
		const auto names = system_library_names();
		const bool protected_file = system && std::find(names.begin(), names.end(), name) != names.end();
		if (!protected_file)
			call.set_last_error(2);
		call.ret(protected_file ? 1 : 0);
	});
	registry.add("SfcGetNextProtectedFile", 2, [](ApiCall &call) { call.fail(259, 0); });
	// Catalogs of signatures go to the catalog database, which the runtime does not keep: their files are trusted by their own signatures.
	registry.add("SfpInstallCatalog", 2, [](ApiCall &call) { call.ret(0); });
	registry.add("SfpDeleteCatalog", 1, [](ApiCall &call) { call.ret(0); });
	// DirectSound where the runtime has no sound device to give: Windows without an audio device answers so, and programs go on silent.
	// The 32-bit runtime's own DirectSound answers its entry points before these do.
	constexpr uint32_t no_driver = 0x88780078U;
	for (const auto *name : {"DirectSoundCreate", "DirectSoundCreate8", "DirectSoundCaptureCreate", "DirectSoundCaptureCreate8"})
		registry.add(name, 3, [](ApiCall &call) {
			if (call[1])
				call.store_pointer(call[1], 0);
			call.ret(no_driver);
		});
	for (const auto *name : {"DirectSoundEnumerateA", "DirectSoundEnumerateW", "DirectSoundCaptureEnumerateA", "DirectSoundCaptureEnumerateW"})
		registry.add(name, 2, [](ApiCall &call) { call.ret(0); });
	registry.add("DirectSoundFullDuplexCreate", 10, [](ApiCall &call) {
		for (const size_t index : {6, 7, 8})
			if (call[index])
				call.store_pointer(call[index], 0);
		call.ret(no_driver);
	});
	registry.add("GetDeviceID", 2, [](ApiCall &call) { call.ret(no_driver); });
	// System Restore is off, as on a Windows installation without restore points: the status says the service is disabled.
	for (const auto *name : {"SRSetRestorePointA", "SRSetRestorePointW", "SRSetRestorePoint"})
		registry.add(name, 2, [](ApiCall &call) {
			constexpr uint32_t service_disabled = 1058;
			if (call[1]) {
				call.store<uint32_t>(call[1], service_disabled);
				call.store<uint64_t>(call[1] + 4, 0);
			}
			call.set_last_error(service_disabled);
			call.ret(0);
		});
	// uxtheme: the visual styles are the window manager's (desktop/theme.cpp); buffered painting draws straight to the target.
	registry.add("EnableTheming", 1, [](ApiCall &call) { call.ret(s_ok); });
	registry.add("BeginBufferedPaint", 5, [](ApiCall &call) { call.ret(0); });
	registry.add("EndBufferedPaint", 2, [](ApiCall &call) { call.ret(s_ok); });
	registry.add("BeginBufferedAnimation", 8, [](ApiCall &call) { call.ret(0); });
	registry.add("EndBufferedAnimation", 2, [](ApiCall &call) { call.ret(s_ok); });
	registry.add("BufferedPaintStopAllAnimations", 1, [](ApiCall &call) { call.ret(s_ok); });
	registry.add("BufferedPaintRenderAnimation", 2, [](ApiCall &call) { call.ret(0); });

	// dwmapi: attribute reads are refused; setting attributes and the frame extended into the client area are the desktop's
	// (window_manager.cpp).
	registry.add("DwmGetWindowAttribute", 4, [](ApiCall &call) { call.ret(e_invalidarg); });
	// Composition is always on since Windows 8.
	registry.add("DwmIsCompositionEnabled", 1, [](ApiCall &call) {
		call.store<uint32_t>(call[0], 1);
		call.ret(s_ok);
	});
	// A hint that a window shows content WPF's renderer draws: the compositor here needs none, and WPF ignores the answer.
	registry.add("DwmAttachMilContent", 1, [](ApiCall &call) { call.ret(s_ok); });
	registry.add("DwmDetachMilContent", 1, [](ApiCall &call) { call.ret(s_ok); });
	registry.add("DwmFlush", 0, [](ApiCall &call) { call.ret(s_ok); });
	// GdiEntry13 is DdQueryDisplaySettingsUniqueness: a count the display driver raises on each mode change, which the Direct3D 9
	// runtime compares to notice one. The display here keeps its mode, so the count stays the same.
	registry.add("GdiEntry13", 0, [](ApiCall &call) { call.ret(1); });
	registry.add("DwmInvalidateIconicBitmaps", 1, [](ApiCall &call) { call.ret(s_ok); });
	registry.add("DwmGetCompositionTimingInfo", 2, [](ApiCall &call) { call.ret(0x80263001U); });

	// Ghosting replaces a hung window with a frame the user can move; it is not drawn here, so turning it off changes nothing. Reasons that
	// hold back a shutdown are kept by Windows only for its shutdown screen, which this desktop does not show.
	registry.add("DisableProcessWindowsGhosting", 0, [](ApiCall &call) { call.ret(0); });
	registry.add("ShutdownBlockReasonCreate", 2, [](ApiCall &call) { call.ret(1); });
	registry.add("ShutdownBlockReasonDestroy", 1, [](ApiCall &call) { call.ret(1); });

	// Owned pop-ups are shown and hidden with their owner by the window manager here already, and a logical palette has nothing to
	// realize on a true colour display; both succeed. Non-client areas are scaled with the window's DPI by the frame drawing itself.
	registry.add("ShowOwnedPopups", 2, [](ApiCall &call) { call.ret(1); });
	registry.add("UnrealizeObject", 1, [](ApiCall &call) { call.ret(1); });
	registry.add("EnableNonClientDpiScaling", 1, [](ApiCall &call) { call.ret(1); });

	// A clipboard listener is told when the clipboard changes; registering one succeeds, and a window not registered cannot be removed.
	registry.add("AddClipboardFormatListener", 1, [](ApiCall &call) { call.ret(call[0] ? 1 : 0); });
	registry.add("RemoveClipboardFormatListener", 1, [](ApiCall &call) { call.ret(call[0] ? 1 : 0); });

	// A system library Raven serves from the host has no COM classes of its own to hand out, as Windows answers for one it does not know.
	registry.add("DllGetClassObject", 3, [](ApiCall &call) {
		if (call[2])
			call.store_pointer(call[2], 0);
		call.ret(0x80040111U);
	});
	registry.add("DllCanUnloadNow", 0, [](ApiCall &call) { call.ret(1); });

	// The shell's desktop window belongs to Explorer, which does not run here; Windows answers no window without a shell.
	registry.add("GetShellWindow", 0, [](ApiCall &call) { call.ret(0); });
	// Synthesised input is refused, as User Interface Privilege Isolation refuses it for a window the caller may not drive. The window
	// composition attributes (blur and accent effects behind a window) are accepted and drawn as an ordinary frame.
	registry.add("SendInput", 3, [](ApiCall &call) { call.fail(5, 0); });
	registry.add("SetWindowCompositionAttribute", 2, [](ApiCall &call) { call.ret(1); });
	// PERFORMANCE_INFORMATION from the host's memory figures, in pages, with this process's handles and the host's process and thread counts.
	for (const char *name : {"K32GetPerformanceInfo", "GetPerformanceInfo"})
		registry.add(name, 2, [](ApiCall &call) {
			const size_t width = call.pointer_width(), size = width == 8 ? 104 : 56;
			if (!call[0] || call.u32(1) < size) {
				call.fail(24, 0);
				return;
			}
			struct sysinfo info {};
			sysinfo(&info);
			const uint64_t unit = info.mem_unit ? info.mem_unit : 1, page = 4096;
			const uint64_t total = info.totalram * unit / page, available = (info.freeram + info.bufferram) * unit / page;
			const uint64_t commit = total - available, limit = total + info.totalswap * unit / page;
			std::vector<uint8_t> bytes(size, 0);
			size_t at = 0;
			const auto put = [&](uint64_t value, size_t field) {
				std::memcpy(bytes.data() + at, &value, field);
				at += field;
			};
			put(size, 4);
			at = width == 8 ? 8 : 4;
			for (const uint64_t value : {commit, limit, commit, total, available, info.bufferram * unit / page, uint64_t(0), uint64_t(0), uint64_t(0), page})
				put(value, width);
			put(256, 4);
			put(info.procs, 4);
			put(info.procs * 4u, 4);
			call.write(call[0], bytes);
			call.ret(1);
		});

	// mpr, the network provider router: the machine has no network drives or shares, so an enumeration of the connected, remembered
	// or browsable resources opens and finds nothing, and a drive or path has no remote connection. Each function returns its error and
	// also sets it as the last error, which callers that read GetLastError take.
	const auto network_error = [](ApiCall &call, uint32_t error) { call.fail(error, error); };
	for (const char *name : {"WNetOpenEnumW", "WNetOpenEnumA"})
		registry.add(name, 5, [&context, network_error](ApiCall &call) {
			const uint32_t scope = call.u32(0);
			if (!call[4] || scope < 1 || scope > 5) {
				network_error(call, 87);
				return;
			}
			const auto made = context.objects.add_object(std::make_shared<FindObject>());
			if (made.status) {
				network_error(call, 8);
				return;
			}
			call.store_pointer(call[4], made.value);
			call.set_last_error(0);
			call.ret(0);
		});
	for (const char *name : {"WNetEnumResourceW", "WNetEnumResourceA"})
		registry.add(name, 4, [&context, network_error](ApiCall &call) {
			if (!context.objects.object_as<FindObject>(call[0])) {
				network_error(call, 6);
				return;
			}
			if (call[1])
				call.store<uint32_t>(call[1], 0);
			network_error(call, 259);
		});
	registry.add("WNetCloseEnum", 1, [&context, network_error](ApiCall &call) {
		if (!context.objects.object_as<FindObject>(call[0]) || context.objects.close(call[0])) {
			network_error(call, 6);
			return;
		}
		call.set_last_error(0);
		call.ret(0);
	});
	for (const char *name : {"WNetGetConnectionW", "WNetGetConnectionA"})
		registry.add(name, 3, [](ApiCall &call) { call.ret(2250); });
	for (const char *name : {"WNetGetUniversalNameW", "WNetGetUniversalNameA"})
		registry.add(name, 4, [](ApiCall &call) { call.ret(2250); });

	// loadperf, what lodctr and unlodctr run on: the names and help texts of a service's performance counters. No counters are kept
	// here, so adding and removing them succeeds and changes nothing.
	for (const char *name : {"LoadPerfCounterTextStringsA", "LoadPerfCounterTextStringsW", "UnloadPerfCounterTextStringsA", "UnloadPerfCounterTextStringsW"})
		registry.add(name, 2, [](ApiCall &call) { call.ret(0); });
	for (const char *name : {"InstallPerfDllA", "InstallPerfDllW"})
		registry.add(name, 3, [](ApiCall &call) { call.ret(0); });

	// imm32: there is no input method, so contexts do not exist and composition calls fail.
	registry.add("ImmGetContext", 1, [](ApiCall &call) { call.ret(0); });
	registry.add("ImmReleaseContext", 2, [](ApiCall &call) { call.ret(1); });
	registry.add("ImmCreateContext", 0, [](ApiCall &call) { call.ret(0); });
	registry.add("ImmDestroyContext", 1, [](ApiCall &call) { call.ret(0); });
	registry.add("ImmAssociateContext", 2, [](ApiCall &call) { call.ret(0); });
	registry.add("ImmAssociateContextEx", 3, [](ApiCall &call) { call.ret(0); });
	registry.add("ImmGetDefaultIMEWnd", 1, [](ApiCall &call) { call.ret(0); });
	registry.add("ImmIsIME", 1, [](ApiCall &call) { call.ret(0); });
	// Turning input methods off for a thread succeeds: there are none to turn off.
	registry.add("ImmDisableIME", 1, [](ApiCall &call) { call.ret(1); });
	registry.add("ImmGetCompositionStringW", 4, [](ApiCall &call) { call.ret(uint32_t(-1)); });
	registry.add("ImmSetCompositionStringW", 6, [](ApiCall &call) { call.ret(0); });
	registry.add("ImmSetCompositionWindow", 2, [](ApiCall &call) { call.ret(0); });
	registry.add("ImmSetCompositionFontW", 2, [](ApiCall &call) { call.ret(0); });
	registry.add("ImmSetCandidateWindow", 2, [](ApiCall &call) { call.ret(0); });
	registry.add("ImmNotifyIME", 4, [](ApiCall &call) { call.ret(0); });
	registry.add("ImmEscapeW", 4, [](ApiCall &call) { call.ret(0); });
	registry.add("ImmGetOpenStatus", 1, [](ApiCall &call) { call.ret(0); });
	registry.add("ImmSetOpenStatus", 2, [](ApiCall &call) { call.ret(0); });
	registry.add("ImmGetConversionStatus", 3, [](ApiCall &call) { call.ret(0); });

	// Network and certificate services: no network is reported, and certificates are found nowhere else than where crypt32 keeps them.
	registry.add("IsNetworkAlive", 1, [](ApiCall &call) {
		if (call[0]) call.store<uint32_t>(call[0], 0);
		call.ret(0);
	});
	registry.add("IsDestinationReachableW", 2, [](ApiCall &call) { call.ret(0); });

	// The Restart Manager, which installers ask which processes hold the files they replace: sessions are kept with their keys, and no
	// process is reported holding a registered file, so there is nothing to shut down or restart. The functions answer with error codes.
	struct RestartSessions {
		std::mutex lock;
		uint32_t next = 0;
		std::map<uint32_t, std::u16string> keys;
	};
	auto sessions = std::make_shared<RestartSessions>();
	const auto known = [sessions](uint32_t handle) {
		std::lock_guard guard(sessions->lock);
		return sessions->keys.contains(handle);
	};
	registry.add("RmStartSession", 3, [sessions](ApiCall &call) {
		if (!call[0] || !call[2]) {
			call.ret(87);
			return;
		}
		uint8_t random[16];
		if (getrandom(random, sizeof random, 0) != sizeof random) {
			call.ret(29);
			return;
		}
		std::u16string key;
		for (const auto byte : random)
			for (const int shift : {4, 0})
				key += u"0123456789abcdef"[(byte >> shift) & 15];
		std::lock_guard guard(sessions->lock);
		const uint32_t handle = sessions->next++;
		sessions->keys[handle] = key;
		call.store<uint32_t>(call[0], handle);
		call.put_wide(call[2], key, key.size() + 1);
		call.ret(0);
	});
	registry.add("RmJoinSession", 2, [sessions](ApiCall &call) {
		if (!call[0] || !call[1]) {
			call.ret(87);
			return;
		}
		const auto key = call.wide(call[1], 33);
		std::lock_guard guard(sessions->lock);
		if (std::none_of(sessions->keys.begin(), sessions->keys.end(), [&](const auto &item) { return item.second == key; })) {
			call.ret(87);
			return;
		}
		const uint32_t handle = sessions->next++;
		sessions->keys[handle] = key;
		call.store<uint32_t>(call[0], handle);
		call.ret(0);
	});
	registry.add("RmEndSession", 1, [sessions](ApiCall &call) {
		std::lock_guard guard(sessions->lock);
		call.ret(sessions->keys.erase(call.u32(0)) ? 0 : 6);
	});
	registry.add("RmGetList", 5, [known](ApiCall &call) {
		if (!known(call.u32(0))) {
			call.ret(6);
			return;
		}
		if (!call[1] || !call[2]) {
			call.ret(87);
			return;
		}
		call.store<uint32_t>(call[1], 0);
		call.store<uint32_t>(call[2], 0);
		if (call[4])
			call.store<uint32_t>(call[4], 0);
		call.ret(0);
	});
	registry.add("RmGetFilterList", 4, [known](ApiCall &call) {
		if (!known(call.u32(0))) {
			call.ret(6);
			return;
		}
		if (call[3])
			call.store<uint32_t>(call[3], 0);
		call.ret(0);
	});
	for (const auto &[name, words] : std::initializer_list<std::pair<const char *, uint32_t>>{{"RmRegisterResources", 7}, {"RmShutdown", 3},
		{"RmRestart", 3}, {"RmCancelCurrentTask", 1}, {"RmAddFilter", 5}, {"RmRemoveFilter", 4}})
		registry.add(name, words, [known](ApiCall &call) { call.ret(known(call.u32(0)) ? 0 : 6); });
	registry.add("ImageNtHeader", 1, [](ApiCall &call) {
		const auto offset = call.load<uint32_t>(call[0] + 0x3c);
		call.ret(call[0] + offset);
	});

#ifndef RAVEN_OPENSSL
	// wininet: URL cracking is pure text processing; with the HTTP client the WinINet layer cracks URLs in both character sets.
	registry.add("InternetCrackUrlW", 4, [](ApiCall &call) {
		const auto url = call.wide(call[0], 4096);
		const auto scheme_end = url.find(u"://");
		if (scheme_end == url.npos || !call[3]) {
			call.fail(12006);
			return;
		}
		const auto scheme = url.substr(0, scheme_end);
		const auto host_start = scheme_end + 3;
		const auto path_start = url.find_first_of(u"/?#", host_start);
		auto authority = url.substr(host_start, path_start == url.npos ? url.npos : path_start - host_start);
		std::u16string user, password;
		if (const auto at = authority.rfind(u'@'); at != authority.npos) {
			const auto credentials = authority.substr(0, at);
			authority = authority.substr(at + 1);
			const auto colon = credentials.find(u':');
			user = credentials.substr(0, colon);
			password = colon == credentials.npos ? u"" : credentials.substr(colon + 1);
		}
		uint32_t port = scheme == u"https" ? 443 : scheme == u"ftp" ? 21 : 80;
		std::u16string host = authority;
		if (const auto colon = authority.rfind(u':'); colon != authority.npos && authority.find(u']') == authority.npos) {
			host = authority.substr(0, colon);
			port = uint32_t(std::stoul(to_utf8(authority.substr(colon + 1))));
		}
		const auto rest = path_start == url.npos ? std::u16string() : url.substr(path_start);
		const auto extra_at = rest.find_first_of(u"?#");
		const auto path = rest.substr(0, extra_at);
		const auto extra = extra_at == rest.npos ? std::u16string() : rest.substr(extra_at);
		// URL_COMPONENTSW: size, scheme ptr/len, scheme id, host ptr/len, port, user ptr/len, password ptr/len, path ptr/len, extra ptr/len.
		const uint64_t components = call[3];
		const auto fill = [&](uint64_t pointer_at, uint64_t length_at, const std::u16string &value) {
			const uint64_t buffer = call.load<uint64_t>(components + pointer_at);
			const uint32_t capacity = call.load<uint32_t>(components + length_at);
			if (buffer) {
				if (capacity <= value.size()) {
					call.fail(122);
					return false;
				}
				call.put_wide(buffer, value, capacity);
			} else if (value.empty()) {
				call.store<uint64_t>(components + pointer_at, 0);
			} else {
				call.store<uint64_t>(components + pointer_at, call[0] + (url.size() - (url.size() - url.find(value))) * 2);
			}
			call.store<uint32_t>(components + length_at, uint32_t(value.size()));
			return true;
		};
		if (!fill(8, 16, scheme) || !fill(24, 32, host) || !fill(48, 56, user) || !fill(64, 72, password) || !fill(80, 88, path) || !fill(96, 104, extra))
			return;
		call.store<uint32_t>(components + 20, scheme == u"https" ? 2 : scheme == u"http" ? 3 : scheme == u"ftp" ? 1 : 0);
		call.store<uint32_t>(components + 40, port & 0xffff);
		call.ret(1);
	});
#endif

	// COM: no class registry exists, so object creation reports "class not registered".
	auto com = std::make_shared<ComState>();
	// A thread joins an apartment on its first initialization; asking for the other kind later fails with RPC_E_CHANGED_MODE.
	// Programs also read the apartment from the thread's OLE data, which TEB.ReservedForOle points at: its flags word (after two
	// pointers and the apartment id) has OLETLS_APARTMENTTHREADED (0x80), or OLETLS_MULTITHREADED with OLETLS_DISABLE_OLE1DDE (0x140).
	const auto ole_flags = [&context](ApiCall &call, uint32_t flags) {
		const size_t width = call.pointer_width();
		const uint64_t slot = context.environment.teb() + (width == 4 ? 0xf80 : 0x1758);
		uint64_t data = call.load_pointer(slot);
		if (!data) {
			data = context.allocator.allocate(0x100, true);
			if (!data)
				return;
			call.store_pointer(slot, data);
		}
		call.store<uint32_t>(data + 2 * width + 4, flags);
	};
	const auto initialize = [com, &context, ole_flags](ApiCall &call, bool multithreaded, bool no_dde) {
		const uint64_t thread = context.environment.teb();
		auto &state = com->threads[thread];
		if (state.count && state.multithreaded != multithreaded) {
			call.ret(0x80010106U);
			return;
		}
		if (!state.count++) {
			state.multithreaded = multithreaded;
			if (multithreaded)
				++com->mta_threads;
			else if (!com->main_sta)
				com->main_sta = thread;
			ole_flags(call, multithreaded ? 0x140U : no_dde ? 0xc0U : 0x80U);
			call.ret(s_ok);
			return;
		}
		call.ret(s_false);
	};
	registry.add("CoInitialize", 1, [initialize](ApiCall &call) { initialize(call, false, false); });
	registry.add("CoInitializeEx", 2, [initialize](ApiCall &call) { initialize(call, !(call.u32(1) & 2), call.u32(1) & 4); });
	registry.add("OleInitialize", 1, [initialize](ApiCall &call) { initialize(call, false, false); });
	// CoInitializeSecurity sets the process's COM security once: the first call succeeds and any later one is too late, as on Windows.
	registry.add("CoInitializeSecurity", 9, [](ApiCall &call) {
		static std::atomic<bool> done{false};
		call.ret(done.exchange(true) ? 0x80010119U : s_ok);
	});
	const auto uninitialize = [com, &context, ole_flags](ApiCall &call) {
		const uint64_t thread = context.environment.teb();
		auto found = com->threads.find(thread);
		if (found == com->threads.end() || !found->second.count)
			return;
		if (!--found->second.count) {
			if (found->second.multithreaded)
				--com->mta_threads;
			else if (com->main_sta == thread)
				com->main_sta = 0;
			com->threads.erase(found);
			ole_flags(call, 0);
		}
	};
	// Message filters belong to single-threaded apartments; there is no filter before the first one, so the previous one is null.
	registry.add("CoRegisterMessageFilter", 2, [com, &context](ApiCall &call) {
		const auto found = com->threads.find(context.environment.teb());
		if (call[1])
			call.store_pointer(call[1], 0);
		if (found == com->threads.end() || !found->second.count || found->second.multithreaded) {
			call.ret(0x80004021U);
			return;
		}
		call.ret(s_ok);
	});
	// No activation is ever handed to a filter, so registering one succeeds and it is never consulted.
	registry.add("CoRegisterActivationFilter", 1, [](ApiCall &call) { call.ret(call[0] ? s_ok : 0x80070057U); });
	// The multithreaded apartment stays alive while a usage cookie is out; the cookie names nothing beyond itself.
	registry.add("CoIncrementMTAUsage", 1, [](ApiCall &call) {
		if (!call[0]) {
			call.ret(0x80070057U);
			return;
		}
		call.store_pointer(call[0], 1);
		call.ret(s_ok);
	});
	registry.add("CoDecrementMTAUsage", 1, [](ApiCall &call) { call.ret(call[0] ? s_ok : 0x80070057U); });
	// The lock keeps an object alive for clients in other processes. Nothing here hands an object outside the process, so its own references
	// are all that keep it, and locking or unlocking leaves it as it is.
	registry.add("CoLockObjectExternal", 3, [](ApiCall &call) { call.ret(call[0] ? s_ok : 0x80070057U); });
	// APTTYPE: STA 0, MTA 1, main STA 3. A thread that never initialized COM is in the multithreaded apartment implicitly while any
	// thread of the process holds it (qualifier APTTYPEQUALIFIER_IMPLICIT_MTA).
	registry.add("CoGetApartmentType", 2, [com, &context](ApiCall &call) {
		if (!call[0] || !call[1]) {
			call.ret(e_invalidarg);
			return;
		}
		const uint64_t thread = context.environment.teb();
		const auto found = com->threads.find(thread);
		uint32_t type = 0, qualifier = 0;
		if (found != com->threads.end() && found->second.count) {
			type = found->second.multithreaded ? 1 : com->main_sta == thread ? 3 : 0;
		} else if (com->mta_threads) {
			type = 1;
			qualifier = 1;
		} else {
			call.store<uint32_t>(call[0], 0xffffffffU);
			call.store<uint32_t>(call[1], 0);
			call.ret(0x800401f0U);
			return;
		}
		call.store<uint32_t>(call[0], type);
		call.store<uint32_t>(call[1], qualifier);
		call.ret(s_ok);
	});
	// CoWaitForMultipleHandles(flags, timeout, count, handles, index): a wait for one handle (or, with COWAIT_WAITALL, all) that may be
	// alertable (COWAIT_ALERTABLE). S_OK with the handle's index, or RPC_S_CALLPENDING when the time runs out. The messages of a
	// single-threaded apartment are not pumped meanwhile.
	registry.add("CoWaitForMultipleHandles", 5, [&registry](ApiCall &call) {
		if (!call[3] || !call[4] || !call.u32(2) || (call.u32(0) & ~7U)) {
			call.ret(e_invalidarg);
			return;
		}
		const auto wait = registry.find("WaitForMultipleObjectsEx");
		ApiCall inner(call.process, call.environment, {call.u32(2), call[3], call.u32(0) & 1, call.u32(1), (call.u32(0) & 2) ? 1U : 0U});
		wait->second(inner);
		const uint32_t result = uint32_t(inner.result);
		if (result == 0x102) {
			call.ret(0x80010115U);
			return;
		}
		if (result == 0xffffffffU) {
			call.ret(0x80070000U | (call.last_error() & 0xffff));
			return;
		}
		// The index is the wait's own answer: the handle's position, WAIT_ABANDONED_0 plus it or WAIT_IO_COMPLETION.
		call.store<uint32_t>(call[4], result);
		call.ret(s_ok);
	});
	registry.add("CoUninitialize", 0, uninitialize);
	registry.add("OleUninitialize", 0, uninitialize);
	// The dialog a program shows when the application it calls into is busy: no other application serves its calls here, so the
	// answer is to try again at once (OLEUI_BZ_RETRYNOW).
	for (const char *name : {"OleUIBusyW", "OleUIBusyA"})
		registry.add(name, 1, [](ApiCall &call) { call.ret(118); });
	registry.add("CoCreateInstance", 5, [](ApiCall &call) {
		if (std::getenv("RAVEN_TRACE_API")) {
			const auto bytes = call.read(call[0], 16);
			std::fprintf(stderr, "com class");
			for (const auto byte : bytes) std::fprintf(stderr, " %02x", byte);
			std::fprintf(stderr, "\n");
		}
		if (call[4]) call.store_pointer(call[4], 0);
		call.ret(class_not_registered);
	});
	registry.add("CoGetClassObject", 5, [](ApiCall &call) {
		if (call[4]) call.store_pointer(call[4], 0);
		call.ret(class_not_registered);
	});
	registry.add("CLSIDFromProgID", 2, [](ApiCall &call) { call.ret(class_string); });
	// A GUID in its registry form, "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}", as the 16 bytes of the structure; nothing for any other text.
	const auto parse_guid = [](std::u16string_view text) -> std::optional<std::vector<uint8_t>> {
		static constexpr size_t digits[16][2] = {{7, 1}, {5, 1}, {3, 1}, {1, 1}, {12, 1}, {10, 1}, {17, 1}, {15, 1}, {20, 1}, {22, 1}, {25, 1}, {27, 1},
			{29, 1}, {31, 1}, {33, 1}, {35, 1}};
		if (text.size() != 38 || text[0] != u'{' || text[37] != u'}' || text[9] != u'-' || text[14] != u'-' || text[19] != u'-' || text[24] != u'-')
			return std::nullopt;
		const auto nibble = [](char16_t c) -> int {
			if (c >= u'0' && c <= u'9') return c - u'0';
			if (c >= u'a' && c <= u'f') return c - u'a' + 10;
			if (c >= u'A' && c <= u'F') return c - u'A' + 10;
			return -1;
		};
		std::vector<uint8_t> bytes(16);
		for (size_t index = 0; index < 16; ++index) {
			const int high = nibble(text[digits[index][0]]), low = nibble(text[digits[index][0] + digits[index][1]]);
			if (high < 0 || low < 0)
				return std::nullopt;
			bytes[index] = uint8_t(high << 4 | low);
		}
		return bytes;
	};
	// An empty or null text is the null class; a text that is not a GUID would be a program identifier, which no registered class answers.
	registry.add("CLSIDFromString", 2, [parse_guid](ApiCall &call) {
		const auto text = call[0] ? call.wide(call[0], 64) : std::u16string();
		if (text.empty()) {
			call.write(call[1], std::vector<uint8_t>(16));
			call.ret(0);
			return;
		}
		const auto bytes = parse_guid(text);
		if (!bytes) {
			call.write(call[1], std::vector<uint8_t>(16));
			call.ret(class_string);
			return;
		}
		call.write(call[1], *bytes);
		call.ret(0);
	});
	// A random GUID of version 4 with the RFC 4122 variant, as CoCreateGuid makes.
	registry.add("CoCreateGuid", 1, [](ApiCall &call) {
		std::vector<uint8_t> bytes(16);
		if (!call[0] || ::getrandom(bytes.data(), bytes.size(), 0) != ssize_t(bytes.size())) {
			call.ret(0x80070057U);
			return;
		}
		bytes[7] = uint8_t((bytes[7] & 0x0f) | 0x40);
		bytes[8] = uint8_t((bytes[8] & 0x3f) | 0x80);
		call.write(call[0], bytes);
		call.ret(0);
	});
	registry.add("IIDFromString", 2, [parse_guid](ApiCall &call) {
		const auto text = call[0] ? call.wide(call[0], 64) : std::u16string();
		const auto bytes = text.empty() ? std::optional(std::vector<uint8_t>(16)) : parse_guid(text);
		if (!bytes) {
			call.ret(0x80070057U);
			return;
		}
		call.write(call[1], *bytes);
		call.ret(0);
	});
	// The registry form of a GUID, "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}", from its sixteen bytes.
	const auto guid_text = [](const std::vector<uint8_t> &bytes) {
		static constexpr int order[16] = {3, 2, 1, 0, -1, 5, 4, -1, 7, 6, -1, 8, 9, -1, 10, 11};
		char text[40];
		int at = 0;
		text[at++] = '{';
		for (const int index : order) {
			if (index < 0) {
				text[at++] = '-';
				continue;
			}
			at += std::snprintf(text + at, 3, "%02X", bytes[size_t(index)]);
		}
		for (size_t index = 12; index < 16; ++index)
			at += std::snprintf(text + at, 3, "%02X", bytes[index]);
		text[at++] = '}';
		return widen_ascii(std::string_view(text, size_t(at)));
	};
	registry.add("StringFromGUID2", 3, [guid_text](ApiCall &call) {
		if (call.u32(2) < 39) {
			call.ret(0);
			return;
		}
		call.put_wide(call[1], guid_text(call.read(call[0], 16)), 39);
		call.ret(39);
	});
	// The same text in memory from the task allocator, which the caller frees with CoTaskMemFree.
	for (const char *name : {"StringFromCLSID", "StringFromIID"})
		registry.add(name, 2, [&context, guid_text](ApiCall &call) {
			const auto memory = context.allocator.allocate(39 * sizeof(char16_t), false);
			if (!memory) {
				call.store_pointer(call[1], 0);
				call.ret(0x8007000eU);
				return;
			}
			call.put_wide(memory, guid_text(call.read(call[0], 16)), 39);
			call.store_pointer(call[1], memory);
			call.ret(0);
		});
	registry.add("CoTaskMemAlloc", 1, [&context](ApiCall &call) { call.ret(context.allocator.allocate(call[0], false)); });
	registry.add("CoTaskMemFree", 1, [&context](ApiCall &call) {
		if (call[0]) context.allocator.release(call[0]);
	});
	registry.add("CoTaskMemRealloc", 2, [&context](ApiCall &call) {
		if (!call[0]) {
			call.ret(context.allocator.allocate(call[1], false));
			return;
		}
		const auto old = context.allocator.size(call[0]);
		const auto address = context.allocator.allocate(call[1], false);
		if (address && old != UINT64_MAX)
			call.write(address, call.read(call[0], std::min<uint64_t>(old, call[1])));
		if (address)
			context.allocator.release(call[0]);
		call.ret(address);
	});
	registry.add("RegisterDragDrop", 2, [](ApiCall &call) { call.ret(s_ok); });
	registry.add("RevokeDragDrop", 1, [](ApiCall &call) { call.ret(s_ok); });
	registry.add("DoDragDrop", 4, [](ApiCall &call) {
		if (call[3]) call.store<uint32_t>(call[3], 0);
		call.ret(0x40101);
	});
	registry.add("ReleaseStgMedium", 1, [](ApiCall &) {});
	registry.add("OleFlushClipboard", 0, [](ApiCall &call) { call.ret(s_ok); });
	// No data object is ever put on the clipboard, so none is the current one.
	registry.add("OleIsCurrentClipboard", 1, [](ApiCall &call) { call.ret(1); });
	registry.add("OleGetClipboard", 1, [](ApiCall &call) {
		if (call[0]) call.store_pointer(call[0], 0);
		call.ret(e_notimpl);
	});

	// BSTRs: a four-byte byte count precedes the characters, which are zero-terminated.
	const auto make_bstr = [&context](ApiCall &call, const uint8_t *bytes, size_t length) {
		const auto block = context.allocator.allocate(length + 4 + 2, false);
		if (!block) {
			call.ret(0);
			return;
		}
		std::vector<uint8_t> data(length + 6);
		std::memcpy(data.data(), &(const uint32_t &)(uint32_t(length)), 4);
		if (bytes)
			std::memcpy(data.data() + 4, bytes, length);
		call.write(block, data);
		call.ret(block + 4);
	};
	registry.add("SysAllocString", 1, [make_bstr](ApiCall &call) {
		if (!call[0]) {
			call.ret(0);
			return;
		}
		const auto text = call.wide(call[0], 1 << 24);
		make_bstr(call, reinterpret_cast<const uint8_t *>(text.data()), text.size() * 2);
	});
	registry.add("SysAllocStringLen", 2, [make_bstr](ApiCall &call) {
		const size_t count = call.u32(1);
		std::vector<uint8_t> bytes(count * 2);
		if (call[0] && count)
			bytes = call.read(call[0], count * 2);
		make_bstr(call, bytes.data(), bytes.size());
	});
	registry.add("SysAllocStringByteLen", 2, [make_bstr](ApiCall &call) {
		const size_t count = call.u32(1);
		std::vector<uint8_t> bytes(count);
		if (call[0] && count)
			bytes = call.read(call[0], count);
		make_bstr(call, bytes.data(), bytes.size());
	});
	registry.add("SysFreeString", 1, [&context](ApiCall &call) {
		if (call[0]) context.allocator.release(call[0] - 4);
	});
	// A VARIANT is 16 bytes in a 32-bit program and 24 in a 64-bit one: the type, three reserved words and the value, which may hold a pointer.
	registry.add("VariantInit", 1, [](ApiCall &call) {
		if (!call[0])
			return;
		call.write(call[0], std::vector<uint8_t>(call.pointer_width() == 4 ? 16 : 24));
	});
	registry.add("VariantClear", 1, [&context, &registry](ApiCall &call) {
		constexpr uint16_t vt_bstr = 8, vt_dispatch = 9, vt_unknown = 13, vt_byref = 0x4000, vt_array = 0x2000;
		if (!call[0]) {
			call.ret(0x80070057U);
			return;
		}
		const uint16_t type = call.load<uint16_t>(call[0]);
		const auto value = call.load_pointer(call[0] + 8);
		if (!(type & (vt_byref | vt_array)) && type == vt_bstr && value) {
			context.allocator.release(value - 4);
		} else if (!(type & (vt_byref | vt_array)) && (type == vt_dispatch || type == vt_unknown) && value) {
			// The object is told its reference is gone: Release is the third method of every interface.
			const auto table = call.load_pointer(value);
			const auto release = call.load_pointer(table + 2 * call.pointer_width());
			const uint64_t arguments[] = {value};
			context.call(release, arguments);
		}
		call.write(call[0], std::vector<uint8_t>(2));
		call.ret(0);
	});
	registry.add("SysStringLen", 1, [](ApiCall &call) { call.ret(call[0] ? call.load<uint32_t>(call[0] - 4) / 2 : 0); });
	registry.add("SysStringByteLen", 1, [](ApiCall &call) { call.ret(call[0] ? call.load<uint32_t>(call[0] - 4) : 0); });
	// SysReAllocString replaces the string a BSTR variable holds: the new copy is made before the old one is released.
	const auto reallocate = [&context, make_bstr](ApiCall &call, bool counted) {
		if (!call[0]) {
			call.fail(87);
			return;
		}
		const uint64_t old = call.load_pointer(call[0]);
		std::vector<uint8_t> bytes;
		if (call[1]) {
			const size_t count = counted ? call.u32(2) : call.wide(call[1], 1 << 24).size();
			bytes = count ? call.read(call[1], count * 2) : std::vector<uint8_t>{};
		}
		make_bstr(call, bytes.data(), bytes.size());
		if (!call.result)
			return;
		call.store_pointer(call[0], call.result);
		if (old)
			context.allocator.release(old - 4);
		call.ret(1);
	};
	registry.add("SysReAllocString", 2, [reallocate](ApiCall &call) { reallocate(call, false); });
	registry.add("SysReAllocStringLen", 3, [reallocate](ApiCall &call) { reallocate(call, true); });

	// Random bytes come from the host: RtlGenRandom (exported from advapi32 as SystemFunction036) and BCryptGenRandom.
	const auto random_bytes = [](ApiCall &call, uint64_t buffer, size_t length) {
		std::vector<uint8_t> bytes(length);
		for (size_t done = 0; done < length;) {
			const auto count = ::getrandom(bytes.data() + done, length - done, 0);
			if (count < 0 && errno != EINTR)
				return false;
			if (count > 0)
				done += size_t(count);
		}
		call.write(buffer, bytes);
		return true;
	};
	// CryptProtectMemory: the block is mixed in place with a keystream that only this process (flag 0) or every process of the user in
	// this prefix (flags 1 and 2) can make again. The length must be a whole number of 16-byte blocks.
	const auto protect_memory = [](ApiCall &call) {
		const uint64_t length = call.u32(1);
		const uint32_t scope = call.u32(2);
		if (!call[0] || length % 16 || scope > 2) {
			call.fail(87, 0);
			return;
		}
		static const std::vector<uint8_t> process_key = [] {
			std::vector<uint8_t> key(32);
			for (size_t done = 0; done < key.size();) {
				const auto count = ::getrandom(key.data() + done, key.size() - done, 0);
				if (count > 0)
					done += size_t(count);
			}
			return key;
		}();
		const std::string shared_seed = "raven-protect-memory:" + prefix_tag() + ":" + std::to_string(::getuid());
		auto bytes = call.read(call[0], length);
		for (uint64_t block = 0; block * 32 < length; ++block) {
			auto digest = make_digest(0x800c);
			if (scope == 0)
				digest->update(process_key);
			else
				digest->update(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(shared_seed.data()), shared_seed.size()));
			digest->update(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(&block), sizeof(block)));
			const auto stream = digest->finish();
			for (size_t at = 0; at < 32 && block * 32 + at < length; ++at)
				bytes[block * 32 + at] ^= stream[at];
		}
		call.write(call[0], bytes);
		call.ret(1);
	};
	registry.add("CryptProtectMemory", 3, protect_memory);
	registry.add("CryptUnprotectMemory", 3, protect_memory);
	registry.add("SystemFunction036", 2, [random_bytes](ApiCall &call) {
		if (!call[0]) {
			call.fail(87);
			return;
		}
		call.ret(random_bytes(call, call[0], call.u32(1)));
	});
	// The generator that Chromium asks for by name: it fills the buffer and always reports success.
	registry.add("ProcessPrng", 2, [random_bytes](ApiCall &call) {
		if (call[0] && call[1])
			call.ret(random_bytes(call, call[0], size_t(call[1])));
		else
			call.ret(1);
	});
	// Windows Runtime strings: a string handle points at a header (flags, length, two unused words, pointer to the characters), the layout
	// programs built with C++/WinRT write themselves for references. A copy owns its characters in the same block; a reference points at
	// the caller's.
	const auto make_hstring = [&context](ApiCall &call, uint64_t source, uint32_t length, uint64_t out) -> uint32_t {
		if (!out)
			return 0x80070057U;
		if (!length) {
			call.store_pointer(out, 0);
			return 0;
		}
		const size_t width = call.pointer_width(), head = 16 + width;
		const auto block = context.allocator.allocate(head + (size_t(length) + 1) * 2, true);
		if (!block)
			return 0x8007000eU;
		std::vector<uint8_t> image(head + (size_t(length) + 1) * 2);
		const uint64_t characters = block + head;
		std::memcpy(image.data() + 4, &length, 4);
		std::memcpy(image.data() + 16, &characters, width);
		const auto text = call.read(source, size_t(length) * 2);
		std::memcpy(image.data() + head, text.data(), text.size());
		call.write(block, image);
		call.store_pointer(out, block);
		return 0;
	};
	registry.add("WindowsCreateString", 3, [make_hstring](ApiCall &call) { call.ret(make_hstring(call, call[0], call.u32(1), call[2])); });
	registry.add("WindowsDuplicateString", 2, [make_hstring](ApiCall &call) {
		if (!call[0]) {
			if (call[1])
				call.store_pointer(call[1], 0);
			call.ret(0);
			return;
		}
		const size_t width = call.pointer_width();
		call.ret(make_hstring(call, call.load_pointer(call[0] + 16), call.load<uint32_t>(call[0] + 4), call[1]));
		(void)width;
	});
	registry.add("WindowsCreateStringReference", 4, [](ApiCall &call) {
		if (!call[2] || !call[3]) {
			call.ret(0x80070057U);
			return;
		}
		if (!call.u32(1)) {
			call.store_pointer(call[3], 0);
			call.ret(0);
			return;
		}
		const size_t width = call.pointer_width();
		std::vector<uint8_t> header(16 + width);
		header[0] = 1;
		const uint32_t length = call.u32(1);
		std::memcpy(header.data() + 4, &length, 4);
		const uint64_t source = call[0];
		std::memcpy(header.data() + 16, &source, width);
		call.write(call[2], header);
		call.store_pointer(call[3], call[2]);
		call.ret(0);
	});
	registry.add("WindowsDeleteString", 1, [&context](ApiCall &call) {
		if (call[0] && !(call.load<uint32_t>(call[0]) & 1))
			context.allocator.release(call[0]);
		call.ret(0);
	});
	registry.add("WindowsGetStringRawBuffer", 2, [](ApiCall &call) {
		if (!call[0]) {
			if (call[1])
				call.store<uint32_t>(call[1], 0);
			static const uint16_t empty = 0;
			(void)empty;
			call.ret(0);
			return;
		}
		if (call[1])
			call.store<uint32_t>(call[1], call.load<uint32_t>(call[0] + 4));
		call.ret(call.load_pointer(call[0] + 16));
	});
	registry.add("WindowsGetStringLen", 1, [](ApiCall &call) { call.ret(call[0] ? call.load<uint32_t>(call[0] + 4) : 0); });
	registry.add("WindowsIsStringEmpty", 1, [](ApiCall &call) { call.ret(!call[0] || !call.load<uint32_t>(call[0] + 4)); });
	// Two strings in code unit order, a null string being the empty one.
	registry.add("WindowsCompareStringOrdinal", 3, [](ApiCall &call) {
		const auto text = [&](uint64_t handle) {
			if (!handle)
				return std::u16string();
			const uint32_t length = call.load<uint32_t>(handle + 4);
			const auto bytes = call.read(call.load_pointer(handle + 16), size_t(length) * 2);
			std::u16string value(length, u'\0');
			std::memcpy(value.data(), bytes.data(), bytes.size());
			return value;
		};
		if (!call[2]) {
			call.ret(0x80070057U);
			return;
		}
		const int order = text(call[0]).compare(text(call[1]));
		call.store<int32_t>(call[2], order < 0 ? -1 : order > 0 ? 1 : 0);
		call.ret(0);
	});
	// Without Windows Runtime metadata, a language exception has nowhere to go, and type names and namespaces resolve to nothing.
	registry.add("RoOriginateLanguageException", 3, [](ApiCall &call) { call.ret(0); });
	registry.add("RoParseTypeName", 3, [](ApiCall &call) { call.ret(0x80004001U); });
	registry.add("RoResolveNamespace", 7, [](ApiCall &call) { call.ret(0x80004001U); });
	registry.add("RoGetParameterizedTypeInstanceIID", 5, [](ApiCall &call) { call.ret(0x80004001U); });
	// No HLSL compiler stands behind d3dcompiler_47 yet: a compile is refused with no code and no message blob, so a program falls back to
	// what it does without a GPU path instead of running an empty shader.
	registry.add("D3DCompile", 11, [](ApiCall &call) {
		if (call[9])
			call.store_pointer(call[9], 0);
		if (call[10])
			call.store_pointer(call[10], 0);
		call.ret(0x80004001U);
	});
	// No Windows Runtime class exists here: initialization joins the thread to a COM apartment like CoInitializeEx (RO_INIT_MULTITHREADED
	// is 1), and every activation reports that the class is not registered.
	registry.add("RoInitialize", 1, [initialize](ApiCall &call) { initialize(call, call.u32(0) == 1, true); });
	registry.add("RoUninitialize", 0, uninitialize);
	registry.add("RoGetActivationFactory", 3, [](ApiCall &call) {
		// The traced failure says which class was asked for.
		static const bool traced = std::getenv("RAVEN_TRACE_FAIL") != nullptr;
		if (traced && call[0]) {
			std::string name;
			try {
				const auto length = std::min<uint32_t>(call.load<uint32_t>(call[0] + 4), 200);
				const auto text = call.read(call.load_pointer(call[0] + 16), length * 2);
				for (size_t i = 0; i < length; ++i) name.push_back(text[i * 2 + 1] ? '?' : char(text[i * 2]));
			} catch (const ApiFault &) {}
			std::fprintf(stderr, "raven: no Windows Runtime class %s\n", name.c_str());
		}
		if (call[2])
			call.store_pointer(call[2], 0);
		call.ret(0x80040154U);
	});
	registry.add("RoActivateInstance", 2, [](ApiCall &call) {
		if (call[1])
			call.store_pointer(call[1], 0);
		call.ret(0x80040154U);
	});
	// Event tracing: there is no session to receive events, so registrations succeed and events go nowhere.
	registry.add("RegisterTraceGuidsW", 8, [](ApiCall &call) {
		if (call[7])
			call.store_pointer(call[7], 1);
		call.ret(0);
	});
	registry.add("UnregisterTraceGuids", 1, [](ApiCall &call) { call.ret(0); });
	// No session listens to any provider.
	registry.add("EventProviderEnabled", 3, [](ApiCall &call) { call.ret(0); });
	// Each thread carries an activity ID, all zeros until set; a created one is a fresh random GUID.
	auto activities = std::make_shared<std::map<uint64_t, std::array<uint8_t, 16>>>();
	registry.add("EventActivityIdControl", 2, [activities, &context](ApiCall &call) {
		const auto code = call.u32(0);
		if (code < 1 || code > 5 || !call[1]) {
			call.ret(87);
			return;
		}
		auto &current = (*activities)[context.environment.teb()];
		std::array<uint8_t, 16> given{}, fresh{};
		if (code == 2 || code == 4) {
			const auto bytes = call.read(call[1], 16);
			std::copy(bytes.begin(), bytes.end(), given.begin());
		}
		if (code == 3 || code == 5) {
			if (::getrandom(fresh.data(), fresh.size(), 0) != ssize_t(fresh.size())) {
				call.ret(31);
				return;
			}
			fresh[6] = uint8_t((fresh[6] & 0x0f) | 0x40);
			fresh[8] = uint8_t((fresh[8] & 0x3f) | 0x80);
		}
		const auto previous = current;
		if (code == 2 || code == 4) current = given;
		if (code == 5) current = fresh;
		if (code == 1 || code == 4 || code == 5) call.write(call[1], std::vector<uint8_t>(previous.begin(), previous.end()));
		if (code == 3) call.write(call[1], std::vector<uint8_t>(fresh.begin(), fresh.end()));
		call.ret(0);
	});
	registry.add("EventRegister", 4, [](ApiCall &call) {
		if (call[3])
			call.store<uint64_t>(call[3], 1);
		call.ret(0);
	});
	registry.add("EventUnregister", 1, [](ApiCall &call) { call.ret(0); });
	registry.add("EventSetInformation", 4, [](ApiCall &call) { call.ret(0); });
	registry.add("EventWriteTransfer", 6, [](ApiCall &call) { call.ret(0); });
	registry.add("EventEnabled", 2, [](ApiCall &call) { call.ret(0); });
	registry.add("TraceEvent", 2, [](ApiCall &call) { call.ret(0); });
	// No tracing sessions are created here. Controllers receive a system status, never a successful stop of a nonexistent session.
	const auto trace_control = [](ApiCall &call, bool wide, bool control) {
		try {
			if (!call[2] || (!call[0] && !call[1]) || (control && call.u32(3) > 5)) {
				call.ret(87);
				return;
			}
			if (call.load<uint32_t>(call[2]) < 48) {
				call.ret(24);
				return;
			}
			if (!call[1] || (wide ? call.wide(call[1], 1025).empty() : call.narrow(call[1], 1025).empty())) {
				call.ret(87);
				return;
			}
			call.ret(4201);
		} catch (const ApiFault &) {
			call.ret(998);
		}
	};
	registry.add("StopTraceA", 3, [trace_control](ApiCall &call) { trace_control(call, false, false); });
	registry.add("StopTraceW", 3, [trace_control](ApiCall &call) { trace_control(call, true, false); });
	registry.add("ControlTraceA", 4, [trace_control](ApiCall &call) { trace_control(call, false, true); });
	registry.add("ControlTraceW", 4, [trace_control](ApiCall &call) { trace_control(call, true, true); });
	const auto start_trace = [](ApiCall &call, bool wide) {
		try {
			if (!call[0] || !call[1] || !call[2] || (wide ? call.wide(call[1], 1025).empty() : call.narrow(call[1], 1025).empty()))
				call.ret(87);
			else if (call.load<uint32_t>(call[2]) < 48)
				call.ret(24);
			else
				call.ret(50);
		} catch (const ApiFault &) {
			call.ret(998);
		}
	};
	registry.add("StartTraceA", 3, [start_trace](ApiCall &call) { start_trace(call, false); });
	registry.add("StartTraceW", 3, [start_trace](ApiCall &call) { start_trace(call, true); });
	// Proxy discovery: no proxy is configured, so sessions open and every lookup answers "connect directly".
	// The event log: a source opens, and what a program reports there goes nowhere, as no one reads it; RAVEN_TRACE_DEBUG prints it, which
	// is often the reason a program gave up.
	struct EventSource : KernelObject {};
	for (const bool wide : {true, false}) {
		registry.add(wide ? "RegisterEventSourceW" : "RegisterEventSourceA", 2, [&context](ApiCall &call) {
			const auto made = context.objects.add_object(std::make_shared<EventSource>());
			if (made.status) {
				call.fail(8, 0);
				return;
			}
			call.ret(made.value);
		});
		registry.add(wide ? "ReportEventW" : "ReportEventA", 9, [&context, wide](ApiCall &call) {
			if (!context.objects.object_as<EventSource>(call[0])) {
				call.fail(6);
				return;
			}
			static const bool shown = std::getenv("RAVEN_TRACE_DEBUG") != nullptr;
			if (shown)
				for (uint32_t index = 0; index < (call.u32(5) & 0xffff) && call[7]; ++index)
					if (const uint64_t text = call.load_pointer(call[7] + index * call.pointer_width()))
						std::fprintf(stderr, "event: %s\n", wide ? to_utf8(call.wide(text, size_t(1) << 20)).c_str() : call.narrow(text, size_t(1) << 20).c_str());
			call.ret(1);
		});
	}
	registry.add("DeregisterEventSource", 1, [&context](ApiCall &call) {
		if (!context.objects.object_as<EventSource>(call[0])) {
			call.fail(6);
			return;
		}
		context.objects.close(call[0]);
		call.ret(1);
	});
	// No error object is set for the thread.
	registry.add("GetErrorInfo", 2, [](ApiCall &call) {
		if (call[1])
			call.store_pointer(call[1], 0);
		call.ret(1);
	});
	registry.add("SetErrorInfo", 2, [](ApiCall &call) { call.ret(0); });
	// One NUMA node holds every processor.
	registry.add("GetNumaHighestNodeNumber", 1, [](ApiCall &call) {
		if (call[0])
			call.store<uint32_t>(call[0], 0);
		call.ret(1);
	});
	// Private namespaces: a boundary describes who shares one, and the objects a program names inside it behave as named objects do.
	struct PrivateNamespace : KernelObject {};
	registry.add("CreateBoundaryDescriptorW", 2, [&context](ApiCall &call) { call.ret(context.allocator.allocate(64, true)); });
	registry.add("AddSIDToBoundaryDescriptor", 2, [](ApiCall &call) { call.ret(1); });
	registry.add("AddIntegrityLabelToBoundaryDescriptor", 2, [](ApiCall &call) { call.ret(1); });
	registry.add("DeleteBoundaryDescriptor", 1, [&context](ApiCall &call) {
		if (call[0])
			context.allocator.release(call[0]);
	});
	for (const auto *name : {"CreatePrivateNamespaceW", "OpenPrivateNamespaceW"})
		registry.add(name, std::string_view(name).starts_with("Create") ? 3 : 2, [&context](ApiCall &call) {
			const auto made = context.objects.add_object(std::make_shared<PrivateNamespace>());
			if (made.status) {
				call.fail(8, 0);
				return;
			}
			call.ret(made.value);
		});
	registry.add("ClosePrivateNamespace", 2, [&context](ApiCall &call) {
		if (!context.objects.object_as<PrivateNamespace>(call[0])) {
			call.fail(6);
			return;
		}
		context.objects.close(call[0]);
		call.ret(1);
	});
#ifndef RAVEN_OPENSSL
	// Without the HTTP client, sessions open and close and nothing more.
	struct WinHttpSession : KernelObject {};
	registry.add("WinHttpOpen", 5, [&context](ApiCall &call) {
		const auto made = context.objects.add_object(std::make_shared<WinHttpSession>());
		if (made.status) {
			call.fail(8, 0);
			return;
		}
		call.ret(made.value);
	});
	registry.add("WinHttpCloseHandle", 1, [&context](ApiCall &call) {
		if (!context.objects.object_as<WinHttpSession>(call[0])) {
			call.fail(6);
			return;
		}
		context.objects.close(call[0]);
		call.ret(1);
	});
	registry.add("WinHttpSetTimeouts", 5, [](ApiCall &call) { call.ret(1); });
#endif
	registry.add("WinHttpGetIEProxyConfigForCurrentUser", 1, [](ApiCall &call) {
		if (!call[0]) {
			call.fail(87);
			return;
		}
		// WINHTTP_CURRENT_USER_IE_PROXY_CONFIG: auto-detect flag and three string pointers, all empty.
		call.write(call[0], std::vector<uint8_t>(call.pointer_width() == 4 ? 16 : 32));
		call.ret(1);
	});
	registry.add("WinHttpDetectAutoProxyConfigUrl", 2, [](ApiCall &call) { call.fail(12180); });
	registry.add("WinHttpGetProxyForUrl", 4, [](ApiCall &call) {
		if (!call[3]) {
			call.fail(87);
			return;
		}
		// WINHTTP_PROXY_INFO: the access type (1 is no proxy) and two empty string pointers.
		std::vector<uint8_t> info(call.pointer_width() == 4 ? 12 : 24);
		info[0] = 1;
		call.write(call[3], info);
		call.ret(1);
	});
	// Display scaling: contexts are the small negative values of the Windows API; there is one display scale, so the setting is only remembered.
	auto dpi_context = std::make_shared<int64_t>(-1);
	registry.add("IsValidDpiAwarenessContext", 1, [](ApiCall &call) {
		const auto value = int64_t(call[0]);
		call.ret(value <= -1 && value >= -5);
	});
	registry.add("SetThreadDpiAwarenessContext", 1, [dpi_context](ApiCall &call) {
		const auto value = call.pointer_width() == 4 ? int64_t(int32_t(call.u32(0))) : int64_t(call[0]);
		if (value > -1 || value < -5) {
			call.ret(0);
			return;
		}
		const auto previous = *dpi_context;
		*dpi_context = value;
		call.ret(uint64_t(previous));
	});
	registry.add("GetThreadDpiAwarenessContext", 0, [dpi_context](ApiCall &call) { call.ret(uint64_t(*dpi_context)); });
	// Windows of another scale are never hosted, so the hosting behavior is only remembered: 0 is DPI_HOSTING_BEHAVIOR_DEFAULT.
	auto hosting = std::make_shared<int32_t>(0);
	registry.add("SetThreadDpiHostingBehavior", 1, [hosting](ApiCall &call) {
		const auto value = int32_t(call.u32(0));
		if (value < 0 || value > 1) {
			call.ret(uint64_t(-1));
			return;
		}
		call.ret(uint64_t(int64_t(std::exchange(*hosting, value))));
	});
	registry.add("GetThreadDpiHostingBehavior", 0, [hosting](ApiCall &call) { call.ret(uint64_t(int64_t(*hosting))); });
	// advapi32 security helpers that need no token or policy database.
	registry.add("AllocateAndInitializeSid", 11, [&context](ApiCall &call) {
		const uint8_t count = uint8_t(call[1]);
		if (count < 1 || count > 8) {
			call.fail(87);
			return;
		}
		std::vector<uint8_t> sid(8 + count * 4);
		sid[0] = 1;
		sid[1] = count;
		const auto authority = call.read(call[0], 6);
		std::memcpy(sid.data() + 2, authority.data(), 6);
		for (uint8_t index = 0; index < count; ++index) {
			const uint32_t part = call.u32(2 + index);
			std::memcpy(sid.data() + 8 + index * 4, &part, 4);
		}
		const auto block = context.allocator.allocate(sid.size(), false);
		if (!block) {
			call.fail(8);
			return;
		}
		call.write(block, sid);
		call.store_pointer(call[10], block);
		call.ret(1);
	});
	registry.add("FreeSid", 1, [&context](ApiCall &call) {
		if (call[0]) context.allocator.release(call[0]);
		call.ret(0);
	});
	registry.add("CheckTokenMembership", 3, [](ApiCall &call) {
		// The groups held are Users (S-1-5-32-545) and Everyone (S-1-1-0); Administrators (S-1-5-32-544) counts only in an elevated
		// process, since without elevation it is held for denying access alone.
		const auto count = call.read(call[1] + 1, 1)[0];
		const auto authority = call.read(call[1] + 2, 6)[5];
		const uint32_t first = count ? call.load<uint32_t>(call[1] + 8) : 0, second = count > 1 ? call.load<uint32_t>(call[1] + 12) : 0;
		const bool users = authority == 5 && count == 2 && first == 32 && second == 545;
		const bool administrators = authority == 5 && count == 2 && first == 32 && second == 544 && process_elevated();
		const bool everyone = authority == 1 && count == 1 && first == 0;
		call.store<uint32_t>(call[2], users || everyone || administrators);
		call.ret(1);
	});
	registry.add("IsTextUnicode", 3, [](ApiCall &call) {
		const size_t size = call.u32(1);
		const auto bytes = call.read(call[0], size);
		uint32_t tests = call[2] ? call.load<uint32_t>(call[2]) : 0xffffffffU;
		uint32_t found = 0;
		const bool even = size % 2 == 0;
		if (even && size >= 2 && (tests & 0x20) && bytes[0] == 0xff && bytes[1] == 0xfe) found |= 0x20;
		if (even && size >= 2 && (tests & 0x40) && bytes[0] == 0xfe && bytes[1] == 0xff) found |= 0x40;
		// Plain text with zero bytes in every second position is UTF-16 in this host's byte order.
		if (even && size >= 4 && (tests & 0x8000)) {
			size_t zeros = 0;
			for (size_t at = 1; at < size; at += 2)
				if (bytes[at] == 0 && bytes[at - 1] != 0)
					++zeros;
			if (zeros * 3 > size / 2 * 2)
				found |= 0x8000;
		}
		if (call[2]) call.store<uint32_t>(call[2], found);
		call.ret(found != 0);
	});

	// CryptoAPI hashing over the digests Raven implements.
	auto crypt = std::make_shared<CryptState>();
	registry.add("CryptAcquireContextW", 5, [crypt](ApiCall &call) {
		const auto handle = crypt->next++;
		crypt->providers[handle] = true;
		call.store_pointer(call[0], handle);
		call.ret(1);
	});
	// CryptGenRandom(provider, length, buffer): random bytes from the host's generator.
	registry.add("CryptGenRandom", 3, [crypt](ApiCall &call) {
		const uint32_t length = call.u32(1);
		if (!crypt->providers.contains(call[0]) || (length && !call[2]) || length > (64u << 20)) {
			call.fail(87);
			return;
		}
		std::vector<uint8_t> bytes(length);
		for (size_t done = 0; done < bytes.size();) {
			const auto got = ::getrandom(bytes.data() + done, bytes.size() - done, 0);
			if (got <= 0) {
				call.fail(0x80090020U);
				return;
			}
			done += size_t(got);
		}
		call.write(call[2], bytes);
		call.ret(1);
	});
	registry.add("CryptReleaseContext", 2, [crypt](ApiCall &call) {
		crypt->providers.erase(call[0]);
		call.ret(1);
	});
	// advapi32's MD5 keeps its state in the caller's MD5_CTX: the bit count (two words), the four state words, the pending bytes and
	// the digest, at offsets 0, 8, 24 and 88.
	registry.add("MD5Init", 1, [](ApiCall &call) {
		std::vector<uint8_t> context(104);
		const uint32_t initial[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
		std::memcpy(context.data() + 8, initial, 16);
		call.write(call[0], context);
	});
	const auto md5_feed = [](std::vector<uint8_t> &context, std::span<const uint8_t> data) {
		uint64_t bits;
		std::memcpy(&bits, context.data(), 8);
		size_t filled = size_t(bits >> 3) & 63;
		bits += uint64_t(data.size()) * 8;
		std::memcpy(context.data(), &bits, 8);
		std::array<uint32_t, 4> state;
		std::memcpy(state.data(), context.data() + 8, 16);
		for (const auto byte : data) {
			context[24 + filled++] = byte;
			if (filled == 64) {
				md5_block(state, context.data() + 24);
				filled = 0;
			}
		}
		std::memcpy(context.data() + 8, state.data(), 16);
	};
	registry.add("MD5Update", 3, [md5_feed](ApiCall &call) {
		auto context = call.read(call[0], 104);
		md5_feed(context, call.read(call[1], call.u32(2)));
		call.write(call[0], context);
	});
	registry.add("MD5Final", 1, [md5_feed](ApiCall &call) {
		auto context = call.read(call[0], 104);
		uint64_t bits;
		std::memcpy(&bits, context.data(), 8);
		const size_t filled = size_t(bits >> 3) & 63;
		// 0x80, zeros up to 56 bytes into a block, then the bit count before the padding.
		std::vector<uint8_t> padding(filled < 56 ? 56 - filled : 120 - filled);
		padding[0] = 0x80;
		md5_feed(context, padding);
		std::vector<uint8_t> length(8);
		std::memcpy(length.data(), &bits, 8);
		md5_feed(context, length);
		std::memcpy(context.data() + 88, context.data() + 8, 16);
		call.write(call[0], context);
	});
	registry.add("CryptCreateHash", 5, [crypt](ApiCall &call) {
		auto digest = make_digest(call.u32(1));
		if (!crypt->providers.contains(call[0]) || !digest) {
			call.fail(digest ? 0x80090016U : 0x80090008U);
			return;
		}
		const auto handle = crypt->next++;
		crypt->hashes[handle] = std::move(digest);
		call.store_pointer(call[4], handle);
		call.ret(1);
	});
	registry.add("CryptHashData", 4, [crypt](ApiCall &call) {
		const auto found = crypt->hashes.find(call[0]);
		if (found == crypt->hashes.end()) {
			call.fail(6);
			return;
		}
		found->second->update(call.read(call[1], call.u32(2)));
		call.ret(1);
	});
	registry.add("CryptGetHashParam", 5, [crypt](ApiCall &call) {
		const auto found = crypt->hashes.find(call[0]);
		if (found == crypt->hashes.end()) {
			call.fail(6);
			return;
		}
		const uint32_t what = call.u32(1);
		if (what == 4) {
			call.store<uint32_t>(call[2], uint32_t(found->second->size()));
			call.store<uint32_t>(call[3], 4);
			call.ret(1);
			return;
		}
		if (what != 2) {
			call.fail(0x80090008U);
			return;
		}
		auto value = found->second->clone()->finish();
		const uint32_t capacity = call.load<uint32_t>(call[3]);
		call.store<uint32_t>(call[3], uint32_t(value.size()));
		if (!call[2]) {
			call.ret(1);
			return;
		}
		if (capacity < value.size()) {
			call.fail(234);
			return;
		}
		call.write(call[2], value);
		call.ret(1);
	});
	// PUBLICKEYBLOB of an RSA key: BLOBHEADER (type 6, version, algorithm), RSAPUBKEY ("RSA1", bit length, exponent), then the modulus,
	// least significant byte first. Private and session keys are not imported.
	registry.add("CryptImportKey", 6, [crypt](ApiCall &call) {
		if (!crypt->providers.contains(call[0])) {
			call.fail(6);
			return;
		}
		const auto blob = call.read(call[1], call.u32(2));
		uint32_t magic = 0, bits = 0, exponent = 0;
		if (blob.size() >= 20) {
			std::memcpy(&magic, blob.data() + 8, 4);
			std::memcpy(&bits, blob.data() + 12, 4);
			std::memcpy(&exponent, blob.data() + 16, 4);
		}
		if (blob.size() < 20 || blob[0] != 6 || magic != 0x31415352U || !bits || bits % 8 || blob.size() < 20 + bits / 8 || !exponent) {
			call.fail(0x8009000aU);
			return;
		}
		const auto handle = crypt->next++;
		crypt->keys[handle] = {words_from_bytes(std::span(blob).subspan(20, bits / 8)), exponent, bits / 8,
			std::vector<uint8_t>(blob.begin(), blob.begin() + 20 + bits / 8)};
		call.store_pointer(call[5], handle);
		call.ret(1);
	});
	// A public key goes out as the PUBLICKEYBLOB it came in as; a null buffer asks for the length.
	registry.add("CryptExportKey", 6, [crypt](ApiCall &call) {
		const auto key = crypt->keys.find(call[0]);
		if (key == crypt->keys.end()) {
			call.fail(6);
			return;
		}
		if (call.u32(2) != 6 || call[1]) {
			call.fail(0x8009000aU);
			return;
		}
		const auto &blob = key->second.blob;
		const uint32_t capacity = call.load<uint32_t>(call[5]);
		call.store<uint32_t>(call[5], uint32_t(blob.size()));
		if (!call[4]) {
			call.ret(1);
			return;
		}
		if (capacity < blob.size()) {
			call.fail(234);
			return;
		}
		call.write(call[4], blob);
		call.ret(1);
	});
	registry.add("CryptDestroyKey", 1, [crypt](ApiCall &call) {
		if (!crypt->keys.erase(call[0]))
			call.fail(6);
		else
			call.ret(1);
	});
	// A PKCS #1 v1.5 signature of the hash, least significant byte first as CryptoAPI writes it; CRYPT_NOHASHOID signs the bare digest.
	const auto verify_signature = [crypt](ApiCall &call) {
		const auto hash = crypt->hashes.find(call[0]);
		const auto key = crypt->keys.find(call[3]);
		if (hash == crypt->hashes.end() || key == crypt->keys.end()) {
			call.fail(6);
			return;
		}
		const auto signature = call.read(call[1], call.u32(2));
		if (signature.size() != key->second.length) {
			call.fail(0x80090006U);
			return;
		}
		const auto message = power_mod(words_from_bytes(signature), key->second.exponent, key->second.modulus);
		std::vector<uint8_t> encoded(key->second.length);
		for (size_t at = 0; at < encoded.size(); ++at)
			encoded[encoded.size() - 1 - at] = uint8_t(message[at / 4] >> (at % 4 * 8));
		const auto digest = hash->second->clone()->finish();
		auto expected = (call.u32(5) & 1) ? std::vector<uint8_t>() : digest_info(digest.size());
		expected.insert(expected.end(), digest.begin(), digest.end());
		bool valid = encoded.size() >= expected.size() + 11 && encoded[0] == 0 && encoded[1] == 1;
		const size_t separator = encoded.size() - expected.size() - 1;
		for (size_t at = 2; valid && at < separator; ++at)
			valid = encoded[at] == 0xff;
		valid = valid && encoded[separator] == 0 && std::equal(expected.begin(), expected.end(), encoded.begin() + separator + 1);
		if (!valid) {
			call.fail(0x80090006U);
			return;
		}
		call.ret(1);
	};
	registry.add("CryptVerifySignatureW", 6, verify_signature);
	registry.add("CryptVerifySignatureA", 6, verify_signature);
	// The providers CryptAcquireContext opens for each type when it is given no name: the base RSA provider and the AES one.
	registry.add("CryptGetDefaultProviderW", 5, [](ApiCall &call) {
		const uint32_t type = call.u32(0);
		const std::u16string name = type == 1 ? u"Microsoft Base Cryptographic Provider v1.0" : type == 24 ?
			u"Microsoft Enhanced RSA and AES Cryptographic Provider" : u"";
		if (name.empty() || call[1] || !call[4]) {
			call.fail(name.empty() ? 0x80090017U : 87);
			return;
		}
		const uint32_t needed = uint32_t(name.size() + 1) * 2, capacity = call.load<uint32_t>(call[4]);
		call.store<uint32_t>(call[4], needed);
		if (!call[3]) {
			call.ret(1);
			return;
		}
		if (capacity < needed) {
			call.fail(234);
			return;
		}
		call.put_wide(call[3], name, name.size() + 1);
		call.ret(1);
	});
	// FIPS mode is off, as on a Windows that has not been told to enforce it.
	registry.add("BCryptGetFipsAlgorithmMode", 1, [](ApiCall &call) {
		if (!call[0]) {
			call.ret(0xc000000dU);
			return;
		}
		call.store<uint8_t>(call[0], 0);
		call.ret(0);
	});
	// CryptGetProvParam(provider, parameter, data, length, flags): the algorithms the provider offers, one per call from CRYPT_FIRST on
	// (PP_ENUMALGS as PROV_ENUMALGS, PP_ENUMALGS_EX as PROV_ENUMALGS_EX) until ERROR_NO_MORE_ITEMS, and its name, version, kind and key
	// pairs. Without a buffer the length needed.
	registry.add("CryptGetProvParam", 5, [crypt](ApiCall &call) {
		if (!crypt->providers.contains(call[0]) || !call[3]) {
			call.fail(call[3] ? 6 : 87);
			return;
		}
		struct Algorithm {
			uint32_t id, length, minimum, maximum, protocols;
			const char *name, *long_name;
		};
		static constexpr Algorithm algorithms[] = {{0x6602, 128, 40, 128, 0, "RC2", "RSA Data Security's RC2"},
			{0x6801, 128, 40, 128, 0, "RC4", "RSA Data Security's RC4"}, {0x6601, 56, 56, 56, 1, "DES", "Data Encryption Standard (DES)"},
			{0x6609, 112, 112, 112, 0, "3DES TWO KEY", "Two Key Triple DES"}, {0x6603, 168, 168, 168, 0, "3DES", "Three Key Triple DES"},
			{0x8004, 160, 160, 160, 0x30, "SHA-1", "Secure Hash Algorithm (SHA-1)"}, {0x8003, 128, 128, 128, 0x30, "MD5", "Message Digest 5 (MD5)"},
			{0x800c, 256, 256, 256, 0, "SHA-256", "Secure Hash Algorithm (SHA-256)"}, {0x800d, 384, 384, 384, 0, "SHA-384", "Secure Hash Algorithm (SHA-384)"},
			{0x800e, 512, 512, 512, 0, "SHA-512", "Secure Hash Algorithm (SHA-512)"}, {0x2400, 1024, 384, 16384, 0x30, "RSA_SIGN", "RSA Signature"},
			{0xa400, 1024, 384, 16384, 0x30, "RSA_KEYX", "RSA Key Exchange"}, {0x660e, 128, 128, 128, 0, "AES 128", "Advanced Encryption Standard (AES) 128 bit"},
			{0x660f, 192, 192, 192, 0, "AES 192", "Advanced Encryption Standard (AES) 192 bit"},
			{0x6610, 256, 256, 256, 0, "AES 256", "Advanced Encryption Standard (AES) 256 bit"}};
		std::vector<uint8_t> value;
		const auto put = [&](size_t at, uint32_t word) { std::memcpy(value.data() + at, &word, 4); };
		const auto text = [&](size_t at, const char *name, size_t capacity) {
			std::memcpy(value.data() + at, name, std::min(std::strlen(name) + 1, capacity));
		};
		const uint32_t parameter = call.u32(1);
		if (parameter == 1 || parameter == 22) {
			auto &position = crypt->enumeration[call[0]];
			if (call.u32(4) & 1)
				position = 0;
			if (position >= std::size(algorithms)) {
				call.fail(259);
				return;
			}
			const auto &algorithm = algorithms[position];
			if (parameter == 1) {
				value.assign(32, 0);
				put(0, algorithm.id); put(4, algorithm.length); put(8, uint32_t(std::strlen(algorithm.name) + 1));
				text(12, algorithm.name, 20);
			} else {
				value.assign(88, 0);
				put(0, algorithm.id); put(4, algorithm.length); put(8, algorithm.minimum); put(12, algorithm.maximum);
				put(16, algorithm.protocols); put(20, uint32_t(std::strlen(algorithm.name) + 1)); text(24, algorithm.name, 20);
				put(44, uint32_t(std::strlen(algorithm.long_name) + 1)); text(48, algorithm.long_name, 40);
			}
			if (call[2])
				++position;
		} else if (parameter == 4) {
			const std::string name = "Microsoft Enhanced RSA and AES Cryptographic Provider";
			value.assign(name.begin(), name.end());
			value.push_back(0);
		} else if (parameter == 6 || parameter == 36) {
			value.assign(1, 0);
		} else if (parameter == 3 || parameter == 5 || parameter == 16 || parameter == 39) {
			value.assign(4, 0);
			put(0, parameter == 3 ? 2 : parameter == 5 ? 0x200 : parameter == 16 ? 24 : 3);
		} else {
			call.fail(0x80090009U);
			return;
		}
		const uint32_t capacity = call.load<uint32_t>(call[3]);
		call.store<uint32_t>(call[3], uint32_t(value.size()));
		if (!call[2]) {
			call.ret(1);
			return;
		}
		if (capacity < value.size()) {
			call.fail(234);
			return;
		}
		call.write(call[2], value);
		call.ret(1);
	});
	registry.add("CryptDestroyHash", 1, [crypt](ApiCall &call) {
		call.ret(crypt->hashes.erase(call[0]) ? 1 : 0);
	});
}

}
