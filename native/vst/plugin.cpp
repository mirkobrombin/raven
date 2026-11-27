// The Linux half of a bridged VST 3 plugin: a plugin module a Linux host loads like any other, which runs the Windows plugin it stands for
// under Raven and forwards everything the host asks of it (native/guest/raven-vsthost/bridge.h).
//
// The module lives in a bundle of its own, NAME.vst3/Contents/x86_64-linux/NAME.so, beside Contents/Resources/raven-bridge.conf, whose
// lines name what to run:
//
//   plugin=PATH   the Windows plugin, a .vst3 file or bundle (required)
//   raven=PATH    the raven launcher (default: raven on PATH)
//   host=PATH     raven-vsthost.exe (default: beside the launcher)
//   prefix=PATH   the Windows prefix the plugin runs in (default: Raven's own)
//
// The Windows side starts with the factory and ends when the module is unloaded. Every call waits for its answer; audio goes through
// memory the two sides share, one block for each processor, without a call.

#include "guest/raven-vsthost/bridge_objects.h"

#include "pluginterfaces/vst/ivsthostapplication.h"

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <thread>

#include <dlfcn.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

using namespace raven_vst;

namespace {

// The memory the two sides share: 256 MiB of address space, taken only where blocks are written.
constexpr uint64_t link_size = uint64_t(256) << 20;

void wake(std::atomic<uint32_t> &word)
{
	::syscall(SYS_futex, &word, FUTEX_WAKE, INT32_MAX, nullptr, nullptr, 0);
}

class Proxy;

class Bridge {
public:
	// The bridge of the module this code is in, started at its first use.
	static Bridge &instance()
	{
		static Bridge bridge;
		return bridge;
	}
	bool running() const { return child > 0 && !failed; }

	// Sends a call and waits for its answer, which is empty when the Windows side is gone. What the plugin told the host meanwhile goes
	// to the host once the answer came, outside the bridge's lock, as the host may call the plugin again from there.
	std::optional<Reader> call(Writer &request)
	{
		std::optional<Reader> answered;
		{
			std::unique_lock guard(calling);
			if (!running())
				return std::nullopt;
			const auto &frame = request.frame();
			for (size_t at = 0; at < frame.size();) {
				const ssize_t written = ::write(to_host, frame.data() + at, frame.size() - at);
				if (written <= 0) {
					failed = true;
					return std::nullopt;
				}
				at += size_t(written);
			}
			std::unique_lock wait(lock);
			answers_ready.wait(wait, [this] { return !answers.empty() || failed; });
			if (answers.empty())
				return std::nullopt;
			answered.emplace(std::move(answers.front()));
			answers.pop_front();
		}
		deliver();
		return answered;
	}
	// Hands on the notices that came, for a call that had none to wait for.
	void deliver();

	void attach(uint32_t object, Proxy *proxy)
	{
		const std::lock_guard guard(lock);
		proxies[object] = proxy;
	}
	void detach(uint32_t object)
	{
		const std::lock_guard guard(lock);
		proxies.erase(object);
	}

	// Room in the link for a block of `size` bytes; none when the link is full.
	std::optional<uint64_t> allocate(uint64_t size)
	{
		const std::lock_guard guard(lock);
		const uint64_t at = (used + 63) / 64 * 64;
		if (at > link_size || size > link_size - at)
			return std::nullopt;
		used = at + size;
		return at;
	}
	uint8_t *link = nullptr;

	PFactoryInfo factory{};
	std::vector<std::pair<ClassId, PClassInfo2>> classes;

private:
	Bridge()
	{
		Dl_info self{};
		if (!::dladdr(reinterpret_cast<void *>(&Bridge::instance), &self) || !self.dli_fname)
			return;
		const std::filesystem::path module = std::filesystem::absolute(self.dli_fname);
		std::ifstream settings(module.parent_path().parent_path() / "Resources" / "raven-bridge.conf");
		std::string line, plugin, raven = "raven", host, prefix;
		while (std::getline(settings, line)) {
			const size_t equal = line.find('=');
			if (equal == std::string::npos || line.starts_with('#'))
				continue;
			const std::string key = line.substr(0, equal), value = line.substr(equal + 1);
			if (key == "plugin")
				plugin = value;
			else if (key == "raven")
				raven = value;
			else if (key == "host")
				host = value;
			else if (key == "prefix")
				prefix = value;
		}
		if (plugin.empty()) {
			std::fprintf(stderr, "raven-vst: %s names no plugin\n", (module.parent_path().parent_path() / "Resources/raven-bridge.conf").c_str());
			return;
		}
		if (raven.find('/') == std::string::npos)
			raven = find_program(raven);
		if (host.empty())
			host = (std::filesystem::path(raven).parent_path() / "raven-vsthost.exe").string();
		if (!start(raven, host, std::filesystem::absolute(plugin), prefix))
			return;
		Writer hello(uint32_t(Call::hello));
		auto answer = call(hello);
		try {
			if (!answer || answer->u32() != protocol)
				throw std::runtime_error("the Windows side speaks another protocol");
			factory = answer->record<PFactoryInfo>();
			for (uint32_t count = answer->u32(); count; --count) {
				const ClassId id = answer->id();
				auto info = answer->record<PClassInfo2>();
				id.to(info.cid);
				classes.emplace_back(id, info);
			}
		} catch (const std::exception &error) {
			std::fprintf(stderr, "raven-vst: %s\n", error.what());
			failed = true;
		}
	}
	~Bridge()
	{
		if (child <= 0)
			return;
		// The end of its input ends the Windows side.
		::close(to_host);
		int status = 0;
		for (int round = 0; round < 50 && ::waitpid(child, &status, WNOHANG) == 0; ++round)
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		if (::waitpid(child, &status, WNOHANG) == 0) {
			::kill(child, SIGKILL);
			::waitpid(child, &status, 0);
		}
		if (reader.joinable())
			reader.join();
		::close(from_host);
	}

	static std::string find_program(const std::string &name)
	{
		const char *path = std::getenv("PATH");
		std::string folders = path ? path : "/usr/bin";
		for (size_t start = 0; start <= folders.size();) {
			const size_t end = std::min(folders.find(':', start), folders.size());
			const auto candidate = std::filesystem::path(folders.substr(start, end - start)) / name;
			if (::access(candidate.c_str(), X_OK) == 0)
				return candidate.string();
			start = end + 1;
		}
		return name;
	}

	// The Windows spelling of a host path, on Z:, where Raven shows the host paths it grants.
	static std::string windows_path(const std::filesystem::path &host)
	{
		std::string text = "Z:" + host.lexically_normal().string();
		for (char &character : text)
			if (character == '/')
				character = '\\';
		return text;
	}

	bool start(const std::string &raven, const std::string &host, const std::filesystem::path &plugin, const std::string &prefix)
	{
		const int memory = ::memfd_create("raven-vst", MFD_CLOEXEC);
		if (memory < 0 || ::ftruncate(memory, off_t(link_size)))
			return false;
		void *const mapped = ::mmap(nullptr, link_size, PROT_READ | PROT_WRITE, MAP_SHARED, memory, 0);
		if (mapped == MAP_FAILED)
			return false;
		link = static_cast<uint8_t *>(mapped);
		int input[2], answers_pipe[2];
		if (::pipe2(input, O_CLOEXEC) || ::pipe2(answers_pipe, O_CLOEXEC))
			return false;
		// The descriptors the child gets are moved out of the way of the numbers it gets them as.
		const int child_input = ::fcntl(input[0], F_DUPFD_CLOEXEC, 10), child_output = ::fcntl(answers_pipe[1], F_DUPFD_CLOEXEC, 10),
			child_link = ::fcntl(memory, F_DUPFD_CLOEXEC, 10);
		::close(input[0]);
		::close(answers_pipe[1]);
		::close(memory);
		to_host = input[1];
		from_host = answers_pipe[0];
		posix_spawn_file_actions_t actions;
		posix_spawn_file_actions_init(&actions);
		posix_spawn_file_actions_adddup2(&actions, child_input, 0);
		posix_spawn_file_actions_adddup2(&actions, child_output, 1);
		posix_spawn_file_actions_adddup2(&actions, child_link, 3);
		const std::string command = "--command-line=raven-vsthost.exe --serve \"" + windows_path(plugin) + "\"";
		const std::string grant = "--grant-read=" + (std::filesystem::is_directory(plugin) ? plugin : plugin.parent_path()).string();
		std::vector<std::string> arguments{raven, "--run-process", "--execution=native", "--stdio=inherit", "--timeout-ms=86400000",
			"--link-fd=3", grant, command, host};
		std::vector<char *> argv;
		for (auto &argument : arguments)
			argv.push_back(argument.data());
		argv.push_back(nullptr);
		std::vector<std::string> environment;
		for (char **variable = environ; *variable; ++variable)
			if (prefix.empty() || !std::string_view(*variable).starts_with("RAVEN_PREFIX="))
				environment.emplace_back(*variable);
		if (!prefix.empty())
			environment.push_back("RAVEN_PREFIX=" + prefix);
		std::vector<char *> envp;
		for (auto &variable : environment)
			envp.push_back(variable.data());
		envp.push_back(nullptr);
		const int spawned = ::posix_spawn(&child, raven.c_str(), &actions, nullptr, argv.data(), envp.data());
		posix_spawn_file_actions_destroy(&actions);
		::close(child_input);
		::close(child_output);
		::close(child_link);
		if (spawned) {
			child = -1;
			std::fprintf(stderr, "raven-vst: %s does not start: %s\n", raven.c_str(), std::strerror(spawned));
			return false;
		}
		reader = std::thread([this] { read(); });
		return true;
	}

	bool read_exactly(void *target, size_t size)
	{
		for (size_t at = 0; at < size;) {
			const ssize_t count = ::read(from_host, static_cast<uint8_t *>(target) + at, size - at);
			if (count <= 0)
				return false;
			at += size_t(count);
		}
		return true;
	}
	void read()
	{
		for (;;) {
			uint32_t length = 0;
			std::vector<uint8_t> frame;
			if (!read_exactly(&length, sizeof length) || length < sizeof(uint32_t) || length > (1U << 28) ||
				(frame.resize(length), !read_exactly(frame.data(), length))) {
				const std::lock_guard guard(lock);
				failed = true;
				answers_ready.notify_all();
				return;
			}
			Reader message(std::move(frame));
			const std::lock_guard guard(lock);
			if (message.tag == answer) {
				answers.push_back(std::move(message));
				answers_ready.notify_all();
			} else
				notices.push_back(std::move(message));
		}
	}

	std::mutex calling, lock;
	std::condition_variable answers_ready;
	std::deque<Reader> answers, notices;
	std::map<uint32_t, Proxy *> proxies;
	std::thread reader;
	pid_t child = -1;
	int to_host = -1, from_host = -1;
	bool failed = false;
	uint64_t used = 4096;
};

// Shorthands for the calls of an object.
std::optional<Reader> ask(Writer &request) { return Bridge::instance().call(request); }

// The result an answer starts with, read in place, so what follows it is read next.
tresult result_of(std::optional<Reader> &answer)
{
	try {
		return answer ? answer->result() : kInternalError;
	} catch (const std::exception &) {
		return kInternalError;
	}
}

tresult result_of(std::optional<Reader> &&answer) { return result_of(answer); }

// What the host makes from a plugin class: its component, processor, controller and connection point, as the Windows object has them.
class Proxy : public IComponent, public IAudioProcessor, public IConnectionPoint {
public:
	Proxy(uint32_t object, uint32_t interfaces) : object(object), interfaces(interfaces) { Bridge::instance().attach(object, this); }

	tresult PLUGIN_API queryInterface(const TUID iid, void **found) override
	{
		*found = nullptr;
		if (same(iid, FUnknown::iid) || same(iid, IPluginBase::iid))
			*found = interfaces & has_component ? static_cast<FUnknown *>(static_cast<IComponent *>(this)) :
				static_cast<FUnknown *>(&controller);
		else if (same(iid, IComponent::iid) && interfaces & has_component)
			*found = static_cast<IComponent *>(this);
		else if (same(iid, IAudioProcessor::iid) && interfaces & has_processor)
			*found = static_cast<IAudioProcessor *>(this);
		else if (same(iid, IEditController::iid) && interfaces & has_controller)
			*found = static_cast<IEditController *>(&controller);
		else if (same(iid, IConnectionPoint::iid) && interfaces & has_connection)
			*found = static_cast<IConnectionPoint *>(this);
		if (!*found)
			return kNoInterface;
		addRef();
		return kResultOk;
	}
	uint32 PLUGIN_API addRef() override { return ++references; }
	uint32 PLUGIN_API release() override
	{
		const uint32 left = --references;
		if (!left)
			delete this;
		return left;
	}

	// IPluginBase
	tresult PLUGIN_API initialize(FUnknown *context) override
	{
		String128 name{};
		IHostApplication *application = nullptr;
		if (context && context->queryInterface(IHostApplication::iid, reinterpret_cast<void **>(&application)) == kResultOk && application) {
			application->getName(name);
			application->release();
		}
		return result_of(ask(Writer(uint32_t(Call::initialize)).u32(object).text(name)));
	}
	tresult PLUGIN_API terminate() override
	{
		block = nullptr;
		return result_of(ask(Writer(uint32_t(Call::terminate)).u32(object)));
	}

	// IComponent
	tresult PLUGIN_API getControllerClassId(TUID id) override
	{
		auto answer = ask(Writer(uint32_t(Call::controller_class)).u32(object));
		const tresult result = result_of(answer);
		if (result == kResultOk)
			answer->id().to(id);
		return result;
	}
	tresult PLUGIN_API setIoMode(IoMode mode) override { return result_of(ask(Writer(uint32_t(Call::set_io_mode)).u32(object).i32(mode))); }
	int32 PLUGIN_API getBusCount(MediaType type, BusDirection direction) override
	{
		auto answer = ask(Writer(uint32_t(Call::bus_count)).u32(object).i32(type).i32(direction));
		return answer ? answer->i32() : 0;
	}
	tresult PLUGIN_API getBusInfo(MediaType type, BusDirection direction, int32 index, BusInfo &bus) override
	{
		auto answer = ask(Writer(uint32_t(Call::bus_info)).u32(object).i32(type).i32(direction).i32(index));
		const tresult result = result_of(answer);
		if (result == kResultOk)
			bus = answer->record<BusInfo>();
		return result;
	}
	tresult PLUGIN_API getRoutingInfo(RoutingInfo &in, RoutingInfo &out) override
	{
		auto answer = ask(Writer(uint32_t(Call::routing_info)).u32(object).record(in));
		const tresult result = result_of(answer);
		if (result == kResultOk)
			out = answer->record<RoutingInfo>();
		return result;
	}
	tresult PLUGIN_API activateBus(MediaType type, BusDirection direction, int32 index, TBool state) override
	{
		return result_of(ask(Writer(uint32_t(Call::activate_bus)).u32(object).i32(type).i32(direction).i32(index).u32(state ? 1 : 0)));
	}
	tresult PLUGIN_API setActive(TBool state) override { return result_of(ask(Writer(uint32_t(Call::set_active)).u32(object).u32(state ? 1 : 0))); }
	tresult PLUGIN_API setState(IBStream *state) override { return send_state(Call::set_state, state); }
	tresult PLUGIN_API getState(IBStream *state) override { return receive_state(Call::get_state, state); }

	// IAudioProcessor
	tresult PLUGIN_API setBusArrangements(SpeakerArrangement *inputs, int32 input_count, SpeakerArrangement *outputs, int32 output_count) override
	{
		if (input_count < 0 || output_count < 0 || (input_count && !inputs) || (output_count && !outputs))
			return kInvalidArgument;
		Writer request(uint32_t(Call::set_arrangements));
		request.u32(object).u32(uint32_t(input_count));
		for (int32 at = 0; at < input_count; ++at)
			request.u64(inputs[at]);
		request.u32(uint32_t(output_count));
		for (int32 at = 0; at < output_count; ++at)
			request.u64(outputs[at]);
		return result_of(Bridge::instance().call(request));
	}
	tresult PLUGIN_API getBusArrangement(BusDirection direction, int32 index, SpeakerArrangement &arrangement) override
	{
		auto answer = ask(Writer(uint32_t(Call::get_arrangement)).u32(object).i32(direction).i32(index));
		const tresult result = result_of(answer);
		if (result == kResultOk)
			arrangement = answer->u64();
		return result;
	}
	tresult PLUGIN_API canProcessSampleSize(int32 size) override
	{
		return result_of(ask(Writer(uint32_t(Call::can_process_sample_size)).u32(object).i32(size)));
	}
	uint32 PLUGIN_API getLatencySamples() override
	{
		auto answer = ask(Writer(uint32_t(Call::latency_samples)).u32(object));
		return answer ? answer->u32() : 0;
	}
	tresult PLUGIN_API setupProcessing(ProcessSetup &setup) override
	{
		if (setup.maxSamplesPerBlock <= 0)
			return kInvalidArgument;
		// Room for the channels of every bus, as many as the plugin's buses have now.
		uint32_t channels = 0;
		for (const BusDirection direction : {kInput, kOutput}) {
			const int32 count = std::min<int32>(getBusCount(kAudio, direction), int32(Block::buses));
			for (int32 index = 0; index < count; ++index) {
				BusInfo bus{};
				if (getBusInfo(kAudio, direction, index, bus) == kResultOk)
					channels += uint32_t(std::max(0, bus.channelCount));
			}
		}
		const uint64_t size = block_size(setup, channels);
		// A block that has room enough is used again; the link does not give room back, so a larger one is taken anew.
		const bool reused = block_room && size <= block_room;
		const std::optional<uint64_t> offset = reused ? std::optional<uint64_t>(block_offset) : Bridge::instance().allocate(size);
		if (!offset)
			return kOutOfMemory;
		block = nullptr;
		auto *const made = reinterpret_cast<Block *>(Bridge::instance().link + *offset);
		std::memset(static_cast<void *>(made), 0, sizeof(Block));
		const tresult result = result_of(ask(Writer(uint32_t(Call::setup_processing)).u32(object).record(setup).u64(*offset).u64(size)));
		if (!reused) {
			block_offset = *offset;
			block_room = size;
		}
		if (result == kResultOk) {
			block = made;
			this->setup = setup;
		}
		return result;
	}
	tresult PLUGIN_API setProcessing(TBool state) override { return result_of(ask(Writer(uint32_t(Call::set_processing)).u32(object).u32(state ? 1 : 0))); }
	tresult PLUGIN_API process(ProcessData &data) override;
	uint32 PLUGIN_API getTailSamples() override
	{
		auto answer = ask(Writer(uint32_t(Call::tail_samples)).u32(object));
		return answer ? answer->u32() : 0;
	}

	// IEditController, which a class of its own answers, as its state calls have the same names as the component's.
	class Controller : public IEditController {
	public:
		explicit Controller(Proxy &owner) : owner(owner) {}
		tresult PLUGIN_API queryInterface(const TUID iid, void **found) override { return owner.queryInterface(iid, found); }
		uint32 PLUGIN_API addRef() override { return owner.addRef(); }
		uint32 PLUGIN_API release() override { return owner.release(); }
		tresult PLUGIN_API initialize(FUnknown *context) override { return owner.initialize(context); }
		tresult PLUGIN_API terminate() override { return owner.terminate(); }
		tresult PLUGIN_API setComponentState(IBStream *state) override { return owner.send_state(Call::set_component_state, state); }
		tresult PLUGIN_API setState(IBStream *state) override { return owner.send_state(Call::set_controller_state, state); }
		tresult PLUGIN_API getState(IBStream *state) override { return owner.receive_state(Call::get_controller_state, state); }
		int32 PLUGIN_API getParameterCount() override
		{
			auto answer = ask(Writer(uint32_t(Call::parameter_count)).u32(owner.object));
			return answer ? answer->i32() : 0;
		}
		tresult PLUGIN_API getParameterInfo(int32 index, ParameterInfo &info) override
		{
			auto answer = ask(Writer(uint32_t(Call::parameter_info)).u32(owner.object).i32(index));
			const tresult result = result_of(answer);
			if (result == kResultOk)
				info = answer->record<ParameterInfo>();
			return result;
		}
		tresult PLUGIN_API getParamStringByValue(ParamID id, ParamValue value, String128 text) override
		{
			auto answer = ask(Writer(uint32_t(Call::string_by_value)).u32(owner.object).u32(id).f64(value));
			const tresult result = result_of(answer);
			if (result == kResultOk)
				answer->text(text, 128);
			return result;
		}
		tresult PLUGIN_API getParamValueByString(ParamID id, TChar *text, ParamValue &value) override
		{
			auto answer = ask(Writer(uint32_t(Call::value_by_string)).u32(owner.object).u32(id).text(text));
			const tresult result = result_of(answer);
			if (result == kResultOk)
				value = answer->f64();
			return result;
		}
		ParamValue PLUGIN_API normalizedParamToPlain(ParamID id, ParamValue value) override
		{
			auto answer = ask(Writer(uint32_t(Call::to_plain)).u32(owner.object).u32(id).f64(value));
			return answer ? answer->f64() : value;
		}
		ParamValue PLUGIN_API plainParamToNormalized(ParamID id, ParamValue value) override
		{
			auto answer = ask(Writer(uint32_t(Call::to_normalized)).u32(owner.object).u32(id).f64(value));
			return answer ? answer->f64() : value;
		}
		ParamValue PLUGIN_API getParamNormalized(ParamID id) override
		{
			auto answer = ask(Writer(uint32_t(Call::get_normalized)).u32(owner.object).u32(id));
			return answer ? answer->f64() : 0;
		}
		tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue value) override
		{
			return result_of(ask(Writer(uint32_t(Call::set_normalized)).u32(owner.object).u32(id).f64(value)));
		}
		tresult PLUGIN_API setComponentHandler(IComponentHandler *given) override
		{
			{
				const std::lock_guard guard(owner.held);
				if (given)
					given->addRef();
				if (owner.handler)
					owner.handler->release();
				owner.handler = given;
			}
			return result_of(ask(Writer(uint32_t(Call::set_handler)).u32(owner.object).u32(given ? 1 : 0)));
		}
		IPlugView *PLUGIN_API createView(FIDString) override { return nullptr; }

	private:
		Proxy &owner;
	} controller{*this};

	// IConnectionPoint: two of the bridge's own objects are connected on the Windows side, where the plugin's objects talk directly; a
	// connection point of the host's gets what the plugin object sends, and sends it what the host wants it to have.
	tresult PLUGIN_API connect(IConnectionPoint *other) override
	{
		if (!other)
			return kInvalidArgument;
		if (auto *const own = dynamic_cast<Proxy *>(other))
			return result_of(ask(Writer(uint32_t(Call::connect)).u32(object).u32(own->object)));
		{
			const std::lock_guard guard(held);
			other->addRef();
			if (peer)
				peer->release();
			peer = other;
		}
		return result_of(ask(Writer(uint32_t(Call::connect)).u32(object).u32(0)));
	}
	tresult PLUGIN_API disconnect(IConnectionPoint *other) override
	{
		if (auto *const own = dynamic_cast<Proxy *>(other))
			return result_of(ask(Writer(uint32_t(Call::disconnect)).u32(object).u32(own->object)));
		{
			const std::lock_guard guard(held);
			if (peer)
				peer->release();
			peer = nullptr;
		}
		return result_of(ask(Writer(uint32_t(Call::disconnect)).u32(object).u32(0)));
	}
	tresult PLUGIN_API notify(IMessage *message) override
	{
		if (!message)
			return kInvalidArgument;
		Writer request(uint32_t(Call::notify));
		request.u32(object);
		Message::write(request, *message);
		return result_of(Bridge::instance().call(request));
	}

	// A notice of the plugin object's, handed on by the thread whose call it came with.
	void tell(Reader &notice)
	{
		IComponentHandler *target = nullptr;
		IConnectionPoint *connected = nullptr;
		{
			const std::lock_guard guard(held);
			if ((target = handler))
				target->addRef();
			if ((connected = peer))
				connected->addRef();
		}
		switch (Notice(notice.tag)) {
		case Notice::begin_edit:
			if (target)
				target->beginEdit(notice.u32());
			break;
		case Notice::perform_edit:
			if (target) {
				const ParamID id = notice.u32();
				target->performEdit(id, notice.f64());
			}
			break;
		case Notice::end_edit:
			if (target)
				target->endEdit(notice.u32());
			break;
		case Notice::restart:
			if (target)
				target->restartComponent(notice.i32());
			break;
		case Notice::message:
			if (connected) {
				auto *const message = new Message;
				message->read(notice);
				connected->notify(message);
				message->release();
			}
			break;
		}
		if (target)
			target->release();
		if (connected)
			connected->release();
	}

	const uint32_t object;

private:
	virtual ~Proxy()
	{
		Bridge::instance().detach(object);
		ask(Writer(uint32_t(Call::release)).u32(object));
		if (handler)
			handler->release();
		if (peer)
			peer->release();
		input_changes->release();
		input_events->release();
	}
	tresult send_state(Call kind, IBStream *state)
	{
		std::vector<uint8_t> bytes;
		if (!read_all(state, bytes))
			return kInvalidArgument;
		return result_of(ask(Writer(uint32_t(kind)).u32(object).bytes(bytes.data(), bytes.size())));
	}
	tresult receive_state(Call kind, IBStream *state)
	{
		if (!state)
			return kInvalidArgument;
		auto answer = ask(Writer(uint32_t(kind)).u32(object));
		const tresult result = result_of(answer);
		if (result != kResultOk)
			return result;
		auto bytes = answer->bytes();
		int32 written = 0;
		return bytes.empty() ? kResultOk : state->write(bytes.data(), int32(bytes.size()), &written);
	}

	std::atomic<uint32> references{1};
	const uint32_t interfaces;
	std::mutex held;
	IComponentHandler *handler = nullptr;
	IConnectionPoint *peer = nullptr;
	Block *block = nullptr;
	uint64_t block_offset = 0, block_room = 0;
	ProcessSetup setup{};
	ParameterChanges *input_changes = new ParameterChanges;
	EventList *input_events = new EventList;
};

void Bridge::deliver()
{
	for (;;) {
		std::optional<Reader> notice;
		Proxy *proxy = nullptr;
		{
			const std::lock_guard guard(lock);
			if (notices.empty())
				return;
			notice.emplace(std::move(notices.front()));
			notices.pop_front();
			try {
				// Every notice names the object it is about first.
				Reader probe = *notice;
				const auto found = proxies.find(probe.u32());
				if (found != proxies.end()) {
					proxy = found->second;
					proxy->addRef();
				}
			} catch (const std::exception &) {
			}
		}
		if (!proxy)
			continue;
		try {
			notice->u32();
			proxy->tell(*notice);
		} catch (const std::exception &) {
		}
		proxy->release();
	}
}

// Hands a block to the Windows side and waits for it, without a call: the input goes into the processor's block, the count of blocks is
// raised and its word woken, and the output is taken once the other side copied the count to `done`.
tresult PLUGIN_API Proxy::process(ProcessData &data)
{
	if (!block)
		return kNotInitialized;
	if (data.numInputs < 0 || data.numOutputs < 0 || uint32_t(data.numInputs) > Block::buses || uint32_t(data.numOutputs) > Block::buses ||
		data.numSamples < 0 || data.numSamples > setup.maxSamplesPerBlock || data.symbolicSampleSize != setup.symbolicSampleSize)
		return kInvalidArgument;
	const size_t sample = data.symbolicSampleSize == kSample64 ? 8 : 4;
	uint64_t channels = 0;
	for (int32 bus = 0; bus < data.numInputs; ++bus)
		channels += uint64_t(std::max(0, data.inputs[bus].numChannels));
	for (int32 bus = 0; bus < data.numOutputs; ++bus)
		channels += uint64_t(std::max(0, data.outputs[bus].numChannels));
	if (block_size(setup, uint32_t(channels)) > block_room)
		return kInvalidArgument;
	Block &shared = *block;
	shared.mode = data.processMode;
	shared.sample_size = data.symbolicSampleSize;
	shared.samples = data.numSamples;
	shared.has_context = data.processContext != nullptr;
	if (data.processContext)
		shared.context = *data.processContext;
	shared.input_buses = uint32_t(data.numInputs);
	shared.output_buses = uint32_t(data.numOutputs);
	uint8_t *const audio = block_audio(block);
	const size_t stride = size_t(setup.maxSamplesPerBlock) * sample, length = size_t(data.numSamples) * sample;
	size_t channel = 0;
	for (int32 bus = 0; bus < data.numInputs; ++bus) {
		const AudioBusBuffers &buffers = data.inputs[bus];
		shared.input_channels[bus] = buffers.numChannels;
		shared.input_silence[bus] = buffers.silenceFlags;
		for (int32 at = 0; at < buffers.numChannels; ++at, ++channel) {
			const void *source = sample == 8 ? static_cast<const void *>(buffers.channelBuffers64[at]) : buffers.channelBuffers32[at];
			if (source)
				std::memcpy(audio + channel * stride, source, length);
			else
				std::memset(audio + channel * stride, 0, length);
		}
	}
	for (int32 bus = 0; bus < data.numOutputs; ++bus)
		shared.output_channels[bus] = data.outputs[bus].numChannels;
	if (data.inputParameterChanges) {
		if (!ParameterChanges::put(*data.inputParameterChanges, shared.input_changes, Block::points, shared.input_points))
			return kOutOfMemory;
	} else
		shared.input_points = 0;
	if (data.inputEvents) {
		if (!put_events(*data.inputEvents, shared.input_event, Block::events, shared.input_events, shared.input_event_data, shared.input_event_bytes))
			return kOutOfMemory;
	} else
		shared.input_events = 0;
	const uint32_t asked = shared.request.load(std::memory_order_relaxed) + 1;
	shared.request.store(asked, std::memory_order_release);
	wake(shared.request);
	// The block comes back within a few milliseconds; a Windows side that stopped answering gives silence instead of holding the host's
	// audio thread.
	const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	for (uint32_t seen; (seen = shared.done.load(std::memory_order_acquire)) != asked;) {
		if (std::chrono::steady_clock::now() >= limit || !Bridge::instance().running()) {
			for (int32 bus = 0; bus < data.numOutputs; ++bus)
				for (int32 at = 0; at < data.outputs[bus].numChannels; ++at)
					if (void *target = sample == 8 ? static_cast<void *>(data.outputs[bus].channelBuffers64[at]) : data.outputs[bus].channelBuffers32[at])
						std::memset(target, 0, length);
			return kResultFalse;
		}
		const timespec slice{0, 100'000'000};
		::syscall(SYS_futex, &shared.done, FUTEX_WAIT, seen, &slice, nullptr, 0);
	}
	for (int32 bus = 0; bus < data.numOutputs; ++bus) {
		AudioBusBuffers &buffers = data.outputs[bus];
		buffers.silenceFlags = shared.output_silence[bus];
		for (int32 at = 0; at < buffers.numChannels; ++at, ++channel)
			if (void *target = sample == 8 ? static_cast<void *>(buffers.channelBuffers64[at]) : buffers.channelBuffers32[at])
				std::memcpy(target, audio + channel * stride, length);
	}
	if (data.outputParameterChanges)
		for (uint32_t at = 0; at < std::min(shared.output_points, Block::points); ++at) {
			int32 index = 0;
			if (IParamValueQueue *queue = data.outputParameterChanges->addParameterData(shared.output_changes[at].id, index))
				queue->addPoint(shared.output_changes[at].offset, shared.output_changes[at].value, index);
		}
	if (data.outputEvents) {
		take_events(*input_events, shared.output_event, std::min(shared.output_events, Block::events), shared.output_event_data,
			std::min(shared.output_event_bytes, Block::event_bytes));
		for (auto &event : input_events->events)
			data.outputEvents->addEvent(event);
	}
	return from_wire(Result(shared.result));
}

class Factory : public Counted<IPluginFactory3, IPluginFactory2, IPluginFactory> {
public:
	tresult PLUGIN_API getFactoryInfo(PFactoryInfo *info) override
	{
		if (!info)
			return kInvalidArgument;
		*info = Bridge::instance().factory;
		return kResultOk;
	}
	int32 PLUGIN_API countClasses() override { return int32(Bridge::instance().classes.size()); }
	tresult PLUGIN_API getClassInfo(int32 index, PClassInfo *info) override
	{
		const PClassInfo2 *more = described(index);
		if (!more || !info)
			return kInvalidArgument;
		*info = PClassInfo(more->cid, more->cardinality, more->category, more->name);
		return kResultOk;
	}
	tresult PLUGIN_API getClassInfo2(int32 index, PClassInfo2 *info) override
	{
		const PClassInfo2 *more = described(index);
		if (!more || !info)
			return kInvalidArgument;
		*info = *more;
		return kResultOk;
	}
	tresult PLUGIN_API getClassInfoUnicode(int32 index, PClassInfoW *info) override
	{
		const PClassInfo2 *more = described(index);
		if (!more || !info)
			return kInvalidArgument;
		*info = PClassInfoW();
		std::memcpy(info->cid, more->cid, sizeof info->cid);
		info->cardinality = more->cardinality;
		std::memcpy(info->category, more->category, sizeof info->category);
		info->classFlags = more->classFlags;
		std::memcpy(info->subCategories, more->subCategories, sizeof info->subCategories);
		widen(info->name, more->name, PClassInfo::kNameSize);
		widen(info->vendor, more->vendor, PClassInfo2::kVendorSize);
		widen(info->version, more->version, PClassInfo2::kVersionSize);
		widen(info->sdkVersion, more->sdkVersion, PClassInfo2::kVersionSize);
		return kResultOk;
	}
	tresult PLUGIN_API setHostContext(FUnknown *) override { return kResultOk; }
	tresult PLUGIN_API createInstance(FIDString cid, FIDString iid, void **object) override
	{
		*object = nullptr;
		auto &bridge = Bridge::instance();
		const auto found = std::find_if(bridge.classes.begin(), bridge.classes.end(), [cid](const auto &entry) { return same(entry.second.cid, cid); });
		if (found == bridge.classes.end())
			return kInvalidArgument;
		const uint32_t wanted = same(iid, IEditController::iid) ? has_controller : same(iid, IAudioProcessor::iid) ? has_processor :
			same(iid, IComponent::iid) ? has_component : 0;
		auto answer = ask(Writer(uint32_t(Call::create)).id(found->first).u32(wanted));
		if (!answer)
			return kInternalError;
		const uint32_t made = answer->u32(), interfaces = answer->u32();
		if (!made)
			return kNoInterface;
		auto *const proxy = new Proxy(made, interfaces);
		const tresult result = proxy->queryInterface(iid, object);
		proxy->release();
		return result;
	}

private:
	static const PClassInfo2 *described(int32 index)
	{
		const auto &classes = Bridge::instance().classes;
		return index >= 0 && size_t(index) < classes.size() ? &classes[size_t(index)].second : nullptr;
	}
	static void widen(char16 *target, const char8 *source, size_t capacity)
	{
		size_t at = 0;
		for (; at + 1 < capacity && source[at]; ++at)
			target[at] = char16(uint8_t(source[at]));
		target[at] = 0;
	}
};

}

extern "C" {

SMTG_EXPORT_SYMBOL bool ModuleEntry(void *) { return true; }
SMTG_EXPORT_SYMBOL bool ModuleExit() { return true; }

SMTG_EXPORT_SYMBOL IPluginFactory *PLUGIN_API GetPluginFactory()
{
	if (!Bridge::instance().running())
		return nullptr;
	return new Factory;
}

}
