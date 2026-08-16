#include "GLSLVersion.h"

namespace Graphics {

bool GLSLVersion::IsGLES()
{
#if defined(ANDROID) || defined(CORE_GLES) || defined(HAVE_GLES)
    return true;
#else
    return false;
#endif
}

std::string GLSLVersion::BuildPrecisionString(int flags)
{
    std::string precision;

    if (flags & (int)PrecisionFlags::FLOAT)
        precision += "precision highp float;\n";
    if (flags & (int)PrecisionFlags::INT)
        precision += "precision highp int;\n";
    if (flags & (int)PrecisionFlags::SAMPLER_UINT)
        precision += "precision highp usampler2D;\n";
    if (flags & (int)PrecisionFlags::SAMPLER_INT)
        precision += "precision highp isampler2D;\n";
    if (flags & (int)PrecisionFlags::ANDROID_DEFINE)
        precision += "#define ANDROID 1\n";

    return precision;
}

std::string GLSLVersion::Get(int flags)
{
    if (IsGLES()) {
        std::string version = "#version 300 es\n";
        version += BuildPrecisionString(flags);
        return version;
    } else {
        return "#version 410 core\n";
    }
}

std::string GLSLVersion::Get2D()
{
    // 2D rendering needs float and int precision
    return Get((int)PrecisionFlags::FLOAT | (int)PrecisionFlags::INT);
}

std::string GLSLVersion::Get3D()
{
    // Standard 3D rendering needs float precision
    return Get((int)PrecisionFlags::FLOAT);
}

std::string GLSLVersion::GetPostProcess()
{
    // Post-processing (SuperAA, etc.) needs float precision
    return Get((int)PrecisionFlags::FLOAT);
}

std::string GLSLVersion::Get3DAdvanced()
{
    // Advanced 3D (R3DShader) needs float and sampler precision
    return Get(
        (int)PrecisionFlags::FLOAT |
        (int)PrecisionFlags::SAMPLER_UINT |
        (int)PrecisionFlags::SAMPLER_INT |
        (int)PrecisionFlags::ANDROID_DEFINE
    );
}

std::string GLSLVersion::GetR3D(bool useQuads)
{
    if (IsGLES()) {
        // GLES always uses version 300 es with advanced precision
        return Get3DAdvanced();
    } else {
        // Desktop: use 450 core if quads, otherwise 410 core
        return useQuads ? "#version 450 core\n" : "#version 410 core\n";
    }
}

std::string GLSLVersion::GetR3DFragment(bool useQuads)
{
    if (IsGLES()) {
        // Fragment shaders use mediump float for 2× throughput on mobile GPUs.
        // Colors, textures and lighting are all fine with 16-bit precision.
        // Samplers and ints keep highp so data texture lookups stay accurate.
        return Get(
            (int)PrecisionFlags::INT |
            (int)PrecisionFlags::SAMPLER_UINT |
            (int)PrecisionFlags::SAMPLER_INT |
            (int)PrecisionFlags::ANDROID_DEFINE
            // NOTE: FLOAT is intentionally omitted → default mediump float
        ) + "precision mediump float;\n";
    } else {
        return useQuads ? "#version 450 core\n" : "#version 410 core\n";
    }
}

std::string GLSLVersion::GetImGui()
{
    if (IsGLES()) {
        return "#version 300 es";
    } else {
        return "#version 410";
    }
}

} // namespace Graphics
