#include "child_process.h"
#include "kernel_context.h"
#include "text.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace raven {

namespace {

constexpr uint32_t job_limit_kill_on_close = 0x2000, job_limit_active_process = 0x8;
constexpr uint32_t message_new_process = 6, message_exit_process = 7, message_active_process_zero = 4;
constexpr uint32_t info_basic_accounting = 1, info_basic_limit = 2, info_process_id_list = 3, info_basic_ui = 4, info_end_of_job_time = 6,
	info_associate_port = 7, info_basic_and_io = 8, info_extended_limit = 9, info_group = 11, info_notification_limit = 12;

// A job gathers processes so they can be limited and ended together. The limits are kept and reported back; the host enforces the ones that
// end processes (killing the members when the job closes, TerminateJobObject).
class JobObject : public KernelObject {
public:
	std::u16string_view type_name() const override { return u"Job"; }
	~JobObject() override
	{
		if (limit_flags & job_limit_kill_on_close)
			for (const auto &member : members)
				member->terminate(1);
	}
	std::vector<std::shared_ptr<ProcessObject>> members;
	uint32_t limit_flags = 0, active_limit = 0, ui_restrictions = 0, total_processes = 0;
	std::shared_ptr<CompletionPort> port;
	uint64_t key = 0;
	std::vector<uint8_t> extended;

	void notify(uint32_t message, uint32_t pid)
	{
		if (port)
			port->post({message, key, pid, 0});
	}
};

}

bool assign_to_job(KernelContext &context, uint64_t handle, const std::shared_ptr<ProcessObject> &process, uint32_t &error)
{
	const auto job = context.objects.object_as<JobObject>(handle);
	if (!job) {
		error = 6;
		return false;
	}
	if (std::find(job->members.begin(), job->members.end(), process) != job->members.end())
		return true;
	if ((job->limit_flags & job_limit_active_process) && job->members.size() >= job->active_limit) {
		error = 5;
		return false;
	}
	job->members.push_back(process);
	++job->total_processes;
	job->notify(message_new_process, uint32_t(process->pid));
	return true;
}

void register_job_api(ApiRegistry &registry, KernelContext &context)
{
	auto &objects = context.objects;
	const auto job_of = [&objects](uint64_t handle) { return objects.object_as<JobObject>(handle); };
	auto names = std::make_shared<std::map<std::u16string, std::weak_ptr<JobObject>>>();
	auto jobs = std::make_shared<std::vector<std::weak_ptr<JobObject>>>();

	const auto create = [&objects, names, jobs](ApiCall &call, bool wide) {
		std::u16string name;
		if (call[1])
			name = wide ? call.wide(call[1], 260) : from_utf8(call.narrow(call[1], 260));
		for (auto &character : name)
			if (character >= u'a' && character <= u'z')
				character = char16_t(character - (u'a' - u'A'));
		std::shared_ptr<JobObject> job;
		bool existed = false;
		if (!name.empty()) {
			if (const auto found = names->find(name); found != names->end())
				job = found->second.lock();
			existed = job != nullptr;
		}
		if (!job) {
			job = std::make_shared<JobObject>();
			if (!name.empty())
				(*names)[name] = job;
			jobs->push_back(job);
		}
		const auto made = objects.add_object(job);
		if (made.status) {
			call.fail(8, 0);
			return;
		}
		call.set_last_error(existed ? 183 : 0);
		call.ret(made.value);
	};
	registry.add("CreateJobObjectW", 2, [create](ApiCall &call) { create(call, true); });
	registry.add("CreateJobObjectA", 2, [create](ApiCall &call) { create(call, false); });
	registry.add("OpenJobObjectW", 3, [&objects, names](ApiCall &call) {
		auto name = call[2] ? call.wide(call[2], 260) : std::u16string();
		for (auto &character : name)
			if (character >= u'a' && character <= u'z')
				character = char16_t(character - (u'a' - u'A'));
		const auto found = names->find(name);
		const auto job = found == names->end() ? nullptr : found->second.lock();
		if (!job) {
			call.fail(2, 0);
			return;
		}
		const auto made = objects.add_object(job);
		if (made.status) {
			call.fail(8, 0);
			return;
		}
		call.ret(made.value);
	});
	// Members that have ended leave the job and are reported to the port the job is tied to.
	context.threads.add_pump([jobs] {
		for (const auto &weak : *jobs) {
			const auto job = weak.lock();
			if (!job)
				continue;
			for (size_t index = 0; index < job->members.size();) {
				if (!job->members[index]->exited()) {
					++index;
					continue;
				}
				const auto pid = uint32_t(job->members[index]->pid);
				job->members.erase(job->members.begin() + ptrdiff_t(index));
				job->notify(message_exit_process, pid);
				if (job->members.empty())
					job->notify(message_active_process_zero, 0);
			}
		}
	});
	registry.add("AssignProcessToJobObject", 2, [&context, &objects](ApiCall &call) {
		const auto process = objects.object_as<ProcessObject>(call[1]);
		if (!process) {
			// The program's own process joins the job without a host process to track.
			if (objects.object_as<JobObject>(call[0]) && (call[1] == UINT64_MAX || objects.object_as<SelfProcessObject>(call[1]))) {
				call.ret(1);
				return;
			}
			call.fail(objects.object_as<JobObject>(call[0]) ? 6 : 6);
			return;
		}
		uint32_t error = 0;
		if (!assign_to_job(context, call[0], process, error)) {
			call.fail(error);
			return;
		}
		call.ret(1);
	});
	registry.add("IsProcessInJob", 3, [&objects, job_of](ApiCall &call) {
		if (!call[2]) {
			call.fail(87);
			return;
		}
		const auto process = objects.object_as<ProcessObject>(call[0]);
		bool inside = false;
		if (process) {
			if (call[1]) {
				const auto job = job_of(call[1]);
				if (!job) {
					call.fail(6);
					return;
				}
				inside = std::find(job->members.begin(), job->members.end(), process) != job->members.end();
			}
		}
		call.store<uint32_t>(call[2], inside);
		call.ret(1);
	});
	registry.add("TerminateJobObject", 2, [job_of](ApiCall &call) {
		const auto job = job_of(call[0]);
		if (!job) {
			call.fail(6);
			return;
		}
		for (const auto &member : job->members)
			member->terminate(call.u32(1));
		call.ret(1);
	});
	registry.add("SetInformationJobObject", 4, [&objects, job_of](ApiCall &call) {
		const auto job = job_of(call[0]);
		if (!job) {
			call.fail(6);
			return;
		}
		const size_t width = call.pointer_width();
		const uint32_t type = call.u32(1), size = call.u32(3);
		if (!call[2]) {
			call.fail(87);
			return;
		}
		switch (type) {
		case info_basic_limit:
		case info_extended_limit: {
			// JOBOBJECT_BASIC_LIMIT_INFORMATION: three 64-bit words, the flags, the working set limits, the active process limit.
			const size_t flags_at = 24, active_at = width == 8 ? 48 : 36;
			if (size < active_at + 4) {
				call.fail(24);
				return;
			}
			job->limit_flags = call.load<uint32_t>(call[2] + flags_at);
			job->active_limit = call.load<uint32_t>(call[2] + active_at);
			if (type == info_extended_limit)
				job->extended = call.read(call[2], size);
			break;
		}
		case info_basic_ui:
			if (size < 4) {
				call.fail(24);
				return;
			}
			job->ui_restrictions = call.load<uint32_t>(call[2]);
			break;
		case info_associate_port: {
			// JOBOBJECT_ASSOCIATE_COMPLETION_PORT: the key, then the port.
			if (size < 2 * width) {
				call.fail(24);
				return;
			}
			const auto port_handle = call.load_pointer(call[2] + width);
			const auto port = objects.object_as<CompletionPort>(port_handle);
			if (!port || !(objects.access_of(port_handle) & 2)) {
				call.fail(port ? 5 : 6);
				return;
			}
			job->port = port;
			job->key = call.load_pointer(call[2]);
			break;
		}
		case info_end_of_job_time:
		case info_group:
		case info_notification_limit:
			break;
		default:
			call.fail(87);
			return;
		}
		call.ret(1);
	});
	registry.add("QueryInformationJobObject", 5, [job_of](ApiCall &call) {
		const auto job = job_of(call[0] ? call[0] : UINT64_MAX);
		if (!job) {
			call.fail(call[0] ? 6 : 87);
			return;
		}
		const size_t width = call.pointer_width();
		std::vector<uint8_t> data;
		switch (call.u32(1)) {
		case info_basic_accounting: {
			// JOBOBJECT_BASIC_ACCOUNTING_INFORMATION: times, page faults, process counts.
			data.assign(48, 0);
			const uint32_t total = job->total_processes, active = uint32_t(job->members.size()), terminated = 0;
			std::memcpy(data.data() + 32, &total, 4);
			std::memcpy(data.data() + 36, &active, 4);
			std::memcpy(data.data() + 40, &terminated, 4);
			break;
		}
		case info_basic_limit:
		case info_extended_limit: {
			const size_t basic = width == 8 ? 64 : 48;
			data.assign(call.u32(1) == info_extended_limit ? (width == 8 ? 144 : 112) : basic, 0);
			if (job->extended.size() >= data.size())
				std::memcpy(data.data(), job->extended.data(), data.size());
			std::memcpy(data.data() + 24, &job->limit_flags, 4);
			std::memcpy(data.data() + (width == 8 ? 48 : 36), &job->active_limit, 4);
			break;
		}
		case info_basic_ui:
			data.assign(4, 0);
			std::memcpy(data.data(), &job->ui_restrictions, 4);
			break;
		case info_process_id_list: {
			// JOBOBJECT_BASIC_PROCESS_ID_LIST: the room for the list, the count, then the ids as pointer-sized words.
			const uint32_t count = uint32_t(job->members.size());
			data.assign(8 + count * width, 0);
			std::memcpy(data.data(), &count, 4);
			std::memcpy(data.data() + 4, &count, 4);
			for (uint32_t index = 0; index < count; ++index) {
				const uint64_t pid = uint64_t(job->members[index]->pid);
				std::memcpy(data.data() + 8 + index * width, &pid, width);
			}
			break;
		}
		default:
			call.fail(87);
			return;
		}
		if (call[4])
			call.store<uint32_t>(call[4], uint32_t(data.size()));
		if (!call[2] || call.u32(3) < data.size()) {
			call.fail(24);
			return;
		}
		call.write(call[2], data);
		call.ret(1);
	});
}

}
