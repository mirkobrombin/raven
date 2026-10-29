#include "elevation.h"
#include "access_token.h"
#include "file_objects.h"
#include "kernel_context.h"
#include "text.h"

#include <map>
#include <optional>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace raven {

namespace {

constexpr uint16_t owner_defaulted = 0x1, group_defaulted = 0x2, dacl_present = 0x4, dacl_defaulted = 0x8, sacl_present = 0x10, sacl_defaulted = 0x20;
constexpr size_t acl_header = 8;

// Where the fields of an absolute security descriptor sit: the four pointers follow the revision and the control word,
// so they start at 4 for a 32-bit program and at 8 for a 64-bit one.
struct Layout {
	size_t size, owner, group, sacl, dacl;
};

Layout layout_of(const ApiCall &call)
{
	const size_t width = call.pointer_width();
	const size_t first = width == 4 ? 4 : 8;
	return {first + 4 * width, first, first + width, first + 2 * width, first + 3 * width};
}

uint32_t sid_length(ApiCall &call, uint64_t sid)
{
	return 8 + 4 * uint32_t(call.read(sid + 1, 1)[0]);
}

bool valid_sid(ApiCall &call, uint64_t sid)
{
	if (!sid)
		return false;
	try {
		const auto header = call.read(sid, 2);
		return header[0] == 1 && header[1] >= 1 && header[1] <= 15;
	} catch (const ApiFault &) {
		return false;
	}
}


// A SID from its string form (S-1-...) or from one of the two-letter names of the security descriptor language.
std::optional<std::vector<uint8_t>> sid_from_text(std::u16string text)
{
	static const std::map<std::u16string, std::u16string> aliases{
		{u"AN", u"S-1-5-7"}, {u"AO", u"S-1-5-32-548"}, {u"AU", u"S-1-5-11"}, {u"BA", u"S-1-5-32-544"}, {u"BG", u"S-1-5-32-546"},
		{u"BO", u"S-1-5-32-551"}, {u"BU", u"S-1-5-32-545"}, {u"CG", u"S-1-3-1"}, {u"CO", u"S-1-3-0"}, {u"CY", u"S-1-5-32-569"},
		{u"ED", u"S-1-5-9"}, {u"HI", u"S-1-16-12288"}, {u"IU", u"S-1-5-4"}, {u"LS", u"S-1-5-19"}, {u"LW", u"S-1-16-4096"},
		{u"ME", u"S-1-16-8192"}, {u"MP", u"S-1-16-8448"}, {u"NO", u"S-1-5-32-556"}, {u"NS", u"S-1-5-20"}, {u"NU", u"S-1-5-2"},
		{u"PO", u"S-1-5-32-550"}, {u"PS", u"S-1-5-10"}, {u"PU", u"S-1-5-32-547"}, {u"RC", u"S-1-5-12"}, {u"RD", u"S-1-5-32-555"},
		{u"RE", u"S-1-5-32-552"}, {u"RU", u"S-1-5-32-554"}, {u"SI", u"S-1-16-16384"}, {u"SO", u"S-1-5-32-549"}, {u"SS", u"S-1-18-2"},
		{u"SU", u"S-1-5-6"}, {u"SY", u"S-1-5-18"}, {u"WD", u"S-1-1-0"}, {u"WR", u"S-1-5-33"}, {u"OW", u"S-1-3-4"},
		{u"UD", u"S-1-5-84-0-0-0-0-0"}, {u"AC", u"S-1-15-2-1"}, {u"S1", u"S-1-16-0"},
		// The local administrator and guest accounts belong to the machine domain the user is part of.
		{u"LA", u"S-1-5-21-1000-1000-1000-500"}, {u"LG", u"S-1-5-21-1000-1000-1000-501"}};
	if (const auto alias = aliases.find(text); alias != aliases.end())
		text = alias->second;
	unsigned long long numbers[16];
	size_t count = 0;
	if (text.size() < 5 || text[0] != u'S' || text[1] != u'-' || text[2] != u'1' || text[3] != u'-')
		return std::nullopt;
	for (size_t at = 4; at < text.size() && count < 16;) {
		unsigned long long value = 0;
		size_t digits = 0;
		while (at < text.size() && text[at] >= u'0' && text[at] <= u'9') {
			value = value * 10 + (text[at] - u'0');
			++at;
			++digits;
		}
		if (!digits || (at < text.size() && text[at] != u'-'))
			return std::nullopt;
		numbers[count++] = value;
		if (at < text.size())
			++at;
	}
	if (count < 2 || count > 9)
		return std::nullopt;
	std::vector<uint8_t> sid{1, uint8_t(count - 1), 0, 0, 0, 0, 0, 0};
	for (int index = 0; index < 6; ++index)
		sid[size_t(2 + index)] = uint8_t(numbers[0] >> (8 * (5 - index)));
	for (size_t index = 1; index < count; ++index)
		for (int shift = 0; shift < 32; shift += 8)
			sid.push_back(uint8_t(numbers[index] >> shift));
	return sid;
}

// The access rights of an ACE in the security descriptor language: a number, or a run of two-letter right names.
std::optional<uint32_t> rights_from_text(std::u16string_view text)
{
	if (text.size() > 2 && text[0] == u'0' && (text[1] == u'x' || text[1] == u'X')) {
		uint32_t value = 0;
		for (const auto character : text.substr(2)) {
			const int digit = character >= u'0' && character <= u'9' ? character - u'0' : character >= u'a' && character <= u'f' ? character - u'a' + 10 :
				character >= u'A' && character <= u'F' ? character - u'A' + 10 : -1;
			if (digit < 0)
				return std::nullopt;
			value = value << 4 | uint32_t(digit);
		}
		return value;
	}
	static const std::map<std::u16string_view, uint32_t> names{
		{u"GA", 0x10000000}, {u"GR", 0x80000000}, {u"GW", 0x40000000}, {u"GX", 0x20000000}, {u"RC", 0x20000}, {u"SD", 0x10000},
		{u"WD", 0x40000}, {u"WO", 0x80000}, {u"RP", 0x10}, {u"WP", 0x20}, {u"CC", 0x1}, {u"DC", 0x2}, {u"LC", 0x4}, {u"SW", 0x8},
		{u"LO", 0x80}, {u"DT", 0x40}, {u"CR", 0x100}, {u"FA", 0x1f01ff}, {u"FR", 0x120089}, {u"FW", 0x120116}, {u"FX", 0x1200a0},
		{u"KA", 0xf003f}, {u"KR", 0x20019}, {u"KW", 0x20006}, {u"KX", 0x20019}, {u"NR", 0x2}, {u"NW", 0x1}, {u"NX", 0x4}};
	uint32_t value = 0;
	if (text.size() % 2)
		return std::nullopt;
	for (size_t at = 0; at < text.size(); at += 2) {
		const auto found = names.find(text.substr(at, 2));
		if (found == names.end())
			return std::nullopt;
		value |= found->second;
	}
	return value;
}

// An ACL from the part of a descriptor after "D:" or "S:": optional flags (P, AI, AR), then entries in parentheses. The control bits the
// flags stand for are added to `control`, shifted for a SACL.
std::optional<std::vector<uint8_t>> acl_from_text(std::u16string_view text, uint16_t &control, bool system)
{
	size_t at = 0;
	while (at < text.size() && text[at] != u'(') {
		if (text.substr(at, 2) == u"AI") { control |= system ? 0x800 : 0x400; at += 2; }
		else if (text.substr(at, 2) == u"AR") { control |= system ? 0x200 : 0x100; at += 2; }
		else if (text[at] == u'P') { control |= system ? 0x2000 : 0x1000; ++at; }
		else return std::nullopt;
	}
	std::vector<uint8_t> entries;
	uint16_t count = 0;
	while (at < text.size()) {
		if (text[at] != u'(')
			return std::nullopt;
		const auto close = text.find(u')', at);
		if (close == text.npos)
			return std::nullopt;
		std::vector<std::u16string_view> fields;
		for (auto rest = text.substr(at + 1, close - at - 1);;) {
			const auto semicolon = rest.find(u';');
			fields.push_back(rest.substr(0, semicolon));
			if (semicolon == rest.npos)
				break;
			rest = rest.substr(semicolon + 1);
		}
		at = close + 1;
		if (fields.size() < 6 || !fields[3].empty() || !fields[4].empty())
			return std::nullopt;
		static const std::map<std::u16string_view, uint8_t> types{{u"A", 0}, {u"D", 1}, {u"AU", 2}, {u"AL", 3}, {u"ML", 0x11}};
		static const std::map<std::u16string_view, uint8_t> flag_names{{u"OI", 0x1}, {u"CI", 0x2}, {u"NP", 0x4}, {u"IO", 0x8}, {u"ID", 0x10},
			{u"SA", 0x40}, {u"FA", 0x80}};
		const auto type = types.find(fields[0]);
		if (type == types.end())
			return std::nullopt;
		uint8_t flags = 0;
		if (fields[1].size() % 2)
			return std::nullopt;
		for (size_t index = 0; index < fields[1].size(); index += 2) {
			const auto flag = flag_names.find(fields[1].substr(index, 2));
			if (flag == flag_names.end())
				return std::nullopt;
			flags |= flag->second;
		}
		const auto mask = rights_from_text(fields[2]);
		const auto sid = sid_from_text(std::u16string(fields[5]));
		if (!mask || !sid)
			return std::nullopt;
		const uint16_t size = uint16_t(8 + sid->size());
		entries.push_back(type->second);
		entries.push_back(flags);
		entries.push_back(uint8_t(size));
		entries.push_back(uint8_t(size >> 8));
		for (int shift = 0; shift < 32; shift += 8)
			entries.push_back(uint8_t(*mask >> shift));
		entries.insert(entries.end(), sid->begin(), sid->end());
		++count;
	}
	const uint16_t size = uint16_t(8 + entries.size());
	std::vector<uint8_t> acl{2, 0, uint8_t(size), uint8_t(size >> 8), uint8_t(count), uint8_t(count >> 8), 0, 0};
	acl.insert(acl.end(), entries.begin(), entries.end());
	return acl;
}

// A self-relative security descriptor from its string form: owner (O:), group (G:), DACL (D:) and SACL (S:), in any order.
std::optional<std::vector<uint8_t>> descriptor_from_text(std::u16string_view text)
{
	std::vector<uint8_t> owner, group, dacl, sacl;
	uint16_t control = 0x8000;
	bool has_dacl = false, has_sacl = false;
	size_t at = 0;
	while (at < text.size()) {
		while (at < text.size() && text[at] == u' ')
			++at;
		if (at + 2 > text.size() || text[at + 1] != u':')
			return std::nullopt;
		const char16_t part = text[at];
		at += 2;
		// A part runs to the next "X:" that starts another part; colons inside entries never follow a single letter at the top level.
		size_t end = at, depth = 0;
		while (end < text.size()) {
			if (text[end] == u'(') ++depth;
			else if (text[end] == u')') --depth;
			else if (!depth && end + 1 < text.size() && text[end + 1] == u':' && (text[end] == u'O' || text[end] == u'G' || text[end] == u'D' || text[end] == u'S'))
				break;
			++end;
		}
		const auto value = text.substr(at, end - at);
		at = end;
		if (part == u'O' || part == u'G') {
			const auto sid = sid_from_text(std::u16string(value));
			if (!sid)
				return std::nullopt;
			(part == u'O' ? owner : group) = *sid;
		} else if (part == u'D' || part == u'S') {
			const auto acl = acl_from_text(value, control, part == u'S');
			if (!acl)
				return std::nullopt;
			if (part == u'D') { dacl = *acl; has_dacl = true; control |= 0x4; }
			else { sacl = *acl; has_sacl = true; control |= 0x10; }
		} else {
			return std::nullopt;
		}
	}
	std::vector<uint8_t> result(20, 0);
	result[0] = 1;
	const auto place = [&](const std::vector<uint8_t> &part, bool present, size_t field) {
		if (!present)
			return;
		const uint32_t offset = uint32_t(result.size());
		std::memcpy(result.data() + field, &offset, 4);
		result.insert(result.end(), part.begin(), part.end());
	};
	place(owner, !owner.empty(), 4);
	place(group, !group.empty(), 8);
	place(sacl, has_sacl, 12);
	place(dacl, has_dacl, 16);
	std::memcpy(result.data() + 2, &control, 2);
	return result;
}
}

void register_security_api(ApiRegistry &registry, KernelContext &context)
{
	auto &objects = context.objects;
#ifdef RAVEN_OPENSSL
	register_schannel_api(registry, context);
#else
	// Without a TLS library no SSPI provider is installed; callers can use the documented initialization failure path.
	registry.add("InitSecurityInterfaceA", 0, [](ApiCall &call) { call.fail(50); });
	registry.add("InitSecurityInterfaceW", 0, [](ApiCall &call) { call.fail(50); });
#endif
	registry.add("InitializeSecurityDescriptor", 2, [](ApiCall &call) {
		if (!call[0]) {
			call.fail(87);
			return;
		}
		if (call.u32(1) != 1) {
			call.fail(1305);
			return;
		}
		std::vector<uint8_t> descriptor(layout_of(call).size);
		descriptor[0] = 1;
		call.write(call[0], descriptor);
		call.ret(1);
	});
	const auto control = [](ApiCall &call, uint16_t set, uint16_t clear) {
		auto value = call.load<uint16_t>(call[0] + 2);
		value = uint16_t((value & ~clear) | set);
		call.store<uint16_t>(call[0] + 2, value);
	};
	registry.add("SetSecurityDescriptorOwner", 3, [control](ApiCall &call) {
		if (!call[0] || call.read(call[0], 1)[0] != 1) {
			call.fail(1338);
			return;
		}
		call.store_pointer(call[0] + layout_of(call).owner, call[1]);
		control(call, call.u32(2) ? owner_defaulted : 0, owner_defaulted);
		call.ret(1);
	});
	registry.add("SetSecurityDescriptorGroup", 3, [control](ApiCall &call) {
		if (!call[0] || call.read(call[0], 1)[0] != 1) {
			call.fail(1338);
			return;
		}
		call.store_pointer(call[0] + layout_of(call).group, call[1]);
		control(call, call.u32(2) ? group_defaulted : 0, group_defaulted);
		call.ret(1);
	});
	registry.add("SetSecurityDescriptorDacl", 4, [control](ApiCall &call) {
		if (!call[0] || call.read(call[0], 1)[0] != 1) {
			call.fail(1338);
			return;
		}
		const bool present = call.u32(1) != 0;
		call.store_pointer(call[0] + layout_of(call).dacl, present ? call[2] : 0);
		control(call, uint16_t((present ? dacl_present : 0) | (call.u32(3) ? dacl_defaulted : 0)), dacl_present | dacl_defaulted);
		call.ret(1);
	});
	registry.add("SetSecurityDescriptorSacl", 4, [control](ApiCall &call) {
		if (!call[0] || call.read(call[0], 1)[0] != 1) {
			call.fail(1338);
			return;
		}
		const bool present = call.u32(1) != 0;
		call.store_pointer(call[0] + layout_of(call).sacl, present ? call[2] : 0);
		control(call, uint16_t((present ? sacl_present : 0) | (call.u32(3) ? sacl_defaulted : 0)), sacl_present | sacl_defaulted);
		call.ret(1);
	});
	registry.add("IsValidSecurityDescriptor", 1, [](ApiCall &call) {
		if (!call[0]) {
			call.fail(87);
			return;
		}
		call.ret(call.read(call[0], 1)[0] == 1);
	});
	// A SID with its authority and room for the count of parts asked for; the parts themselves are the caller's to fill.
	registry.add("InitializeSid", 3, [](ApiCall &call) {
		if (!call[0] || !call[1] || call.u32(2) > 15) {
			call.fail(87);
			return;
		}
		std::vector<uint8_t> header{1, uint8_t(call.u32(2))};
		const auto authority = call.read(call[1], 6);
		header.insert(header.end(), authority.begin(), authority.end());
		call.write(call[0], header);
		call.ret(1);
	});
	registry.add("GetLengthSid", 1, [](ApiCall &call) { call.ret(sid_length(call, call[0])); });
	registry.add("IsValidSid", 1, [](ApiCall &call) { call.ret(valid_sid(call, call[0])); });
	registry.add("EqualSid", 2, [](ApiCall &call) {
		if (!valid_sid(call, call[0]) || !valid_sid(call, call[1])) {
			call.fail(1337);
			return;
		}
		const auto length = sid_length(call, call[0]);
		call.ret(length == sid_length(call, call[1]) && call.read(call[0], length) == call.read(call[1], length));
	});
	registry.add("CopySid", 3, [](ApiCall &call) {
		if (!valid_sid(call, call[2])) {
			call.fail(1337);
			return;
		}
		const auto length = sid_length(call, call[2]);
		if (call.u32(0) < length) {
			call.fail(122);
			return;
		}
		call.write(call[1], call.read(call[2], length));
		call.ret(1);
	});
	// The ntdll forms of the same: lengths and checks as they are, equality as a boolean, copies with an NTSTATUS.
	registry.add("RtlLengthSid", 1, [](ApiCall &call) { call.ret(sid_length(call, call[0])); });
	registry.add("RtlValidSid", 1, [](ApiCall &call) { call.ret(valid_sid(call, call[0])); });
	registry.add("RtlEqualSid", 2, [](ApiCall &call) {
		const auto length = sid_length(call, call[0]);
		call.ret(valid_sid(call, call[0]) && valid_sid(call, call[1]) && length == sid_length(call, call[1]) &&
			call.read(call[0], length) == call.read(call[1], length));
	});
	registry.add("RtlCopySid", 3, [](ApiCall &call) {
		const auto length = sid_length(call, call[2]);
		if (call.u32(0) < length) {
			call.ret(0xc0000023U);
			return;
		}
		call.write(call[1], call.read(call[2], length));
		call.ret(0);
	});
	// An access control list is a header followed by its entries: the size of the whole, and how many entries it holds.
	registry.add("InitializeAcl", 3, [](ApiCall &call) {
		const uint32_t length = call.u32(1);
		if (call.u32(2) < 2 || call.u32(2) > 4) {
			call.fail(87);
			return;
		}
		if (length < acl_header || length > 0xffff) {
			call.fail(122);
			return;
		}
		std::vector<uint8_t> header(acl_header);
		header[0] = uint8_t(call.u32(2));
		const uint16_t size = uint16_t(length);
		std::memcpy(header.data() + 2, &size, 2);
		call.write(call[0], header);
		call.ret(1);
	});
	registry.add("AddMandatoryAce", 5, [](ApiCall &call) {
		if (call.u32(1) != 2 && call.u32(1) != 4) { call.fail(1306); return; }
		if ((call.u32(2) & ~0x1fU) || (call.u32(3) & ~7U)) { call.fail(87); return; }
		if (!valid_sid(call, call[4])) { call.fail(1337); return; }
		const auto sid = call.read(call[4], sid_length(call, call[4]));
		if (sid[1] != 1 || sid[2] || sid[3] || sid[4] || sid[5] || sid[6] || sid[7] != 16) { call.fail(1337); return; }
		const auto header = call.read(call[0], acl_header);
		uint16_t size, count;
		std::memcpy(&size, header.data() + 2, 2);
		std::memcpy(&count, header.data() + 4, 2);
		if (size < acl_header || (header[0] != 2 && header[0] != 4)) { call.fail(1336); return; }
		auto acl = call.read(call[0], size);
		size_t used = acl_header;
		for (uint32_t index = 0; index < count; ++index) {
			if (used + 4 > size) { call.fail(1336); return; }
			uint16_t length;
			std::memcpy(&length, acl.data() + used + 2, 2);
			if (length < 4 || length % 4 || used + length > size) { call.fail(1336); return; }
			used += length;
		}
		const auto length = uint16_t(8 + sid.size());
		if (used + length > size || count == 0xffff) { call.fail(1344); return; }
		acl[used] = 0x11;
		acl[used + 1] = uint8_t(call.u32(2));
		std::memcpy(acl.data() + used + 2, &length, 2);
		const auto mask = call.u32(3);
		std::memcpy(acl.data() + used + 4, &mask, 4);
		std::memcpy(acl.data() + used + 8, sid.data(), sid.size());
		++count;
		std::memcpy(acl.data() + 4, &count, 2);
		acl[0] = std::max(acl[0], uint8_t(call.u32(1)));
		call.write(call[0], acl);
		call.ret(1);
	});
	const auto add_ace = [](ApiCall &call, uint8_t type) {
		if (!valid_sid(call, call[3])) {
			call.fail(1337);
			return;
		}
		const auto header = call.read(call[0], acl_header);
		uint16_t size, count;
		std::memcpy(&size, header.data() + 2, 2);
		std::memcpy(&count, header.data() + 4, 2);
		uint32_t used = acl_header;
		for (uint16_t index = 0; index < count; ++index) {
			const auto entry = call.read(call[0] + used, 4);
			uint16_t entry_size;
			std::memcpy(&entry_size, entry.data() + 2, 2);
			used += entry_size;
		}
		const auto length = sid_length(call, call[3]);
		const uint32_t entry_size = 8 + length;
		if (used + entry_size > size) {
			call.fail(122);
			return;
		}
		std::vector<uint8_t> entry(entry_size);
		entry[0] = type;
		const uint16_t entry_size16 = uint16_t(entry_size);
		const uint32_t mask = call.u32(2);
		std::memcpy(entry.data() + 2, &entry_size16, 2);
		std::memcpy(entry.data() + 4, &mask, 4);
		const auto sid = call.read(call[3], length);
		std::memcpy(entry.data() + 8, sid.data(), length);
		call.write(call[0] + used, entry);
		call.store<uint16_t>(call[0] + 4, uint16_t(count + 1));
		call.ret(1);
	};
	// Token information: the account is one fixed user in session 1, an administrator under User Account Control: medium integrity and a
	// limited token, or high integrity and the full token when the process runs elevated.
	registry.add("GetTokenInformation", 5, [&objects, &context](ApiCall &call) {
		// The pseudo handles of the process token, the thread token and the thread's effective token name the program's own token.
		const int32_t pseudo = int32_t(uint32_t(call[0]));
		const auto impersonation = context.threads.current().impersonation;
		const auto token = pseudo == -4 ? context.process_token : pseudo == -5 ? impersonation :
			pseudo == -6 ? (impersonation ? impersonation : context.process_token) : objects.object_as<AccessToken>(call[0]);
		if (!token) {
			call.fail(pseudo == -5 ? 1008 : 6);
			return;
		}
		if ((pseudo < -6 || pseudo > -4) && !(objects.access_of(call[0]) & 8)) {
			call.fail(5);
			return;
		}
		const auto value = token->data();
		const size_t width = call.pointer_width();
		const size_t record = width == 4 ? 8 : 16;
		std::vector<uint8_t> data;
		// A structure that holds a SID: the pointer sits in the structure and points at the SID placed after it.
		const auto with_sid = [&](const std::vector<uint8_t> &sid, uint32_t attributes, bool pair) {
			const size_t head = pair ? record : width;
			data.assign(head + sid.size(), 0);
			const uint64_t at = call[2] + head;
			std::memcpy(data.data(), &at, width);
			if (pair)
				std::memcpy(data.data() + width, &attributes, 4);
			std::memcpy(data.data() + head, sid.data(), sid.size());
		};
		switch (call.u32(1)) {
		case 1: with_sid(user_sid(), 0, true); break;
		case 3: {
			data.assign(4 + 12 * value.privileges.size(), 0);
			const auto count = uint32_t(value.privileges.size());
			std::memcpy(data.data(), &count, 4);
			for (size_t index = 0; index < value.privileges.size(); ++index) {
				std::memcpy(data.data() + 4 + 12 * index, &value.privileges[index].luid, 8);
				std::memcpy(data.data() + 12 + 12 * index, &value.privileges[index].attributes, 4);
			}
			break;
		}
		case 4: with_sid(user_sid(), 0, false); break;
		case 5: with_sid(make_sid(5, {21, 1000, 1000, 1000, 513}), 0, false); break;
		case 6: {
			data.assign(width, 0);
			if (!value.default_dacl.empty()) {
				const uint64_t at = call[2] + width;
				std::memcpy(data.data(), &at, width);
				data.insert(data.end(), value.default_dacl.begin(), value.default_dacl.end());
			}
			break;
		}
		case 2: case 28: {
			const bool logon = call.u32(1) == 28;
			// The logon SID identifies the same session for every process in this runtime.
			const auto session = make_sid(5, {5, 0, 1});
			std::vector<std::vector<uint8_t>> sids{session};
			// The groups of an interactive desktop logon: programs look for INTERACTIVE to tell a user's session from a service.
			if (!logon) {
				sids.insert(sids.begin(), {make_sid(5, {32, 545}), make_sid(5, {32, 544}), make_sid(1, {0})});
				sids.insert(sids.end(), {make_sid(5, {4}), make_sid(2, {1}), make_sid(5, {11}), make_sid(5, {15}), make_sid(2, {0})});
			}
			const size_t first = width == 4 ? 4 : 8;
			size_t total = first + record * sids.size();
			for (const auto &item : sids)
				total += item.size();
			data.assign(total, 0);
			const uint32_t count = uint32_t(sids.size());
			std::memcpy(data.data(), &count, 4);
			size_t placed = first + record * sids.size();
			for (size_t index = 0; index < sids.size(); ++index) {
				// Users and Everyone: mandatory, enabled by default and enabled. The account is an administrator under User Account
				// Control: Administrators is enabled (and an owner) in an elevated process and held for denying access only otherwise.
				const uint32_t attributes = logon || index == 3 ? 0xc0000007U : index == 1 ? (process_elevated() ? 0xf : 0x10) : 7;
				const uint64_t at = call[2] + placed;
				std::memcpy(data.data() + first + index * record, &at, width);
				std::memcpy(data.data() + first + index * record + width, &attributes, 4);
				std::memcpy(data.data() + placed, sids[index].data(), sids[index].size());
				placed += sids[index].size();
			}
			break;
		}
		case 8: data = {uint8_t(value.type), 0, 0, 0}; break;
		case 9:
			if (value.type != 2) { call.fail(87); return; }
			data = {uint8_t(value.impersonation_level), 0, 0, 0};
			break;
		case 12: data = {1, 0, 0, 0}; break;
		// TokenElevationType: full for an elevated process, limited (the filtered token of an administrator) otherwise; TokenElevation.
		case 18: data = {process_elevated() ? uint8_t(2) : uint8_t(3), 0, 0, 0}; break;
		case 20: data = {process_elevated() ? uint8_t(1) : uint8_t(0), 0, 0, 0}; break;
		// TokenUIAccess and TokenIsAppContainer: no process here is an accessibility tool or runs in an app container.
		case 26: case 29: data = {0, 0, 0, 0}; break;
		case 25: with_sid(make_sid(16, {value.integrity}), 0x60, true); break;
		case 10: data.assign(56, 0); break;
		default:
			call.fail(87);
			return;
		}
		if (call[4])
			call.store<uint32_t>(call[4], uint32_t(data.size()));
		if (!call[2] || call.u32(3) < data.size()) {
			call.fail(122);
			return;
		}
		call.write(call[2], data);
		call.ret(1);
	});
	// A generic right becomes the specific rights the mapping names for it.
	registry.add("MapGenericMask", 2, [](ApiCall &call) {
		if (!call[0] || !call[1]) {
			call.fail(87);
			return;
		}
		auto mask = call.load<uint32_t>(call[0]);
		const auto mapping = call.read(call[1], 16);
		uint32_t specific[4];
		std::memcpy(specific, mapping.data(), 16);
		if (mask & 0x80000000U) mask |= specific[0];
		if (mask & 0x40000000U) mask |= specific[1];
		if (mask & 0x20000000U) mask |= specific[2];
		if (mask & 0x10000000U) mask |= specific[3];
		mask &= ~0xf0000000U;
		call.store<uint32_t>(call[0], mask);
	});
	// Descriptors are not kept: every object answers with the same one, so setting one changes nothing that could be read back.
	registry.add("SetSecurityInfo", 7, [&objects](ApiCall &call) { call.ret(0); });
	registry.add("SetNamedSecurityInfoW", 7, [](ApiCall &call) { call.ret(0); });
	// The access a descriptor grants to the token's account: the entries of its list in order, allow entries adding rights and deny entries
	// refusing them, for the account, its group, the users of the machine and everyone.
	registry.add("AccessCheck", 8, [&objects](ApiCall &call) {
		if (!objects.object_as<AccessToken>(call[1]) || !call[0] || !call[6] || !call[7]) {
			call.fail(call[0] && call[6] && call[7] ? 6 : 87);
			return;
		}
		uint32_t wanted = call.u32(2);
		const auto mapping = call.read(call[3], 16);
		uint32_t specific[4];
		std::memcpy(specific, mapping.data(), 16);
		if (wanted & 0x80000000U) wanted |= specific[0];
		if (wanted & 0x40000000U) wanted |= specific[1];
		if (wanted & 0x20000000U) wanted |= specific[2];
		if (wanted & 0x10000000U) wanted |= specific[3];
		wanted &= ~0xf0000000U;
		const uint64_t descriptor = call[0];
		const uint16_t control = call.load<uint16_t>(descriptor + 2);
		uint64_t acl = 0;
		if (control & dacl_present) {
			if (control & 0x8000) {
				const auto offset = call.load<uint32_t>(descriptor + 16);
				acl = offset ? descriptor + offset : 0;
			} else {
				acl = call.load_pointer(descriptor + layout_of(call).dacl);
			}
		}
		// A descriptor without a list gives everyone everything.
		uint32_t granted = 0, denied = 0;
		if (!(control & dacl_present) || !acl) {
			granted = wanted ? wanted : 0x1f01ffU;
		} else {
			const auto header = call.read(acl, acl_header);
			uint16_t count;
			std::memcpy(&count, header.data() + 4, 2);
			uint64_t at = acl + acl_header;
			std::vector<std::vector<uint8_t>> mine{user_sid(), make_sid(5, {21, 1000, 1000, 1000, 513}), make_sid(5, {32, 545}), make_sid(1, {0}),
				make_sid(5, {11}), make_sid(5, {4}), make_sid(5, {1})};
			// Administrators grants access only to an elevated token; the filtered token holds it for denying.
			if (process_elevated())
				mine.push_back(make_sid(5, {32, 544}));
			for (uint16_t index = 0; index < count; ++index) {
				const auto entry = call.read(at, 8);
				uint16_t size;
				std::memcpy(&size, entry.data() + 2, 2);
				uint32_t mask;
				std::memcpy(&mask, entry.data() + 4, 4);
				if (size < 12 || !valid_sid(call, at + 8)) {
					at += size ? size : 8;
					continue;
				}
				const auto sid = call.read(at + 8, sid_length(call, at + 8));
				const bool applies = std::any_of(mine.begin(), mine.end(), [&](const auto &candidate) { return candidate == sid; });
				if (applies && entry[0] == 0)
					granted |= mask & ~denied;
				else if (applies && entry[0] == 1)
					denied |= mask & ~granted;
				at += size;
			}
		}
		// MAXIMUM_ALLOWED asks for everything the descriptor grants.
		const bool maximum = wanted & 0x02000000U;
		const bool allowed = maximum ? granted != 0 : wanted && (granted & wanted) == wanted;
		call.store<uint32_t>(call[6], maximum ? granted : allowed ? wanted : 0);
		call.store<uint32_t>(call[7], allowed);
		if (!allowed)
			call.set_last_error(5);
		call.ret(1);
	});
	// Copies own their DACL and integrity level; changing one leaves the source token unchanged.
	registry.add("DuplicateTokenEx", 6, [&objects](ApiCall &call) {
		if (!objects.object_as<AccessToken>(call[0]) || !call[5]) {
			call.fail(call[5] ? 6 : 87);
			return;
		}
		if (!(objects.access_of(call[0]) & 2)) {
			call.fail(5);
			return;
		}
		const auto access = token_access(call.u32(1));
		if (!access || call.u32(3) > 3 || (call.u32(4) != 1 && call.u32(4) != 2)) {
			call.fail(87);
			return;
		}
		auto token = std::make_shared<AccessToken>(*objects.object_as<AccessToken>(call[0]));
		token->set_type(call.u32(4));
		token->set_impersonation_level(call.u32(3));
		const auto made = objects.add_object(token);
		if (made.status) {
			call.fail(8);
			return;
		}
		objects.set_access(made.value, call.u32(1) ? *access : objects.access_of(call[0]));
		call.store_pointer(call[5], made.value);
		call.ret(1);
	});
	registry.add("DuplicateToken", 3, [&objects](ApiCall &call) {
		if (!objects.object_as<AccessToken>(call[0]) || !call[2]) {
			call.fail(call[2] ? 6 : 87);
			return;
		}
		if (!(objects.access_of(call[0]) & 2)) {
			call.fail(5);
			return;
		}
		if (call.u32(1) > 3) { call.fail(87); return; }
		auto token = std::make_shared<AccessToken>(*objects.object_as<AccessToken>(call[0]));
		token->set_type(2);
		token->set_impersonation_level(call.u32(1));
		const auto made = objects.add_object(token);
		if (made.status) {
			call.fail(8);
			return;
		}
		objects.set_access(made.value, objects.access_of(call[0]));
		call.store_pointer(call[2], made.value);
		call.ret(1);
	});
	registry.add("CreateRestrictedToken", 9, [&objects](ApiCall &call) {
		const auto source = objects.object_as<AccessToken>(call[0]);
		if (!source || !call[8]) {
			call.fail(call[8] ? 6 : 87);
			return;
		}
		if (!(objects.access_of(call[0]) & 2)) { call.fail(5); return; }
		if (call.u32(1) & ~0xfU) { call.fail(87); return; }
		call.write(call[8], call.read(call[8], call.pointer_width()));
		std::vector<uint64_t> removed;
		if (!(call.u32(1) & 1)) {
			const auto count = call.u32(4);
			if (count > 64 || (count && !call[5])) { call.fail(87); return; }
			const auto entries = call.read(call[5], size_t(count) * 12);
			for (uint32_t index = 0; index < count; ++index) {
				uint64_t luid;
				std::memcpy(&luid, entries.data() + 12 * index, 8);
				removed.push_back(luid);
			}
		}
		auto token = std::make_shared<AccessToken>(*source);
		token->restrict_privileges(call.u32(1) & 1, removed);
		const auto made = objects.add_object(token);
		if (made.status) {
			call.fail(8);
			return;
		}
		objects.set_access(made.value, objects.access_of(call[0]));
		call.store_pointer(call[8], made.value);
		call.ret(1);
	});
	registry.add("SetTokenInformation", 4, [&objects](ApiCall &call) {
		const auto token = objects.object_as<AccessToken>(call[0]);
		if (!token) {
			call.fail(6);
			return;
		}
		if (!(objects.access_of(call[0]) & 0x80)) {
			call.fail(5);
			return;
		}
		if (!call[2] || call.u32(3) < call.pointer_width()) {
			call.fail(87);
			return;
		}
		const uint64_t at = call.load_pointer(call[2]);
		if (call.u32(1) == 6) {
			std::vector<uint8_t> acl;
			if (at) {
				const auto size = call.load<uint16_t>(at + 2), count = call.load<uint16_t>(at + 4);
				const auto revision = call.load<uint8_t>(at);
				if (size < acl_header || (revision != 2 && revision != 4)) {
					call.fail(1336);
					return;
				}
				acl = call.read(at, size);
				size_t used = acl_header;
				for (uint16_t index = 0; index < count; ++index) {
					uint16_t entry = 0;
					if (used + 4 <= acl.size())
						std::memcpy(&entry, acl.data() + used + 2, 2);
					if (entry < 4 || used + entry > acl.size()) {
						call.fail(1336);
						return;
					}
					used += entry;
				}
			}
			token->set_default_dacl(acl);
		} else if (call.u32(1) == 25) {
			const auto authority = make_sid(16, {0});
			if (call.u32(3) < (call.pointer_width() == 4 ? 8 : 16) || !valid_sid(call, at) ||
				call.read(at, 8) != std::vector<uint8_t>(authority.begin(), authority.begin() + 8)) {
				call.fail(1337);
				return;
			}
			token->set_integrity(call.load<uint32_t>(at + 8));
		} else {
			call.fail(87);
			return;
		}
		call.ret(1);
	});
	// The SIDs that exist on every system, by their WELL_KNOWN_SID_TYPE; none for a type this system does not name without a domain.
	const auto well_known_sid = [](uint32_t type) {
		std::vector<uint8_t> sid;
		switch (type) {
		case 0: sid = make_sid(0, {0}); break;
		case 1: sid = make_sid(1, {0}); break;
		case 2: sid = make_sid(2, {0}); break;
		case 11: sid = make_sid(5, {4}); break;
		case 12: sid = make_sid(5, {6}); break;
		case 17: sid = make_sid(5, {11}); break;
		case 18: sid = make_sid(5, {12}); break;
		case 36: sid = make_sid(5, {32, 555}); break;
		case 22: sid = make_sid(5, {18}); break;
		case 23: sid = make_sid(5, {19}); break;
		case 24: sid = make_sid(5, {20}); break;
		case 26: sid = make_sid(5, {32, 544}); break;
		case 27: sid = make_sid(5, {32, 545}); break;
		case 66: sid = make_sid(16, {4096}); break;
		case 67: sid = make_sid(16, {8192}); break;
		case 68: sid = make_sid(16, {12288}); break;
		case 69: sid = make_sid(16, {16384}); break;
		default: break;
		}
		return sid;
	};
	registry.add("CreateWellKnownSid", 4, [well_known_sid](ApiCall &call) {
		const auto sid = well_known_sid(call.u32(0));
		if (sid.empty()) {
			call.fail(87);
			return;
		}
		if (!call[3]) {
			call.fail(87);
			return;
		}
		const uint32_t capacity = call.load<uint32_t>(call[3]);
		call.store<uint32_t>(call[3], uint32_t(sid.size()));
		if (!call[2] || capacity < sid.size()) {
			call.fail(122);
			return;
		}
		call.write(call[2], sid);
		call.ret(1);
	});
	registry.add("IsWellKnownSid", 2, [well_known_sid](ApiCall &call) {
		if (!valid_sid(call, call[0])) {
			call.ret(0);
			return;
		}
		const auto sid = well_known_sid(call.u32(1));
		call.ret(!sid.empty() && sid_length(call, call[0]) == sid.size() && call.read(call[0], sid.size()) == sid);
	});
	// A descriptor as one self-relative block that the program frees with LocalFree: the owner, group and ACL pointers point inside it.
	struct Entry {
		uint8_t flags;
		uint32_t mask;
		std::vector<uint8_t> sid;
	};
	const auto answer_descriptor = [&context](ApiCall &call, const std::vector<uint8_t> &owner, const std::vector<uint8_t> &group,
		const std::vector<Entry> &entries, uint16_t inheritance) {
		const uint32_t wanted = call.u32(2);
		std::vector<uint8_t> acl(acl_header);
		acl[0] = 2;
		for (const auto &entry : entries) {
			const uint16_t size = uint16_t(8 + entry.sid.size());
			const size_t at = acl.size();
			acl.resize(at + size);
			acl[at + 1] = entry.flags;
			std::memcpy(acl.data() + at + 2, &size, 2);
			std::memcpy(acl.data() + at + 4, &entry.mask, 4);
			std::memcpy(acl.data() + at + 8, entry.sid.data(), entry.sid.size());
		}
		const uint16_t acl_size = uint16_t(acl.size()), count = uint16_t(entries.size());
		std::memcpy(acl.data() + 2, &acl_size, 2);
		std::memcpy(acl.data() + 4, &count, 2);
		std::vector<uint8_t> block(20);
		const auto place = [&](const std::vector<uint8_t> &bytes) {
			const uint32_t at = uint32_t(block.size());
			block.insert(block.end(), bytes.begin(), bytes.end());
			while (block.size() % 4)
				block.push_back(0);
			return at;
		};
		const uint32_t owner_at = place(owner), group_at = place(group), dacl_at = place(acl);
		const uint16_t control = 0x8000 | dacl_present | inheritance;
		block[0] = 1;
		std::memcpy(block.data() + 2, &control, 2);
		std::memcpy(block.data() + 4, &owner_at, 4);
		std::memcpy(block.data() + 8, &group_at, 4);
		std::memcpy(block.data() + 16, &dacl_at, 4);
		const auto memory = context.allocator.allocate(block.size(), true);
		if (!memory) {
			call.ret(8);
			return;
		}
		call.write(memory, block);
		if (call[3] && (wanted & 1))
			call.store_pointer(call[3], memory + owner_at);
		if (call[4] && (wanted & 2))
			call.store_pointer(call[4], memory + group_at);
		if (call[5] && (wanted & 4))
			call.store_pointer(call[5], memory + dacl_at);
		if (call[6] && (wanted & 8))
			call.store_pointer(call[6], 0);
		if (call[7])
			call.store_pointer(call[7], memory);
		else
			context.allocator.release(memory);
		call.ret(0);
	};
	// Objects other than files are owned by the account, and readable and usable by everyone.
	const auto security_info = [answer_descriptor](ApiCall &call) {
		answer_descriptor(call, user_sid(), make_sid(5, {21, 1000, 1000, 1000, 513}), {{0, 0x1f01ffU, make_sid(1, {0})}}, 0);
	};
	// A file or directory carries the list Windows gives it. The drive's root, Windows and the Program Files folders belong to
	// TrustedInstaller; what an installer creates in them inherits their list and belongs to the administrators. Only those, SYSTEM and
	// TrustedInstaller may change them, the users may read and run them, and programs such as installers check that before they trust a
	// folder. Everywhere else the account owns what it has, with SYSTEM and the administrators.
	const auto file_security = [answer_descriptor](ApiCall &call, std::u16string path, bool directory) {
		if (path.starts_with(u"\\\\?\\"))
			path.erase(0, 4);
		for (auto &character : path)
			character = character == u'/' ? u'\\' : character >= u'a' && character <= u'z' ? char16_t(character - 32) : character;
		while (path.size() > 2 && path.back() == u'\\')
			path.pop_back();
		const auto system = make_sid(5, {18}), administrators = make_sid(5, {32, 544}), users = make_sid(5, {32, 545});
		const auto installer = make_sid(5, {80, 956008885, 3418522649U, 1831038044, 1853292631, 2271478464U});
		const uint32_t full = 0x1f01ffU, read_run = 0x1200a9U, modify = 0x1301bfU;
		// Entries of a directory pass to what is made in it (objects and containers); an entry only for those does not apply to the folder.
		const uint8_t passed = directory ? 0x3 : 0, only_below = 0xb, inherited = 0x10;
		if (path.size() == 2 && path[1] == u':') {
			answer_descriptor(call, installer, system, {{passed, full, administrators}, {passed, full, system}, {passed, read_run, users},
				{0, 0x4, make_sid(5, {11})}, {only_below, modify, make_sid(5, {11})}}, 0x1000);
			return;
		}
		std::u16string_view top;
		for (const std::u16string_view folder : {u"C:\\WINDOWS", u"C:\\PROGRAM FILES", u"C:\\PROGRAM FILES (X86)"})
			if (path.starts_with(folder) && (path.size() == folder.size() || path[folder.size()] == u'\\'))
				top = folder;
		if (!top.empty()) {
			const bool own = path.size() == top.size();
			const uint8_t from = own ? 0 : inherited;
			std::vector<Entry> entries{{uint8_t(passed | from), full, installer}, {uint8_t(passed | from), full, system},
				{uint8_t(passed | from), full, administrators}, {uint8_t(passed | from), read_run, users},
				{uint8_t(passed | from), read_run, make_sid(15, {2, 1})}};
			if (directory)
				entries.push_back({uint8_t(only_below | from), 0x10000000U, make_sid(3, {0})});
			answer_descriptor(call, own ? installer : administrators, system, entries, own ? 0x1400 : 0x400);
			return;
		}
		answer_descriptor(call, user_sid(), make_sid(5, {21, 1000, 1000, 1000, 513}),
			{{uint8_t(passed | inherited), full, system}, {uint8_t(passed | inherited), full, administrators},
				{uint8_t(passed | inherited), full, user_sid()}}, 0x400);
	};
	// GetSecurityInfo of a file asks the handle's file.
	registry.add("GetSecurityInfo", 8, [&objects, security_info, file_security](ApiCall &call) {
		if (call.u32(1) == 1)
			if (const auto file = objects.object_as<FileObject>(call[0])) {
				file_security(call, file->dos_path, file->directory);
				return;
			}
		security_info(call);
	});
	// A file or directory that is not there has no descriptor to give.
	registry.add("GetNamedSecurityInfoW", 8, [&context, security_info, file_security](ApiCall &call) {
		if (call.u32(1) == 1) {
			const auto name = call.wide(call[0], 32768);
			const auto resolved = context.paths.resolve(name);
			if (!resolved || !resolved->exists) {
				call.ret(resolved && !resolved->parent_exists ? 3 : 2);
				return;
			}
			std::error_code error;
			file_security(call, context.paths.full(name), std::filesystem::is_directory(resolved->host, error));
			return;
		}
		security_info(call);
	});
	// The descriptor of a kernel object, self-relative: the user owns what the process made, with the user's domain group, and a DACL that is
	// present but null leaves the object open to everyone, which is how the runtime checks access to its objects.
	registry.add("NtQuerySecurityObject", 5, [&objects](ApiCall &call) {
		if (call[0] != UINT64_MAX && call[0] != UINT64_MAX - 1 && !objects.object(call[0])) {
			call.ret(0xc0000008U);
			return;
		}
		const uint32_t wanted = call.u32(1);
		const auto sid = [](uint32_t last) {
			std::vector<uint8_t> bytes{1, 5, 0, 0, 0, 0, 0, 5};
			for (const uint32_t value : {21u, 1000u, 1000u, 1000u, last})
				for (int shift = 0; shift < 32; shift += 8)
					bytes.push_back(uint8_t(value >> shift));
			return bytes;
		};
		std::vector<uint8_t> descriptor(20, 0);
		descriptor[0] = 1;
		uint16_t control = 0x8000;
		const auto attach = [&](size_t offset_at, const std::vector<uint8_t> &part) {
			const uint32_t offset = uint32_t(descriptor.size());
			std::memcpy(descriptor.data() + offset_at, &offset, 4);
			descriptor.insert(descriptor.end(), part.begin(), part.end());
		};
		if (wanted & 1)
			attach(4, sid(1000));
		if (wanted & 2)
			attach(8, sid(513));
		if (wanted & 4)
			control |= 0x4;
		std::memcpy(descriptor.data() + 2, &control, 2);
		if (call[4])
			call.store<uint32_t>(call[4], uint32_t(descriptor.size()));
		if (call.u32(3) < descriptor.size()) {
			call.ret(0xc0000023U);
			return;
		}
		call.write(call[2], descriptor);
		call.ret(0);
	});
	// Objects keep no descriptor of their own, so a new one is accepted for any object that exists.
	registry.add("NtSetSecurityObject", 3, [&objects](ApiCall &call) {
		const bool known = call[0] == UINT64_MAX || call[0] == UINT64_MAX - 1 || objects.object(call[0]);
		call.ret(!known ? 0xc0000008U : !call[2] ? 0xc000000dU : 0);
	});
	// No job objects exist by name here, so opening one finds nothing.
	registry.add("NtOpenJobObject", 3, [](ApiCall &call) { call.ret(0xc0000034U); });
	// Reading a descriptor: the control bits say whether its parts are offsets from its start (self-relative) or pointers.
	const auto part = [](ApiCall &call, uint64_t descriptor, size_t index) -> uint64_t {
		const uint16_t control = call.load<uint16_t>(descriptor + 2);
		if (control & 0x8000) {
			const uint32_t offset = call.load<uint32_t>(descriptor + 4 + index * 4);
			return offset ? descriptor + offset : 0;
		}
		const size_t width = call.pointer_width(), first = width == 4 ? 4 : 8;
		return call.load_pointer(descriptor + first + index * width);
	};
	registry.add("GetSecurityDescriptorControl", 3, [](ApiCall &call) {
		if (!call[0]) {
			call.fail(87);
			return;
		}
		if (call[1])
			call.store<uint16_t>(call[1], call.load<uint16_t>(call[0] + 2));
		if (call[2])
			call.store<uint32_t>(call[2], call.read(call[0], 1)[0]);
		call.ret(1);
	});
	// Only the inheritance bits may be changed this way: automatic inheritance asked for or done, protection, and the resource manager bits.
	registry.add("SetSecurityDescriptorControl", 3, [](ApiCall &call) {
		const uint16_t interest = uint16_t(call.u32(1)), bits = uint16_t(call.u32(2)), allowed = 0x7f00;
		if (!call[0] || (interest & ~allowed) || (bits & ~allowed)) {
			call.fail(87);
			return;
		}
		const uint16_t control = call.load<uint16_t>(call[0] + 2);
		call.store<uint16_t>(call[0] + 2, uint16_t((control & ~interest) | (bits & interest)));
		call.ret(1);
	});
	const auto owner_or_group = [part](ApiCall &call, size_t index, uint16_t defaulted) {
		if (!call[0] || !call[1] || !call[2]) {
			call.fail(87);
			return;
		}
		call.store_pointer(call[1], part(call, call[0], index));
		call.store<uint32_t>(call[2], (call.load<uint16_t>(call[0] + 2) & defaulted) != 0);
		call.ret(1);
	};
	registry.add("GetSecurityDescriptorOwner", 3, [owner_or_group](ApiCall &call) { owner_or_group(call, 0, owner_defaulted); });
	registry.add("GetSecurityDescriptorGroup", 3, [owner_or_group](ApiCall &call) { owner_or_group(call, 1, group_defaulted); });
	const auto acl_part = [part](ApiCall &call, size_t index, uint16_t present, uint16_t defaulted) {
		if (!call[0] || !call[1] || !call[2] || !call[3]) {
			call.fail(87);
			return;
		}
		const uint16_t control = call.load<uint16_t>(call[0] + 2);
		const bool has = (control & present) != 0;
		call.store<uint32_t>(call[1], has);
		call.store_pointer(call[2], has ? part(call, call[0], index) : 0);
		call.store<uint32_t>(call[3], (control & defaulted) != 0);
		call.ret(1);
	};
	registry.add("GetSecurityDescriptorSacl", 4, [acl_part](ApiCall &call) { acl_part(call, 2, sacl_present, sacl_defaulted); });
	registry.add("GetSecurityDescriptorDacl", 4, [acl_part](ApiCall &call) { acl_part(call, 3, dacl_present, dacl_defaulted); });
	registry.add("GetSecurityDescriptorLength", 1, [part](ApiCall &call) {
		// A self-relative descriptor ends where its last part ends; an absolute one counts its header and every part it points to, as
		// much as the self-relative copy of it needs.
		const auto control = call.load<uint16_t>(call[0] + 2);
		if (!(control & 0x8000)) {
			uint64_t total = layout_of(call).size;
			for (size_t index = 0; index < 4; ++index)
				if (const auto at = part(call, call[0], index))
					total += index < 2 ? 8 + 4 * uint64_t(call.read(at + 1, 1)[0]) : call.load<uint16_t>(at + 2);
			call.ret(total);
			return;
		}
		uint64_t end = 20;
		for (size_t index = 0; index < 4; ++index) {
			const auto at = part(call, call[0], index);
			if (!at)
				continue;
			const uint64_t offset = at - call[0];
			uint64_t size = 0;
			if (index < 2)
				size = 8 + 4 * uint64_t(call.read(at + 1, 1)[0]);
			else
				size = call.load<uint16_t>(at + 2);
			end = std::max(end, offset + size);
		}
		call.ret(end);
	});
	// The descriptor packed into one block: a 20-byte header whose four offsets lead to the owner, the group, the SACL and the DACL
	// that follow it, in that order.
	registry.add("MakeSelfRelativeSD", 3, [part](ApiCall &call) {
		if (!call[0] || !call[2]) {
			call.fail(87);
			return;
		}
		const auto control = call.load<uint16_t>(call[0] + 2);
		if (control & 0x8000) {
			call.fail(1361);
			return;
		}
		std::vector<uint8_t> block(20);
		block[0] = 1;
		const uint16_t packed = uint16_t(control | 0x8000);
		std::memcpy(block.data() + 2, &packed, 2);
		for (size_t index = 0; index < 4; ++index) {
			const auto at = part(call, call[0], index);
			if (!at)
				continue;
			const uint32_t size = index < 2 ? 8 + 4 * uint32_t(call.read(at + 1, 1)[0]) : call.load<uint16_t>(at + 2);
			const uint32_t offset = uint32_t(block.size());
			std::memcpy(block.data() + 4 + 4 * index, &offset, 4);
			const auto bytes = call.read(at, size);
			block.insert(block.end(), bytes.begin(), bytes.end());
		}
		const uint32_t capacity = call.load<uint32_t>(call[2]);
		call.store<uint32_t>(call[2], uint32_t(block.size()));
		if (!call[1] || capacity < block.size()) {
			call.fail(122);
			return;
		}
		call.write(call[1], block);
		call.ret(1);
	});
	registry.add("IsValidAcl", 1, [](ApiCall &call) {
		if (!call[0]) {
			call.ret(0);
			return;
		}
		const auto header = call.read(call[0], 8);
		call.ret(header[0] >= 2 && header[0] <= 4);
	});
	registry.add("GetAclInformation", 4, [](ApiCall &call) {
		if (!call[0] || !call[1]) {
			call.fail(87);
			return;
		}
		const auto header = call.read(call[0], 8);
		uint16_t size, count;
		std::memcpy(&size, header.data() + 2, 2);
		std::memcpy(&count, header.data() + 4, 2);
		if (call.u32(3) == 1 && call.u32(2) >= 4) {
			call.store<uint32_t>(call[1], header[0]);
		} else if (call.u32(3) == 2 && call.u32(2) >= 12) {
			uint32_t used = 8;
			for (uint16_t index = 0; index < count; ++index) {
				uint16_t entry;
				std::memcpy(&entry, call.read(call[0] + used + 2, 2).data(), 2);
				used += entry;
			}
			call.store<uint32_t>(call[1], count);
			call.store<uint32_t>(call[1] + 4, used);
			call.store<uint32_t>(call[1] + 8, size > used ? size - used : 0);
		} else {
			call.fail(24);
			return;
		}
		call.ret(1);
	});
	registry.add("GetAce", 3, [](ApiCall &call) {
		if (!call[0] || !call[2]) {
			call.fail(87);
			return;
		}
		const auto header = call.read(call[0], 8);
		uint16_t count;
		std::memcpy(&count, header.data() + 4, 2);
		if (call.u32(1) >= count) {
			call.fail(87);
			return;
		}
		uint32_t at = 8;
		for (uint32_t index = 0; index < call.u32(1); ++index) {
			uint16_t entry;
			std::memcpy(&entry, call.read(call[0] + at + 2, 2).data(), 2);
			at += entry;
		}
		call.store_pointer(call[2], call[0] + at);
		call.ret(1);
	});
	// The entries after the one removed move down over it; the list holds one entry less.
	registry.add("DeleteAce", 2, [](ApiCall &call) {
		if (!call[0]) {
			call.fail(87);
			return;
		}
		const auto header = call.read(call[0], 8);
		uint16_t size, count;
		std::memcpy(&size, header.data() + 2, 2);
		std::memcpy(&count, header.data() + 4, 2);
		if (call.u32(1) >= count) {
			call.fail(87);
			return;
		}
		uint32_t at = 8, removed = 0, end = 8;
		for (uint32_t index = 0; index < count; ++index) {
			uint16_t entry;
			std::memcpy(&entry, call.read(call[0] + end + 2, 2).data(), 2);
			if (index == call.u32(1)) {
				at = end;
				removed = entry;
			}
			end += entry;
		}
		if (end > size) {
			call.fail(1336);
			return;
		}
		const auto rest = call.read(call[0] + at + removed, end - at - removed);
		call.write(call[0] + at, rest);
		call.store<uint16_t>(call[0] + 4, uint16_t(count - 1));
		call.ret(1);
	});
	registry.add("GetSidSubAuthorityCount", 1, [](ApiCall &call) { call.ret(call[0] + 1); });
	registry.add("GetSidIdentifierAuthority", 1, [](ApiCall &call) { call.ret(call[0] + 2); });
	registry.add("GetSidSubAuthority", 2, [](ApiCall &call) { call.ret(call[0] + 8 + 4 * uint64_t(call.u32(1))); });
	registry.add("GetSidLengthRequired", 1, [](ApiCall &call) { call.ret(8 + 4 * uint32_t(uint8_t(call.u32(0)))); });
	// S-R-I-S-S...: the string form of a SID, in memory the program frees with LocalFree.
	for (const bool wide : {true, false})
		registry.add(wide ? "ConvertSidToStringSidW" : "ConvertSidToStringSidA", 2, [&context, wide](ApiCall &call) {
			if (!valid_sid(call, call[0]) || !call[1]) {
				call.fail(87);
				return;
			}
			const auto sid = call.read(call[0], sid_length(call, call[0]));
			uint64_t authority = 0;
			for (int index = 2; index < 8; ++index)
				authority = authority << 8 | sid[size_t(index)];
			std::string text = "S-1-" + std::to_string(authority);
			for (size_t index = 0; index < sid[1]; ++index) {
				uint32_t value;
				std::memcpy(&value, sid.data() + 8 + index * 4, 4);
				text += "-" + std::to_string(value);
			}
			const size_t unit = wide ? 2 : 1;
			const auto memory = context.allocator.allocate((text.size() + 1) * unit, true);
			if (!memory) {
				call.fail(8);
				return;
			}
			std::vector<uint8_t> bytes((text.size() + 1) * unit);
			for (size_t index = 0; index < text.size(); ++index)
				bytes[index * unit] = uint8_t(text[index]);
			call.write(memory, bytes);
			call.store_pointer(call[1], memory);
			call.ret(1);
		});
	// The string form of a security descriptor to a self-relative one in memory the program frees with LocalFree.
	for (const bool wide : {true, false})
		registry.add(wide ? "ConvertStringSecurityDescriptorToSecurityDescriptorW" : "ConvertStringSecurityDescriptorToSecurityDescriptorA", 4,
			[&context, wide](ApiCall &call) {
				if (!call[0] || !call[2]) {
					call.fail(87);
					return;
				}
				if (call.u32(1) != 1) {
					call.fail(1305);
					return;
				}
				const auto text = wide ? call.wide(call[0], 65536) : from_utf8(call.narrow(call[0], 65536));
				const auto descriptor = descriptor_from_text(text);
				if (!descriptor) {
					call.fail(1336);
					return;
				}
				const auto memory = context.allocator.allocate(descriptor->size(), true);
				if (!memory) {
					call.fail(8);
					return;
				}
				call.write(memory, *descriptor);
				call.store_pointer(call[2], memory);
				if (call[3])
					call.store<uint32_t>(call[3], uint32_t(descriptor->size()));
				call.ret(1);
			});
	// A security descriptor, self-relative or absolute, in its string form: the parts the information flags ask for (owner 1, group 2,
	// DACL 4, SACL 8), with the two-letter names Windows writes for well-known accounts and rights.
	for (const bool wide : {true, false})
		registry.add(wide ? "ConvertSecurityDescriptorToStringSecurityDescriptorW" : "ConvertSecurityDescriptorToStringSecurityDescriptorA", 5,
			[&context, wide](ApiCall &call) {
				if (!call[0] || !call[3]) {
					call.fail(87);
					return;
				}
				if (call.u32(1) != 1) {
					call.fail(1305);
					return;
				}
				const uint64_t descriptor = call[0];
				const auto header = call.read(descriptor, 4);
				const uint16_t control = uint16_t(header[2] | header[3] << 8);
				const bool relative = control & 0x8000;
				const size_t width = call.pointer_width();
				const auto part = [&](size_t index) -> uint64_t {
					if (relative) {
						const uint32_t offset = call.load<uint32_t>(descriptor + 4 + index * 4);
						return offset ? descriptor + offset : 0;
					}
					return call.load_pointer(descriptor + (width == 8 ? 8 : 4) + index * width);
				};
				static const std::map<std::string, std::string> names{
					{"S-1-5-7", "AN"}, {"S-1-5-32-548", "AO"}, {"S-1-5-11", "AU"}, {"S-1-5-32-544", "BA"}, {"S-1-5-32-546", "BG"},
					{"S-1-5-32-551", "BO"}, {"S-1-5-32-545", "BU"}, {"S-1-3-1", "CG"}, {"S-1-3-0", "CO"}, {"S-1-5-9", "ED"},
					{"S-1-16-12288", "HI"}, {"S-1-5-4", "IU"}, {"S-1-5-19", "LS"}, {"S-1-16-4096", "LW"}, {"S-1-16-8192", "ME"},
					{"S-1-5-32-556", "NO"}, {"S-1-5-20", "NS"}, {"S-1-5-2", "NU"}, {"S-1-5-32-550", "PO"}, {"S-1-5-10", "PS"},
					{"S-1-5-32-547", "PU"}, {"S-1-5-12", "RC"}, {"S-1-5-32-555", "RD"}, {"S-1-5-32-552", "RE"}, {"S-1-5-32-554", "RU"},
					{"S-1-16-16384", "SI"}, {"S-1-5-32-549", "SO"}, {"S-1-5-6", "SU"}, {"S-1-5-18", "SY"}, {"S-1-1-0", "WD"},
					{"S-1-5-33", "WR"}, {"S-1-3-4", "OW"}, {"S-1-15-2-1", "AC"}, {"S-1-5-21-1000-1000-1000-500", "LA"},
					{"S-1-5-21-1000-1000-1000-501", "LG"}};
				const auto sid_text = [&](uint64_t sid) {
					const auto bytes = call.read(sid, sid_length(call, sid));
					uint64_t authority = 0;
					for (int index = 2; index < 8; ++index)
						authority = authority << 8 | bytes[size_t(index)];
					std::string text = "S-1-" + std::to_string(authority);
					for (size_t index = 0; index < bytes[1]; ++index) {
						uint32_t value;
						std::memcpy(&value, bytes.data() + 8 + index * 4, 4);
						text += "-" + std::to_string(value);
					}
					const auto name = names.find(text);
					return name == names.end() ? text : name->second;
				};
				static const std::pair<uint32_t, const char *> rights[] = {{0x1f01ff, "FA"}, {0x120089, "FR"}, {0x120116, "FW"},
					{0x1200a0, "FX"}, {0xf003f, "KA"}, {0x20019, "KR"}, {0x20006, "KW"}, {0x10000000, "GA"}, {0x80000000, "GR"},
					{0x40000000, "GW"}, {0x20000000, "GX"}};
				const auto acl_text = [&](uint64_t acl, uint16_t protect, uint16_t requested, uint16_t inherited) {
					std::string text;
					if (control & protect) text += "P";
					if (control & requested) text += "AR";
					if (control & inherited) text += "AI";
					if (!acl)
						return text + "NO_ACCESS_CONTROL";
					const uint16_t count = call.load<uint16_t>(acl + 4);
					uint64_t entry = acl + 8;
					for (uint16_t index = 0; index < count; ++index) {
						const auto head = call.read(entry, 8);
						const uint16_t size = uint16_t(head[2] | head[3] << 8);
						uint32_t mask;
						std::memcpy(&mask, head.data() + 4, 4);
						static const std::map<uint8_t, const char *> types{{0, "A"}, {1, "D"}, {2, "AU"}, {3, "AL"}, {0x11, "ML"}};
						const auto type = types.find(head[0]);
						text += "(";
						text += type == types.end() ? "A" : type->second;
						text += ";";
						static const std::pair<uint8_t, const char *> flags[] = {{0x1, "OI"}, {0x2, "CI"}, {0x4, "NP"}, {0x8, "IO"},
							{0x10, "ID"}, {0x40, "SA"}, {0x80, "FA"}};
						for (const auto &[bit, flag] : flags)
							if (head[1] & bit)
								text += flag;
						text += ";";
						const auto right = std::find_if(std::begin(rights), std::end(rights), [&](const auto &item) { return item.first == mask; });
						if (right != std::end(rights))
							text += right->second;
						else {
							char hex[16];
							std::snprintf(hex, sizeof hex, "0x%x", mask);
							text += hex;
						}
						text += ";;;" + sid_text(entry + 8) + ")";
						if (size < 8)
							break;
						entry += size;
					}
					return text;
				};
				const uint32_t information = call.u32(2);
				std::string text;
				if ((information & 1) && part(0)) text += "O:" + sid_text(part(0));
				if ((information & 2) && part(1)) text += "G:" + sid_text(part(1));
				if ((information & 4) && (control & 0x4)) text += "D:" + acl_text(part(3), 0x1000, 0x100, 0x400);
				if ((information & 8) && (control & 0x10)) text += "S:" + acl_text(part(2), 0x2000, 0x200, 0x800);
				const size_t unit = wide ? 2 : 1;
				const auto memory = context.allocator.allocate((text.size() + 1) * unit, true);
				if (!memory) {
					call.fail(8);
					return;
				}
				std::vector<uint8_t> bytes((text.size() + 1) * unit, 0);
				for (size_t index = 0; index < text.size(); ++index)
					bytes[index * unit] = uint8_t(text[index]);
				call.write(memory, bytes);
				call.store_pointer(call[3], memory);
				if (call[4])
					call.store<uint32_t>(call[4], uint32_t(text.size() + 1));
				call.ret(1);
			});
	// RtlEncryptMemory and RtlDecryptMemory (SystemFunction040 and 041) keep a buffer unreadable to other processes while it sits in
	// memory; a process of the prefix is not visible to the others, so the bytes stay as they are and both directions succeed.
	registry.add("SystemFunction040", 3, [](ApiCall &call) { call.ret(0); });
	registry.add("SystemFunction041", 3, [](ApiCall &call) { call.ret(0); });
	registry.add("ConvertStringSidToSidW", 2, [&context](ApiCall &call) {
		const auto parsed = sid_from_text(call.wide(call[0], 256));
		if (!parsed) {
			call.fail(1337);
			return;
		}
		const auto &sid = *parsed;
		const auto memory = context.allocator.allocate(sid.size(), true);
		if (!memory || !call[1]) {
			call.fail(memory ? 87 : 8);
			return;
		}
		call.write(memory, sid);
		call.store_pointer(call[1], memory);
		call.ret(1);
	});
	// The trustee names programs give by name: the account itself and the well-known groups.
	const auto named_sid = [](std::u16string name) -> std::vector<uint8_t> {
		for (auto &c : name)
			if (c >= u'a' && c <= u'z')
				c = char16_t(c - 32);
		if (name.starts_with(u"BUILTIN\\") || name.starts_with(u"NT AUTHORITY\\"))
			name = name.substr(name.find(u'\\') + 1);
		if (name == u"EVERYONE")
			return make_sid(1, {0});
		if (name == u"CURRENT_USER")
			return user_sid();
		if (name == u"USERS")
			return make_sid(5, {32, 545});
		if (name == u"ADMINISTRATORS")
			return make_sid(5, {32, 544});
		if (name == u"SYSTEM")
			return make_sid(5, {18});
		return {};
	};
	// BuildSecurityDescriptorW: a self-relative descriptor in memory LocalFree releases. Its DACL is the null one, which grants every
	// access; the entries a program lists are not turned into access rules, which no other user of this machine could test anyway.
	registry.add("BuildSecurityDescriptorW", 9, [&context](ApiCall &call) {
		if (!call[7] || !call[8]) {
			call.ret(87);
			return;
		}
		std::vector<uint8_t> descriptor(20, 0);
		descriptor[0] = 1;
		const uint16_t control = 0x8000 | 0x4;
		std::memcpy(descriptor.data() + 2, &control, 2);
		const auto memory = context.allocator.allocate(descriptor.size(), true);
		if (!memory) {
			call.ret(8);
			return;
		}
		call.write(memory, descriptor);
		call.store<uint32_t>(call[7], uint32_t(descriptor.size()));
		call.store_pointer(call[8], memory);
		call.ret(0);
	});
	// EXPLICIT_ACCESS_W with a trustee given by name: the permissions, the mode, the inheritance, then the TRUSTEE_W.
	registry.add("BuildExplicitAccessWithNameW", 5, [](ApiCall &call) {
		if (!call[0])
			return;
		const size_t width = call.pointer_width(), trustee_at = width == 4 ? 12 : 16;
		std::vector<uint8_t> entry(trustee_at + (width == 4 ? 20 : 32));
		const uint32_t fields[3] = {call.u32(2), call.u32(3), call.u32(4)};
		std::memcpy(entry.data(), fields, 12);
		const uint32_t form = 1;
		std::memcpy(entry.data() + trustee_at + (width == 4 ? 8 : 12), &form, 4);
		const uint64_t name = call[1];
		std::memcpy(entry.data() + trustee_at + (width == 4 ? 16 : 24), &name, width);
		call.write(call[0], entry);
	});
	// Building an ACL from explicit entries: each entry grants or denies a mask to a trustee named by its SID.
	registry.add("BuildTrusteeWithSidW", 2, [](ApiCall &call) {
		if (!call[0])
			return;
		const size_t width = call.pointer_width();
		std::vector<uint8_t> trustee(width == 4 ? 20 : 32);
		const uint64_t sid = call[1];
		std::memcpy(trustee.data() + (width == 4 ? 16 : 24), &sid, width);
		call.write(call[0], trustee);
	});
	registry.add("SetEntriesInAclW", 4, [&context, named_sid](ApiCall &call) {
		const uint32_t count = call.u32(0);
		if (!call[3] || (count && !call[1])) {
			call.ret(87);
			return;
		}
		const size_t width = call.pointer_width(), trustee_size = width == 4 ? 20 : 32, entry_size = 12 + (width == 4 ? 0 : 4) + trustee_size;
		const size_t trustee_at = 12 + (width == 4 ? 0 : 4);
		std::vector<uint8_t> acl(8);
		acl[0] = 2;
		uint16_t entries = 0;
		// An existing list keeps its entries first.
		if (call[2]) {
			const auto header = call.read(call[2], 8);
			uint16_t old_count, capacity;
			std::memcpy(&capacity, header.data() + 2, 2);
			std::memcpy(&old_count, header.data() + 4, 2);
			if (capacity < acl_header || (header[0] != 2 && header[0] != 4)) {
				call.ret(1336);
				return;
			}
			const auto old = call.read(call[2], capacity);
			uint32_t used = 8;
			for (uint16_t index = 0; index < old_count; ++index) {
				uint16_t size = 0;
				if (used + 4 <= old.size())
					std::memcpy(&size, old.data() + used + 2, 2);
				if (size < 4 || used + size > old.size()) {
					call.ret(1336);
					return;
				}
				used += size;
			}
			acl.insert(acl.end(), old.begin() + acl_header, old.begin() + used);
			entries = old_count;
		}
		for (uint32_t index = 0; index < count; ++index) {
			const uint64_t at = call[1] + uint64_t(index) * entry_size;
			const uint32_t permissions = call.load<uint32_t>(at), mode = call.load<uint32_t>(at + 4);
			const uint64_t name = call.load_pointer(at + trustee_at + (width == 4 ? 16 : 24));
			const uint32_t form = call.load<uint32_t>(at + trustee_at + (width == 4 ? 8 : 12));
			std::vector<uint8_t> sid;
			if (form == 0 && valid_sid(call, name))
				sid = call.read(name, sid_length(call, name));
			else if (form == 1 && (sid = named_sid(call.wide(name, 64))).size())
				;
			else {
				call.ret(1332);
				return;
			}
			if (mode < 1 || mode > 4) {
				call.ret(87);
				return;
			}
			if (mode == 4) {
				for (size_t offset = acl_header; offset + 4 <= acl.size();) {
					uint16_t size;
					std::memcpy(&size, acl.data() + offset + 2, 2);
					if (size < 4 || offset + size > acl.size()) {
						call.ret(1336);
						return;
					}
					if (acl[offset] <= 1 && size == 8 + sid.size() &&
						std::equal(sid.begin(), sid.end(), acl.begin() + offset + 8)) {
						acl.erase(acl.begin() + offset, acl.begin() + offset + size);
						--entries;
					} else {
						offset += size;
					}
				}
				continue;
			}
			const uint16_t size = uint16_t(8 + sid.size());
			std::vector<uint8_t> entry(size);
			entry[0] = mode == 3 ? 1 : 0;
			entry[1] = uint8_t(call.load<uint32_t>(at + 8));
			std::memcpy(entry.data() + 2, &size, 2);
			std::memcpy(entry.data() + 4, &permissions, 4);
			std::memcpy(entry.data() + 8, sid.data(), sid.size());
			acl.insert(acl.end(), entry.begin(), entry.end());
			++entries;
		}
		if (acl.size() > UINT16_MAX) {
			call.ret(1344);
			return;
		}
		const uint16_t total = uint16_t(acl.size());
		std::memcpy(acl.data() + 2, &total, 2);
		std::memcpy(acl.data() + 4, &entries, 2);
		const auto memory = context.allocator.allocate(acl.size(), true);
		if (!memory) {
			call.ret(8);
			return;
		}
		call.write(memory, acl);
		call.store_pointer(call[3], memory);
		call.ret(0);
	});
	// AddAce: ready-made entries go into the list at a position, or at its end for MAXDWORD, and the entries after them move up.
	registry.add("AddAce", 5, [](ApiCall &call) {
		if (!call[0] || !call[3]) {
			call.fail(87);
			return;
		}
		const auto header = call.read(call[0], acl_header);
		uint16_t size, count;
		std::memcpy(&size, header.data() + 2, 2);
		std::memcpy(&count, header.data() + 4, 2);
		std::vector<uint32_t> offsets;
		uint32_t used = acl_header;
		for (uint16_t index = 0; index < count; ++index) {
			offsets.push_back(used);
			uint16_t entry_size;
			std::memcpy(&entry_size, call.read(call[0] + used + 2, 2).data(), 2);
			used += entry_size;
		}
		const auto added = call.read(call[3], call.u32(4));
		uint16_t added_count = 0;
		for (size_t at = 0; at + 4 <= added.size();) {
			uint16_t entry_size;
			std::memcpy(&entry_size, added.data() + at + 2, 2);
			if (entry_size < 4 || at + entry_size > added.size()) {
				call.fail(87);
				return;
			}
			at += entry_size;
			++added_count;
		}
		if (used + added.size() > size) {
			call.fail(1344);
			return;
		}
		const uint32_t index = call.u32(2);
		const uint32_t at = index >= count ? used : offsets[index];
		auto moved = call.read(call[0] + at, used - at);
		std::vector<uint8_t> tail(added.begin(), added.end());
		tail.insert(tail.end(), moved.begin(), moved.end());
		call.write(call[0] + at, tail);
		call.store<uint16_t>(call[0] + 4, uint16_t(count + added_count));
		call.ret(1);
	});
	registry.add("AddAccessAllowedAce", 4, [add_ace](ApiCall &call) { add_ace(call, 0); });
	registry.add("AddAccessDeniedAce", 4, [add_ace](ApiCall &call) { add_ace(call, 1); });
}

}
