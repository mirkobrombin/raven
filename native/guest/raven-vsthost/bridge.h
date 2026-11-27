// The wire between the two halves of a bridged VST 3 plugin: the Linux plugin a host loads, and raven-vsthost.exe, which runs the Windows
// plugin under Raven. Both are built from this header, with their own compilers, on the same x86-64 layout.
//
// Calls go from Linux on the program's standard input; each is answered on its standard output, in order. Between two answers the program
// may also send what the plugin told its host meanwhile (Notice), which the Linux side hands on once the call it waits for is answered. A
// frame is the length of what follows it, a tag (a Call, a Notice or Answer) and the values Writer put down.
//
// Audio does not travel as messages. Each processor has a block of the memory the two sides share (the runtime's link, RavenLinkMap): the
// Linux side writes a block's input there, counts it in `request` and wakes the word; the program processes it, writes the output, copies
// the count to `done` and wakes that word.

#pragma once

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace raven_vst {

using namespace Steinberg;
using namespace Steinberg::Vst;

constexpr uint32_t protocol = 1;

// The structures that cross as they are have one layout on both sides.
static_assert(sizeof(ProcessContext) == 112 && sizeof(ProcessSetup) == 24 && sizeof(ParameterInfo) == 792 && sizeof(BusInfo) == 276 &&
	sizeof(RoutingInfo) == 12 && sizeof(Event) == 48 && sizeof(PClassInfo2) == 440 && sizeof(PClassInfoW) == 696 && sizeof(PFactoryInfo) == 452,
	"a structure that crosses the bridge has another layout here");

enum class Call : uint32_t {
	// -> protocol, PFactoryInfo, the number of classes, then for each class its id and PClassInfo2 (PClassInfo's fields for a factory
	// without IPluginFactory2)
	hello = 1,
	// class id, the interface the host asked for (Interfaces) -> object (0 when the class makes none), interfaces it has
	create,
	// object
	release,
	// object, object: the plugin's own two objects are connected as the host connected their halves; with no second object the host
	// connected the first to a connection point of its own, which the plugin's messages then go to (Notice::message)
	connect,
	disconnect,
	// object, Message: a message the host's connection point sent to the object
	notify,
	// object, the host's name (UTF-16) -> result
	initialize,
	terminate,
	// IComponent
	controller_class, // object -> result, class id
	set_io_mode, // object, mode -> result
	bus_count, // object, type, direction -> count
	bus_info, // object, type, direction, index -> result, BusInfo
	routing_info, // object, RoutingInfo -> result, RoutingInfo
	activate_bus, // object, type, direction, index, state -> result
	set_active, // object, state -> result
	set_state, // object, bytes -> result
	get_state, // object -> result, bytes
	// IAudioProcessor
	set_arrangements, // object, inputs, outputs -> result
	get_arrangement, // object, direction, index -> result, arrangement
	can_process_sample_size, // object, size -> result
	latency_samples, // object -> samples
	setup_processing, // object, ProcessSetup, offset and size of its block in the link -> result
	set_processing, // object, state -> result
	tail_samples, // object -> samples
	// IEditController
	set_component_state, // object, bytes -> result
	set_controller_state, // object, bytes -> result
	get_controller_state, // object -> result, bytes
	parameter_count, // object -> count
	parameter_info, // object, index -> result, ParameterInfo
	string_by_value, // object, id, value -> result, text
	value_by_string, // object, id, text -> result, value
	to_plain, // object, id, value -> value
	to_normalized, // object, id, value -> value
	get_normalized, // object, id -> value
	set_normalized, // object, id, value -> result
	set_handler, // object, whether the host gave one -> result
};

// What the plugin told the host between two answers.
enum class Notice : uint32_t {
	begin_edit = 0x100, // object, id
	perform_edit, // object, id, value
	end_edit, // object, id
	restart, // object, flags
	message, // from object, Message: what a plugin object sent the peer it is connected to
};

constexpr uint32_t answer = 0;

// The interfaces an object has, for the Linux half to answer queryInterface as the plugin would.
enum Interfaces : uint32_t {
	has_component = 1,
	has_processor = 2,
	has_controller = 4,
	has_connection = 8,
};

// The results of calls, as each side's tresult differs (COM's values on Windows).
enum class Result : int32_t { ok, untrue, no_interface, invalid_argument, not_implemented, internal_error, not_initialized, out_of_memory };

inline Result to_wire(tresult result)
{
	switch (result) {
	case kResultOk: return Result::ok;
	case kResultFalse: return Result::untrue;
	case kNoInterface: return Result::no_interface;
	case kInvalidArgument: return Result::invalid_argument;
	case kNotImplemented: return Result::not_implemented;
	case kNotInitialized: return Result::not_initialized;
	case kOutOfMemory: return Result::out_of_memory;
	default: return Result::internal_error;
	}
}

inline tresult from_wire(Result result)
{
	switch (result) {
	case Result::ok: return kResultOk;
	case Result::untrue: return kResultFalse;
	case Result::no_interface: return kNoInterface;
	case Result::invalid_argument: return kInvalidArgument;
	case Result::not_implemented: return kNotImplemented;
	case Result::not_initialized: return kNotInitialized;
	case Result::out_of_memory: return kOutOfMemory;
	default: return kInternalError;
	}
}

// A class id as the four words of its FUID, which mean the same on both sides where the bytes of a TUID do not (COM's order on Windows).
struct ClassId {
	uint32_t words[4]{};
	static ClassId of(const TUID id)
	{
		ClassId made;
		FUID::fromTUID(id).to4Int(made.words[0], made.words[1], made.words[2], made.words[3]);
		return made;
	}
	void to(TUID id) const
	{
		FUID made;
		made.from4Int(words[0], words[1], words[2], words[3]);
		made.toTUID(id);
	}
	bool operator==(const ClassId &other) const { return std::memcmp(words, other.words, sizeof words) == 0; }
};

class Writer {
public:
	explicit Writer(uint32_t tag) { u32(0); u32(tag); }
	Writer &u32(uint32_t value) { return raw(&value, sizeof value); }
	Writer &i32(int32_t value) { return raw(&value, sizeof value); }
	Writer &u64(uint64_t value) { return raw(&value, sizeof value); }
	Writer &f64(double value) { return raw(&value, sizeof value); }
	Writer &result(tresult value) { return i32(int32_t(to_wire(value))); }
	Writer &id(const ClassId &value) { return raw(value.words, sizeof value.words); }
	Writer &bytes(const void *data, size_t size)
	{
		u32(uint32_t(size));
		return raw(data, size);
	}
	Writer &text(const char16 *value)
	{
		size_t length = 0;
		while (value && value[length])
			++length;
		return bytes(value, length * sizeof(char16));
	}
	template <typename Plain>
	Writer &record(const Plain &value) { return raw(&value, sizeof value); }
	// The frame, with its length in front.
	const std::vector<uint8_t> &frame()
	{
		const uint32_t length = uint32_t(data.size() - sizeof(uint32_t));
		std::memcpy(data.data(), &length, sizeof length);
		return data;
	}

private:
	Writer &raw(const void *value, size_t size)
	{
		const auto *bytes = static_cast<const uint8_t *>(value);
		data.insert(data.end(), bytes, bytes + size);
		return *this;
	}
	std::vector<uint8_t> data;
};

// Reads what a Writer put down, in the same order; a frame that ends early or names more than it holds is refused.
class Reader {
public:
	explicit Reader(std::vector<uint8_t> frame) : data(std::move(frame)) { tag = u32(); }
	uint32_t tag = 0;
	uint32_t u32() { return plain<uint32_t>(); }
	int32_t i32() { return plain<int32_t>(); }
	uint64_t u64() { return plain<uint64_t>(); }
	double f64() { return plain<double>(); }
	tresult result() { return from_wire(Result(i32())); }
	ClassId id()
	{
		ClassId made;
		take(made.words, sizeof made.words);
		return made;
	}
	std::vector<uint8_t> bytes()
	{
		const uint32_t size = u32();
		if (size > data.size() - at)
			throw std::runtime_error("a frame names more than it holds");
		std::vector<uint8_t> made(data.begin() + at, data.begin() + at + size);
		at += size;
		return made;
	}
	// A UTF-16 text into a buffer of `capacity` units, cut to fit and ended with a zero.
	void text(char16 *target, size_t capacity)
	{
		const auto units = bytes();
		const size_t count = std::min(units.size() / sizeof(char16), capacity - 1);
		std::memcpy(target, units.data(), count * sizeof(char16));
		target[count] = 0;
	}
	template <typename Plain>
	Plain record() { return plain<Plain>(); }

private:
	template <typename Plain>
	Plain plain()
	{
		Plain value;
		take(&value, sizeof value);
		return value;
	}
	void take(void *target, size_t size)
	{
		if (size > data.size() - at)
			throw std::runtime_error("a frame ends early");
		std::memcpy(target, data.data() + at, size);
		at += size;
	}
	std::vector<uint8_t> data;
	size_t at = 0;
};

// A point of a parameter's change within a block.
struct Point {
	uint32_t id;
	int32_t offset;
	double value;
};

// A processor's block in the link. The audio follows the record: for each input channel and then each output channel, room for the most
// samples a block may have, of the sample size set up.
struct Block {
	static constexpr uint32_t buses = 16, points = 2048, events = 1024, event_bytes = 65536;
	std::atomic<uint32_t> request;
	std::atomic<uint32_t> done;
	int32_t result;
	int32_t mode, sample_size, samples, has_context;
	ProcessContext context;
	uint32_t input_buses, output_buses;
	int32_t input_channels[buses], output_channels[buses];
	uint64_t input_silence[buses], output_silence[buses];
	uint32_t input_points, output_points;
	Point input_changes[points], output_changes[points];
	uint32_t input_events, output_events;
	// A data event's bytes are in `event_data`, its `bytes` an offset there.
	Event input_event[events], output_event[events];
	uint32_t input_event_bytes, output_event_bytes;
	uint8_t input_event_data[event_bytes], output_event_data[event_bytes];
};
static_assert(std::atomic<uint32_t>::is_always_lock_free);

// The room a block needs: the record, then the channels of every bus each way, at the largest block the processor was set up for.
inline uint64_t block_size(const ProcessSetup &setup, uint32_t channels)
{
	const uint64_t sample = setup.symbolicSampleSize == kSample64 ? 8 : 4;
	return (sizeof(Block) + 63) / 64 * 64 + uint64_t(channels) * uint64_t(setup.maxSamplesPerBlock) * sample;
}

// The audio of a block, after its record.
inline uint8_t *block_audio(Block *block) { return reinterpret_cast<uint8_t *>(block) + (sizeof(Block) + 63) / 64 * 64; }

}
