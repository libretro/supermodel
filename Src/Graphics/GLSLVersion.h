#ifndef _GLSL_VERSION_H_
#define _GLSL_VERSION_H_

#include <string>

namespace Graphics {

/**
 * @brief Helper class for GLSL version string generation
 * 
 * Centralizes shader version string generation for both desktop OpenGL and
 * OpenGL ES 3.0 (Android/RPi). Provides consistent precision qualifiers and
 * version headers across all shader compilation points.
 */
class GLSLVersion
{
public:
    enum class PrecisionFlags
    {
        FLOAT = 1 << 0,           // precision highp float;
        INT = 1 << 1,             // precision highp int;
        SAMPLER_UINT = 1 << 2,    // precision highp usampler2D;
        SAMPLER_INT = 1 << 3,     // precision highp isampler2D;
        ANDROID_DEFINE = 1 << 4   // #define ANDROID 1
    };

    /**
     * @brief Get GLSL version string for desktop or GLES
     * @param flags Combination of PrecisionFlags for additional precision qualifiers
     * @return GLSL version string (e.g., "#version 300 es\nprecision highp float;\n")
     */
    static std::string Get(int flags = (int)PrecisionFlags::FLOAT);

    /**
     * @brief Get GLSL version string for 2D rendering
     * Includes float and int precision for general 2D operations
     */
    static std::string Get2D();

    /**
     * @brief Get GLSL version string for 3D rendering
     * Includes float precision for standard 3D operations
     */
    static std::string Get3D();

    /**
     * @brief Get GLSL version string for post-processing (SuperAA, etc.)
     * Includes float precision for post-process shaders
     */
    static std::string GetPostProcess();

    /**
     * @brief Get GLSL version string for advanced 3D with sampler precision
     * Includes float and sampler precision for complex texture operations
     */
    static std::string Get3DAdvanced();

    /**
     * @brief Get GLSL version string for R3D shader with quad support
     * On desktop, supports both 410 core and 450 core based on quad rendering flag
     * On GLES, always returns 300 es with advanced precision
     */
    static std::string GetR3D(bool useQuads = false);

    /**
     * @brief Get GLSL version string for R3D fragment shaders on GLES.
     * Uses mediump float (16-bit) instead of highp for all color/texture/lighting
     * calculations — 2× throughput on mobile GPUs (VideoCore VII, Mali, etc.).
     * Vertex positions remain highp via GetR3D() on the vertex shader source.
     */
    static std::string GetR3DFragment(bool useQuads = false);

    /**
     * @brief Get GLSL version string for ImGui compatibility
     * Returns minimal version string for ImGui's OpenGL3 backend
     */
    static std::string GetImGui();

private:
    /**
     * @brief Check if running on GLES (Android or CORE_GLES defined)
     */
    static bool IsGLES();

    /**
     * @brief Build precision string from flags
     */
    static std::string BuildPrecisionString(int flags);
};

} // namespace Graphics

#endif
