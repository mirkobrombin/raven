#include "audio_engine.h"
#include "api.h"
#include "kernel_context.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <span>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace raven {

namespace {

// The client library of PulseAudio's protocol, which PipeWire also serves: a threaded main loop, one context per process and a stream
// per device. Declared here because the runtime loads the library when a program first opens a device, if it is installed at all.
struct pa_sample_spec {
	int format;
	uint32_t rate;
	uint8_t channels;
};

struct pa_buffer_attr {
	uint32_t maxlength, tlength, prebuf, minreq, fragsize;
};

using pa_notify = void (*)(void *, void *);
using pa_request = void (*)(void *, size_t, void *);
using pa_success = void (*)(void *, int, void *);

constexpr int pa_context_ready = 4, pa_context_failed = 5, pa_context_terminated = 6;
constexpr int pa_stream_ready = 2, pa_stream_failed = 3, pa_stream_terminated = 4;
constexpr int pa_operation_running = 0;
constexpr int pa_context_noautospawn = 0x1;
constexpr int pa_stream_start_corked = 0x1, pa_stream_interpolate_timing = 0x2, pa_stream_auto_timing_update = 0x8,
	pa_stream_adjust_latency = 0x2000;
constexpr int pa_seek_relative = 0;
constexpr int pa_sample_u8 = 0, pa_sample_s16le = 3, pa_sample_float32le = 5, pa_sample_s32le = 7, pa_sample_s24le = 9;

struct PulseApi {
	void *(*mainloop_new)();
	int (*mainloop_start)(void *);
	void (*mainloop_stop)(void *);
	void (*mainloop_free)(void *);
	void (*mainloop_lock)(void *);
	void (*mainloop_unlock)(void *);
	void (*mainloop_wait)(void *);
	void (*mainloop_signal)(void *, int);
	void *(*mainloop_get_api)(void *);
	void *(*context_new)(void *, const char *);
	int (*context_connect)(void *, const char *, int, const void *);
	void (*context_disconnect)(void *);
	void (*context_unref)(void *);
	int (*context_get_state)(void *);
	void (*context_set_state_callback)(void *, pa_notify, void *);
	void *(*stream_new)(void *, const char *, const pa_sample_spec *, const void *);
	int (*stream_connect_playback)(void *, const char *, const pa_buffer_attr *, int, const void *, void *);
	int (*stream_connect_record)(void *, const char *, const pa_buffer_attr *, int);
	int (*stream_disconnect)(void *);
	void (*stream_unref)(void *);
	int (*stream_get_state)(void *);
	void (*stream_set_state_callback)(void *, pa_notify, void *);
	void (*stream_set_write_callback)(void *, pa_request, void *);
	void (*stream_set_read_callback)(void *, pa_request, void *);
	void (*stream_set_underflow_callback)(void *, pa_notify, void *);
	size_t (*stream_writable_size)(void *);
	int (*stream_write)(void *, const void *, size_t, void (*)(void *), int64_t, int);
	int (*stream_peek)(void *, const void **, size_t *);
	int (*stream_drop)(void *);
	void *(*stream_cork)(void *, int, pa_success, void *);
	void *(*stream_flush)(void *, pa_success, void *);
	void *(*stream_update_timing_info)(void *, pa_success, void *);
	int (*stream_get_time)(void *, uint64_t *);
	int (*operation_get_state)(void *);
	void (*operation_unref)(void *);
};

const PulseApi *pulse_api()
{
	static const std::unique_ptr<PulseApi> loaded = []() -> std::unique_ptr<PulseApi> {
		void *library = ::dlopen("libpulse.so.0", RTLD_NOW | RTLD_LOCAL);
		if (!library)
			return nullptr;
		auto api = std::make_unique<PulseApi>();
		bool complete = true;
		const auto bind = [&](auto &slot, const char *name) {
			slot = reinterpret_cast<std::remove_reference_t<decltype(slot)>>(::dlsym(library, name));
			complete = complete && slot;
		};
		bind(api->mainloop_new, "pa_threaded_mainloop_new");
		bind(api->mainloop_start, "pa_threaded_mainloop_start");
		bind(api->mainloop_stop, "pa_threaded_mainloop_stop");
		bind(api->mainloop_free, "pa_threaded_mainloop_free");
		bind(api->mainloop_lock, "pa_threaded_mainloop_lock");
		bind(api->mainloop_unlock, "pa_threaded_mainloop_unlock");
		bind(api->mainloop_wait, "pa_threaded_mainloop_wait");
		bind(api->mainloop_signal, "pa_threaded_mainloop_signal");
		bind(api->mainloop_get_api, "pa_threaded_mainloop_get_api");
		bind(api->context_new, "pa_context_new");
		bind(api->context_connect, "pa_context_connect");
		bind(api->context_disconnect, "pa_context_disconnect");
		bind(api->context_unref, "pa_context_unref");
		bind(api->context_get_state, "pa_context_get_state");
		bind(api->context_set_state_callback, "pa_context_set_state_callback");
		bind(api->stream_new, "pa_stream_new");
		bind(api->stream_connect_playback, "pa_stream_connect_playback");
		bind(api->stream_connect_record, "pa_stream_connect_record");
		bind(api->stream_disconnect, "pa_stream_disconnect");
		bind(api->stream_unref, "pa_stream_unref");
		bind(api->stream_get_state, "pa_stream_get_state");
		bind(api->stream_set_state_callback, "pa_stream_set_state_callback");
		bind(api->stream_set_write_callback, "pa_stream_set_write_callback");
		bind(api->stream_set_read_callback, "pa_stream_set_read_callback");
		bind(api->stream_set_underflow_callback, "pa_stream_set_underflow_callback");
		bind(api->stream_writable_size, "pa_stream_writable_size");
		bind(api->stream_write, "pa_stream_write");
		bind(api->stream_peek, "pa_stream_peek");
		bind(api->stream_drop, "pa_stream_drop");
		bind(api->stream_cork, "pa_stream_cork");
		bind(api->stream_flush, "pa_stream_flush");
		bind(api->stream_update_timing_info, "pa_stream_update_timing_info");
		bind(api->stream_get_time, "pa_stream_get_time");
		bind(api->operation_get_state, "pa_operation_get_state");
		bind(api->operation_unref, "pa_operation_unref");
		if (!complete)
			return nullptr;
		return api;
	}();
	return loaded.get();
}

// The process's connection to the sound server, made when the first stream opens and kept for the next ones.
class PulseContext {
public:
	static std::shared_ptr<PulseContext> get()
	{
		static std::mutex guard;
		static std::shared_ptr<PulseContext> shared;
		std::lock_guard lock(guard);
		// A server that went away (restarted, or the session ended) is connected to again.
		if (shared && !shared->ready())
			shared.reset();
		if (shared)
			return shared;
		if (const auto *api = pulse_api()) {
			auto made = std::shared_ptr<PulseContext>(new PulseContext(*api));
			if (made->connect())
				shared = std::move(made);
		}
		return shared;
	}
	~PulseContext()
	{
		if (!loop)
			return;
		if (context) {
			api.mainloop_lock(loop);
			api.context_set_state_callback(context, nullptr, nullptr);
			api.context_disconnect(context);
			api.context_unref(context);
			api.mainloop_unlock(loop);
		}
		api.mainloop_stop(loop);
		api.mainloop_free(loop);
	}

	class Lock {
	public:
		explicit Lock(const PulseContext &pulse) : pulse(pulse) { pulse.api.mainloop_lock(pulse.loop); }
		~Lock() { pulse.api.mainloop_unlock(pulse.loop); }
		Lock(const Lock &) = delete;
		Lock &operator=(const Lock &) = delete;
	private:
		const PulseContext &pulse;
	};

	// Waits, with the main loop locked, for an operation started under that lock to finish. A stream the server dropped starts none.
	void finish(void *operation) const
	{
		if (!operation)
			return;
		while (api.operation_get_state(operation) == pa_operation_running)
			api.mainloop_wait(loop);
		api.operation_unref(operation);
	}
	bool ready() const
	{
		Lock lock(*this);
		return api.context_get_state(context) == pa_context_ready;
	}
	static void signal(void *, int, void *userdata)
	{
		auto *pulse = static_cast<PulseContext *>(userdata);
		pulse->api.mainloop_signal(pulse->loop, 0);
	}

	// A copy: the loaded table is destroyed before the shared context at exit.
	const PulseApi api;
	void *loop = nullptr;
	void *context = nullptr;

private:
	explicit PulseContext(const PulseApi &api) : api(api) {}
	bool connect()
	{
		loop = api.mainloop_new();
		if (!loop)
			return false;
		context = api.context_new(api.mainloop_get_api(loop), "Raven");
		if (!context)
			return false;
		api.context_set_state_callback(context, [](void *, void *userdata) {
			auto *pulse = static_cast<PulseContext *>(userdata);
			pulse->api.mainloop_signal(pulse->loop, 0);
		}, this);
		if (api.context_connect(context, nullptr, pa_context_noautospawn, nullptr) < 0 || api.mainloop_start(loop) < 0)
			return false;
		Lock lock(*this);
		for (;;) {
			const int state = api.context_get_state(context);
			if (state == pa_context_ready)
				return true;
			if (state == pa_context_failed || state == pa_context_terminated)
				return false;
			api.mainloop_wait(loop);
		}
	}
};

struct Mapping {
	int descriptor = -1;
	size_t size = 0;
	RavenAudioShared *page = nullptr;
};

void scale(std::span<uint8_t> samples, const AudioFormat &format, float volume)
{
	// The page is the guest's to write: whatever it holds, the gain stays between silence and the samples as written.
	volume = volume >= 0.0f ? std::min(volume, 1.0f) : 0.0f;
	if (format.floating)
		for (size_t index = 0; index + 4 <= samples.size(); index += 4) {
			float value;
			std::memcpy(&value, samples.data() + index, 4);
			value *= volume;
			std::memcpy(samples.data() + index, &value, 4);
		}
	else if (format.bits == 16)
		for (size_t index = 0; index + 2 <= samples.size(); index += 2) {
			int16_t value;
			std::memcpy(&value, samples.data() + index, 2);
			value = int16_t(std::clamp(float(value) * volume, -32768.0f, 32767.0f));
			std::memcpy(samples.data() + index, &value, 2);
		}
}

}

std::optional<AudioFormat> read_wave_format(ApiCall &call, uint64_t address)
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
		// cbSize, wValidBitsPerSample and dwChannelMask come before SubFormat: the format tag, then the same twelve bytes for every tag.
		static constexpr uint8_t subformat_tail[12] = {0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};
		const auto extended = call.read(address + 16, 24);
		uint16_t extra;
		std::memcpy(&extra, extended.data(), 2);
		uint32_t subformat;
		std::memcpy(&subformat, extended.data() + 8, 4);
		if (extra < 22 || subformat > 0xffff || std::memcmp(extended.data() + 12, subformat_tail, sizeof subformat_tail))
			return std::nullopt;
		tag = uint16_t(subformat);
	}
	AudioFormat format{rate, channels, bits, block, tag == 3};
	if ((tag != 1 && tag != 3) || !channels || channels > 8 || rate < 1000 || rate > 384000 || block != channels * (bits / 8) ||
		(tag == 1 && bits != 8 && bits != 16 && bits != 24 && bits != 32) || (tag == 3 && bits != 32))
		return std::nullopt;
	return format;
}

AudioStream::AudioStream(Options options, int memory, size_t size, RavenAudioShared *page)
	: stream_format(options.format), capturing(options.capture), ring_frames(options.buffer_frames), period_frames(options.period_frames),
	  notify(std::move(options.notify)), memory(memory), size(size), page(page)
{
}

AudioStream::~AudioStream()
{
	::munmap(page, size);
	::close(memory);
}

uint64_t AudioStream::pending() const
{
	const uint64_t held = counter(page->write_frames).load(std::memory_order_acquire) - counter(page->read_frames).load(std::memory_order_relaxed);
	return std::min<uint64_t>(held, ring_frames);
}

namespace {

// Copies `count` frames between a flat buffer and the ring starting at the frame counter `at`, across the ring's end if need be. Without
// a flat buffer, frames going into the ring are silence: `silence` in every byte.
void copy_ring(uint8_t *ring, uint64_t frames, uint32_t block, uint64_t at, uint8_t *flat, uint64_t count, bool into_ring,
	uint8_t silence = 0)
{
	const uint64_t start = at % frames, first = std::min(count, frames - start);
	const auto move = [&](uint64_t ring_frame, uint64_t flat_frame, uint64_t length) {
		if (!length)
			return;
		uint8_t *place = ring + ring_frame * block;
		if (!flat) {
			if (into_ring)
				std::memset(place, silence, length * block);
			return;
		}
		uint8_t *other = flat + flat_frame * block;
		if (into_ring)
			std::memcpy(place, other, length * block);
		else
			std::memcpy(other, place, length * block);
	};
	move(start, 0, first);
	move(0, first, count - first);
}

}

void AudioStream::consume(uint8_t *out, uint64_t count)
{
	const uint64_t at = counter(page->read_frames).load(std::memory_order_relaxed);
	copy_ring(ring(), ring_frames, stream_format.block, at, out, count, false);
	counter(page->read_frames).store(at + count, std::memory_order_release);
}

void AudioStream::produce(const uint8_t *in, uint64_t count)
{
	const uint64_t at = counter(page->write_frames).load(std::memory_order_relaxed);
	const uint64_t used = at - counter(page->read_frames).load(std::memory_order_acquire);
	const uint64_t room = ring_frames - std::min<uint64_t>(used, ring_frames), taken = std::min(count, room);
	// Unsigned 8-bit samples are silent at their midpoint.
	const uint8_t silence = !stream_format.floating && stream_format.bits == 8 ? 0x80 : 0;
	copy_ring(ring(), ring_frames, stream_format.block, at, const_cast<uint8_t *>(in), taken, true, silence);
	counter(page->write_frames).store(at + taken, std::memory_order_release);
	if (taken < count)
		counter(page->glitch_frames).fetch_add(count - taken);
}

void AudioStream::set_position(uint64_t frames)
{
	counter(page->position_counter).store(counter_ticks());
	counter(page->position_frames).store(frames);
}

void AudioStream::clear_counters()
{
	for (auto *field : {&page->write_frames, &page->read_frames, &page->glitch_frames})
		counter(*field).store(0);
	set_position(0);
}

void AudioStream::set_running(bool value) { std::atomic_ref<uint32_t>(page->running).store(value); }

float AudioStream::volume() const { return std::bit_cast<float>(std::atomic_ref<uint32_t>(page->volume).load()); }

namespace {

// The device that plays to nowhere and records silence: a host thread that, once a period, takes or gives as many frames as the time
// since the start asks for, so a program's buffers come back at the pace they would from a sound card.
class NullStream final : public AudioStream {
public:
	NullStream(Options options, Mapping mapping) : AudioStream(std::move(options), mapping.descriptor, mapping.size, mapping.page)
	{
		worker = std::thread([this] { run(); });
	}
	~NullStream() override
	{
		{
			std::lock_guard lock(mutex);
			closing = true;
		}
		changed.notify_all();
		worker.join();
	}
	void start() override
	{
		{
			std::lock_guard lock(mutex);
			if (active)
				return;
			active = true;
			since = std::chrono::steady_clock::now();
			before = device_frames;
			// The position is stamped as of the start: a clock read before the first period carries on from now, not from the stop.
			set_position(played);
			set_running(true);
		}
		changed.notify_all();
	}
	void stop() override
	{
		std::lock_guard lock(mutex);
		active = false;
		set_running(false);
	}
	void reset() override
	{
		std::lock_guard lock(mutex);
		active = false;
		set_running(false);
		clear_counters();
		device_frames = before = played = 0;
	}

private:
	void run()
	{
		const uint64_t rate = stream_format.rate, second = 1000000000;
		// Split at whole seconds: the plain product passes 64 bits after some hours of one run.
		const auto frames_in = [&](uint64_t nanoseconds) { return nanoseconds / second * rate + nanoseconds % second * rate / second; };
		const auto time_of = [&](uint64_t frames) { return std::chrono::nanoseconds(frames / rate * second + frames % rate * second / rate); };
		std::unique_lock lock(mutex);
		while (!closing) {
			if (!active) {
				changed.wait(lock);
				continue;
			}
			const auto due = since + time_of(device_frames - before + period_frames);
			changed.wait_until(lock, due);
			if (closing || !active)
				continue;
			const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - since).count();
			const uint64_t total = before + frames_in(uint64_t(elapsed));
			if (total <= device_frames)
				continue;
			const uint64_t frames = total - device_frames;
			device_frames = total;
			if (capturing) {
				// Stamped before the frames are published, so a program that sees them sees the time they were recorded by.
				played += frames;
				set_position(played);
				produce(nullptr, frames);
			} else {
				const uint64_t taken = std::min(frames, pending());
				consume(nullptr, taken);
				played += taken;
				if (taken < frames)
					counter(shared().glitch_frames).fetch_add(frames - taken);
				set_position(played);
			}
			lock.unlock();
			if (notify)
				notify();
			lock.lock();
		}
	}

	std::mutex mutex;
	std::condition_variable changed;
	bool closing = false, active = false;
	std::chrono::steady_clock::time_point since;
	// Frames of device time: in all, and when the current run started; and the frames of the ring played or captured.
	uint64_t device_frames = 0, before = 0, played = 0;
	std::thread worker;
};

// A stream of the sound server. Playback is fed from the ring whenever the server asks for data and once a period from a host thread,
// which also tells the owner; the position is the server's interpolated playback time, never past what was written and all of it once
// the server ran out. Recording copies what the server delivers into the ring.
class PulseStream final : public AudioStream {
public:
	PulseStream(Options options, Mapping mapping, std::shared_ptr<PulseContext> context)
		: AudioStream(std::move(options), mapping.descriptor, mapping.size, mapping.page), pulse(std::move(context))
	{
	}
	~PulseStream() override
	{
		if (ticker.joinable()) {
			{
				std::lock_guard lock(mutex);
				closing = true;
			}
			changed.notify_all();
			ticker.join();
		}
		if (stream) {
			PulseContext::Lock lock(*pulse);
			const auto &api = pulse->api;
			api.stream_set_state_callback(stream, nullptr, nullptr);
			api.stream_set_write_callback(stream, nullptr, nullptr);
			api.stream_set_read_callback(stream, nullptr, nullptr);
			api.stream_set_underflow_callback(stream, nullptr, nullptr);
			api.stream_disconnect(stream);
			api.stream_unref(stream);
		}
	}
	bool connect()
	{
		const auto &api = pulse->api;
		const auto &format = stream_format;
		const int sample = format.floating ? pa_sample_float32le : format.bits == 8 ? pa_sample_u8 : format.bits == 16 ? pa_sample_s16le :
			format.bits == 24 ? pa_sample_s24le : pa_sample_s32le;
		const pa_sample_spec spec{sample, format.rate, uint8_t(format.channels)};
		const uint32_t period = period_frames * format.block;
		PulseContext::Lock lock(*pulse);
		stream = api.stream_new(pulse->context, capturing ? "Windows program recording" : "Windows program", &spec, nullptr);
		if (!stream)
			return false;
		api.stream_set_state_callback(stream, [](void *, void *userdata) {
			auto *self = static_cast<PulseStream *>(userdata);
			self->pulse->api.mainloop_signal(self->pulse->loop, 0);
		}, this);
		const int flags = pa_stream_start_corked | pa_stream_interpolate_timing | pa_stream_auto_timing_update | pa_stream_adjust_latency;
		int result;
		if (capturing) {
			api.stream_set_read_callback(stream, [](void *, size_t, void *userdata) { static_cast<PulseStream *>(userdata)->take(); }, this);
			const pa_buffer_attr attributes{UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, period};
			result = api.stream_connect_record(stream, nullptr, &attributes, flags);
		} else {
			api.stream_set_write_callback(stream, [](void *, size_t, void *userdata) { static_cast<PulseStream *>(userdata)->feed(); }, this);
			api.stream_set_underflow_callback(stream, [](void *, void *userdata) { static_cast<PulseStream *>(userdata)->underflowed = true; }, this);
			// Four periods ahead of the speaker, asked for a period at a time; playback starts, and resumes after running dry, as soon as
			// a frame is there, so the last short piece of a sound is not held back waiting for more.
			const pa_buffer_attr attributes{UINT32_MAX, period * 4, format.block, period, UINT32_MAX};
			result = api.stream_connect_playback(stream, nullptr, &attributes, flags, nullptr, nullptr);
		}
		if (result < 0)
			return false;
		for (;;) {
			const int state = api.stream_get_state(stream);
			if (state == pa_stream_ready)
				break;
			if (state == pa_stream_failed || state == pa_stream_terminated)
				return false;
			api.mainloop_wait(pulse->loop);
		}
		ticker = std::thread([this] { tick(); });
		return true;
	}
	void start() override
	{
		PulseContext::Lock lock(*pulse);
		if (active)
			return;
		active = true;
		pulse->finish(pulse->api.stream_cork(stream, 0, &PulseContext::signal, pulse.get()));
		// finish() releases the lock while it waits, so a stop can come in between.
		if (!active)
			return;
		set_position(played);
		set_running(true);
		feed();
		set_ticking(true);
	}
	void stop() override
	{
		PulseContext::Lock lock(*pulse);
		halt();
	}
	void reset() override
	{
		PulseContext::Lock lock(*pulse);
		halt();
		pulse->finish(pulse->api.stream_flush(stream, &PulseContext::signal, pulse.get()));
		// The flush moved the server's clock; what the client has cached of it is stale until asked again.
		pulse->finish(pulse->api.stream_update_timing_info(stream, &PulseContext::signal, pulse.get()));
		clear_counters();
		written = played = 0;
		underflowed = false;
		uint64_t now = 0;
		base = pulse->api.stream_get_time(stream, &now) == 0 ? now : 0;
	}

private:
	void halt()
	{
		if (!active)
			return;
		active = false;
		pulse->finish(pulse->api.stream_cork(stream, 1, &PulseContext::signal, pulse.get()));
		set_running(false);
		set_ticking(false);
		update();
	}
	void set_ticking(bool value)
	{
		if (ticker.joinable()) {
			{
				std::lock_guard lock(mutex);
				ticking = value;
			}
			changed.notify_all();
		}
	}
	// With the main loop locked: everything the ring holds that the server has room for goes to it.
	void feed()
	{
		if (!active || capturing)
			return;
		const auto &api = pulse->api;
		const size_t room = api.stream_writable_size(stream);
		if (room != size_t(-1)) {
			const uint64_t frames = std::min<uint64_t>(room / stream_format.block, pending());
			if (frames) {
				if (played == written)
					anchor();
				scratch.resize(frames * stream_format.block);
				consume(scratch.data(), frames);
				if (const float gain = volume(); gain != 1.0f)
					scale(scratch, stream_format, gain);
				if (api.stream_write(stream, scratch.data(), scratch.size(), nullptr, 0, pa_seek_relative) == 0) {
					written += frames;
					underflowed = false;
				}
			}
		}
		update();
	}
	void update()
	{
		uint64_t now = 0, heard = played;
		if (underflowed)
			heard = written;
		else if (pulse->api.stream_get_time(stream, &now) == 0 && now > base)
			heard = std::min<uint64_t>(written, (now - base) * stream_format.rate / 1000000);
		played = std::max(played, heard);
		if (played == written)
			anchor();
		if (!capturing)
			set_position(played);
	}
	// With everything written heard: the clock's base moves up to now, so the time the device spends with nothing to play (run dry,
	// or not yet fed since a reset) is not counted as frames played the moment more arrive.
	void anchor()
	{
		uint64_t now = 0;
		if (pulse->api.stream_get_time(stream, &now) != 0)
			return;
		const uint64_t heard = written * 1000000 / stream_format.rate;
		if (now > base + heard)
			base = now - heard;
	}
	// With the main loop locked, after the server dropped the stream: a period plays to nowhere or records silence, so the program's
	// buffers still come back.
	void discard()
	{
		if (capturing) {
			played += period_frames;
			set_position(played);
			produce(nullptr, period_frames);
			return;
		}
		const uint64_t frames = std::min<uint64_t>(period_frames, pending());
		consume(nullptr, frames);
		written += frames;
		played = written;
		set_position(played);
	}
	// With the main loop locked: the server delivered recorded frames.
	void take()
	{
		const auto &api = pulse->api;
		const void *data = nullptr;
		size_t bytes = 0;
		bool any = false;
		while (api.stream_peek(stream, &data, &bytes) == 0 && bytes) {
			const uint64_t frames = bytes / stream_format.block;
			if (active) {
				// Stamped before the frames are published, so a program that sees them sees the time they were recorded by.
				played += frames;
				set_position(played);
				produce(static_cast<const uint8_t *>(data), frames);
				any = true;
			}
			api.stream_drop(stream);
		}
		if (any && notify)
			notify();
	}
	// Once a period while the stream runs: feeds the server and tells the owner of a playback stream, and stands in for a server that
	// dropped the stream; parked while it is stopped. The thread's own lock is never held while it takes the main loop's, which start and
	// halt hold while they take the thread's.
	void tick()
	{
		const auto period = std::chrono::nanoseconds(uint64_t(period_frames) * 1000000000ULL / stream_format.rate);
		auto next = std::chrono::steady_clock::now() + period;
		std::unique_lock lock(mutex);
		while (!closing) {
			if (!ticking) {
				changed.wait(lock, [this] { return closing || ticking; });
				next = std::chrono::steady_clock::now() + period;
				continue;
			}
			changed.wait_until(lock, next);
			if (closing || !ticking)
				continue;
			next = std::max(next + period, std::chrono::steady_clock::now());
			lock.unlock();
			bool tell;
			{
				PulseContext::Lock pulse_lock(*pulse);
				const bool lost = active && pulse->api.stream_get_state(stream) != pa_stream_ready;
				// take() tells the owner of a recording that the server still delivers.
				tell = active && (lost || !capturing);
				if (lost)
					discard();
				else
					feed();
				// The server ran dry and nothing came since: the device went a period without frames.
				if (active && underflowed)
					counter(shared().glitch_frames).fetch_add(period_frames);
			}
			if (tell && notify)
				notify();
			lock.lock();
		}
	}

	std::shared_ptr<PulseContext> pulse;
	void *stream = nullptr;
	// Guarded by the main loop's lock.
	bool active = false, underflowed = false;
	uint64_t written = 0, played = 0, base = 0;
	std::vector<uint8_t> scratch;
	// Guarded by `mutex`.
	std::mutex mutex;
	std::condition_variable changed;
	bool closing = false, ticking = false;
	std::thread ticker;
};

std::optional<Mapping> make_mapping(const AudioStream::Options &options)
{
	const auto &format = options.format;
	const uint32_t buffer = options.buffer_frames, period = options.period_frames;
	const uint64_t data = (uint64_t(buffer) * format.block + 4095) & ~uint64_t(4095);
	if (data > (64u << 20))
		return std::nullopt;
	Mapping mapping;
	mapping.size = RAVEN_AUDIO_DATA_OFFSET + data;
	mapping.descriptor = ::memfd_create("raven-audio", MFD_CLOEXEC);
	if (mapping.descriptor < 0)
		return std::nullopt;
	void *address = MAP_FAILED;
	if (::ftruncate(mapping.descriptor, off_t(mapping.size)) == 0)
		address = ::mmap(nullptr, mapping.size, PROT_READ | PROT_WRITE, MAP_SHARED, mapping.descriptor, 0);
	if (address == MAP_FAILED) {
		::close(mapping.descriptor);
		return std::nullopt;
	}
	mapping.page = static_cast<RavenAudioShared *>(address);
	auto &page = *mapping.page;
	page.volume = std::bit_cast<uint32_t>(1.0f);
	page.buffer_frames = buffer;
	page.frame_bytes = format.block;
	page.rate = format.rate;
	page.channels = format.channels;
	page.bits = format.bits;
	page.format_tag = format.floating ? 3 : 1;
	page.period_frames = period;
	page.flags = options.capture ? RAVEN_AUDIO_CAPTURE : 0;
	page.position_counter = counter_ticks();
	return mapping;
}

}

std::unique_ptr<AudioStream> AudioStream::open(Options options)
{
	const auto &format = options.format;
	if (!format.rate || !format.block || !options.period_frames || options.buffer_frames < options.period_frames)
		return nullptr;
	const char *chosen = std::getenv("RAVEN_AUDIO");
	if (!chosen || std::string_view(chosen) != "null")
		if (auto context = PulseContext::get()) {
			auto mapping = make_mapping(options);
			if (!mapping)
				return nullptr;
			auto stream = std::make_unique<PulseStream>(options, *mapping, std::move(context));
			if (stream->connect())
				return stream;
		}
	auto mapping = make_mapping(options);
	if (!mapping)
		return nullptr;
	return std::make_unique<NullStream>(std::move(options), *mapping);
}

}
