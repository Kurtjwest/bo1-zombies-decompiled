#pragma once
// The OpenGL functions the renderer uses (gl_rhi.cpp), loaded through SDL_GL_GetProcAddress once the first context
// exists: what OpenGL 3.1 with direct state access has, and the 4.x functions of the features it works around without.
// The function pointers are declared, defined and loaded from the lists.

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
    /* textures */                                                                                                     \
    X(PFNGLCREATETEXTURESPROC, glCreateTextures)                                                                       \
    X(PFNGLGENTEXTURESPROC, glGenTextures)                                                                             \
    X(PFNGLDELETETEXTURESPROC, glDeleteTextures)                                                                       \
    X(PFNGLTEXTURESTORAGE2DPROC, glTextureStorage2D)                                                                   \
    X(PFNGLTEXTURESTORAGE3DPROC, glTextureStorage3D)                                                                   \
    X(PFNGLTEXTURESUBIMAGE2DPROC, glTextureSubImage2D)                                                                 \
    X(PFNGLTEXTURESUBIMAGE3DPROC, glTextureSubImage3D)                                                                 \
    X(PFNGLCOMPRESSEDTEXTURESUBIMAGE2DPROC, glCompressedTextureSubImage2D)                                             \
    X(PFNGLCOMPRESSEDTEXTURESUBIMAGE3DPROC, glCompressedTextureSubImage3D)                                             \
    X(PFNGLTEXTUREPARAMETERIPROC, glTextureParameteri)                                                                 \
    X(PFNGLTEXTUREPARAMETERIVPROC, glTextureParameteriv)                                                               \
    X(PFNGLGETTEXTURESUBIMAGEPROC, glGetTextureSubImage)                                                               \
    X(PFNGLGETCOMPRESSEDTEXTURESUBIMAGEPROC, glGetCompressedTextureSubImage)                                           \
    X(PFNGLGENERATETEXTUREMIPMAPPROC, glGenerateTextureMipmap)                                                         \
    X(PFNGLBINDTEXTUREUNITPROC, glBindTextureUnit)                                                                     \
    /* buffers */                                                                                                      \
    X(PFNGLCREATEBUFFERSPROC, glCreateBuffers)                                                                         \
    X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers)                                                                         \
    X(PFNGLNAMEDBUFFERDATAPROC, glNamedBufferData)                                                                     \
    X(PFNGLNAMEDBUFFERSUBDATAPROC, glNamedBufferSubData)                                                               \
    X(PFNGLBINDBUFFERRANGEPROC, glBindBufferRange)                                                                     \
    /* framebuffers */                                                                                                 \
    X(PFNGLCREATEFRAMEBUFFERSPROC, glCreateFramebuffers)                                                               \
    X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers)                                                               \
    X(PFNGLNAMEDFRAMEBUFFERTEXTUREPROC, glNamedFramebufferTexture)                                                     \
    X(PFNGLNAMEDFRAMEBUFFERTEXTURELAYERPROC, glNamedFramebufferTextureLayer)                                           \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D)                                                           \
    X(PFNGLNAMEDFRAMEBUFFERDRAWBUFFERSPROC, glNamedFramebufferDrawBuffers)                                             \
    X(PFNGLNAMEDFRAMEBUFFERDRAWBUFFERPROC, glNamedFramebufferDrawBuffer)                                               \
    X(PFNGLNAMEDFRAMEBUFFERREADBUFFERPROC, glNamedFramebufferReadBuffer)                                               \
    X(PFNGLCHECKNAMEDFRAMEBUFFERSTATUSPROC, glCheckNamedFramebufferStatus)                                             \
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
    X(PFNGLVIEWPORTPROC, glViewport)                                                                                   \
    X(PFNGLDEPTHRANGEPROC, glDepthRange)                                                                               \
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

// OpenGL 4.x functions a 3.x driver may not have (the extensions are checked before they are called): loaded when
// there, null otherwise
#define BO1_GL_OPTIONAL_FUNCTIONS(X)                                                                                   \
    X(PFNGLCLIPCONTROLPROC, glClipControl)                                                                             \
    X(PFNGLDEBUGMESSAGECALLBACKPROC, glDebugMessageCallback)                                                           \
    X(PFNGLDEBUGMESSAGECONTROLPROC, glDebugMessageControl)                                                             \
    X(PFNGLOBJECTLABELPROC, glObjectLabel)                                                                             \
    X(PFNGLNAMEDBUFFERSTORAGEPROC, glNamedBufferStorage)                                                               \
    X(PFNGLMAPNAMEDBUFFERRANGEPROC, glMapNamedBufferRange)                                                             \
    X(PFNGLUNMAPNAMEDBUFFERPROC, glUnmapNamedBuffer)                                                                   \
    X(PFNGLNAMEDFRAMEBUFFERPARAMETERIPROC, glNamedFramebufferParameteri)                                               \
    X(PFNGLVIEWPORTINDEXEDFPROC, glViewportIndexedf)                                                                   \
    X(PFNGLDEPTHRANGEINDEXEDPROC, glDepthRangeIndexed)                                                                 \
    X(PFNGLPROVOKINGVERTEXPROC, glProvokingVertex)                                                                     \
    X(PFNGLTEXTUREVIEWPROC, glTextureView)                                                                             \
    X(PFNGLCOPYIMAGESUBDATAPROC, glCopyImageSubData)                                                                   \
    X(PFNGLCLEARTEXSUBIMAGEPROC, glClearTexSubImage)                                                                   \
    X(PFNGLTEXTURESTORAGE2DMULTISAMPLEPROC, glTextureStorage2DMultisample)                                             \
    X(PFNGLGETINTERNALFORMATIVPROC, glGetInternalformativ)

namespace glfn
{
#define BO1_GL_DECLARE(type, name) extern type name;
BO1_GL_FUNCTIONS(BO1_GL_DECLARE)
BO1_GL_OPTIONAL_FUNCTIONS(BO1_GL_DECLARE)
#undef BO1_GL_DECLARE
// loads the functions of both lists (a context must be current); false names the first one of the required list missing
bool Load(void *(*getProc)(const char *name), const char **missing);
} // namespace glfn
