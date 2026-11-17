#include "audio_engine.h"
#include "event.h"
#include "kernel_context.h"

#include <mutex>
#include <vector>

namespace raven {

namespace {

// Views of streams whose last handle went, waiting to leave the guest: a handle can be closed where guest memory cannot be unmapped
// (a serial served CloseHandle), so the stream drops its view here and the service unmaps it at the next point that can.
struct ClosedViews {
	std::mutex mutex;
	std::vector<uint64_t> views;
};

// The event a stream sets once a period. The event object itself is held, so a handle the program closes or reuses meanwhile is never
// set by mistake; the stream's host thread reads it while the guest may replace it.
struct EventTarget {
	std::mutex mutex;
	std::shared_ptr<EventObject> event;
	void set()
	{
		std::shared_ptr<EventObject> current;
		{
			std::lock_guard lock(mutex);
			current = event;
		}
		if (current)
			current->set_state(true);
	}
};

// A stream of the sound engine opened for a guest component, with the view of its shared page in the guest. The view goes with the last
// handle, however that one is closed.
class GuestAudio : public KernelObject {
public:
	explicit GuestAudio(std::shared_ptr<ClosedViews> closed) : closed(std::move(closed)) {}
	std::shared_ptr<EventTarget> target;
	~GuestAudio() override
	{
		stream.reset();
		if (view) {
			std::lock_guard lock(closed->mutex);
			closed->views.push_back(view);
		}
	}

	std::unique_ptr<AudioStream> stream;
	uint64_t view = 0;

private:
	std::shared_ptr<ClosedViews> closed;
};

}

// ravenhost.dll's sound primitives, on which the guest's mmdevapi, dsound and winmm stand. The samples and the counters never pass
// through a call: they live in the shared page RavenAudioOpen maps (raven_audio.h), and the calls only open, start, stop, reset and close a
// stream and name the event it sets.
void register_audio_service(ApiRegistry &registry, KernelContext &context)
{
	auto &objects = context.objects;
	const auto stream_of = [&objects](uint64_t handle) { return objects.object_as<GuestAudio>(handle); };
	const auto wake = context.threads.remote_nudge();
	const auto closed = std::make_shared<ClosedViews>();
	const auto unmap_closed = [&context, closed] {
		if (context.process.serving() && !context.process.parallel())
			return;
		std::vector<uint64_t> views;
		{
			std::lock_guard lock(closed->mutex);
			views.swap(closed->views);
		}
		for (const uint64_t view : views)
			context.memory.free(view, 0, 0x8000);
	};
	context.threads.add_service(unmap_closed);

	// HANDLE RavenAudioOpen(const WAVEFORMATEX *format, DWORD flags, DWORD buffer_frames, DWORD period_frames, HANDLE event,
	// RavenAudioShared **shared): a stopped stream, its page mapped at *shared. Zero frames ask for the defaults (a hundredth of a second
	// of period; a tenth of a second of ring, or one period if that is longer); the ring is at most ten seconds and holds a period. The
	// event, if any, is set once a period while the stream runs. Zero on failure.
	registry.add("RavenAudioOpen", 6, [&context, &objects, wake, closed, unmap_closed](ApiCall &call) {
		unmap_closed();
		const uint32_t flags = call.u32(1), buffer = call.u32(2), period = call.u32(3);
		const uint64_t event_handle = call[4], out = call[5];
		const auto fail = [&call](uint32_t error) {
			call.set_last_error(error);
			call.ret(0);
		};
		if (!call[0] || !out || (flags & ~RAVEN_AUDIO_CAPTURE)) {
			fail(87);
			return;
		}
		const auto format = read_wave_format(call, call[0]);
		if (!format) {
			fail(87);
			return;
		}
		const uint64_t period_frames = period ? period : std::max<uint32_t>(format->rate / 100, 1);
		const uint64_t buffer_frames = buffer ? buffer : std::max<uint64_t>(format->rate / 10, period_frames);
		if (period_frames > buffer_frames || buffer_frames > uint64_t(format->rate) * 10) {
			fail(87);
			return;
		}
		auto target = std::make_shared<EventTarget>();
		if (event_handle) {
			target->event = objects.object_as<EventObject>(event_handle);
			if (!target->event) {
				fail(6);
				return;
			}
		}
		AudioStream::Options options;
		options.format = *format;
		options.capture = (flags & RAVEN_AUDIO_CAPTURE) != 0;
		options.buffer_frames = uint32_t(buffer_frames);
		options.period_frames = uint32_t(period_frames);
		options.notify = [target, wake] {
			target->set();
			wake();
		};
		auto audio = std::make_shared<GuestAudio>(closed);
		audio->target = target;
		audio->stream = AudioStream::open(std::move(options));
		if (!audio->stream) {
			fail(8);
			return;
		}
		const auto view = context.memory.map_view(audio->stream->descriptor(), 0, audio->stream->mapping_size(), 0x04);
		if (view.status || !view.address) {
			fail(8);
			return;
		}
		audio->view = view.address;
		const auto made = objects.add_object(audio);
		if (made.status) {
			audio.reset();
			unmap_closed();
			fail(8);
			return;
		}
		call.store_pointer(out, view.address);
		call.ret(made.value);
	});
	// BOOL RavenAudioSetEvent(HANDLE stream, HANDLE event): the event set once a period from now on, in place of the one given at open;
	// NULL for none. WASAPI programs name their event only after the stream is initialized.
	registry.add("RavenAudioSetEvent", 2, [&objects, stream_of](ApiCall &call) {
		const auto audio = stream_of(call[0]);
		std::shared_ptr<EventObject> event;
		if (call[1])
			event = objects.object_as<EventObject>(call[1]);
		if (!audio || (call[1] && !event)) {
			call.set_last_error(6);
			call.ret(0);
			return;
		}
		std::lock_guard lock(audio->target->mutex);
		audio->target->event = std::move(event);
		call.ret(1);
	});
	registry.add("RavenAudioStart", 1, [stream_of](ApiCall &call) {
		const auto audio = stream_of(call[0]);
		if (audio)
			audio->stream->start();
		else
			call.set_last_error(6);
		call.ret(audio != nullptr);
	});
	registry.add("RavenAudioStop", 1, [stream_of](ApiCall &call) {
		const auto audio = stream_of(call[0]);
		if (audio)
			audio->stream->stop();
		else
			call.set_last_error(6);
		call.ret(audio != nullptr);
	});
	// Stops the stream and empties it; its counters and position are zero afterwards.
	registry.add("RavenAudioReset", 1, [stream_of](ApiCall &call) {
		const auto audio = stream_of(call[0]);
		if (audio)
			audio->stream->reset();
		else
			call.set_last_error(6);
		call.ret(audio != nullptr);
	});
	// Ends the stream and closes the handle; the page leaves the guest with the last handle to the stream.
	registry.add("RavenAudioClose", 1, [&objects, stream_of, unmap_closed](ApiCall &call) {
		bool closed_one = false;
		if (const auto audio = stream_of(call[0])) {
			audio->stream->stop();
			objects.close(call[0]);
			closed_one = true;
		}
		// After the reference above is gone, so the view of the stream just closed, if this was its last handle, goes now.
		unmap_closed();
		if (!closed_one)
			call.set_last_error(6);
		call.ret(closed_one);
	});
}

}
