// A VST 3 plugin for Windows as a host meets one, built on the interface headers alone: a factory with a processor class and the edit
// controller it names, one stereo bus each way, one parameter. The processor multiplies its input by twice the parameter's normalized
// value, takes the last point of each block's changes for it, and keeps the value in its state, which the controller reads back. The
// controller's editor is a green window of 300 by 200 pixels; a click on it sets the gain to 0.75 as a host hears of an edit.

#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

#include <atomic>
#include <cstring>

#include <windows.h>

using namespace Steinberg;
using namespace Steinberg::Vst;

// The identifiers of the Vst interfaces, which the SDK's helper classes define and the headers only declare; coreiids.cpp has the others.
namespace Steinberg {
DEF_CLASS_IID(IPlugView)
DEF_CLASS_IID(IPlugFrame)
}
namespace Steinberg::Vst {
DEF_CLASS_IID(IComponent)
DEF_CLASS_IID(IAudioProcessor)
DEF_CLASS_IID(IEditController)
DEF_CLASS_IID(IComponentHandler)
DEF_CLASS_IID(IParameterChanges)
DEF_CLASS_IID(IParamValueQueue)
}

static const FUID processor_id(0x52a7e1c3, 0x0b4d4f21, 0x9a6e5c8d, 0x17f3b2a4);
static const FUID controller_id(0x52a7e1c3, 0x0b4d4f21, 0x9a6e5c8d, 0x17f3b2a5);
static constexpr ParamID gain_id = 0;

static void copy_text(char16 *target, const char16 *text, size_t capacity)
{
	size_t at = 0;
	for (; text[at] && at + 1 < capacity; ++at)
		target[at] = text[at];
	target[at] = 0;
}

template <typename Self>
class Counted : public Self {
public:
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
	tresult answer(void *self, void **object)
	{
		addRef();
		*object = self;
		return kResultOk;
	}

private:
	std::atomic<uint32> references{1};
};

static bool read_gain(IBStream *stream, double &value)
{
	double read = 0;
	int32 count = 0;
	if (!stream || stream->read(&read, sizeof read, &count) != kResultOk || count != sizeof read || !(read >= 0 && read <= 1))
		return false;
	value = read;
	return true;
}

class Processor : public Counted<IComponent>, public IAudioProcessor {
public:
	tresult PLUGIN_API queryInterface(const TUID iid, void **object) override
	{
		if (FUnknownPrivate::iidEqual(iid, FUnknown::iid) || FUnknownPrivate::iidEqual(iid, IPluginBase::iid) ||
			FUnknownPrivate::iidEqual(iid, IComponent::iid))
			return answer(static_cast<IComponent *>(this), object);
		if (FUnknownPrivate::iidEqual(iid, IAudioProcessor::iid))
			return answer(static_cast<IAudioProcessor *>(this), object);
		*object = nullptr;
		return kNoInterface;
	}
	uint32 PLUGIN_API addRef() override { return Counted::addRef(); }
	uint32 PLUGIN_API release() override { return Counted::release(); }

	tresult PLUGIN_API initialize(FUnknown *) override { return kResultOk; }
	tresult PLUGIN_API terminate() override { return kResultOk; }
	tresult PLUGIN_API getControllerClassId(TUID id) override
	{
		controller_id.toTUID(id);
		return kResultOk;
	}
	tresult PLUGIN_API setIoMode(IoMode) override { return kResultOk; }
	int32 PLUGIN_API getBusCount(MediaType type, BusDirection) override { return type == kAudio ? 1 : 0; }
	tresult PLUGIN_API getBusInfo(MediaType type, BusDirection direction, int32 index, BusInfo &bus) override
	{
		if (type != kAudio || index != 0)
			return kInvalidArgument;
		bus.mediaType = kAudio;
		bus.direction = direction;
		bus.channelCount = 2;
		copy_text(bus.name, direction == kInput ? u"Input" : u"Output", 128);
		bus.busType = kMain;
		bus.flags = BusInfo::kDefaultActive;
		return kResultOk;
	}
	tresult PLUGIN_API getRoutingInfo(RoutingInfo &, RoutingInfo &) override { return kNotImplemented; }
	tresult PLUGIN_API activateBus(MediaType type, BusDirection, int32 index, TBool) override
	{
		return type == kAudio && index == 0 ? kResultOk : kInvalidArgument;
	}
	tresult PLUGIN_API setActive(TBool) override { return kResultOk; }
	tresult PLUGIN_API setState(IBStream *state) override { return read_gain(state, gain) ? kResultOk : kResultFalse; }
	tresult PLUGIN_API getState(IBStream *state) override
	{
		int32 count = 0;
		return state && state->write(&gain, sizeof gain, &count) == kResultOk && count == sizeof gain ? kResultOk : kResultFalse;
	}

	tresult PLUGIN_API setBusArrangements(SpeakerArrangement *inputs, int32 inputCount, SpeakerArrangement *outputs, int32 outputCount) override
	{
		return inputCount == 1 && outputCount == 1 && inputs[0] == SpeakerArr::kStereo && outputs[0] == SpeakerArr::kStereo ? kResultTrue : kResultFalse;
	}
	tresult PLUGIN_API getBusArrangement(BusDirection, int32 index, SpeakerArrangement &arrangement) override
	{
		if (index != 0)
			return kInvalidArgument;
		arrangement = SpeakerArr::kStereo;
		return kResultOk;
	}
	tresult PLUGIN_API canProcessSampleSize(int32 size) override { return size == kSample32 ? kResultTrue : kResultFalse; }
	uint32 PLUGIN_API getLatencySamples() override { return 0; }
	tresult PLUGIN_API setupProcessing(ProcessSetup &setup) override { return setup.symbolicSampleSize == kSample32 ? kResultOk : kResultFalse; }
	tresult PLUGIN_API setProcessing(TBool) override { return kResultOk; }
	tresult PLUGIN_API process(ProcessData &data) override
	{
		if (data.inputParameterChanges)
			for (int32 index = 0; index < data.inputParameterChanges->getParameterCount(); ++index) {
				IParamValueQueue *queue = data.inputParameterChanges->getParameterData(index);
				int32 offset = 0;
				ParamValue value = 0;
				if (queue && queue->getParameterId() == gain_id && queue->getPointCount() > 0 &&
					queue->getPoint(queue->getPointCount() - 1, offset, value) == kResultOk)
					gain = value;
			}
		if (data.numInputs < 1 || data.numOutputs < 1 || data.symbolicSampleSize != kSample32)
			return kResultOk;
		const AudioBusBuffers &in = data.inputs[0];
		AudioBusBuffers &out = data.outputs[0];
		for (int32 channel = 0; channel < out.numChannels; ++channel) {
			const float *source = channel < in.numChannels ? in.channelBuffers32[channel] : nullptr;
			float *target = out.channelBuffers32[channel];
			for (int32 at = 0; at < data.numSamples; ++at)
				target[at] = source ? float(source[at] * gain * 2) : 0.0f;
		}
		out.silenceFlags = 0;
		return kResultOk;
	}
	uint32 PLUGIN_API getTailSamples() override { return kNoTail; }

private:
	double gain = 0.5;
};

class Controller;

// The editor: a child of the window the host attaches it to, which tells the controller of a click.
class Editor : public Counted<IPlugView> {
public:
	explicit Editor(Controller &controller);
	tresult PLUGIN_API queryInterface(const TUID iid, void **object) override
	{
		if (FUnknownPrivate::iidEqual(iid, FUnknown::iid) || FUnknownPrivate::iidEqual(iid, IPlugView::iid))
			return answer(static_cast<IPlugView *>(this), object);
		*object = nullptr;
		return kNoInterface;
	}
	tresult PLUGIN_API isPlatformTypeSupported(FIDString type) override { return std::strcmp(type, kPlatformTypeHWND) ? kResultFalse : kResultTrue; }
	tresult PLUGIN_API attached(void *parent, FIDString type) override
	{
		if (isPlatformTypeSupported(type) != kResultTrue || !parent || window)
			return kResultFalse;
		static const ATOM kind = [] {
			WNDCLASSW made{};
			made.lpfnWndProc = procedure;
			made.hInstance = GetModuleHandleW(nullptr);
			made.lpszClassName = L"RavenGainEditor";
			return RegisterClassW(&made);
		}();
		window = CreateWindowExW(0, MAKEINTATOM(kind), L"", WS_CHILD | WS_VISIBLE, 0, 0, 300, 200, static_cast<HWND>(parent), nullptr,
			GetModuleHandleW(nullptr), nullptr);
		if (!window)
			return kResultFalse;
		SetWindowLongPtrW(window, GWLP_USERDATA, LONG_PTR(this));
		return kResultTrue;
	}
	tresult PLUGIN_API removed() override
	{
		if (window)
			DestroyWindow(window);
		window = nullptr;
		return kResultOk;
	}
	tresult PLUGIN_API onWheel(float) override { return kResultFalse; }
	tresult PLUGIN_API onKeyDown(char16, int16, int16) override { return kResultFalse; }
	tresult PLUGIN_API onKeyUp(char16, int16, int16) override { return kResultFalse; }
	tresult PLUGIN_API getSize(ViewRect *size) override
	{
		if (!size)
			return kInvalidArgument;
		*size = ViewRect(0, 0, 300, 200);
		return kResultOk;
	}
	tresult PLUGIN_API onSize(ViewRect *) override { return kResultOk; }
	tresult PLUGIN_API onFocus(TBool) override { return kResultOk; }
	tresult PLUGIN_API setFrame(IPlugFrame *) override { return kResultOk; }
	tresult PLUGIN_API canResize() override { return kResultFalse; }
	tresult PLUGIN_API checkSizeConstraint(ViewRect *) override { return kResultFalse; }

protected:
	~Editor() override;

private:
	static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
	Controller &controller;
	HWND window = nullptr;
};

class Controller : public Counted<IEditController> {
public:
	tresult PLUGIN_API queryInterface(const TUID iid, void **object) override
	{
		if (FUnknownPrivate::iidEqual(iid, FUnknown::iid) || FUnknownPrivate::iidEqual(iid, IPluginBase::iid) ||
			FUnknownPrivate::iidEqual(iid, IEditController::iid))
			return answer(static_cast<IEditController *>(this), object);
		*object = nullptr;
		return kNoInterface;
	}

	tresult PLUGIN_API initialize(FUnknown *) override { return kResultOk; }
	tresult PLUGIN_API terminate() override
	{
		if (handler)
			handler->release();
		handler = nullptr;
		return kResultOk;
	}
	tresult PLUGIN_API setComponentState(IBStream *state) override { return read_gain(state, gain) ? kResultOk : kResultFalse; }
	tresult PLUGIN_API setState(IBStream *) override { return kResultOk; }
	tresult PLUGIN_API getState(IBStream *) override { return kResultOk; }
	int32 PLUGIN_API getParameterCount() override { return 1; }
	tresult PLUGIN_API getParameterInfo(int32 index, ParameterInfo &info) override
	{
		if (index != 0)
			return kInvalidArgument;
		std::memset(&info, 0, sizeof info);
		info.id = gain_id;
		copy_text(info.title, u"Gain", 128);
		copy_text(info.shortTitle, u"Gain", 128);
		copy_text(info.units, u"x", 128);
		info.defaultNormalizedValue = 0.5;
		info.flags = ParameterInfo::kCanAutomate;
		return kResultOk;
	}
	tresult PLUGIN_API getParamStringByValue(ParamID id, ParamValue value, String128 text) override
	{
		if (id != gain_id)
			return kInvalidArgument;
		const int hundredths = int(value * 200 + 0.5);
		char16 digits[8] = {char16(u'0' + hundredths / 100), u'.', char16(u'0' + hundredths / 10 % 10), char16(u'0' + hundredths % 10), 0};
		copy_text(text, digits, 128);
		return kResultOk;
	}
	tresult PLUGIN_API getParamValueByString(ParamID, TChar *, ParamValue &) override { return kNotImplemented; }
	ParamValue PLUGIN_API normalizedParamToPlain(ParamID, ParamValue value) override { return value * 2; }
	ParamValue PLUGIN_API plainParamToNormalized(ParamID, ParamValue value) override { return value / 2; }
	ParamValue PLUGIN_API getParamNormalized(ParamID id) override { return id == gain_id ? gain : 0; }
	tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue value) override
	{
		if (id != gain_id || !(value >= 0 && value <= 1))
			return kInvalidArgument;
		gain = value;
		return kResultOk;
	}
	tresult PLUGIN_API setComponentHandler(IComponentHandler *given) override
	{
		if (given)
			given->addRef();
		if (handler)
			handler->release();
		handler = given;
		return kResultOk;
	}
	IPlugView *PLUGIN_API createView(FIDString name) override
	{
		return name && !std::strcmp(name, ViewType::kEditor) ? new Editor(*this) : nullptr;
	}
	// A click on the editor: the gain becomes 0.75, which the host hears of as an edit of the parameter.
	void clicked()
	{
		gain = 0.75;
		if (!handler)
			return;
		handler->beginEdit(gain_id);
		handler->performEdit(gain_id, gain);
		handler->endEdit(gain_id);
	}

private:
	double gain = 0.5;
	IComponentHandler *handler = nullptr;
};

Editor::Editor(Controller &controller) : controller(controller) { controller.addRef(); }

Editor::~Editor()
{
	removed();
	controller.release();
}

LRESULT CALLBACK Editor::procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	auto *const editor = reinterpret_cast<Editor *>(GetWindowLongPtrW(window, GWLP_USERDATA));
	if (message == WM_PAINT) {
		PAINTSTRUCT paint;
		const HDC context = BeginPaint(window, &paint);
		const HBRUSH green = CreateSolidBrush(RGB(0, 255, 0));
		RECT area;
		GetClientRect(window, &area);
		FillRect(context, &area, green);
		DeleteObject(green);
		EndPaint(window, &paint);
		return 0;
	}
	if (message == WM_LBUTTONDOWN && editor) {
		editor->controller.clicked();
		return 0;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

class Factory : public IPluginFactory {
public:
	tresult PLUGIN_API queryInterface(const TUID iid, void **object) override
	{
		if (FUnknownPrivate::iidEqual(iid, FUnknown::iid) || FUnknownPrivate::iidEqual(iid, IPluginFactory::iid)) {
			*object = this;
			return kResultOk;
		}
		*object = nullptr;
		return kNoInterface;
	}
	uint32 PLUGIN_API addRef() override { return 1; }
	uint32 PLUGIN_API release() override { return 1; }
	tresult PLUGIN_API getFactoryInfo(PFactoryInfo *info) override
	{
		*info = PFactoryInfo("Raven", "", "", PFactoryInfo::kUnicode);
		return kResultOk;
	}
	int32 PLUGIN_API countClasses() override { return 2; }
	tresult PLUGIN_API getClassInfo(int32 index, PClassInfo *info) override
	{
		if (index == 0)
			*info = PClassInfo(processor_id.toTUID(), PClassInfo::kManyInstances, kVstAudioEffectClass, "Raven Gain");
		else if (index == 1)
			*info = PClassInfo(controller_id.toTUID(), PClassInfo::kManyInstances, kVstComponentControllerClass, "Raven Gain Controller");
		else
			return kInvalidArgument;
		return kResultOk;
	}
	tresult PLUGIN_API createInstance(FIDString cid, FIDString iid, void **object) override
	{
		*object = nullptr;
		FUnknown *made = nullptr;
		if (FUnknownPrivate::iidEqual(cid, processor_id.toTUID()))
			made = static_cast<IComponent *>(new Processor);
		else if (FUnknownPrivate::iidEqual(cid, controller_id.toTUID()))
			made = new Controller;
		if (!made)
			return kNoInterface;
		const tresult result = made->queryInterface(iid, object);
		made->release();
		return result;
	}
};

extern "C" __declspec(dllexport) bool InitDll() { return true; }
extern "C" __declspec(dllexport) bool ExitDll() { return true; }
extern "C" __declspec(dllexport) IPluginFactory *PLUGIN_API GetPluginFactory()
{
	static Factory factory;
	return &factory;
}
