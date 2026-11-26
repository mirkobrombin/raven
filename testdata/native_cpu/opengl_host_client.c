#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <GL/glu.h>
#include <stddef.h>
#include <stdint.h>

typedef void (WINAPI *gen_buffers)(GLsizei, GLuint *);
typedef void (WINAPI *bind_buffer)(GLenum, GLuint);
typedef void (WINAPI *buffer_data)(GLenum, ptrdiff_t, const void *, GLenum);
typedef void (WINAPI *delete_buffers)(GLsizei, const GLuint *);

static HDC device;
static HGLRC owned;
static HGLRC transferred;

static int option_requested(const char *wanted)
{
	for (const char *line = GetCommandLineA(); *line; ++line) {
		const char *cursor = line, *option = wanted;
		while (*option && *cursor == *option) { ++cursor; ++option; }
		if (!*option) return 1;
	}
	return 0;
}

static volatile uint64_t *queue_block(void)
{
	uintptr_t address;
	__asm__ volatile("movq %%gs:0x1ff0, %0" : "=r"(address));
	return (volatile uint64_t *)address;
}

static DWORD WINAPI other_thread(void *unused)
{
	(void)unused;
	if (wglGetCurrentContext() || wglGetCurrentDC() || !wglMakeCurrent(NULL, NULL)) return 20;
	if (wglMakeCurrent(device, owned) || GetLastError() != ERROR_BUSY) return 21;
	HGLRC context = wglCreateContext(device);
	if (!context || !wglMakeCurrent(device, context) || wglGetCurrentContext() != context) return 22;
	glClearColor(0, 0, 1, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glColor4f(.125f, .25f, .5f, 1);
	GLfloat colour[4];
	glGetFloatv(GL_CURRENT_COLOR, colour);
	if (colour[0] != .125f || colour[1] != .25f || colour[2] != .5f || colour[3] != 1) return 52;
	SetLastError(0);
	glDrawElements(GL_POINTS, 1, GL_UNSIGNED_BYTE, (void *)1);
	if (GetLastError() != ERROR_NOACCESS || glGetError()) return 51;
	if (option_requested("--queued-elements")) {
		gen_buffers generate = (gen_buffers)wglGetProcAddress("glGenBuffers");
		bind_buffer bind = (bind_buffer)wglGetProcAddress("glBindBuffer");
		buffer_data upload = (buffer_data)wglGetProcAddress("glBufferData");
		delete_buffers release = (delete_buffers)wglGetProcAddress("glDeleteBuffers");
		volatile uint64_t *queue = queue_block();
		if (!generate || !bind || !upload || !release || !queue || !queue[18] || queue[19]) return 83;
		GLuint buffers[2];
		generate(2, buffers);
		const float quad[] = {-1, -1, 1, -1, 1, 1, -1, 1};
		const unsigned char indices[] = {0, 1, 2, 3};
		glViewport(0, 0, 32, 32);
		glClearColor(1, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glColor4f(0, 0, 1, 1);
		bind(0x8892, buffers[0]);
		upload(0x8892, sizeof(quad), quad, 0x88e4);
		glEnableClientState(GL_VERTEX_ARRAY);
		glVertexPointer(2, GL_FLOAT, 0, NULL);
		bind(0x8893, buffers[1]);
		upload(0x8893, sizeof(indices), indices, 0x88e4);
		if (queue[19] != 1) return 84;
		const uint64_t head = queue[16];
		glDrawElements(GL_QUADS, 4, GL_UNSIGNED_BYTE, NULL);
		if (queue[16] != head + 1) return 85;
		unsigned char bytes[4];
		glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, bytes);
		if (glGetError() || bytes[0] || bytes[1] || bytes[2] != 255 || bytes[3] != 255) return 86;
		release(2, buffers);
		if (queue[19]) return 87;
		glDisableClientState(GL_VERTEX_ARRAY);
		glClearColor(0, 0, 1, 1);
	}
	transferred = context;
	return 0;
}

static int pixel(int x, int y, unsigned char r, unsigned char g, unsigned char b)
{
	unsigned char bytes[6] = {0x7b, 0, 0, 0, 0, 0x7b};
	glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, bytes + 1);
	const GLenum error = glGetError();
	const int matches = !error && bytes[0] == 0x7b && bytes[5] == 0x7b &&
		bytes[1] == r && bytes[2] == g && bytes[3] == b && bytes[4] == 255;
	if (!matches) {
		char message[] = "OpenGL pixel mismatch: error 0000 RGBA 00000000";
		const char hex[] = "0123456789abcdef";
		const unsigned error_at = sizeof("OpenGL pixel mismatch: error ") - 1;
		const unsigned color_at = sizeof("OpenGL pixel mismatch: error 0000 RGBA ") - 1;
		for (unsigned index = 0; index < 4; ++index) {
			message[error_at + index] = hex[(error >> ((3 - index) * 4)) & 15];
			message[color_at + index * 2] = hex[bytes[index + 1] >> 4];
			message[color_at + index * 2 + 1] = hex[bytes[index + 1] & 15];
		}
		OutputDebugStringA(message);
	}
	return matches;
}

// Vertices a game keeps in its own memory, as Windows draws them: a draw reads only the vertices it uses, from where the arrays point
// at the time of the draw.
static int client_arrays(void)
{
	// Two quads, and the draw takes the second, so the copy must begin where the draw does.
	const float positions[] = {9, 9, 9, 9, 9, 9, 9, 9, -1, -1, 1, -1, 1, 1, -1, 1};
	unsigned char colours[32] = {0};
	for (unsigned index = 16; index < 32; index += 4) {
		colours[index] = 255; colours[index + 2] = 255; colours[index + 3] = 255;
	}
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);
	glVertexPointer(2, GL_FLOAT, 0, positions);
	glColorPointer(4, GL_UNSIGNED_BYTE, 0, colours);
	glDrawArrays(GL_QUADS, 4, 4);
	if (glGetError() || !pixel(16, 16, 255, 0, 255)) return 91;
	// Interleaved vertices, drawn through indices that are in the game's memory as well.
	struct vertex { float x, y; unsigned char r, g, b, a; } vertices[8];
	for (unsigned index = 0; index < 8; ++index) {
		vertices[index].x = index < 4 ? 9 : positions[index * 2];
		vertices[index].y = index < 4 ? 9 : positions[index * 2 + 1];
		vertices[index].r = 0; vertices[index].g = 255; vertices[index].b = 255; vertices[index].a = 255;
	}
	const unsigned short indices[] = {4, 5, 6, 7};
	glVertexPointer(2, GL_FLOAT, sizeof(struct vertex), &vertices[0].x);
	glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(struct vertex), &vertices[0].r);
	glClear(GL_COLOR_BUFFER_BIT);
	glDrawElements(GL_QUADS, 4, GL_UNSIGNED_SHORT, indices);
	if (glGetError() || !pixel(16, 16, 0, 255, 255)) return 92;
	// What the game writes between draws is what the next draw shows.
	for (unsigned index = 4; index < 8; ++index) { vertices[index].r = 255; vertices[index].b = 0; }
	glClear(GL_COLOR_BUFFER_BIT);
	glDrawElements(GL_QUADS, 4, GL_UNSIGNED_SHORT, indices);
	if (glGetError() || !pixel(16, 16, 255, 255, 0)) return 93;
	// An array at memory the game cannot read fails the draw as any unreadable pointer does, and nothing is drawn.
	glVertexPointer(2, GL_FLOAT, 0, (void *)8);
	glClear(GL_COLOR_BUFFER_BIT);
	SetLastError(0);
	glDrawArrays(GL_QUADS, 0, 4);
	if (GetLastError() != ERROR_NOACCESS || glGetError() || !pixel(16, 16, 0, 0, 0)) return 94;
	glDisableClientState(GL_COLOR_ARRAY);
	glDisableClientState(GL_VERTEX_ARRAY);
	return 0;
}

static int game_features(void)
{
#define GL_PROC(result, name, arguments) result (WINAPI *name) arguments = (void *)wglGetProcAddress(#name); if (!name) return 35
	GL_PROC(GLuint, glCreateShader, (GLenum));
	GL_PROC(void, glShaderSource, (GLuint, GLsizei, const char *const *, const GLint *));
	GL_PROC(void, glCompileShader, (GLuint));
	GL_PROC(void, glGetShaderiv, (GLuint, GLenum, GLint *));
	GL_PROC(GLuint, glCreateProgram, (void));
	GL_PROC(void, glAttachShader, (GLuint, GLuint));
	GL_PROC(void, glLinkProgram, (GLuint));
	GL_PROC(void, glGetProgramiv, (GLuint, GLenum, GLint *));
	GL_PROC(void, glUseProgram, (GLuint));
	GL_PROC(GLint, glGetUniformLocation, (GLuint, const char *));
	GL_PROC(void, glUniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat));
	GL_PROC(void, glUniform3f, (GLint, GLfloat, GLfloat, GLfloat));
	GL_PROC(void, glUniform1f, (GLint, GLfloat));
	GL_PROC(void, glUniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat *));
	GL_PROC(void, glDeleteShader, (GLuint));
	GL_PROC(void, glDeleteProgram, (GLuint));
#undef GL_PROC
	const char *vertex_source[] = {"#version 120\nuniform mat4 transform; void main() { gl_Position = transform * gl_Vertex; gl_TexCoord[0] = gl_MultiTexCoord0; }"};
	const char *fragment_source[] = {"#version 120\nuniform vec4 tint; uniform vec3 adjust; uniform float brightness; uniform sampler2D image; void main() { gl_FragColor = texture2D(image, gl_TexCoord[0].xy) * tint * vec4(adjust * brightness, 1); }"};
	GLuint vertex = glCreateShader(0x8b31), fragment = glCreateShader(0x8b30), program = glCreateProgram();
	if (!vertex || !fragment || !program) return 36;
	GLint length = sizeof("#version 120\nuniform mat4 transform; void main() { gl_Position = transform * gl_Vertex; gl_TexCoord[0] = gl_MultiTexCoord0; }") - 1;
	glShaderSource(vertex, 1, vertex_source, &length);
	glShaderSource(fragment, 1, fragment_source, NULL);
	glCompileShader(vertex); glCompileShader(fragment);
	GLint status[3] = {123, 0, 456};
	glGetShaderiv(vertex, 0x8b81, status + 1);
	if (status[0] != 123 || status[1] != 1 || status[2] != 456) return 37;
	glGetShaderiv(fragment, 0x8b81, status + 1);
	if (status[1] != 1) return 38;
	glAttachShader(program, vertex); glAttachShader(program, fragment); glLinkProgram(program);
	glGetProgramiv(program, 0x8b82, status + 1);
	if (status[0] != 123 || status[1] != 1 || status[2] != 456) return 39;
	glUseProgram(program);
	GLint tint = glGetUniformLocation(program, "tint"), transform = glGetUniformLocation(program, "transform");
	if (tint < 0 || transform < 0) return 40;
	glUniform4f(tint, .25f, .5f, .75f, 1);
	GLint adjust = glGetUniformLocation(program, "adjust"), brightness = glGetUniformLocation(program, "brightness");
	if (adjust < 0 || brightness < 0) return 53;
	glUniform3f(adjust, .5f, 1, .25f);
	glUniform1f(brightness, 2);
	const GLfloat identity[] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
	glUniformMatrix4fv(transform, 1, GL_FALSE, identity);
	GLuint texture = 0;
	glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 8); glPixelStorei(GL_UNPACK_ROW_LENGTH, 3);
	glPixelStorei(GL_UNPACK_SKIP_ROWS, 1); glPixelStorei(GL_UNPACK_SKIP_PIXELS, 1);
	const unsigned char padded[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 255, 255, 255};
	SetLastError(0);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, (void *)1);
	if (GetLastError() != ERROR_NOACCESS || glGetError()) return 41;
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, padded);
	if (gluBuild2DMipmaps(GL_TEXTURE_2D, GL_RGBA, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, padded)) return 44;
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, padded);
	HGLRC shared = wglCreateContext(device);
	if (!shared || !wglShareLists(owned, shared) || !wglMakeCurrent(device, shared) || !glIsTexture(texture)) return 48;
	if (wglShareLists(owned, shared) || !wglMakeCurrent(device, owned) || !wglDeleteContext(shared)) return 49;
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4); glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glPixelStorei(GL_UNPACK_SKIP_ROWS, 0); glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
	glBegin(GL_QUADS);
	glTexCoord2f(0, 0); glVertex2f(-1, -1);
	glTexCoord2f(1, 0); glVertex2f(1, -1);
	glTexCoord2f(1, 1); glVertex2f(1, 1);
	glTexCoord2f(0, 1); glVertex2f(-1, 1);
	glEnd();
	if (!pixel(1, 1, 64, 255, 96)) return 42;
	glUniform3f(adjust, 1, 1, 1);
	glUniform1f(brightness, 1);
	glBegin(GL_QUADS);
	glTexCoord2f(0, 0); glVertex2f(-1, -1);
	glTexCoord2f(1, 0); glVertex2f(1, -1);
	glTexCoord2f(1, 1); glVertex2f(1, 1);
	glTexCoord2f(0, 1); glVertex2f(-1, 1);
	glEnd();
	if (!pixel(1, 1, 64, 128, 191)) return 54;
	glUseProgram(0); glDeleteShader(vertex); glDeleteShader(fragment); glDeleteProgram(program); glDeleteTextures(1, &texture);
	glMatrixMode(GL_PROJECTION); glLoadIdentity();
	glOrtho(-2, 2, -4, 4, -8, 8);
	GLdouble matrix[18];
	matrix[0] = 123; matrix[17] = 456;
	glGetDoublev(GL_PROJECTION_MATRIX, matrix + 1);
	if (glGetError() || matrix[0] != 123 || matrix[17] != 456 || matrix[1] != .5 || matrix[6] != .25 || matrix[11] != -.125 || matrix[16] != 1) return 43;
	const GLdouble unity[] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
	const GLint viewport[] = {0, 0, 32, 32};
	GLdouble coordinates[] = {123, 0, 0, 0, 456};
	if (!gluUnProject(8, 16, .25, unity, unity, viewport, coordinates + 1, coordinates + 2, coordinates + 3) ||
		coordinates[0] != 123 || coordinates[1] != -.5 || coordinates[2] != 0 || coordinates[3] != -.5 || coordinates[4] != 456) return 45;
	SetLastError(0);
	if (gluUnProject(8, 16, .25, unity, unity, viewport, coordinates + 1, (void *)1, coordinates + 3) ||
		GetLastError() != ERROR_NOACCESS || coordinates[1] != -.5 || coordinates[3] != -.5) return 46;
	glLoadIdentity(); glMatrixMode(GL_MODELVIEW);
	return 0;
}

int raven_entry(void)
{
	if (wglGetCurrentContext() || wglGetCurrentDC() || !wglMakeCurrent(NULL, NULL) ||
		wglGetProcAddress("glGenBuffers")) return 1;
	{
		HWND hidden = CreateWindowExA(0, "STATIC", "OpenGL bootstrap", 0, 0, 0, 16, 16, NULL, NULL, NULL, NULL);
		HDC dc = GetDC(hidden);
		PIXELFORMATDESCRIPTOR format = {0};
		format.nSize = sizeof(format); format.nVersion = 1;
		format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
		if (!hidden || !dc || !SetPixelFormat(dc, ChoosePixelFormat(dc, &format), &format)) return 24;
		HGLRC context = wglCreateContext(dc);
		if (!context || !wglMakeCurrent(dc, context) || !glGetString(GL_VERSION) ||
			!wglMakeCurrent(NULL, NULL) || !wglDeleteContext(context)) return 25;
		ReleaseDC(hidden, dc);
		DestroyWindow(hidden);
	}
	HWND window = CreateWindowExA(0, "STATIC", "Native OpenGL pixels", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
		0, 0, 128, 128, NULL, NULL, NULL, NULL);
	if (!window) return 2;
	device = GetDC(window);
	PIXELFORMATDESCRIPTOR format = {0};
	format.nSize = sizeof(format); format.nVersion = 1;
	format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
	format.iPixelType = PFD_TYPE_RGBA; format.cColorBits = 32; format.cDepthBits = 24;
	const int index = ChoosePixelFormat(device, &format);
	if (!index || !DescribePixelFormat(device, index, sizeof(format), &format) || format.nSize != sizeof(format) ||
		format.cRedBits < 8 || format.cDepthBits < 24 || GetPixelFormat(device) ||
		!SetPixelFormat(device, index, &format) || GetPixelFormat(device) != index) return 3;
	if (SetPixelFormat(device, index, &format) || GetPixelFormat(device) != index) return 4;
	owned = wglCreateContext(device);
	if (!owned || !wglMakeCurrent(device, owned) || wglGetCurrentContext() != owned || wglGetCurrentDC() != device) return 5;
	if (wglGetProcAddress("glRavenMissing")) return 6;
	OutputDebugStringA((const char *)glGetString(GL_RENDERER));
	gen_buffers generate = (gen_buffers)wglGetProcAddress("glGenBuffers");
	bind_buffer bind = (bind_buffer)wglGetProcAddress("glBindBuffer");
	buffer_data upload = (buffer_data)wglGetProcAddress("glBufferData");
	delete_buffers release = (delete_buffers)wglGetProcAddress("glDeleteBuffers");
	if (!generate || !bind || !upload || !release) return 7;
	glViewport(0, 0, 32, 32);
	glClearColor(.25f, .5f, .75f, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	if (!pixel(1, 1, 64, 128, 191)) return 8;
	{
		DWORD previous;
		void *readonly = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
		if (!readonly || !VirtualProtect(readonly, 4096, PAGE_READONLY, &previous)) return 26;
		SetLastError(0);
		glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, readonly);
		if (GetLastError() != ERROR_NOACCESS || glGetError()) return 27;
		VirtualFree(readonly, 0, MEM_RELEASE);
		SetLastError(0);
		glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, (void *)1);
		if (GetLastError() != ERROR_NOACCESS || glGetError() || !pixel(1, 1, 64, 128, 191)) return 28;
		SetLastError(0);
		generate(1, (GLuint *)1);
		if (GetLastError() != ERROR_NOACCESS || glGetError()) return 29;
	}
	HANDLE thread = CreateThread(NULL, 0, other_thread, NULL, 0, NULL);
	DWORD result = 0;
	if (!thread || WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0 || !GetExitCodeThread(thread, &result) || result) return 9;
	CloseHandle(thread);
	if (!transferred || !wglMakeCurrent(device, transferred)) return 30;
	glClear(GL_COLOR_BUFFER_BIT);
	if (!pixel(1, 1, 0, 0, 255) || !wglMakeCurrent(device, owned) || !wglDeleteContext(transferred)) return 31;
	glClear(GL_COLOR_BUFFER_BIT);
	if (wglGetCurrentContext() != owned || !pixel(1, 1, 64, 128, 191)) return 10;
	GLuint invalid_vao = 0;
	HGLRC invalid_context = NULL;
	void (WINAPI *bind_vao)(GLuint) = NULL;
	if (option_requested("--array-vao")) {
		gen_buffers generate_vaos = (gen_buffers)wglGetProcAddress("glGenVertexArrays");
		bind_vao = (void *)wglGetProcAddress("glBindVertexArray");
		if (!generate_vaos || !bind_vao) return 64;
		generate_vaos(1, &invalid_vao);
		bind_vao(invalid_vao);
		glEnableClientState(GL_COLOR_ARRAY);
		bind_vao(0);
	} else if (option_requested("--array-context")) {
		invalid_context = wglCreateContext(device);
		if (!invalid_context || !wglMakeCurrent(device, invalid_context)) return 65;
		glEnableClientState(GL_COLOR_ARRAY);
		if (!wglMakeCurrent(device, owned)) return 66;
	}
	GLuint buffer = 0;
	generate(1, &buffer);
	if (!buffer) return 11;
	bind(0x8892, buffer);
	glVertexPointer(2, GL_FLOAT, 0, (void *)8);
	if (glGetError()) return 47;
	const float quad[] = {-1, -1, 1, -1, 1, 1, -1, 1};
	upload(0x8892, sizeof(quad), quad, 0x88e4);
	glEnableClientState(GL_VERTEX_ARRAY);
	glVertexPointer(2, GL_FLOAT, 0, NULL);
	glColor4f(0, 1, 0, 1);
	glDrawArrays(GL_QUADS, 0, 4);
	const unsigned char indices[] = {0, 1, 2, 3};
	volatile uint64_t *queue = option_requested("--queued-elements") ? queue_block() : NULL;
	if (option_requested("--queued-elements") && (!queue || !queue[18])) return 68;
	uint64_t head = queue ? queue[16] : 0;
	glDrawElements(GL_QUADS, 4, GL_UNSIGNED_BYTE, indices);
	if (queue && (queue[16] != head || queue[19])) return 69;
	GLuint element_buffer = 0;
	generate(1, &element_buffer);
	bind(0x8893, element_buffer);
	upload(0x8893, sizeof(indices), indices, 0x88e4);
	if (option_requested("--bad-element-offset") || option_requested("--bad-element-count")) {
		glDrawElements(GL_QUADS, option_requested("--bad-element-count") ? -1 : 4, GL_UNSIGNED_BYTE,
			option_requested("--bad-element-offset") ? (void *)(uintptr_t)-1 : NULL);
		glGetError();
		return 88;
	}
	if (queue) {
		head = queue[16];
		bind(0x8893, 0);
		if (queue[16] != head || queue[19]) return 70;
		bind(0x8893, element_buffer);
		if (queue[16] != head || queue[19] != 1) return 71;
		HGLRC other = wglCreateContext(device);
		if (!other || !wglMakeCurrent(device, other) || queue[19]) return 72;
		if (!wglMakeCurrent(device, owned) || queue[19] != 1 || !wglDeleteContext(other) || queue[19] != 1) return 73;
		gen_buffers generate_vaos = (gen_buffers)wglGetProcAddress("glGenVertexArrays");
		void (WINAPI *bind_array)(GLuint) = (void *)wglGetProcAddress("glBindVertexArray");
		GLuint empty = 0;
		if (!generate_vaos || !bind_array) return 74;
		generate_vaos(1, &empty);
		bind_array(empty);
		if (queue[19]) return 75;
		bind_array(0);
		if (queue[19] != 1) return 76;
		glBegin(GL_POINTS);
		if (!wglMakeCurrent(device, owned)) return 89;
		glVertex3f(0, 0, 0);
		glEnd();
		if (glGetError() || queue[19] != 1) return 90;
		head = queue[16];
	}
	glDrawElements(GL_QUADS, 4, GL_UNSIGNED_BYTE, NULL);
	if (queue && queue[16] != head + 1) return 77;
	if (queue) {
		if (glGetError()) return 78;
		queue[18] = 0;
		head = queue[16];
		glDrawElements(GL_QUADS, 4, GL_UNSIGNED_BYTE, NULL);
		if (queue[16] != head || glGetError()) return 79;
		queue[18] = 1;
	}
	release(1, &element_buffer);
	if (queue && queue[19]) return 80;
	SetLastError(0);
	glDrawElements(GL_QUADS, 4, GL_UNSIGNED_BYTE, (void *)1);
	if (GetLastError() != ERROR_NOACCESS || glGetError()) return 50;
	if (option_requested("--forged-elements")) {
		volatile uint64_t *forged = queue_block();
		if (!forged || !forged[18]) return 81;
		forged[19] = 1;
		glDrawElements(GL_QUADS, 4, GL_UNSIGNED_BYTE, indices);
		glGetError();
		return 82;
	}
	if (option_requested("--array-color")) glEnableClientState(GL_COLOR_ARRAY);
	else if (option_requested("--array-texture")) {
		void (WINAPI *active)(GLenum) = (void *)wglGetProcAddress("glClientActiveTexture");
		if (!active) return 61;
		active(0x84c1);
		glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	} else if (option_requested("--array-delete")) release(1, &buffer);
	else if (invalid_vao) bind_vao(invalid_vao);
	else if (invalid_context) {
		if (!wglMakeCurrent(device, invalid_context)) return 67;
	} else if (option_requested("--array-attribute")) {
		void (WINAPI *enable)(GLuint) = (void *)wglGetProcAddress("glEnableVertexAttribArray");
		if (!enable) return 62;
		enable(0);
	}
	// Each of these leaves an array enabled at an address the game never filled: the draw fails as for any unreadable pointer.
	if (option_requested("--array-")) {
		SetLastError(0);
		glDrawArrays(GL_QUADS, 0, 4);
		return GetLastError() == ERROR_NOACCESS && !glGetError() ? 95 : 63;
	}
	glDisableClientState(GL_VERTEX_ARRAY);
	if (!pixel(16, 16, 0, 255, 0)) return 12;
	const GLuint list = glGenLists(1);
	if (!list) return 13;
	glNewList(list, GL_COMPILE);
	glBegin(GL_TRIANGLES);
	glColor4f(1, 0, 0, 1);
	glVertex3f(-1, -1, 0); glVertex3f(1, -1, 0); glVertex3f(-1, 1, 0);
	glEnd();
	glEndList();
	glCallList(list);
	if (!pixel(1, 1, 255, 0, 0) || !pixel(30, 30, 0, 255, 0)) return 14;
	glDeleteLists(list, 1);
	release(1, &buffer);
	const int client = client_arrays();
	if (client) return client;
	const int features = game_features();
	if (features) return features;
	glClearColor(1, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	ShowWindow(window, SW_HIDE);
	ShowWindow(window, SW_SHOW);
	glClear(GL_COLOR_BUFFER_BIT);
	if (wglGetCurrentContext() != owned) return 32;
	if (!pixel(1, 1, 255, 0, 0)) return 34;
	if (option_requested("--maximize")) {
		GLuint texture;
		glGenTextures(1, &texture);
		glBindTexture(GL_TEXTURE_2D, texture);
		glBindTexture(GL_TEXTURE_2D, 0);
		if (!SwapBuffers(device)) return 55;
		Sleep(200);
		RECT original;
		if (!GetWindowRect(window, &original)) return 57;
		const unsigned commands[] = {SW_MAXIMIZE, SW_RESTORE, SW_MAXIMIZE};
		for (unsigned step = 0; step < 3; ++step) {
			ShowWindow(window, commands[step]);
			MSG message;
			while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
			if (!glIsTexture(texture)) return 60;
			if (step == 1) {
				RECT restored;
				if (!GetWindowRect(window, &restored) || restored.left != original.left || restored.top != original.top ||
					restored.right != original.right || restored.bottom != original.bottom || wglGetCurrentContext() != owned) return 58;
				glClear(GL_COLOR_BUFFER_BIT);
				if (!pixel(1, 1, 255, 0, 0)) return 59;
			}
		}
		RECT client;
		if (!GetClientRect(window, &client) || client.right < 32 || client.bottom < 32 || wglGetCurrentContext() != owned) return 56;
		glViewport(0, 0, client.right, client.bottom);
		glClear(GL_COLOR_BUFFER_BIT);
		glClearColor(0, 1, 0, 1);
		glEnable(GL_SCISSOR_TEST);
		for (unsigned corner = 0; corner < 4; ++corner) {
			glScissor(corner & 1 ? client.right - 16 : 0, corner & 2 ? client.bottom - 16 : 0, 16, 16);
			glClear(GL_COLOR_BUFFER_BIT);
		}
		glDisable(GL_SCISSOR_TEST);
		glDeleteTextures(1, &texture);
	}
	if (!SwapBuffers(device)) return 15;
	OutputDebugStringA("OpenGL frame presented");
	Sleep(3000);
	if (!DestroyWindow(window)) return 33;
	if (!wglMakeCurrent(NULL, NULL) || !wglDeleteContext(owned) || wglGetCurrentContext() || wglGetCurrentDC()) return 16;
	return 0;
}
