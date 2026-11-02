#include "child_process.h"
#include "host_names.h"
#include "kernel_context.h"
#include "registry.h"
#include "text.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <signal.h>
#include <unistd.h>

namespace raven {

namespace {

constexpr uint32_t error_service_does_not_exist = 1060, error_service_exists = 1073, error_invalid_service_control = 1052, error_service_not_active = 1062,
	error_service_already_running = 1056, error_service_disabled = 1058, error_failed_service_controller_connect = 1063;
constexpr uint32_t state_stopped = 1, state_start_pending = 2, state_stop_pending = 3, state_running = 4;
constexpr uint32_t control_stop = 1;
constexpr uint32_t no_change = 0xffffffffU;

// The service control manager and the services it opened are handles to these.
class ServiceManager : public KernelObject {};

class ServiceHandle : public KernelObject {
public:
	std::u16string name;
	// For a service status handle in the service's own process: what the program registered to receive controls.
	uint64_t handler = 0, context = 0;
	bool extended = false;
};

struct Status {
	uint32_t fields[7] = {0x10, state_stopped, 0, 0, 0, 0, 0};
	uint32_t process = 0;
};

std::filesystem::path state_directory()
{
	std::filesystem::path base;
	if (const char *runtime = std::getenv("XDG_RUNTIME_DIR"); runtime && *runtime)
		base = runtime;
	else
		base = NativePrefix::from_environment().root();
	auto directory = base / ("raven-services-" + prefix_tag());
	std::error_code error;
	std::filesystem::create_directories(directory, error);
	return directory;
}

std::string key_of(const std::u16string &name)
{
	auto text = to_utf8(name);
	for (auto &character : text)
		character = char(std::tolower(static_cast<unsigned char>(character)));
	return hex_text(name_hash(text));
}

bool alive(uint32_t pid)
{
	if (!pid)
		return false;
	std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
	std::string line;
	if (!std::getline(stat, line))
		return false;
	const auto close = line.rfind(')');
	return close != line.npos && close + 2 < line.size() && line[close + 2] != 'Z';
}

Status read_status(const std::u16string &name)
{
	Status status;
	std::ifstream input(state_directory() / (key_of(name) + ".status"), std::ios::binary);
	if (input.read(reinterpret_cast<char *>(&status), sizeof(status)) && alive(status.process))
		return status;
	return Status();
}

void write_status(const std::u16string &name, const Status &status)
{
	const auto path = state_directory() / (key_of(name) + ".status");
	auto temporary = path;
	temporary += "." + std::to_string(::getpid());
	{
		std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
		output.write(reinterpret_cast<const char *>(&status), sizeof(status));
	}
	std::error_code error;
	std::filesystem::rename(temporary, path, error);
}

const std::u16string services_path = u"SYSTEM\\CurrentControlSet\\Services\\";

}

// The manager hands out a service's command with the system folder spelled out, however the registry writes it.
static std::u16string expanded_image_path(std::u16string path)
{
	for (const std::u16string_view name : {u"%systemroot%", u"%windir%"})
		if (path.size() >= name.size() && std::equal(name.begin(), name.end(), path.begin(), [](char16_t left, char16_t right) {
			return (left >= u'A' && left <= u'Z' ? char16_t(left + 32) : left) == (right >= u'A' && right <= u'Z' ? char16_t(right + 32) : right);
		}))
			return u"C:\\Windows" + path.substr(name.size());
	return path;
}

void register_service_api(ApiRegistry &registry, KernelContext &context, NativeRegistry &hive)
{
	auto &objects = context.objects;
	const auto new_handle = [&objects](ApiCall &call, std::shared_ptr<KernelObject> object) {
		const auto made = objects.add_object(std::move(object));
		if (made.status) {
			call.fail(8, 0);
			return;
		}
		call.ret(made.value);
	};
	// A registry key of the services branch, held for the length of one call.
	const auto with_key = [&hive, &objects](const std::u16string &name, bool create, const std::function<void(uint64_t)> &use) -> bool {
		uint64_t key = 0;
		uint32_t disposition = 0;
		const auto path = services_path + name;
		const uint32_t status = create ? hive.create(0x80000002ULL, path, key, disposition) : hive.open(0x80000002ULL, path, key);
		if (status)
			return false;
		use(key);
		hive.close(key);
		objects.release_reserved(key);
		return true;
	};
	const auto set_dword = [&hive](uint64_t key, const char16_t *name, uint32_t value) {
		hive.set(key, name, 4, std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(&value), 4));
	};
	const auto set_text = [&hive](uint64_t key, const char16_t *name, const std::u16string &text, uint32_t type) {
		std::vector<uint8_t> bytes((text.size() + 1) * 2, 0);
		std::memcpy(bytes.data(), text.data(), text.size() * 2);
		hive.set(key, name, type, bytes);
	};

	const auto open_manager = [new_handle](ApiCall &call) { new_handle(call, std::make_shared<ServiceManager>()); };
	registry.add("OpenSCManagerW", 3, open_manager);
	registry.add("OpenSCManagerA", 3, open_manager);

	const auto open_service = [&objects, with_key, new_handle](ApiCall &call, bool wide) {
		if (!objects.object_as<ServiceManager>(call[0]) || !call[1]) {
			call.fail(call[1] ? 6 : 87, 0);
			return;
		}
		const auto name = wide ? call.wide(call[1], 256) : from_utf8(call.narrow(call[1], 256));
		if (!with_key(name, false, [](uint64_t) {})) {
			call.fail(error_service_does_not_exist, 0);
			return;
		}
		auto service = std::make_shared<ServiceHandle>();
		service->name = name;
		new_handle(call, service);
	};
	registry.add("OpenServiceW", 3, [open_service](ApiCall &call) { open_service(call, true); });
	registry.add("OpenServiceA", 3, [open_service](ApiCall &call) { open_service(call, false); });

	// CreateService(manager, name, display, access, type, start, error control, path, group, tag, dependencies, account, password).
	const auto create_service = [&objects, with_key, set_dword, set_text, new_handle](ApiCall &call, bool wide) {
		if (!objects.object_as<ServiceManager>(call[0]) || !call[1] || !call[7]) {
			call.fail(call[1] && call[7] ? 6 : 87, 0);
			return;
		}
		const auto text = [&](uint64_t address) { return !address ? std::u16string() : wide ? call.wide(address, 4096) : from_utf8(call.narrow(address, 4096)); };
		const auto name = text(call[1]);
		if (with_key(name, false, [](uint64_t) {})) {
			call.fail(error_service_exists, 0);
			return;
		}
		with_key(name, true, [&](uint64_t key) {
			set_text(key, u"DisplayName", call[2] ? text(call[2]) : name, 1);
			set_dword(key, u"Type", call.u32(4));
			set_dword(key, u"Start", call.u32(5));
			set_dword(key, u"ErrorControl", call.u32(6));
			set_text(key, u"ImagePath", text(call[7]), 2);
			if (call[11])
				set_text(key, u"ObjectName", text(call[11]), 1);
		});
		auto service = std::make_shared<ServiceHandle>();
		service->name = name;
		if (call[9])
			call.store<uint32_t>(call[9], 0);
		new_handle(call, service);
	};
	registry.add("CreateServiceW", 13, [create_service](ApiCall &call) { create_service(call, true); });
	registry.add("CreateServiceA", 13, [create_service](ApiCall &call) { create_service(call, false); });
	registry.add("CloseServiceHandle", 1, [&objects](ApiCall &call) {
		if (!objects.object_as<ServiceManager>(call[0]) && !objects.object_as<ServiceHandle>(call[0])) {
			call.fail(6);
			return;
		}
		objects.close(call[0]);
		call.ret(1);
	});
	registry.add("DeleteService", 1, [&objects, &hive](ApiCall &call) {
		const auto service = objects.object_as<ServiceHandle>(call[0]);
		if (!service) {
			call.fail(6);
			return;
		}
		if (hive.remove_key(0x80000002ULL, services_path + service->name)) {
			call.fail(error_service_does_not_exist);
			return;
		}
		call.ret(1);
	});
	// ChangeServiceConfig(service, type, start, error control, path, group, tag, dependencies, account, password, display name).
	registry.add("ChangeServiceConfigW", 11, [&objects, with_key, set_dword, set_text](ApiCall &call) {
		const auto service = objects.object_as<ServiceHandle>(call[0]);
		if (!service) {
			call.fail(6);
			return;
		}
		with_key(service->name, true, [&](uint64_t key) {
			if (call.u32(1) != no_change) set_dword(key, u"Type", call.u32(1));
			if (call.u32(2) != no_change) set_dword(key, u"Start", call.u32(2));
			if (call.u32(3) != no_change) set_dword(key, u"ErrorControl", call.u32(3));
			if (call[4]) set_text(key, u"ImagePath", call.wide(call[4], 4096), 2);
			if (call[10]) set_text(key, u"DisplayName", call.wide(call[10], 256), 1);
		});
		call.ret(1);
	});

	// Status: kept in a file per service so every process of the prefix sees it; a service whose process is gone reads as stopped.
	registry.add("QueryServiceStatus", 2, [&objects](ApiCall &call) {
		const auto service = objects.object_as<ServiceHandle>(call[0]);
		if (!service || !call[1]) {
			call.fail(service ? 87 : 6);
			return;
		}
		const auto status = read_status(service->name);
		call.write(call[1], std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(status.fields), 28));
		call.ret(1);
	});
	registry.add("QueryServiceStatusEx", 5, [&objects](ApiCall &call) {
		const auto service = objects.object_as<ServiceHandle>(call[0]);
		if (!service || call.u32(1) != 0 || !call[4]) {
			call.fail(service ? 87 : 6);
			return;
		}
		call.store<uint32_t>(call[4], 36);
		if (!call[2] || call.u32(3) < 36) {
			call.fail(122);
			return;
		}
		const auto status = read_status(service->name);
		std::vector<uint8_t> bytes(36, 0);
		std::memcpy(bytes.data(), status.fields, 28);
		std::memcpy(bytes.data() + 28, &status.process, 4);
		call.write(call[2], bytes);
		call.ret(1);
	});
	// QUERY_SERVICE_CONFIG: type, start, error control, then the path, group, tag, dependencies, account and display name, with the
	// strings after the structure. A service with no account runs as LocalSystem.
	const auto query_config = [&objects, with_key, &hive](ApiCall &call, bool wide) {
		const auto service = objects.object_as<ServiceHandle>(call[0]);
		if (!service) {
			call.fail(6);
			return;
		}
		uint32_t numbers[3] = {0x10, 3, 1};
		std::u16string path, account = u"LocalSystem", display = service->name;
		const bool found = with_key(service->name, false, [&](uint64_t key) {
			const char16_t *names[] = {u"Type", u"Start", u"ErrorControl"};
			for (size_t index = 0; index < 3; ++index) {
				uint32_t type = 0;
				std::vector<uint8_t> data;
				if (!hive.query(key, names[index], type, data) && data.size() >= 4)
					std::memcpy(&numbers[index], data.data(), 4);
			}
			const auto text = [&](const char16_t *name, std::u16string &out) {
				uint32_t type = 0;
				std::vector<uint8_t> data;
				if (hive.query(key, name, type, data) || data.size() < 2)
					return;
				out.assign(reinterpret_cast<const char16_t *>(data.data()), data.size() / 2);
				while (!out.empty() && !out.back())
					out.pop_back();
			};
			text(u"ImagePath", path);
			path = expanded_image_path(path);
			text(u"ObjectName", account);
			text(u"DisplayName", display);
		});
		if (!found) {
			call.fail(error_service_does_not_exist);
			return;
		}
		const size_t width = call.pointer_width(), unit = wide ? 2 : 1;
		const size_t head = width == 8 ? 64 : 36;
		const std::u16string strings[] = {path, u"", u"", account, display};
		size_t needed = head;
		for (const auto &item : strings)
			needed += (item.size() + 1 + (item.empty() ? 1 : 0)) * unit;
		if (call[3])
			call.store<uint32_t>(call[3], uint32_t(needed));
		if (!call[1] || call.u32(2) < needed) {
			call.fail(122);
			return;
		}
		std::vector<uint8_t> bytes(needed, 0);
		std::memcpy(bytes.data(), numbers, 12);
		// Pointer slots: path, group, dependencies, account, display; the tag word sits between group and dependencies.
		const size_t slots[] = {width == 8 ? 16u : 12u, width == 8 ? 24u : 16u, width == 8 ? 40u : 24u, width == 8 ? 48u : 28u, width == 8 ? 56u : 32u};
		size_t at = head;
		for (size_t index = 0; index < 5; ++index) {
			const uint64_t address = call[1] + at;
			std::memcpy(bytes.data() + slots[index], &address, width);
			if (wide) {
				std::memcpy(bytes.data() + at, strings[index].data(), strings[index].size() * 2);
				at += (strings[index].size() + 1 + (strings[index].empty() ? 1 : 0)) * 2;
			} else {
				const auto narrow = to_utf8(strings[index]);
				std::memcpy(bytes.data() + at, narrow.data(), narrow.size());
				at += narrow.size() + 1 + (strings[index].empty() ? 1 : 0);
			}
		}
		call.write(call[1], bytes);
		call.ret(1);
	};
	registry.add("QueryServiceConfigW", 4, [query_config](ApiCall &call) { query_config(call, true); });
	registry.add("QueryServiceConfigA", 4, [query_config](ApiCall &call) { query_config(call, false); });
	// GetServiceDisplayName(manager, name, buffer, characters): the service's DisplayName, or its own name when it has none. The count
	// comes back without the terminator; a buffer too small learns the length it needs.
	const auto display_name = [&objects, &hive, with_key](ApiCall &call, bool wide) {
		if (!objects.object_as<ServiceManager>(call[0]) || !call[1] || !call[3]) {
			call.fail(call[1] && call[3] ? 6 : 87, 0);
			return;
		}
		const auto name = wide ? call.wide(call[1], 256) : from_utf8(call.narrow(call[1], 256));
		std::u16string display;
		const bool found = with_key(name, false, [&](uint64_t key) {
			uint32_t type = 0;
			std::vector<uint8_t> data;
			if (!hive.query(key, u"DisplayName", type, data) && data.size() >= 2) {
				display.assign(reinterpret_cast<const char16_t *>(data.data()), data.size() / 2);
				while (!display.empty() && !display.back())
					display.pop_back();
			}
		});
		if (!found) {
			call.fail(error_service_does_not_exist, 0);
			return;
		}
		if (display.empty())
			display = name;
		const auto narrow = to_utf8(display);
		const size_t length = wide ? display.size() : narrow.size();
		if (!call[2] || call.u32(3) <= length) {
			call.store<uint32_t>(call[3], uint32_t(length));
			call.fail(122, 0);
			return;
		}
		if (wide)
			call.put_wide(call[2], display, display.size() + 1);
		else {
			std::vector<uint8_t> bytes(narrow.begin(), narrow.end());
			bytes.push_back(0);
			call.write(call[2], bytes);
		}
		call.store<uint32_t>(call[3], uint32_t(length));
		call.ret(1);
	};
	registry.add("GetServiceDisplayNameW", 4, [display_name](ApiCall &call) { display_name(call, true); });
	registry.add("GetServiceDisplayNameA", 4, [display_name](ApiCall &call) { display_name(call, false); });
	// SERVICE_CONFIG_DESCRIPTION keeps the description; the other optional settings (failure actions, delayed start and the like)
	// are accepted and have no effect here.
	const auto change_config2 = [&objects, with_key, set_text](ApiCall &call, bool wide) {
		const auto service = objects.object_as<ServiceHandle>(call[0]);
		if (!service) {
			call.fail(6);
			return;
		}
		if (call.u32(1) == 1 && call[2]) {
			const uint64_t text = call.load_pointer(call[2]);
			if (text)
				with_key(service->name, true, [&](uint64_t key) {
					set_text(key, u"Description", wide ? call.wide(text, 4096) : from_utf8(call.narrow(text, 4096)), 1);
				});
		}
		call.ret(1);
	};
	registry.add("ChangeServiceConfig2W", 3, [change_config2](ApiCall &call) { change_config2(call, true); });
	registry.add("ChangeServiceConfig2A", 3, [change_config2](ApiCall &call) { change_config2(call, false); });
	// The optional settings read back: the description that was kept, no failure actions and no delayed start. A buffer too small
	// learns the size it needs.
	const auto query_config2 = [&objects, &hive, with_key](ApiCall &call, bool wide) {
		const auto service = objects.object_as<ServiceHandle>(call[0]);
		if (!service || !call[4]) {
			call.fail(service ? 87 : 6);
			return;
		}
		const size_t pointer = call.pointer_width();
		std::vector<uint8_t> block;
		std::optional<size_t> text_slot;
		std::vector<uint8_t> text;
		switch (call.u32(1)) {
		case 1: {
			std::u16string description;
			with_key(service->name, false, [&](uint64_t key) {
				uint32_t type = 0;
				std::vector<uint8_t> data;
				if (!hive.query(key, u"Description", type, data) && data.size() >= 2) {
					description.assign(reinterpret_cast<const char16_t *>(data.data()), data.size() / 2);
					while (!description.empty() && !description.back())
						description.pop_back();
				}
			});
			block.resize(pointer);
			if (wide) {
				text.resize((description.size() + 1) * 2);
				std::memcpy(text.data(), description.data(), description.size() * 2);
			} else {
				const auto narrow = to_utf8(description);
				text.assign(narrow.begin(), narrow.end());
				text.push_back(0);
			}
			text_slot = 0;
			break;
		}
		case 2:
			block.resize(pointer == 8 ? 40 : 20);
			break;
		case 3:
			block.resize(4);
			break;
		default:
			call.fail(124);
			return;
		}
		const size_t needed = block.size() + text.size();
		call.store<uint32_t>(call[4], uint32_t(needed));
		if (!call[2] || call.u32(3) < needed) {
			call.fail(122);
			return;
		}
		if (text_slot) {
			const uint64_t address = call[2] + block.size();
			std::memcpy(block.data() + *text_slot, &address, pointer);
		}
		block.insert(block.end(), text.begin(), text.end());
		call.write(call[2], block);
		call.ret(1);
	};
	registry.add("QueryServiceConfig2W", 5, [query_config2](ApiCall &call) { query_config2(call, true); });
	registry.add("QueryServiceConfig2A", 5, [query_config2](ApiCall &call) { query_config2(call, false); });
	// The security of a service is a self-relative descriptor kept with its configuration; a service that never had one set grants
	// Administrators and LocalSystem every right and everyone else the right to read its state.
	registry.add("QueryServiceObjectSecurity", 5, [&objects, with_key, &hive](ApiCall &call) {
		const auto service = objects.object_as<ServiceHandle>(call[0]);
		if (!service) {
			call.fail(6);
			return;
		}
		std::vector<uint8_t> descriptor;
		with_key(service->name, false, [&](uint64_t key) {
			uint32_t type = 0;
			hive.query(key, u"Security", type, descriptor);
		});
		if (descriptor.empty()) {
			const auto ace = [](uint32_t mask, std::vector<uint8_t> sid) {
				std::vector<uint8_t> entry{0, 0, 0, 0};
				for (int shift = 0; shift < 32; shift += 8)
					entry.push_back(uint8_t(mask >> shift));
				entry.insert(entry.end(), sid.begin(), sid.end());
				entry[2] = uint8_t(entry.size());
				return entry;
			};
			const std::vector<uint8_t> system{1, 1, 0, 0, 0, 0, 0, 5, 18, 0, 0, 0};
			const std::vector<uint8_t> administrators{1, 2, 0, 0, 0, 0, 0, 5, 32, 0, 0, 0, 0x20, 2, 0, 0};
			const std::vector<uint8_t> everyone{1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0};
			std::vector<uint8_t> aces;
			for (const auto &entry : {ace(0xf01ff, system), ace(0xf01ff, administrators), ace(0x2018d, everyone)})
				aces.insert(aces.end(), entry.begin(), entry.end());
			std::vector<uint8_t> acl{2, 0, 0, 0, 3, 0, 0, 0};
			acl[2] = uint8_t((8 + aces.size()) & 0xff);
			acl[3] = uint8_t((8 + aces.size()) >> 8);
			acl.insert(acl.end(), aces.begin(), aces.end());
			// SECURITY_DESCRIPTOR_RELATIVE: revision, control (self-relative, DACL present), owner, group, SACL and DACL offsets.
			descriptor = {1, 0, 0x04, 0x80};
			const uint32_t owner = 20, group = owner + uint32_t(system.size()), dacl = group + uint32_t(system.size());
			for (const uint32_t value : {owner, group, 0u, dacl})
				for (int shift = 0; shift < 32; shift += 8)
					descriptor.push_back(uint8_t(value >> shift));
			descriptor.insert(descriptor.end(), system.begin(), system.end());
			descriptor.insert(descriptor.end(), system.begin(), system.end());
			descriptor.insert(descriptor.end(), acl.begin(), acl.end());
		}
		if (call[4])
			call.store<uint32_t>(call[4], uint32_t(descriptor.size()));
		if (!call[2] || call.u32(3) < descriptor.size()) {
			call.fail(122);
			return;
		}
		call.write(call[2], descriptor);
		call.ret(1);
	});
	registry.add("SetServiceObjectSecurity", 3, [&objects, with_key, &hive](ApiCall &call) {
		const auto service = objects.object_as<ServiceHandle>(call[0]);
		if (!service || !call[2]) {
			call.fail(service ? 87 : 6);
			return;
		}
		// A descriptor comes self-relative (offsets after the header) or absolute (pointers to its parts); it is kept self-relative.
		const size_t width = call.pointer_width();
		const auto head = call.read(call[2], 4);
		const bool relative = head[3] & 0x80;
		const auto part_at = [&](size_t index) -> uint64_t {
			if (relative) {
				uint32_t offset = 0;
				std::memcpy(&offset, call.read(call[2] + 4 + index * 4, 4).data(), 4);
				return offset ? call[2] + offset : 0;
			}
			// SECURITY_DESCRIPTOR: revision, padding, control, then the owner, group, SACL and DACL pointers at pointer alignment.
			return call.load_pointer(call[2] + (width == 8 ? 8 : 4) + index * width);
		};
		std::vector<uint8_t> descriptor{head[0], 0, head[2], uint8_t(head[3] | 0x80)};
		descriptor.resize(20, 0);
		for (size_t index = 0; index < 4; ++index) {
			const uint64_t at = part_at(index);
			if (!at)
				continue;
			const auto start = call.read(at, 4);
			const uint32_t length = index < 2 ? 8 + 4 * uint32_t(start[1]) : uint32_t(start[2] | start[3] << 8);
			const auto part = call.read(at, length);
			const uint32_t offset = uint32_t(descriptor.size());
			std::memcpy(descriptor.data() + 4 + index * 4, &offset, 4);
			descriptor.insert(descriptor.end(), part.begin(), part.end());
		}
		with_key(service->name, true, [&](uint64_t key) { hive.set(key, u"Security", 3, descriptor); });
		call.ret(1);
	});

	// Starting a service runs its program; the program reports its state once it calls StartServiceCtrlDispatcher.
	registry.add("StartServiceW", 3, [&context, &objects, with_key, &hive](ApiCall &call) {
		const auto service = objects.object_as<ServiceHandle>(call[0]);
		if (!service) {
			call.fail(6);
			return;
		}
		std::u16string image;
		uint32_t start_type = 3;
		const bool found = with_key(service->name, false, [&](uint64_t key) {
			uint32_t type = 0;
			std::vector<uint8_t> data;
			if (!hive.query(key, u"ImagePath", type, data))
				for (size_t at = 0; at + 1 < data.size(); at += 2)
					image.push_back(char16_t(data[at] | data[at + 1] << 8));
			while (!image.empty() && image.back() == 0)
				image.pop_back();
			if (!hive.query(key, u"Start", type, data) && data.size() >= 4)
				std::memcpy(&start_type, data.data(), 4);
		});
		if (!found || image.empty()) {
			call.fail(error_service_does_not_exist);
			return;
		}
		if (start_type == 4) {
			call.fail(error_service_disabled);
			return;
		}
		if (read_status(service->name).fields[1] != state_stopped) {
			call.fail(error_service_already_running);
			return;
		}
		const int standard[3] = {0, 1, 2};
		uint32_t error = 0;
		const auto child = start_program(context, u"", image, u"", standard, context.environment_block ? context.environment_block() : std::u16string(), nullptr, error);
		if (!child) {
			call.fail(error ? error : 2);
			return;
		}
		Status status;
		status.fields[0] = 0x10;
		status.fields[1] = state_start_pending;
		status.process = uint32_t(child->pid);
		write_status(service->name, status);
		call.ret(1);
	});
	registry.add("StartServiceA", 3, [](ApiCall &call) { call.fail(120); });

	// ControlService asks a running service to handle a control; the request is a file its process picks up.
	registry.add("ControlService", 3, [&objects](ApiCall &call) {
		const auto service = objects.object_as<ServiceHandle>(call[0]);
		if (!service || !call[2]) {
			call.fail(service ? 87 : 6);
			return;
		}
		auto status = read_status(service->name);
		if (status.fields[1] == state_stopped) {
			call.fail(error_service_not_active);
			return;
		}
		{
			std::ofstream request(state_directory() / (key_of(service->name) + ".control"), std::ios::binary | std::ios::trunc);
			const uint32_t control = call.u32(1);
			request.write(reinterpret_cast<const char *>(&control), 4);
		}
		call.write(call[2], std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(status.fields), 28));
		call.ret(1);
	});

	// The service side. The program's main thread gives its service table to the dispatcher, which runs the service's main function on
	// a thread of its own and waits until the service reports that it stopped.
	struct Running {
		std::u16string name;
		std::shared_ptr<ServiceHandle> handle;
		std::shared_ptr<GuestThread> main;
		bool stopped = false;
	};
	auto running = std::make_shared<std::vector<std::shared_ptr<Running>>>();
	registry.add("StartServiceCtrlDispatcherW", 1, [&context, &objects, running](ApiCall &call) {
		if (!call[0]) {
			call.fail(87);
			return;
		}
		const size_t width = call.pointer_width();
		const auto name_address = call.load_pointer(call[0]), procedure = call.load_pointer(call[0] + width);
		if (!procedure) {
			call.fail(error_failed_service_controller_connect);
			return;
		}
		auto service = std::make_shared<Running>();
		service->name = name_address ? call.wide(name_address, 256) : u"";
		// SERVICE_TABLE_ENTRY for a service of its own process may leave the name empty; the status file is keyed by the name given.
		const auto name_memory = context.allocator.allocate((service->name.size() + 1) * 2, true);
		const auto vector = context.allocator.allocate(2 * width, true);
		if (!name_memory || !vector) {
			call.fail(8);
			return;
		}
		call.put_wide(name_memory, service->name, service->name.size() + 1);
		call.store_pointer(vector, name_memory);
		call.store_pointer(vector + width, 0);
		running->push_back(service);
		Status status;
		status.fields[1] = state_start_pending;
		status.process = uint32_t(::getpid());
		write_status(service->name, status);
		service->main = context.threads.create(procedure, 1, 1024 * 1024, false);
		service->main->arguments = {1, vector};
		HostWait waiting;
		context.threads.block([service] { return service->stopped || service->main->state == GuestThread::State::done; }, std::nullopt);
		// A service main function that returned without reporting a stop has stopped all the same.
		Status last = read_status(service->name);
		last.fields[1] = state_stopped;
		write_status(service->name, last);
		call.ret(1);
	});
	const auto register_handler = [&objects, running, new_handle](ApiCall &call, bool extended, bool wide) {
		const auto name = wide ? call.wide(call[0], 256) : from_utf8(call.narrow(call[0], 256));
		auto handle = std::make_shared<ServiceHandle>();
		handle->name = name;
		handle->handler = call[1];
		handle->context = extended ? call[2] : 0;
		handle->extended = extended;
		for (const auto &service : *running)
			if (service->name == name || name.empty())
				service->handle = handle;
		new_handle(call, handle);
	};
	registry.add("RegisterServiceCtrlHandlerW", 2, [register_handler](ApiCall &call) { register_handler(call, false, true); });
	registry.add("RegisterServiceCtrlHandlerExW", 3, [register_handler](ApiCall &call) { register_handler(call, true, true); });
	registry.add("SetServiceStatus", 2, [&objects, running](ApiCall &call) {
		const auto handle = objects.object_as<ServiceHandle>(call[0]);
		if (!handle || !call[1]) {
			call.fail(handle ? 87 : 6);
			return;
		}
		Status status;
		const auto raw = call.read(call[1], 28);
		std::memcpy(status.fields, raw.data(), 28);
		status.process = uint32_t(::getpid());
		write_status(handle->name, status);
		if (status.fields[1] == state_stopped)
			for (const auto &service : *running)
				if (service->name == handle->name || handle->name.empty())
					service->stopped = true;
		call.ret(1);
	});
	// Requests from other processes arrive as files; each one runs the handler the service registered, on a thread of its own.
	context.threads.add_pump([&context, running] {
		for (const auto &service : *running) {
			if (!service->handle || !service->handle->handler)
				continue;
			const auto path = state_directory() / (key_of(service->name) + ".control");
			std::ifstream request(path, std::ios::binary);
			uint32_t control = 0;
			if (!request.read(reinterpret_cast<char *>(&control), 4))
				continue;
			request.close();
			std::error_code error;
			std::filesystem::remove(path, error);
			const auto thread = context.threads.create(service->handle->handler, control, 1024 * 1024, false);
			thread->arguments = service->handle->extended ? std::vector<uint64_t>{control, 0, 0, service->handle->context} : std::vector<uint64_t>{control};
		}
	});
	context.threads.add_external([running] { return !running->empty(); });
	(void)control_stop;
	(void)state_running;
	(void)state_stop_pending;
	(void)error_invalid_service_control;
}

}
