#define GL_GLEXT_PROTOTYPES
#include "opengl_bridge.h"
#include "desktop/objects.h"
#include "desktop/windows.h"
#include "nt/api.h"
#include "nt/threads.h"
#include "pe/module_set.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glu.h>
#ifdef RAVEN_OPENGL_WAYLAND
#include <wayland-egl.h>
#endif
#ifdef RAVEN_XCB
#include <xcb/xcb.h>
#endif
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <type_traits>
#include <sys/mman.h>

namespace raven {

namespace {

constexpr std::string_view names[] = {
	"glBegin", "glClear", "glClearColor", "glColor4f", "glDeleteLists", "glDisable", "glEnable", "glEnd", "glEndList",
	"glGenLists", "glGetError", "glGetString", "glLoadIdentity", "glMatrixMode", "glNewList", "glReadPixels",
	"glVertex2f", "glVertex3f", "glViewport", "glCallList",
	"glGenBuffers", "glDeleteBuffers", "glBindBuffer", "glBufferData", "glEnableClientState", "glDisableClientState",
	"glVertexPointer", "glDrawArrays",
	"glActiveTexture", "glAlphaFunc", "glAttachShader", "glBindFramebuffer", "glBindRenderbuffer",
	"glBindTexture", "glBindVertexArray", "glBlendFunc", "glBlitFramebuffer", "glCheckFramebufferStatus", "glClampColor",
	"glClearDepth", "glClientActiveTexture", "glColor3f", "glColorMask", "glCompileShader", "glCreateProgram", "glCreateShader",
	"glCullFace", "glDeleteProgram", "glDeleteShader", "glDepthFunc", "glDepthMask", "glDisableVertexAttribArray",
	"glDrawBuffer", "glEnableVertexAttribArray", "glFinish", "glFlush", "glFramebufferRenderbuffer", "glFramebufferTexture2D",
	"glFrontFace", "glFrustum", "glGetDoublev", "glGetFloatv", "glGetIntegerv", "glGetStringi", "glHint", "glLinkProgram",
	"glOrtho", "glPixelStorei", "glPolygonMode", "glPopMatrix", "glPushMatrix", "glReadBuffer", "glRenderbufferStorageMultisample",
	"glRotatef", "glScalef", "glScissor", "glShadeModel", "glStencilFunc", "glStencilMask", "glStencilOp", "glTexCoord2f",
	"glTexEnvf", "glTexGeni", "glTexImage2DMultisample", "glTexParameterf", "glTexParameteri", "glTranslatef",
	"glUniform1f", "glUniform1i", "glUniform2f", "glUniform3f", "glUniform4f", "glUseProgram",
	"glBindAttribLocation", "glColorPointer", "glDeleteTextures", "glDrawBuffers", "glDrawElements", "glGenFramebuffers",
	"glGenRenderbuffers", "glGenTextures", "glGenVertexArrays", "glGetProgramInfoLog", "glGetProgramiv", "glGetShaderInfoLog",
	"glGetShaderiv", "glGetTexLevelParameteriv", "glGetUniformLocation", "glNormalPointer", "glShaderSource", "glTexCoordPointer",
	"glTexGenfv", "glTexImage2D", "glTexImage3D", "glUniformMatrix4fv", "glVertexAttribPointer",
	"glColor4fv", "glColor4ub", "glIsTexture", "glLineWidth", "glRenderMode", "glTexSubImage2D", "wglShareLists",
	"glColorMaterial", "glCopyTexSubImage2D", "glInterleavedArrays", "glLightfv", "glLineStipple", "glPointSize", "glRectd",
	"wglGetExtensionsStringARB", "wglGetExtensionsStringEXT", "wglSwapIntervalEXT", "wglGetSwapIntervalEXT",
	"wglChoosePixelFormat", "wglCreateContext", "wglDeleteContext", "wglDescribePixelFormat", "wglGetCurrentContext",
	"wglGetCurrentDC", "wglGetPixelFormat", "wglGetProcAddress", "wglMakeCurrent", "wglSetPixelFormat", "wglSwapBuffers"
};

std::array<float, 4> floats(ApiCall &call)
{
	const auto bits = call.process.api_float_arguments();
	std::array<float, 4> result;
	for (size_t index = 0; index < result.size(); ++index)
		std::memcpy(&result[index], &bits[index], sizeof(float));
	return result;
}

template<typename T> T scalar_argument(ApiCall &call, const std::array<uint64_t, 4> &floating, size_t index)
{
	static_assert(!std::is_pointer_v<T>);
	if constexpr (std::is_floating_point_v<T>) {
		const auto bits = index < floating.size() ? floating[index] : call[index];
		T value;
		std::memcpy(&value, &bits, sizeof(value));
		return value;
	} else return T(call[index]);
}

template<typename Result, typename... Args, size_t... Index>
void scalar_call(ApiCall &call, Result (*function)(Args...), std::index_sequence<Index...>)
{
	std::array<uint64_t, 4> floating{};
	if constexpr ((std::is_floating_point_v<Args> || ...)) floating = call.process.api_float_arguments();
	if constexpr (std::is_void_v<Result>) function(scalar_argument<Args>(call, floating, Index)...);
	else call.ret(uint64_t(function(scalar_argument<Args>(call, floating, Index)...)));
}

size_t query_count(GLenum name)
{
	switch (name) {
	case GL_MODELVIEW_MATRIX: case GL_PROJECTION_MATRIX: case GL_TEXTURE_MATRIX: return 16;
	case GL_VIEWPORT: case GL_SCISSOR_BOX: case GL_CURRENT_COLOR: case GL_COLOR_CLEAR_VALUE: return 4;
	case GL_DEPTH_RANGE: case GL_ALIASED_LINE_WIDTH_RANGE: case GL_ALIASED_POINT_SIZE_RANGE: return 2;
	case GL_MAJOR_VERSION: case GL_MINOR_VERSION: case GL_NUM_EXTENSIONS: case GL_CONTEXT_FLAGS: case GL_CONTEXT_PROFILE_MASK:
	case GL_RESET_NOTIFICATION_STRATEGY: case GL_CONTEXT_RELEASE_BEHAVIOR: case GL_CONTEXT_ROBUST_ACCESS:
	case GL_MAX_TEXTURE_SIZE: case GL_MAX_3D_TEXTURE_SIZE: case GL_MAX_CUBE_MAP_TEXTURE_SIZE: case GL_MAX_TEXTURE_UNITS:
	case GL_MAX_TEXTURE_IMAGE_UNITS: case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: case GL_MAX_VERTEX_ATTRIBS:
	case GL_MAX_DRAW_BUFFERS: case GL_MAX_COLOR_ATTACHMENTS: case GL_MAX_SAMPLES: case GL_MAX_RENDERBUFFER_SIZE:
	case GL_RED_BITS: case GL_GREEN_BITS: case GL_BLUE_BITS: case GL_ALPHA_BITS: case GL_DEPTH_BITS: case GL_STENCIL_BITS:
	case GL_DOUBLEBUFFER: case GL_STEREO: case GL_SAMPLE_BUFFERS: case GL_SAMPLES: case GL_ACTIVE_TEXTURE:
	case GL_CLIENT_ACTIVE_TEXTURE: case GL_CURRENT_PROGRAM: case GL_ARRAY_BUFFER_BINDING: case GL_ELEMENT_ARRAY_BUFFER_BINDING:
	case GL_PACK_ALIGNMENT: case GL_UNPACK_ALIGNMENT: case GL_FRAMEBUFFER_BINDING: case GL_READ_FRAMEBUFFER_BINDING:
	case GL_TEXTURE_BINDING_2D: case GL_TEXTURE_BINDING_3D: case GL_TEXTURE_BINDING_CUBE_MAP: return 1;
	default: throw std::runtime_error("unsupported OpenGL query size: " + std::to_string(name));
	}
}

template<typename T, typename Function> void output(ApiCall &call, uint64_t address, size_t count, Function function)
{
	if (count > (256ULL << 20) / sizeof(T)) throw std::runtime_error("OpenGL output exceeds bounds");
	if (!count) { function(static_cast<T *>(nullptr)); return; }
	auto bytes = call.read(address, count * sizeof(T));
	call.write(address, bytes);
	std::vector<T> values(count);
	std::memcpy(values.data(), bytes.data(), bytes.size());
	function(values.data());
	call.write(address, {reinterpret_cast<const uint8_t *>(values.data()), bytes.size()});
}

template<typename T> std::vector<T> input(ApiCall &call, uint64_t address, size_t count)
{
	if (count > (256ULL << 20) / sizeof(T)) throw std::runtime_error("OpenGL input exceeds bounds");
	std::vector<T> values(count);
	if (count) {
		const auto bytes = call.read(address, count * sizeof(T));
		std::memcpy(values.data(), bytes.data(), bytes.size());
	}
	return values;
}

void require_array_buffer()
{
	GLint buffer = 0;
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &buffer);
	if (!buffer) throw std::runtime_error("OpenGL client arrays require guest copies");
}

void validate_arrays()
{
	for (const auto [array, binding] : {std::pair{GL_VERTEX_ARRAY, GL_VERTEX_ARRAY_BUFFER_BINDING},
		{GL_COLOR_ARRAY, GL_COLOR_ARRAY_BUFFER_BINDING}, {GL_NORMAL_ARRAY, GL_NORMAL_ARRAY_BUFFER_BINDING},
		{GL_INDEX_ARRAY, GL_INDEX_ARRAY_BUFFER_BINDING}, {GL_EDGE_FLAG_ARRAY, GL_EDGE_FLAG_ARRAY_BUFFER_BINDING},
		{GL_SECONDARY_COLOR_ARRAY, GL_SECONDARY_COLOR_ARRAY_BUFFER_BINDING}, {GL_FOG_COORD_ARRAY, GL_FOG_COORD_ARRAY_BUFFER_BINDING}}) {
		GLint buffer = 0;
		if (!glIsEnabled(GLenum(array))) continue;
		glGetIntegerv(GLenum(binding), &buffer);
		if (!buffer) throw std::runtime_error("OpenGL draw uses an uncopied client array");
	}
	GLint original = 0, textures = 0;
	glGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE, &original);
	glGetIntegerv(GL_MAX_TEXTURE_COORDS, &textures);
	if (textures < 0 || textures > 32) throw std::runtime_error("OpenGL texture array count exceeds bounds");
	for (GLint index = 0; index < textures; ++index) {
		glClientActiveTexture(GL_TEXTURE0 + index);
		GLint buffer = 0;
		if (glIsEnabled(GL_TEXTURE_COORD_ARRAY)) glGetIntegerv(GL_TEXTURE_COORD_ARRAY_BUFFER_BINDING, &buffer);
		if (glIsEnabled(GL_TEXTURE_COORD_ARRAY) && !buffer) {
			glClientActiveTexture(GLenum(original));
			throw std::runtime_error("OpenGL draw uses an uncopied texture array");
		}
	}
	glClientActiveTexture(GLenum(original));
	GLint attributes = 0;
	glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &attributes);
	if (attributes < 0 || attributes > 64) throw std::runtime_error("OpenGL vertex attribute count exceeds bounds");
	for (GLint index = 0; index < attributes; ++index) {
		GLint enabled = 0, buffer = 0;
		glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
		if (enabled) glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &buffer);
		if (enabled && !buffer) throw std::runtime_error("OpenGL draw uses an uncopied vertex attribute");
	}
}

size_t unpack_size(GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type)
{
	if (width < 0 || height < 0 || depth < 0 || width > 8192 || height > 8192 || depth > 2048)
		throw std::runtime_error("OpenGL texture dimensions exceed bounds");
	if (!width || !height || !depth) return 0;
	size_t components = 0, element = 0;
	switch (format) {
	case GL_RED: case GL_ALPHA: case GL_LUMINANCE: case GL_DEPTH_COMPONENT: components = 1; break;
	case GL_RG: case GL_LUMINANCE_ALPHA: components = 2; break;
	case GL_RGB: case GL_BGR: components = 3; break;
	case GL_RGBA: case GL_BGRA: components = 4; break;
	case GL_DEPTH_STENCIL: components = 1; break;
	default: throw std::runtime_error("unsupported OpenGL texture format");
	}
	switch (type) {
	case GL_UNSIGNED_BYTE: case GL_BYTE: element = 1; break;
	case GL_UNSIGNED_SHORT: case GL_SHORT: case GL_HALF_FLOAT: element = 2; break;
	case GL_UNSIGNED_INT: case GL_INT: case GL_FLOAT: element = 4; break;
	case GL_UNSIGNED_INT_24_8:
		if (format != GL_DEPTH_STENCIL) throw std::runtime_error("unsupported OpenGL packed texture format");
		element = 4; break;
	default: throw std::runtime_error("unsupported OpenGL texture element");
	}
	if (format == GL_DEPTH_STENCIL && type != GL_UNSIGNED_INT_24_8)
		throw std::runtime_error("unsupported OpenGL depth-stencil element");
	GLint alignment = 0, row = 0, image = 0, skip_rows = 0, skip_pixels = 0, skip_images = 0;
	glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment); glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row);
	glGetIntegerv(GL_UNPACK_IMAGE_HEIGHT, &image); glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_rows);
	glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_pixels); glGetIntegerv(GL_UNPACK_SKIP_IMAGES, &skip_images);
	if ((alignment != 1 && alignment != 2 && alignment != 4 && alignment != 8) || row < 0 || image < 0 ||
		skip_rows < 0 || skip_pixels < 0 || skip_images < 0 || row > 8192 || image > 8192 ||
		skip_rows > 8192 || skip_pixels > 8192 || skip_images > 2048)
		throw std::runtime_error("OpenGL unpack state exceeds bounds");
	const size_t stride = ((size_t(row ? row : width) * components * element + alignment - 1) / alignment) * alignment;
	const size_t layer = stride * size_t(image ? image : height);
	const size_t bytes = (size_t(skip_images) + depth - 1) * layer + (size_t(skip_rows) + height - 1) * stride +
		(size_t(skip_pixels) + width) * components * element;
	if (bytes > (256ULL << 20)) throw std::runtime_error("OpenGL texture upload exceeds bounds");
	return bytes;
}

}

struct OpenGLBridge::State {
	NativeProcess &process;
	ModuleSet &modules;
	DesktopObjects &objects;
	NativeWindows &windows;
	NativeThreads *threads = nullptr;
	EGLDisplay display = EGL_NO_DISPLAY;
	EGLConfig config = nullptr;
	// Whether contexts draw to host windows, and on which window system; without one they draw to offscreen buffers.
	bool windowed = false;
	HostPlatform platform = HostPlatform::wayland;
	bool queued_elements = std::getenv("RAVEN_QUEUED_GRAPHICS") && std::getenv("RAVEN_QUEUED_OPENGL");
	uint64_t queued_draws = 0;
	struct Context { EGLContext host; uint32_t owner = 0; int interval = 1; bool used = false, arrays_valid = false, elements = false; };
	struct Current { uint64_t context = 0, dc = 0; };
	struct Surface {
		EGLSurface host = EGL_NO_SURFACE;
#ifdef RAVEN_OPENGL_WAYLAND
		wl_egl_window *window = nullptr;
#endif
		// The X11 child window the surface draws to.
		uint32_t xid = 0;
		uint32_t width = 0, height = 0;
	};
	std::map<uint64_t, Context> contexts;
	std::map<uint32_t, Current> current;
	std::map<uint64_t, Surface> surfaces;
	std::map<uint64_t, uint32_t> formats;
	std::map<std::string, uint64_t> strings;
	uint64_t next_context = 0x4700000000000001ULL;

	~State()
	{
		windows.set_surface_cleanup({});
		if (display == EGL_NO_DISPLAY) return;
		eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		for (const auto &[token, context] : contexts) eglDestroyContext(display, context.host);
		while (!surfaces.empty()) drop_surface(surfaces.begin()->first);
		eglTerminate(display);
	}

	uint64_t dc_window(uint64_t handle) const
	{
		const auto *dc = objects.dc(handle);
		return dc && !dc->foreign && dc->window && objects.window_root(dc->window) == dc->window &&
			windows.client_size(dc->window) ? dc->window : 0;
	}

	bool initialize()
	{
		if (display != EGL_NO_DISPLAY) return true;
		void *native = windows.host_display();
		platform = windows.host_platform();
		EGLenum kind = EGL_PLATFORM_SURFACELESS_MESA;
#ifdef RAVEN_OPENGL_WAYLAND
		if (native && platform == HostPlatform::wayland) kind = EGL_PLATFORM_WAYLAND_KHR;
#endif
#ifdef RAVEN_XCB
		if (native && platform == HostPlatform::xcb) kind = EGL_PLATFORM_XCB_EXT;
#endif
		windowed = kind != EGL_PLATFORM_SURFACELESS_MESA;
		auto candidate = eglGetPlatformDisplay(kind, windowed ? native : nullptr, nullptr);
		if (candidate == EGL_NO_DISPLAY || !eglInitialize(candidate, nullptr, nullptr)) return false;
		if (!eglBindAPI(EGL_OPENGL_API)) { eglTerminate(candidate); return false; }
		// An X11 window takes only configurations of its visual: the root's, where the drawable has no alpha when the root is 24 bits deep.
		const bool xcb = windowed && platform == HostPlatform::xcb;
		const EGLint attributes[] = {EGL_SURFACE_TYPE, windowed ? EGL_WINDOW_BIT : EGL_PBUFFER_BIT,
			EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
			EGL_ALPHA_SIZE, xcb ? 0 : 8, EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE};
		EGLint count = 0;
		if (!xcb) {
			if (!eglChooseConfig(candidate, attributes, &config, 1, &count) || !count) { eglTerminate(candidate); return false; }
			display = candidate;
			return true;
		}
#ifdef RAVEN_XCB
		const auto visual = xcb_setup_roots_iterator(xcb_get_setup(static_cast<xcb_connection_t *>(native))).data->root_visual;
		std::array<EGLConfig, 256> configs{};
		if (!eglChooseConfig(candidate, attributes, configs.data(), EGLint(configs.size()), &count)) count = 0;
		config = nullptr;
		EGLint best_alpha = -1;
		for (EGLint index = 0; index < count; ++index) {
			EGLint id = 0, alpha = 0;
			eglGetConfigAttrib(candidate, configs[size_t(index)], EGL_NATIVE_VISUAL_ID, &id);
			eglGetConfigAttrib(candidate, configs[size_t(index)], EGL_ALPHA_SIZE, &alpha);
			if (uint32_t(id) == visual && alpha > best_alpha) { config = configs[size_t(index)]; best_alpha = alpha; }
		}
#endif
		if (!config) { eglTerminate(candidate); return false; }
		display = candidate;
		return true;
	}

	void drop_surface(uint64_t handle)
	{
		const auto found = surfaces.find(handle);
		if (found == surfaces.end()) return;
		if (found->second.host != EGL_NO_SURFACE &&
			(eglGetCurrentSurface(EGL_DRAW) == found->second.host || eglGetCurrentSurface(EGL_READ) == found->second.host))
			eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		if (found->second.host != EGL_NO_SURFACE) eglDestroySurface(display, found->second.host);
#ifdef RAVEN_OPENGL_WAYLAND
		if (found->second.window) {
			wl_egl_window_destroy(found->second.window);
			windows.release_host_surface(handle);
		}
#endif
		if (found->second.xid) windows.release_host_surface(handle);
		surfaces.erase(found);
	}

	EGLSurface surface(uint64_t handle)
	{
		const auto size = windows.client_size(handle);
		if (!size) throw std::runtime_error("OpenGL device context no longer has a window");
		if (size->first > 8192 || size->second > 8192 || uint64_t(size->first) * size->second > 16 * 1024 * 1024)
			throw std::runtime_error("OpenGL drawing surface exceeds bounds");
		auto &target = surfaces[handle];
		if (target.host != EGL_NO_SURFACE && (target.width != size->first || target.height != size->second)) {
			if ((eglGetCurrentSurface(EGL_DRAW) == target.host || eglGetCurrentSurface(EGL_READ) == target.host) &&
				!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT))
				throw std::runtime_error("cannot release OpenGL surface for resize");
			// A fresh drawable has the new size before the first draw, while its context keeps all GL objects and state.
			if (!eglDestroySurface(display, target.host)) throw std::runtime_error("cannot resize OpenGL drawing surface");
			target.host = EGL_NO_SURFACE;
#ifdef RAVEN_OPENGL_WAYLAND
			if (target.window) wl_egl_window_resize(target.window, int(size->first), int(size->second), 0, 0);
#endif
			// The X11 child window has a size of its own, which follows the client area.
			if (target.xid && !windows.host_surface(handle)) throw std::runtime_error("OpenGL window lost its host surface");
		}
		target.width = size->first;
		target.height = size->second;
		if (target.host != EGL_NO_SURFACE) return target.host;
		if (windowed && platform == HostPlatform::xcb) {
#ifdef RAVEN_XCB
			if (!target.xid) {
				const auto native = windows.host_surface(handle);
				if (!native) return EGL_NO_SURFACE;
				if (native->platform != HostPlatform::xcb || eglGetPlatformDisplay(EGL_PLATFORM_XCB_EXT, native->display, nullptr) != display)
					throw std::runtime_error("OpenGL window belongs to another display");
				target.xid = native->window;
			}
			xcb_window_t window = target.xid;
			target.host = eglCreatePlatformWindowSurface(display, config, &window, nullptr);
#endif
		} else if (windowed) {
#ifdef RAVEN_OPENGL_WAYLAND
			if (!target.window) {
				const auto native = windows.host_surface(handle);
				if (!native) return EGL_NO_SURFACE;
				if (native->platform != HostPlatform::wayland || eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, native->display, nullptr) != display)
					throw std::runtime_error("OpenGL window belongs to another display");
				target.window = wl_egl_window_create(static_cast<wl_surface *>(native->surface), int(target.width), int(target.height));
				if (!target.window) throw std::runtime_error("cannot create OpenGL Wayland window");
			}
			target.host = eglCreateWindowSurface(display, config, reinterpret_cast<EGLNativeWindowType>(target.window), nullptr);
#endif
		} else {
			const EGLint dimensions[] = {EGL_WIDTH, int(target.width), EGL_HEIGHT, int(target.height), EGL_NONE};
			target.host = eglCreatePbufferSurface(display, config, dimensions);
		}
		if (target.host == EGL_NO_SURFACE) {
			drop_surface(handle);
			throw std::runtime_error("cannot create OpenGL drawing surface");
		}
		return target.host;
	}

	Current selected(ApiCall &call) const
	{
		const auto found = current.find(call.environment.thread_id());
		return found == current.end() ? Current{} : found->second;
	}

	void release_finished_owner(Context &context)
	{
		if (!context.owner) return;
		const auto thread = threads->find(context.owner);
		if (thread && thread->state != GuestThread::State::done) return;
		current.erase(context.owner);
		context.owner = 0;
	}

	void activate(ApiCall &call)
	{
		const auto selected = this->selected(call);
		const auto found = contexts.find(selected.context);
		const auto window = dc_window(selected.dc);
		if (found == contexts.end() || !window) throw std::runtime_error("OpenGL call has no current rendering context");
		const auto draw = surface(window);
		if (eglGetCurrentContext() != found->second.host || eglGetCurrentSurface(EGL_DRAW) != draw) {
			if (!eglMakeCurrent(display, draw, draw, found->second.host)) throw std::runtime_error("cannot bind OpenGL rendering context");
			if (draw != EGL_NO_SURFACE && !eglSwapInterval(display, found->second.interval))
				throw std::runtime_error("cannot restore OpenGL swap interval");
		}
	}

	void validate_draw_arrays(ApiCall &call)
	{
		auto &context = contexts.at(selected(call).context);
		if (context.arrays_valid) return;
		validate_arrays();
		context.arrays_valid = true;
	}

	void publish_elements(ApiCall &call, bool refresh = false)
	{
		if (!queued_elements) return;
		const auto found = contexts.find(selected(call).context);
		// A context switch can happen inside a primitive; only binding changes may query GL state.
		if (refresh && found != contexts.end()) {
			GLint buffer = 0;
			glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &buffer);
			found->second.elements = buffer != 0;
		}
		process.set_queued_gl_elements(found != contexts.end() && found->second.elements);
	}

	uint64_t string(std::string text)
	{
		if (const auto found = strings.find(text); found != strings.end()) return found->second;
		if (text.size() > 65535 || strings.size() >= 1024) throw std::runtime_error("OpenGL string table exceeds bounds");
		const auto size = (text.size() + 4096) & ~size_t(4095);
		const auto address = process.map_memory(size, PROT_READ | PROT_WRITE);
		process.write_memory(address, {reinterpret_cast<const uint8_t *>(text.c_str()), text.size() + 1});
		process.protect_memory(address, size, PROT_READ);
		strings.emplace(std::move(text), address);
		return address;
	}

	std::array<uint8_t, 40> descriptor() const
	{
		std::array<uint8_t, 40> result{};
		result[0] = 40; result[2] = 1; result[4] = 0x25;
		for (const auto [field, attribute] : {std::pair{9, EGL_BUFFER_SIZE}, {10, EGL_RED_SIZE}, {12, EGL_GREEN_SIZE},
			{14, EGL_BLUE_SIZE}, {16, EGL_ALPHA_SIZE}, {23, EGL_DEPTH_SIZE}, {24, EGL_STENCIL_SIZE}}) {
			EGLint value = 0;
			if (!eglGetConfigAttrib(display, config, attribute, &value) || value < 0 || value > 255)
				throw std::runtime_error("invalid OpenGL pixel-format attribute");
			result[size_t(field)] = uint8_t(value);
		}
		result[11] = uint8_t(result[14] + result[12]); result[13] = result[14];
		result[17] = uint8_t(result[10] + result[12] + result[14]);
		return result;
	}
};

OpenGLBridge::OpenGLBridge(NativeProcess &process, ModuleSet &modules, DesktopObjects &objects, NativeWindows &windows)
	: state(std::make_unique<State>(process, modules, objects, windows))
{
	windows.set_surface_cleanup([this](uint64_t window) { state->drop_surface(window); });
}

OpenGLBridge::~OpenGLBridge() = default;

// The bridge is the OpenGL of 64-bit programs unless a guest build of the libraries is supplied or RAVEN_NATIVE_OPENGL=0 turns it off.
bool OpenGLBridge::enabled()
{
	const auto *value = std::getenv("RAVEN_NATIVE_OPENGL");
	if (value) return std::string_view(value) == "1";
	return !std::getenv("RAVEN_OPENGL_DIRECTORY");
}

bool OpenGLBridge::requested()
{
	const auto *value = std::getenv("RAVEN_NATIVE_OPENGL");
	return value && std::string_view(value) == "1";
}

bool OpenGLBridge::served_api(std::string_view name)
{
	// Keep the trapping path available for comparisons with the same executable and components.
	if (name == "glDrawElements") return std::getenv("RAVEN_PARALLEL") && !std::getenv("RAVEN_SYNC_INDEXED_GL");
	// Guest strings and output buffers stay at a stop; indexed drawing copies only its input indices.
	static constexpr std::string_view scalar[] = {
		"glActiveTexture", "glAlphaFunc", "glAttachShader", "glBegin", "glBindBuffer", "glBindFramebuffer", "glBindRenderbuffer",
		"glBindTexture", "glBindVertexArray", "glBlendFunc", "glBlitFramebuffer", "glCallList", "glCheckFramebufferStatus", "glClampColor",
		"glClear", "glClearColor", "glClearDepth", "glClientActiveTexture", "glColor3f", "glColor4f", "glColor4ub", "glColorMask",
		"glColorMaterial", "glColorPointer", "glCompileShader", "glCopyTexSubImage2D", "glCreateProgram", "glCreateShader", "glCullFace", "glDeleteLists", "glDeleteProgram", "glDeleteShader",
		"glDepthFunc", "glDepthMask", "glDisable", "glDisableClientState", "glDisableVertexAttribArray", "glDrawArrays", "glDrawBuffer",
		"glEnable", "glEnableClientState", "glEnableVertexAttribArray", "glEnd", "glEndList", "glFinish", "glFlush",
		"glFramebufferRenderbuffer", "glFramebufferTexture2D", "glFrontFace", "glFrustum", "glGenLists", "glGetError", "glHint",
		"glIsTexture", "glLineStipple", "glLineWidth", "glLinkProgram", "glLoadIdentity", "glMatrixMode", "glNewList", "glNormalPointer", "glOrtho", "glPixelStorei", "glPointSize", "glPolygonMode",
		"glPopMatrix", "glPushMatrix", "glReadBuffer", "glRectd", "glRenderMode", "glRenderbufferStorageMultisample", "glRotatef", "glScalef", "glScissor",
		"glShadeModel", "glStencilFunc", "glStencilMask", "glStencilOp", "glTexCoord2f", "glTexCoordPointer", "glTexEnvf", "glTexGeni",
		"glTexImage2DMultisample", "glTexParameterf", "glTexParameteri", "glTranslatef", "glUniform1f", "glUniform1i", "glUniform2f",
		"glUniform3f", "glUniform4f", "glUseProgram", "glVertex2f", "glVertex3f", "glVertexAttribPointer", "glVertexPointer", "glViewport"
	};
	return std::find(std::begin(scalar), std::end(scalar), name) != std::end(scalar);
}

std::span<const std::string_view> OpenGLBridge::exports()
{
	static const auto sorted = [] {
		std::vector<std::string_view> result(std::begin(names), std::end(names));
		std::sort(result.begin(), result.end());
		return result;
	}();
	return sorted;
}

void OpenGLBridge::register_api(ApiRegistry &registry, NativeThreads &threads)
{
	auto &s = *state;
	s.threads = &threads;
	const auto choose = [&s](ApiCall &call) {
		if (!s.dc_window(call[0])) { call.fail(6); return; }
		if (!call[1] || call.load<uint16_t>(call[1]) < 40 || call.load<uint16_t>(call[1] + 2) != 1 ||
			call.load<uint8_t>(call[1] + 8) != 0) { call.fail(2000); return; }
		if (!s.initialize()) { call.fail(50); return; }
		call.ret(1);
	};
	const auto describe = [&s](ApiCall &call) {
		if (!s.dc_window(call[0])) { call.fail(6); return; }
		if (call.u32(1) != 1 || (call[3] && call.u32(2) < 40)) { call.fail(2000); return; }
		if (!s.initialize()) { call.fail(50); return; }
		if (call[3]) call.write(call[3], s.descriptor());
		call.ret(1);
	};
	const auto set = [&s](ApiCall &call) {
		const auto window = s.dc_window(call[0]);
		if (!window) { call.fail(6); return; }
		if (call.u32(1) != 1 || s.formats.contains(window)) { call.fail(2000); return; }
		if (!s.initialize()) { call.fail(50); return; }
		s.formats[window] = 1;
		call.ret(1);
	};
	const auto get = [&s](ApiCall &call) {
		const auto window = s.dc_window(call[0]);
		if (!window) { call.fail(6); return; }
		const auto found = s.formats.find(window);
		call.ret(found == s.formats.end() ? 0 : found->second);
	};
	const auto swap = [&s](ApiCall &call) {
		if (s.selected(call).dc != call[0]) { call.fail(6); return; }
		s.activate(call);
		const auto window = s.dc_window(call[0]);
		const auto draw = s.surfaces.at(window).host;
		if (!s.windowed || draw == EGL_NO_SURFACE || !eglSwapBuffers(s.display, draw)) { call.fail(50); return; }
		call.ret(1);
	};
	for (const auto prefix : {"", "wgl"}) {
		registry.add(std::string(prefix) + "ChoosePixelFormat", 2, choose);
		registry.add(std::string(prefix) + "DescribePixelFormat", 4, describe);
		registry.add(std::string(prefix) + "SetPixelFormat", 3, set);
		registry.add(std::string(prefix) + "GetPixelFormat", 1, get);
		registry.add(std::string(prefix) + "SwapBuffers", 1, swap);
	}
	registry.add("wglCreateContext", 1, [&s](ApiCall &call) {
		const auto window = s.dc_window(call[0]);
		if (!window || !s.formats.contains(window)) { call.fail(2000); return; }
		const auto context = eglCreateContext(s.display, s.config, EGL_NO_CONTEXT, nullptr);
		if (context == EGL_NO_CONTEXT) { call.fail(50); return; }
		const auto token = s.next_context++;
		s.contexts.emplace(token, State::Context{context});
		call.ret(token);
	});
	registry.add("wglMakeCurrent", 2, [&s](ApiCall &call) {
		const auto thread = call.environment.thread_id();
		const auto previous = s.selected(call);
		if (!call[1]) {
			if (!previous.context) { s.process.set_queued_gl_elements(false); call.ret(1); return; }
			if (!eglMakeCurrent(s.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT)) { call.fail(50); return; }
			if (const auto found = s.contexts.find(previous.context); found != s.contexts.end()) found->second.owner = 0;
			s.current.erase(thread);
			s.publish_elements(call);
			call.ret(1);
			return;
		}
		const auto found = s.contexts.find(call[1]);
		const auto window = s.dc_window(call[0]);
		if (found == s.contexts.end() || !window || !s.formats.contains(window)) { call.fail(6); return; }
		s.release_finished_owner(found->second);
		if (found->second.owner && found->second.owner != thread) { call.fail(170); return; }
		const auto draw = s.surface(window);
		if (!eglMakeCurrent(s.display, draw, draw, found->second.host)) { call.fail(50); return; }
		if (draw != EGL_NO_SURFACE && !eglSwapInterval(s.display, found->second.interval)) { call.fail(50); return; }
		if (const auto old = s.contexts.find(previous.context); old != s.contexts.end()) old->second.owner = 0;
		found->second.owner = thread;
		found->second.used = true;
		s.current[thread] = {call[1], call[0]};
		s.publish_elements(call);
		call.ret(1);
	});
	registry.add("wglDeleteContext", 1, [&s](ApiCall &call) {
		const auto found = s.contexts.find(call[0]);
		if (found == s.contexts.end()) { call.fail(6); return; }
		s.release_finished_owner(found->second);
		if (found->second.owner && found->second.owner != call.environment.thread_id()) { call.fail(170); return; }
		if (found->second.owner || eglGetCurrentContext() == found->second.host) {
			if (!eglMakeCurrent(s.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT)) { call.fail(50); return; }
			s.current.erase(found->second.owner);
		}
		if (!eglDestroyContext(s.display, found->second.host)) { call.fail(50); return; }
		s.contexts.erase(found);
		if (s.contexts.empty()) while (!s.surfaces.empty()) s.drop_surface(s.surfaces.begin()->first);
		s.publish_elements(call);
		call.ret(1);
	});
	registry.add("wglShareLists", 2, [&s](ApiCall &call) {
		const auto source = s.contexts.find(call[0]), destination = s.contexts.find(call[1]);
		if (source == s.contexts.end() || destination == s.contexts.end() || source == destination) { call.fail(6); return; }
		if (destination->second.used) { call.fail(50); return; }
		const auto shared = eglCreateContext(s.display, s.config, source->second.host, nullptr);
		if (shared == EGL_NO_CONTEXT) { call.fail(50); return; }
		if (!eglDestroyContext(s.display, destination->second.host)) { eglDestroyContext(s.display, shared); call.fail(50); return; }
		destination->second.host = shared;
		call.ret(1);
	});
	registry.add("wglGetCurrentContext", 0, [&s](ApiCall &call) { call.ret(s.selected(call).context); });
	registry.add("wglGetCurrentDC", 0, [&s](ApiCall &call) { call.ret(s.selected(call).dc); });
	registry.add("wglGetProcAddress", 1, [&s](ApiCall &call) {
		if (!s.selected(call).context) { call.fail(6); return; }
		const auto name = call.narrow(call[0], 256);
		const auto exports = OpenGLBridge::exports();
		if (!std::binary_search(exports.begin(), exports.end(), name)) { call.ret(0); return; }
		if (name.starts_with("gl") && !eglGetProcAddress(name.c_str())) { call.ret(0); return; }
		call.ret(s.modules.resolve({"opengl32.dll", name, 0, false, 0}));
	});
	const auto add = [&registry, &s](const char *name, uint32_t count, ApiHandler handler) {
		static constexpr std::array<std::string_view, 13> array_changes{
			"glBindVertexArray", "glCallList", "glColorPointer", "glDeleteBuffers", "glDisableClientState", "glDisableVertexAttribArray",
			"glEnableClientState", "glInterleavedArrays", "glEnableVertexAttribArray", "glNormalPointer", "glTexCoordPointer", "glVertexAttribPointer", "glVertexPointer"};
		const bool changed = std::find(array_changes.begin(), array_changes.end(), name) != array_changes.end();
		const bool elements = std::string_view(name) == "glBindVertexArray" || std::string_view(name) == "glDeleteBuffers";
		const bool binding = std::string_view(name) == "glBindBuffer";
		registry.add(name, count, [&s, changed, elements, binding, handler = std::move(handler)](ApiCall &call) {
			s.activate(call);
			// Only array state changes invalidate the check; buffer binds alone leave the recorded array sources intact.
			if (changed) s.contexts.at(s.selected(call).context).arrays_valid = false;
			handler(call);
			if (elements || (binding && call.u32(0) == GL_ELEMENT_ARRAY_BUFFER)) s.publish_elements(call, true);
		});
	};
	const auto scalar = [&add]<typename Result, typename... Args>(const char *name, Result (*function)(Args...)) {
		add(name, sizeof...(Args), [function](ApiCall &call) { scalar_call(call, function, std::index_sequence_for<Args...>{}); });
	};
	scalar("glActiveTexture", glActiveTexture);
	scalar("glAlphaFunc", glAlphaFunc);
	scalar("glAttachShader", glAttachShader);
	scalar("glBindFramebuffer", glBindFramebuffer);
	scalar("glBindRenderbuffer", glBindRenderbuffer);
	scalar("glBindTexture", glBindTexture);
	scalar("glBindVertexArray", glBindVertexArray);
	scalar("glBlendFunc", glBlendFunc);
	scalar("glBlitFramebuffer", glBlitFramebuffer);
	scalar("glCheckFramebufferStatus", glCheckFramebufferStatus);
	scalar("glClampColor", glClampColor);
	scalar("glClearDepth", glClearDepth);
	scalar("glClientActiveTexture", glClientActiveTexture);
	scalar("glColor3f", glColor3f);
	scalar("glColorMask", glColorMask);
	scalar("glCompileShader", glCompileShader);
	scalar("glCreateProgram", glCreateProgram);
	scalar("glCreateShader", glCreateShader);
	scalar("glCullFace", glCullFace);
	scalar("glDeleteProgram", glDeleteProgram);
	scalar("glDeleteShader", glDeleteShader);
	scalar("glDepthFunc", glDepthFunc);
	scalar("glDepthMask", glDepthMask);
	scalar("glDisableVertexAttribArray", glDisableVertexAttribArray);
	scalar("glDrawBuffer", glDrawBuffer);
	scalar("glEnableVertexAttribArray", glEnableVertexAttribArray);
	scalar("glFinish", glFinish);
	scalar("glFlush", glFlush);
	scalar("glFramebufferRenderbuffer", glFramebufferRenderbuffer);
	scalar("glFramebufferTexture2D", glFramebufferTexture2D);
	scalar("glFrontFace", glFrontFace);
	scalar("glFrustum", glFrustum);
	scalar("glHint", glHint);
	scalar("glLinkProgram", glLinkProgram);
	scalar("glOrtho", glOrtho);
	scalar("glPixelStorei", glPixelStorei);
	scalar("glPolygonMode", glPolygonMode);
	scalar("glPopMatrix", glPopMatrix);
	scalar("glPushMatrix", glPushMatrix);
	scalar("glReadBuffer", glReadBuffer);
	scalar("glRenderbufferStorageMultisample", glRenderbufferStorageMultisample);
	scalar("glRotatef", glRotatef);
	scalar("glScalef", glScalef);
	scalar("glScissor", glScissor);
	scalar("glShadeModel", glShadeModel);
	scalar("glStencilFunc", glStencilFunc);
	scalar("glStencilMask", glStencilMask);
	scalar("glStencilOp", glStencilOp);
	scalar("glTexCoord2f", glTexCoord2f);
	scalar("glTexEnvf", glTexEnvf);
	scalar("glTexGeni", glTexGeni);
	scalar("glTexImage2DMultisample", glTexImage2DMultisample);
	scalar("glTexParameterf", glTexParameterf);
	scalar("glTexParameteri", glTexParameteri);
	scalar("glTranslatef", glTranslatef);
	scalar("glUniform1f", glUniform1f);
	scalar("glUniform1i", glUniform1i);
	scalar("glUniform2f", glUniform2f);
	scalar("glUniform3f", glUniform3f);
	scalar("glUniform4f", glUniform4f);
	scalar("glUseProgram", glUseProgram);
	scalar("glColor4ub", glColor4ub);
	scalar("glIsTexture", glIsTexture);
	scalar("glLineWidth", glLineWidth);
	scalar("glLineStipple", glLineStipple);
	scalar("glPointSize", glPointSize);
	scalar("glRectd", glRectd);
	scalar("glColorMaterial", glColorMaterial);
	scalar("glCopyTexSubImage2D", glCopyTexSubImage2D);
	add("glLightfv", 3, [](ApiCall &call) {
		// Colours and the position are four values, the spot direction three, every other parameter one.
		const auto name = call.u32(1);
		const size_t count = name == GL_AMBIENT || name == GL_DIFFUSE || name == GL_SPECULAR || name == GL_POSITION ? 4 :
			name == GL_SPOT_DIRECTION ? 3 : 1;
		const auto values = input<GLfloat>(call, call[2], count);
		glLightfv(call.u32(0), name, values.data());
	});
	scalar("glRenderMode", glRenderMode);
	add("glColor4fv", 1, [](ApiCall &call) {
		const auto values = input<GLfloat>(call, call[0], 4);
		glColor4fv(values.data());
	});
	add("glTexSubImage2D", 9, [](ApiCall &call) {
		const auto size = unpack_size(call.i32(4), call.i32(5), 1, call.u32(6), call.u32(7));
		GLint buffer = 0;
		glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &buffer);
		std::vector<uint8_t> bytes;
		const void *data = nullptr;
		if (buffer) {
			GLint64 capacity = 0;
			glGetBufferParameteri64v(GL_PIXEL_UNPACK_BUFFER, GL_BUFFER_SIZE, &capacity);
			if (capacity < 0 || call[8] > uint64_t(capacity) || size > uint64_t(capacity) - call[8])
				throw std::runtime_error("OpenGL texture update offset exceeds its buffer");
			data = reinterpret_cast<const void *>(call[8]);
		} else if (size) { bytes = call.read(call[8], size); data = bytes.data(); }
		glTexSubImage2D(call.u32(0), call.i32(1), call.i32(2), call.i32(3), call.i32(4), call.i32(5), call.u32(6), call.u32(7), data);
	});
	scalar("gluPerspective", gluPerspective);
	scalar("gluLookAt", gluLookAt);
	scalar("gluOrtho2D", gluOrtho2D);
	add("gluErrorString", 1, [&s](ApiCall &call) {
		const auto *text = gluErrorString(call.u32(0));
		call.ret(text ? s.string(reinterpret_cast<const char *>(text)) : 0);
	});
	add("gluBuild2DMipmaps", 7, [](ApiCall &call) {
		GLint buffer = 0;
		glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &buffer);
		if (buffer) throw std::runtime_error("GLU mipmap input cannot reference an unpack buffer");
		const auto size = unpack_size(call.i32(2), call.i32(3), 1, call.u32(4), call.u32(5));
		const auto bytes = input<uint8_t>(call, call[6], size);
		call.ret(uint64_t(int64_t(gluBuild2DMipmaps(call.u32(0), call.i32(1), call.i32(2), call.i32(3), call.u32(4), call.u32(5), bytes.data()))));
	});
	for (const auto [name, function] : {std::pair{"gluProject", &gluProject}, {"gluUnProject", gluUnProject}})
		add(name, 9, [function](ApiCall &call) {
			const auto floating = call.process.api_float_arguments();
			const auto model = input<GLdouble>(call, call[3], 16), projection = input<GLdouble>(call, call[4], 16);
			const auto viewport = input<GLint>(call, call[5], 4);
			std::array<GLdouble, 3> result;
			for (size_t index = 0; index < result.size(); ++index) {
				result[index] = call.load<GLdouble>(call[6 + index]);
				call.store(call[6 + index], result[index]);
			}
			const auto status = function(scalar_argument<GLdouble>(call, floating, 0), scalar_argument<GLdouble>(call, floating, 1),
				scalar_argument<GLdouble>(call, floating, 2), model.data(), projection.data(), viewport.data(), &result[0], &result[1], &result[2]);
			for (size_t index = 0; index < result.size(); ++index) call.store(call[6 + index], result[index]);
			call.ret(uint64_t(status));
		});
	for (const auto name : {"wglGetExtensionsStringARB", "wglGetExtensionsStringEXT"})
		registry.add(name, std::string_view(name).ends_with("ARB") ? 1 : 0, [&s](ApiCall &call) {
			if (!s.selected(call).context) { call.fail(6); return; }
			call.ret(s.string("WGL_EXT_extensions_string WGL_ARB_extensions_string WGL_EXT_swap_control"));
		});
	add("wglSwapIntervalEXT", 1, [&s](ApiCall &call) {
		if (call.i32(0) < 0 || !eglSwapInterval(s.display, call.i32(0))) { call.fail(87); return; }
		s.contexts.at(s.selected(call).context).interval = call.i32(0);
		call.ret(1);
	});
	add("wglGetSwapIntervalEXT", 0, [&s](ApiCall &call) { call.ret(s.contexts.at(s.selected(call).context).interval); });
	add("glGetStringi", 2, [&s](ApiCall &call) {
		const auto *text = glGetStringi(call.u32(0), call.u32(1));
		call.ret(text ? s.string(reinterpret_cast<const char *>(text)) : 0);
	});
	add("glGetIntegerv", 2, [](ApiCall &call) {
		output<GLint>(call, call[1], query_count(call.u32(0)), [&](auto *values) { glGetIntegerv(call.u32(0), values); });
	});
	add("glGetFloatv", 2, [](ApiCall &call) {
		output<GLfloat>(call, call[1], query_count(call.u32(0)), [&](auto *values) { glGetFloatv(call.u32(0), values); });
	});
	add("glGetDoublev", 2, [](ApiCall &call) {
		output<GLdouble>(call, call[1], query_count(call.u32(0)), [&](auto *values) { glGetDoublev(call.u32(0), values); });
	});
	for (const auto [name, function] : {std::pair{"glGenTextures", &glGenTextures}, {"glGenFramebuffers", glGenFramebuffers},
		{"glGenRenderbuffers", glGenRenderbuffers}, {"glGenVertexArrays", glGenVertexArrays}})
		add(name, 2, [function](ApiCall &call) {
			if (call.i32(0) < 0 || call.u32(0) > 65536) throw std::runtime_error("OpenGL object count exceeds bounds");
			output<GLuint>(call, call[1], call.u32(0), [&](auto *values) { function(call.i32(0), values); });
		});
	add("glDeleteTextures", 2, [](ApiCall &call) {
		if (call.i32(0) < 0 || call.u32(0) > 65536) throw std::runtime_error("OpenGL texture count exceeds bounds");
		const auto values = input<GLuint>(call, call[1], call.u32(0));
		glDeleteTextures(call.i32(0), values.data());
	});
	add("glDrawBuffers", 2, [](ApiCall &call) {
		if (call.i32(0) < 0 || call.u32(0) > 32) throw std::runtime_error("OpenGL draw buffer count exceeds bounds");
		const auto values = input<GLenum>(call, call[1], call.u32(0));
		glDrawBuffers(call.i32(0), values.data());
	});
	add("glShaderSource", 4, [](ApiCall &call) {
		if (call.i32(1) < 0 || call.u32(1) > 256) throw std::runtime_error("OpenGL shader string count exceeds bounds");
		std::vector<std::string> strings;
		std::vector<const char *> pointers;
		std::vector<GLint> lengths;
		size_t total = 0;
		for (uint32_t index = 0; index < call.u32(1); ++index) {
			const auto pointer = call.load_pointer(call[2] + index * call.pointer_width());
			const auto length = call[3] ? call.load<GLint>(call[3] + index * sizeof(GLint)) : -1;
			if (length > 4 * 1024 * 1024) throw std::runtime_error("OpenGL shader string exceeds bounds");
			if (length < 0) strings.push_back(call.narrow(pointer, 4 * 1024 * 1024));
			else {
				const auto bytes = input<char>(call, pointer, size_t(length));
				strings.emplace_back(bytes.begin(), bytes.end());
			}
			total += strings.back().size();
			if (total > 16 * 1024 * 1024) throw std::runtime_error("OpenGL shader sources exceed bounds");
		}
		for (const auto &text : strings) { pointers.push_back(text.c_str()); lengths.push_back(GLint(text.size())); }
		glShaderSource(call.u32(0), call.i32(1), pointers.data(), lengths.data());
	});
	for (const auto [name, function] : {std::pair{"glGetShaderiv", &glGetShaderiv}, {"glGetProgramiv", glGetProgramiv}})
		add(name, 3, [function](ApiCall &call) {
			const size_t count = function == glGetProgramiv && call.u32(1) == GL_COMPUTE_WORK_GROUP_SIZE ? 3 : 1;
			output<GLint>(call, call[2], count, [&](auto *values) { function(call.u32(0), call.u32(1), values); });
		});
	for (const auto [name, function] : {std::pair{"glGetShaderInfoLog", &glGetShaderInfoLog}, {"glGetProgramInfoLog", glGetProgramInfoLog}})
		add(name, 4, [function](ApiCall &call) {
			if (call.i32(1) < 0 || call.u32(1) > 4 * 1024 * 1024) throw std::runtime_error("OpenGL info log exceeds bounds");
			GLsizei length = 0;
			if (call[2]) { length = call.load<GLsizei>(call[2]); call.store(call[2], length); }
			output<char>(call, call[3], call.u32(1), [&](auto *text) { function(call.u32(0), call.i32(1), &length, text); });
			if (call[2]) call.store(call[2], length);
		});
	add("glGetUniformLocation", 2, [](ApiCall &call) {
		const auto name = call.narrow(call[1], 65536);
		call.ret(uint64_t(int64_t(glGetUniformLocation(call.u32(0), name.c_str()))));
	});
	add("glBindAttribLocation", 3, [](ApiCall &call) {
		const auto name = call.narrow(call[2], 65536);
		glBindAttribLocation(call.u32(0), call.u32(1), name.c_str());
	});
	add("glUniformMatrix4fv", 4, [](ApiCall &call) {
		if (call.i32(1) < 0 || call.u32(1) > 65536) throw std::runtime_error("OpenGL uniform matrix count exceeds bounds");
		const auto values = input<GLfloat>(call, call[3], size_t(call.u32(1)) * 16);
		glUniformMatrix4fv(call.i32(0), call.i32(1), GLboolean(call.u32(2)), values.data());
	});
	add("glTexGenfv", 3, [](ApiCall &call) {
		const auto name = call.u32(1);
		if (name != GL_OBJECT_PLANE && name != GL_EYE_PLANE && name != GL_TEXTURE_GEN_MODE)
			throw std::runtime_error("unsupported OpenGL texture generation query");
		const auto values = input<GLfloat>(call, call[2], name == GL_TEXTURE_GEN_MODE ? 1 : 4);
		glTexGenfv(call.u32(0), name, values.data());
	});
	add("glGetTexLevelParameteriv", 4, [](ApiCall &call) {
		output<GLint>(call, call[3], 1, [&](auto *values) { glGetTexLevelParameteriv(call.u32(0), call.i32(1), call.u32(2), values); });
	});
	for (const auto dimensions : {2, 3})
		add(dimensions == 2 ? "glTexImage2D" : "glTexImage3D", dimensions == 2 ? 9 : 10, [dimensions](ApiCall &call) {
			const size_t tail = dimensions == 2 ? 6 : 7;
			const auto pointer = call[tail + 2];
			const bool proxy = call.u32(0) == GL_PROXY_TEXTURE_2D || call.u32(0) == GL_PROXY_TEXTURE_3D ||
				call.u32(0) == GL_PROXY_TEXTURE_CUBE_MAP || call.u32(0) == GL_PROXY_TEXTURE_RECTANGLE;
			const size_t size = proxy ? 0 : unpack_size(call.i32(3), call.i32(4), dimensions == 2 ? 1 : call.i32(5), call.u32(tail), call.u32(tail + 1));
			GLint buffer = 0;
			glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &buffer);
			std::vector<uint8_t> bytes;
			const void *data = nullptr;
			if (buffer && !proxy) {
				GLint64 capacity = 0;
				glGetBufferParameteri64v(GL_PIXEL_UNPACK_BUFFER, GL_BUFFER_SIZE, &capacity);
				if (capacity < 0 || pointer > uint64_t(capacity) || size > uint64_t(capacity) - pointer)
					throw std::runtime_error("OpenGL texture offset exceeds its buffer");
				data = reinterpret_cast<const void *>(pointer);
			} else if (pointer && !proxy) { bytes = call.read(pointer, size); data = bytes.data(); }
			if (dimensions == 2) glTexImage2D(call.u32(0), call.i32(1), call.i32(2), call.i32(3), call.i32(4), call.i32(5), call.u32(6), call.u32(7), data);
			else glTexImage3D(call.u32(0), call.i32(1), call.i32(2), call.i32(3), call.i32(4), call.i32(5), call.i32(6), call.u32(7), call.u32(8), data);
		});
	add("glColorPointer", 4, [](ApiCall &call) {
		require_array_buffer(); glColorPointer(call.i32(0), call.u32(1), call.i32(2), reinterpret_cast<const void *>(call[3]));
	});
	add("glTexCoordPointer", 4, [](ApiCall &call) {
		require_array_buffer(); glTexCoordPointer(call.i32(0), call.u32(1), call.i32(2), reinterpret_cast<const void *>(call[3]));
	});
	add("glNormalPointer", 3, [](ApiCall &call) {
		require_array_buffer(); glNormalPointer(call.u32(0), call.i32(1), reinterpret_cast<const void *>(call[2]));
	});
	add("glVertexAttribPointer", 6, [](ApiCall &call) {
		require_array_buffer(); glVertexAttribPointer(call.u32(0), call.i32(1), call.u32(2), GLboolean(call.u32(3)), call.i32(4), reinterpret_cast<const void *>(call[5]));
	});
	add("glDrawElements", 4, [&s](ApiCall &call) {
		s.validate_draw_arrays(call);
		if (call.i32(1) < 0 || call.u32(1) > 16 * 1024 * 1024) throw std::runtime_error("OpenGL element count exceeds bounds");
		const auto type = call.u32(2);
		const size_t element = type == GL_UNSIGNED_BYTE ? 1 : type == GL_UNSIGNED_SHORT ? 2 : type == GL_UNSIGNED_INT ? 4 : 0;
		if (!element) throw std::runtime_error("unsupported OpenGL element type");
		GLint buffer = 0;
		glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &buffer);
		const bool queued = call.process.serving_queued();
		if (!buffer && queued)
			throw std::runtime_error("queued OpenGL indexed draw has no element buffer");
		std::vector<uint8_t> bytes;
		const void *indices = nullptr;
		const size_t size = size_t(call.u32(1)) * element;
		if (buffer) {
			GLint64 capacity = 0;
			glGetBufferParameteri64v(GL_ELEMENT_ARRAY_BUFFER, GL_BUFFER_SIZE, &capacity);
			if (capacity < 0 || call[3] > uint64_t(capacity) || size > uint64_t(capacity) - call[3])
				throw std::runtime_error("OpenGL element offset exceeds its buffer");
			indices = reinterpret_cast<const void *>(call[3]);
		} else if (size) { bytes = call.read(call[3], size); indices = bytes.data(); }
		glDrawElements(call.u32(0), call.i32(1), type, indices);
		if (queued) ++s.queued_draws;
	});
	add("glClear", 1, [](ApiCall &call) { glClear(call.u32(0)); });
	add("glClearColor", 4, [](ApiCall &call) { const auto f = floats(call); glClearColor(f[0], f[1], f[2], f[3]); });
	add("glColor4f", 4, [](ApiCall &call) { const auto f = floats(call); glColor4f(f[0], f[1], f[2], f[3]); });
	add("glVertex2f", 2, [](ApiCall &call) { const auto f = floats(call); glVertex2f(f[0], f[1]); });
	add("glVertex3f", 3, [](ApiCall &call) { const auto f = floats(call); glVertex3f(f[0], f[1], f[2]); });
	add("glBegin", 1, [](ApiCall &call) { glBegin(call.u32(0)); });
	add("glEnd", 0, [](ApiCall &) { glEnd(); });
	add("glViewport", 4, [](ApiCall &call) { glViewport(call.i32(0), call.i32(1), call.i32(2), call.i32(3)); });
	add("glGetError", 0, [](ApiCall &call) { call.ret(glGetError()); });
	add("glEnable", 1, [](ApiCall &call) { glEnable(call.u32(0)); });
	add("glDisable", 1, [](ApiCall &call) { glDisable(call.u32(0)); });
	add("glEnableClientState", 1, [](ApiCall &call) { glEnableClientState(call.u32(0)); });
	add("glDisableClientState", 1, [](ApiCall &call) { glDisableClientState(call.u32(0)); });
	add("glGenBuffers", 2, [](ApiCall &call) {
		if (call.i32(0) < 0 || call.u32(0) > 65536) throw std::runtime_error("OpenGL buffer count exceeds bounds");
		if (!call.u32(0)) { glGenBuffers(0, nullptr); return; }
		auto bytes = call.read(call[1], size_t(call.u32(0)) * sizeof(GLuint));
		call.write(call[1], bytes);
		std::vector<GLuint> buffers(call.u32(0));
		glGenBuffers(call.i32(0), buffers.data());
		call.write(call[1], {reinterpret_cast<const uint8_t *>(buffers.data()), buffers.size() * sizeof(GLuint)});
	});
	add("glDeleteBuffers", 2, [](ApiCall &call) {
		if (call.i32(0) < 0 || call.u32(0) > 65536) throw std::runtime_error("OpenGL buffer count exceeds bounds");
		if (!call.u32(0)) { glDeleteBuffers(0, nullptr); return; }
		const auto bytes = call.read(call[1], size_t(call.u32(0)) * sizeof(GLuint));
		std::vector<GLuint> buffers(call.u32(0));
		std::memcpy(buffers.data(), bytes.data(), bytes.size());
		glDeleteBuffers(call.i32(0), buffers.data());
	});
	add("glBindBuffer", 2, [](ApiCall &call) { glBindBuffer(call.u32(0), call.u32(1)); });
	add("glBufferData", 4, [](ApiCall &call) {
		if (call[1] > (256ULL << 20)) throw std::runtime_error("OpenGL buffer upload exceeds bounds");
		const auto bytes = call[2] && call[1] ? call.read(call[2], size_t(call[1])) : std::vector<uint8_t>();
		glBufferData(call.u32(0), GLsizeiptr(call[1]), bytes.empty() ? nullptr : bytes.data(), call.u32(3));
	});
	add("glVertexPointer", 4, [](ApiCall &call) {
		require_array_buffer();
		glVertexPointer(call.i32(0), call.u32(1), call.i32(2), reinterpret_cast<const void *>(call[3]));
	});
	add("glInterleavedArrays", 3, [](ApiCall &call) {
		require_array_buffer();
		glInterleavedArrays(call.u32(0), call.i32(1), reinterpret_cast<const void *>(call[2]));
	});
	add("glDrawArrays", 3, [&s](ApiCall &call) {
		s.validate_draw_arrays(call);
		glDrawArrays(call.u32(0), call.i32(1), call.i32(2));
	});
	add("glMatrixMode", 1, [](ApiCall &call) { glMatrixMode(call.u32(0)); });
	add("glLoadIdentity", 0, [](ApiCall &) { glLoadIdentity(); });
	add("glGenLists", 1, [](ApiCall &call) { call.ret(glGenLists(call.i32(0))); });
	add("glNewList", 2, [](ApiCall &call) { glNewList(call.u32(0), call.u32(1)); });
	add("glEndList", 0, [](ApiCall &) { glEndList(); });
	add("glCallList", 1, [](ApiCall &call) { glCallList(call.u32(0)); });
	add("glDeleteLists", 2, [](ApiCall &call) { glDeleteLists(call.u32(0), call.i32(1)); });
	add("glGetString", 1, [&s](ApiCall &call) {
		const auto *text = glGetString(call.u32(0));
		call.ret(text ? s.string(reinterpret_cast<const char *>(text)) : 0);
	});
	add("glReadPixels", 7, [](ApiCall &call) {
		// Other pixel layouts need their own bounded row/skip calculation before reaching the driver.
		if (call.u32(4) != GL_RGBA || call.u32(5) != GL_UNSIGNED_BYTE || call.i32(2) < 0 || call.i32(3) < 0 ||
			call.u32(2) > 8192 || call.u32(3) > 8192) throw std::runtime_error("unsupported OpenGL readback layout");
		GLint alignment = 0, row = 0, skip_rows = 0, skip_pixels = 0, buffer = 0;
		glGetIntegerv(GL_PACK_ALIGNMENT, &alignment); glGetIntegerv(GL_PACK_ROW_LENGTH, &row);
		glGetIntegerv(GL_PACK_SKIP_ROWS, &skip_rows); glGetIntegerv(GL_PACK_SKIP_PIXELS, &skip_pixels);
		glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &buffer);
		if (buffer || row || skip_rows || skip_pixels || alignment != 4) throw std::runtime_error("unsupported OpenGL pack state");
		const size_t size = size_t(call.u32(2)) * call.u32(3) * 4;
		if (!size) { glReadPixels(call.i32(0), call.i32(1), call.i32(2), call.i32(3), GL_RGBA, GL_UNSIGNED_BYTE, nullptr); return; }
		auto bytes = call.read(call[6], size);
		call.write(call[6], bytes);
		glReadPixels(call.i32(0), call.i32(1), call.i32(2), call.i32(3), GL_RGBA, GL_UNSIGNED_BYTE, bytes.data());
		call.write(call[6], bytes);
	});
}

}
