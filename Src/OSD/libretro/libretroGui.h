#ifndef _LIBRETRO_GUI_H_
#define _LIBRETRO_GUI_H_

#include <string>
#include <vector>
#include <map>
#include "../../Util/NewConfig.h"
#include "../../Model3/Model3.h"
#include "GameLoader.h"

class CLibretroInputSystem;

// Satisfies the call in LibretroWrapper::Emulate
std::vector<std::string> RunGUI(const std::string& configPath, Util::Config::Node& config);

// The actual frame-render call to be placed in your retro_run
void Libretro_UpdateGUI(Util::Config::Node& config,
                        const std::map<std::string, Game>& games,
                        bool& menuOpen,
                        CLibretroInputSystem* inputSys);

void Libretro_ShutdownGUI();

struct LibretroFrontendTimings
{
    float engineMs = 0.0f;
    float audioSubmitMs = 0.0f;
    float overlayMs = 0.0f;
    float blitMs = 0.0f;
    float otherMs = 0.0f;
    float coreAndBlitMs = 0.0f;
    float presentMs = 0.0f;
    float retroRunMs = 0.0f;
    float worstRetroRunMs = 0.0f;
    float actualFps = 0.0f;
};

// In-game timing overlay: draw into Supermodel's framebuffer before the final
// blit so the frontend receives it as part of the submitted hardware frame.
void Libretro_InitOverlay(const char* glslVersion);
void Libretro_ShutdownOverlay();
void Libretro_DrawTimingOverlay(const FrameTimings& t,
                                const LibretroFrontendTimings& frontend,
                                int displayW, int displayH,
                                double framesPerSecond,
                                float gpuMs = 0.0f);

#endif
