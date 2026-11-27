// The Wayland compositor a bridged plugin (plugin.cpp) gives the runtime when the host is a Wayland program. VST 3 has a host show a
// plugin's editor on a Wayland surface of its own (kPlatformTypeWaylandSurfaceID), on a connection the host opens for the plugin
// (IWaylandHost), where the plugin's own surface is a subsurface of the host's. The runtime is another process and draws its windows as
// any Wayland client does, so the plugin is its compositor: a toplevel the runtime tags as shown inside a window of the host's
// (raven-embed-ID, the Wayland presentation's embed) becomes a subsurface of the surface the host gave for that view, and the runtime's
// buffers are shown there as they are, from the same shared memory. The pointer and the keyboard over those surfaces go back to the
// runtime as its compositor's input.
//
// The compositor runs on a thread of its own, as the runtime connects to it and waits for its answers long before the host shows an
// editor; the host's connection is read there on a queue of the plugin's own.

#pragma once

#include <cstdint>
#include <memory>

struct wl_display;
struct wl_surface;

namespace raven_vst {

class WaylandRelay {
public:
	// Starts the compositor; `client` is the descriptor of the connection the runtime takes (WAYLAND_SOCKET).
	static std::unique_ptr<WaylandRelay> start(int &client);
	virtual ~WaylandRelay() = default;

	// Shows the runtime's toplevel tagged with `view` as a subsurface of `parent`, on the host's connection `display`: from now on if it is
	// shown already, and once it is otherwise. Fails for a display other than the one shown on so far.
	virtual bool show(uint64_t view, wl_display *display, wl_surface *parent) = 0;
	// Shows the toplevel tagged with `view` nowhere again.
	virtual void hide(uint64_t view) = 0;
	// Forgets the host's connection, once no view shows on it, before the host closes it.
	virtual void forget(wl_display *display) = 0;
};

}
