#include <Supermodel.h>
#include <Model3/IEmulator.h>
#include <Model3/Model3.h>
#include <libretro.h>
#include "LibretroTiming.h"
#include "ROMSet.h"
#include <Inputs/Input.h>
#include <GL/glew.h>

#ifndef LIBRETRO_WRAPPER_H
#define LIBRETRO_WRAPPER_H
#define EEPROM_SIZE   0x200
#define BACKUP_SIZE   0x20000
#define SAVE_RAM_SIZE (EEPROM_SIZE + BACKUP_SIZE)

Util::Config::Node DefaultConfig();

class LibretroWrapper
{
public:

    CInputs *Inputs = nullptr;
    // --- Libretro intermediate render target ---
    GLuint m_libretrFBO     = 0;
    GLuint m_libretrTex     = 0;
    GLuint m_libretrDepth   = 0;
    
    static std::string s_analysisPath;
    static std::string s_configFilePath;
    static std::string s_gameXMLFilePath;
    static std::string s_musicXMLFilePath;
    static std::string s_logFilePath;
    
    LibretroWrapper();
    virtual ~LibretroWrapper();

    unsigned getXRes() const { return xRes; }
    unsigned getYRes() const { return yRes; }
    unsigned getXOffset() const { return xOffset; }
    unsigned getYOffset() const { return yOffset; }
    unsigned getTotalXRes() const { return totalXRes; }
    unsigned getTotalYRes() const { return totalYRes; }
    int getAaValue() const { return aaValue; }
    SuperAA* getSuperAA() const { return superAA; }
    CRTcolor getCRTColors() const { return CRTcolors; }
    Game getGame() const { return game; }
    IEmulator* getEmulator() const { return Model3; }
    void Reset();   // hard reset of the emulated machine (retro_reset)
    FrameTimings GetTimings() const;
    float GetLastEngineMs() const { return lastEngineMs; }
    float GetLastAudioSubmitMs() const { return lastAudioSubmitMs; }
    double GetFramesPerSecond() const;
    std::shared_ptr<CInputSystem> getInputSystem() const { return m_inputSystem; }
    retro_hw_render_callback getHwRender() const { return hw_render; }
    static const std::string& GetGameXMLPath() { return s_gameXMLFilePath; }
    void SetWidescreen(bool enabled, bool wideBackground);
    void SetCrosshairs(unsigned mask);
    void setXRes(unsigned val) { xRes = val; }
    void setYRes(unsigned val) { yRes = val; }
    void setXOffset(unsigned val) { xOffset = val; }
    void setYOffset(unsigned val) { yOffset = val; }
    void setTotalXRes(unsigned val) { totalXRes = val; }
    void setTotalYRes(unsigned val) { totalYRes = val; }
    void setAaValue(int val) { aaValue = val; }
    void setSuperAA(SuperAA* val) { superAA = val; }
    void setCRTColors(CRTcolor val) { CRTcolors = val; }
    void setHwRender(retro_hw_render_callback val) { hw_render = val; }
    void InitializePaths(const std::string& systemPath);
    void UpdateScreenSize(unsigned viewWidth, unsigned viewHeight,
                          unsigned outputWidth, unsigned outputHeight);
    void SetSoundVolume(int volume);
    void SetMusicVolume(int volume);
    int Emulate(const char* romPath);
    void SetFullScreenRefreshRate();
    int Supermodel(const Game &game);
    void DestroyGLScreen();
    Result ConfigureInputs(CInputs *Inputs, Util::Config::Node *fileConfig, Util::Config::Node *runtimeConfig, const Game &game, bool configure);
    void PrintGLInfo(bool createScreen, bool infoLog, bool printExtensions);
    void SaveFrameBuffer(const std::string& file);
    void Screenshot();
    void TestPolygonHeaderBits(IEmulator *Emu);
    int SuperModelInit(const Game &game);
    void ShutDownSupermodel();
    bool InitRenderers();
    bool InitGL();
    // Returns the base-resolution frame resolved for Libretro submission.
    GLuint getSuperModelFBO() const;

private:
    float lastEngineMs = 0.0f;
    float lastAudioSubmitMs = 0.0f;
    uint64_t m_frameRateMicroHz = LibretroTiming::kDefaultFrameRateMicroHz;
    uint64_t m_audioFrameRemainder = 0;
    uint64_t m_lastFrameTime = 0;
    static const char* s_outputNames[];
    struct retro_hw_render_callback hw_render{};
    unsigned  xOffset, yOffset;                                         // offset of renderer output within OpenGL viewport
    unsigned  xRes, yRes;                                               // renderer output resolution (can be smaller than GL viewport)
    unsigned  totalXRes, totalYRes;                                     // total resolution (the whole GL viewport)
    int aaValue = 1;                                                    // default is 1 which is no aa
    CRTcolor CRTcolors;                                                 // default to no gamma/color adaption being done

    Game game;
    ROMSet rom_set;
    IEmulator *Model3 = nullptr;
    COutputs *Outputs = nullptr;
    SuperAA* superAA = nullptr;
    CRender2D *Render2D = nullptr;
    IRender3D *Render3D = nullptr;
    std::string initialState;
    uint64_t    prevFPSTicks;
    unsigned    fpsFramesElapsed;
    bool        gameHasLightguns;
    bool        quit;
    bool        paused;
    bool        dumpTimings;
    char baseTitleStr[128];
    char titleStr[128];
    UpscaleMode upscaleMode;
    uint64_t perfCountPerFrame;
    uint64_t nextTime;

    std::shared_ptr<CInputSystem> m_inputSystem;

};

#endif
