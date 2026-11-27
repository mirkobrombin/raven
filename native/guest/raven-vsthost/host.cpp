// raven-vsthost.exe: the Windows side of a VST 3 plugin that a Linux host loads through Raven. It loads the plugin's module, a single file
// or the DLL inside a bundle (NAME.vst3/Contents/x86_64-win/NAME.vst3), as a Windows host does, and calls the plugin's own code natively.
//
//   raven-vsthost.exe --list PLUGIN    one line for each class of the plugin's factory: its id, as the four words of a FUID in hex, its
//                                      category, its subcategories (empty without IPluginFactory2), and its name.
//   raven-vsthost.exe --serve PLUGIN   answers the calls of the Linux half on standard input and output (bridge.h), until the input ends.
//
// The exit status is 0 on success, 1 for a module that does not load or has no factory, 2 for a wrong command line.

#include "bridge_objects.h"

#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#include "pluginterfaces/vst/ivsthostapplication.h"

#include <windows.h>

#include <cstdio>
#include <deque>
#include <mutex>
#include <string>

using namespace raven_vst;

extern "C" {
__declspec(dllimport) void *WINAPI RavenLinkMap(SIZE_T *size);
__declspec(dllimport) DWORD WINAPI RavenLinkWait(const volatile LONG *word, LONG seen, DWORD milliseconds);
__declspec(dllimport) BOOL WINAPI RavenLinkWake(volatile LONG *word);
__declspec(dllimport) BOOL WINAPI RavenEmbedWindow(HWND window, UINT64 host);
}

namespace {

// The module a path names, in full, as LoadLibraryEx searches the module's folder for its own imports only for a full path: the path itself
// for a file, the DLL of its own name inside it for a bundle folder.
std::wstring module_path(const std::wstring &given)
{
	std::wstring path(MAX_PATH, L'\0');
	for (;;) {
		const DWORD length = GetFullPathNameW(given.c_str(), DWORD(path.size()), path.data(), nullptr);
		if (!length)
			return given;
		if (length < path.size()) {
			path.resize(length);
			break;
		}
		path.resize(length);
	}
	const DWORD attributes = GetFileAttributesW(path.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
		return path;
	std::wstring folder = path;
	while (!folder.empty() && (folder.back() == L'\\' || folder.back() == L'/'))
		folder.pop_back();
	const size_t slash = folder.find_last_of(L"\\/");
	return folder + L"\\Contents\\x86_64-win\\" + folder.substr(slash == std::wstring::npos ? 0 : slash + 1);
}

class Module {
public:
	explicit Module(const std::wstring &path) : handle(LoadLibraryExW(module_path(path).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH))
	{
		if (!handle)
			return;
		// A plugin may set itself up in InitDll and refuse to load from there; GetPluginFactory is all a module must have.
		using Init = bool (*)();
		if (const auto init = reinterpret_cast<Init>(GetProcAddress(handle, "InitDll")); init && !init())
			return;
		initialized = true;
		using Get = IPluginFactory *(PLUGIN_API *)();
		if (const auto get = reinterpret_cast<Get>(GetProcAddress(handle, "GetPluginFactory")))
			factory = get();
	}
	~Module()
	{
		if (factory)
			factory->release();
		using Exit = bool (*)();
		if (initialized)
			if (const auto exit = reinterpret_cast<Exit>(GetProcAddress(handle, "ExitDll")))
				exit();
		if (handle)
			FreeLibrary(handle);
	}
	Module(const Module &) = delete;
	Module &operator=(const Module &) = delete;

	HMODULE handle;
	bool initialized = false;
	IPluginFactory *factory = nullptr;
};

template <typename Interface>
Interface *query(FUnknown *object)
{
	void *found = nullptr;
	if (!object || object->queryInterface(Interface::iid, &found) != kResultOk)
		return nullptr;
	return static_cast<Interface *>(found);
}

int list(IPluginFactory &factory)
{
	IPluginFactory2 *const second = query<IPluginFactory2>(&factory);
	const int32 count = factory.countClasses();
	for (int32 index = 0; index < count; ++index) {
		PClassInfo info{};
		if (factory.getClassInfo(index, &info) != kResultOk)
			continue;
		PClassInfo2 more{};
		const bool described = second && second->getClassInfo2(index, &more) == kResultOk;
		const ClassId id = ClassId::of(info.cid);
		std::printf("%08X%08X%08X%08X\t%.*s\t%.*s\t%.*s\n", id.words[0], id.words[1], id.words[2], id.words[3], int(sizeof info.category),
			info.category, described ? int(sizeof more.subCategories) : 0, more.subCategories, int(sizeof info.name), info.name);
	}
	if (second)
		second->release();
	std::fflush(stdout);
	return 0;
}

// Standard output, which answers and notices share; any thread may send.
class Output {
public:
	void send(Writer &message)
	{
		const auto &frame = message.frame();
		const std::lock_guard guard(lock);
		const HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
		for (size_t at = 0; at < frame.size();) {
			DWORD written = 0;
			if (!WriteFile(handle, frame.data() + at, DWORD(frame.size() - at), &written, nullptr) || !written)
				ExitProcess(3);
			at += written;
		}
	}

private:
	std::mutex lock;
} output;

// The frames standard input brings, read by a thread of their own so the main thread can wait for them and for its windows' messages.
class Input {
public:
	Input() : ready(CreateEventW(nullptr, FALSE, FALSE, nullptr))
	{
		CloseHandle(CreateThread(nullptr, 0, [](void *self) -> DWORD {
			static_cast<Input *>(self)->read();
			return 0;
		}, this, 0, nullptr));
	}
	// The next frame, if one came; `ended` once the input is over and every frame was taken.
	bool next(std::vector<uint8_t> &frame, bool &ended)
	{
		const std::lock_guard guard(lock);
		ended = frames.empty() && over;
		if (frames.empty())
			return false;
		frame = std::move(frames.front());
		frames.pop_front();
		return true;
	}
	HANDLE ready;

private:
	static bool read_exactly(void *target, size_t size)
	{
		const HANDLE handle = GetStdHandle(STD_INPUT_HANDLE);
		for (size_t at = 0; at < size;) {
			DWORD count = 0;
			if (!ReadFile(handle, static_cast<uint8_t *>(target) + at, DWORD(size - at), &count, nullptr) || !count)
				return false;
			at += count;
		}
		return true;
	}
	void read()
	{
		for (;;) {
			uint32_t length = 0;
			std::vector<uint8_t> frame;
			const bool whole = read_exactly(&length, sizeof length) && length >= sizeof(uint32_t) && length <= (1U << 28) &&
				(frame.resize(length), read_exactly(frame.data(), length));
			{
				const std::lock_guard guard(lock);
				if (whole)
					frames.push_back(std::move(frame));
				else
					over = true;
			}
			SetEvent(ready);
			if (!whole)
				return;
		}
	}
	std::mutex lock;
	std::deque<std::vector<uint8_t>> frames;
	bool over = false;
};

// The host a plugin sees: the Linux host's name, and the messages and attribute lists plugins ask it to make.
class HostApplication : public Counted<IHostApplication> {
public:
	tresult PLUGIN_API getName(String128 given) override
	{
		size_t at = 0;
		for (; at + 1 < 128 && at < name.size(); ++at)
			given[at] = name[at];
		given[at] = 0;
		return kResultOk;
	}
	tresult PLUGIN_API createInstance(TUID cid, TUID iid, void **object) override
	{
		FUnknown *made = nullptr;
		if (same(cid, IMessage::iid))
			made = new Message;
		else if (same(cid, IAttributeList::iid))
			made = new AttributeList;
		if (!made) {
			*object = nullptr;
			return kResultFalse;
		}
		const tresult result = made->queryInterface(iid, object);
		made->release();
		return result;
	}
	std::u16string name = u"Raven";
} *host = new HostApplication;

// What a plugin's edit controller tells the host, sent to the Linux side as it comes.
class Handler : public Counted<IComponentHandler> {
public:
	explicit Handler(uint32_t object) : object(object) {}
	tresult PLUGIN_API beginEdit(ParamID id) override { return tell(Writer(uint32_t(Notice::begin_edit)).u32(object).u32(id)); }
	tresult PLUGIN_API performEdit(ParamID id, ParamValue value) override
	{
		return tell(Writer(uint32_t(Notice::perform_edit)).u32(object).u32(id).f64(value));
	}
	tresult PLUGIN_API endEdit(ParamID id) override { return tell(Writer(uint32_t(Notice::end_edit)).u32(object).u32(id)); }
	tresult PLUGIN_API restartComponent(int32 flags) override { return tell(Writer(uint32_t(Notice::restart)).u32(object).i32(flags)); }

private:
	static tresult tell(Writer &notice)
	{
		output.send(notice);
		return kResultOk;
	}
	uint32_t object;
};

// The connection point of the Linux host's that a plugin object was connected to: what the plugin sends it goes to the Linux side.
class Peer : public Counted<IConnectionPoint> {
public:
	explicit Peer(uint32_t object) : object(object) {}
	tresult PLUGIN_API connect(IConnectionPoint *) override { return kResultOk; }
	tresult PLUGIN_API disconnect(IConnectionPoint *) override { return kResultOk; }
	tresult PLUGIN_API notify(IMessage *message) override
	{
		if (!message)
			return kInvalidArgument;
		Writer notice(uint32_t(Notice::message));
		notice.u32(object);
		Message::write(notice, *message);
		output.send(notice);
		return kResultOk;
	}

private:
	uint32_t object;
};

// The link's memory, mapped once.
struct Link {
	uint8_t *base = nullptr;
	uint64_t size = 0;
} link;

// A processor's audio: its block in the link, and the thread that processes each block the Linux side hands over.
class Audio {
public:
	Audio(IAudioProcessor &processor, const ProcessSetup &setup, Block *block, uint64_t size)
		: processor(processor), setup(setup), block(block), size(size)
	{
		processor.addRef();
		seen = block->done.load(std::memory_order_acquire);
		thread = CreateThread(nullptr, 0, [](void *self) -> DWORD {
			static_cast<Audio *>(self)->run();
			return 0;
		}, this, 0, nullptr);
		SetThreadPriority(thread, THREAD_PRIORITY_TIME_CRITICAL);
	}
	~Audio()
	{
		stopping.store(true);
		WaitForSingleObject(thread, INFINITE);
		CloseHandle(thread);
		input_changes->release();
		output_changes->release();
		input_events->release();
		output_events->release();
		processor.release();
	}
	Audio(const Audio &) = delete;
	Audio &operator=(const Audio &) = delete;

private:
	void run()
	{
		auto *const request = reinterpret_cast<volatile LONG *>(&block->request);
		while (!stopping.load()) {
			const uint32_t asked = block->request.load(std::memory_order_acquire);
			if (asked == seen) {
				// The wait looks at `stopping` again every so often, as nothing wakes it for that.
				RavenLinkWait(request, LONG(seen), 200);
				continue;
			}
			seen = asked;
			block->result = int32_t(to_wire(process()));
			block->done.store(seen, std::memory_order_release);
			RavenLinkWake(reinterpret_cast<volatile LONG *>(&block->done));
		}
	}
	tresult process()
	{
		const uint64_t sample = block->sample_size == kSample64 ? 8 : 4;
		if (block->samples < 0 || block->samples > setup.maxSamplesPerBlock || block->input_buses > Block::buses ||
			block->output_buses > Block::buses || block->sample_size != setup.symbolicSampleSize)
			return kInvalidArgument;
		uint64_t channels = 0;
		for (uint32_t bus = 0; bus < block->input_buses; ++bus)
			channels += uint64_t(std::max(0, block->input_channels[bus]));
		for (uint32_t bus = 0; bus < block->output_buses; ++bus)
			channels += uint64_t(std::max(0, block->output_channels[bus]));
		if (block_size(setup, uint32_t(channels)) > size)
			return kInvalidArgument;
		pointers.resize(size_t(channels));
		uint8_t *const audio = block_audio(block);
		for (uint64_t channel = 0; channel < channels; ++channel)
			pointers[channel] = audio + channel * uint64_t(setup.maxSamplesPerBlock) * sample;
		AudioBusBuffers inputs[Block::buses]{}, outputs[Block::buses]{};
		size_t next = 0;
		for (uint32_t bus = 0; bus < block->input_buses; ++bus) {
			inputs[bus].numChannels = block->input_channels[bus];
			inputs[bus].silenceFlags = block->input_silence[bus];
			inputs[bus].channelBuffers32 = reinterpret_cast<Sample32 **>(pointers.data() + next);
			next += size_t(std::max(0, block->input_channels[bus]));
		}
		for (uint32_t bus = 0; bus < block->output_buses; ++bus) {
			outputs[bus].numChannels = block->output_channels[bus];
			outputs[bus].channelBuffers32 = reinterpret_cast<Sample32 **>(pointers.data() + next);
			next += size_t(std::max(0, block->output_channels[bus]));
		}
		input_changes->take(block->input_changes, std::min(block->input_points, Block::points));
		output_changes->clear();
		take_events(*input_events, block->input_event, std::min(block->input_events, Block::events), block->input_event_data,
			std::min(block->input_event_bytes, Block::event_bytes));
		output_events->events.clear();
		ProcessData data;
		data.processMode = block->mode;
		data.symbolicSampleSize = block->sample_size;
		data.numSamples = block->samples;
		data.numInputs = int32(block->input_buses);
		data.numOutputs = int32(block->output_buses);
		data.inputs = block->input_buses ? inputs : nullptr;
		data.outputs = block->output_buses ? outputs : nullptr;
		data.inputParameterChanges = input_changes;
		data.outputParameterChanges = output_changes;
		data.inputEvents = input_events;
		data.outputEvents = output_events;
		data.processContext = block->has_context ? &block->context : nullptr;
		const tresult result = processor.process(data);
		for (uint32_t bus = 0; bus < block->output_buses; ++bus)
			block->output_silence[bus] = outputs[bus].silenceFlags;
		if (!output_changes->put(block->output_changes, Block::points, block->output_points) ||
			!put_events(*output_events, block->output_event, Block::events, block->output_events, block->output_event_data, block->output_event_bytes))
			return kOutOfMemory;
		return result;
	}

	IAudioProcessor &processor;
	ProcessSetup setup;
	Block *block;
	uint64_t size;
	uint32_t seen = 0;
	std::vector<void *> pointers;
	ParameterChanges *input_changes = new ParameterChanges, *output_changes = new ParameterChanges;
	EventList *input_events = new EventList, *output_events = new EventList;
	std::atomic<bool> stopping{false};
	HANDLE thread = nullptr;
};

// A plugin object the Linux side made, with the interfaces it has.
struct Object {
	FUnknown *unknown = nullptr;
	IComponent *component = nullptr;
	IAudioProcessor *processor = nullptr;
	IEditController *controller = nullptr;
	IConnectionPoint *connection = nullptr;
	Handler *handler = nullptr;
	Peer *peer = nullptr;
	std::unique_ptr<Audio> audio;

	uint32_t interfaces() const
	{
		return (component ? has_component : 0) | (processor ? has_processor : 0) | (controller ? has_controller : 0) |
			(connection ? has_connection : 0);
	}
	void end()
	{
		audio.reset();
		if (controller && handler)
			controller->setComponentHandler(nullptr);
		for (FUnknown *const held : {static_cast<FUnknown *>(component), static_cast<FUnknown *>(processor), static_cast<FUnknown *>(controller),
				 static_cast<FUnknown *>(connection), static_cast<FUnknown *>(handler), static_cast<FUnknown *>(peer), unknown})
			if (held)
				held->release();
	}
};

// The frame of a plugin's view: the window the view is attached to, shown inside the host's X11 window. A view that asks for another
// size gets it at once, and the Linux side hears of it to resize the host's window and tell the view.
class Frame : public Counted<IPlugFrame> {
public:
	Frame(uint32_t view, HWND window) : view(view), window(window) {}
	tresult PLUGIN_API resizeView(IPlugView *, ViewRect *size) override
	{
		if (!size || size->right < size->left || size->bottom < size->top)
			return kInvalidArgument;
		SetWindowPos(window, nullptr, 0, 0, size->right - size->left, size->bottom - size->top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
		Writer notice(uint32_t(Notice::resize_view));
		notice.u32(view).record(*size);
		output.send(notice);
		return kResultTrue;
	}

private:
	uint32_t view;
	HWND window;
};

// A plugin's view, with the window it is attached to once the Linux side has a window of the host's to show it in.
struct View {
	IPlugView *view = nullptr;
	HWND window = nullptr;
	Frame *frame = nullptr;

	void detach()
	{
		if (window) {
			view->removed();
			view->setFrame(nullptr);
			DestroyWindow(window);
			window = nullptr;
		}
		if (frame)
			frame->release();
		frame = nullptr;
	}
	void end()
	{
		detach();
		view->release();
	}
};

// The window a view is attached to: a popup with no frame, which the runtime shows inside the host's window.
HWND view_window(int32 width, int32 height)
{
	static const ATOM type = [] {
		WNDCLASSW made{};
		made.lpfnWndProc = DefWindowProcW;
		made.hInstance = GetModuleHandleW(nullptr);
		made.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		made.lpszClassName = L"RavenPluginView";
		return RegisterClassW(&made);
	}();
	return CreateWindowExW(0, MAKEINTATOM(type), L"", WS_POPUP | WS_CLIPCHILDREN, 0, 0, std::max(width, 1), std::max(height, 1), nullptr,
		nullptr, GetModuleHandleW(nullptr), nullptr);
}

class Server {
public:
	explicit Server(IPluginFactory &factory) : factory(factory) {}
	~Server()
	{
		for (auto &[id, view] : views)
			view.end();
		for (auto &[id, object] : objects)
			object.end();
	}

	void serve(Reader &call)
	{
		Writer answer(raven_vst::answer);
		switch (Call(call.tag)) {
		case Call::hello: hello(call, answer); break;
		case Call::create: create(call, answer); break;
		case Call::release: {
			const uint32_t id = call.u32();
			if (const auto found = objects.find(id); found != objects.end()) {
				found->second.end();
				objects.erase(found);
			} else if (const auto view = views.find(id); view != views.end()) {
				view->second.end();
				views.erase(view);
			}
			break;
		}
		case Call::connect:
		case Call::disconnect: {
			const uint32_t own = call.u32(), other = call.u32();
			Object &first = object(own);
			IConnectionPoint *target = nullptr;
			if (other)
				target = object(other).connection;
			else if (Call(call.tag) == Call::connect) {
				if (!first.peer)
					first.peer = new Peer(own);
				target = first.peer;
			} else
				target = first.peer;
			if (!first.connection || !target)
				answer.result(kResultFalse);
			else
				answer.result(Call(call.tag) == Call::connect ? first.connection->connect(target) : first.connection->disconnect(target));
			break;
		}
		case Call::notify: {
			Object &target = object(call.u32());
			auto *const message = new Message;
			message->read(call);
			answer.result(target.connection ? target.connection->notify(message) : kNoInterface);
			message->release();
			break;
		}
		case Call::initialize: {
			Object &target = object(call.u32());
			const auto name = call.bytes();
			if (!name.empty())
				host->name.assign(reinterpret_cast<const char16_t *>(name.data()), name.size() / sizeof(char16_t));
			IPluginBase *const base = target.component ? static_cast<IPluginBase *>(target.component) : target.controller;
			answer.result(base ? base->initialize(host) : kNoInterface);
			break;
		}
		case Call::terminate: {
			Object &target = object(call.u32());
			target.audio.reset();
			IPluginBase *const base = target.component ? static_cast<IPluginBase *>(target.component) : target.controller;
			answer.result(base ? base->terminate() : kNoInterface);
			break;
		}
		default:
			if (!component(call, answer) && !processor(call, answer) && !controller(call, answer) && !view(call, answer))
				throw std::runtime_error("an unknown call");
		}
		output.send(answer);
	}

private:
	Object &object(uint32_t id)
	{
		const auto found = objects.find(id);
		if (found == objects.end())
			throw std::runtime_error("a call names no object");
		return found->second;
	}

	void hello(Reader &, Writer &answer)
	{
		PFactoryInfo info{};
		factory.getFactoryInfo(&info);
		answer.u32(protocol).record(info);
		IPluginFactory2 *const second = query<IPluginFactory2>(&factory);
		const int32 count = factory.countClasses();
		std::vector<std::pair<ClassId, PClassInfo2>> classes;
		for (int32 index = 0; index < count; ++index) {
			PClassInfo2 more{};
			if (second && second->getClassInfo2(index, &more) == kResultOk) {
				classes.emplace_back(ClassId::of(more.cid), more);
				continue;
			}
			PClassInfo plain{};
			if (factory.getClassInfo(index, &plain) != kResultOk)
				continue;
			std::memcpy(more.cid, plain.cid, sizeof more.cid);
			more.cardinality = plain.cardinality;
			std::memcpy(more.category, plain.category, sizeof more.category);
			std::memcpy(more.name, plain.name, sizeof more.name);
			classes.emplace_back(ClassId::of(plain.cid), more);
		}
		if (second)
			second->release();
		answer.u32(uint32_t(classes.size()));
		for (const auto &[id, more] : classes)
			answer.id(id).record(more);
	}

	void create(Reader &call, Writer &answer)
	{
		TUID cid;
		call.id().to(cid);
		const uint32_t wanted = call.u32();
		const TUID &iid = wanted & has_controller ? IEditController::iid : wanted & has_processor ? IAudioProcessor::iid :
			wanted & has_component ? IComponent::iid : FUnknown::iid;
		void *made = nullptr;
		if (factory.createInstance(cid, iid, &made) != kResultOk || !made) {
			answer.u32(0).u32(0);
			return;
		}
		Object object;
		object.unknown = static_cast<FUnknown *>(made);
		object.component = query<IComponent>(object.unknown);
		object.processor = query<IAudioProcessor>(object.unknown);
		object.controller = query<IEditController>(object.unknown);
		object.connection = query<IConnectionPoint>(object.unknown);
		const uint32_t id = next++;
		answer.u32(id).u32(object.interfaces());
		objects.emplace(id, std::move(object));
	}

	bool component(Reader &call, Writer &answer)
	{
		const Call kind = Call(call.tag);
		if (kind < Call::controller_class || kind > Call::get_state)
			return false;
		const uint32_t id = call.u32();
		IComponent *const component = object(id).component;
		if (!component) {
			answer.result(kNoInterface);
			return true;
		}
		switch (kind) {
		case Call::controller_class: {
			TUID cid{};
			const tresult result = component->getControllerClassId(cid);
			answer.result(result).id(ClassId::of(cid));
			break;
		}
		case Call::set_io_mode: answer.result(component->setIoMode(call.i32())); break;
		case Call::bus_count: {
			const int32 type = call.i32();
			answer.i32(component->getBusCount(type, call.i32()));
			break;
		}
		case Call::bus_info: {
			const int32 type = call.i32(), direction = call.i32(), index = call.i32();
			BusInfo bus{};
			const tresult result = component->getBusInfo(type, direction, index, bus);
			answer.result(result).record(bus);
			break;
		}
		case Call::routing_info: {
			auto in = call.record<RoutingInfo>();
			RoutingInfo out{};
			const tresult result = component->getRoutingInfo(in, out);
			answer.result(result).record(out);
			break;
		}
		case Call::activate_bus: {
			const int32 type = call.i32(), direction = call.i32(), index = call.i32();
			answer.result(component->activateBus(type, direction, index, TBool(call.u32() != 0)));
			break;
		}
		case Call::set_active: answer.result(component->setActive(TBool(call.u32() != 0))); break;
		case Call::set_state: {
			auto *const stream = new MemoryStream(call.bytes());
			answer.result(component->setState(stream));
			stream->release();
			break;
		}
		case Call::get_state: {
			auto *const stream = new MemoryStream;
			const tresult result = component->getState(stream);
			answer.result(result).bytes(stream->data.data(), stream->data.size());
			stream->release();
			break;
		}
		default: return false;
		}
		return true;
	}

	bool processor(Reader &call, Writer &answer)
	{
		const Call kind = Call(call.tag);
		if (kind < Call::set_arrangements || kind > Call::tail_samples)
			return false;
		const uint32_t id = call.u32();
		Object &target = object(id);
		IAudioProcessor *const processor = target.processor;
		if (!processor) {
			answer.result(kNoInterface);
			return true;
		}
		switch (kind) {
		case Call::set_arrangements: {
			std::vector<SpeakerArrangement> inputs(call.u32()), outputs;
			for (auto &arrangement : inputs)
				arrangement = call.u64();
			outputs.resize(call.u32());
			for (auto &arrangement : outputs)
				arrangement = call.u64();
			answer.result(processor->setBusArrangements(inputs.data(), int32(inputs.size()), outputs.data(), int32(outputs.size())));
			break;
		}
		case Call::get_arrangement: {
			const int32 direction = call.i32(), index = call.i32();
			SpeakerArrangement arrangement = 0;
			const tresult result = processor->getBusArrangement(direction, index, arrangement);
			answer.result(result).u64(arrangement);
			break;
		}
		case Call::can_process_sample_size: answer.result(processor->canProcessSampleSize(call.i32())); break;
		case Call::latency_samples: answer.u32(processor->getLatencySamples()); break;
		case Call::setup_processing: {
			auto setup = call.record<ProcessSetup>();
			const uint64_t offset = call.u64(), size = call.u64();
			if (!link.base) {
				SIZE_T mapped = 0;
				link.base = static_cast<uint8_t *>(RavenLinkMap(&mapped));
				link.size = mapped;
			}
			if (!link.base || offset % 64 || size < sizeof(Block) || offset > link.size || size > link.size - offset ||
				setup.maxSamplesPerBlock <= 0) {
				answer.result(kInvalidArgument);
				break;
			}
			target.audio.reset();
			const tresult result = processor->setupProcessing(setup);
			if (result == kResultOk)
				target.audio = std::make_unique<Audio>(*processor, setup, reinterpret_cast<Block *>(link.base + offset), size);
			answer.result(result);
			break;
		}
		case Call::set_processing: answer.result(processor->setProcessing(TBool(call.u32() != 0))); break;
		case Call::tail_samples: answer.u32(processor->getTailSamples()); break;
		default: return false;
		}
		return true;
	}

	bool controller(Reader &call, Writer &answer)
	{
		const Call kind = Call(call.tag);
		if (kind < Call::set_component_state || kind > Call::set_handler)
			return false;
		const uint32_t id = call.u32();
		Object &target = object(id);
		IEditController *const controller = target.controller;
		if (!controller) {
			answer.result(kNoInterface);
			return true;
		}
		switch (kind) {
		case Call::set_component_state:
		case Call::set_controller_state: {
			auto *const stream = new MemoryStream(call.bytes());
			answer.result(kind == Call::set_component_state ? controller->setComponentState(stream) : controller->setState(stream));
			stream->release();
			break;
		}
		case Call::get_controller_state: {
			auto *const stream = new MemoryStream;
			const tresult result = controller->getState(stream);
			answer.result(result).bytes(stream->data.data(), stream->data.size());
			stream->release();
			break;
		}
		case Call::parameter_count: answer.i32(controller->getParameterCount()); break;
		case Call::parameter_info: {
			ParameterInfo info{};
			const tresult result = controller->getParameterInfo(call.i32(), info);
			answer.result(result).record(info);
			break;
		}
		case Call::string_by_value: {
			const ParamID parameter = call.u32();
			String128 text{};
			const tresult result = controller->getParamStringByValue(parameter, call.f64(), text);
			answer.result(result).text(text);
			break;
		}
		case Call::value_by_string: {
			const ParamID parameter = call.u32();
			auto units = call.bytes();
			std::vector<TChar> text(units.size() / sizeof(TChar) + 1, 0);
			std::memcpy(text.data(), units.data(), units.size() / sizeof(TChar) * sizeof(TChar));
			ParamValue value = 0;
			const tresult result = controller->getParamValueByString(parameter, text.data(), value);
			answer.result(result).f64(value);
			break;
		}
		case Call::to_plain: {
			const ParamID parameter = call.u32();
			answer.f64(controller->normalizedParamToPlain(parameter, call.f64()));
			break;
		}
		case Call::to_normalized: {
			const ParamID parameter = call.u32();
			answer.f64(controller->plainParamToNormalized(parameter, call.f64()));
			break;
		}
		case Call::get_normalized: answer.f64(controller->getParamNormalized(call.u32())); break;
		case Call::set_normalized: {
			const ParamID parameter = call.u32();
			answer.result(controller->setParamNormalized(parameter, call.f64()));
			break;
		}
		case Call::set_handler: {
			if (call.u32()) {
				if (!target.handler)
					target.handler = new Handler(id);
				answer.result(controller->setComponentHandler(target.handler));
			} else
				answer.result(controller->setComponentHandler(nullptr));
			break;
		}
		default: return false;
		}
		return true;
	}

	bool view(Reader &call, Writer &answer)
	{
		const Call kind = Call(call.tag);
		if (kind == Call::create_view) {
			IEditController *const controller = object(call.u32()).controller;
			const auto name = call.bytes();
			const std::string text(name.begin(), name.end());
			IPlugView *const made = controller ? controller->createView(text.c_str()) : nullptr;
			if (!made) {
				answer.u32(0);
				return true;
			}
			const uint32_t id = next++;
			views[id].view = made;
			answer.u32(id);
			return true;
		}
		if (kind < Call::view_platform || kind > Call::view_scale)
			return false;
		const auto found = views.find(call.u32());
		if (found == views.end())
			throw std::runtime_error("a call names no view");
		View &target = found->second;
		IPlugView &view = *target.view;
		switch (kind) {
		case Call::view_platform: {
			// The host's X11 window or Wayland surface shows a window of the program's own: the view is attached to that as on Windows.
			const auto type = call.bytes();
			const std::string name(type.begin(), type.end());
			const bool linux_type = name == kPlatformTypeX11EmbedWindowID || name == kPlatformTypeWaylandSurfaceID;
			answer.result(linux_type ? view.isPlatformTypeSupported(kPlatformTypeHWND) : kResultFalse);
			break;
		}
		case Call::view_attach: {
			const uint64_t host = call.u64();
			target.detach();
			ViewRect size{};
			if (view.getSize(&size) != kResultOk)
				size = ViewRect(0, 0, 400, 300);
			target.window = view_window(size.getWidth(), size.getHeight());
			if (!target.window || !RavenEmbedWindow(target.window, host)) {
				if (target.window)
					DestroyWindow(target.window);
				target.window = nullptr;
				answer.result(kResultFalse);
				break;
			}
			target.frame = new Frame(found->first, target.window);
			view.setFrame(target.frame);
			ShowWindow(target.window, SW_SHOWNOACTIVATE);
			const tresult result = view.attached(target.window, kPlatformTypeHWND);
			if (result != kResultOk)
				target.detach();
			else
				UpdateWindow(target.window);
			answer.result(result);
			break;
		}
		case Call::view_remove: {
			const bool attached = target.window != nullptr;
			target.detach();
			answer.result(attached ? kResultOk : kResultFalse);
			break;
		}
		case Call::view_size: {
			ViewRect size{};
			const tresult result = view.getSize(&size);
			answer.result(result).record(size);
			break;
		}
		case Call::view_on_size: {
			auto size = call.record<ViewRect>();
			if (target.window)
				SetWindowPos(target.window, nullptr, 0, 0, size.getWidth(), size.getHeight(), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
			answer.result(view.onSize(&size));
			break;
		}
		case Call::view_focus: answer.result(view.onFocus(TBool(call.u32() != 0))); break;
		case Call::view_can_resize: answer.result(view.canResize()); break;
		case Call::view_check_size: {
			auto size = call.record<ViewRect>();
			const tresult result = view.checkSizeConstraint(&size);
			answer.result(result).record(size);
			break;
		}
		case Call::view_scale: {
			const double factor = call.f64();
			auto *const scaling = query<IPlugViewContentScaleSupport>(&view);
			answer.result(scaling ? scaling->setContentScaleFactor(IPlugViewContentScaleSupport::ScaleFactor(factor)) : kNotImplemented);
			if (scaling)
				scaling->release();
			break;
		}
		default: return false;
		}
		return true;
	}

	IPluginFactory &factory;
	std::map<uint32_t, Object> objects;
	std::map<uint32_t, View> views;
	uint32_t next = 1;
};

// Answers each call as it comes and dispatches the messages of the plugin's windows meanwhile, on the thread that loaded the plugin, as
// a Windows host calls a plugin's controller and editor from its user interface thread.
int serve(IPluginFactory &factory)
{
	Server server(factory);
	Input input;
	for (;;) {
		std::vector<uint8_t> frame;
		bool ended = false;
		while (input.next(frame, ended)) {
			Reader call(std::move(frame));
			server.serve(call);
		}
		if (ended)
			return 0;
		MsgWaitForMultipleObjects(1, &input.ready, FALSE, INFINITE, QS_ALLINPUT);
		MSG message;
		while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&message);
			DispatchMessageW(&message);
		}
	}
}

}

int wmain(int count, wchar_t **arguments)
{
	const std::wstring mode = count == 3 ? arguments[1] : L"";
	if (mode != L"--list" && mode != L"--serve") {
		std::fputs("usage: raven-vsthost.exe --list|--serve PLUGIN\n", stderr);
		return 2;
	}
	// Plugins use COM for their own objects, as on Windows, where every host initializes it on the thread it loads plugins from.
	OleInitialize(nullptr);
	Module module(arguments[2]);
	if (!module.factory) {
		std::fprintf(stderr, "raven-vsthost: %ls has no plugin factory (error %lu)\n", arguments[2], GetLastError());
		return 1;
	}
	if (mode == L"--list")
		return list(*module.factory);
	try {
		return serve(*module.factory);
	} catch (const std::exception &error) {
		std::fprintf(stderr, "raven-vsthost: %s\n", error.what());
		return 3;
	}
}
