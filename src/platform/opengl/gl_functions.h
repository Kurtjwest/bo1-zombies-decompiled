#pragma once
// The OpenGL 4.5 core functions the renderer uses (gl_rhi.cpp), loaded through SDL_GL_GetProcAddress once the first
// context exists. One list: the function pointers are declared, defined and loaded from it.

#include <GL/glcorearb.h>

#define BO1_GL_FUNCTIONS(X)                                                                                            \
    X(PFNGLGETSTRINGPROC, glGetString)                                                                                 \
    X(PFNGLGETSTRINGIPROC, glGetStringi)                                                                               \
    X(PFNGLGETINTEGERVPROC, glGetIntegerv)                                                                             \
    X(PFNGLGETINTEGER64VPROC, glGetInteger64v)                                                                         \
    X(PFNGLGETFLOATVPROC, glGetFloatv)                                                                                 \
    X(PFNGLGETERRORPROC, glGetError)                                                                                   \
    X(PFNGLENABLEPROC, glEnable)                                                                                       \
    X(PFNGLDISABLEPROC, glDisable)                                                                                     \
    X(PFNGLENABLEIPROC, glEnablei)                                                                                     \
    X(PFNGLDISABLEIPROC, glDisablei)                                                                                   \
    X(PFNGLFLUSHPROC, glFlush)                                                                                         \
    X(PFNGLFINISHPROC, glFinish)                                                                                       \
    X(PFNGLPIXELSTOREIPROC, glPixelStorei)                                                                             \
    X(PFNGLCLIPCONTROLPROC, glClipControl)                                                                             \
    X(PFNGLDEBUGMESSAGECALLBACKPROC, glDebugMessageCallback)                                                           \
    X(PFNGLDEBUGMESSAGECONTROLPROC, glDebugMessageControl)                                                             \
    X(PFNGLOBJECTLABELPROC, glObjectLabel)                                                                             \
    /* textures */                                                                                                     \
    X(PFNGLCREATETEXTURESPROC, glCreateTextures)                                                                       \
    X(PFNGLGENTEXTURESPROC, glGenTextures)                                                                             \
    X(PFNGLDELETETEXTURESPROC, glDeleteTextures)                                                                       \
    X(PFNGLTEXTURESTORAGE2DPROC, glTextureStorage2D)                                                                   \
    X(PFNGLTEXTURESTORAGE3DPROC, glTextureStorage3D)                                                                   \
    X(PFNGLTEXTURESTORAGE2DMULTISAMPLEPROC, glTextureStorage2DMultisample)                                             \
    X(PFNGLTEXTURESUBIMAGE2DPROC, glTextureSubImage2D)                                                                 \
    X(PFNGLTEXTURESUBIMAGE3DPROC, glTextureSubImage3D)                                                                 \
    X(PFNGLCOMPRESSEDTEXTURESUBIMAGE2DPROC, glCompressedTextureSubImage2D)                                             \
    X(PFNGLCOMPRESSEDTEXTURESUBIMAGE3DPROC, glCompressedTextureSubImage3D)                                             \
    X(PFNGLTEXTUREPARAMETERIPROC, glTextureParameteri)                                                                 \
    X(PFNGLTEXTUREPARAMETERIVPROC, glTextureParameteriv)                                                               \
    X(PFNGLTEXTUREVIEWPROC, glTextureView)                                                                             \
    X(PFNGLGETTEXTURESUBIMAGEPROC, glGetTextureSubImage)                                                               \
    X(PFNGLGETCOMPRESSEDTEXTURESUBIMAGEPROC, glGetCompressedTextureSubImage)                                           \
    X(PFNGLGENERATETEXTUREMIPMAPPROC, glGenerateTextureMipmap)                                                         \
    X(PFNGLCOPYIMAGESUBDATAPROC, glCopyImageSubData)                                                                   \
    X(PFNGLBINDTEXTUREUNITPROC, glBindTextureUnit)                                                                     \
    X(PFNGLGETINTERNALFORMATIVPROC, glGetInternalformativ)                                                             \
    X(PFNGLCLEARTEXSUBIMAGEPROC, glClearTexSubImage)                                                                   \
    /* buffers */                                                                                                      \
    X(PFNGLCREATEBUFFERSPROC, glCreateBuffers)                                                                         \
    X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers)                                                                         \
    X(PFNGLNAMEDBUFFERSTORAGEPROC, glNamedBufferStorage)                                                               \
    X(PFNGLNAMEDBUFFERSUBDATAPROC, glNamedBufferSubData)                                                               \
    X(PFNGLMAPNAMEDBUFFERRANGEPROC, glMapNamedBufferRange)                                                             \
    X(PFNGLUNMAPNAMEDBUFFERPROC, glUnmapNamedBuffer)                                                                   \
    X(PFNGLBINDBUFFERRANGEPROC, glBindBufferRange)                                                                     \
    /* framebuffers */                                                                                                 \
    X(PFNGLCREATEFRAMEBUFFERSPROC, glCreateFramebuffers)                                                               \
    X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers)                                                               \
    X(PFNGLNAMEDFRAMEBUFFERTEXTUREPROC, glNamedFramebufferTexture)                                                     \
    X(PFNGLNAMEDFRAMEBUFFERTEXTURELAYERPROC, glNamedFramebufferTextureLayer)                                           \
    X(PFNGLNAMEDFRAMEBUFFERDRAWBUFFERSPROC, glNamedFramebufferDrawBuffers)                                             \
    X(PFNGLNAMEDFRAMEBUFFERDRAWBUFFERPROC, glNamedFramebufferDrawBuffer)                                               \
    X(PFNGLNAMEDFRAMEBUFFERREADBUFFERPROC, glNamedFramebufferReadBuffer)                                               \
    X(PFNGLCHECKNAMEDFRAMEBUFFERSTATUSPROC, glCheckNamedFramebufferStatus)                                             \
    X(PFNGLNAMEDFRAMEBUFFERPARAMETERIPROC, glNamedFramebufferParameteri)                                               \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer)                                                                     \
    X(PFNGLBLITNAMEDFRAMEBUFFERPROC, glBlitNamedFramebuffer)                                                           \
    X(PFNGLCLEARNAMEDFRAMEBUFFERFVPROC, glClearNamedFramebufferfv)                                                     \
    X(PFNGLCLEARNAMEDFRAMEBUFFERFIPROC, glClearNamedFramebufferfi)                                                     \
    X(PFNGLCLEARNAMEDFRAMEBUFFERIVPROC, glClearNamedFramebufferiv)                                                     \
    /* shaders and programs */                                                                                         \
    X(PFNGLCREATESHADERPROC, glCreateShader)                                                                           \
    X(PFNGLSHADERSOURCEPROC, glShaderSource)                                                                           \
    X(PFNGLCOMPILESHADERPROC, glCompileShader)                                                                         \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv)                                                                             \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog)                                                                   \
    X(PFNGLDELETESHADERPROC, glDeleteShader)                                                                           \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram)                                                                         \
    X(PFNGLATTACHSHADERPROC, glAttachShader)                                                                           \
    X(PFNGLDETACHSHADERPROC, glDetachShader)                                                                           \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram)                                                                             \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv)                                                                           \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog)                                                                 \
    X(PFNGLDELETEPROGRAMPROC, glDeleteProgram)                                                                         \
    X(PFNGLUSEPROGRAMPROC, glUseProgram)                                                                               \
    X(PFNGLPROGRAMUNIFORM4FVPROC, glProgramUniform4fv)                                                                 \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation)                                                               \
    X(PFNGLGETACTIVEUNIFORMPROC, glGetActiveUniform)                                                                   \
    X(PFNGLGETUNIFORMIVPROC, glGetUniformiv)                                                                           \
    /* vertex input */                                                                                                 \
    X(PFNGLCREATEVERTEXARRAYSPROC, glCreateVertexArrays)                                                               \
    X(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays)                                                               \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray)                                                                     \
    X(PFNGLVERTEXARRAYATTRIBFORMATPROC, glVertexArrayAttribFormat)                                                     \
    X(PFNGLVERTEXARRAYATTRIBBINDINGPROC, glVertexArrayAttribBinding)                                                   \
    X(PFNGLVERTEXARRAYBINDINGDIVISORPROC, glVertexArrayBindingDivisor)                                                 \
    X(PFNGLENABLEVERTEXARRAYATTRIBPROC, glEnableVertexArrayAttrib)                                                     \
    X(PFNGLDISABLEVERTEXARRAYATTRIBPROC, glDisableVertexArrayAttrib)                                                   \
    X(PFNGLVERTEXARRAYVERTEXBUFFERPROC, glVertexArrayVertexBuffer)                                                     \
    X(PFNGLVERTEXARRAYELEMENTBUFFERPROC, glVertexArrayElementBuffer)                                                   \
    X(PFNGLVERTEXATTRIB4FPROC, glVertexAttrib4f)                                                                       \
    /* samplers */                                                                                                     \
    X(PFNGLCREATESAMPLERSPROC, glCreateSamplers)                                                                       \
    X(PFNGLDELETESAMPLERSPROC, glDeleteSamplers)                                                                       \
    X(PFNGLSAMPLERPARAMETERIPROC, glSamplerParameteri)                                                                 \
    X(PFNGLSAMPLERPARAMETERFPROC, glSamplerParameterf)                                                                 \
    X(PFNGLSAMPLERPARAMETERFVPROC, glSamplerParameterfv)                                                               \
    X(PFNGLBINDSAMPLERPROC, glBindSampler)                                                                             \
    /* fixed-function state */                                                                                         \
    X(PFNGLVIEWPORTINDEXEDFPROC, glViewportIndexedf)                                                                   \
    X(PFNGLDEPTHRANGEINDEXEDPROC, glDepthRangeIndexed)                                                                 \
    X(PFNGLSCISSORPROC, glScissor)                                                                                     \
    X(PFNGLCULLFACEPROC, glCullFace)                                                                                   \
    X(PFNGLFRONTFACEPROC, glFrontFace)                                                                                 \
    X(PFNGLPOLYGONMODEPROC, glPolygonMode)                                                                             \
    X(PFNGLPOLYGONOFFSETPROC, glPolygonOffset)                                                                         \
    X(PFNGLDEPTHFUNCPROC, glDepthFunc)                                                                                 \
    X(PFNGLDEPTHMASKPROC, glDepthMask)                                                                                 \
    X(PFNGLSTENCILFUNCSEPARATEPROC, glStencilFuncSeparate)                                                             \
    X(PFNGLSTENCILOPSEPARATEPROC, glStencilOpSeparate)                                                                 \
    X(PFNGLSTENCILMASKSEPARATEPROC, glStencilMaskSeparate)                                                             \
    X(PFNGLCOLORMASKIPROC, glColorMaski)                                                                               \
    X(PFNGLBLENDFUNCSEPARATEIPROC, glBlendFuncSeparatei)                                                               \
    X(PFNGLBLENDEQUATIONSEPARATEIPROC, glBlendEquationSeparatei)                                                       \
    X(PFNGLBLENDCOLORPROC, glBlendColor)                                                                               \
    X(PFNGLPROVOKINGVERTEXPROC, glProvokingVertex)                                                                     \
    /* draws */                                                                                                        \
    X(PFNGLDRAWARRAYSPROC, glDrawArrays)                                                                               \
    X(PFNGLDRAWELEMENTSBASEVERTEXPROC, glDrawElementsBaseVertex)                                                       \
    /* queries and synchronization */                                                                                  \
    X(PFNGLCREATEQUERIESPROC, glCreateQueries)                                                                         \
    X(PFNGLDELETEQUERIESPROC, glDeleteQueries)                                                                         \
    X(PFNGLBEGINQUERYPROC, glBeginQuery)                                                                               \
    X(PFNGLENDQUERYPROC, glEndQuery)                                                                                   \
    X(PFNGLGETQUERYOBJECTUIVPROC, glGetQueryObjectuiv)                                                                 \
    X(PFNGLGETQUERYOBJECTUI64VPROC, glGetQueryObjectui64v)                                                             \
    X(PFNGLFENCESYNCPROC, glFenceSync)                                                                                 \
    X(PFNGLCLIENTWAITSYNCPROC, glClientWaitSync)                                                                       \
    X(PFNGLWAITSYNCPROC, glWaitSync)                                                                                   \
    X(PFNGLDELETESYNCPROC, glDeleteSync)

namespace glfn
{
#define BO1_GL_DECLARE(type, name) extern type name;
BO1_GL_FUNCTIONS(BO1_GL_DECLARE)
#undef BO1_GL_DECLARE
// loads every function of the list (a context must be current); false names the first one missing
bool Load(void *(*getProc)(const char *name), const char **missing);
} // namespace glfn
