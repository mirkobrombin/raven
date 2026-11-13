#include "objects.h"
#include "event.h"
#include "api.h"
#include "handle_channel.h"
#include "shared_file.h"

#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <limits>

namespace raven {

static constexpr uint32_t invalid_handle=0xc0000008U, denied=0xc0000022U;

// A handle another process sent has not necessarily been taken in yet; looking its value up is the moment it must be.
void NativeObjects::arrive(uint64_t handle) const
{
	if (handle < handle_range_to_child)
		return;
	{
		std::lock_guard lock(mutex);
		if (handles.contains(handle))
			return;
	}
	receive_handles(const_cast<NativeObjects &>(*this));
}

uint64_t NativeObjects::take()
{
	while (handles.contains(next)) next += 4;
	const auto value = next;
	next += 4;
	static const bool traced = std::getenv("RAVEN_TRACE_HANDLES") != nullptr;
	if (traced) std::fprintf(stderr, "handle %llx made by %s\n", static_cast<unsigned long long>(value), api_in_progress);
	return value;
}

NativeObjectResult NativeObjects::create_event(uint32_t access, uint32_t type, bool signaled)
{
	if (type>1)
		return {0,0xc000000dU};
	const auto mapped=event_access(access);
	if (!mapped)
		return {0,0xc00000bbU};
	std::lock_guard lock(mutex);
	if (handles.size()>=limit || next>UINT32_MAX-4)
		return {0,0xc000009aU};
	const auto handle=take();
	handles.emplace(handle,Handle{std::make_shared<EventObject>(type,signaled),*mapped,{}});
	return {handle};
}

NativeObjectResult NativeObjects::create_named_event(const std::u16string &name, uint32_t access, uint32_t type, bool signaled, bool &existed)
{
	existed=false;
	if (type>1)
		return {0,0xc000000dU};
	const auto mapped=event_access(access);
	if (!mapped)
		return {0,0xc00000bbU};
	std::u16string key=name;
	for (auto &character : key) if (character>=u'a' && character<=u'z') character=char16_t(character-(u'a'-u'A'));
	std::lock_guard lock(mutex);
	if (handles.size()>=limit || next>UINT32_MAX-4)
		return {0,0xc000009aU};
	std::shared_ptr<EventObject> shared;
	if (const auto found=named_events.find(key); found!=named_events.end()) shared=found->second.lock();
	existed=shared!=nullptr;
	if (!shared) {
		shared=std::make_shared<EventObject>(type,signaled);
		named_events[key]=shared;
	}
	const auto handle=take();
	handles.emplace(handle,Handle{shared,*mapped,{}});
	return {handle};
}

NativeObjectResult NativeObjects::open_named_event(const std::u16string &name, uint32_t access)
{
	const auto mapped=event_access(access);
	if (!mapped)
		return {0,0xc00000bbU};
	std::u16string key=name;
	for (auto &character : key) if (character>=u'a' && character<=u'z') character=char16_t(character-(u'a'-u'A'));
	std::lock_guard lock(mutex);
	const auto found=named_events.find(key);
	const auto shared=found==named_events.end() ? nullptr : found->second.lock();
	if (!shared) return {0,0xc0000034U};
	if (handles.size()>=limit || next>UINT32_MAX-4)
		return {0,0xc000009aU};
	const auto handle=take();
	handles.emplace(handle,Handle{shared,*mapped,{}});
	return {handle};
}

std::pair<std::shared_ptr<EventObject>,uint32_t> NativeObjects::event(uint64_t handle, uint32_t access) const
{
	arrive(handle);
	std::lock_guard lock(mutex);
	const auto found=handles.find(handle);
	if (found==handles.end()) return {nullptr,invalid_handle};
	const auto object = found->second.event ? found->second.event : std::dynamic_pointer_cast<EventObject>(found->second.generic);
	if (!object) return {nullptr,0xc0000024U};
	if ((found->second.access & access)!=access) return {nullptr,denied};
	return {object,0};
}

NativeObjectResult NativeObjects::add_object(std::shared_ptr<KernelObject> object)
{
	std::lock_guard lock(mutex);
	if (handles.size()>=limit || next>UINT32_MAX-4)
		return {0,0xc000009aU};
	const auto handle=take();
	const uint32_t access=object->default_access();
	handles.emplace(handle,Handle{{},access,{},std::move(object)});
	return {handle};
}

std::shared_ptr<KernelObject> NativeObjects::object(uint64_t handle) const
{
	arrive(handle);
	std::lock_guard lock(mutex);
	const auto found=handles.find(handle);
	return found==handles.end() ? nullptr : found->second.generic ? found->second.generic : found->second.event;
}

int NativeObjects::signaled(uint64_t handle, uint32_t thread) const
{
	const auto held = object(handle);
	if (!held) {
		arrive(handle);
		std::lock_guard lock(mutex);
		return handles.contains(handle) ? -1 : -2;
	}
	if ((held->type_name() == u"Event" || held->type_name() == u"Mutant") && !(access_of(handle) & 0x100000))
		return -3;
	return held->signaled(thread);
}

int WaitedObjects::signaled(size_t index, uint32_t thread)
{
	if (held[index])
		return held[index]->signaled(thread);
	const int state = objects.signaled(handles[index], thread);
	if (state >= 0)
		held[index] = objects.object(handles[index]);
	return state;
}

bool WaitedObjects::try_consume(size_t index, uint32_t thread)
{
	return held[index] ? held[index]->try_consume(thread) : objects.try_consume(handles[index], thread);
}

void WaitedObjects::consume(size_t index, uint32_t thread)
{
	if (held[index])
		held[index]->consume(thread);
	else
		objects.consume(handles[index], thread);
}

std::optional<std::chrono::steady_clock::time_point> WaitedObjects::wakes_at(size_t index) const
{
	const auto object = held[index] ? held[index] : objects.object(handles[index]);
	return object ? object->wakes_at() : std::nullopt;
}

void NativeObjects::consume(uint64_t handle, uint32_t thread)
{
	if (const auto held = object(handle)) held->consume(thread);
}

bool NativeObjects::try_consume(uint64_t handle, uint32_t thread)
{
	const auto held = object(handle);
	return held && held->try_consume(thread);
}

NativeObjectResult NativeObjects::duplicate(uint64_t handle, std::optional<uint32_t> access)
{
	arrive(handle);
	std::lock_guard lock(mutex);
	const auto found=handles.find(handle);
	if (found==handles.end())
		return {0,invalid_handle};
	if (handles.size()>=limit || next>UINT32_MAX-4)
		return {0,0xc000009aU};
	const auto copy=take();
	// The copy is a new handle: it inherits only when the caller says so.
	auto duplicate_handle=found->second;
	duplicate_handle.inherit=false;
	if (access) duplicate_handle.access=*access;
	handles.emplace(copy,std::move(duplicate_handle));
	return {copy};
}

uint32_t NativeObjects::access_of(uint64_t handle) const
{
	arrive(handle);
	std::lock_guard lock(mutex);
	const auto found=handles.find(handle);
	return found==handles.end() ? 0 : found->second.access;
}

void NativeObjects::set_access(uint64_t handle, uint32_t access)
{
	arrive(handle);
	std::lock_guard lock(mutex);
	if (const auto found=handles.find(handle); found!=handles.end())
		found->second.access=access;
}

void NativeObjects::set_inherit(uint64_t handle, bool inherit)
{
	arrive(handle);
	std::lock_guard lock(mutex);
	if (const auto found=handles.find(handle); found!=handles.end())
		found->second.inherit=inherit;
}

bool NativeObjects::inherit(uint64_t handle) const
{
	arrive(handle);
	std::lock_guard lock(mutex);
	const auto found=handles.find(handle);
	return found!=handles.end() && found->second.inherit;
}

std::vector<uint64_t> NativeObjects::inheritable() const
{
	std::lock_guard lock(mutex);
	std::vector<uint64_t> result;
	for (const auto &[value,handle] : handles)
		if (handle.inherit)
			result.push_back(value);
	return result;
}

bool NativeObjects::adopt(uint64_t handle, std::shared_ptr<KernelObject> object, bool inherit)
{
	std::lock_guard lock(mutex);
	if (handles.contains(handle)) {
		if (std::getenv("RAVEN_TRACE_FAIL"))
			std::fprintf(stderr, "raven: incoming handle %llx is already in use\n", static_cast<unsigned long long>(handle));
		return false;
	}
	handles.emplace(handle,Handle{{},0x1f0003,{},std::move(object),inherit});
	return true;
}

uint32_t NativeObjects::close(uint64_t handle)
{
	arrive(handle);
	static const bool traced = std::getenv("RAVEN_TRACE_HANDLES") != nullptr;
	if (traced) std::fprintf(stderr, "handle %llx closed by %s\n", static_cast<unsigned long long>(handle), api_in_progress);
	std::shared_ptr<KernelObject> generic;
	{
		std::lock_guard lock(mutex);
		const auto found=handles.find(handle);
		if (found==handles.end()) return invalid_handle;
		generic=std::move(found->second.generic);
		handles.erase(found);
	}
	if (const auto reserved=std::dynamic_pointer_cast<ReservedObject>(generic); reserved && reserved->on_close)
		reserved->on_close(handle);
	return 0;
}

uint64_t NativeObjects::reserve(std::function<void(uint64_t)> on_close)
{
	auto object=std::make_shared<ReservedObject>();
	object->on_close=std::move(on_close);
	const auto made=add_object(object);
	return made.status ? 0 : made.value;
}

void NativeObjects::release_reserved(uint64_t handle)
{
	std::lock_guard lock(mutex);
	if (const auto found=handles.find(handle); found!=handles.end() && std::dynamic_pointer_cast<ReservedObject>(found->second.generic))
		handles.erase(found);
}

std::atomic<int> shared_object_count{0};

NativeObjectResult NativeObjects::set_event(uint64_t handle, bool signaled)
{
	const auto held = object(handle);
	// A file is a handle too, of a type that has no event state.
	if (!held) return {0,type_name(handle) ? 0xc0000024U : invalid_handle};
	if (held->type_name() == u"Event" && !(access_of(handle) & 2))
		return {0,denied};
	const auto previous = held->set_state(signaled);
	return previous >= 0 ? NativeObjectResult{uint64_t(previous)} : NativeObjectResult{0,0xc0000024U};
}

std::optional<std::u16string> NativeObjects::type_name(uint64_t handle) const
{
	arrive(handle);
	std::lock_guard lock(mutex);
	const auto found=handles.find(handle);
	if (found==handles.end()) return std::nullopt;
	if (found->second.event) return u"Event";
	if (found->second.file) return u"File";
	if (found->second.generic) return std::u16string(found->second.generic->type_name());
	return std::u16string();
}

NativeObjectResult NativeObjects::query_event(uint64_t handle)
{
	arrive(handle);
	const auto [object,status]=event(handle,1);
	if (status) return {0,status};
	std::lock_guard lock(object->mutex);
	return {uint64_t(object->type) | (uint64_t(object->is_set())<<32)};
}

NativeObjectResult NativeObjects::wait(uint64_t handle, std::optional<int64_t> timeout,
	std::chrono::steady_clock::time_point deadline)
{
	const auto [object,status]=event(handle,0x100000);
	if (status) return {0,status};
	if (timeout && *timeout>0) return {0,0xc00000bbU};
	std::unique_lock lock(object->mutex);
	if (!object->take()) {
		if (timeout && !*timeout) return {0,0x102};
		if (object->waiters.size()>=10000) return {0,0xc000009aU};
		const auto now=std::chrono::steady_clock::now();
		auto expires=std::chrono::steady_clock::time_point::max();
		if (timeout) {
			const auto ticks=uint64_t(-(*timeout+1))+1;
			const auto room=std::chrono::duration_cast<std::chrono::nanoseconds>(expires-now).count();
			if (ticks<=uint64_t(room)/100)
				expires=now+std::chrono::nanoseconds(ticks*100);
		}
		const auto waiter=std::make_shared<EventObject::Waiter>();
		object->waiters.push_back(waiter);
		const auto until = std::min(expires,deadline);
		while (!waiter->ready) {
			if (object->shared && object->take()) {
				std::erase(object->waiters,waiter);
				return {};
			}
			const auto now = std::chrono::steady_clock::now();
			if (now >= until) {
				std::erase(object->waiters,waiter);
				return expires<=deadline ? NativeObjectResult{0,0x102} : NativeObjectResult{0,0,true};
			}
			object->changed.wait_until(lock,object->shared ? std::min(until,now+std::chrono::milliseconds(5)) : until);
		}
		return {};
	}
	return {};
}

size_t NativeObjects::count() const
{
	std::lock_guard lock(mutex);
	return handles.size();
}

std::vector<uint64_t> NativeObjects::values() const
{
	std::lock_guard lock(mutex);
	std::vector<uint64_t> result;
	result.reserve(handles.size());
	for (const auto &[value, handle] : handles) result.push_back(value);
	return result;
}

size_t NativeObjects::waiting(uint64_t handle) const
{
	const auto [object,status]=event(handle,0);
	if (status) return 0;
	std::lock_guard lock(object->mutex);
	return object->waiters.size();
}

}
