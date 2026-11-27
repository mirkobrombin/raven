// See wayland_relay.h. The runtime's requests come through one dispatcher, by interface and name; what the runtime asks of a surface
// shown on the host is asked again of the surface's own on the host's connection, and what the host sends about those surfaces (the
// buffers it is done with, the frames it showed, the pointer and keyboard) is sent on to the runtime.

#include "vst/wayland_relay.h"

#include "xdg-shell-server-protocol.h"
#include <wayland-client.h>
#include <wayland-server.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

namespace raven_vst {
namespace {

struct Relay;

// The memory of a pool of the runtime's, kept as long as a buffer cut from it is, as the runtime destroys a pool once it cut its buffer.
struct Pool {
	int descriptor = -1;
	int32_t size = 0;
	~Pool()
	{
		if (descriptor >= 0)
			::close(descriptor);
	}
};

struct Buffer {
	Relay *relay = nullptr;
	wl_resource *resource = nullptr;
	std::shared_ptr<Pool> pool;
	int32_t offset = 0, width = 0, height = 0, stride = 0;
	uint32_t format = 0;
	// The same memory as a buffer of the host's, and whether the host still shows it.
	wl_buffer *host = nullptr;
	bool held = false;
};

// A region as the runtime built it, made again on the host when a surface shown there is given it.
struct Region {
	std::vector<std::array<int32_t, 5>> steps; // add (1) or subtract (0), x, y, width, height
};

struct Surface {
	Relay *relay = nullptr;
	wl_resource *resource = nullptr;
	// What the next commit applies.
	Buffer *pending = nullptr;
	bool attached = false;
	std::vector<wl_resource *> callbacks;
	int32_t pending_scale = 1, scale = 1;
	std::optional<Region> pending_input, input;
	bool input_changed = false;
	Buffer *current = nullptr;
	// Its role: a toplevel (with the view its application id names), a popup or a subsurface of `parent`.
	wl_resource *shell = nullptr, *toplevel = nullptr, *popup = nullptr, *subsurface = nullptr;
	bool configured = false;
	uint64_t view = 0;
	Surface *parent = nullptr;
	std::vector<Surface *> children;
	int32_t x = 0, y = 0;
	bool synchronized = true;
	// Its surface on the host, while shown there.
	wl_surface *host = nullptr;
	wl_subsurface *host_role = nullptr;
};

void close_descriptor(int descriptor)
{
	if (descriptor >= 0)
		::close(descriptor);
}

struct Relay final : WaylandRelay {
	wl_display *server = wl_display_create();
	int wake = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	std::thread thread;
	bool stopping = false;
	std::mutex lock;
	std::deque<std::function<void()>> work;

	std::set<Surface *> surfaces;
	std::set<Buffer *> buffers;
	std::set<wl_resource *> pointers, keyboards;
	// A frame callback of the runtime's, with the host's it waits for and the surface it was asked of.
	struct Frame {
		wl_callback *waited;
		Surface *surface;
	};
	std::map<wl_resource *, Frame> frames;
	// Each view the host shows an editor in, by the surface it gave.
	std::map<uint64_t, wl_surface *> views;
	// The host's connection, read on a queue of the relay's own.
	wl_display *host = nullptr;
	wl_event_queue *queue = nullptr;
	wl_display *wrapper = nullptr;
	wl_registry *registry = nullptr;
	wl_compositor *compositor = nullptr;
	wl_subcompositor *subcompositor = nullptr;
	wl_shm *shm = nullptr;
	wl_seat *seat = nullptr;
	wl_pointer *pointer = nullptr;
	wl_keyboard *keyboard = nullptr;
	Surface *pointer_focus = nullptr, *keyboard_focus = nullptr;
	int keymap = -1;
	uint32_t keymap_format = 0, keymap_size = 0;
	std::optional<std::pair<int32_t, int32_t>> repeat;

	~Relay() override
	{
		run([this] { stopping = true; });
		if (thread.joinable())
			thread.join();
		drop_host();
		wl_display_destroy_clients(server);
		wl_display_destroy(server);
		close_descriptor(wake);
		close_descriptor(keymap);
	}

	// Runs `step` on the relay's thread and waits for it.
	void run(std::function<void()> step)
	{
		if (std::this_thread::get_id() == thread.get_id()) {
			step();
			return;
		}
		std::promise<void> done;
		auto finished = done.get_future();
		{
			const std::lock_guard guard(lock);
			work.push_back([&] {
				step();
				done.set_value();
			});
		}
		const uint64_t one = 1;
		[[maybe_unused]] const auto written = ::write(wake, &one, sizeof one);
		finished.wait();
	}

	bool show(uint64_t view, wl_display *display, wl_surface *parent) override
	{
		bool shown = false;
		run([&] {
			if (!display || !parent || (host && host != display) || (!host && !take_host(display)))
				return;
			hide_now(view);
			views[view] = parent;
			for (Surface *surface : surfaces)
				if (surface->toplevel && surface->view == view)
					place(*surface);
			wl_display_flush(host);
			shown = true;
		});
		return shown;
	}

	void hide(uint64_t view) override
	{
		run([&] {
			hide_now(view);
			if (host)
				wl_display_flush(host);
		});
	}

	void forget(wl_display *display) override
	{
		run([&] {
			if (host == display && views.empty())
				drop_host();
		});
	}

	void hide_now(uint64_t view)
	{
		if (!views.erase(view))
			return;
		for (Surface *surface : surfaces)
			if (surface->toplevel && surface->view == view)
				take_back(*surface);
	}

	void loop()
	{
		wl_event_loop *const events = wl_display_get_event_loop(server);
		while (!stopping) {
			if (host) {
				while (wl_display_prepare_read_queue(host, queue) != 0)
					wl_display_dispatch_queue_pending(host, queue);
				wl_display_flush(host);
			}
			wl_display_flush_clients(server);
			pollfd watched[3] = {{wl_event_loop_get_fd(events), POLLIN, 0}, {wake, POLLIN, 0}, {host ? wl_display_get_fd(host) : -1, POLLIN, 0}};
			::poll(watched, host ? 3 : 2, -1);
			if (host) {
				if (watched[2].revents & POLLIN)
					wl_display_read_events(host);
				else
					wl_display_cancel_read(host);
				wl_display_dispatch_queue_pending(host, queue);
			}
			wl_event_loop_dispatch(events, 0);
			uint64_t drained = 0;
			[[maybe_unused]] const auto read = ::read(wake, &drained, sizeof drained);
			std::deque<std::function<void()>> steps;
			{
				const std::lock_guard guard(lock);
				steps.swap(work);
			}
			for (auto &step : steps)
				step();
		}
	}

	// The host's connection.

	bool take_host(wl_display *display)
	{
		host = display;
		queue = wl_display_create_queue(display);
		wrapper = static_cast<wl_display *>(wl_proxy_create_wrapper(display));
		wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(wrapper), queue);
		registry = wl_display_get_registry(wrapper);
		static const wl_registry_listener listener{
			[](void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
				auto &relay = *static_cast<Relay *>(data);
				if (!std::strcmp(interface, "wl_compositor"))
					relay.compositor = static_cast<wl_compositor *>(wl_registry_bind(registry, name, &wl_compositor_interface, std::min(version, 4U)));
				else if (!std::strcmp(interface, "wl_subcompositor"))
					relay.subcompositor = static_cast<wl_subcompositor *>(wl_registry_bind(registry, name, &wl_subcompositor_interface, 1));
				else if (!std::strcmp(interface, "wl_shm"))
					relay.shm = static_cast<wl_shm *>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
				else if (!std::strcmp(interface, "wl_seat") && !relay.seat) {
					relay.seat = static_cast<wl_seat *>(wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 5U)));
					wl_seat_add_listener(relay.seat, &seat_listener, &relay);
				}
			},
			[](void *, wl_registry *, uint32_t) {}};
		wl_registry_add_listener(registry, &listener, this);
		if (wl_display_roundtrip_queue(host, queue) < 0 || wl_display_roundtrip_queue(host, queue) < 0 || !compositor || !subcompositor || !shm) {
			drop_host();
			return false;
		}
		return true;
	}

	void drop_host()
	{
		if (!host)
			return;
		views.clear();
		for (Surface *surface : surfaces)
			if (!surface->parent)
				take_back(*surface);
		for (Buffer *buffer : buffers)
			drop_host_buffer(*buffer);
		finish_frames(nullptr);
		if (pointer)
			wl_pointer_destroy(pointer);
		if (keyboard)
			wl_keyboard_destroy(keyboard);
		if (seat)
			wl_seat_destroy(seat);
		if (shm)
			wl_shm_destroy(shm);
		if (subcompositor)
			wl_subcompositor_destroy(subcompositor);
		if (compositor)
			wl_compositor_destroy(compositor);
		if (registry)
			wl_registry_destroy(registry);
		wl_proxy_wrapper_destroy(wrapper);
		wl_display_flush(host);
		wl_event_queue_destroy(queue);
		pointer = nullptr;
		keyboard = nullptr;
		seat = nullptr;
		shm = nullptr;
		subcompositor = nullptr;
		compositor = nullptr;
		registry = nullptr;
		wrapper = nullptr;
		queue = nullptr;
		host = nullptr;
		pointer_focus = keyboard_focus = nullptr;
	}

	static const wl_seat_listener seat_listener;
	static const wl_pointer_listener pointer_listener;
	static const wl_keyboard_listener keyboard_listener;

	Surface *ours(wl_surface *shown)
	{
		if (!shown)
			return nullptr;
		for (Surface *surface : surfaces)
			if (surface->host == shown)
				return surface;
		return nullptr;
	}

	// Each of the runtime's pointers or keyboards an event goes to, with its version.
	template <typename Send>
	void each(const std::set<wl_resource *> &devices, Send send)
	{
		for (wl_resource *device : devices)
			send(device, wl_resource_get_version(device));
	}

	// A surface shown on the host, and what shows on it.

	// The toplevel the runtime tagged for a view is shown as a subsurface of the surface the host gave, at its corner.
	void place(Surface &surface)
	{
		const auto found = views.find(surface.view);
		if (found == views.end() || surface.host)
			return;
		realize(surface, found->second);
		wl_subsurface_set_position(surface.host_role, 0, 0);
		wl_subsurface_set_desync(surface.host_role);
		publish(surface);
	}

	void realize(Surface &surface, wl_surface *parent)
	{
		surface.host = wl_compositor_create_surface(compositor);
		surface.host_role = wl_subcompositor_get_subsurface(subcompositor, surface.host, parent);
		if (surface.parent) {
			wl_subsurface_set_position(surface.host_role, surface.x, surface.y);
			if (!surface.synchronized)
				wl_subsurface_set_desync(surface.host_role);
		}
		for (Surface *child : surface.children) {
			realize(*child, surface.host);
			publish(*child);
		}
	}

	// Shows on the host what the surface shows now.
	void publish(Surface &surface)
	{
		if (surface.current) {
			if (wl_buffer *const shown = host_buffer(*surface.current)) {
				wl_surface_attach(surface.host, shown, 0, 0);
				surface.current->held = true;
			}
			wl_surface_damage(surface.host, 0, 0, INT32_MAX, INT32_MAX);
		}
		wl_surface_set_buffer_scale(surface.host, surface.scale);
		if (surface.input)
			apply_input(surface);
		wl_surface_commit(surface.host);
	}

	// The frames asked of `surface` (of every surface for none) that the host will not show: the runtime has them at once.
	void finish_frames(Surface *surface)
	{
		std::vector<wl_resource *> finished;
		for (auto &[callback, frame] : frames)
			if (!surface || frame.surface == surface) {
				wl_callback_destroy(frame.waited);
				finished.push_back(callback);
			}
		for (wl_resource *callback : finished) {
			frames.erase(callback);
			wl_callback_send_done(callback, 0);
			wl_resource_destroy(callback);
		}
	}

	void take_back(Surface &surface)
	{
		for (Surface *child : surface.children)
			take_back(*child);
		if (pointer_focus == &surface)
			leave_pointer();
		if (keyboard_focus == &surface)
			leave_keyboard();
		finish_frames(&surface);
		if (surface.host_role)
			wl_subsurface_destroy(surface.host_role);
		if (surface.host)
			wl_surface_destroy(surface.host);
		surface.host_role = nullptr;
		surface.host = nullptr;
		// The host shows the buffer no more, so the runtime may have it back at its next commit.
		if (surface.current)
			surface.current->held = false;
	}

	void apply_input(Surface &surface)
	{
		wl_region *const region = wl_compositor_create_region(compositor);
		for (const auto &step : surface.input->steps)
			(step[0] ? wl_region_add : wl_region_subtract)(region, step[1], step[2], step[3], step[4]);
		wl_surface_set_input_region(surface.host, region);
		wl_region_destroy(region);
	}

	// The buffer the host shows for one of the runtime's, from the same memory.
	wl_buffer *host_buffer(Buffer &buffer)
	{
		if (!buffer.host && shm) {
			wl_shm_pool *const pool = wl_shm_create_pool(shm, buffer.pool->descriptor, buffer.pool->size);
			buffer.host = wl_shm_pool_create_buffer(pool, buffer.offset, buffer.width, buffer.height, buffer.stride, buffer.format);
			wl_shm_pool_destroy(pool);
			static const wl_buffer_listener listener{[](void *data, wl_buffer *) {
				auto &buffer = *static_cast<Buffer *>(data);
				buffer.held = false;
				if (buffer.resource)
					wl_buffer_send_release(buffer.resource);
			}};
			wl_buffer_add_listener(buffer.host, &listener, &buffer);
		}
		return buffer.host;
	}

	void drop_host_buffer(Buffer &buffer)
	{
		if (buffer.host)
			wl_buffer_destroy(buffer.host);
		buffer.host = nullptr;
		buffer.held = false;
	}

	void commit(Surface &surface)
	{
		Buffer *const before = surface.current;
		const bool attached = surface.attached;
		if (attached) {
			surface.current = surface.pending;
			surface.attached = false;
			surface.pending = nullptr;
		}
		surface.scale = surface.pending_scale;
		if (surface.input_changed) {
			surface.input = surface.pending_input;
			surface.input_changed = false;
		}
		std::vector<wl_resource *> callbacks;
		callbacks.swap(surface.callbacks);
		if (surface.host) {
			if (attached) {
				wl_buffer *const shown = surface.current ? host_buffer(*surface.current) : nullptr;
				wl_surface_attach(surface.host, shown, 0, 0);
				if (surface.current && shown)
					surface.current->held = true;
				wl_surface_damage(surface.host, 0, 0, INT32_MAX, INT32_MAX);
			}
			wl_surface_set_buffer_scale(surface.host, surface.scale);
			if (surface.input)
				apply_input(surface);
			for (wl_resource *callback : callbacks) {
				wl_callback *const waited = wl_surface_frame(surface.host);
				static const wl_callback_listener listener{[](void *data, wl_callback *waited, uint32_t time) {
					auto *const callback = static_cast<wl_resource *>(data);
					wl_callback_destroy(waited);
					auto &relay = *static_cast<Relay *>(wl_resource_get_user_data(callback));
					relay.frames.erase(callback);
					wl_callback_send_done(callback, time);
					wl_resource_destroy(callback);
				}};
				wl_callback_add_listener(waited, &listener, callback);
				frames[callback] = {waited, &surface};
			}
			wl_surface_commit(surface.host);
			// The buffer shown before goes back once the host is done with it; one the host never had goes back at once.
			if (attached && before && before != surface.current && !before->held && before->resource)
				wl_buffer_send_release(before->resource);
		} else {
			// Shown nowhere: the runtime has its buffer and its frame back at once.
			if (attached && before && before != surface.current && before->resource)
				wl_buffer_send_release(before->resource);
			if (attached && surface.current && surface.current->resource)
				wl_buffer_send_release(surface.current->resource);
			for (wl_resource *callback : callbacks) {
				wl_callback_send_done(callback, 0);
				wl_resource_destroy(callback);
			}
		}
		if (surface.shell && !surface.configured) {
			surface.configured = true;
			if (surface.toplevel) {
				wl_array states;
				wl_array_init(&states);
				xdg_toplevel_send_configure(surface.toplevel, 0, 0, &states);
				wl_array_release(&states);
				xdg_surface_send_configure(surface.shell, wl_display_next_serial(server));
			} else if (surface.popup) {
				// The relay has no place for the runtime's popups on the host yet: they are dismissed as a compositor dismisses one.
				xdg_popup_send_popup_done(surface.popup);
			}
		}
		if (surface.toplevel && !surface.host)
			place(surface);
	}

	void forget_surface(Surface *surface)
	{
		take_back(*surface);
		for (Surface *child : surface->children)
			child->parent = nullptr;
		if (surface->parent)
			std::erase(surface->parent->children, surface);
		for (wl_resource *callback : surface->callbacks)
			wl_resource_destroy(callback);
		// Its roles may outlive it, naming it no more.
		for (wl_resource *role : {surface->shell, surface->toplevel, surface->popup, surface->subsurface})
			if (role)
				wl_resource_set_user_data(role, nullptr);
		surfaces.erase(surface);
		delete surface;
	}

	void leave_pointer()
	{
		if (pointer_focus && pointer_focus->resource)
			each(pointers, [&](wl_resource *device, int) {
				if (wl_resource_get_client(device) == wl_resource_get_client(pointer_focus->resource))
					wl_pointer_send_leave(device, wl_display_next_serial(server), pointer_focus->resource);
			});
		pointer_focus = nullptr;
	}

	void leave_keyboard()
	{
		if (keyboard_focus && keyboard_focus->resource)
			each(keyboards, [&](wl_resource *device, int) { wl_keyboard_send_leave(device, wl_display_next_serial(server), keyboard_focus->resource); });
		keyboard_focus = nullptr;
	}

	// The runtime's requests.

	static int dispatch(const void *, void *target, uint32_t, const wl_message *message, wl_argument *arguments);
	wl_resource *make(wl_client *client, const wl_interface *interface, int version, uint32_t id, void *data, wl_resource_destroy_func_t gone)
	{
		wl_resource *const made = wl_resource_create(client, interface, version, id);
		if (made)
			wl_resource_set_dispatcher(made, dispatch, this, data, gone);
		return made;
	}
	template <const wl_interface *interface>
	static void bind(wl_client *client, void *data, uint32_t version, uint32_t id)
	{
		auto &relay = *static_cast<Relay *>(data);
		wl_resource *const made = relay.make(client, interface, int(version), id, &relay, nullptr);
		if (!made)
			return;
		if constexpr (interface == &wl_shm_interface) {
			wl_shm_send_format(made, WL_SHM_FORMAT_ARGB8888);
			wl_shm_send_format(made, WL_SHM_FORMAT_XRGB8888);
		} else if constexpr (interface == &wl_seat_interface) {
			wl_seat_send_capabilities(made, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
		} else if constexpr (interface == &wl_output_interface) {
			wl_output_send_geometry(made, 0, 0, 0, 0, WL_OUTPUT_SUBPIXEL_UNKNOWN, "Raven", "plugin view", WL_OUTPUT_TRANSFORM_NORMAL);
			wl_output_send_mode(made, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED, 1920, 1080, 60000);
			if (version >= 2) {
				wl_output_send_scale(made, 1);
				wl_output_send_done(made);
			}
		}
	}
	bool start(int &client)
	{
		int ends[2];
		if (!server || wake < 0 || ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, ends))
			return false;
		if (!wl_global_create(server, &wl_compositor_interface, 4, this, bind<&wl_compositor_interface>) ||
			!wl_global_create(server, &wl_subcompositor_interface, 1, this, bind<&wl_subcompositor_interface>) ||
			!wl_global_create(server, &wl_shm_interface, 1, this, bind<&wl_shm_interface>) ||
			!wl_global_create(server, &xdg_wm_base_interface, 1, this, bind<&xdg_wm_base_interface>) ||
			!wl_global_create(server, &wl_seat_interface, 5, this, bind<&wl_seat_interface>) ||
			!wl_global_create(server, &wl_output_interface, 2, this, bind<&wl_output_interface>) || !wl_client_create(server, ends[0])) {
			::close(ends[0]);
			::close(ends[1]);
			return false;
		}
		// The runtime's end is inherited by the process started with it.
		::fcntl(ends[1], F_SETFD, 0);
		client = ends[1];
		thread = std::thread([this] { loop(); });
		return true;
	}
};

const wl_seat_listener Relay::seat_listener{
	[](void *data, wl_seat *seat, uint32_t capabilities) {
		auto &relay = *static_cast<Relay *>(data);
		if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !relay.pointer) {
			relay.pointer = wl_seat_get_pointer(seat);
			wl_pointer_add_listener(relay.pointer, &pointer_listener, &relay);
		}
		if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !relay.keyboard) {
			relay.keyboard = wl_seat_get_keyboard(seat);
			wl_keyboard_add_listener(relay.keyboard, &keyboard_listener, &relay);
		}
	},
	[](void *, wl_seat *, const char *) {}};

const wl_pointer_listener Relay::pointer_listener{
	[](void *data, wl_pointer *, uint32_t, wl_surface *entered, wl_fixed_t x, wl_fixed_t y) {
		auto &relay = *static_cast<Relay *>(data);
		relay.leave_pointer();
		Surface *const surface = relay.ours(entered);
		if (!surface)
			return;
		relay.pointer_focus = surface;
		relay.each(relay.pointers, [&](wl_resource *device, int version) {
			if (wl_resource_get_client(device) != wl_resource_get_client(surface->resource))
				return;
			wl_pointer_send_enter(device, wl_display_next_serial(relay.server), surface->resource, x, y);
			if (version >= 5)
				wl_pointer_send_frame(device);
		});
	},
	[](void *data, wl_pointer *, uint32_t, wl_surface *) { static_cast<Relay *>(data)->leave_pointer(); },
	[](void *data, wl_pointer *, uint32_t time, wl_fixed_t x, wl_fixed_t y) {
		auto &relay = *static_cast<Relay *>(data);
		if (relay.pointer_focus)
			relay.each(relay.pointers, [&](wl_resource *device, int) { wl_pointer_send_motion(device, time, x, y); });
	},
	[](void *data, wl_pointer *, uint32_t, uint32_t time, uint32_t button, uint32_t state) {
		auto &relay = *static_cast<Relay *>(data);
		if (relay.pointer_focus)
			relay.each(relay.pointers, [&](wl_resource *device, int) {
				wl_pointer_send_button(device, wl_display_next_serial(relay.server), time, button, state);
			});
	},
	[](void *data, wl_pointer *, uint32_t time, uint32_t axis, wl_fixed_t value) {
		auto &relay = *static_cast<Relay *>(data);
		if (relay.pointer_focus)
			relay.each(relay.pointers, [&](wl_resource *device, int) { wl_pointer_send_axis(device, time, axis, value); });
	},
	[](void *data, wl_pointer *) {
		auto &relay = *static_cast<Relay *>(data);
		if (relay.pointer_focus)
			relay.each(relay.pointers, [&](wl_resource *device, int version) {
				if (version >= 5)
					wl_pointer_send_frame(device);
			});
	},
	[](void *, wl_pointer *, uint32_t) {},
	[](void *, wl_pointer *, uint32_t, uint32_t) {},
	[](void *, wl_pointer *, uint32_t, int32_t) {},
	nullptr,
	nullptr};

const wl_keyboard_listener Relay::keyboard_listener{
	[](void *data, wl_keyboard *, uint32_t format, int32_t descriptor, uint32_t size) {
		auto &relay = *static_cast<Relay *>(data);
		close_descriptor(relay.keymap);
		relay.keymap = descriptor;
		relay.keymap_format = format;
		relay.keymap_size = size;
		relay.each(relay.keyboards, [&](wl_resource *device, int) { wl_keyboard_send_keymap(device, format, descriptor, size); });
	},
	[](void *data, wl_keyboard *, uint32_t, wl_surface *entered, wl_array *keys) {
		auto &relay = *static_cast<Relay *>(data);
		relay.leave_keyboard();
		Surface *const surface = relay.ours(entered);
		if (!surface)
			return;
		relay.keyboard_focus = surface;
		relay.each(relay.keyboards, [&](wl_resource *device, int) {
			wl_keyboard_send_enter(device, wl_display_next_serial(relay.server), surface->resource, keys);
		});
	},
	[](void *data, wl_keyboard *, uint32_t, wl_surface *) { static_cast<Relay *>(data)->leave_keyboard(); },
	[](void *data, wl_keyboard *, uint32_t, uint32_t time, uint32_t key, uint32_t state) {
		auto &relay = *static_cast<Relay *>(data);
		if (relay.keyboard_focus)
			relay.each(relay.keyboards, [&](wl_resource *device, int) {
				wl_keyboard_send_key(device, wl_display_next_serial(relay.server), time, key, state);
			});
	},
	[](void *data, wl_keyboard *, uint32_t, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
		auto &relay = *static_cast<Relay *>(data);
		relay.each(relay.keyboards, [&](wl_resource *device, int) {
			wl_keyboard_send_modifiers(device, wl_display_next_serial(relay.server), depressed, latched, locked, group);
		});
	},
	[](void *data, wl_keyboard *, int32_t rate, int32_t delay) {
		auto &relay = *static_cast<Relay *>(data);
		relay.repeat = std::pair{rate, delay};
		relay.each(relay.keyboards, [&](wl_resource *device, int version) {
			if (version >= 4)
				wl_keyboard_send_repeat_info(device, rate, delay);
		});
	}};

int Relay::dispatch(const void *data, void *target, uint32_t, const wl_message *message, wl_argument *arguments)
{
	auto &relay = *static_cast<Relay *>(const_cast<void *>(data));
	auto *const resource = static_cast<wl_resource *>(target);
	wl_client *const client = wl_resource_get_client(resource);
	const int version = wl_resource_get_version(resource);
	const std::string request = std::string(wl_resource_get_class(resource)) + "." + message->name;
	const auto object = [&](size_t index) { return reinterpret_cast<wl_resource *>(arguments[index].o); };
	const auto surface_of = [](wl_resource *of) { return of ? static_cast<Surface *>(wl_resource_get_user_data(of)) : nullptr; };
	// A role whose surface is gone asks nothing more; its destroy still ends it.
	const std::string_view type = wl_resource_get_class(resource);
	if ((type == "xdg_surface" || type == "xdg_toplevel" || type == "xdg_popup" || type == "wl_subsurface") && !surface_of(resource)) {
		if (!std::strcmp(message->name, "destroy"))
			wl_resource_destroy(resource);
		return 0;
	}
	// A descriptor that comes with a request is the relay's to close, unless the request keeps it.
	int descriptor = -1;
	for (size_t index = 0, at = 0; message->signature[at]; ++at) {
		const char type = message->signature[at];
		if ((type >= '0' && type <= '9') || type == '?')
			continue;
		if (type == 'h')
			descriptor = arguments[index].h;
		++index;
	}

	if (request == "wl_compositor.create_surface") {
		auto *const surface = new Surface;
		surface->relay = &relay;
		surface->resource = relay.make(client, &wl_surface_interface, version, arguments[0].n, surface, [](wl_resource *gone) {
			auto *const surface = static_cast<Surface *>(wl_resource_get_user_data(gone));
			surface->resource = nullptr;
			auto *const relay = surface->relay;
			relay->forget_surface(surface);
		});
		if (!surface->resource) {
			delete surface;
			wl_client_post_no_memory(client);
			return 0;
		}
		relay.surfaces.insert(surface);
	} else if (request == "wl_compositor.create_region" ) {
		relay.make(client, &wl_region_interface, version, arguments[0].n, new Region,
			[](wl_resource *gone) { delete static_cast<Region *>(wl_resource_get_user_data(gone)); });
	} else if (request == "wl_region.add" || request == "wl_region.subtract") {
		static_cast<Region *>(wl_resource_get_user_data(resource))
			->steps.push_back({request == "wl_region.add", arguments[0].i, arguments[1].i, arguments[2].i, arguments[3].i});
	} else if (request == "wl_shm.create_pool") {
		auto pool = std::make_shared<Pool>();
		pool->descriptor = descriptor;
		pool->size = arguments[2].i;
		descriptor = -1;
		relay.make(client, &wl_shm_pool_interface, version, arguments[0].n, new std::shared_ptr<Pool>(std::move(pool)),
			[](wl_resource *gone) { delete static_cast<std::shared_ptr<Pool> *>(wl_resource_get_user_data(gone)); });
	} else if (request == "wl_shm_pool.resize") {
		(*static_cast<std::shared_ptr<Pool> *>(wl_resource_get_user_data(resource)))->size = arguments[0].i;
	} else if (request == "wl_shm_pool.create_buffer") {
		auto *const buffer = new Buffer;
		buffer->relay = &relay;
		buffer->pool = *static_cast<std::shared_ptr<Pool> *>(wl_resource_get_user_data(resource));
		buffer->offset = arguments[1].i;
		buffer->width = arguments[2].i;
		buffer->height = arguments[3].i;
		buffer->stride = arguments[4].i;
		buffer->format = arguments[5].u;
		buffer->resource = relay.make(client, &wl_buffer_interface, 1, arguments[0].n, buffer, [](wl_resource *gone) {
			auto *const buffer = static_cast<Buffer *>(wl_resource_get_user_data(gone));
			auto *const relay = buffer->relay;
			for (Surface *surface : relay->surfaces) {
				if (surface->current == buffer)
					surface->current = nullptr;
				if (surface->pending == buffer)
					surface->pending = nullptr;
			}
			relay->drop_host_buffer(*buffer);
			relay->buffers.erase(buffer);
			delete buffer;
		});
		if (buffer->resource)
			relay.buffers.insert(buffer);
		else
			delete buffer;
	} else if (request == "wl_surface.attach") {
		Surface &surface = *surface_of(resource);
		surface.pending = object(0) ? static_cast<Buffer *>(wl_resource_get_user_data(object(0))) : nullptr;
		surface.attached = true;
	} else if (request == "wl_surface.frame") {
		wl_resource *const callback = wl_resource_create(client, &wl_callback_interface, 1, arguments[0].n);
		if (!callback) {
			wl_client_post_no_memory(client);
			return 0;
		}
		wl_resource_set_implementation(callback, nullptr, &relay, [](wl_resource *gone) {
			auto &relay = *static_cast<Relay *>(wl_resource_get_user_data(gone));
			const auto found = relay.frames.find(gone);
			if (found == relay.frames.end())
				return;
			wl_callback_destroy(found->second.waited);
			relay.frames.erase(found);
		});
		surface_of(resource)->callbacks.push_back(callback);
	} else if (request == "wl_surface.set_input_region") {
		Surface &surface = *surface_of(resource);
		surface.pending_input = object(0) ? std::optional(*static_cast<Region *>(wl_resource_get_user_data(object(0)))) : std::nullopt;
		surface.input_changed = true;
	} else if (request == "wl_surface.set_buffer_scale") {
		surface_of(resource)->pending_scale = arguments[0].i;
	} else if (request == "wl_surface.commit") {
		relay.commit(*surface_of(resource));
	} else if (request == "wl_subcompositor.get_subsurface") {
		Surface &surface = *surface_of(object(1));
		Surface &parent = *surface_of(object(2));
		surface.subsurface = relay.make(client, &wl_subsurface_interface, version, arguments[0].n, &surface, [](wl_resource *gone) {
			auto *const surface = static_cast<Surface *>(wl_resource_get_user_data(gone));
			if (!surface)
				return;
			auto *const relay = surface->relay;
			relay->take_back(*surface);
			if (surface->parent)
				std::erase(surface->parent->children, surface);
			surface->parent = nullptr;
			surface->subsurface = nullptr;
		});
		surface.parent = &parent;
		parent.children.push_back(&surface);
		if (parent.host) {
			relay.realize(surface, parent.host);
			relay.publish(surface);
		}
	} else if (request == "wl_subsurface.set_position") {
		Surface &surface = *surface_of(resource);
		surface.x = arguments[0].i;
		surface.y = arguments[1].i;
		if (surface.host_role)
			wl_subsurface_set_position(surface.host_role, surface.x, surface.y);
	} else if (request == "wl_subsurface.place_above" || request == "wl_subsurface.place_below") {
		Surface &surface = *surface_of(resource);
		Surface *const sibling = surface_of(object(0));
		if (surface.host_role && sibling && sibling->host)
			(request == "wl_subsurface.place_above" ? wl_subsurface_place_above : wl_subsurface_place_below)(surface.host_role, sibling->host);
	} else if (request == "wl_subsurface.set_sync" || request == "wl_subsurface.set_desync") {
		Surface &surface = *surface_of(resource);
		surface.synchronized = request == "wl_subsurface.set_sync";
		if (surface.host_role)
			(surface.synchronized ? wl_subsurface_set_sync : wl_subsurface_set_desync)(surface.host_role);
	} else if (request == "xdg_wm_base.create_positioner") {
		relay.make(client, &xdg_positioner_interface, version, arguments[0].n, nullptr, nullptr);
	} else if (request == "xdg_wm_base.get_xdg_surface") {
		Surface &surface = *surface_of(object(1));
		surface.shell = relay.make(client, &xdg_surface_interface, version, arguments[0].n, &surface, [](wl_resource *gone) {
			if (auto *const surface = static_cast<Surface *>(wl_resource_get_user_data(gone)))
				surface->shell = nullptr;
		});
	} else if (request == "xdg_surface.get_toplevel" || request == "xdg_surface.get_popup") {
		Surface &surface = *surface_of(resource);
		const bool toplevel = request == "xdg_surface.get_toplevel";
		wl_resource *const role = relay.make(client, toplevel ? &xdg_toplevel_interface : &xdg_popup_interface, version, arguments[0].n,
			&surface, [](wl_resource *gone) {
				auto *const surface = static_cast<Surface *>(wl_resource_get_user_data(gone));
				if (!surface)
					return;
				auto *const relay = surface->relay;
				if (surface->toplevel == gone) {
					relay->take_back(*surface);
					surface->toplevel = nullptr;
					surface->view = 0;
				} else
					surface->popup = nullptr;
				surface->configured = false;
			});
		(toplevel ? surface.toplevel : surface.popup) = role;
	} else if (request == "xdg_toplevel.set_app_id") {
		// The Wayland presentation tags a window shown inside a window of another program as raven-embed-ID (see wayland.cpp's embed).
		Surface &surface = *surface_of(resource);
		const std::string_view name = arguments[0].s ? arguments[0].s : "";
		constexpr std::string_view tag = "raven-embed-";
		const uint64_t view = name.starts_with(tag) ? std::strtoull(std::string(name.substr(tag.size())).c_str(), nullptr, 10) : 0;
		if (view != surface.view) {
			relay.take_back(surface);
			surface.view = view;
		}
	} else if (request == "wl_seat.get_pointer") {
		relay.pointers.insert(relay.make(client, &wl_pointer_interface, version, arguments[0].n, &relay,
			[](wl_resource *gone) { static_cast<Relay *>(wl_resource_get_user_data(gone))->pointers.erase(gone); }));
	} else if (request == "wl_seat.get_keyboard") {
		wl_resource *const made = relay.make(client, &wl_keyboard_interface, version, arguments[0].n, &relay,
			[](wl_resource *gone) { static_cast<Relay *>(wl_resource_get_user_data(gone))->keyboards.erase(gone); });
		relay.keyboards.insert(made);
		if (relay.keymap >= 0)
			wl_keyboard_send_keymap(made, relay.keymap_format, relay.keymap, relay.keymap_size);
		if (relay.repeat && version >= 4)
			wl_keyboard_send_repeat_info(made, relay.repeat->first, relay.repeat->second);
	} else if (request == "wl_seat.get_touch") {
		relay.make(client, &wl_touch_interface, version, arguments[0].n, nullptr, nullptr);
	}
	close_descriptor(descriptor);
	// Every request named destroy or release ends its object; the rest the relay has no use for (titles, sizes, the cursor) ask nothing.
	if (!std::strcmp(message->name, "destroy") || !std::strcmp(message->name, "release"))
		wl_resource_destroy(resource);
	return 0;
}

}

std::unique_ptr<WaylandRelay> WaylandRelay::start(int &client)
{
	auto relay = std::make_unique<Relay>();
	if (!relay->start(client))
		return nullptr;
	return relay;
}

}
