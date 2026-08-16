#include <iostream>
#include <new>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <string>
#include <memory>
#include <vector>
#include <chrono>
#include <GL/glew.h>
#if !defined(ANDROID) && !defined(CORE_GLES)
#include <glsym/rglgen.h>
#endif
#include <libretro.h>
#include "Inputs/Inputs.h"

#ifdef SUPERMODEL_WIN32
#include "DirectInputSystem.h"
#include "WinOutputs.h"
#endif

#include "Supermodel.h"
#include "OSD/libretro/CoreOptionsTypes.h"
#include "Util/Format.h"
#include "Util/NewConfig.h"
#include "Util/ConfigBuilders.h"
#include "OSD/FileSystemPath.h"
#include "GameLoader.h"
#include "libretro_cbs.h"
#include "LibretroTiming.h"
#include "vfs_ioapi.h"
#include "Debugger/SupermodelDebugger.h"
#if !defined(ANDROID) && !defined(CORE_GLES) || defined(USE_LEGACY3D)
#include "Graphics/Legacy3D/Legacy3D.h"
#endif
#include "Graphics/New3D/New3D.h"
#include "Model3/IEmulator.h"
#include "Model3/Model3.h"
#include "OSD/Audio.h"
#include "Graphics/New3D/VBO.h"
#include "Graphics/SuperAA.h"
#include "Sound/MPEG/MpegAudio.h"
#include "Util/BMPFile.h"
#include "libretroCrosshair.h"
#include "LibretroWrapper.h"
#include "CLibretroInputSystem.h"
#include "CLibretroOutputSystem.h"
#include "libretroGui.h"
#include "LibretroConfigProvider.h"
#include "CoreOptionsTypes.h"

// --- External Audio Hooks ---
extern void PlayCallback(void *userdata, UINT8 *stream, int len);
extern retro_environment_t environ_cb;
extern retro_log_printf_t log_cb;  // defined in libretro.cpp

#ifdef ANDROID
#include <android/log.h>
#endif
#define LOG_TAG "SupermodelCore"

// --- RetroArch Logger Bridge ---
class CRetroArchLogger : public CLogger
{
public:
    void DebugLog(const char *fmt, va_list vl) override;
    void InfoLog(const char *fmt, va_list vl) override;
    void ErrorLog(const char *fmt, va_list vl) override;
};

void CRetroArchLogger::DebugLog(const char *fmt, va_list vl)
{
#ifndef DEBUG
    (void)fmt; (void)vl;
    return;
#endif
#ifdef ANDROID
    va_list vl_copy;
    va_copy(vl_copy, vl);
    __android_log_vprint(ANDROID_LOG_DEBUG, LOG_TAG, fmt, vl_copy);
    va_end(vl_copy);
#endif
    if (!log_cb) return;
    char buf[2048];
    vsnprintf(buf, sizeof(buf), fmt, vl);
    log_cb(RETRO_LOG_DEBUG, "%s", buf);
}

void CRetroArchLogger::InfoLog(const char *fmt, va_list vl)
{
#ifdef ANDROID
    va_list vl_copy;
    va_copy(vl_copy, vl);
    __android_log_vprint(ANDROID_LOG_INFO, LOG_TAG, fmt, vl_copy);
    va_end(vl_copy);
#endif
    if (!log_cb) return;
    char buf[2048];
    vsnprintf(buf, sizeof(buf), fmt, vl);
    log_cb(RETRO_LOG_INFO, "%s\n", buf);
}

void CRetroArchLogger::ErrorLog(const char *fmt, va_list vl)
{
#ifdef ANDROID
    va_list vl_copy;
    va_copy(vl_copy, vl);
    __android_log_vprint(ANDROID_LOG_ERROR, LOG_TAG, fmt, vl_copy);
    va_end(vl_copy);
#endif
    if (!log_cb) return;
    char buf[2048];
    vsnprintf(buf, sizeof(buf), fmt, vl);
    log_cb(RETRO_LOG_ERROR, "%s\n", buf);
}

/******************************************************************************
 Global Run-time Config
******************************************************************************/

std::string LibretroWrapper::s_analysisPath;
std::string LibretroWrapper::s_configFilePath;
std::string LibretroWrapper::s_gameXMLFilePath;
std::string LibretroWrapper::s_musicXMLFilePath;
std::string LibretroWrapper::s_logFilePath;

static Util::Config::Node s_runtime_config("Global");
static LibretroWrapper* g_ctx = nullptr;

/*
 * Crosshair stuff
 */
static CCrosshair* s_crosshair = nullptr;

#ifdef ANDROID
#define ALOG(...) __android_log_print(ANDROID_LOG_INFO, "SupermodelCore", __VA_ARGS__)
#else
#define ALOG(...)
#endif

LibretroWrapper::LibretroWrapper() :
    xOffset(0), yOffset(0), xRes(800), yRes(600),
    totalXRes(800), totalYRes(600), aaValue(0), CRTcolors(CRTcolor::None),
    upscaleMode(UpscaleMode::Bilinear)
{
      g_ctx = this;
}

LibretroWrapper::~LibretroWrapper() {}

double LibretroWrapper::GetFramesPerSecond() const
{
    return static_cast<double>(m_frameRateMicroHz) /
           static_cast<double>(LibretroTiming::kMicroHzScale);
}

FrameTimings LibretroWrapper::GetTimings() const
{
    if (Model3) return static_cast<CModel3*>(Model3)->GetTimings();
    return FrameTimings{};
}

static bool FileExists(const std::string& path)
{
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp)
        return false;
    fclose(fp);
    return true;
}

static std::string ResolveSystemAsset(const std::string& systemPath, const char* fileName)
{
    const std::string preferredPath = systemPath + "/" + fileName;
    if (FileExists(preferredPath))
        return preferredPath;

    // Compatibility with the directory layout used by the initial port.
    const std::string legacyPath = systemPath + "/Config/" + fileName;
    if (FileExists(legacyPath))
        return legacyPath;

    return preferredPath;
}

void LibretroWrapper::InitializePaths(const std::string& systemPath)
{
    s_configFilePath   = ResolveSystemAsset(systemPath, "Supermodel.ini");
    s_gameXMLFilePath  = ResolveSystemAsset(systemPath, "Games.xml");
    s_musicXMLFilePath = ResolveSystemAsset(systemPath, "Music.xml");
    s_logFilePath      = systemPath + "/Supermodel.log";
    s_analysisPath     = systemPath + "/Analysis/";

    std::cout << "[Supermodel] System assets: " << systemPath << std::endl;
}

[[maybe_unused]] static void GLAPIENTRY DebugCallback(GLenum /*source*/, GLenum /*type*/, GLuint id,
                                                      GLenum /*severity*/, GLsizei /*length*/,
                                                      const GLchar* message, const void* /*userParam*/)
{
    printf("OGLDebug:: 0x%X: %s\n", id, message);
}

void LibretroWrapper::UpdateScreenSize(unsigned viewWidth, unsigned viewHeight,
                                       unsigned outputWidth,
                                       unsigned outputHeight)
{
    // If dimensions match and renderers are initialized, skip costly re-init
    if (viewWidth == xRes && viewHeight == yRes &&
        outputWidth == totalXRes && outputHeight == totalYRes &&
        superAA != nullptr)
        return;

    xRes = std::min(viewWidth, outputWidth);
    yRes = std::min(viewHeight, outputHeight);
    totalXRes = outputWidth;
    totalYRes = outputHeight;
    xOffset = (totalXRes - xRes) / 2;
    yOffset = (totalYRes - yRes) / 2;

    if (Model3)
        Model3->PauseThreads();

    InitRenderers();

    if (Model3)
        Model3->ResumeThreads();
}

void LibretroWrapper::SaveFrameBuffer(const std::string& file)
{
    std::shared_ptr<uint8_t> pixels(new uint8_t[totalXRes * totalYRes * 4], std::default_delete<uint8_t[]>());
    glReadPixels(0, 0, totalXRes, totalYRes, GL_RGBA, GL_UNSIGNED_BYTE, pixels.get());
    Util::WriteSurfaceToBMP<Util::RGBA8>(file, pixels.get(), totalXRes, totalYRes, true);
}

void LibretroWrapper::Screenshot()
{
    time_t now = std::time(nullptr);
    tm* ltm = std::localtime(&now);
    std::string file = Util::Format() << FileSystemPath::GetPath(FileSystemPath::Screenshots)
        << "Screenshot_" << std::setfill('0') << std::setw(4) << (1900 + ltm->tm_year)
        << '-' << std::setw(2) << (1 + ltm->tm_mon)
        << '-' << std::setw(2) << ltm->tm_mday
        << "_(" << std::setw(2) << ltm->tm_hour
        << '-' << std::setw(2) << ltm->tm_min
        << '-' << std::setw(2) << ltm->tm_sec
        << ").bmp";

    std::cout << "Screenshot created: " << file << std::endl;
    this->SaveFrameBuffer(file);
}

/******************************************************************************
 Save States and NVRAM
******************************************************************************/

static const int STATE_FILE_VERSION = 6;  // keep in sync with standalone Supermodel
static unsigned s_saveSlot = 0;           // save state slot #

static void SaveState(IEmulator *Model3)
{
  CBlockFile  SaveState;
  std::string file_path = Util::Format() << FileSystemPath::GetPath(FileSystemPath::Saves) << Model3->GetGame().name << ".st" << s_saveSlot;
  
  if (Result::OKAY != SaveState.Create(file_path, "Supermodel Save State", "Supermodel Version " SUPERMODEL_VERSION))
  {
    ErrorLog("Unable to save state to '%s'.", file_path.c_str());
    return;
  }

  int32_t fileVersion = STATE_FILE_VERSION;
  SaveState.Write(&fileVersion, sizeof(fileVersion));
  SaveState.Write(Model3->GetGame().name);

  Model3->SaveState(&SaveState);
  SaveState.Close();
  InfoLog("Saved state to '%s'.", file_path.c_str());
}

static void LoadState(IEmulator *Model3, std::string file_path = std::string())
{
  CBlockFile  SaveState;

  if (file_path.empty())
    file_path = Util::Format() << FileSystemPath::GetPath(FileSystemPath::Saves) << Model3->GetGame().name << ".st" << s_saveSlot;

  if (Result::OKAY != SaveState.Load(file_path))
  {
    ErrorLog("Unable to load state from '%s'.", file_path.c_str());
    return;
  }

  if (Result::OKAY != SaveState.FindBlock("Supermodel Save State"))
  {
    ErrorLog("'%s' does not appear to be a valid save state file.", file_path.c_str());
    return;
  }

  int32_t fileVersion;
  SaveState.Read(&fileVersion, sizeof(fileVersion));
  if (fileVersion != STATE_FILE_VERSION)
  {
    ErrorLog("'%s' is incompatible with this version of Supermodel.", file_path.c_str());
    return;
  }

  Model3->LoadState(&SaveState);
  SaveState.Close();
  InfoLog("Loaded state from '%s'.", file_path.c_str());
}

/******************************************************************************
 Video Callbacks
******************************************************************************/

static CInputs *videoInputs = NULL;
static uint32_t currentInputs = 0;

bool BeginFrameVideo()
{
  return true;
}

void EndFrameVideo()
{
  // Show crosshairs for light gun games
  if (videoInputs && s_crosshair)
  {
    // Render2D/New3D restore framebuffer 0 after drawing to Supermodel's
    // off-screen target. In the standalone build that is the window
    // backbuffer, but in Libretro it is not the image later blitted to the
    // frontend. Rebind Supermodel's target so the crosshair becomes part of
    // the submitted frame.
    glBindFramebuffer(GL_FRAMEBUFFER, g_ctx->getSuperModelFBO());
    s_crosshair->Update(currentInputs, videoInputs, g_ctx->getXOffset(), g_ctx->getYOffset(), g_ctx->getXRes(), g_ctx->getYRes());
  }
}

/******************************************************************************
 Frame Timing & Init
******************************************************************************/
bool LibretroWrapper::InitRenderers()
{
    delete Render2D; Render2D = nullptr;
    delete Render3D; Render3D = nullptr;
    delete superAA;  superAA  = nullptr;

    // Destroy previous libretro-managed FBO if any
    if (m_libretrFBO) {
        glDeleteFramebuffers(1,  &m_libretrFBO);   m_libretrFBO   = 0;
        glDeleteTextures(1,      &m_libretrTex);    m_libretrTex   = 0;
        glDeleteRenderbuffers(1, &m_libretrDepth);  m_libretrDepth = 0;
    }

    superAA = new SuperAA(aaValue, CRTcolors);
    superAA->Init(totalXRes, totalYRes);

    GLuint renderTarget = superAA->GetTargetID();

    // Libretro always needs a base-resolution resolved target. With native
    // supersampling/CRT colour active, SuperAA renders its high-resolution
    // input down into this FBO. Without that pass, the Model 3 renderers draw
    // here directly. Never submit SuperAA's high-resolution source FBO to the
    // frontend: doing so crops the image to its lower-left base-size region.
    glGenFramebuffers(1, &m_libretrFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_libretrFBO);

    glGenTextures(1, &m_libretrTex);
    glBindTexture(GL_TEXTURE_2D, m_libretrTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                 totalXRes, totalYRes,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, m_libretrTex, 0);

    glGenRenderbuffers(1, &m_libretrDepth);
    glBindRenderbuffer(GL_RENDERBUFFER, m_libretrDepth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8,
                          totalXRes, totalYRes);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, m_libretrDepth);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
        ErrorLog("[Supermodel] Libretro FBO incomplete: 0x%X", status);
    else
        InfoLog("[Supermodel] Libretro resolved FBO created: %ux%u (id=%u)",
                totalXRes, totalYRes, m_libretrFBO);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (renderTarget != 0)
        superAA->SetOutputTarget(m_libretrFBO);
    else
        renderTarget = m_libretrFBO;

    Render2D = new CRender2D(s_runtime_config);
    // Normal desktop builds can include both renderers and select at runtime.
    // Platforms whose graphics API cannot build Legacy3D omit it entirely;
    // explicit RENDERER=legacy builds remain fixed to that renderer.
    Render3D =
#if defined(USE_LEGACY3D)
        (IRender3D*)new Legacy3D::CLegacy3D(s_runtime_config);
#elif defined(HAVE_LEGACY3D)
        s_runtime_config["New3DEngine"].ValueAs<bool>()
        ? (IRender3D*)new New3D::CNew3D(s_runtime_config, Model3->GetGame().name)
        : (IRender3D*)new Legacy3D::CLegacy3D(s_runtime_config);
#else
        (IRender3D*)new New3D::CNew3D(s_runtime_config, Model3->GetGame().name);
#endif

     unsigned render_xRes = xRes;
    unsigned render_xOffset = xOffset;

    if (Result::OKAY != Render2D->Init(
            render_xOffset*aaValue, yOffset*aaValue,
            render_xRes*aaValue, yRes*aaValue,
            totalXRes*aaValue, totalYRes*aaValue,
            renderTarget, upscaleMode))   // ← use renderTarget, not superAA->GetTargetID()
        return false;

    if (Result::OKAY != Render3D->Init(
            render_xOffset*aaValue, yOffset*aaValue,
            render_xRes*aaValue, yRes*aaValue,
            totalXRes*aaValue, totalYRes*aaValue,
            renderTarget))               // ← same here
        return false;

    // Initialize crosshair if it exists and hasn't been initialized yet
    if (s_crosshair)
    {
        if (s_crosshair->Init() != Result::OKAY)
        {
            InfoLog("[Supermodel] Crosshair initialization failed (Assets missing?), falling back to vector mode.");
        }
    }

    Model3->AttachRenderers(Render2D, Render3D, superAA);
    return true;
}

int LibretroWrapper::SuperModelInit(const Game &game) {

  initialState = s_runtime_config["InitStateFile"].ValueAs<std::string>();

  gameHasLightguns = false;
  quit = false;
  paused = false;
  dumpTimings = false;
  m_audioFrameRemainder = 0;

  // Initialize and load ROMs
  if (Result::OKAY != Model3->Init())
    return 1;
  if (Model3->LoadGame(game, rom_set) != Result::OKAY)
    return 1;
  rom_set = ROMSet();  

  MpegDec::LoadCustomTracks(s_musicXMLFilePath, game);

  totalXRes = xRes = s_runtime_config["XResolution"].ValueAs<unsigned>();
  totalYRes = yRes = s_runtime_config["YResolution"].ValueAs<unsigned>();
  snprintf(baseTitleStr, sizeof(baseTitleStr), "Supermodel - %s", game.title.c_str());

  SetAudioType(game.audio);
  if (Result::OKAY != OpenAudio(s_runtime_config))
    return 1;

  gameHasLightguns = !!(game.inputs &
    (Game::INPUT_GUN1 | Game::INPUT_GUN2 |
     Game::INPUT_ANALOG_GUN1 | Game::INPUT_ANALOG_GUN2));
  currentInputs = game.inputs;
  
  if (gameHasLightguns)
    videoInputs = Inputs;
  else
    videoInputs = nullptr;

  // --- FORCE FEEDBACK & OUTPUTS INITIALIZATION ---
  
  // 1. Initialize the concrete Output system if it hasn't been already
  if (Outputs == nullptr) {
      Outputs = new CLibretroOutputSystem();
  }

  // 2. Fetch the Rumble Interface from the Libretro environment
  // (Assuming you have access to the environment callback here)
  retro_rumble_interface rumble;
  if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumble)) {
      // We must cast the IInputSystem pointer to our specific Libretro implementation
      // to access the rumble setter.
      auto* libInput = static_cast<CLibretroInputSystem*>(m_inputSystem.get());
      if (libInput) {
          libInput->SetRumbleInterface(rumble);
          printf("[Supermodel] Libretro Rumble Interface initialized.\n");
      }
  }

  // 3. Attach everything to the Model 3 Core
  Model3->AttachInputs(Inputs);
  Model3->AttachOutputs(Outputs);
  CModel3* m3 = dynamic_cast<CModel3*>(Model3);
  if (m3 && log_cb)
    log_cb(RETRO_LOG_INFO, "[Supermodel] DriveBoard ptr=%p IsAttached=%d\n", 
           (void*)m3->GetDriveBoard(), m3->GetDriveBoard()->IsAttached() ? 1 : 0);
  
  Model3->Reset();

  if (!initialState.empty())
    LoadState(Model3, initialState);

  fpsFramesElapsed = 0;
  return 0;
}

int LibretroWrapper::Supermodel(const Game &game)
{
#ifndef SUPERMODEL_DEBUGGER
    (void)game;
#endif
    const auto engineStart = std::chrono::steady_clock::now();
    if (paused)
    {
        Model3->RenderFrame();
        lastEngineMs = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - engineStart).count();
        lastAudioSubmitMs = 0.0f;
    }
    else
    {
        Model3->RunFrame();

        const auto audioStart = std::chrono::steady_clock::now();
        lastEngineMs = std::chrono::duration<float, std::milli>(
            audioStart - engineStart).count();

        // Submit exactly 44.1 kHz over the selected video cadence. At 60 Hz
        // this is a fixed 735-frame packet; native Model 3 timing uses a
        // deterministic 766/767-frame sequence. Never derive the count from
        // measured performance: that would create a pacing feedback loop.
        const unsigned audio_frames = LibretroTiming::NextAudioFrames(
            m_frameRateMicroHz, m_audioFrameRemainder);
        PlayCallback(NULL, NULL,
                     static_cast<int>(audio_frames *
                         LibretroTiming::kStereoAudioBytesPerFrame));
        lastAudioSubmitMs = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - audioStart).count();
    }

    if (Inputs->uiExit->Pressed())
    {
      quit = true;
    }
    else if (Inputs->uiReset->Pressed())
    {
      Reset();
    }
    else if (Inputs->uiPause->Pressed())
    {
      paused = !paused;
      if (paused) Model3->PauseThreads();
      else        Model3->ResumeThreads();

      if (Outputs != NULL)
        Outputs->SetValue(OutputPause, paused);
    }
    else if (Inputs->uiSaveState->Pressed())
    {
      if (!paused) Model3->PauseThreads();
      SaveState(Model3);
      if (!paused) Model3->ResumeThreads();
    }
    else if (Inputs->uiChangeSlot->Pressed())
    {
      ++s_saveSlot;
      s_saveSlot %= 10;
      printf("Save slot: %d\n", s_saveSlot);
    }
    else if (Inputs->uiLoadState->Pressed())
    {
      if (!paused) Model3->PauseThreads();
      LoadState(Model3);
      if (!paused) Model3->ResumeThreads();
    }
    else if (Inputs->uiMusicVolUp->Pressed())
    {
      if (!Model3->GetGame().mpeg_board.empty()) {
        int vol = (std::min)(200, s_runtime_config["MusicVolume"].ValueAs<int>() + 10);
        s_runtime_config.Get("MusicVolume").SetValue(vol);
      }
    }
    else if (Inputs->uiMusicVolDown->Pressed())
    {
      if (!Model3->GetGame().mpeg_board.empty()) {
        int vol = (std::max)(0, s_runtime_config["MusicVolume"].ValueAs<int>() - 10);
        s_runtime_config.Get("MusicVolume").SetValue(vol);
      }
    }
    else if (Inputs->uiSoundVolUp->Pressed())
    {
      int vol = (std::min)(200, s_runtime_config["SoundVolume"].ValueAs<int>() + 10);
      s_runtime_config.Get("SoundVolume").SetValue(vol);
    }
    else if (Inputs->uiSoundVolDown->Pressed())
    {
      int vol = (std::max)(0, s_runtime_config["SoundVolume"].ValueAs<int>() - 10);
      s_runtime_config.Get("SoundVolume").SetValue(vol);
    }
#ifdef SUPERMODEL_DEBUGGER
    else if (Inputs->uiDumpInpState->Pressed())
    {
      Inputs->DumpState(&game);
    }
    else if (Inputs->uiDumpTimings->Pressed())
    {
      dumpTimings = !dumpTimings;
    }
#endif
    else if (Inputs->uiSelectCrosshairs->Pressed() && gameHasLightguns)
    {
      int crosshairs = (s_runtime_config["Crosshairs"].ValueAs<unsigned>() + 1) & 3;
      s_runtime_config.Get("Crosshairs").SetValue(crosshairs);
    }
    else if (Inputs->uiClearNVRAM->Pressed())
    {
      Model3->ClearNVRAM();
      puts("NVRAM cleared.");
    }
    else if (Inputs->uiToggleFrLimit->Pressed())
    {
      s_runtime_config.Get("Throttle").SetValue(!s_runtime_config["Throttle"].ValueAs<bool>());
    }
    else if (Inputs->uiScreenshot->Pressed())
    {
      Screenshot();
    }

   if (s_runtime_config["ShowFrameRate"].ValueAs<bool>())
    {
      fpsFramesElapsed += 1;
    }

    if (dumpTimings && !paused)
    {
      CModel3 *M = dynamic_cast<CModel3 *>(Model3);
      if (M) M->DumpTimings();
    }

  return 0;
}

void LibretroWrapper::ShutDownSupermodel()
{
  if (Model3)
    Model3->PauseThreads();

  // Stop rumble after the drive-board thread has paused, while the Libretro
  // input interface is still alive. Otherwise the thread can overwrite an
  // earlier stop command during teardown.
  auto libretroInput = std::static_pointer_cast<CLibretroInputSystem>(m_inputSystem);
  if (libretroInput)
    libretroInput->StopAllRumble();

  // NOTE: NVRAM is now saved by retro_unload_game() to the libretro buffer
  // Don't call SaveNVRAM() here - it would save to a file, which we don't want

  CloseAudio();

  delete Model3;
  Model3 = nullptr;

  delete Inputs;
  Inputs = nullptr;

  delete Outputs;
  Outputs = nullptr;

  m_inputSystem.reset();
  videoInputs = nullptr;
  currentInputs = 0;

  delete Render2D;
  Render2D = nullptr;
  delete Render3D;
  Render3D = nullptr;
  delete superAA;
  superAA = nullptr;

  if (m_libretrFBO)
  {
    glDeleteFramebuffers(1, &m_libretrFBO);
    m_libretrFBO = 0;
  }
  if (m_libretrTex)
  {
    glDeleteTextures(1, &m_libretrTex);
    m_libretrTex = 0;
  }
  if (m_libretrDepth)
  {
    glDeleteRenderbuffers(1, &m_libretrDepth);
    m_libretrDepth = 0;
  }

  delete s_crosshair;
  s_crosshair = nullptr;

  game = Game();
  rom_set = ROMSet();
}

/******************************************************************************
 Entry Point and Command Line Processing
******************************************************************************/

// Create and configure inputs
Result LibretroWrapper::ConfigureInputs(CInputs *Inputs, Util::Config::Node *fileConfig, Util::Config::Node *runtimeConfig, const Game &game, bool configure)
{
  (void)fileConfig;
  (void)game;

  Inputs->LoadFromConfig(*runtimeConfig);

  if (configure)
    ErrorLog("Interactive input configuration is not available in the Libretro core; use frontend remaps instead.");

  return Result::OKAY;
}

// Same sequence as the uiReset hotkey handler: the render/sound threads must be parked
// while the machine is reset, or they keep touching state that Reset() is tearing down.
void LibretroWrapper::Reset()
{
    if (!Model3)
        return;
    if (!paused) Model3->PauseThreads();
    Model3->Reset();
    if (!paused) Model3->ResumeThreads();
    InfoLog("Model 3 reset.");
}

int LibretroWrapper::Emulate(const char* romPath)
{
    // Route ALL of Supermodel's logging (InfoLog/DebugLog/ErrorLog, including the
    // DumpTimings profiler output) through the RetroArch log callback, on every
    // platform — not just Android. Previously non-Android builds used a file/console
    // logger via CreateLogger(), so InfoLog never reached the RetroArch frontend log
    // and the profiler lines were invisible.
    auto ra_logger = std::make_shared<CRetroArchLogger>();
    SetLogger(ra_logger);

    char* argv[] = { (char*)"supermodel", (char*)romPath };
    int argc = 2;
    auto cmd_line = LibretroConfigProvider::ParseCommandLine(argc, argv);
    if (cmd_line.error) return 1;

    InfoLog("Supermodel Version " SUPERMODEL_VERSION);
  
    bool rom_specified = !cmd_line.rom_files.empty();
    if (!rom_specified && !cmd_line.print_games && !cmd_line.config_inputs && !cmd_line.print_inputs)
    {
        ErrorLog("No ROM file specified.");
        return 0;
    }
    
    // Load and Merge Configuration
    Util::Config::Node fileConfig("Global");
    {
        Util::Config::Node fileConfigWithDefaults("Global");
        Util::Config::Node config3("Global");
        Util::Config::Node config4("Global");

        // A Supermodel.ini is an optional, advanced, read-only override. Normal
        // user configuration belongs to Libretro core options and frontend
        // input remaps; the core never creates or updates this file.
        if (FileExists(s_configFilePath))
        {
            if (Util::Config::FromINIFile(&fileConfig, s_configFilePath))
                return 1;
            InfoLog("Loaded optional configuration override: %s", s_configFilePath.c_str());
        }

        Util::Config::MergeINISections(&fileConfigWithDefaults, LibretroConfigProvider::DefaultConfig(s_gameXMLFilePath), fileConfig);
        Util::Config::MergeINISections(&config3, fileConfigWithDefaults, cmd_line.config);

	config3.Set("GameXMLFile", s_gameXMLFilePath);
        if (rom_specified || cmd_line.print_games)
        {
            std::string xml_file = config3["GameXMLFile"].ValueAs<std::string>();
            if (g_vfs_interface) {
                static zlib_filefunc64_def vfs_filefunc;
                fill_retro_vfs_filefunc64(&vfs_filefunc, g_vfs_interface);
                GameLoader::SetZipFilefunc(&vfs_filefunc);
            }
            GameLoader loader(xml_file);
            if (loader.Load(&game, &rom_set, *cmd_line.rom_files.begin()))
                return 1;
            Util::Config::MergeINISections(&config4, config3, fileConfig[game.name]);   
        }
        else
            config4 = config3;
            
        Util::Config::MergeINISections(&s_runtime_config, config4, cmd_line.config);

        // Libretro options are authoritative over both defaults and the optional INI.
        LibretroConfigProvider::ApplyCoreOptions(s_runtime_config);
        const bool native_timing =
            g_options.av_timing_mode == AVTimingMode::Native57524Hz;
        m_frameRateMicroHz = LibretroTiming::FrameRateMicroHz(native_timing);
        m_audioFrameRemainder = 0;
        InfoLog("Libretro timing: %.6f Hz video, %u Hz audio",
                GetFramesPerSecond(), LibretroTiming::kAudioSampleRate);
        InfoLog("Libretro emulation threading: MultiThreaded=%d, GPUMultiThreaded=%d",
                s_runtime_config["MultiThreaded"].ValueAs<bool>(),
                s_runtime_config["GPUMultiThreaded"].ValueAs<bool>());
        const LibretroInputProfiles::Profile *input_profile =
            LibretroConfigProvider::ApplyInputProfile(s_runtime_config, game);
        if (input_profile)
            InfoLog("Libretro control profile: %s", input_profile->name);
        else
            InfoLog("No exact Libretro control profile for input mask 0x%08X; using generic mappings.",
                    LibretroInputProfiles::NormalizeInputs(game.inputs));
    }

    if (Model3 || Inputs || Outputs)
    {
        ErrorLog("A Supermodel session is already active.");
        return 1;
    }

    aaValue   = s_runtime_config["Supersampling"].ValueAs<int>();
    CRTcolors = (CRTcolor)s_runtime_config["CRTcolors"].ValueAs<int>();   // 0 = None; was never read, left indeterminate
    upscaleMode = static_cast<UpscaleMode>(
        s_runtime_config["UpscaleMode"].ValueAs<int>());

    auto inputSystem = std::make_shared<CLibretroInputSystem>();
    auto inputs = std::make_unique<CInputs>(inputSystem);

    if (!inputs->Initialize())
    {
      ErrorLog("Failed to initialize input system.");
      return 1;
    }

    // Allocate crosshair object (Initialization deferred to InitRenderers)
    if (s_crosshair) delete s_crosshair;
    s_crosshair = new CCrosshair(s_runtime_config);

    auto model3 = std::make_unique<CModel3>(s_runtime_config);
    if (ConfigureInputs(inputs.get(), &fileConfig, &s_runtime_config, game, cmd_line.config_inputs) != Result::OKAY)
    {
        delete s_crosshair;
        s_crosshair = nullptr;
        return 1;
    }

    if (!rom_specified)
    {
        delete s_crosshair;
        s_crosshair = nullptr;
        return 1;
    }

    // Fire up Supermodel
    m_inputSystem = std::move(inputSystem);
    Model3 = model3.release();
    Inputs = inputs.release();
    return 0;
}

bool LibretroWrapper::InitGL()
{
    static bool glsym_done = false;
    if (!glsym_done)
    {
#if !defined(ANDROID) && !defined(CORE_GLES)
        rglgen_resolve_symbols(hw_render.get_proc_address);
#endif
        glsym_done = true;
    }

    // CRITICAL: Ensure internal textures (fonts, UI) are 1-byte aligned 
    // to match the fix in libretro.cpp. This prevents artifacts on legacy GL drivers.
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    glDisable(GL_DITHER);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);

    return InitRenderers();
}

GLuint LibretroWrapper::getSuperModelFBO() const
{
    return m_libretrFBO;
}

void LibretroWrapper::SetWidescreen(bool enabled, bool wideBackground)
{
    const bool currentWide =
        s_runtime_config["WideScreen"].ValueAsDefault<bool>(false);
    const bool currentWideBackground =
        s_runtime_config["WideBackground"].ValueAsDefault<bool>(false);
    if (currentWide == enabled && currentWideBackground == wideBackground)
        return;

    const auto setBool = [](const char *key, bool value)
    {
        try {
            s_runtime_config.Get(key).SetValue(value);
        }
        catch (const std::range_error&) {
            s_runtime_config.Add(key).SetValue(value);
        }
    };
    setBool("WideScreen", enabled);
    setBool("WideBackground", enabled && wideBackground);
}

void LibretroWrapper::SetCrosshairs(unsigned mask)
{
    mask &= 3u;
    try {
        s_runtime_config.Get("Crosshairs").SetValue(static_cast<int>(mask));
    }
    catch (const std::range_error&) {
        s_runtime_config.Add("Crosshairs").SetValue(static_cast<int>(mask));
    }

    InfoLog("[Supermodel] Crosshairs mask applied: %u", mask);
}

void LibretroWrapper::SetSoundVolume(int volume)
{
    s_runtime_config.Get("SoundVolume").SetValue(volume);
}

void LibretroWrapper::SetMusicVolume(int volume)
{
    s_runtime_config.Get("MusicVolume").SetValue(volume);
}
