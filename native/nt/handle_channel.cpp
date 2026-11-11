#include "handle_channel.h"

#include "child_process.h"
#include "host_names.h"
#include "local_socket.h"
#include "objects.h"
#include "threads.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace raven {

namespace {

std::mutex channels_mutex;
std::vector<HandleChannel *> children;
HandleChannel *parent = nullptr;

// Frames that are not a handle sent over: a request names the value it wants in `handle` and its number in `size`; an answer carries
// the request number in `handle` and the kind of the object after this prefix, nothing after it when there is no such handle.
const std::string request_kind = "request", answer_prefix = "answer:", transfer_prefix = "transfer:";

struct ProcessChannels {
	int listener = -1;
	std::vector<std::shared_ptr<HandleChannel>> accepted;
	std::map<pid_t, std::shared_ptr<HandleChannel>> outgoing;
	~ProcessChannels() { if (listener >= 0) ::close(listener); }
};

ProcessChannels process_channels;

std::string process_name(pid_t pid)
{
	return "raven-process-" + prefix_tag() + "-" + std::to_string(pid);
}

sockaddr_un process_address(pid_t pid, socklen_t &length)
{
	sockaddr_un address;
	length = local_socket_address(process_name(pid), address);
	return address;
}

bool process_peer(int descriptor, pid_t expected = 0)
{
	ucred peer{};
	socklen_t length = sizeof(peer);
	return !::getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &peer, &length) && peer.uid == ::geteuid() &&
		peer.pid > 0 && (!expected || peer.pid == expected) && runtime_process_alive(peer.pid);
}

}

void enable_process_requests()
{
	if (process_channels.listener >= 0)
		return;
	const int descriptor = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (descriptor < 0)
		return;
	if (bind_local_socket(descriptor, process_name(::getpid())) || ::listen(descriptor, 64)) {
		::close(descriptor);
		return;
	}
	process_channels.listener = descriptor;
}

std::shared_ptr<HandleChannel> process_request_channel(pid_t pid)
{
	if (!runtime_process_alive(pid))
		return nullptr;
	auto &channel = process_channels.outgoing[pid];
	if (channel && channel->connected())
		return channel;
	channel.reset();
	const int descriptor = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (descriptor < 0)
		return nullptr;
	socklen_t length;
	const auto address = process_address(pid, length);
	if (::connect(descriptor, reinterpret_cast<const sockaddr *>(&address), length) || !process_peer(descriptor, pid)) {
		::close(descriptor);
		return nullptr;
	}
	channel = std::make_shared<HandleChannel>(descriptor, true);
	return channel;
}

bool HandleChannel::connected() const
{
	uint8_t byte;
	const auto count = ::recv(descriptor, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
	return count > 0 || (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR));
}

HandleChannel::~HandleChannel()
{
	if (descriptor >= 0)
		::close(descriptor);
}

bool HandleChannel::send_frame(const InheritedHandle &record, int shared)
{
	if (descriptor < 0)
		return false;
	const auto bytes = serialize_inherited({record});
	iovec vector{const_cast<uint8_t *>(bytes.data()), bytes.size()};
	alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
	msghdr message{};
	message.msg_iov = &vector;
	message.msg_iovlen = 1;
	if (shared >= 0) {
		message.msg_control = control;
		message.msg_controllen = sizeof(control);
		auto *header = CMSG_FIRSTHDR(&message);
		header->cmsg_level = SOL_SOCKET;
		header->cmsg_type = SCM_RIGHTS;
		header->cmsg_len = CMSG_LEN(sizeof(int));
		std::memcpy(CMSG_DATA(header), &shared, sizeof(int));
	}
	ssize_t sent;
	do sent = ::sendmsg(descriptor, &message, MSG_NOSIGNAL); while (sent < 0 && errno == EINTR);
	return sent == ssize_t(bytes.size());
}

NativeObjectResult HandleChannel::send(NativeObjects &objects, NativeThreads &threads, InheritedHandle record, uint32_t inherit)
{
	if (process_only)
		return {0, 0xc00000bbU};
	const int shared = record.descriptor;
	if (shared < 0)
		return {0, 0xc0000008U};
	// Exchange uses size for its request number; keep the object's size in the unused source-handle word.
	record.handle = record.size;
	record.kind = transfer_prefix + record.kind;
	record.descriptor = -1;
	if (inherit)
		record.bits |= 0x80000000U;
	const auto answer = exchange(objects, threads, std::move(record), shared);
	if (!answer || answer->kind != "transfer") return {0, 0xc0000001U};
	return {answer->size, answer->access};
}

std::shared_ptr<KernelObject> HandleChannel::fetch(NativeObjects &objects, NativeThreads &threads, uint64_t value, bool close_source, uint32_t &access)
{
	if (process_only)
		return nullptr;
	InheritedHandle request;
	request.kind = request_kind;
	request.handle = value;
	request.flags = close_source ? 1 : 0;
	const auto answer = exchange(objects, threads, std::move(request));
	if (!answer || answer->kind.empty() || answer->descriptor < 0)
		return nullptr;
	access = answer->handle_access;
	auto object = object_from_record(*answer);
	if (!object)
		::close(answer->descriptor);
	return object;
}

std::optional<InheritedHandle> HandleChannel::request_process(NativeObjects &objects, NativeThreads &threads, InheritedHandle request)
{
	if (!request.kind.starts_with("process:"))
		return std::nullopt;
	return exchange(objects, threads, std::move(request));
}

std::optional<InheritedHandle> HandleChannel::exchange(NativeObjects &objects, NativeThreads &threads, InheritedHandle request, int shared)
{
	const uint64_t number = next_request++;
	request.size = number;
	answers[number] = std::nullopt;
	if (!send_frame(request, shared)) {
		answers.erase(number);
		return std::nullopt;
	}
	// The other process answers when it next schedules its threads; meanwhile the threads of this one run.
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	HostWait waiting;
	threads.block([&] {
		receive(objects);
		const auto found = answers.find(number);
		return found == answers.end() || found->second.has_value();
	}, deadline);
	const auto found = answers.find(number);
	if (found == answers.end() || !found->second) {
		answers.erase(number);
		return std::nullopt;
	}
	auto record = *found->second;
	answers.erase(found);
	return record;
}

void HandleChannel::receive(NativeObjects &objects)
{
	for (;;) {
		uint8_t buffer[65536];
		iovec vector{buffer, sizeof(buffer)};
		alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 4)] = {};
		msghdr message{};
		message.msg_iov = &vector;
		message.msg_iovlen = 1;
		message.msg_control = control;
		message.msg_controllen = sizeof(control);
		ssize_t count;
		do count = ::recvmsg(descriptor, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC); while (count < 0 && errno == EINTR);
		if (count <= 0)
			return;
		int received = -1;
		for (auto *header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header))
			if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS)
				std::memcpy(&received, CMSG_DATA(header), sizeof(int));
		std::vector<InheritedHandle> records;
		try {
			records = parse_inherited({buffer, size_t(count)});
		} catch (const std::exception &) {
		}
		if (records.size() != 1) {
			if (received >= 0)
				::close(received);
			continue;
		}
		auto &record = records.front();
		if (process_only && !record.kind.starts_with("process:") && record.kind != answer_prefix + "process") {
			if (received >= 0) ::close(received);
			continue;
		}
		if (record.kind.starts_with(transfer_prefix)) {
			InheritedHandle answer;
			answer.kind = answer_prefix + "transfer";
			answer.handle = record.size;
			answer.access = 0xc0000008U;
			if (received >= 0) {
				record.kind.erase(0, transfer_prefix.size());
				record.size = record.handle;
				record.descriptor = received;
				const bool inherit = record.bits & 0x80000000U;
				record.bits &= ~0x80000000U;
				try {
					if (const auto object = object_from_record(record)) {
						const auto made = objects.add_object(object);
						answer.access = made.status;
						if (!made.status) {
							objects.set_access(made.value, record.handle_access);
							objects.set_inherit(made.value, inherit);
							answer.size = made.value;
						}
					} else {
						::close(received);
						answer.access = 0xc00000bbU;
					}
				} catch (const std::exception &) {
					answer.access = 0xc0000008U;
				}
			}
			if (!send_frame(answer, -1) && answer.size) objects.close(answer.size);
			continue;
		}
		if (record.kind.starts_with("process:")) {
			if (received >= 0)
				::close(received);
			InheritedHandle answer;
			answer.access = 0xc0000002U;
			if (objects.process_request)
				answer = objects.process_request(record);
			answer.kind = answer_prefix + "process";
			answer.handle = record.size;
			const int shared = answer.descriptor;
			answer.descriptor = -1;
			send_frame(answer, shared);
			continue;
		}
		if (record.kind == request_kind) {
			// The other process wants one of this process's handles: the answer carries the descriptor behind it.
			InheritedHandle answer;
			answer.kind = answer_prefix;
			answer.handle = record.size;
			const auto object = objects.object(record.handle);
			InheritedHandle described;
			int shared = -1;
			if (object && object->describe(described)) {
				shared = described.descriptor;
				answer = described;
				answer.kind = answer_prefix + described.kind;
				answer.handle = record.size;
				answer.descriptor = -1;
				answer.handle_access = objects.access_of(record.handle);
			}
			send_frame(answer, shared);
			if (shared >= 0 && (record.flags & 1))
				objects.close(record.handle);
			continue;
		}
		if (record.kind.starts_with(answer_prefix)) {
			record.kind = record.kind.substr(answer_prefix.size());
			record.descriptor = received;
			const auto found = answers.find(record.handle);
			if (found != answers.end())
				found->second = record;
			else if (received >= 0)
				::close(received);
			continue;
		}
		if (received < 0)
			continue;
		record.descriptor = received;
		try {
			adopt_inherited(objects, records);
		} catch (const std::exception &) {
			::close(received);
		}
	}
}

void set_parent_channel(int descriptor)
{
	std::lock_guard lock(channels_mutex);
	parent = new HandleChannel(descriptor);
}

HandleChannel *parent_channel()
{
	std::lock_guard lock(channels_mutex);
	return parent;
}

void add_child_channel(HandleChannel *channel)
{
	std::lock_guard lock(channels_mutex);
	children.push_back(channel);
}

void remove_child_channel(HandleChannel *channel)
{
	std::lock_guard lock(channels_mutex);
	children.erase(std::remove(children.begin(), children.end(), channel), children.end());
}

void receive_handles(NativeObjects &objects)
{
	const auto owners = retained_children();
	std::vector<std::shared_ptr<HandleChannel>> peers = process_channels.accepted;
	for (const auto &[pid, channel] : process_channels.outgoing)
		if (channel)
			peers.push_back(channel);
	std::vector<HandleChannel *> sources;
	{
		std::lock_guard lock(channels_mutex);
		sources = children;
		if (parent)
			sources.push_back(parent);
	}
	const size_t listener = process_channels.listener >= 0 ? 1 : 0, peer_count = peers.size();
	std::vector<pollfd> descriptors;
	descriptors.reserve(listener + sources.size() + peer_count);
	if (listener)
		descriptors.push_back({process_channels.listener, POLLIN, 0});
	for (const auto *channel : sources)
		descriptors.push_back({channel->host_descriptor(), POLLIN, 0});
	for (const auto &channel : peers)
		descriptors.push_back({channel->host_descriptor(), POLLIN, 0});
	int ready = 0;
	if (!descriptors.empty())
		do ready = ::poll(descriptors.data(), descriptors.size(), 0); while (ready < 0 && errno == EINTR);
	// A failed readiness query falls back to the nonblocking receives rather than losing pending transfers.
	if (listener && (ready < 0 || descriptors[0].revents)) {
		for (;;) {
			const int descriptor = ::accept4(process_channels.listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
			if (descriptor < 0) break;
			if (!process_peer(descriptor) || process_channels.accepted.size() >= 256) {
				::close(descriptor);
				continue;
			}
			auto channel = std::make_shared<HandleChannel>(descriptor, true);
			process_channels.accepted.push_back(channel);
			peers.push_back(std::move(channel));
		}
	}
	for (size_t index = 0; index < sources.size(); ++index)
		if (ready < 0 || descriptors[listener + index].revents)
			sources[index]->receive(objects);
	std::vector<HandleChannel *> disconnected;
	for (size_t index = 0; index < peers.size(); ++index)
		if (index >= peer_count || ready < 0 || descriptors[listener + sources.size() + index].revents) {
			peers[index]->receive(objects);
			if (!peers[index]->connected())
				disconnected.push_back(peers[index].get());
		}
	std::erase_if(process_channels.accepted, [&](const auto &channel) {
		return std::find(disconnected.begin(), disconnected.end(), channel.get()) != disconnected.end();
	});
	std::erase_if(process_channels.outgoing, [&](const auto &entry) {
		return !entry.second || std::find(disconnected.begin(), disconnected.end(), entry.second.get()) != disconnected.end();
	});
	// A child can send and exit after the poll. Keep its channel alive and drain it once more after reaping.
	const auto live = live_children();
	for (const auto &owner : owners)
		if (owner->channel && std::find(live.begin(), live.end(), owner) == live.end())
			owner->channel->receive(objects);
}

}
