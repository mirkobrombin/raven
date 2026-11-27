// A Linux host for a bridged plugin (plugin.cpp), as a DAW meets one: loads its module, reads the factory's classes, makes the processor
// and the controller it names, hands the processor's state to the controller, and processes blocks, the first with a change of the
// gain parameter. Checks what the test plugin (testdata/native_vst/gain_plugin.cpp) gives back at each step, then times a run of blocks
// paced as audio comes and prints how long each took to come back.
//
// usage: bridge_test MODULE [BLOCKS [GAP_MICROSECONDS]]

#include "guest/raven-vsthost/bridge_objects.h"

#include "pluginterfaces/vst/ivsthostapplication.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#include <dlfcn.h>

using namespace raven_vst;

namespace {

void check(bool value, const char *message)
{
	if (!value)
		throw std::runtime_error(message);
}

template <typename Interface>
Interface *query(FUnknown *object)
{
	void *found = nullptr;
	if (!object || object->queryInterface(Interface::iid, &found) != kResultOk)
		return nullptr;
	return static_cast<Interface *>(found);
}

class Host : public Counted<IHostApplication> {
public:
	tresult PLUGIN_API getName(String128 name) override
	{
		const char16 own[] = u"Raven bridge test";
		std::memcpy(name, own, sizeof own);
		return kResultOk;
	}
	tresult PLUGIN_API createInstance(TUID, TUID, void **object) override
	{
		*object = nullptr;
		return kResultFalse;
	}
};

// Counts what the controller tells the host.
class Handler : public Counted<IComponentHandler> {
public:
	tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
	tresult PLUGIN_API performEdit(ParamID, ParamValue) override { return kResultOk; }
	tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }
	tresult PLUGIN_API restartComponent(int32) override { return kResultOk; }
};

std::string narrow(const char16 *text)
{
	std::string made;
	for (; *text; ++text)
		made.push_back(char(*text));
	return made;
}

}

int main(int count, char **arguments)
{
	if (count < 2) {
		std::fputs("usage: bridge_test MODULE [BLOCKS [GAP_MICROSECONDS]]\n", stderr);
		return 2;
	}
	const int blocks = count > 2 ? std::atoi(arguments[2]) : 1000;
	const auto gap = std::chrono::microseconds(count > 3 ? std::atoi(arguments[3]) : 0);
	try {
		void *const module = ::dlopen(arguments[1], RTLD_NOW | RTLD_LOCAL);
		check(module, "the module does not load");
		using Entry = bool (*)(void *);
		using Get = IPluginFactory *(PLUGIN_API *)();
		const auto entry = reinterpret_cast<Entry>(::dlsym(module, "ModuleEntry"));
		const auto get = reinterpret_cast<Get>(::dlsym(module, "GetPluginFactory"));
		check(entry && get && entry(module), "the module has no entry");
		IPluginFactory *const factory = get();
		check(factory, "the module has no factory");

		IPluginFactory2 *const second = query<IPluginFactory2>(factory);
		check(second, "the factory has no IPluginFactory2");
		TUID processor_id{};
		bool found = false;
		for (int32 index = 0; index < factory->countClasses(); ++index) {
			PClassInfo2 info{};
			check(second->getClassInfo2(index, &info) == kResultOk, "a class has no description");
			std::printf("class: %s, %s, %s\n", info.category, info.subCategories, info.name);
			if (!std::strcmp(info.category, kVstAudioEffectClass)) {
				std::memcpy(processor_id, info.cid, sizeof processor_id);
				found = true;
			}
		}
		second->release();
		check(found, "no audio effect class");

		auto *const host = new Host;
		IComponent *component = nullptr;
		check(factory->createInstance(processor_id, IComponent::iid, reinterpret_cast<void **>(&component)) == kResultOk && component,
			"the processor is not made");
		check(component->initialize(host) == kResultOk, "the processor does not initialize");
		IAudioProcessor *const processor = query<IAudioProcessor>(component);
		check(processor, "the processor has no IAudioProcessor");
		TUID controller_id{};
		check(component->getControllerClassId(controller_id) == kResultOk, "no controller class");
		IEditController *controller = nullptr;
		check(factory->createInstance(controller_id, IEditController::iid, reinterpret_cast<void **>(&controller)) == kResultOk && controller,
			"the controller is not made");
		check(controller->initialize(host) == kResultOk, "the controller does not initialize");
		auto *const handler = new Handler;
		check(controller->setComponentHandler(handler) == kResultOk, "the controller takes no handler");

		check(component->getBusCount(kAudio, kInput) == 1 && component->getBusCount(kAudio, kOutput) == 1, "not one bus each way");
		BusInfo bus{};
		check(component->getBusInfo(kAudio, kOutput, 0, bus) == kResultOk && bus.channelCount == 2, "the output bus is not stereo");
		check(controller->getParameterCount() == 1, "not one parameter");
		ParameterInfo parameter{};
		check(controller->getParameterInfo(0, parameter) == kResultOk, "no parameter description");
		std::printf("parameter: %u %s\n", parameter.id, narrow(parameter.title).c_str());
		String128 text{};
		check(controller->getParamStringByValue(parameter.id, 0.25, text) == kResultOk, "no text for a value");
		std::printf("text of 0.25: %s\n", narrow(text).c_str());

		SpeakerArrangement stereo = SpeakerArr::kStereo;
		check(processor->setBusArrangements(&stereo, 1, &stereo, 1) == kResultOk, "stereo is refused");
		ProcessSetup setup{kRealtime, kSample32, 512, 48000};
		check(processor->setupProcessing(setup) == kResultOk, "the processing is not set up");
		check(component->activateBus(kAudio, kInput, 0, true) == kResultOk && component->activateBus(kAudio, kOutput, 0, true) == kResultOk,
			"the buses do not activate");
		check(component->setActive(true) == kResultOk && processor->setProcessing(true) == kResultOk, "the processor does not start");

		float left_in[512], right_in[512], left_out[512], right_out[512];
		for (int at = 0; at < 512; ++at)
			left_in[at] = right_in[at] = 1.0f;
		float *inputs[] = {left_in, right_in}, *outputs[] = {left_out, right_out};
		AudioBusBuffers input, output;
		input.numChannels = output.numChannels = 2;
		input.channelBuffers32 = inputs;
		output.channelBuffers32 = outputs;
		auto *const changes = new ParameterChanges;
		ProcessContext context{};
		context.sampleRate = 48000;
		ProcessData data;
		data.processMode = kRealtime;
		data.symbolicSampleSize = kSample32;
		data.numSamples = 512;
		data.numInputs = data.numOutputs = 1;
		data.inputs = &input;
		data.outputs = &output;
		data.inputParameterChanges = changes;
		data.processContext = &context;

		// The first block sets the gain to 0.25, which doubled gives half the input.
		int32 index = 0;
		changes->addParameterData(parameter.id, index)->addPoint(0, 0.25, index);
		check(processor->process(data) == kResultOk, "the first block fails");
		std::printf("first block: %.3f %.3f\n", left_out[0], right_out[511]);
		check(std::fabs(left_out[0] - 0.5f) < 1e-6f && std::fabs(right_out[511] - 0.5f) < 1e-6f, "the gain was not applied");
		changes->clear();

		// The processor's state now holds the gain, which the controller then shows.
		auto *const state = new MemoryStream;
		check(component->getState(state) == kResultOk && state->data.size() == sizeof(double), "no state");
		state->seek(0, IBStream::kIBSeekSet, nullptr);
		check(controller->setComponentState(state) == kResultOk, "the controller refuses the state");
		std::printf("controller gain: %.3f\n", controller->getParamNormalized(parameter.id));
		check(std::fabs(controller->getParamNormalized(parameter.id) - 0.25) < 1e-9, "the controller did not take the state");
		state->release();

		std::vector<double> took;
		took.reserve(size_t(blocks));
		for (int block = 0; block < blocks; ++block) {
			if (gap.count())
				std::this_thread::sleep_for(gap);
			const auto start = std::chrono::steady_clock::now();
			check(processor->process(data) == kResultOk, "a block fails");
			took.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count());
		}
		check(std::fabs(left_out[100] - 0.5f) < 1e-6f, "a later block lost the gain");
		std::sort(took.begin(), took.end());
		if (!took.empty())
			std::printf("blocks: %d, median %.1f us, 99th percentile %.1f us, longest %.1f us\n", blocks, took[took.size() / 2],
				took[took.size() * 99 / 100], took.back());

		check(processor->setProcessing(false) == kResultOk && component->setActive(false) == kResultOk, "the processor does not stop");
		controller->setComponentHandler(nullptr);
		controller->terminate();
		component->terminate();
		changes->release();
		processor->release();
		controller->release();
		component->release();
		handler->release();
		host->release();
		factory->release();
		using Exit = bool (*)();
		if (const auto exit = reinterpret_cast<Exit>(::dlsym(module, "ModuleExit")))
			exit();
		::dlclose(module);
		std::puts("bridge: done");
	} catch (const std::exception &error) {
		std::fprintf(stderr, "bridge_test: %s\n", error.what());
		return 1;
	}
	return 0;
}
