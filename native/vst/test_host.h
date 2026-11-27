// What the editor tests (editor_test.cpp, wayland_editor_test.cpp) have of a host: the run loop its frames bring, a frame, and a
// component handler that keeps the edits it is told of.

#pragma once

#include "guest/raven-vsthost/bridge_objects.h"

#include "pluginterfaces/gui/iplugview.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <vector>

#include <poll.h>

namespace raven_vst::test {

inline void check(bool value, const char *message)
{
	if (!value)
		throw std::runtime_error(message);
}

// The run loop a host's frame brings: the descriptors the plugin registers are looked at while the host waits.
class RunLoop : public Counted<Linux::IRunLoop> {
public:
	tresult PLUGIN_API registerEventHandler(Linux::IEventHandler *handler, Linux::FileDescriptor descriptor) override
	{
		if (!handler || descriptor < 0)
			return kInvalidArgument;
		handler->addRef();
		handlers.emplace(descriptor, handler);
		return kResultOk;
	}
	tresult PLUGIN_API unregisterEventHandler(Linux::IEventHandler *handler) override
	{
		for (auto at = handlers.begin(); at != handlers.end();)
			if (at->second == handler) {
				at->second->release();
				at = handlers.erase(at);
			} else
				++at;
		return kResultOk;
	}
	tresult PLUGIN_API registerTimer(Linux::ITimerHandler *, Linux::TimerInterval) override { return kNotImplemented; }
	tresult PLUGIN_API unregisterTimer(Linux::ITimerHandler *) override { return kNotImplemented; }
	// Runs what became ready within `slice`; `also` is watched with the handlers' descriptors, for the host's own connection.
	void run(std::chrono::milliseconds slice, int also = -1)
	{
		std::vector<pollfd> watched;
		for (const auto &[descriptor, handler] : handlers)
			watched.push_back({descriptor, POLLIN, 0});
		if (also >= 0)
			watched.push_back({also, POLLIN, 0});
		::poll(watched.data(), watched.size(), int(slice.count()));
		for (const auto &entry : watched)
			if (entry.revents)
				if (const auto found = handlers.find(entry.fd); found != handlers.end())
					found->second->onFDIsSet(entry.fd);
	}
	size_t count() const { return handlers.size(); }

private:
	std::multimap<int, Linux::IEventHandler *> handlers;
};

class Frame : public Counted<IPlugFrame> {
public:
	explicit Frame(RunLoop &loop) : loop(loop) {}
	tresult PLUGIN_API queryInterface(const TUID iid, void **object) override
	{
		if (same(iid, Linux::IRunLoop::iid))
			return loop.queryInterface(iid, object);
		return Counted::queryInterface(iid, object);
	}
	tresult PLUGIN_API resizeView(IPlugView *, ViewRect *) override { return kResultTrue; }

private:
	RunLoop &loop;
};

// Keeps the edits the controller tells the host of.
class Handler : public Counted<IComponentHandler> {
public:
	tresult PLUGIN_API beginEdit(ParamID) override
	{
		++begun;
		return kResultOk;
	}
	tresult PLUGIN_API performEdit(ParamID id, ParamValue value) override
	{
		edited = id;
		this->value = value;
		return kResultOk;
	}
	tresult PLUGIN_API endEdit(ParamID) override
	{
		++ended;
		return kResultOk;
	}
	tresult PLUGIN_API restartComponent(int32) override { return kResultOk; }
	int begun = 0, ended = 0;
	ParamID edited = UINT32_MAX;
	ParamValue value = -1;
};

// The first controller class a module's factory names.
inline bool controller_class(IPluginFactory &factory, TUID id)
{
	for (int32 index = 0; index < factory.countClasses(); ++index) {
		PClassInfo info{};
		if (factory.getClassInfo(index, &info) == kResultOk && !std::strcmp(info.category, kVstComponentControllerClass)) {
			std::memcpy(id, info.cid, sizeof(TUID));
			return true;
		}
	}
	return false;
}

}
