#include "child_process.h"
#include "file_objects.h"
#include "kernel_context.h"
#include "security_attributes.h"
#include "text.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace raven {

namespace {

// PROC_THREAD_ATTRIBUTE_LIST in this runtime: the capacity and the count, then one entry per attribute holding the attribute, the size of
// its value and the address of the value, as 64-bit words.
constexpr size_t attribute_header = 8, attribute_entry = 24;
constexpr uint64_t attribute_handle_list = 0x20002, attribute_job_list = 0x2000d;
constexpr uint32_t extended_startup_info = 0x80000;

constexpr uint64_t current_process = UINT64_MAX;
constexpr uint32_t create_suspended = 0x4, create_unicode_environment = 0x400, startf_use_std_handles = 0x100;

// A pipe is a connected pair of sockets with one direction closed on each end, which lets a reader look at bytes without taking them.
bool make_pipe(int &reader, int &writer)
{
	int pair[2];
	if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair))
		return false;
	::shutdown(pair[0], SHUT_WR);
	::shutdown(pair[1], SHUT_RD);
	reader = pair[0];
	writer = pair[1];
	return true;
}

// The program name a command line starts with: quoted, or up to the first blank.
std::u16string first_word(std::u16string_view line)
{
	size_t start = line.find_first_not_of(u" \t");
	if (start == line.npos)
		return {};
	if (line[start] == u'"') {
		const auto end = line.find(u'"', start + 1);
		return std::u16string(line.substr(start + 1, end == line.npos ? line.npos : end - start - 1));
	}
	const auto end = line.find_first_of(u" \t", start);
	return std::u16string(line.substr(start, end == line.npos ? line.npos : end - start));
}

}

std::shared_ptr<ProcessObject> start_program(KernelContext &context, const std::u16string &application, const std::u16string &line,
	const std::u16string &directory, const int (&standard)[3], const std::u16string &environment, int *gate, uint32_t &error,
	std::vector<InheritedHandle> inherited)
{
	auto &paths = context.paths;
	// Without an application name, an unquoted command line names its program by its first blank-separated words: Windows tries the
	// shortest run of words first and takes the first that is a file, so a path with spaces runs without quotes.
	std::vector<std::u16string> names;
	if (!application.empty())
		names.push_back(application);
	else if (const auto start = line.find_first_not_of(u" \t"); start != line.npos && line[start] != u'"') {
		for (auto end = line.find_first_of(u" \t", start); end != line.npos; end = line.find_first_of(u" \t", end + 1))
			names.emplace_back(line.substr(start, end - start));
		names.emplace_back(line.substr(start));
		while (!names.back().empty() && (names.back().back() == u' ' || names.back().back() == u'\t'))
			names.back().pop_back();
	} else
		names.push_back(first_word(line));
	if (names.front().empty()) {
		error = 87;
		return nullptr;
	}
	const auto own_directory = context.launch.image_path.substr(0, std::max<size_t>(context.launch.image_path.find_last_of(u'\\'), 0));
	const auto find = [&](const std::u16string &name) -> std::optional<NativePaths::Resolved> {
		std::vector<std::u16string> candidates;
		const bool qualified = name.find_first_of(u"\\/:") != name.npos;
		const bool extension = name.find(u'.', name.find_last_of(u"\\/") == name.npos ? 0 : name.find_last_of(u"\\/")) != name.npos;
		const auto add = [&](std::u16string path) {
			candidates.push_back(path);
			if (!extension)
				candidates.push_back(path + u".exe");
		};
		if (qualified) {
			add(name);
		} else {
			add(own_directory + u"\\" + name);
			add(paths.current() + u"\\" + name);
			add(u"C:\\Windows\\System32\\" + name);
			add(u"C:\\Windows\\" + name);
		}
		for (const auto &candidate : candidates)
			if (const auto resolved = paths.resolve(candidate); resolved && resolved->exists && std::filesystem::is_regular_file(resolved->host))
				return resolved;
		return std::nullopt;
	};
	std::optional<NativePaths::Resolved> image;
	for (const auto &name : names)
		if ((image = find(name)))
			break;
	if (!image) {
		error = 2;
		return nullptr;
	}
	auto program = image->host;
	auto command_line = line.empty() ? u"\"" + names.front() + u"\"" : line;
	// A batch file runs in the command processor, which CreateProcess starts with the whole command line as `cmd /c "..."`; the
	// system folder resolves to SysWOW64 for a 32-bit program, as its cmd.exe is the 32-bit one there.
	const auto extension = program.extension().u16string();
	if (equal_fold(extension, u".bat") || equal_fold(extension, u".cmd")) {
		const std::u16string processor = u"C:\\Windows\\System32\\cmd.exe";
		const auto interpreter = paths.resolve(processor);
		if (!interpreter || !interpreter->exists) {
			error = 2;
			return nullptr;
		}
		program = interpreter->host;
		command_line = processor + u" /c \"" + command_line + u"\"";
	} else {
		char magic[2] = {};
		std::ifstream file(program, std::ios::binary);
		file.read(magic, 2);
		if (magic[0] != 'M' || magic[1] != 'Z') {
			error = 193;
			return nullptr;
		}
	}
	ChildLaunch launch;
	launch.image = program;
	launch.command_line = command_line;
	launch.directory = directory.empty() ? paths.current() : directory;
	std::copy(std::begin(standard), std::end(standard), launch.standard);
	launch.environment = environment;
	launch.suspended = gate != nullptr;
	launch.gate = gate;
	launch.inherited = std::move(inherited);
	return spawn_child(launch, error);
}

void register_child_process_api(ApiRegistry &registry, KernelContext &context)
{
	auto &objects = context.objects;
	auto &paths = context.paths;
	context.threads.add_external(host_events_pending);
	const auto descriptor_of = [&objects](uint64_t handle) -> int {
		if (const auto file = objects.object_as<FileObject>(handle))
			return file->descriptor;
		return objects.descriptor(handle);
	};

	registry.add("CreatePipe", 4, [&objects](ApiCall &call) {
		int reader, writer;
		if (!call[0] || !call[1] || !make_pipe(reader, writer)) {
			call.fail(call[0] && call[1] ? 8 : 87);
			return;
		}
		const auto make_end = [&call](int descriptor, uint32_t access) {
			auto end = std::make_shared<PipeObject>(descriptor, access, 0);
			end->connected = true;
			end->inherit = call[2] && call.load<uint32_t>(call[2] + (call.pointer_width() == 8 ? 16 : 8)) != 0;
			return end;
		};
		const auto read_end = objects.add_object(make_end(reader, 0x80000000U));
		const auto write_end = objects.add_object(make_end(writer, 0x40000000U));
		if (!read_end.status && !write_end.status && inheritable_handle(call, call[2])) {
			objects.set_inherit(read_end.value, true);
			objects.set_inherit(write_end.value, true);
		}
		if (read_end.status || write_end.status) {
			if (!read_end.status)
				objects.close(read_end.value);
			if (!write_end.status)
				objects.close(write_end.value);
			call.fail(4);
			return;
		}
		call.store_pointer(call[0], read_end.value);
		call.store_pointer(call[1], write_end.value);
		call.ret(1);
	});
	registry.add("PeekNamedPipe", 6, [descriptor_of, &objects](ApiCall &call) {
		const auto file = objects.object_as<FileObject>(call[0]);
		if (!file || !file->readable() || file->directory) {
			call.fail(6);
			return;
		}
		if (const auto pipe = std::dynamic_pointer_cast<PipeObject>(file); pipe && !ensure_pipe_connected(*pipe)) {
			call.fail(pipe->server ? 536 : 233);
			return;
		}
		int available = 0;
		if (::ioctl(file->descriptor, FIONREAD, &available) || available < 0) {
			call.fail(109);
			return;
		}
		uint32_t copied = 0;
		const size_t wanted = call[1] ? std::min<size_t>(call.u32(2), 1 << 20) : 0;
		if (wanted) {
			std::vector<uint8_t> bytes(wanted);
			const auto count = ::recv(file->descriptor, bytes.data(), bytes.size(), MSG_PEEK | MSG_DONTWAIT);
			if (count > 0) {
				call.write(call[1], std::span<const uint8_t>(bytes.data(), size_t(count)));
				copied = uint32_t(count);
			}
		}
		if (call[3])
			call.store<uint32_t>(call[3], copied);
		if (call[4])
			call.store<uint32_t>(call[4], uint32_t(available));
		if (call[5])
			call.store<uint32_t>(call[5], 0);
		(void)descriptor_of;
		call.ret(1);
	});

	registry.add("InitializeProcThreadAttributeList", 4, [](ApiCall &call) {
		if (!call[3]) {
			call.fail(87);
			return;
		}
		const uint32_t count = call.u32(1);
		const uint64_t needed = attribute_header + count * attribute_entry;
		const uint64_t capacity = call.load_pointer(call[3]);
		call.store_pointer(call[3], needed);
		if (!call[0] || capacity < needed) {
			call.fail(122);
			return;
		}
		call.store<uint32_t>(call[0], count);
		call.store<uint32_t>(call[0] + 4, 0);
		call.ret(1);
	});
	registry.add("UpdateProcThreadAttribute", 7, [](ApiCall &call) {
		if (!call[0] || !call[3] || !call[4]) {
			call.fail(87);
			return;
		}
		const uint32_t capacity = call.load<uint32_t>(call[0]), count = call.load<uint32_t>(call[0] + 4);
		if (count >= capacity) {
			call.fail(122);
			return;
		}
		const uint64_t entry = call[0] + attribute_header + count * attribute_entry;
		call.store<uint64_t>(entry, call[2]);
		call.store<uint64_t>(entry + 8, call[4]);
		call.store<uint64_t>(entry + 16, call[3]);
		call.store<uint32_t>(call[0] + 4, count + 1);
		call.ret(1);
	});
	registry.add("DeleteProcThreadAttributeList", 1, [](ApiCall &call) {
		if (call[0])
			call.store<uint32_t>(call[0] + 4, 0);
	});
	registry.add("CreateProcessW", 10, [&, descriptor_of](ApiCall &call) {
		if (!call[9]) {
			call.fail(87);
			return;
		}
		const auto application = call[0] ? call.wide(call[0]) : std::u16string();
		const auto line = call[1] ? call.wide(call[1]) : std::u16string();
		auto directory = call[7] ? paths.full(call.wide(call[7])) : std::u16string();
		int standard[3] = {0, 1, 2};
		uint64_t standard_handles[3] = {};
		if (call[8]) {
			// STARTUPINFOW: after the flags come the show command and two reserved fields, then the three standard handles.
			const size_t width = call.pointer_width();
			const size_t flags_at = (4 + width - 1) / width * width + 3 * width + 28;
			const size_t handles_at = ((flags_at + 8 + width - 1) / width) * width + width;
			if (call.load<uint32_t>(call[8] + flags_at) & startf_use_std_handles)
				for (size_t index = 0; index < 3; ++index) {
					const auto handle = call.load_pointer(call[8] + handles_at + index * width);
					const auto widened = width == 4 && handle == 0xffffffffULL ? UINT64_MAX : handle;
					if (widened == UINT64_MAX || !widened)
						continue;
					const int descriptor = descriptor_of(widened);
					if (descriptor < 0) {
						call.fail(6);
						return;
					}
					standard[index] = descriptor;
					standard_handles[index] = widened;
				}
		}
		// Handles the child inherits: the ones the attribute list names, else every inheritable one when the caller asks for inheritance.
		const size_t width_now = call.pointer_width();
		std::optional<std::vector<uint64_t>> listed;
		std::vector<uint64_t> jobs;
		if ((call.u32(5) & extended_startup_info) && call[8]) {
			const uint64_t list = call.load_pointer(call[8] + (width_now == 8 ? 104 : 68));
			if (list) {
				const uint32_t count = call.load<uint32_t>(list + 4);
				for (uint32_t index = 0; index < count && index < 64; ++index) {
					const uint64_t entry = list + attribute_header + index * attribute_entry;
					const uint64_t attribute = call.load<uint64_t>(entry), size = call.load<uint64_t>(entry + 8), value = call.load<uint64_t>(entry + 16);
					if (attribute == attribute_handle_list || attribute == attribute_job_list) {
						std::vector<uint64_t> handles;
						for (uint64_t at = 0; at + width_now <= size && handles.size() < 4096; at += width_now) {
							const auto handle = call.load_pointer(value + at);
							handles.push_back(width_now == 4 && handle == 0xffffffffULL ? UINT64_MAX : handle);
						}
						if (attribute == attribute_handle_list)
							listed = std::move(handles);
						else
							jobs = std::move(handles);
					}
				}
			}
		}
		std::vector<InheritedHandle> inherited;
		{
			const auto candidates = listed ? *listed : call.u32(4) ? objects.inheritable() : std::vector<uint64_t>();
			for (const auto handle : candidates) {
				const auto object = objects.object(handle);
				InheritedHandle record;
				if (listed && !objects.inherit(handle)) {
					call.fail(87);
					return;
				}
				if (!object || !object->describe(record)) {
					if (std::getenv("RAVEN_TRACE_API"))
						std::fprintf(stderr, "raven: handle %llx cannot be inherited\n", static_cast<unsigned long long>(handle));
					continue;
				}
				record.handle = handle;
				record.handle_access = objects.access_of(handle);
				// A standard handle that is inherited too is the same object in the child, under the same value, as on Windows.
				for (uint32_t index = 0; index < 3; ++index)
					if (standard_handles[index] == handle)
						record.standard |= 1U << index;
				inherited.push_back(std::move(record));
			}
		}
		// The child starts with the environment the caller names, or with the one the caller has now. The block is read a page at a
		// time, so one that ends just before unmapped memory is still read whole.
		std::u16string environment;
		if (call[6]) {
			const bool wide_block = call.u32(5) & create_unicode_environment;
			for (uint64_t at = call[6]; environment.size() < 1 << 20;) {
				const size_t room = 4096 - at % 4096;
				// Whole characters only; a wide character that straddles the page is read across it.
				const auto chunk = call.read(at, !wide_block ? room : room >= 2 ? room & ~size_t(1) : 2);
				at += chunk.size();
				bool done = false;
				for (size_t index = 0; index + 1 < chunk.size() + (wide_block ? 0 : 1) && !done; index += wide_block ? 2 : 1) {
					const char16_t unit = wide_block ? char16_t(chunk[index] | chunk[index + 1] << 8) : char16_t(chunk[index]);
					environment.push_back(unit);
					if (unit == 0 && environment.size() >= 2 && environment[environment.size() - 2] == 0)
						done = true;
				}
				if (done)
					break;
			}
		} else if (context.environment_block) {
			environment = context.environment_block();
		}
		int gate = -1;
		uint32_t error = 0;
		const auto child = start_program(context, application, line, directory, standard, environment, call.u32(5) & create_suspended ? &gate : nullptr, error,
			std::move(inherited));
		if (!child) {
			call.fail(error);
			return;
		}
		for (const auto job : jobs) {
			uint32_t job_error = 0;
			if (!assign_to_job(context, job, child, job_error)) {
				child->terminate(1);
				call.fail(job_error);
				return;
			}
		}
		const auto process_handle = objects.add_object(child);
		const auto thread_object = std::make_shared<ChildThreadObject>(child);
		if (gate >= 0)
			thread_object->hold(gate);
		const auto thread_handle = objects.add_object(thread_object);
		if (process_handle.status || thread_handle.status) {
			child->terminate(1);
			call.fail(4);
			return;
		}
		const size_t width = call.pointer_width();
		call.store_pointer(call[9], process_handle.value);
		call.store_pointer(call[9] + width, thread_handle.value);
		call.store<uint32_t>(call[9] + 2 * width, uint32_t(child->pid));
		call.store<uint32_t>(call[9] + 2 * width + 4, 1);
		call.ret(1);
	});
	registry.add("GetExitCodeProcess", 2, [&objects](ApiCall &call) {
		if (!call[1]) {
			call.fail(87);
			return;
		}
		if (call[0] == current_process) {
			call.store<uint32_t>(call[1], 259);
			call.ret(1);
			return;
		}
		const auto child = objects.object_as<ProcessObject>(call[0]);
		if (!child) {
			call.fail(6);
			return;
		}
		call.store<uint32_t>(call[1], child->exit_code());
		call.ret(1);
	});
	registry.add("TerminateProcess", 2, [&objects](ApiCall &call) {
		// A handle OpenProcess gave for this process ends it as the pseudo handle does (Process.Kill on itself).
		if (call[0] == current_process || objects.object_as<SelfProcessObject>(call[0])) {
			call.process.abandon_call();
			throw ProcessExit{call.u32(1), 0, true};
		}
		const auto child = objects.object_as<ProcessObject>(call[0]);
		if (!child) {
			call.fail(6);
			return;
		}
		child->terminate(call.u32(1));
		call.ret(1);
	});
	// RaiseFailFastException ends the process at once, with no handler run, its status the exception code (STATUS_FAIL_FAST_EXCEPTION
	// without a record).
	registry.add("RaiseFailFastException", 3, [](ApiCall &call) {
		const uint32_t code = call[0] ? call.load<uint32_t>(call[0]) : 0xc0000602U;
		std::fprintf(stderr, "raven: fail fast exception %x\n", code);
		call.process.abandon_call();
		throw ProcessExit{code, 0, true};
	});
	registry.add("GetProcessId", 1, [&objects](ApiCall &call) {
		if (call[0] == current_process || objects.object_as<SelfProcessObject>(call[0])) {
			call.ret(uint32_t(::getpid()));
			return;
		}
		const auto child = objects.object_as<ProcessObject>(call[0]);
		if (!child) {
			call.fail(6);
			return;
		}
		call.ret(uint32_t(child->pid));
	});
	// Every process runs as the same host user, so a process created for a token is created as any other.
	registry.add("CreateProcessAsUserW", 11, [&registry](ApiCall &call) {
		const auto create = registry.find("CreateProcessW");
		ApiCall inner(call.process, call.environment, {call[1], call[2], call[3], call[4], call[5], call[6], call[7], call[8], call[9], call[10]});
		create->second(inner);
		call.ret(inner.result);
	});
	// A child has no message queue the parent could watch; it counts as idle from the start.
	registry.add("WaitForInputIdle", 2, [&objects](ApiCall &call) {
		if (!objects.object_as<ProcessObject>(call[0])) {
			call.fail(6, UINT32_MAX);
			return;
		}
		call.ret(0);
	});
}

}
