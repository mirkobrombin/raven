#include "kernel_context.h"
#include "text.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <dlfcn.h>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace raven {

namespace {

constexpr uint32_t mmsyserr_error = 1, mmsyserr_baddeviceid = 2, mmsyserr_invalhandle = 5, mmsyserr_nomem = 7, mmsyserr_notsupported = 8,
	mmsyserr_invalflag = 10, mmsyserr_invalparam = 11, waverr_badformat = 32, waverr_stillplaying = 33, waverr_unprepared = 34;
constexpr uint32_t whdr_done = 0x1, whdr_prepared = 0x2, whdr_beginloop = 0x4, whdr_endloop = 0x8, whdr_inqueue = 0x10;
constexpr uint32_t callback_kind = 0x70000, callback_null = 0, callback_event = 0x50000, wave_format_query = 0x1;
constexpr uint64_t wave_mapper = 0xffffffffU;

// The PulseAudio simple client, which PipeWire also serves, loaded when a device first opens. Without it the samples play to nowhere.
struct SampleSpec {
	int format;
	uint32_t rate;
	uint8_t channels;
};

struct BufferAttributes {
	uint32_t maxlength, tlength, prebuf, minreq, fragsize;
};

struct Pulse {
	void *(*open)(const char *, const char *, int, const char *, const char *, const SampleSpec *, const void *, const BufferAttributes *, int *);
	int (*write)(void *, const void *, size_t, int *);
	int (*flush)(void *, int *);
	uint64_t (*latency)(void *, int *);
	void (*close)(void *);
};

const Pulse *pulse()
{
	static const std::unique_ptr<Pulse> loaded = []() -> std::unique_ptr<Pulse> {
		void *library = ::dlopen("libpulse-simple.so.0", RTLD_NOW | RTLD_LOCAL);
		if (!library)
			return nullptr;
		auto api = std::make_unique<Pulse>();
		api->open = reinterpret_cast<decltype(api->open)>(::dlsym(library, "pa_simple_new"));
		api->write = reinterpret_cast<decltype(api->write)>(::dlsym(library, "pa_simple_write"));
		api->flush = reinterpret_cast<decltype(api->flush)>(::dlsym(library, "pa_simple_flush"));
		api->latency = reinterpret_cast<decltype(api->latency)>(::dlsym(library, "pa_simple_get_latency"));
		api->close = reinterpret_cast<decltype(api->close)>(::dlsym(library, "pa_simple_free"));
		if (!api->open || !api->write || !api->flush || !api->latency || !api->close)
			return nullptr;
		return api;
	}();
	return loaded.get();
}

struct Format {
	uint32_t rate = 0, channels = 0, bits = 0, block = 0;
	bool floating = false;
	uint32_t bytes_per_second() const { return rate * block; }
};

// WAVEFORMATEX, or WAVEFORMATEXTENSIBLE naming PCM or IEEE float in its subformat.
std::optional<Format> read_format(ApiCall &call, uint64_t address)
{
	const auto bytes = call.read(address, 16);
	uint16_t tag, channels, block, bits;
	uint32_t rate;
	std::memcpy(&tag, bytes.data(), 2);
	std::memcpy(&channels, bytes.data() + 2, 2);
	std::memcpy(&rate, bytes.data() + 4, 4);
	std::memcpy(&block, bytes.data() + 12, 2);
	std::memcpy(&bits, bytes.data() + 14, 2);
	if (tag == 0xfffe) {
		const auto extended = call.read(address + 16, 26);
		uint16_t extra;
		std::memcpy(&extra, extended.data(), 2);
		if (extra < 22)
			return std::nullopt;
		uint32_t subformat;
		std::memcpy(&subformat, extended.data() + 10, 4);
		tag = uint16_t(subformat);
	}
	Format format{rate, channels, bits, block, tag == 3};
	if ((tag != 1 && tag != 3) || !channels || channels > 8 || rate < 1000 || rate > 384000 || block != channels * (bits / 8) ||
		(tag == 1 && bits != 8 && bits != 16 && bits != 24 && bits != 32) || (tag == 3 && bits != 32))
		return std::nullopt;
	return format;
}

// One opened device: a host thread plays the buffers in order, and the headers it finished wait for the guest to be told.
class WaveOut : public KernelObject {
public:
	WaveOut(const Format &format, uint32_t callback, uint64_t target) : format(format), callback(callback), target(target)
	{
		if (const auto *api = pulse()) {
			const int sample = format.floating ? 5 : format.bits == 8 ? 0 : format.bits == 16 ? 3 : format.bits == 24 ? 9 : 7;
			const SampleSpec spec{sample, format.rate, uint8_t(format.channels)};
			// About a twentieth of a second ahead of the speaker, so a finished header means the sound is almost out.
			const uint32_t ahead = std::max<uint32_t>(format.bytes_per_second() / 20 / format.block * format.block, format.block);
			const BufferAttributes attributes{UINT32_MAX, ahead, UINT32_MAX, UINT32_MAX, UINT32_MAX};
			int error = 0;
			stream = api->open(nullptr, "Raven", 1, nullptr, "Windows program", &spec, nullptr, &attributes, &error);
		}
		worker = std::thread([this] { play(); });
	}
	~WaveOut() override
	{
		{
			std::lock_guard lock(mutex);
			stopping = true;
		}
		changed.notify_all();
		worker.join();
		if (stream)
			pulse()->close(stream);
	}
	void enqueue(uint64_t header, std::vector<uint8_t> data)
	{
		{
			std::lock_guard lock(mutex);
			queued.push_back({header, std::move(data)});
		}
		changed.notify_all();
	}
	// Every buffer not played yet finishes now, and the position starts again from zero.
	void reset()
	{
		std::lock_guard lock(mutex);
		for (const auto &item : queued)
			finished.push_back(item.header);
		queued.clear();
		played = 0;
		paused = false;
		if (stream) {
			int error = 0;
			pulse()->flush(stream, &error);
		}
		changed.notify_all();
	}
	void pause(bool value)
	{
		{
			std::lock_guard lock(mutex);
			paused = value;
		}
		changed.notify_all();
	}
	bool busy()
	{
		std::lock_guard lock(mutex);
		return !queued.empty() || playing;
	}
	std::vector<uint64_t> take_finished()
	{
		std::lock_guard lock(mutex);
		return std::exchange(finished, {});
	}
	uint64_t position()
	{
		std::lock_guard lock(mutex);
		return played;
	}

	const Format format;
	const uint32_t callback;
	const uint64_t target;
	uint32_t volume = 0xffffffffU;

private:
	struct Item {
		uint64_t header;
		std::vector<uint8_t> data;
	};
	void play()
	{
		std::unique_lock lock(mutex);
		for (;;) {
			changed.wait(lock, [&] { return stopping || (!paused && !queued.empty()); });
			if (stopping)
				return;
			auto item = std::move(queued.front());
			queued.pop_front();
			playing = true;
			lock.unlock();
			if (stream) {
				int error = 0;
				pulse()->write(stream, item.data.data(), item.data.size(), &error);
			} else {
				// Played to nowhere at the pace of the format.
				std::this_thread::sleep_for(std::chrono::microseconds(uint64_t(item.data.size()) * 1000000 / std::max(format.bytes_per_second(), 1U)));
			}
			lock.lock();
			playing = false;
			played += item.data.size();
			finished.push_back(item.header);
		}
	}

	void *stream = nullptr;
	std::mutex mutex;
	std::condition_variable changed;
	std::deque<Item> queued;
	std::vector<uint64_t> finished;
	uint64_t played = 0;
	bool stopping = false, paused = false, playing = false;
	std::thread worker;
};

}

void register_midi_api(ApiRegistry &registry)
{
	// No MIDI port, in or out: a program lists none, and opening one finds no such device; the mapper finds no device behind it.
	constexpr uint32_t midierr_nodevice = 68, midi_mapper = 0xffffffffU;
	for (const char *name : {"midiInGetNumDevs", "midiOutGetNumDevs"})
		registry.add(name, 0, [](ApiCall &call) { call.ret(0); });
	for (const char *name : {"midiInGetDevCapsW", "midiInGetDevCapsA", "midiOutGetDevCapsW", "midiOutGetDevCapsA"})
		registry.add(name, 3, [](ApiCall &call) { call.ret(mmsyserr_baddeviceid); });
	for (const char *name : {"midiInOpen", "midiOutOpen"})
		registry.add(name, 5, [](ApiCall &call) {
			if (call[0])
				call.store_pointer(call[0], 0);
			call.ret(call.u32(1) == midi_mapper ? midierr_nodevice : mmsyserr_baddeviceid);
		});
}

void register_wave_api(ApiRegistry &registry, KernelContext &context)
{
	register_midi_api(registry);
	auto &objects = context.objects;
	auto devices = std::make_shared<std::map<uint64_t, std::weak_ptr<WaveOut>>>();
	const auto device_of = [&objects](uint64_t handle) { return objects.object_as<WaveOut>(handle); };
	const auto flags_at = [](ApiCall &call, uint64_t header) { return header + (call.pointer_width() == 4 ? 16 : 24); };

	// Finished headers are marked done whenever the scheduler runs, so a program that watches WHDR_DONE sees its buffers come back.
	context.threads.add_service([devices, &context] {
		for (auto entry = devices->begin(); entry != devices->end();) {
			const auto device = entry->second.lock();
			if (!device) {
				entry = devices->erase(entry);
				continue;
			}
			const auto done = device->take_finished();
			const size_t width = context.process.mode() == NativeMode::x86 ? 4 : 8;
			for (const auto header : done) {
				try {
					const uint64_t at = header + (width == 4 ? 16 : 24);
					const auto raw = context.process.read_memory(at, 4);
					uint32_t flags;
					std::memcpy(&flags, raw.data(), 4);
					flags = (flags | whdr_done) & ~whdr_inqueue;
					context.process.write_memory(at, std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(&flags), 4));
				} catch (...) {
				}
				if (device->callback == callback_event && device->target)
					context.objects.set_event(device->target, true);
			}
			++entry;
		}
	});

	registry.add("waveOutGetNumDevs", 0, [](ApiCall &call) { call.ret(1); });
	// There is no capture device: recording programs hear that none exists.
	registry.add("waveInGetNumDevs", 0, [](ApiCall &call) { call.ret(0); });
	// There is no capture device and no mixer: their calls report a bad device or no driver, as Windows does on such a machine.
	registry.add("waveInMessage", 4, [](ApiCall &call) { call.ret(2); });
	registry.add("waveInOpen", 6, [](ApiCall &call) {
		if (call[0])
			call.store_pointer(call[0], 0);
		call.ret(2);
	});
	for (const char *name : {"waveInClose", "waveInStart", "waveInStop", "waveInReset"})
		registry.add(name, 1, [](ApiCall &call) { call.ret(5); });
	registry.add("mixerGetNumDevs", 0, [](ApiCall &call) { call.ret(0); });
	registry.add("mixerOpen", 5, [](ApiCall &call) {
		if (call[0])
			call.store_pointer(call[0], 0);
		call.ret(6);
	});
	registry.add("mixerGetID", 3, [](ApiCall &call) { call.ret(6); });
	for (const auto *name : {"waveInGetDevCapsW", "waveInGetDevCapsA"})
		registry.add(name, 3, [](ApiCall &call) { call.ret(mmsyserr_baddeviceid); });
	for (const bool wide : {true, false})
		registry.add(wide ? "waveOutGetDevCapsW" : "waveOutGetDevCapsA", 3, [wide](ApiCall &call) {
			if (call[0] != 0 && call[0] != wave_mapper && uint32_t(call[0]) != uint32_t(wave_mapper)) {
				call.ret(mmsyserr_baddeviceid);
				return;
			}
			const size_t size = wide ? 84 : 52;
			if (!call[1] || call.u32(2) < 8) {
				call.ret(mmsyserr_invalparam);
				return;
			}
			std::vector<uint8_t> caps(size, 0);
			const uint16_t manufacturer = 1, product = 2;
			std::memcpy(caps.data(), &manufacturer, 2);
			std::memcpy(caps.data() + 2, &product, 2);
			const std::string name = "Speakers (Raven)";
			for (size_t index = 0; index < name.size(); ++index)
				caps[8 + index * (wide ? 2 : 1)] = uint8_t(name[index]);
			const size_t after = wide ? 72 : 40;
			const uint32_t formats = 0xfffff;
			const uint16_t channels = 2;
			const uint32_t support = 0x4 | 0x8;
			std::memcpy(caps.data() + after, &formats, 4);
			std::memcpy(caps.data() + after + 4, &channels, 2);
			std::memcpy(caps.data() + after + 8, &support, 4);
			call.write(call[1], std::span<const uint8_t>(caps.data(), std::min<size_t>(size, call.u32(2))));
			call.ret(0);
		});
	registry.add("waveOutOpen", 6, [&objects, devices](ApiCall &call) {
		const uint32_t flags = call.u32(5);
		if (call[1] != 0 && uint32_t(call[1]) != uint32_t(wave_mapper)) {
			call.ret(mmsyserr_baddeviceid);
			return;
		}
		if (!call[2]) {
			call.ret(mmsyserr_invalparam);
			return;
		}
		const auto format = read_format(call, call[2]);
		if (!format) {
			call.ret(waverr_badformat);
			return;
		}
		if (flags & wave_format_query) {
			call.ret(0);
			return;
		}
		// Window, thread and function callbacks need the window system or a guest thread of their own; only events are told here.
		const uint32_t kind = flags & callback_kind;
		if (kind != callback_null && kind != callback_event) {
			call.ret(kind == 0x10000 || kind == 0x20000 || kind == 0x30000 ? mmsyserr_notsupported : mmsyserr_invalflag);
			return;
		}
		if (!call[0]) {
			call.ret(mmsyserr_invalparam);
			return;
		}
		const auto device = std::make_shared<WaveOut>(*format, kind, kind == callback_event ? call[3] : 0);
		const auto made = objects.add_object(device);
		if (made.status) {
			call.ret(mmsyserr_nomem);
			return;
		}
		(*devices)[made.value] = device;
		call.store_pointer(call[0], made.value);
		if (kind == callback_event && call[3])
			objects.set_event(call[3], true);
		call.ret(0);
	});
	registry.add("waveOutPrepareHeader", 3, [device_of, flags_at](ApiCall &call) {
		if (!device_of(call[0])) {
			call.ret(mmsyserr_invalhandle);
			return;
		}
		if (!call[1] || call.u32(2) < (call.pointer_width() == 4 ? 32U : 48U)) {
			call.ret(mmsyserr_invalparam);
			return;
		}
		const uint64_t at = flags_at(call, call[1]);
		call.store<uint32_t>(at, call.load<uint32_t>(at) | whdr_prepared);
		call.ret(0);
	});
	registry.add("waveOutUnprepareHeader", 3, [device_of, flags_at](ApiCall &call) {
		if (!device_of(call[0])) {
			call.ret(mmsyserr_invalhandle);
			return;
		}
		if (!call[1]) {
			call.ret(mmsyserr_invalparam);
			return;
		}
		const uint64_t at = flags_at(call, call[1]);
		const uint32_t flags = call.load<uint32_t>(at);
		if (flags & whdr_inqueue) {
			call.ret(waverr_stillplaying);
			return;
		}
		call.store<uint32_t>(at, flags & ~whdr_prepared);
		call.ret(0);
	});
	// The samples are taken when the buffer is written, as the header promises they stay put until it is done. A buffer that begins and
	// ends a loop plays as often as it asks.
	registry.add("waveOutWrite", 3, [device_of, flags_at](ApiCall &call) {
		const auto device = device_of(call[0]);
		if (!device) {
			call.ret(mmsyserr_invalhandle);
			return;
		}
		if (!call[1]) {
			call.ret(mmsyserr_invalparam);
			return;
		}
		const uint64_t at = flags_at(call, call[1]);
		const uint32_t flags = call.load<uint32_t>(at);
		if (!(flags & whdr_prepared)) {
			call.ret(waverr_unprepared);
			return;
		}
		if (flags & whdr_inqueue) {
			call.ret(waverr_stillplaying);
			return;
		}
		const uint64_t data = call.load_pointer(call[1]);
		const uint32_t length = call.load<uint32_t>(call[1] + call.pointer_width());
		std::vector<uint8_t> samples = length ? call.read(data, length) : std::vector<uint8_t>();
		const uint32_t loops = call.load<uint32_t>(at + 4);
		if ((flags & whdr_beginloop) && (flags & whdr_endloop) && loops > 1 && size_t(length) * loops <= (64u << 20)) {
			std::vector<uint8_t> repeated;
			repeated.reserve(size_t(length) * loops);
			for (uint32_t index = 0; index < loops; ++index)
				repeated.insert(repeated.end(), samples.begin(), samples.end());
			samples = std::move(repeated);
		}
		// The volume scales samples of the formats it can: 16-bit integers and floats, left and right halves of the volume word.
		if (device->volume != 0xffffffffU) {
			const float left = float(device->volume & 0xffff) / 65535.0f, right = float(device->volume >> 16) / 65535.0f;
			const size_t channels = device->format.channels;
			if (device->format.floating)
				for (size_t index = 0; index + 4 <= samples.size(); index += 4) {
					float value;
					std::memcpy(&value, samples.data() + index, 4);
					value *= (index / 4) % channels == 1 ? right : left;
					std::memcpy(samples.data() + index, &value, 4);
				}
			else if (device->format.bits == 16)
				for (size_t index = 0; index + 2 <= samples.size(); index += 2) {
					int16_t value;
					std::memcpy(&value, samples.data() + index, 2);
					value = int16_t(float(value) * ((index / 2) % channels == 1 ? right : left));
					std::memcpy(samples.data() + index, &value, 2);
				}
		}
		call.store<uint32_t>(at, (flags & ~whdr_done) | whdr_inqueue);
		device->enqueue(call[1], std::move(samples));
		call.ret(0);
	});
	registry.add("waveOutReset", 1, [device_of](ApiCall &call) {
		const auto device = device_of(call[0]);
		if (!device) {
			call.ret(mmsyserr_invalhandle);
			return;
		}
		device->reset();
		call.ret(0);
	});
	registry.add("waveOutPause", 1, [device_of](ApiCall &call) {
		const auto device = device_of(call[0]);
		call.ret(device ? (device->pause(true), 0U) : mmsyserr_invalhandle);
	});
	registry.add("waveOutRestart", 1, [device_of](ApiCall &call) {
		const auto device = device_of(call[0]);
		call.ret(device ? (device->pause(false), 0U) : mmsyserr_invalhandle);
	});
	registry.add("waveOutClose", 1, [&objects, device_of](ApiCall &call) {
		const auto device = device_of(call[0]);
		if (!device) {
			call.ret(mmsyserr_invalhandle);
			return;
		}
		if (device->busy()) {
			call.ret(waverr_stillplaying);
			return;
		}
		objects.close(call[0]);
		call.ret(0);
	});
	// MMTIME: the kind asked for (milliseconds, samples or bytes; other kinds get bytes) and the position played since the last reset.
	registry.add("waveOutGetPosition", 3, [device_of](ApiCall &call) {
		const auto device = device_of(call[0]);
		if (!device) {
			call.ret(mmsyserr_invalhandle);
			return;
		}
		if (!call[1] || call.u32(2) < 12) {
			call.ret(mmsyserr_invalparam);
			return;
		}
		const uint64_t bytes = device->position();
		uint32_t kind = call.load<uint32_t>(call[1]), value;
		if (kind == 1)
			value = uint32_t(bytes * 1000 / std::max(device->format.bytes_per_second(), 1U));
		else if (kind == 2)
			value = uint32_t(bytes / std::max(device->format.block, 1U));
		else {
			kind = 4;
			value = uint32_t(bytes);
		}
		call.store<uint32_t>(call[1], kind);
		call.store<uint32_t>(call[1] + 4, value);
		call.ret(0);
	});
	registry.add("waveOutGetVolume", 2, [device_of](ApiCall &call) {
		const auto device = device_of(call[0]);
		if (!call[1]) {
			call.ret(mmsyserr_invalparam);
			return;
		}
		call.store<uint32_t>(call[1], device ? device->volume : 0xffffffffU);
		call.ret(0);
	});
	registry.add("waveOutSetVolume", 2, [device_of](ApiCall &call) {
		if (const auto device = device_of(call[0]))
			device->volume = call.u32(1);
		call.ret(0);
	});
	registry.add("waveOutGetID", 2, [device_of](ApiCall &call) {
		if (!device_of(call[0])) {
			call.ret(mmsyserr_invalhandle);
			return;
		}
		if (call[1])
			call.store<uint32_t>(call[1], 0);
		call.ret(0);
	});
	registry.add("waveOutMessage", 4, [](ApiCall &call) { call.ret(mmsyserr_notsupported); });
	for (const bool wide : {true, false})
		registry.add(wide ? "waveOutGetErrorTextW" : "waveOutGetErrorTextA", 3, [wide](ApiCall &call) {
			static const std::map<uint32_t, std::string> texts{{0, "The command completed successfully."},
				{mmsyserr_error, "An unspecified error occurred."}, {mmsyserr_baddeviceid, "The device identifier is out of range."},
				{mmsyserr_invalhandle, "The handle is not valid."}, {mmsyserr_nomem, "There is not enough memory."},
				{mmsyserr_notsupported, "The function is not supported."}, {mmsyserr_invalflag, "A flag is not valid."},
				{mmsyserr_invalparam, "A parameter is not valid."}, {waverr_badformat, "The wave format is not supported."},
				{waverr_stillplaying, "The device is still playing."}, {waverr_unprepared, "The header is not prepared."}};
			const auto found = texts.find(call.u32(0));
			if (found == texts.end()) {
				call.ret(mmsyserr_baddeviceid);
				return;
			}
			if (!call[1] || !call.u32(2)) {
				call.ret(mmsyserr_invalparam);
				return;
			}
			if (wide)
				call.put_wide(call[1], from_utf8(found->second), call.u32(2));
			else
				call.put_narrow(call[1], found->second, call.u32(2));
			call.ret(0);
		});
}

}
