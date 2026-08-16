#ifndef LIBRETRO_INPUT_SYSTEM_H
#define LIBRETRO_INPUT_SYSTEM_H

#include <Inputs/InputSystem.h>
#include "libretro.h"

class CLibretroInputSystem : public CInputSystem
{
public:
    CLibretroInputSystem();
    virtual ~CLibretroInputSystem() override;

    // Must match EXACTLY the =0 functions in InputSystem.h
    virtual bool InitializeSystem() override;
    virtual bool Poll() override;

    virtual int GetNumKeyboards() const override { return 1; }
    virtual int GetKeyIndex(const char *keyName) override;
    virtual const char *GetKeyName(int keyIndex) override;
    virtual bool IsKeyPressed(int kbdNum, int keyIndex) const override;
    virtual const KeyDetails *GetKeyDetails(int kbdNum) override;
    bool Initialize();

    virtual int GetNumMice() const override { return 7; }
    virtual int GetMouseAxisValue(int mseNum, int axisNum) const override;
    virtual int GetMouseWheelDir(int mseNum) const override;
    virtual bool IsMouseButPressed(int mseNum, int butNum) const override;
    virtual const MouseDetails *GetMouseDetails(int mseNum) override;
    virtual void SetMouseVisibility(bool visible) override;
    virtual int GetNumJoysticks() const override { return 2; }
    virtual int GetJoyAxisValue(int joyNum, int axisNum) const override;
    virtual bool IsJoyPOVInDir(int joyNum, int povNum, int povDir) const override;
    virtual bool IsJoyButPressed(int joyNum, int butNum) const override;
    virtual const JoyDetails *GetJoyDetails(int joyNum) override;
    void SetRumbleInterface(struct retro_rumble_interface interface) { m_rumbleInterface = interface; }
    void SetFFBEnabled(bool enabled) { m_ffbEnabled = enabled; }
    void StopAllRumble();
    // Signature match check: Supermodel usually uses the struct directly, 
    // but ensure your .cpp matches this exactly.
    virtual bool ProcessForceFeedbackCmd(int joyNum, int axisNum, ForceFeedbackCmd ffCmd) override;

private:
    int16_t m_joyButtons[2][NUM_JOY_BUTTONS];
    int16_t m_joyAxes[2][NUM_JOY_AXES];
    uint8_t m_joyPOV[2][4]; // Up, Down, Left, Right
    unsigned m_hGatePosition; // active diagonal: 0=center, 1-4=gear position
    bool m_keyState[512];  // Add this for keyboard
    
    // Supermodel mouse numbers:
    //   MOUSE/MOUSE2 = RetroMouse ports 1/2
    //   MOUSE3/MOUSE4 = RetroLightgun ports 1/2
    //   MOUSE5/MOUSE6 = virtual gun cursors for players 1/2
    //   MOUSE7 = virtual Star Wars analog joystick (stick or relative mouse)
    static constexpr int kMouseDeviceCount = 7;
    int16_t m_mouseAxes[kMouseDeviceCount][NUM_MOUSE_AXES];
    bool m_mouseButtons[kMouseDeviceCount][NUM_MOUSE_BUTTONS];
    int m_mouseWheelDir[kMouseDeviceCount];
    bool m_mouseIsAbsolute[kMouseDeviceCount];
    int m_mouseX[2];
    int m_mouseY[2];
    bool m_mousePositionInitialized[2];
    int m_gunCursorX[2];
    int m_gunCursorY[2];
    int16_t m_previousLightgunX[2];
    int16_t m_previousLightgunY[2];
    bool m_gunCursorInitialized[2];
    bool m_lightgunPositionInitialized[2];
    int m_analogJoystickCursorX;
    int m_analogJoystickCursorY;
    bool m_analogJoystickCursorInitialized;
    unsigned m_analogJoystickSource; // 0: stick, 1: mouse

    retro_rumble_interface m_rumbleInterface;
    bool m_ffbEnabled = false; // Guard flag
};

#endif
