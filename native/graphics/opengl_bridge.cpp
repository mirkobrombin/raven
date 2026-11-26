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
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <map>
#include <span>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <sys/mman.h>
#include <unistd.h>

namespace raven {

namespace {

// Calls whose arguments are all values, passed to the host function as they are.
#define RAVEN_GL_SCALARS(X) \
	X(glAccum) X(glBeginConditionalRender) X(glBeginQuery) X(glBeginTransformFeedback) X(glBindBufferBase) X(glBindBufferRange) \
	X(glBindSampler) X(glBlendColor) X(glBlendEquation) X(glBlendEquationSeparate) X(glBlendFuncSeparate) X(glClearAccum) \
	X(glClearBufferfi) X(glClearIndex) X(glClearStencil) X(glColor3b) X(glColor3d) X(glColor3i) X(glColor3s) X(glColor3ub) \
	X(glColor3ui) X(glColor3us) X(glColor4b) X(glColor4d) X(glColor4i) X(glColor4s) X(glColor4ui) X(glColor4us) X(glColorMaski) \
	X(glColorP3ui) X(glColorP4ui) X(glCopyBufferSubData) X(glCopyPixels) X(glCopyTexImage1D) X(glCopyTexImage2D) \
	X(glCopyTexSubImage1D) X(glCopyTexSubImage3D) X(glDepthRange) X(glDetachShader) X(glDisablei) X(glEdgeFlag) X(glEnablei) \
	X(glEndConditionalRender) X(glEndQuery) X(glEndTransformFeedback) X(glEvalCoord1d) X(glEvalCoord1f) X(glEvalCoord2d) \
	X(glEvalCoord2f) X(glEvalMesh1) X(glEvalMesh2) X(glEvalPoint1) X(glEvalPoint2) X(glFogCoordd) X(glFogCoordf) X(glFogf) \
	X(glFogi) X(glFramebufferTexture) X(glFramebufferTexture1D) X(glFramebufferTexture3D) X(glFramebufferTextureLayer) \
	X(glGenerateMipmap) X(glIndexMask) X(glIndexd) X(glIndexf) X(glIndexi) X(glIndexs) X(glIndexub) X(glInitNames) X(glIsBuffer) \
	X(glIsEnabled) X(glIsEnabledi) X(glIsFramebuffer) X(glIsList) X(glIsProgram) X(glIsQuery) X(glIsRenderbuffer) X(glIsSampler) \
	X(glIsShader) X(glIsVertexArray) X(glLightModelf) X(glLightModeli) X(glLightf) X(glLighti) X(glListBase) X(glLoadName) \
	X(glLogicOp) X(glMapGrid1d) X(glMapGrid1f) X(glMapGrid2d) X(glMapGrid2f) X(glMaterialf) X(glMateriali) X(glMultiTexCoord1d) \
	X(glMultiTexCoord1f) X(glMultiTexCoord1i) X(glMultiTexCoord1s) X(glMultiTexCoord2d) X(glMultiTexCoord2f) X(glMultiTexCoord2i) \
	X(glMultiTexCoord2s) X(glMultiTexCoord3d) X(glMultiTexCoord3f) X(glMultiTexCoord3i) X(glMultiTexCoord3s) X(glMultiTexCoord4d) \
	X(glMultiTexCoord4f) X(glMultiTexCoord4i) X(glMultiTexCoord4s) X(glMultiTexCoordP1ui) X(glMultiTexCoordP2ui) \
	X(glMultiTexCoordP3ui) X(glMultiTexCoordP4ui) X(glNormal3b) X(glNormal3d) X(glNormal3f) X(glNormal3i) X(glNormal3s) \
	X(glNormalP3ui) X(glPassThrough) X(glPixelStoref) X(glPixelTransferf) X(glPixelTransferi) X(glPixelZoom) X(glPointParameterf) \
	X(glPointParameteri) X(glPolygonOffset) X(glPopAttrib) X(glPopClientAttrib) X(glPopName) X(glPrimitiveRestartIndex) \
	X(glProvokingVertex) X(glPushAttrib) X(glPushClientAttrib) X(glPushName) X(glQueryCounter) X(glRasterPos2d) X(glRasterPos2f) \
	X(glRasterPos2i) X(glRasterPos2s) X(glRasterPos3d) X(glRasterPos3f) X(glRasterPos3i) X(glRasterPos3s) X(glRasterPos4d) \
	X(glRasterPos4f) X(glRasterPos4i) X(glRasterPos4s) X(glRectf) X(glRecti) X(glRects) X(glRenderbufferStorage) X(glRotated) \
	X(glSampleCoverage) X(glSampleMaski) X(glSamplerParameterf) X(glSamplerParameteri) X(glScaled) X(glSecondaryColor3b) \
	X(glSecondaryColor3d) X(glSecondaryColor3f) X(glSecondaryColor3i) X(glSecondaryColor3s) X(glSecondaryColor3ub) \
	X(glSecondaryColor3ui) X(glSecondaryColor3us) X(glSecondaryColorP3ui) X(glStencilFuncSeparate) X(glStencilMaskSeparate) \
	X(glStencilOpSeparate) X(glTexBuffer) X(glTexCoord1d) X(glTexCoord1f) X(glTexCoord1i) X(glTexCoord1s) X(glTexCoord2d) \
	X(glTexCoord2i) X(glTexCoord2s) X(glTexCoord3d) X(glTexCoord3f) X(glTexCoord3i) X(glTexCoord3s) X(glTexCoord4d) \
	X(glTexCoord4f) X(glTexCoord4i) X(glTexCoord4s) X(glTexCoordP1ui) X(glTexCoordP2ui) X(glTexCoordP3ui) X(glTexCoordP4ui) \
	X(glTexEnvi) X(glTexGend) X(glTexGenf) X(glTexImage3DMultisample) X(glTranslated) X(glUniform1ui) X(glUniform2i) \
	X(glUniform2ui) X(glUniform3i) X(glUniform3ui) X(glUniform4i) X(glUniform4ui) X(glUniformBlockBinding) X(glValidateProgram) \
	X(glVertex2d) X(glVertex2i) X(glVertex2s) X(glVertex3d) X(glVertex3i) X(glVertex3s) X(glVertex4d) X(glVertex4f) X(glVertex4i) \
	X(glVertex4s) X(glVertexAttrib1d) X(glVertexAttrib1f) X(glVertexAttrib1s) X(glVertexAttrib2d) X(glVertexAttrib2f) \
	X(glVertexAttrib2s) X(glVertexAttrib3d) X(glVertexAttrib3f) X(glVertexAttrib3s) X(glVertexAttrib4Nub) X(glVertexAttrib4d) \
	X(glVertexAttrib4f) X(glVertexAttrib4s) X(glVertexAttribDivisor) X(glVertexAttribI1i) X(glVertexAttribI1ui) \
	X(glVertexAttribI2i) X(glVertexAttribI2ui) X(glVertexAttribI3i) X(glVertexAttribI3ui) X(glVertexAttribI4i) \
	X(glVertexAttribI4ui) X(glVertexAttribP1ui) X(glVertexAttribP2ui) X(glVertexAttribP3ui) X(glVertexAttribP4ui) X(glVertexP2ui) \
	X(glVertexP3ui) X(glVertexP4ui) X(glWindowPos2d) X(glWindowPos2f) X(glWindowPos2i) X(glWindowPos2s) X(glWindowPos3d) \
	X(glWindowPos3f) X(glWindowPos3i) X(glWindowPos3s)

// Calls that read a fixed number of values from their last argument.
#define RAVEN_GL_VECTORS(X) \
	X(glClipPlane, 4) X(glColor3bv, 3) X(glColor3dv, 3) X(glColor3fv, 3) X(glColor3iv, 3) X(glColor3sv, 3) X(glColor3ubv, 3) \
	X(glColor3uiv, 3) X(glColor3usv, 3) X(glColor4bv, 4) X(glColor4dv, 4) X(glColor4iv, 4) X(glColor4sv, 4) X(glColor4ubv, 4) \
	X(glColor4uiv, 4) X(glColor4usv, 4) X(glColorP3uiv, 1) X(glColorP4uiv, 1) X(glEdgeFlagv, 1) X(glEvalCoord1dv, 1) \
	X(glEvalCoord1fv, 1) X(glEvalCoord2dv, 2) X(glEvalCoord2fv, 2) X(glFogCoorddv, 1) X(glFogCoordfv, 1) X(glIndexdv, 1) \
	X(glIndexfv, 1) X(glIndexiv, 1) X(glIndexsv, 1) X(glIndexubv, 1) X(glLoadMatrixd, 16) X(glLoadMatrixf, 16) \
	X(glLoadTransposeMatrixd, 16) X(glLoadTransposeMatrixf, 16) X(glMultMatrixd, 16) X(glMultMatrixf, 16) \
	X(glMultTransposeMatrixd, 16) X(glMultTransposeMatrixf, 16) X(glMultiTexCoord1dv, 1) X(glMultiTexCoord1fv, 1) \
	X(glMultiTexCoord1iv, 1) X(glMultiTexCoord1sv, 1) X(glMultiTexCoord2dv, 2) X(glMultiTexCoord2fv, 2) X(glMultiTexCoord2iv, 2) \
	X(glMultiTexCoord2sv, 2) X(glMultiTexCoord3dv, 3) X(glMultiTexCoord3fv, 3) X(glMultiTexCoord3iv, 3) X(glMultiTexCoord3sv, 3) \
	X(glMultiTexCoord4dv, 4) X(glMultiTexCoord4fv, 4) X(glMultiTexCoord4iv, 4) X(glMultiTexCoord4sv, 4) \
	X(glMultiTexCoordP1uiv, 1) X(glMultiTexCoordP2uiv, 1) X(glMultiTexCoordP3uiv, 1) X(glMultiTexCoordP4uiv, 1) X(glNormal3bv, 3) \
	X(glNormal3dv, 3) X(glNormal3fv, 3) X(glNormal3iv, 3) X(glNormal3sv, 3) X(glNormalP3uiv, 1) X(glRasterPos2dv, 2) \
	X(glRasterPos2fv, 2) X(glRasterPos2iv, 2) X(glRasterPos2sv, 2) X(glRasterPos3dv, 3) X(glRasterPos3fv, 3) X(glRasterPos3iv, 3) \
	X(glRasterPos3sv, 3) X(glRasterPos4dv, 4) X(glRasterPos4fv, 4) X(glRasterPos4iv, 4) X(glRasterPos4sv, 4) \
	X(glSecondaryColor3bv, 3) X(glSecondaryColor3dv, 3) X(glSecondaryColor3fv, 3) X(glSecondaryColor3iv, 3) \
	X(glSecondaryColor3sv, 3) X(glSecondaryColor3ubv, 3) X(glSecondaryColor3uiv, 3) X(glSecondaryColor3usv, 3) \
	X(glSecondaryColorP3uiv, 1) X(glTexCoord1dv, 1) X(glTexCoord1fv, 1) X(glTexCoord1iv, 1) X(glTexCoord1sv, 1) \
	X(glTexCoord2dv, 2) X(glTexCoord2fv, 2) X(glTexCoord2iv, 2) X(glTexCoord2sv, 2) X(glTexCoord3dv, 3) X(glTexCoord3fv, 3) \
	X(glTexCoord3iv, 3) X(glTexCoord3sv, 3) X(glTexCoord4dv, 4) X(glTexCoord4fv, 4) X(glTexCoord4iv, 4) X(glTexCoord4sv, 4) \
	X(glTexCoordP1uiv, 1) X(glTexCoordP2uiv, 1) X(glTexCoordP3uiv, 1) X(glTexCoordP4uiv, 1) X(glVertex2dv, 2) X(glVertex2fv, 2) \
	X(glVertex2iv, 2) X(glVertex2sv, 2) X(glVertex3dv, 3) X(glVertex3fv, 3) X(glVertex3iv, 3) X(glVertex3sv, 3) X(glVertex4dv, 4) \
	X(glVertex4fv, 4) X(glVertex4iv, 4) X(glVertex4sv, 4) X(glVertexAttrib1dv, 1) X(glVertexAttrib1fv, 1) X(glVertexAttrib1sv, 1) \
	X(glVertexAttrib2dv, 2) X(glVertexAttrib2fv, 2) X(glVertexAttrib2sv, 2) X(glVertexAttrib3dv, 3) X(glVertexAttrib3fv, 3) \
	X(glVertexAttrib3sv, 3) X(glVertexAttrib4Nbv, 4) X(glVertexAttrib4Niv, 4) X(glVertexAttrib4Nsv, 4) X(glVertexAttrib4Nubv, 4) \
	X(glVertexAttrib4Nuiv, 4) X(glVertexAttrib4Nusv, 4) X(glVertexAttrib4bv, 4) X(glVertexAttrib4dv, 4) X(glVertexAttrib4fv, 4) \
	X(glVertexAttrib4iv, 4) X(glVertexAttrib4sv, 4) X(glVertexAttrib4ubv, 4) X(glVertexAttrib4uiv, 4) X(glVertexAttrib4usv, 4) \
	X(glVertexAttribI1iv, 1) X(glVertexAttribI1uiv, 1) X(glVertexAttribI2iv, 2) X(glVertexAttribI2uiv, 2) \
	X(glVertexAttribI3iv, 3) X(glVertexAttribI3uiv, 3) X(glVertexAttribI4bv, 4) X(glVertexAttribI4iv, 4) X(glVertexAttribI4sv, 4) \
	X(glVertexAttribI4ubv, 4) X(glVertexAttribI4uiv, 4) X(glVertexAttribI4usv, 4) X(glVertexAttribP1uiv, 1) \
	X(glVertexAttribP2uiv, 1) X(glVertexAttribP3uiv, 1) X(glVertexAttribP4uiv, 1) X(glVertexP2uiv, 1) X(glVertexP3uiv, 1) \
	X(glVertexP4uiv, 1) X(glWindowPos2dv, 2) X(glWindowPos2fv, 2) X(glWindowPos2iv, 2) X(glWindowPos2sv, 2) X(glWindowPos3dv, 3) \
	X(glWindowPos3fv, 3) X(glWindowPos3iv, 3) X(glWindowPos3sv, 3)

// Uniform calls that read count groups of values from their last argument.
#define RAVEN_GL_UNIFORMS(X) \
	X(glUniform1fv, 1) X(glUniform1iv, 1) X(glUniform1uiv, 1) X(glUniform2fv, 2) X(glUniform2iv, 2) X(glUniform2uiv, 2) \
	X(glUniform3fv, 3) X(glUniform3iv, 3) X(glUniform3uiv, 3) X(glUniform4fv, 4) X(glUniform4iv, 4) X(glUniform4uiv, 4) \
	X(glUniformMatrix2fv, 4) X(glUniformMatrix2x3fv, 6) X(glUniformMatrix2x4fv, 8) X(glUniformMatrix3fv, 9) \
	X(glUniformMatrix3x2fv, 6) X(glUniformMatrix3x4fv, 12) X(glUniformMatrix4x2fv, 8) X(glUniformMatrix4x3fv, 12)

// Calls with their own handler below: what they read and write depends on more than their arguments.
#define RAVEN_GL_HANDLED(X) \
	X(glAreTexturesResident) X(glArrayElement) X(glBindFragDataLocation) X(glBindFragDataLocationIndexed) X(glBitmap) \
	X(glBufferSubData) X(glCallLists) X(glClearBufferfv) X(glClearBufferiv) X(glClearBufferuiv) X(glClientWaitSync) \
	X(glCompressedTexImage1D) X(glCompressedTexImage2D) X(glCompressedTexImage3D) X(glCompressedTexSubImage1D) \
	X(glCompressedTexSubImage2D) X(glCompressedTexSubImage3D) X(glDeleteFramebuffers) X(glDeleteQueries) X(glDeleteRenderbuffers) \
	X(glDeleteSamplers) X(glDeleteSync) X(glDeleteVertexArrays) X(glDrawArraysInstanced) X(glDrawElementsBaseVertex) \
	X(glDrawElementsInstanced) X(glDrawElementsInstancedBaseVertex) X(glDrawPixels) X(glDrawRangeElements) \
	X(glDrawRangeElementsBaseVertex) X(glEdgeFlagPointer) X(glFeedbackBuffer) X(glFenceSync) X(glFlushMappedBufferRange) \
	X(glFogCoordPointer) X(glFogfv) X(glFogiv) X(glGenQueries) X(glGenSamplers) X(glGetActiveAttrib) X(glGetActiveUniform) \
	X(glGetActiveUniformBlockName) X(glGetActiveUniformBlockiv) X(glGetActiveUniformName) X(glGetActiveUniformsiv) \
	X(glGetAttachedShaders) X(glGetAttribLocation) X(glGetBooleani_v) X(glGetBooleanv) X(glGetBufferParameteri64v) \
	X(glGetBufferParameteriv) X(glGetBufferPointerv) X(glGetBufferSubData) X(glGetClipPlane) X(glGetCompressedTexImage) \
	X(glGetFragDataIndex) X(glGetFragDataLocation) X(glGetFramebufferAttachmentParameteriv) X(glGetInteger64i_v) \
	X(glGetInteger64v) X(glGetIntegeri_v) X(glGetLightfv) X(glGetLightiv) X(glGetMapdv) X(glGetMapfv) X(glGetMapiv) \
	X(glGetMaterialfv) X(glGetMaterialiv) X(glGetMultisamplefv) X(glGetPixelMapfv) X(glGetPixelMapuiv) X(glGetPixelMapusv) \
	X(glGetPointerv) X(glGetPolygonStipple) X(glGetQueryObjecti64v) X(glGetQueryObjectiv) X(glGetQueryObjectui64v) \
	X(glGetQueryObjectuiv) X(glGetQueryiv) X(glGetRenderbufferParameteriv) X(glGetSamplerParameterIiv) \
	X(glGetSamplerParameterIuiv) X(glGetSamplerParameterfv) X(glGetSamplerParameteriv) X(glGetShaderSource) X(glGetSynciv) \
	X(glGetTexEnvfv) X(glGetTexEnviv) X(glGetTexGendv) X(glGetTexGenfv) X(glGetTexGeniv) X(glGetTexImage) \
	X(glGetTexLevelParameterfv) X(glGetTexParameterIiv) X(glGetTexParameterIuiv) X(glGetTexParameterfv) X(glGetTexParameteriv) \
	X(glGetTransformFeedbackVarying) X(glGetUniformBlockIndex) X(glGetUniformIndices) X(glGetUniformfv) X(glGetUniformiv) \
	X(glGetUniformuiv) X(glGetVertexAttribIiv) X(glGetVertexAttribIuiv) X(glGetVertexAttribPointerv) X(glGetVertexAttribdv) \
	X(glGetVertexAttribfv) X(glGetVertexAttribiv) X(glIndexPointer) X(glIsSync) X(glLightModelfv) X(glLightModeliv) X(glLightiv) \
	X(glMap1d) X(glMap1f) X(glMap2d) X(glMap2f) X(glMapBuffer) X(glMapBufferRange) X(glMaterialfv) X(glMaterialiv) \
	X(glMultiDrawArrays) X(glMultiDrawElements) X(glMultiDrawElementsBaseVertex) X(glPixelMapfv) X(glPixelMapuiv) \
	X(glPixelMapusv) X(glPointParameterfv) X(glPointParameteriv) X(glPolygonStipple) X(glPrioritizeTextures) X(glRectdv) \
	X(glRectfv) X(glRectiv) X(glRectsv) X(glSamplerParameterIiv) X(glSamplerParameterIuiv) X(glSamplerParameterfv) \
	X(glSamplerParameteriv) X(glSecondaryColorPointer) X(glSelectBuffer) X(glTexEnvfv) X(glTexEnviv) X(glTexGendv) X(glTexGeniv) \
	X(glTexImage1D) X(glTexParameterIiv) X(glTexParameterIuiv) X(glTexParameterfv) X(glTexParameteriv) X(glTexSubImage1D) \
	X(glTexSubImage3D) X(glTransformFeedbackVaryings) X(glUnmapBuffer) X(glVertexAttribIPointer) X(glWaitSync)

#define RAVEN_GL_NAME(function, ...) #function,
constexpr std::string_view names[] = {
	RAVEN_GL_SCALARS(RAVEN_GL_NAME) RAVEN_GL_VECTORS(RAVEN_GL_NAME) RAVEN_GL_UNIFORMS(RAVEN_GL_NAME)
	RAVEN_GL_HANDLED(RAVEN_GL_NAME)
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
#undef RAVEN_GL_NAME

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

// The most values a state query may write for its name: the lists of formats are as long as the driver says, every other name
// writes at most a matrix.
size_t state_limit(GLenum name)
{
	GLenum count_name = 0;
	switch (name) {
	case GL_COMPRESSED_TEXTURE_FORMATS: count_name = GL_NUM_COMPRESSED_TEXTURE_FORMATS; break;
	case GL_PROGRAM_BINARY_FORMATS: count_name = GL_NUM_PROGRAM_BINARY_FORMATS; break;
	case GL_SHADER_BINARY_FORMATS: count_name = GL_NUM_SHADER_BINARY_FORMATS; break;
	default: return 64;
	}
	GLint count = 0;
	glGetIntegerv(count_name, &count);
	if (count < 0 || count > 4096) throw std::runtime_error("OpenGL format count exceeds bounds");
	return size_t(count) + 64;
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

// An array the guest pointed at its own memory instead of a buffer. The host driver keeps the guest's address only as a value: each draw
// reads a copy of the vertices it uses (see draw_client).
struct ClientArray {
	// The array's client state, or 0 for a generic vertex attribute.
	GLenum kind = 0;
	// The texture unit of a texture coordinate array, or the index of a generic attribute.
	GLuint index = 0;
	GLint size = 0, stride = 0, normalized = 0, integer = 0, divisor = 0;
	GLenum type = 0;
	uint64_t address = 0;
};

size_t element_bytes(GLint size, GLenum type)
{
	if (type == GL_INT_2_10_10_10_REV || type == GL_UNSIGNED_INT_2_10_10_10_REV || type == GL_UNSIGNED_INT_10F_11F_11F_REV) return 4;
	const size_t components = size == GL_BGRA ? 4 : size >= 1 && size <= 4 ? size_t(size) : 0;
	size_t bytes = 0;
	switch (type) {
	case GL_BYTE: case GL_UNSIGNED_BYTE: bytes = 1; break;
	case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_HALF_FLOAT: bytes = 2; break;
	case GL_INT: case GL_UNSIGNED_INT: case GL_FLOAT: case GL_FIXED: bytes = 4; break;
	case GL_DOUBLE: bytes = 8; break;
	}
	if (!components || !bytes) throw std::runtime_error("unsupported OpenGL client array layout");
	return components * bytes;
}

std::vector<ClientArray> find_client_arrays()
{
	std::vector<ClientArray> arrays;
	// An array whose layout query is 0 has a fixed layout: the size and type given.
	const auto fixed = [&arrays](GLenum kind, GLuint index, GLenum binding, GLenum size, GLint fixed_size, GLenum type, GLenum fixed_type,
		GLenum stride, GLenum pointer) {
		GLint buffer = 0;
		if (!glIsEnabled(kind)) return;
		glGetIntegerv(binding, &buffer);
		if (buffer) return;
		ClientArray array{kind, index};
		array.size = fixed_size;
		if (size) glGetIntegerv(size, &array.size);
		GLint value = GLint(fixed_type);
		if (type) glGetIntegerv(type, &value);
		array.type = GLenum(value);
		glGetIntegerv(stride, &array.stride);
		void *address = nullptr;
		glGetPointerv(pointer, &address);
		array.address = reinterpret_cast<uintptr_t>(address);
		arrays.push_back(array);
	};
	fixed(GL_VERTEX_ARRAY, 0, GL_VERTEX_ARRAY_BUFFER_BINDING, GL_VERTEX_ARRAY_SIZE, 0, GL_VERTEX_ARRAY_TYPE, 0, GL_VERTEX_ARRAY_STRIDE,
		GL_VERTEX_ARRAY_POINTER);
	fixed(GL_NORMAL_ARRAY, 0, GL_NORMAL_ARRAY_BUFFER_BINDING, 0, 3, GL_NORMAL_ARRAY_TYPE, 0, GL_NORMAL_ARRAY_STRIDE, GL_NORMAL_ARRAY_POINTER);
	fixed(GL_COLOR_ARRAY, 0, GL_COLOR_ARRAY_BUFFER_BINDING, GL_COLOR_ARRAY_SIZE, 0, GL_COLOR_ARRAY_TYPE, 0, GL_COLOR_ARRAY_STRIDE,
		GL_COLOR_ARRAY_POINTER);
	fixed(GL_SECONDARY_COLOR_ARRAY, 0, GL_SECONDARY_COLOR_ARRAY_BUFFER_BINDING, GL_SECONDARY_COLOR_ARRAY_SIZE, 0,
		GL_SECONDARY_COLOR_ARRAY_TYPE, 0, GL_SECONDARY_COLOR_ARRAY_STRIDE, GL_SECONDARY_COLOR_ARRAY_POINTER);
	fixed(GL_FOG_COORD_ARRAY, 0, GL_FOG_COORD_ARRAY_BUFFER_BINDING, 0, 1, GL_FOG_COORD_ARRAY_TYPE, 0, GL_FOG_COORD_ARRAY_STRIDE,
		GL_FOG_COORD_ARRAY_POINTER);
	fixed(GL_INDEX_ARRAY, 0, GL_INDEX_ARRAY_BUFFER_BINDING, 0, 1, GL_INDEX_ARRAY_TYPE, 0, GL_INDEX_ARRAY_STRIDE, GL_INDEX_ARRAY_POINTER);
	fixed(GL_EDGE_FLAG_ARRAY, 0, GL_EDGE_FLAG_ARRAY_BUFFER_BINDING, 0, 1, 0, GL_UNSIGNED_BYTE, GL_EDGE_FLAG_ARRAY_STRIDE,
		GL_EDGE_FLAG_ARRAY_POINTER);
	GLint original = 0, textures = 0;
	glGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE, &original);
	glGetIntegerv(GL_MAX_TEXTURE_COORDS, &textures);
	if (textures < 0 || textures > 32) throw std::runtime_error("OpenGL texture array count exceeds bounds");
	for (GLint index = 0; index < textures; ++index) {
		glClientActiveTexture(GL_TEXTURE0 + index);
		fixed(GL_TEXTURE_COORD_ARRAY, GLuint(index), GL_TEXTURE_COORD_ARRAY_BUFFER_BINDING, GL_TEXTURE_COORD_ARRAY_SIZE, 0,
			GL_TEXTURE_COORD_ARRAY_TYPE, 0, GL_TEXTURE_COORD_ARRAY_STRIDE, GL_TEXTURE_COORD_ARRAY_POINTER);
	}
	glClientActiveTexture(GLenum(original));
	GLint attributes = 0;
	glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &attributes);
	if (attributes < 0 || attributes > 64) throw std::runtime_error("OpenGL vertex attribute count exceeds bounds");
	for (GLint index = 0; index < attributes; ++index) {
		GLint enabled = 0, buffer = 0;
		glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
		if (!enabled) continue;
		glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &buffer);
		if (buffer) continue;
		ClientArray array{0, GLuint(index)};
		GLint type = 0;
		glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_SIZE, &array.size);
		glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_TYPE, &type);
		glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &array.stride);
		glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &array.normalized);
		glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_INTEGER, &array.integer);
		glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_DIVISOR, &array.divisor);
		array.type = GLenum(type);
		void *address = nullptr;
		glGetVertexAttribPointerv(index, GL_VERTEX_ATTRIB_ARRAY_POINTER, &address);
		array.address = reinterpret_cast<uintptr_t>(address);
		arrays.push_back(array);
	}
	return arrays;
}

// Points one array at a host address; GL_ARRAY_BUFFER must be unbound.
void point_array(const ClientArray &array, const void *data)
{
	switch (array.kind) {
	case GL_VERTEX_ARRAY: glVertexPointer(array.size, array.type, array.stride, data); break;
	case GL_NORMAL_ARRAY: glNormalPointer(array.type, array.stride, data); break;
	case GL_COLOR_ARRAY: glColorPointer(array.size, array.type, array.stride, data); break;
	case GL_SECONDARY_COLOR_ARRAY: glSecondaryColorPointer(array.size, array.type, array.stride, data); break;
	case GL_FOG_COORD_ARRAY: glFogCoordPointer(array.type, array.stride, data); break;
	case GL_INDEX_ARRAY: glIndexPointer(array.type, array.stride, data); break;
	case GL_EDGE_FLAG_ARRAY: glEdgeFlagPointer(array.stride, data); break;
	case GL_TEXTURE_COORD_ARRAY:
		glClientActiveTexture(GL_TEXTURE0 + array.index);
		glTexCoordPointer(array.size, array.type, array.stride, data);
		break;
	default:
		if (array.integer) glVertexAttribIPointer(array.index, array.size, array.type, array.stride, data);
		else glVertexAttribPointer(array.index, array.size, array.type, GLboolean(array.normalized), array.stride, data);
	}
}

// Runs a draw of the given instances that reads vertices first to last from the guest's client arrays; an array with a divisor reads
// one element for every divisor instances instead. Each array's elements are copied from guest memory before the host is touched, so
// an unreadable array fails the call with nothing drawn; the driver reads the copies, and the guest's addresses are put back after.
template<typename Draw>
void draw_client(ApiCall &call, const std::vector<ClientArray> &arrays, int64_t first, int64_t last, uint64_t instances, Draw draw)
{
	if (arrays.empty()) { draw(); return; }
	if (!instances) return;
	std::vector<std::vector<uint8_t>> copies;
	std::vector<int64_t> starts;
	copies.reserve(arrays.size());
	size_t total = 0;
	for (const auto &array : arrays) {
		const int64_t element = int64_t(element_bytes(array.size, array.type));
		const int64_t stride = array.stride > 0 ? array.stride : element;
		const int64_t low = array.divisor ? 0 : first, high = array.divisor ? int64_t((instances - 1) / uint64_t(array.divisor)) : last;
		const uint64_t bytes = uint64_t(high - low) * uint64_t(stride) + uint64_t(element);
		const int64_t start = low * stride;
		uint64_t address = 0;
		if (high < low || bytes > (256ULL << 20) || (total += bytes) > (256ULL << 20))
			throw std::runtime_error("OpenGL client arrays exceed bounds");
		if (start < 0 ? __builtin_sub_overflow(array.address, uint64_t(-start), &address) :
			__builtin_add_overflow(array.address, uint64_t(start), &address)) throw ApiFault{};
		copies.push_back(call.read(address, size_t(bytes)));
		starts.push_back(start);
	}
	GLint buffer = 0, unit = 0;
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &buffer);
	glGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE, &unit);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	// The driver reads element i at the array's address plus i strides, so each copy is offered at its first element's place.
	for (size_t index = 0; index < arrays.size(); ++index)
		point_array(arrays[index], reinterpret_cast<const void *>(reinterpret_cast<uintptr_t>(copies[index].data()) - uintptr_t(starts[index])));
	draw();
	for (const auto &array : arrays) point_array(array, reinterpret_cast<const void *>(uintptr_t(array.address)));
	glClientActiveTexture(GLenum(unit));
	glBindBuffer(GL_ARRAY_BUFFER, GLuint(buffer));
}

// The bytes a pixel of format and type takes, or zero for bitmaps, whose pixels are bits.
size_t pixel_bytes(GLenum format, GLenum type)
{
	switch (type) {
	case GL_BITMAP:
		if (format != GL_COLOR_INDEX && format != GL_STENCIL_INDEX) throw std::runtime_error("unsupported OpenGL bitmap format");
		return 0;
	case GL_UNSIGNED_BYTE_3_3_2: case GL_UNSIGNED_BYTE_2_3_3_REV: return 1;
	case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_5_6_5_REV: case GL_UNSIGNED_SHORT_4_4_4_4: case GL_UNSIGNED_SHORT_4_4_4_4_REV:
	case GL_UNSIGNED_SHORT_5_5_5_1: case GL_UNSIGNED_SHORT_1_5_5_5_REV: return 2;
	case GL_UNSIGNED_INT_8_8_8_8: case GL_UNSIGNED_INT_8_8_8_8_REV: case GL_UNSIGNED_INT_10_10_10_2: case GL_UNSIGNED_INT_2_10_10_10_REV:
	case GL_UNSIGNED_INT_10F_11F_11F_REV: case GL_UNSIGNED_INT_5_9_9_9_REV: case GL_UNSIGNED_INT_24_8: return 4;
	case GL_FLOAT_32_UNSIGNED_INT_24_8_REV: return 8;
	}
	if (format == GL_DEPTH_STENCIL) throw std::runtime_error("unsupported OpenGL depth-stencil element");
	size_t components = 0, element = 0;
	switch (format) {
	case GL_RED: case GL_GREEN: case GL_BLUE: case GL_ALPHA: case GL_LUMINANCE: case GL_DEPTH_COMPONENT: case GL_STENCIL_INDEX:
	case GL_COLOR_INDEX: case GL_RED_INTEGER: case GL_GREEN_INTEGER: case GL_BLUE_INTEGER: case GL_ALPHA_INTEGER: components = 1; break;
	case GL_RG: case GL_LUMINANCE_ALPHA: case GL_RG_INTEGER: components = 2; break;
	case GL_RGB: case GL_BGR: case GL_RGB_INTEGER: case GL_BGR_INTEGER: components = 3; break;
	case GL_RGBA: case GL_BGRA: case GL_RGBA_INTEGER: case GL_BGRA_INTEGER: components = 4; break;
	default: throw std::runtime_error("unsupported OpenGL texture format");
	}
	switch (type) {
	case GL_UNSIGNED_BYTE: case GL_BYTE: element = 1; break;
	case GL_UNSIGNED_SHORT: case GL_SHORT: case GL_HALF_FLOAT: element = 2; break;
	case GL_UNSIGNED_INT: case GL_INT: case GL_FLOAT: element = 4; break;
	default: throw std::runtime_error("unsupported OpenGL texture element");
	}
	return components * element;
}

// The bytes an image of the given size spans in client memory under the pack or unpack state, from its first byte to its last.
size_t pixel_size(GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, bool pack)
{
	if (width < 0 || height < 0 || depth < 0 || width > 8192 || height > 8192 || depth > 2048)
		throw std::runtime_error("OpenGL texture dimensions exceed bounds");
	if (!width || !height || !depth) return 0;
	const size_t bytes_per_pixel = pixel_bytes(format, type);
	GLint alignment = 0, row = 0, image = 0, skip_rows = 0, skip_pixels = 0, skip_images = 0;
	glGetIntegerv(pack ? GL_PACK_ALIGNMENT : GL_UNPACK_ALIGNMENT, &alignment);
	glGetIntegerv(pack ? GL_PACK_ROW_LENGTH : GL_UNPACK_ROW_LENGTH, &row);
	glGetIntegerv(pack ? GL_PACK_IMAGE_HEIGHT : GL_UNPACK_IMAGE_HEIGHT, &image);
	glGetIntegerv(pack ? GL_PACK_SKIP_ROWS : GL_UNPACK_SKIP_ROWS, &skip_rows);
	glGetIntegerv(pack ? GL_PACK_SKIP_PIXELS : GL_UNPACK_SKIP_PIXELS, &skip_pixels);
	glGetIntegerv(pack ? GL_PACK_SKIP_IMAGES : GL_UNPACK_SKIP_IMAGES, &skip_images);
	if ((alignment != 1 && alignment != 2 && alignment != 4 && alignment != 8) || row < 0 || image < 0 ||
		skip_rows < 0 || skip_pixels < 0 || skip_images < 0 || row > 8192 || image > 8192 ||
		skip_rows > 8192 || skip_pixels > 8192 || skip_images > 2048)
		throw std::runtime_error("OpenGL unpack state exceeds bounds");
	const size_t pixels = size_t(row ? row : width);
	// A bitmap row holds a bit for each pixel; skipped pixels are skipped bits.
	const size_t stride = bytes_per_pixel ? ((pixels * bytes_per_pixel + alignment - 1) / alignment) * alignment :
		(((pixels + 7) / 8 + alignment - 1) / alignment) * alignment;
	const size_t last_row = bytes_per_pixel ? (size_t(skip_pixels) + width) * bytes_per_pixel : (size_t(skip_pixels) + width + 7) / 8;
	const size_t layer = stride * size_t(image ? image : height);
	const size_t bytes = (size_t(skip_images) + depth - 1) * layer + (size_t(skip_rows) + height - 1) * stride + last_row;
	if (bytes > (256ULL << 20)) throw std::runtime_error("OpenGL texture upload exceeds bounds");
	return bytes;
}

size_t unpack_size(GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type)
{
	return pixel_size(width, height, depth, format, type, false);
}

// How many values a query writes for its name. A query writes the same values each time it is asked, so running it over two buffers
// with different fills shows which elements it wrote: those that differ from their fill in either run. Queries write a prefix.
template<typename T, typename Query> size_t written_count(Query query, size_t limit = 64)
{
	std::vector<T> first(limit), second(limit);
	std::memset(static_cast<void *>(first.data()), 0x5a, limit * sizeof(T));
	std::memset(static_cast<void *>(second.data()), 0xa5, limit * sizeof(T));
	query(first.data());
	query(second.data());
	std::array<uint8_t, sizeof(T)> first_fill, second_fill;
	first_fill.fill(0x5a);
	second_fill.fill(0xa5);
	size_t count = 0;
	for (size_t index = 0; index < limit; ++index)
		if (std::memcmp(&first[index], first_fill.data(), sizeof(T)) || std::memcmp(&second[index], second_fill.data(), sizeof(T)))
			count = index + 1;
	return count;
}

// Answers a query into guest memory with as many values as the query writes for its name.
template<typename T, typename Query> void query_output(ApiCall &call, uint64_t address, Query query, size_t limit = 64)
{
	output<T>(call, address, written_count<T>(query, limit), [&](T *values) { if (values) query(values); });
}

template<typename... Args> using last_argument = std::tuple_element_t<sizeof...(Args) - 1, std::tuple<Args...>>;

// Calls a function whose last argument points at count values of the guest's, copied to host memory first.
template<typename... Args, size_t... Index>
void vector_call(ApiCall &call, void (*function)(Args...), size_t count, std::index_sequence<Index...>)
{
	using Value = std::remove_cv_t<std::remove_pointer_t<last_argument<Args...>>>;
	const auto values = input<Value>(call, call[sizeof...(Args) - 1], count);
	std::array<uint64_t, 4> floating{};
	if constexpr ((std::is_floating_point_v<Args> || ...)) floating = call.process.api_float_arguments();
	function(scalar_argument<std::tuple_element_t<Index, std::tuple<Args...>>>(call, floating, Index)..., values.data());
}

// An array of count guest strings, as shader sources and variable names are passed.
std::vector<std::string> guest_strings(ApiCall &call, uint64_t address, int32_t count)
{
	if (count < 0 || count > 4096) throw std::runtime_error("OpenGL string count exceeds bounds");
	std::vector<std::string> strings;
	size_t total = 0;
	for (int32_t index = 0; index < count; ++index) {
		strings.push_back(call.narrow(call.load_pointer(address + size_t(index) * call.pointer_width()), 65536));
		if ((total += strings.back().size()) > 16 * 1024 * 1024) throw std::runtime_error("OpenGL strings exceed bounds");
	}
	return strings;
}

GLenum buffer_binding(GLenum target)
{
	switch (target) {
	case GL_ARRAY_BUFFER: return GL_ARRAY_BUFFER_BINDING;
	case GL_ELEMENT_ARRAY_BUFFER: return GL_ELEMENT_ARRAY_BUFFER_BINDING;
	case GL_PIXEL_PACK_BUFFER: return GL_PIXEL_PACK_BUFFER_BINDING;
	case GL_PIXEL_UNPACK_BUFFER: return GL_PIXEL_UNPACK_BUFFER_BINDING;
	case GL_UNIFORM_BUFFER: return GL_UNIFORM_BUFFER_BINDING;
	case GL_TEXTURE_BUFFER: return GL_TEXTURE_BUFFER_BINDING;
	case GL_TRANSFORM_FEEDBACK_BUFFER: return GL_TRANSFORM_FEEDBACK_BUFFER_BINDING;
	case GL_COPY_READ_BUFFER: return GL_COPY_READ_BUFFER_BINDING;
	case GL_COPY_WRITE_BUFFER: return GL_COPY_WRITE_BUFFER_BINDING;
	case GL_DRAW_INDIRECT_BUFFER: return GL_DRAW_INDIRECT_BUFFER_BINDING;
	case GL_QUERY_BUFFER: return GL_QUERY_BUFFER_BINDING;
	default: return 0;
	}
}

GLuint bound_buffer(GLenum binding)
{
	GLint buffer = 0;
	if (binding) glGetIntegerv(binding, &buffer);
	return GLuint(buffer);
}

// The pixels an upload reads: guest memory, or an offset into the bound unpack buffer, which must lie inside it. A null pointer is
// accepted only where the call allocates without reading.
const void *unpack_data(ApiCall &call, uint64_t address, size_t size, std::vector<uint8_t> &bytes, bool null_allowed = false)
{
	if (bound_buffer(GL_PIXEL_UNPACK_BUFFER_BINDING)) {
		GLint64 capacity = 0;
		glGetBufferParameteri64v(GL_PIXEL_UNPACK_BUFFER, GL_BUFFER_SIZE, &capacity);
		if (capacity < 0 || address > uint64_t(capacity) || size > uint64_t(capacity) - address)
			throw std::runtime_error("OpenGL upload offset exceeds its buffer");
		return reinterpret_cast<const void *>(address);
	}
	if (!address && null_allowed) return nullptr;
	bytes = call.read(address, size);
	return bytes.data();
}

// Pixels a read-back writes: into guest memory, or at an offset into the bound pack buffer, which must lie inside it.
template<typename Read> void pack_output(ApiCall &call, uint64_t address, size_t size, Read read)
{
	if (bound_buffer(GL_PIXEL_PACK_BUFFER_BINDING)) {
		GLint64 capacity = 0;
		glGetBufferParameteri64v(GL_PIXEL_PACK_BUFFER, GL_BUFFER_SIZE, &capacity);
		if (capacity < 0 || address > uint64_t(capacity) || size > uint64_t(capacity) - address)
			throw std::runtime_error("OpenGL read-back offset exceeds its buffer");
		read(reinterpret_cast<void *>(address));
		return;
	}
	if (size > (256ULL << 20)) throw std::runtime_error("OpenGL read-back exceeds bounds");
	output<uint8_t>(call, address, size, [&](uint8_t *bytes) { read(bytes); });
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
	// Windows shows an OpenGL window opaque whatever alpha its pixels hold, and a game that never writes alpha leaves it at zero. A
	// Wayland surface is presented opaque through EGL_EXT_present_opaque; without it, the configuration has no alpha.
	bool present_opaque = false;
	HostPlatform platform = HostPlatform::wayland;
	bool queued_elements = std::getenv("RAVEN_QUEUED_GRAPHICS") && std::getenv("RAVEN_QUEUED_OPENGL");
	uint64_t queued_draws = 0;
	struct Context {
		EGLContext host;
		uint32_t owner = 0;
		int interval = 1;
		bool used = false, arrays_valid = false, elements = false;
		// The arrays draws read from guest memory, found again when array state changes (arrays_valid false).
		std::vector<ClientArray> client{};
		// Errors raised before a query Raven made for itself, which the guest has yet to read (see quietly).
		std::vector<GLenum> errors{};
		// The feedback or selection buffer the driver fills while the render mode lasts, copied to the guest's when it ends.
		struct Capture { uint64_t address = 0; std::vector<uint8_t> bytes; };
		Capture feedback{}, selection{};
	};
	// Sync objects by the token the guest holds, so that the guest never hands the driver an address.
	std::map<uint64_t, GLsync> syncs;
	uint64_t next_sync = 0x5900000000000001ULL;
	// A buffer range the guest mapped: guest memory stands in for the driver's mapping, filled from the buffer when the guest reads it,
	// and written to the buffer when the guest unmaps or flushes it. Keyed by context and buffer name.
	struct Mapping { uint64_t address = 0, offset = 0, length = 0; GLbitfield access = 0; };
	std::map<std::pair<uint64_t, GLuint>, Mapping> mappings;
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
	// With a host thread for each guest thread, the host thread whose binding of a context and a surface stands; 0 when none does.
	std::atomic<pid_t> bound{0};
	std::map<uint32_t, Current> current;
	std::map<uint64_t, Surface> surfaces;
	std::map<uint64_t, uint32_t> formats;
	std::map<std::string, uint64_t> strings;
	uint64_t next_context = 0x4700000000000001ULL;

	~State()
	{
		if (threads) threads->set_host_end({});
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
			const char *extensions = windowed ? eglQueryString(candidate, EGL_EXTENSIONS) : nullptr;
			for (const char *found = extensions; found && (found = std::strstr(found, "EGL_EXT_present_opaque")); found += 22)
				if ((found == extensions || found[-1] == ' ') && (found[22] == ' ' || found[22] == 0)) present_opaque = true;
			if (!windowed || present_opaque) {
				if (!eglChooseConfig(candidate, attributes, &config, 1, &count) || !count) { eglTerminate(candidate); return false; }
				display = candidate;
				return true;
			}
		}
		std::array<EGLConfig, 256> configs{};
		config = nullptr;
		if (!xcb) {
			// A Wayland surface without EGL_EXT_present_opaque: a configuration without alpha, which the compositor shows opaque.
			const EGLint opaque[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
				EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 0, EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE};
			if (!eglChooseConfig(candidate, opaque, configs.data(), EGLint(configs.size()), &count)) count = 0;
			for (EGLint index = 0; index < count && !config; ++index) {
				EGLint alpha = 0;
				eglGetConfigAttrib(candidate, configs[size_t(index)], EGL_ALPHA_SIZE, &alpha);
				if (!alpha) config = configs[size_t(index)];
			}
		}
#ifdef RAVEN_XCB
		if (xcb) {
			const auto visual = xcb_setup_roots_iterator(xcb_get_setup(static_cast<xcb_connection_t *>(native))).data->root_visual;
			if (!eglChooseConfig(candidate, attributes, configs.data(), EGLint(configs.size()), &count)) count = 0;
			EGLint best_alpha = -1;
			for (EGLint index = 0; index < count; ++index) {
				EGLint id = 0, alpha = 0;
				eglGetConfigAttrib(candidate, configs[size_t(index)], EGL_NATIVE_VISUAL_ID, &id);
				eglGetConfigAttrib(candidate, configs[size_t(index)], EGL_ALPHA_SIZE, &alpha);
				if (uint32_t(id) == visual && alpha > best_alpha) { config = configs[size_t(index)]; best_alpha = alpha; }
			}
		}
#endif
		if (!config) { eglTerminate(candidate); return false; }
		display = candidate;
		return true;
	}

	// The host keeps a binding for the thread that made it, and a surface or a context bound in one thread cannot be bound in another.
	// The guest's threads take turns at the one binding the runtime keeps, so the host thread that holds it lets go of it first.
	void claim()
	{
		if (!threads || !threads->host_threads()) return;
		// The host also keeps for each thread which API its contexts are made for, and a new thread starts with another one.
		if (thread_local bool chosen = false; !chosen) {
			if (!eglBindAPI(EGL_OPENGL_API)) throw std::runtime_error("cannot select OpenGL for a host thread");
			chosen = true;
		}
		const pid_t self = pid_t(::gettid());
		const pid_t holder = bound.exchange(self);
		if (holder && holder != self)
			threads->on_host(holder, [display = display] { eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); });
	}

	void drop_surface(uint64_t handle)
	{
		const auto found = surfaces.find(handle);
		if (found == surfaces.end()) return;
		claim();
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
		claim();
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
			if (target.xid && !windows.host_surface(handle, true)) throw std::runtime_error("OpenGL window lost its host surface");
		}
		target.width = size->first;
		target.height = size->second;
		if (target.host != EGL_NO_SURFACE) return target.host;
		// A window not shown yet gets its surface too, as a game makes its context current and swaps a first frame before it shows
		// the window: Windows swaps it, and nothing appears until the window is shown.
		if (windowed && platform == HostPlatform::xcb) {
#ifdef RAVEN_XCB
			if (!target.xid) {
				const auto native = windows.host_surface(handle, true);
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
				const auto native = windows.host_surface(handle, true);
				if (!native) return EGL_NO_SURFACE;
				if (native->platform != HostPlatform::wayland || eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, native->display, nullptr) != display)
					throw std::runtime_error("OpenGL window belongs to another display");
				target.window = wl_egl_window_create(static_cast<wl_surface *>(native->surface), int(target.width), int(target.height));
				if (!target.window) throw std::runtime_error("cannot create OpenGL Wayland window");
			}
			const EGLint opaque[] = {EGL_PRESENT_OPAQUE_EXT, EGL_TRUE, EGL_NONE};
			target.host = eglCreateWindowSurface(display, config, reinterpret_cast<EGLNativeWindowType>(target.window),
				present_opaque ? opaque : nullptr);
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

	// Runs a query Raven makes for itself: an error it raises is not the guest's, and errors the guest has not read yet are kept for it.
	template<typename Function> void quietly(ApiCall &call, Function function)
	{
		auto &errors = contexts.at(selected(call).context).errors;
		for (int count = 0; count < 8; ++count) {
			const auto error = glGetError();
			if (error == GL_NO_ERROR) break;
			if (std::find(errors.begin(), errors.end(), error) == errors.end()) errors.push_back(error);
		}
		function();
		for (int count = 0; count < 8 && glGetError() != GL_NO_ERROR; ++count) {}
	}

	// The values a parameter call reads: as many as the matching query writes for the same name, in a buffer of at least sixteen so
	// that a name the query does not know still finds memory the driver may read.
	template<typename T, typename Query> std::vector<T> parameter_input(ApiCall &call, uint64_t address, Query query)
	{
		size_t count = 0;
		quietly(call, [&] { count = written_count<T>(query); });
		auto values = input<T>(call, address, count);
		values.resize(std::max<size_t>(values.size(), 16));
		return values;
	}

	// An indexed draw: the indices come from the element buffer or from guest memory, and client arrays are copied for the vertices
	// they name, moved by base.
	template<typename Draw>
	void draw_elements(ApiCall &call, int32_t count, GLenum type, uint64_t address, int32_t base, int32_t instances, Draw draw)
	{
		const auto &arrays = client_arrays(call);
		if (count < 0 || count > 16 * 1024 * 1024) throw std::runtime_error("OpenGL element count exceeds bounds");
		const size_t element = type == GL_UNSIGNED_BYTE ? 1 : type == GL_UNSIGNED_SHORT ? 2 : type == GL_UNSIGNED_INT ? 4 : 0;
		if (!element) throw std::runtime_error("unsupported OpenGL element type");
		const GLuint buffer = bound_buffer(GL_ELEMENT_ARRAY_BUFFER_BINDING);
		const bool queued = call.process.serving_queued();
		if (!buffer && queued)
			throw std::runtime_error("queued OpenGL indexed draw has no element buffer");
		std::vector<uint8_t> bytes;
		const void *indices = nullptr;
		const size_t size = size_t(count) * element;
		if (buffer) {
			GLint64 capacity = 0;
			glGetBufferParameteri64v(GL_ELEMENT_ARRAY_BUFFER, GL_BUFFER_SIZE, &capacity);
			if (capacity < 0 || address > uint64_t(capacity) || size > uint64_t(capacity) - address)
				throw std::runtime_error("OpenGL element offset exceeds its buffer");
			indices = reinterpret_cast<const void *>(address);
		} else if (size) { bytes = call.read(address, size); indices = bytes.data(); }
		if (arrays.empty()) draw(indices);
		else if (size && instances > 0) {
			// The vertices the draw reads run from its lowest index to its highest, leaving out the index that restarts a primitive.
			if (buffer) {
				bytes.resize(size);
				glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER, GLintptr(address), GLsizeiptr(size), bytes.data());
			}
			const uint32_t all = element == 1 ? 0xffu : element == 2 ? 0xffffu : 0xffffffffu;
			GLint restart_index = 0;
			glGetIntegerv(GL_PRIMITIVE_RESTART_INDEX, &restart_index);
			const bool fixed = glIsEnabled(GL_PRIMITIVE_RESTART_FIXED_INDEX), restart = fixed || glIsEnabled(GL_PRIMITIVE_RESTART);
			const uint32_t skipped = fixed ? all : uint32_t(restart_index);
			uint32_t first = UINT32_MAX, last = 0;
			for (size_t offset = 0; offset < size; offset += element) {
				uint32_t index = 0;
				std::memcpy(&index, bytes.data() + offset, element);
				if (restart && index == skipped) continue;
				first = std::min(first, index);
				last = std::max(last, index);
			}
			// Indices that only restart primitives draw nothing.
			if (first <= last) draw_client(call, arrays, int64_t(first) + base, int64_t(last) + base, uint64_t(instances), [&] { draw(indices); });
		}
		if (queued) ++queued_draws;
	}

	template<typename Draw> void draw_arrays(ApiCall &call, int32_t first, int32_t count, int32_t instances, Draw draw)
	{
		const auto &arrays = client_arrays(call);
		// A negative first, count or instance count is refused by the driver before it reads a vertex.
		if (arrays.empty() || first < 0 || count < 0 || instances < 0) draw();
		else if (count) draw_client(call, arrays, first, int64_t(first) + count - 1, uint64_t(instances), draw);
	}

	const std::vector<ClientArray> &client_arrays(ApiCall &call)
	{
		auto &context = contexts.at(selected(call).context);
		if (!context.arrays_valid) {
			context.client = find_client_arrays();
			context.arrays_valid = true;
		}
		return context.client;
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
	// A host thread that ends with a binding would keep its context and surface from every other thread.
	threads.set_host_end([&s] {
		if (s.display == EGL_NO_DISPLAY || eglGetCurrentContext() == EGL_NO_CONTEXT) return;
		eglMakeCurrent(s.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		pid_t self = pid_t(::gettid());
		s.bound.compare_exchange_strong(self, 0);
	});
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
		s.claim();
		const auto context = eglCreateContext(s.display, s.config, EGL_NO_CONTEXT, nullptr);
		if (context == EGL_NO_CONTEXT) { call.fail(50); return; }
		const auto token = s.next_context++;
		s.contexts.emplace(token, State::Context{context});
		call.ret(token);
	});
	registry.add("wglMakeCurrent", 2, [&s](ApiCall &call) {
		const auto thread = call.environment.thread_id();
		const auto previous = s.selected(call);
		s.claim();
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
		s.claim();
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
		s.claim();
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
		static constexpr std::string_view array_changes[] = {
			"glBindVertexArray", "glCallList", "glCallLists", "glColorPointer", "glDeleteBuffers", "glDeleteVertexArrays", "glDisableClientState",
			"glDisableVertexAttribArray", "glEdgeFlagPointer", "glEnableClientState", "glEnableVertexAttribArray", "glFogCoordPointer",
			"glIndexPointer", "glInterleavedArrays", "glNormalPointer", "glPopClientAttrib", "glSecondaryColorPointer", "glTexCoordPointer",
			"glVertexAttribDivisor", "glVertexAttribIPointer", "glVertexAttribPointer", "glVertexPointer"};
		const bool changed = std::find(std::begin(array_changes), std::end(array_changes), name) != std::end(array_changes);
		const bool elements = std::string_view(name) == "glBindVertexArray" || std::string_view(name) == "glDeleteBuffers" ||
			std::string_view(name) == "glDeleteVertexArrays" || std::string_view(name) == "glPopClientAttrib";
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
		query_output<GLint>(call, call[1], [&](GLint *values) { glGetIntegerv(call.u32(0), values); }, state_limit(call.u32(0)));
	});
	add("glGetFloatv", 2, [](ApiCall &call) {
		query_output<GLfloat>(call, call[1], [&](GLfloat *values) { glGetFloatv(call.u32(0), values); }, state_limit(call.u32(0)));
	});
	add("glGetDoublev", 2, [](ApiCall &call) {
		query_output<GLdouble>(call, call[1], [&](GLdouble *values) { glGetDoublev(call.u32(0), values); }, state_limit(call.u32(0)));
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
		glColorPointer(call.i32(0), call.u32(1), call.i32(2), reinterpret_cast<const void *>(call[3]));
	});
	add("glTexCoordPointer", 4, [](ApiCall &call) {
		glTexCoordPointer(call.i32(0), call.u32(1), call.i32(2), reinterpret_cast<const void *>(call[3]));
	});
	add("glNormalPointer", 3, [](ApiCall &call) {
		glNormalPointer(call.u32(0), call.i32(1), reinterpret_cast<const void *>(call[2]));
	});
	add("glVertexAttribPointer", 6, [](ApiCall &call) {
		glVertexAttribPointer(call.u32(0), call.i32(1), call.u32(2), GLboolean(call.u32(3)), call.i32(4), reinterpret_cast<const void *>(call[5]));
	});
	add("glDrawElements", 4, [&s](ApiCall &call) {
		s.draw_elements(call, call.i32(1), call.u32(2), call[3], 0, 1,
			[&call](const void *indices) { glDrawElements(call.u32(0), call.i32(1), call.u32(2), indices); });
	});
	add("glClear", 1, [](ApiCall &call) { glClear(call.u32(0)); });
	add("glClearColor", 4, [](ApiCall &call) { const auto f = floats(call); glClearColor(f[0], f[1], f[2], f[3]); });
	add("glColor4f", 4, [](ApiCall &call) { const auto f = floats(call); glColor4f(f[0], f[1], f[2], f[3]); });
	add("glVertex2f", 2, [](ApiCall &call) { const auto f = floats(call); glVertex2f(f[0], f[1]); });
	add("glVertex3f", 3, [](ApiCall &call) { const auto f = floats(call); glVertex3f(f[0], f[1], f[2]); });
	add("glBegin", 1, [](ApiCall &call) { glBegin(call.u32(0)); });
	add("glEnd", 0, [](ApiCall &) { glEnd(); });
	add("glViewport", 4, [](ApiCall &call) { glViewport(call.i32(0), call.i32(1), call.i32(2), call.i32(3)); });
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
		glVertexPointer(call.i32(0), call.u32(1), call.i32(2), reinterpret_cast<const void *>(call[3]));
	});
	add("glInterleavedArrays", 3, [](ApiCall &call) {
		glInterleavedArrays(call.u32(0), call.i32(1), reinterpret_cast<const void *>(call[2]));
	});
	add("glDrawArrays", 3, [&s](ApiCall &call) {
		s.draw_arrays(call, call.i32(1), call.i32(2), 1, [&call] { glDrawArrays(call.u32(0), call.i32(1), call.i32(2)); });
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
	const auto vector = [&add]<typename... Args>(const char *name, void (*function)(Args...), size_t count) {
		add(name, sizeof...(Args), [function, count](ApiCall &call) {
			vector_call(call, function, count, std::make_index_sequence<sizeof...(Args) - 1>{});
		});
	};
	const auto uniform = [&add]<typename... Args>(const char *name, void (*function)(Args...), size_t group) {
		add(name, sizeof...(Args), [function, group](ApiCall &call) {
			if (call.i32(1) < 0 || call.u32(1) > 65536) throw std::runtime_error("OpenGL uniform count exceeds bounds");
			vector_call(call, function, size_t(call.u32(1)) * group, std::make_index_sequence<sizeof...(Args) - 1>{});
		});
	};
#define RAVEN_GL_SCALAR(function) scalar(#function, function);
#define RAVEN_GL_VECTOR(function, count) vector(#function, function, count);
#define RAVEN_GL_UNIFORM(function, group) uniform(#function, function, group);
	RAVEN_GL_SCALARS(RAVEN_GL_SCALAR)
	RAVEN_GL_VECTORS(RAVEN_GL_VECTOR)
	RAVEN_GL_UNIFORMS(RAVEN_GL_UNIFORM)
#undef RAVEN_GL_SCALAR
#undef RAVEN_GL_VECTOR
#undef RAVEN_GL_UNIFORM
	add("glRectfv", 2, [](ApiCall &call) { glRectfv(input<GLfloat>(call, call[0], 2).data(), input<GLfloat>(call, call[1], 2).data()); });
	add("glRectdv", 2, [](ApiCall &call) { glRectdv(input<GLdouble>(call, call[0], 2).data(), input<GLdouble>(call, call[1], 2).data()); });
	add("glRectiv", 2, [](ApiCall &call) { glRectiv(input<GLint>(call, call[0], 2).data(), input<GLint>(call, call[1], 2).data()); });
	add("glRectsv", 2, [](ApiCall &call) { glRectsv(input<GLshort>(call, call[0], 2).data(), input<GLshort>(call, call[1], 2).data()); });

	// Draws.
	add("glDrawArraysInstanced", 4, [&s](ApiCall &call) {
		s.draw_arrays(call, call.i32(1), call.i32(2), call.i32(3),
			[&call] { glDrawArraysInstanced(call.u32(0), call.i32(1), call.i32(2), call.i32(3)); });
	});
	add("glArrayElement", 1, [&s](ApiCall &call) { s.draw_arrays(call, call.i32(0), 1, 1, [&call] { glArrayElement(call.i32(0)); }); });
	add("glDrawElementsInstanced", 5, [&s](ApiCall &call) {
		s.draw_elements(call, call.i32(1), call.u32(2), call[3], 0, call.i32(4),
			[&call](const void *indices) { glDrawElementsInstanced(call.u32(0), call.i32(1), call.u32(2), indices, call.i32(4)); });
	});
	add("glDrawElementsBaseVertex", 5, [&s](ApiCall &call) {
		s.draw_elements(call, call.i32(1), call.u32(2), call[3], call.i32(4), 1,
			[&call](const void *indices) { glDrawElementsBaseVertex(call.u32(0), call.i32(1), call.u32(2), indices, call.i32(4)); });
	});
	add("glDrawElementsInstancedBaseVertex", 6, [&s](ApiCall &call) {
		s.draw_elements(call, call.i32(1), call.u32(2), call[3], call.i32(5), call.i32(4), [&call](const void *indices) {
			glDrawElementsInstancedBaseVertex(call.u32(0), call.i32(1), call.u32(2), indices, call.i32(4), call.i32(5));
		});
	});
	// The range is only a promise about the indices; the copy follows the indices themselves.
	add("glDrawRangeElements", 6, [&s](ApiCall &call) {
		s.draw_elements(call, call.i32(3), call.u32(4), call[5], 0, 1, [&call](const void *indices) {
			glDrawRangeElements(call.u32(0), call.u32(1), call.u32(2), call.i32(3), call.u32(4), indices);
		});
	});
	add("glDrawRangeElementsBaseVertex", 7, [&s](ApiCall &call) {
		s.draw_elements(call, call.i32(3), call.u32(4), call[5], call.i32(6), 1, [&call](const void *indices) {
			glDrawRangeElementsBaseVertex(call.u32(0), call.u32(1), call.u32(2), call.i32(3), call.u32(4), indices, call.i32(6));
		});
	});
	// Several draws in one call are the same draws made one by one.
	add("glMultiDrawArrays", 4, [&s](ApiCall &call) {
		if (call.i32(3) < 0 || call.u32(3) > 65536) throw std::runtime_error("OpenGL draw count exceeds bounds");
		const auto firsts = input<GLint>(call, call[1], call.u32(3)), counts = input<GLsizei>(call, call[2], call.u32(3));
		for (size_t index = 0; index < firsts.size(); ++index)
			s.draw_arrays(call, firsts[index], counts[index], 1, [&] { glDrawArrays(call.u32(0), firsts[index], counts[index]); });
	});
	for (const bool based : {false, true})
		add(based ? "glMultiDrawElementsBaseVertex" : "glMultiDrawElements", based ? 6 : 5, [&s, based](ApiCall &call) {
			if (call.i32(4) < 0 || call.u32(4) > 65536) throw std::runtime_error("OpenGL draw count exceeds bounds");
			const auto counts = input<GLsizei>(call, call[1], call.u32(4));
			const auto bases = based ? input<GLint>(call, call[5], call.u32(4)) : std::vector<GLint>(call.u32(4));
			for (size_t index = 0; index < counts.size(); ++index) {
				const auto indices = call.load_pointer(call[3] + index * call.pointer_width());
				s.draw_elements(call, counts[index], call.u32(2), indices, bases[index], 1, [&](const void *data) {
					glDrawElementsBaseVertex(call.u32(0), counts[index], call.u32(2), data, bases[index]);
				});
			}
		});

	// Client arrays in the guest's memory, beside the ones above.
	add("glVertexAttribIPointer", 5, [](ApiCall &call) {
		glVertexAttribIPointer(call.u32(0), call.i32(1), call.u32(2), call.i32(3), reinterpret_cast<const void *>(call[4]));
	});
	add("glSecondaryColorPointer", 4, [](ApiCall &call) {
		glSecondaryColorPointer(call.i32(0), call.u32(1), call.i32(2), reinterpret_cast<const void *>(call[3]));
	});
	add("glFogCoordPointer", 3, [](ApiCall &call) { glFogCoordPointer(call.u32(0), call.i32(1), reinterpret_cast<const void *>(call[2])); });
	add("glIndexPointer", 3, [](ApiCall &call) { glIndexPointer(call.u32(0), call.i32(1), reinterpret_cast<const void *>(call[2])); });
	add("glEdgeFlagPointer", 2, [](ApiCall &call) { glEdgeFlagPointer(call.i32(0), reinterpret_cast<const void *>(call[1])); });

	// Objects named in arrays.
	for (const auto [name, function] : {std::pair{"glGenQueries", &glGenQueries}, {"glGenSamplers", glGenSamplers}})
		add(name, 2, [function](ApiCall &call) {
			if (call.i32(0) < 0 || call.u32(0) > 65536) throw std::runtime_error("OpenGL object count exceeds bounds");
			output<GLuint>(call, call[1], call.u32(0), [&](auto *values) { function(call.i32(0), values); });
		});
	for (const auto [name, function] : {std::pair{"glDeleteFramebuffers", &glDeleteFramebuffers}, {"glDeleteRenderbuffers", glDeleteRenderbuffers},
		{"glDeleteVertexArrays", glDeleteVertexArrays}, {"glDeleteQueries", glDeleteQueries}, {"glDeleteSamplers", glDeleteSamplers}})
		add(name, 2, [function](ApiCall &call) {
			if (call.i32(0) < 0 || call.u32(0) > 65536) throw std::runtime_error("OpenGL object count exceeds bounds");
			function(call.i32(0), input<GLuint>(call, call[1], call.u32(0)).data());
		});
	add("glAreTexturesResident", 3, [](ApiCall &call) {
		if (call.i32(0) < 0 || call.u32(0) > 65536) throw std::runtime_error("OpenGL texture count exceeds bounds");
		const auto textures = input<GLuint>(call, call[1], call.u32(0));
		GLboolean resident = GL_FALSE;
		output<GLboolean>(call, call[2], call.u32(0), [&](auto *values) { resident = glAreTexturesResident(call.i32(0), textures.data(), values); });
		call.ret(resident);
	});
	add("glPrioritizeTextures", 3, [](ApiCall &call) {
		if (call.i32(0) < 0 || call.u32(0) > 65536) throw std::runtime_error("OpenGL texture count exceeds bounds");
		glPrioritizeTextures(call.i32(0), input<GLuint>(call, call[1], call.u32(0)).data(), input<GLclampf>(call, call[2], call.u32(0)).data());
	});
	add("glCallLists", 3, [](ApiCall &call) {
		if (call.i32(0) < 0 || call.u32(0) > 1024 * 1024) throw std::runtime_error("OpenGL list count exceeds bounds");
		size_t element = 0;
		switch (call.u32(1)) {
		case GL_BYTE: case GL_UNSIGNED_BYTE: element = 1; break;
		case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_2_BYTES: element = 2; break;
		case GL_3_BYTES: element = 3; break;
		case GL_INT: case GL_UNSIGNED_INT: case GL_FLOAT: case GL_4_BYTES: element = 4; break;
		}
		// A type the driver refuses is refused before it reads a name.
		const auto names = input<uint8_t>(call, call[2], size_t(call.u32(0)) * element);
		glCallLists(call.i32(0), call.u32(1), element ? names.data() : nullptr);
	});

	// Programs: names, variables and blocks.
	for (const auto [name, function] : {std::pair{"glGetAttribLocation", &glGetAttribLocation},
		{"glGetFragDataLocation", glGetFragDataLocation}, {"glGetFragDataIndex", glGetFragDataIndex}})
		add(name, 2, [function](ApiCall &call) { call.ret(uint64_t(int64_t(function(call.u32(0), call.narrow(call[1], 65536).c_str())))); });
	add("glGetUniformBlockIndex", 2, [](ApiCall &call) { call.ret(glGetUniformBlockIndex(call.u32(0), call.narrow(call[1], 65536).c_str())); });
	add("glBindFragDataLocation", 3, [](ApiCall &call) { glBindFragDataLocation(call.u32(0), call.u32(1), call.narrow(call[2], 65536).c_str()); });
	add("glBindFragDataLocationIndexed", 4, [](ApiCall &call) {
		glBindFragDataLocationIndexed(call.u32(0), call.u32(1), call.u32(2), call.narrow(call[3], 65536).c_str());
	});
	const auto text_output = [](ApiCall &call, size_t capacity, size_t length, size_t text, auto function) {
		if (call.i32(capacity) < 0 || call.u32(capacity) > 4 * 1024 * 1024) throw std::runtime_error("OpenGL text exceeds bounds");
		GLsizei written = 0;
		output<char>(call, call[text], call.u32(capacity), [&](char *values) { function(&written, values); });
		if (call[length]) call.store(call[length], written);
	};
	add("glGetShaderSource", 4, [text_output](ApiCall &call) {
		text_output(call, 1, 2, 3, [&](GLsizei *length, char *text) { glGetShaderSource(call.u32(0), call.i32(1), length, text); });
	});
	for (const auto [name, function] : {std::pair{"glGetActiveUniformName", &glGetActiveUniformName},
		{"glGetActiveUniformBlockName", glGetActiveUniformBlockName}})
		add(name, 5, [text_output, function](ApiCall &call) {
			text_output(call, 2, 3, 4, [&](GLsizei *length, char *text) { function(call.u32(0), call.u32(1), call.i32(2), length, text); });
		});
	const auto variable = [&add, text_output]<typename Size>(const char *name, void (*function)(GLuint, GLuint, GLsizei, GLsizei *, Size *, GLenum *, GLchar *)) {
		add(name, 7, [text_output, function](ApiCall &call) {
			Size size = 0;
			GLenum type = 0;
			text_output(call, 2, 3, 6, [&](GLsizei *length, char *text) { function(call.u32(0), call.u32(1), call.i32(2), length, &size, &type, text); });
			if (call[4]) call.store(call[4], size);
			if (call[5]) call.store(call[5], type);
		});
	};
	variable("glGetActiveUniform", glGetActiveUniform);
	variable("glGetActiveAttrib", glGetActiveAttrib);
	variable("glGetTransformFeedbackVarying", glGetTransformFeedbackVarying);
	add("glGetAttachedShaders", 4, [](ApiCall &call) {
		if (call.i32(1) < 0 || call.u32(1) > 4096) throw std::runtime_error("OpenGL shader count exceeds bounds");
		GLsizei count = 0;
		output<GLuint>(call, call[3], call.u32(1), [&](auto *values) { glGetAttachedShaders(call.u32(0), call.i32(1), &count, values); });
		if (call[2]) call.store(call[2], count);
	});
	add("glGetUniformIndices", 4, [](ApiCall &call) {
		const auto strings = guest_strings(call, call[2], call.i32(1));
		std::vector<const char *> pointers;
		for (const auto &text : strings) pointers.push_back(text.c_str());
		output<GLuint>(call, call[3], strings.size(), [&](auto *values) { glGetUniformIndices(call.u32(0), call.i32(1), pointers.data(), values); });
	});
	add("glTransformFeedbackVaryings", 4, [](ApiCall &call) {
		const auto strings = guest_strings(call, call[2], call.i32(1));
		std::vector<const char *> pointers;
		for (const auto &text : strings) pointers.push_back(text.c_str());
		glTransformFeedbackVaryings(call.u32(0), call.i32(1), pointers.data(), call.u32(3));
	});
	add("glGetActiveUniformsiv", 5, [](ApiCall &call) {
		if (call.i32(1) < 0 || call.u32(1) > 65536) throw std::runtime_error("OpenGL uniform count exceeds bounds");
		const auto indices = input<GLuint>(call, call[2], call.u32(1));
		output<GLint>(call, call[4], call.u32(1), [&](auto *values) { glGetActiveUniformsiv(call.u32(0), call.i32(1), indices.data(), call.u32(3), values); });
	});
	add("glGetActiveUniformBlockiv", 4, [](ApiCall &call) {
		// The indices of a block's uniforms are as many as its uniforms.
		GLint uniforms = 0;
		if (call.u32(2) == GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES)
			glGetActiveUniformBlockiv(call.u32(0), call.u32(1), GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS, &uniforms);
		if (uniforms < 0 || uniforms > 65536) throw std::runtime_error("OpenGL uniform count exceeds bounds");
		query_output<GLint>(call, call[3], [&](GLint *values) { glGetActiveUniformBlockiv(call.u32(0), call.u32(1), call.u32(2), values); },
			size_t(uniforms) + 64);
	});
	for (const auto [name, function] : {std::pair{"glGetUniformfv", &glGetUniformfv}})
		add(name, 3, [function](ApiCall &call) { query_output<GLfloat>(call, call[2], [&](GLfloat *values) { function(call.u32(0), call.i32(1), values); }, 16); });
	add("glGetUniformiv", 3, [](ApiCall &call) { query_output<GLint>(call, call[2], [&](GLint *values) { glGetUniformiv(call.u32(0), call.i32(1), values); }, 16); });
	add("glGetUniformuiv", 3, [](ApiCall &call) { query_output<GLuint>(call, call[2], [&](GLuint *values) { glGetUniformuiv(call.u32(0), call.i32(1), values); }, 16); });

	// State queries: they write as many values as the name asks for.
	add("glGetBooleanv", 2, [](ApiCall &call) {
		query_output<GLboolean>(call, call[1], [&](GLboolean *values) { glGetBooleanv(call.u32(0), values); }, state_limit(call.u32(0)));
	});
	add("glGetInteger64v", 2, [](ApiCall &call) {
		query_output<GLint64>(call, call[1], [&](GLint64 *values) { glGetInteger64v(call.u32(0), values); }, state_limit(call.u32(0)));
	});
	add("glGetIntegeri_v", 3, [](ApiCall &call) { query_output<GLint>(call, call[2], [&](GLint *values) { glGetIntegeri_v(call.u32(0), call.u32(1), values); }); });
	add("glGetBooleani_v", 3, [](ApiCall &call) { query_output<GLboolean>(call, call[2], [&](GLboolean *values) { glGetBooleani_v(call.u32(0), call.u32(1), values); }); });
	add("glGetInteger64i_v", 3, [](ApiCall &call) { query_output<GLint64>(call, call[2], [&](GLint64 *values) { glGetInteger64i_v(call.u32(0), call.u32(1), values); }); });
	const auto query3 = [&add]<typename A, typename T>(const char *name, void (*function)(A, GLenum, T *)) {
		add(name, 3, [function](ApiCall &call) { query_output<T>(call, call[2], [&](T *values) { function(A(call[0]), call.u32(1), values); }); });
	};
	query3("glGetRenderbufferParameteriv", glGetRenderbufferParameteriv);
	query3("glGetQueryiv", glGetQueryiv);
	query3("glGetTexParameterfv", glGetTexParameterfv);
	query3("glGetTexParameteriv", glGetTexParameteriv);
	query3("glGetTexParameterIiv", glGetTexParameterIiv);
	query3("glGetTexParameterIuiv", glGetTexParameterIuiv);
	query3("glGetSamplerParameterfv", glGetSamplerParameterfv);
	query3("glGetSamplerParameteriv", glGetSamplerParameteriv);
	query3("glGetSamplerParameterIiv", glGetSamplerParameterIiv);
	query3("glGetSamplerParameterIuiv", glGetSamplerParameterIuiv);
	query3("glGetVertexAttribdv", glGetVertexAttribdv);
	query3("glGetVertexAttribfv", glGetVertexAttribfv);
	query3("glGetVertexAttribiv", glGetVertexAttribiv);
	query3("glGetVertexAttribIiv", glGetVertexAttribIiv);
	query3("glGetVertexAttribIuiv", glGetVertexAttribIuiv);
	query3("glGetLightfv", glGetLightfv);
	query3("glGetLightiv", glGetLightiv);
	query3("glGetMaterialfv", glGetMaterialfv);
	query3("glGetMaterialiv", glGetMaterialiv);
	query3("glGetTexEnvfv", glGetTexEnvfv);
	query3("glGetTexEnviv", glGetTexEnviv);
	query3("glGetTexGendv", glGetTexGendv);
	query3("glGetTexGenfv", glGetTexGenfv);
	query3("glGetTexGeniv", glGetTexGeniv);
	query3("glGetMultisamplefv", glGetMultisamplefv);
	add("glGetTexLevelParameterfv", 4, [](ApiCall &call) {
		query_output<GLfloat>(call, call[3], [&](GLfloat *values) { glGetTexLevelParameterfv(call.u32(0), call.i32(1), call.u32(2), values); });
	});
	add("glGetFramebufferAttachmentParameteriv", 4, [](ApiCall &call) {
		query_output<GLint>(call, call[3], [&](GLint *values) { glGetFramebufferAttachmentParameteriv(call.u32(0), call.u32(1), call.u32(2), values); });
	});
	add("glGetClipPlane", 2, [](ApiCall &call) { output<GLdouble>(call, call[1], 4, [&](auto *values) { glGetClipPlane(call.u32(0), values); }); });
	// A map holds at most the largest order squared control points of four values.
	const auto get_map = [&add]<typename T>(const char *name, void (*function)(GLenum, GLenum, T *)) {
		add(name, 3, [function](ApiCall &call) {
			GLint order = 0;
			glGetIntegerv(GL_MAX_EVAL_ORDER, &order);
			if (order < 0 || order > 64) throw std::runtime_error("OpenGL evaluator order exceeds bounds");
			query_output<T>(call, call[2], [&](T *values) { function(call.u32(0), call.u32(1), values); }, size_t(order) * size_t(order) * 4);
		});
	};
	get_map("glGetMapdv", glGetMapdv);
	get_map("glGetMapfv", glGetMapfv);
	get_map("glGetMapiv", glGetMapiv);
	// Pointers the guest set are its own addresses or buffer offsets, and go back as they are.
	add("glGetPointerv", 2, [](ApiCall &call) {
		void *value = nullptr;
		call.store_pointer(call[1], 0);
		glGetPointerv(call.u32(0), &value);
		call.store_pointer(call[1], reinterpret_cast<uintptr_t>(value));
	});
	add("glGetVertexAttribPointerv", 3, [](ApiCall &call) {
		void *value = nullptr;
		call.store_pointer(call[2], 0);
		glGetVertexAttribPointerv(call.u32(0), call.u32(1), &value);
		call.store_pointer(call[2], reinterpret_cast<uintptr_t>(value));
	});

	// Parameters whose count depends on their name: as many as the matching query reads back.
	const auto parameters3 = [&add, &s]<typename A, typename T>(const char *name, void (*set)(A, GLenum, const T *), void (*get)(A, GLenum, T *)) {
		add(name, 3, [&s, set, get](ApiCall &call) {
			const auto values = s.parameter_input<T>(call, call[2], [&](T *out) { get(A(call[0]), call.u32(1), out); });
			set(A(call[0]), call.u32(1), values.data());
		});
	};
	parameters3("glTexParameterfv", glTexParameterfv, glGetTexParameterfv);
	parameters3("glTexParameteriv", glTexParameteriv, glGetTexParameteriv);
	parameters3("glTexParameterIiv", glTexParameterIiv, glGetTexParameterIiv);
	parameters3("glTexParameterIuiv", glTexParameterIuiv, glGetTexParameterIuiv);
	parameters3("glSamplerParameterfv", glSamplerParameterfv, glGetSamplerParameterfv);
	parameters3("glSamplerParameteriv", glSamplerParameteriv, glGetSamplerParameteriv);
	parameters3("glSamplerParameterIiv", glSamplerParameterIiv, glGetSamplerParameterIiv);
	parameters3("glSamplerParameterIuiv", glSamplerParameterIuiv, glGetSamplerParameterIuiv);
	parameters3("glLightiv", glLightiv, glGetLightiv);
	parameters3("glTexEnvfv", glTexEnvfv, glGetTexEnvfv);
	parameters3("glTexEnviv", glTexEnviv, glGetTexEnviv);
	parameters3("glTexGendv", glTexGendv, glGetTexGendv);
	parameters3("glTexGeniv", glTexGeniv, glGetTexGeniv);
	const auto parameters2 = [&add, &s]<typename T>(const char *name, void (*set)(GLenum, const T *), void (*get)(GLenum, T *)) {
		add(name, 2, [&s, set, get](ApiCall &call) {
			const auto values = s.parameter_input<T>(call, call[1], [&](T *out) { get(call.u32(0), out); });
			set(call.u32(0), values.data());
		});
	};
	parameters2("glFogfv", glFogfv, glGetFloatv);
	parameters2("glFogiv", glFogiv, glGetIntegerv);
	parameters2("glLightModelfv", glLightModelfv, glGetFloatv);
	parameters2("glLightModeliv", glLightModeliv, glGetIntegerv);
	parameters2("glPointParameterfv", glPointParameterfv, glGetFloatv);
	parameters2("glPointParameteriv", glPointParameteriv, glGetIntegerv);
	// A material set for both faces, or ambient and diffuse at once, reads as many values as one face's ambient colour.
	const auto material = [&add, &s]<typename T>(const char *name, void (*set)(GLenum, GLenum, const T *), void (*get)(GLenum, GLenum, T *)) {
		add(name, 3, [&s, set, get](ApiCall &call) {
			const GLenum face = call.u32(0) == GL_FRONT_AND_BACK ? GL_FRONT : call.u32(0);
			const GLenum parameter = call.u32(1) == GL_AMBIENT_AND_DIFFUSE ? GL_AMBIENT : call.u32(1);
			const auto values = s.parameter_input<T>(call, call[2], [&](T *out) { get(face, parameter, out); });
			set(call.u32(0), call.u32(1), values.data());
		});
	};
	material("glMaterialfv", glMaterialfv, glGetMaterialfv);
	material("glMaterialiv", glMaterialiv, glGetMaterialiv);
	const auto clear = [&add]<typename T>(const char *name, void (*function)(GLenum, GLint, const T *)) {
		add(name, 3, [function](ApiCall &call) {
			function(call.u32(0), call.i32(1), input<T>(call, call[2], call.u32(0) == GL_COLOR ? 4 : 1).data());
		});
	};
	clear("glClearBufferfv", glClearBufferfv);
	clear("glClearBufferiv", glClearBufferiv);
	clear("glClearBufferuiv", glClearBufferuiv);

	// Evaluator maps: order control points of stride values each, the last of which holds as many values as the map's target.
	const auto map_values = [](GLenum target) -> size_t {
		switch (target) {
		case GL_MAP1_INDEX: case GL_MAP2_INDEX: case GL_MAP1_TEXTURE_COORD_1: case GL_MAP2_TEXTURE_COORD_1: return 1;
		case GL_MAP1_TEXTURE_COORD_2: case GL_MAP2_TEXTURE_COORD_2: return 2;
		case GL_MAP1_VERTEX_3: case GL_MAP2_VERTEX_3: case GL_MAP1_NORMAL: case GL_MAP2_NORMAL: case GL_MAP1_TEXTURE_COORD_3:
		case GL_MAP2_TEXTURE_COORD_3: return 3;
		case GL_MAP1_VERTEX_4: case GL_MAP2_VERTEX_4: case GL_MAP1_COLOR_4: case GL_MAP2_COLOR_4: case GL_MAP1_TEXTURE_COORD_4:
		case GL_MAP2_TEXTURE_COORD_4: return 4;
		}
		return 0;
	};
	const auto map_points = [map_values](GLenum target, GLint stride, GLint order) -> size_t {
		const size_t values = map_values(target);
		// What the driver refuses it refuses before reading a point.
		if (!values || stride < GLint(values) || order < 1 || order > 64 || stride > 65536) return 0;
		return size_t(order - 1) * size_t(stride);
	};
	const auto map1 = [&add, map_values, map_points]<typename T>(const char *name, void (*function)(GLenum, T, T, GLint, GLint, const T *)) {
		add(name, 6, [function, map_values, map_points](ApiCall &call) {
			const auto floating = call.process.api_float_arguments();
			const auto span = map_points(call.u32(0), call.i32(3), call.i32(4));
			auto points = input<T>(call, call[5], span ? span + map_values(call.u32(0)) : 0);
			points.resize(std::max<size_t>(points.size(), 16));
			function(call.u32(0), scalar_argument<T>(call, floating, 1), scalar_argument<T>(call, floating, 2), call.i32(3), call.i32(4), points.data());
		});
	};
	map1("glMap1d", glMap1d);
	map1("glMap1f", glMap1f);
	const auto map2 = [&add, map_values, map_points]<typename T>(const char *name,
		void (*function)(GLenum, T, T, GLint, GLint, T, T, GLint, GLint, const T *)) {
		add(name, 10, [function, map_values, map_points](ApiCall &call) {
			const auto floating = call.process.api_float_arguments();
			const auto u = map_points(call.u32(0), call.i32(3), call.i32(4)), v = map_points(call.u32(0), call.i32(7), call.i32(8));
			const bool valid = map_values(call.u32(0)) && call.i32(4) >= 1 && call.i32(8) >= 1 && call.i32(4) <= 64 && call.i32(8) <= 64 &&
				call.i32(3) >= GLint(map_values(call.u32(0))) && call.i32(7) >= GLint(map_values(call.u32(0))) && call.i32(3) <= 65536 && call.i32(7) <= 65536;
			auto points = input<T>(call, call[9], valid ? u + v + map_values(call.u32(0)) : 0);
			points.resize(std::max<size_t>(points.size(), 16));
			function(call.u32(0), scalar_argument<T>(call, floating, 1), scalar_argument<T>(call, floating, 2), call.i32(3), call.i32(4),
				scalar_argument<T>(call, floating, 5), scalar_argument<T>(call, floating, 6), call.i32(7), call.i32(8), points.data());
		});
	};
	map2("glMap2d", glMap2d);
	map2("glMap2f", glMap2f);

	// Pixel maps, stipples and bitmaps: client memory, or the bound pixel buffer.
	const auto pixel_map = [&add]<typename T>(const char *name, void (*function)(GLenum, GLsizei, const T *)) {
		add(name, 3, [function](ApiCall &call) {
			if (call.i32(1) < 0 || call.u32(1) > 65536) throw std::runtime_error("OpenGL pixel map exceeds bounds");
			std::vector<uint8_t> bytes;
			function(call.u32(0), call.i32(1), static_cast<const T *>(unpack_data(call, call[2], size_t(call.u32(1)) * sizeof(T), bytes)));
		});
	};
	pixel_map("glPixelMapfv", glPixelMapfv);
	pixel_map("glPixelMapuiv", glPixelMapuiv);
	pixel_map("glPixelMapusv", glPixelMapusv);
	// A map's size is named by the map's own name plus 0x40.
	const auto get_pixel_map = [&add]<typename T>(const char *name, void (*function)(GLenum, T *)) {
		add(name, 2, [function](ApiCall &call) {
			GLint size = 0;
			glGetIntegerv(call.u32(0) + 0x40, &size);
			if (size < 0 || size > 65536) throw std::runtime_error("OpenGL pixel map exceeds bounds");
			pack_output(call, call[1], size_t(size) * sizeof(T), [&](void *values) { function(call.u32(0), static_cast<T *>(values)); });
		});
	};
	get_pixel_map("glGetPixelMapfv", glGetPixelMapfv);
	get_pixel_map("glGetPixelMapuiv", glGetPixelMapuiv);
	get_pixel_map("glGetPixelMapusv", glGetPixelMapusv);
	add("glPolygonStipple", 1, [](ApiCall &call) {
		std::vector<uint8_t> bytes;
		glPolygonStipple(static_cast<const GLubyte *>(unpack_data(call, call[0], pixel_size(32, 32, 1, GL_COLOR_INDEX, GL_BITMAP, false), bytes)));
	});
	add("glGetPolygonStipple", 1, [](ApiCall &call) {
		pack_output(call, call[0], pixel_size(32, 32, 1, GL_COLOR_INDEX, GL_BITMAP, true), [](void *mask) { glGetPolygonStipple(static_cast<GLubyte *>(mask)); });
	});
	add("glBitmap", 7, [](ApiCall &call) {
		const auto floating = call.process.api_float_arguments();
		std::vector<uint8_t> bytes;
		const auto *bitmap = static_cast<const GLubyte *>(unpack_data(call, call[6], pixel_size(call.i32(0), call.i32(1), 1, GL_COLOR_INDEX, GL_BITMAP, false),
			bytes, true));
		glBitmap(call.i32(0), call.i32(1), scalar_argument<GLfloat>(call, floating, 2), scalar_argument<GLfloat>(call, floating, 3),
			scalar_argument<GLfloat>(call, floating, 4), scalar_argument<GLfloat>(call, floating, 5), bitmap);
	});
	add("glDrawPixels", 5, [](ApiCall &call) {
		std::vector<uint8_t> bytes;
		glDrawPixels(call.i32(0), call.i32(1), call.u32(2), call.u32(3),
			unpack_data(call, call[4], pixel_size(call.i32(0), call.i32(1), 1, call.u32(2), call.u32(3), false), bytes));
	});

	// Textures.
	add("glTexImage1D", 8, [](ApiCall &call) {
		const bool proxy = call.u32(0) == GL_PROXY_TEXTURE_1D;
		std::vector<uint8_t> bytes;
		const auto *data = proxy ? nullptr : unpack_data(call, call[7], pixel_size(call.i32(3), 1, 1, call.u32(5), call.u32(6), false), bytes, true);
		glTexImage1D(call.u32(0), call.i32(1), call.i32(2), call.i32(3), call.i32(4), call.u32(5), call.u32(6), data);
	});
	add("glTexSubImage1D", 7, [](ApiCall &call) {
		std::vector<uint8_t> bytes;
		glTexSubImage1D(call.u32(0), call.i32(1), call.i32(2), call.i32(3), call.u32(4), call.u32(5),
			unpack_data(call, call[6], pixel_size(call.i32(3), 1, 1, call.u32(4), call.u32(5), false), bytes));
	});
	add("glTexSubImage3D", 11, [](ApiCall &call) {
		std::vector<uint8_t> bytes;
		glTexSubImage3D(call.u32(0), call.i32(1), call.i32(2), call.i32(3), call.i32(4), call.i32(5), call.i32(6), call.i32(7), call.u32(8),
			call.u32(9), unpack_data(call, call[10], pixel_size(call.i32(5), call.i32(6), call.i32(7), call.u32(8), call.u32(9), false), bytes));
	});
	// Compressed images carry their size.
	const auto compressed = [](ApiCall &call, size_t size_index, bool allocates, std::vector<uint8_t> &bytes) {
		if (call.i32(size_index) < 0 || call.u32(size_index) > (256U << 20)) throw std::runtime_error("OpenGL compressed image exceeds bounds");
		return unpack_data(call, call[size_index + 1], call.u32(size_index), bytes, allocates);
	};
	add("glCompressedTexImage1D", 7, [compressed](ApiCall &call) {
		std::vector<uint8_t> bytes;
		glCompressedTexImage1D(call.u32(0), call.i32(1), call.u32(2), call.i32(3), call.i32(4), call.i32(5), compressed(call, 5, true, bytes));
	});
	add("glCompressedTexImage2D", 8, [compressed](ApiCall &call) {
		std::vector<uint8_t> bytes;
		glCompressedTexImage2D(call.u32(0), call.i32(1), call.u32(2), call.i32(3), call.i32(4), call.i32(5), call.i32(6),
			compressed(call, 6, true, bytes));
	});
	add("glCompressedTexImage3D", 9, [compressed](ApiCall &call) {
		std::vector<uint8_t> bytes;
		glCompressedTexImage3D(call.u32(0), call.i32(1), call.u32(2), call.i32(3), call.i32(4), call.i32(5), call.i32(6), call.i32(7),
			compressed(call, 7, true, bytes));
	});
	add("glCompressedTexSubImage1D", 7, [compressed](ApiCall &call) {
		std::vector<uint8_t> bytes;
		glCompressedTexSubImage1D(call.u32(0), call.i32(1), call.i32(2), call.i32(3), call.u32(4), call.i32(5), compressed(call, 5, false, bytes));
	});
	add("glCompressedTexSubImage2D", 9, [compressed](ApiCall &call) {
		std::vector<uint8_t> bytes;
		glCompressedTexSubImage2D(call.u32(0), call.i32(1), call.i32(2), call.i32(3), call.i32(4), call.i32(5), call.u32(6), call.i32(7),
			compressed(call, 7, false, bytes));
	});
	add("glCompressedTexSubImage3D", 11, [compressed](ApiCall &call) {
		std::vector<uint8_t> bytes;
		glCompressedTexSubImage3D(call.u32(0), call.i32(1), call.i32(2), call.i32(3), call.i32(4), call.i32(5), call.i32(6), call.i32(7),
			call.u32(8), call.i32(9), compressed(call, 9, false, bytes));
	});
	add("glGetTexImage", 5, [](ApiCall &call) {
		GLint width = 0, height = 0, depth = 0;
		glGetTexLevelParameteriv(call.u32(0), call.i32(1), GL_TEXTURE_WIDTH, &width);
		glGetTexLevelParameteriv(call.u32(0), call.i32(1), GL_TEXTURE_HEIGHT, &height);
		glGetTexLevelParameteriv(call.u32(0), call.i32(1), GL_TEXTURE_DEPTH, &depth);
		pack_output(call, call[4], pixel_size(width, height, depth, call.u32(2), call.u32(3), true),
			[&](void *pixels) { glGetTexImage(call.u32(0), call.i32(1), call.u32(2), call.u32(3), pixels); });
	});
	add("glGetCompressedTexImage", 3, [](ApiCall &call) {
		GLint size = 0;
		glGetTexLevelParameteriv(call.u32(0), call.i32(1), GL_TEXTURE_COMPRESSED_IMAGE_SIZE, &size);
		if (size < 0) throw std::runtime_error("OpenGL compressed image exceeds bounds");
		pack_output(call, call[2], size_t(size), [&](void *pixels) { glGetCompressedTexImage(call.u32(0), call.i32(1), pixels); });
	});

	// Buffers.
	add("glBufferSubData", 4, [](ApiCall &call) {
		if (call[2] > (256ULL << 20)) throw std::runtime_error("OpenGL buffer upload exceeds bounds");
		const auto bytes = call.read(call[3], size_t(call[2]));
		glBufferSubData(call.u32(0), GLintptr(call[1]), GLsizeiptr(call[2]), bytes.data());
	});
	add("glGetBufferSubData", 4, [](ApiCall &call) {
		if (call[2] > (256ULL << 20)) throw std::runtime_error("OpenGL buffer read-back exceeds bounds");
		output<uint8_t>(call, call[3], size_t(call[2]), [&](uint8_t *bytes) { glGetBufferSubData(call.u32(0), GLintptr(call[1]), GLsizeiptr(call[2]), bytes); });
	});
	// What the driver says of a mapping is said of the guest's.
	const auto mapping_value = [&s](ApiCall &call, GLenum target, GLenum name, auto &value) {
		const auto found = s.mappings.find({s.selected(call).context, bound_buffer(buffer_binding(target))});
		if (found == s.mappings.end()) return;
		if (name == GL_BUFFER_MAPPED) value = GL_TRUE;
		else if (name == GL_BUFFER_MAP_OFFSET) value = decltype(+value)(found->second.offset);
		else if (name == GL_BUFFER_MAP_LENGTH) value = decltype(+value)(found->second.length);
		else if (name == GL_BUFFER_ACCESS_FLAGS) value = decltype(+value)(found->second.access);
		else if (name == GL_BUFFER_ACCESS) {
			const auto access = found->second.access & (GL_MAP_READ_BIT | GL_MAP_WRITE_BIT);
			value = decltype(+value)(access == GL_MAP_READ_BIT ? GL_READ_ONLY : access == GL_MAP_WRITE_BIT ? GL_WRITE_ONLY : GL_READ_WRITE);
		}
	};
	add("glGetBufferParameteriv", 3, [mapping_value](ApiCall &call) {
		GLint value = 0;
		call.store(call[2], call.load<GLint>(call[2]));
		glGetBufferParameteriv(call.u32(0), call.u32(1), &value);
		mapping_value(call, call.u32(0), call.u32(1), value);
		call.store(call[2], value);
	});
	add("glGetBufferParameteri64v", 3, [mapping_value](ApiCall &call) {
		GLint64 value = 0;
		call.store(call[2], call.load<GLint64>(call[2]));
		glGetBufferParameteri64v(call.u32(0), call.u32(1), &value);
		mapping_value(call, call.u32(0), call.u32(1), value);
		call.store(call[2], value);
	});
	add("glGetBufferPointerv", 3, [&s](ApiCall &call) {
		const auto found = s.mappings.find({s.selected(call).context, bound_buffer(buffer_binding(call.u32(0)))});
		call.store_pointer(call[2], found != s.mappings.end() && call.u32(1) == GL_BUFFER_MAP_POINTER ? found->second.address : 0);
	});
	const auto map_range = [&s](ApiCall &call, GLenum target, uint64_t offset, uint64_t length, GLbitfield access) -> uint64_t {
		const auto buffer = bound_buffer(buffer_binding(target));
		GLint64 size = 0;
		if (buffer) glGetBufferParameteri64v(target, GL_BUFFER_SIZE, &size);
		const std::pair key{s.selected(call).context, buffer};
		const bool valid = buffer && size >= 0 && length && offset <= uint64_t(size) && length <= uint64_t(size) - offset &&
			(access & (GL_MAP_READ_BIT | GL_MAP_WRITE_BIT)) && !s.mappings.contains(key) && length <= (256ULL << 20);
		if (!valid) {
			// The driver raises the error a refused mapping raises; a mapping it grants anyway is undone at once.
			if (glMapBufferRange(target, GLintptr(offset), GLsizeiptr(length), access)) glUnmapBuffer(target);
			return 0;
		}
		const auto address = s.process.map_memory(size_t((length + 4095) & ~uint64_t(4095)), PROT_READ | PROT_WRITE);
		if (!(access & (GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT))) {
			std::vector<uint8_t> bytes(length);
			glGetBufferSubData(target, GLintptr(offset), GLsizeiptr(length), bytes.data());
			s.process.write_memory(address, bytes);
		}
		s.mappings.emplace(key, State::Mapping{address, offset, length, access});
		return address;
	};
	add("glMapBufferRange", 4, [map_range](ApiCall &call) { call.ret(map_range(call, call.u32(0), call[1], call[2], call.u32(3))); });
	add("glMapBuffer", 2, [map_range](ApiCall &call) {
		GLint64 size = 0;
		if (bound_buffer(buffer_binding(call.u32(0)))) glGetBufferParameteri64v(call.u32(0), GL_BUFFER_SIZE, &size);
		const auto access = call.u32(1) == GL_READ_ONLY ? GL_MAP_READ_BIT : call.u32(1) == GL_WRITE_ONLY ? GL_MAP_WRITE_BIT :
			call.u32(1) == GL_READ_WRITE ? GL_MAP_READ_BIT | GL_MAP_WRITE_BIT : 0;
		call.ret(map_range(call, call.u32(0), 0, uint64_t(std::max<GLint64>(size, 0)), access));
	});
	add("glFlushMappedBufferRange", 3, [&s](ApiCall &call) {
		const auto found = s.mappings.find({s.selected(call).context, bound_buffer(buffer_binding(call.u32(0)))});
		if (found == s.mappings.end() || !(found->second.access & GL_MAP_FLUSH_EXPLICIT_BIT) || call[1] > found->second.length ||
			call[2] > found->second.length - call[1]) {
			glFlushMappedBufferRange(call.u32(0), GLintptr(call[1]), GLsizeiptr(call[2]));
			return;
		}
		const auto bytes = call.read(found->second.address + call[1], size_t(call[2]));
		glBufferSubData(call.u32(0), GLintptr(found->second.offset + call[1]), GLsizeiptr(call[2]), bytes.data());
	});
	add("glUnmapBuffer", 1, [&s](ApiCall &call) {
		const auto found = s.mappings.find({s.selected(call).context, bound_buffer(buffer_binding(call.u32(0)))});
		if (found == s.mappings.end()) { call.ret(glUnmapBuffer(call.u32(0))); return; }
		const auto mapping = found->second;
		s.mappings.erase(found);
		if ((mapping.access & GL_MAP_WRITE_BIT) && !(mapping.access & GL_MAP_FLUSH_EXPLICIT_BIT)) {
			const auto bytes = s.process.read_memory(mapping.address, size_t(mapping.length));
			glBufferSubData(call.u32(0), GLintptr(mapping.offset), GLsizeiptr(mapping.length), bytes.data());
		}
		s.process.unmap_memory(mapping.address);
		call.ret(GL_TRUE);
	});

	// Queries: a query buffer bound takes the result at an offset instead of client memory.
	const auto query_object = [&add]<typename T>(const char *name, void (*function)(GLuint, GLenum, T *)) {
		add(name, 3, [function](ApiCall &call) {
			if (bound_buffer(GL_QUERY_BUFFER_BINDING)) function(call.u32(0), call.u32(1), reinterpret_cast<T *>(call[2]));
			else output<T>(call, call[2], 1, [&](T *value) { function(call.u32(0), call.u32(1), value); });
		});
	};
	query_object("glGetQueryObjectiv", glGetQueryObjectiv);
	query_object("glGetQueryObjectuiv", glGetQueryObjectuiv);
	query_object("glGetQueryObjecti64v", glGetQueryObjecti64v);
	query_object("glGetQueryObjectui64v", glGetQueryObjectui64v);

	// Sync objects.
	add("glFenceSync", 2, [&s](ApiCall &call) {
		const auto sync = glFenceSync(call.u32(0), call.u32(1));
		if (!sync) return;
		s.syncs.emplace(s.next_sync, sync);
		call.ret(s.next_sync++);
	});
	add("glDeleteSync", 1, [&s](ApiCall &call) {
		const auto found = s.syncs.find(call[0]);
		// A token Raven never gave out names no sync object, and deleting it changes nothing.
		if (found == s.syncs.end()) return;
		glDeleteSync(found->second);
		s.syncs.erase(found);
	});
	add("glIsSync", 1, [&s](ApiCall &call) {
		const auto found = s.syncs.find(call[0]);
		call.ret(found != s.syncs.end() && glIsSync(found->second));
	});
	add("glClientWaitSync", 3, [&s](ApiCall &call) {
		const auto found = s.syncs.find(call[0]);
		call.ret(found == s.syncs.end() ? GL_WAIT_FAILED : glClientWaitSync(found->second, call.u32(1), call[2]));
	});
	add("glWaitSync", 3, [&s](ApiCall &call) {
		const auto found = s.syncs.find(call[0]);
		if (found != s.syncs.end()) glWaitSync(found->second, call.u32(1), call[2]);
	});
	add("glGetSynciv", 5, [&s](ApiCall &call) {
		const auto found = s.syncs.find(call[0]);
		if (call.i32(2) < 0 || call.u32(2) > 64) throw std::runtime_error("OpenGL sync query exceeds bounds");
		if (found == s.syncs.end()) return;
		GLsizei length = 0;
		output<GLint>(call, call[4], call.u32(2), [&](GLint *values) { glGetSynciv(found->second, call.u32(1), call.i32(2), &length, values); });
		if (call[3]) call.store(call[3], length);
	});

	// Feedback and selection: the driver fills a host buffer while the render mode lasts, and the guest's gets it when the mode ends.
	add("glFeedbackBuffer", 3, [&s](ApiCall &call) {
		if (call.i32(0) < 0 || call.u32(0) > 16 * 1024 * 1024) throw std::runtime_error("OpenGL feedback buffer exceeds bounds");
		auto &capture = s.contexts.at(s.selected(call).context).feedback;
		// The guest's buffer must be writable now, not only when the mode ends.
		auto bytes = call.read(call[2], size_t(call.u32(0)) * sizeof(GLfloat));
		call.write(call[2], bytes);
		capture = {call[2], std::move(bytes)};
		glFeedbackBuffer(call.i32(0), call.u32(1), reinterpret_cast<GLfloat *>(capture.bytes.data()));
	});
	add("glSelectBuffer", 2, [&s](ApiCall &call) {
		if (call.i32(0) < 0 || call.u32(0) > 16 * 1024 * 1024) throw std::runtime_error("OpenGL selection buffer exceeds bounds");
		auto &capture = s.contexts.at(s.selected(call).context).selection;
		// The guest's buffer must be writable now, not only when the mode ends.
		auto bytes = call.read(call[1], size_t(call.u32(0)) * sizeof(GLuint));
		call.write(call[1], bytes);
		capture = {call[1], std::move(bytes)};
		glSelectBuffer(call.i32(0), reinterpret_cast<GLuint *>(capture.bytes.data()));
	});
	// Leaving feedback or selection mode ends the capture: the values the driver wrote go to the guest's buffer.
	add("glRenderMode", 1, [&s](ApiCall &call) {
		GLint mode = 0;
		glGetIntegerv(GL_RENDER_MODE, &mode);
		const GLint result = glRenderMode(call.u32(0));
		auto &context = s.contexts.at(s.selected(call).context);
		auto &capture = mode == GL_FEEDBACK ? context.feedback : context.selection;
		// The capture started as a copy of the guest's buffer, so the values the driver left alone go back unchanged.
		if ((mode == GL_FEEDBACK || mode == GL_SELECT) && capture.address) call.write(capture.address, capture.bytes);
		call.ret(uint64_t(int64_t(result)));
	});
	// Errors raised before Raven's own queries are the guest's, and it reads them first.
	add("glGetError", 0, [&s](ApiCall &call) {
		auto &errors = s.contexts.at(s.selected(call).context).errors;
		if (errors.empty()) { call.ret(glGetError()); return; }
		call.ret(errors.front());
		errors.erase(errors.begin());
	});
}

}
