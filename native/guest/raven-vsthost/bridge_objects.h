// The objects a VST 3 host hands a plugin, which both halves of the bridge make: a stream over memory for states, the messages a plugin's
// objects send each other with their attributes, and the parameter changes and events of an audio block. Each side builds them from what
// crosses the bridge and gives them to the code it calls, the plugin on Windows or the host on Linux.

#pragma once

#include "bridge.h"

#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstattributes.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <variant>

namespace raven_vst {

inline bool same(const TUID first, const TUID second) { return FUnknownPrivate::iidEqual(first, second); }

// Reference counting for an object of one interface, which answers for FUnknown and the interfaces it derives from.
template <typename Self, typename... Bases>
class Counted : public Self {
public:
	tresult PLUGIN_API queryInterface(const TUID iid, void **object) override
	{
		if (same(iid, FUnknown::iid) || same(iid, Self::iid) || (same(iid, Bases::iid) || ...)) {
			addRef();
			*object = static_cast<Self *>(this);
			return kResultOk;
		}
		*object = nullptr;
		return kNoInterface;
	}
	uint32 PLUGIN_API addRef() override { return ++references; }
	uint32 PLUGIN_API release() override
	{
		const uint32 left = --references;
		if (!left)
			delete this;
		return left;
	}

protected:
	virtual ~Counted() = default;

private:
	std::atomic<uint32> references{1};
};

// A plugin's state, read and written as a stream.
class MemoryStream : public Counted<IBStream> {
public:
	MemoryStream() = default;
	explicit MemoryStream(std::vector<uint8_t> bytes) : data(std::move(bytes)) {}
	tresult PLUGIN_API queryInterface(const TUID iid, void **object) override
	{
		if (same(iid, ISizeableStream::iid)) {
			addRef();
			*object = &sizeable;
			return kResultOk;
		}
		return Counted::queryInterface(iid, object);
	}
	tresult PLUGIN_API read(void *buffer, int32 count, int32 *read) override
	{
		const int64 left = std::max<int64>(0, int64(data.size()) - at);
		const int32 taken = count < 0 ? 0 : int32(std::min<int64>(count, left));
		if (taken)
			std::memcpy(buffer, data.data() + at, size_t(taken));
		at += taken;
		if (read)
			*read = taken;
		return kResultOk;
	}
	tresult PLUGIN_API write(void *buffer, int32 count, int32 *written) override
	{
		if (count < 0)
			return kInvalidArgument;
		if (size_t(at) + size_t(count) > data.size())
			data.resize(size_t(at) + size_t(count));
		std::memcpy(data.data() + at, buffer, size_t(count));
		at += count;
		if (written)
			*written = count;
		return kResultOk;
	}
	tresult PLUGIN_API seek(int64 position, int32 mode, int64 *result) override
	{
		const int64 base = mode == kIBSeekSet ? 0 : mode == kIBSeekCur ? at : mode == kIBSeekEnd ? int64(data.size()) : -1;
		if (base < 0 || base + position < 0)
			return kInvalidArgument;
		at = base + position;
		if (result)
			*result = at;
		return kResultOk;
	}
	tresult PLUGIN_API tell(int64 *position) override
	{
		if (!position)
			return kInvalidArgument;
		*position = at;
		return kResultOk;
	}
	std::vector<uint8_t> data;

private:
	class Sizeable : public ISizeableStream {
	public:
		explicit Sizeable(MemoryStream &stream) : stream(stream) {}
		tresult PLUGIN_API queryInterface(const TUID iid, void **object) override { return stream.queryInterface(iid, object); }
		uint32 PLUGIN_API addRef() override { return stream.addRef(); }
		uint32 PLUGIN_API release() override { return stream.release(); }
		tresult PLUGIN_API getStreamSize(int64 &size) override
		{
			size = int64(stream.data.size());
			return kResultOk;
		}
		tresult PLUGIN_API setStreamSize(int64 size) override
		{
			if (size < 0)
				return kInvalidArgument;
			stream.data.resize(size_t(size));
			return kResultOk;
		}

	private:
		MemoryStream &stream;
	} sizeable{*this};
	int64 at = 0;
};

// Reads a whole stream the host or the plugin hands over, from where it stands.
inline bool read_all(IBStream *stream, std::vector<uint8_t> &bytes)
{
	if (!stream)
		return false;
	uint8_t chunk[65536];
	for (;;) {
		int32 count = 0;
		if (stream->read(chunk, sizeof chunk, &count) != kResultOk || count <= 0)
			return true;
		bytes.insert(bytes.end(), chunk, chunk + count);
	}
}

class AttributeList : public Counted<IAttributeList> {
public:
	tresult PLUGIN_API setInt(AttrID id, int64 value) override { return set(id, value); }
	tresult PLUGIN_API getInt(AttrID id, int64 &value) override { return get(id, value); }
	tresult PLUGIN_API setFloat(AttrID id, double value) override { return set(id, value); }
	tresult PLUGIN_API getFloat(AttrID id, double &value) override { return get(id, value); }
	tresult PLUGIN_API setString(AttrID id, const TChar *text) override
	{
		if (!id || !text)
			return kInvalidArgument;
		std::u16string value;
		for (size_t at = 0; text[at]; ++at)
			value.push_back(char16_t(text[at]));
		return set(id, std::move(value));
	}
	tresult PLUGIN_API getString(AttrID id, TChar *text, uint32 size) override
	{
		const auto found = id ? values.find(id) : values.end();
		if (found == values.end() || !std::holds_alternative<std::u16string>(found->second) || !text || size < sizeof(TChar))
			return kResultFalse;
		const auto &value = std::get<std::u16string>(found->second);
		const size_t count = std::min<size_t>(value.size(), size / sizeof(TChar) - 1);
		std::copy_n(value.begin(), count, text);
		text[count] = 0;
		return kResultOk;
	}
	tresult PLUGIN_API setBinary(AttrID id, const void *data, uint32 size) override
	{
		if (!id || (!data && size))
			return kInvalidArgument;
		const auto *bytes = static_cast<const uint8_t *>(data);
		return set(id, std::vector<uint8_t>(bytes, bytes + size));
	}
	tresult PLUGIN_API getBinary(AttrID id, const void *&data, uint32 &size) override
	{
		const auto found = id ? values.find(id) : values.end();
		if (found == values.end() || !std::holds_alternative<std::vector<uint8_t>>(found->second))
			return kResultFalse;
		const auto &value = std::get<std::vector<uint8_t>>(found->second);
		data = value.data();
		size = uint32(value.size());
		return kResultOk;
	}
	void write(Writer &to) const
	{
		to.u32(uint32_t(values.size()));
		for (const auto &[id, value] : values) {
			to.bytes(id.data(), id.size()).u32(uint32_t(value.index()));
			if (const auto *number = std::get_if<int64>(&value))
				to.u64(uint64_t(*number));
			else if (const auto *real = std::get_if<double>(&value))
				to.f64(*real);
			else if (const auto *text = std::get_if<std::u16string>(&value))
				to.bytes(text->data(), text->size() * sizeof(char16_t));
			else
				to.bytes(std::get<std::vector<uint8_t>>(value).data(), std::get<std::vector<uint8_t>>(value).size());
		}
	}
	void read(Reader &from)
	{
		for (uint32_t count = from.u32(); count; --count) {
			const auto name = from.bytes();
			const std::string id(name.begin(), name.end());
			switch (from.u32()) {
			case 0: values[id] = int64(from.u64()); break;
			case 1: values[id] = from.f64(); break;
			case 2: {
				const auto units = from.bytes();
				values[id] = std::u16string(reinterpret_cast<const char16_t *>(units.data()), units.size() / sizeof(char16_t));
				break;
			}
			case 3: values[id] = from.bytes(); break;
			default: throw std::runtime_error("an attribute of an unknown kind");
			}
		}
	}

private:
	using Value = std::variant<int64, double, std::u16string, std::vector<uint8_t>>;
	tresult set(AttrID id, Value value)
	{
		if (!id)
			return kInvalidArgument;
		values[id] = std::move(value);
		return kResultOk;
	}
	template <typename Plain>
	tresult get(AttrID id, Plain &value)
	{
		const auto found = id ? values.find(id) : values.end();
		if (found == values.end() || !std::holds_alternative<Plain>(found->second))
			return kResultFalse;
		value = std::get<Plain>(found->second);
		return kResultOk;
	}
	std::map<std::string, Value> values;
};

class Message : public Counted<IMessage> {
public:
	Message() : attributes(new AttributeList) {}
	FIDString PLUGIN_API getMessageID() override { return id.c_str(); }
	void PLUGIN_API setMessageID(FIDString given) override { id = given ? given : ""; }
	IAttributeList *PLUGIN_API getAttributes() override { return attributes; }
	// A message of the other side's, or one it is sent: its id and attributes. One a plugin made itself is read through its interfaces,
	// whose attributes cannot be listed, so only those of the bridge's own lists cross.
	static void write(Writer &to, IMessage &message)
	{
		const char *name = message.getMessageID();
		to.bytes(name, name ? std::strlen(name) : 0);
		if (auto *own = dynamic_cast<AttributeList *>(message.getAttributes()))
			own->write(to);
		else
			to.u32(0);
	}
	void read(Reader &from)
	{
		const auto name = from.bytes();
		id.assign(name.begin(), name.end());
		attributes->read(from);
	}

protected:
	~Message() override { attributes->release(); }

private:
	std::string id;
	AttributeList *attributes;
};

class ValueQueue : public Counted<IParamValueQueue> {
public:
	explicit ValueQueue(ParamID id) : id(id) { points.reserve(64); }
	ParamID PLUGIN_API getParameterId() override { return id; }
	int32 PLUGIN_API getPointCount() override { return int32(points.size()); }
	tresult PLUGIN_API getPoint(int32 index, int32 &offset, ParamValue &value) override
	{
		if (index < 0 || size_t(index) >= points.size())
			return kInvalidArgument;
		offset = points[size_t(index)].first;
		value = points[size_t(index)].second;
		return kResultOk;
	}
	tresult PLUGIN_API addPoint(int32 offset, ParamValue value, int32 &index) override
	{
		// Points stay in the order of their offsets; one at an offset that has a point already replaces it.
		auto at = std::lower_bound(points.begin(), points.end(), offset, [](const auto &point, int32 wanted) { return point.first < wanted; });
		if (at != points.end() && at->first == offset)
			at->second = value;
		else
			at = points.insert(at, {offset, value});
		index = int32(at - points.begin());
		return kResultOk;
	}
	ParamID id;
	std::vector<std::pair<int32, ParamValue>> points;
};

// The changes of a block, one queue for each parameter; queues stay between blocks, so a block reuses what the one before made.
class ParameterChanges : public Counted<IParameterChanges> {
public:
	int32 PLUGIN_API getParameterCount() override { return int32(used); }
	IParamValueQueue *PLUGIN_API getParameterData(int32 index) override
	{
		return index >= 0 && size_t(index) < used ? queues[size_t(index)] : nullptr;
	}
	IParamValueQueue *PLUGIN_API addParameterData(const ParamID &id, int32 &index) override
	{
		for (size_t at = 0; at < used; ++at)
			if (queues[at]->id == id) {
				index = int32(at);
				return queues[at];
			}
		if (used == queues.size())
			queues.push_back(new ValueQueue(id));
		queues[used]->id = id;
		queues[used]->points.clear();
		index = int32(used);
		return queues[used++];
	}
	void clear() { used = 0; }
	// From the points of a block, in the order they came.
	void take(const Point *points, uint32_t count)
	{
		clear();
		for (uint32_t at = 0; at < count; ++at) {
			int32 index = 0;
			addParameterData(points[at].id, index)->addPoint(points[at].offset, points[at].value, index);
		}
	}
	// Into the points of a block; false when they do not fit.
	bool put(Point *points, uint32_t capacity, uint32_t &count) const
	{
		count = 0;
		for (size_t at = 0; at < used; ++at)
			for (const auto &[offset, value] : queues[at]->points) {
				if (count == capacity)
					return false;
				points[count++] = Point{queues[at]->id, offset, value};
			}
		return true;
	}
	// From the host's or the plugin's own object, which only its interface reads.
	static bool put(IParameterChanges &changes, Point *points, uint32_t capacity, uint32_t &count)
	{
		count = 0;
		for (int32 index = 0; index < changes.getParameterCount(); ++index) {
			IParamValueQueue *const queue = changes.getParameterData(index);
			for (int32 point = 0; queue && point < queue->getPointCount(); ++point) {
				int32 offset = 0;
				ParamValue value = 0;
				if (queue->getPoint(point, offset, value) != kResultOk)
					continue;
				if (count == capacity)
					return false;
				points[count++] = Point{queue->getParameterId(), offset, value};
			}
		}
		return true;
	}

protected:
	~ParameterChanges() override
	{
		for (auto *queue : queues)
			queue->release();
	}

private:
	std::vector<ValueQueue *> queues;
	size_t used = 0;
};

class EventList : public Counted<IEventList> {
public:
	EventList() { events.reserve(Block::events); }
	int32 PLUGIN_API getEventCount() override { return int32(events.size()); }
	tresult PLUGIN_API getEvent(int32 index, Event &event) override
	{
		if (index < 0 || size_t(index) >= events.size())
			return kInvalidArgument;
		event = events[size_t(index)];
		return kResultOk;
	}
	tresult PLUGIN_API addEvent(Event &event) override
	{
		if (events.size() == Block::events)
			return kOutOfMemory;
		events.push_back(event);
		return kResultOk;
	}
	std::vector<Event> events;
};

// The events of a block into its record and back: a data event's bytes travel in the block's own room, the others as they are. Those of a
// kind with a pointer the record cannot carry (a note expression's text, a chord's or a scale's name) lose the text.
inline bool put_events(IEventList &list, Event *events, uint32_t capacity, uint32_t &count, uint8_t *room, uint32_t &used)
{
	count = 0;
	used = 0;
	for (int32 index = 0; index < list.getEventCount(); ++index) {
		Event event{};
		if (list.getEvent(index, event) != kResultOk)
			continue;
		if (count == capacity)
			return false;
		if (event.type == Event::kDataEvent) {
			if (event.data.size > Block::event_bytes - used || (!event.data.bytes && event.data.size))
				return false;
			std::memcpy(room + used, event.data.bytes, event.data.size);
			event.data.bytes = reinterpret_cast<const uint8 *>(uintptr_t(used));
			used += event.data.size;
		} else if (event.type == Event::kNoteExpressionTextEvent) {
			event.noteExpressionText.text = nullptr;
			event.noteExpressionText.textLen = 0;
		} else if (event.type == Event::kChordEvent) {
			event.chord.text = nullptr;
			event.chord.textLen = 0;
		} else if (event.type == Event::kScaleEvent) {
			event.scale.text = nullptr;
			event.scale.textLen = 0;
		}
		events[count++] = event;
	}
	return true;
}

inline void take_events(EventList &list, const Event *events, uint32_t count, const uint8_t *room, uint32_t used)
{
	list.events.clear();
	for (uint32_t index = 0; index < count && index < Block::events; ++index) {
		Event event = events[index];
		if (event.type == Event::kDataEvent) {
			const auto offset = uintptr_t(event.data.bytes);
			if (offset > used || event.data.size > used - offset)
				continue;
			event.data.bytes = room + offset;
		}
		list.events.push_back(event);
	}
}

}
