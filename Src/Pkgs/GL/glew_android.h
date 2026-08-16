#ifndef __GLEW_ANDROID_H__
#define __GLEW_ANDROID_H__

#ifdef ANDROID

#include <GLES3/gl3.h>
#include <GLES3/gl3ext.h>
#include <GLES2/gl2ext.h>

// Desktop GL compatibility definitions
#ifndef GLAPI
#define GLAPI extern
#endif

#ifndef GLAPIENTRY
#define GLAPIENTRY
#endif

#ifndef APIENTRY
#define APIENTRY GLAPIENTRY
#endif

// Some NDK versions might already define these in gl2ext.h
#ifndef GL_DOUBLE
typedef double GLdouble;
typedef double GLclampd;
#endif

// GLEW types
typedef GLchar GLEWchar;
typedef GLsizeiptr GLEWsizeiptr;
typedef GLintptr GLEWintptr;

// GLEW constants
#define GLEW_OK 0
#define GLEW_NO_ERROR 0
#define GLEW_ERROR_NO_GL_VERSION 1

// GLEW versioning
#define GLEW_VERSION_1_1 GL_TRUE
#define GLEW_VERSION_1_2 GL_TRUE
#define GLEW_VERSION_1_3 GL_TRUE
#define GLEW_VERSION_1_4 GL_TRUE
#define GLEW_VERSION_1_5 GL_TRUE
#define GLEW_VERSION_2_0 GL_TRUE
#define GLEW_VERSION_2_1 GL_TRUE
#define GLEW_VERSION_3_0 GL_TRUE
#define GLEW_VERSION_3_1 GL_TRUE
#define GLEW_VERSION_3_2 GL_TRUE
#define GLEW_VERSION_3_3 GL_TRUE

// Extensions (Stub them as true if they exist in GLES3)
#define GLEW_ARB_vertex_buffer_object GL_TRUE
#define GLEW_ARB_pixel_buffer_object GL_TRUE
#define GLEW_ARB_framebuffer_object GL_TRUE
#define GLEW_ARB_texture_non_power_of_two GL_TRUE

// Essential defines that might be missing or named differently
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif

// Function stubs/mappings
inline GLenum glewInit() { return GLEW_OK; }
inline const GLubyte* glewGetErrorString(GLenum error) { return (const GLubyte*)"No error"; }
inline GLboolean glewIsSupported(const char* name) { return GL_TRUE; }

// Missing Desktop GL constants used in Supermodel
#ifndef GL_TEXTURE_RECTANGLE
#define GL_TEXTURE_RECTANGLE 0x84F5
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif

// GLES specific mappings
#ifndef glClearDepth
#define glClearDepth glClearDepthf
#endif

// Geometry shader constants
#ifndef GL_GEOMETRY_SHADER
#define GL_GEOMETRY_SHADER 0x8DD9
#endif
#ifndef GL_LINES_ADJACENCY
#define GL_LINES_ADJACENCY 0x000A
#endif

// glDrawBuffer shim for GLES 3.0
inline void glDrawBuffer(GLenum mode) {
    GLenum bufs[1] = { mode };
    glDrawBuffers(1, bufs);
}

// GLES3 only supports float depth clear
#define glClearDepth glClearDepthf

// Add more as needed based on compiler errors

#endif // ANDROID

#endif // __GLEW_ANDROID_H__
