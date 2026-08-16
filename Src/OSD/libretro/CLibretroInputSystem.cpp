#include <cstring>
#include <libretro.h>
#include "CLibretroInputSystem.h"
#include "CoreOptionsTypes.h"
#include <Inputs/Input.h>
#include <cmath>
#include <algorithm>

extern retro_input_poll_t input_poll_cb;
extern retro_input_state_t input_state_cb;
extern retro_log_printf_t log_cb;  // defined in libretro.cpp
extern bool g_cabinet_controls_enabled[2];
extern bool g_star_wars_invert_x;

CLibretroInputSystem::CLibretroInputSystem()
    : CInputSystem("Libretro")
{
    memset(m_joyButtons, 0, sizeof(m_joyButtons));
    memset(m_joyAxes,    0, sizeof(m_joyAxes));
    memset(m_joyPOV,     0, sizeof(m_joyPOV));
    m_hGatePosition = 0;
    memset(m_keyState,   0, sizeof(m_keyState));
    memset(m_mouseAxes,  0, sizeof(m_mouseAxes));
    memset(m_mouseButtons, 0, sizeof(m_mouseButtons));
    memset(m_mouseWheelDir, 0, sizeof(m_mouseWheelDir));
    memset(m_mouseIsAbsolute, 0, sizeof(m_mouseIsAbsolute));
    memset(m_mouseX, 0, sizeof(m_mouseX));
    memset(m_mouseY, 0, sizeof(m_mouseY));
    memset(m_mousePositionInitialized, 0, sizeof(m_mousePositionInitialized));
    memset(m_gunCursorX, 0, sizeof(m_gunCursorX));
    memset(m_gunCursorY, 0, sizeof(m_gunCursorY));
    memset(m_previousLightgunX, 0, sizeof(m_previousLightgunX));
    memset(m_previousLightgunY, 0, sizeof(m_previousLightgunY));
    memset(m_gunCursorInitialized, 0, sizeof(m_gunCursorInitialized));
    memset(m_lightgunPositionInitialized, 0,
           sizeof(m_lightgunPositionInitialized));
    m_analogJoystickCursorX = 0;
    m_analogJoystickCursorY = 0;
    m_analogJoystickCursorInitialized = false;
    m_analogJoystickSource = 0;
    memset(&m_rumbleInterface, 0, sizeof(m_rumbleInterface));
}

CLibretroInputSystem::~CLibretroInputSystem() {}

bool CLibretroInputSystem::Poll()
{
    if (!input_poll_cb || !input_state_cb)
        return false;

    input_poll_cb();

    // ----- Keyboard -----
    for (int k = 0; k < 512; k++)
        m_keyState[k] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, k);

    // Ensure we have some display dimensions
    if (m_dispW == 0) { m_dispW = 496; m_dispH = 384; }

    int mouseDeltaX[2] = {};
    int mouseDeltaY[2] = {};
    int16_t lightgunX[2] = {};
    int16_t lightgunY[2] = {};
    bool lightgunOffscreen[2] = {};
    bool lightgunPositionMoved[2] = {};

    // ----- RetroMouse ports (Supermodel MOUSE and MOUSE2) -----
    for (int port = 0; port < 2; ++port)
    {
        if (!m_mousePositionInitialized[port])
        {
            m_mouseX[port] = static_cast<int>(m_dispX + m_dispW / 2);
            m_mouseY[port] = static_cast<int>(m_dispY + m_dispH / 2);
            m_mousePositionInitialized[port] = true;
        }

        mouseDeltaX[port] = input_state_cb(
            port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
        mouseDeltaY[port] = input_state_cb(
            port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);
        m_mouseX[port] += mouseDeltaX[port];
        m_mouseY[port] += mouseDeltaY[port];

        const int min_x = static_cast<int>(m_dispX);
        const int min_y = static_cast<int>(m_dispY);
        const int max_x = min_x + static_cast<int>(m_dispW) - 1;
        const int max_y = min_y + static_cast<int>(m_dispH) - 1;
        m_mouseX[port] = std::clamp(m_mouseX[port], min_x, max_x);
        m_mouseY[port] = std::clamp(m_mouseY[port], min_y, max_y);

        m_mouseAxes[port][AXIS_X] = m_mouseX[port];
        m_mouseAxes[port][AXIS_Y] = m_mouseY[port];
        m_mouseWheelDir[port] = 0;
        if (input_state_cb(port, RETRO_DEVICE_MOUSE, 0,
                           RETRO_DEVICE_ID_MOUSE_WHEELUP))
            m_mouseWheelDir[port] = 1;
        else if (input_state_cb(port, RETRO_DEVICE_MOUSE, 0,
                                RETRO_DEVICE_ID_MOUSE_WHEELDOWN))
            m_mouseWheelDir[port] = -1;

        // CInputSystem mouse button order is Left, Middle, Right, X1, X2.
        m_mouseButtons[port][0] = input_state_cb(
            port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT);
        m_mouseButtons[port][1] = input_state_cb(
            port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_MIDDLE);
        m_mouseButtons[port][2] = input_state_cb(
            port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT);
        m_mouseButtons[port][3] = input_state_cb(
            port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_BUTTON_4);
        m_mouseButtons[port][4] = input_state_cb(
            port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_BUTTON_5);
        m_mouseIsAbsolute[port] = false;
    }

    // ----- RetroLightgun ports (Supermodel MOUSE3 and MOUSE4) -----
    for (int port = 0; port < 2; ++port)
    {
        const int dev = port + 2;
        // Use SCREEN_X/Y for absolute position (-32768 to 32767)
        lightgunX[port] = input_state_cb(
            port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X);
        lightgunY[port] = input_state_cb(
            port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y);
        lightgunOffscreen[port] = input_state_cb(
            port, RETRO_DEVICE_LIGHTGUN, 0,
            RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN);
        
        // Scale to display coordinates
        m_mouseAxes[dev][AXIS_X] = static_cast<int>(m_dispX) +
            static_cast<int>(((static_cast<float>(lightgunX[port]) + 32768.0f) / 65535.0f) *
                             static_cast<float>(m_dispW - 1));
        m_mouseAxes[dev][AXIS_Y] = static_cast<int>(m_dispY) +
            static_cast<int>(((static_cast<float>(lightgunY[port]) + 32768.0f) / 65535.0f) *
                             static_cast<float>(m_dispH - 1));
        m_mouseAxes[dev][AXIS_Z] = 0;
        m_mouseWheelDir[dev] = 0;

        m_mouseButtons[dev][0] = input_state_cb(
            port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_TRIGGER);
        m_mouseButtons[dev][1] = 0;
        // Ocean Hunter has two independent shot triggers. AUX_A is the
        // natural second trigger; Reload is also accepted because RetroArch
        // binds it to the second mouse button by default. IS_OFFSCREEN is a
        // position state, not a button: treating it as one leaves Shot 2 stuck.
        m_mouseButtons[dev][2] = input_state_cb(
            port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_AUX_A) ||
            input_state_cb(
            port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_RELOAD);
        m_mouseButtons[dev][3] = input_state_cb(
            port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_START);
        m_mouseButtons[dev][4] = input_state_cb(
            port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SELECT);
        m_mouseIsAbsolute[dev] = true;
    }

    const int16_t THRESHOLD = 8000;

    for (int joy = 0; joy < 2; joy++)
    {
        // ----- Initialize all buttons to 0 first -----
        for (int b = 0; b < NUM_JOY_BUTTONS; b++)
        {
            m_joyButtons[joy][b] = 0;
        }

        // ----- Axes -----
        int16_t x = input_state_cb(joy, RETRO_DEVICE_ANALOG,
                                   RETRO_DEVICE_INDEX_ANALOG_LEFT,
                                   RETRO_DEVICE_ID_ANALOG_X);

        int16_t y = input_state_cb(joy, RETRO_DEVICE_ANALOG,
                                   RETRO_DEVICE_INDEX_ANALOG_LEFT,
                                   RETRO_DEVICE_ID_ANALOG_Y);

        m_joyAxes[joy][AXIS_X] = x;
        m_joyAxes[joy][AXIS_Y] = y;

        // Right stick -> RX/RY, which the analog driving layout uses as a gate shifter
        // (JOY1_RYAXIS_NEG/POS = gears 1/2, JOY1_RXAXIS_NEG/POS = gears 3/4).
        m_joyAxes[joy][AXIS_RX] = input_state_cb(joy, RETRO_DEVICE_ANALOG,
                                                 RETRO_DEVICE_INDEX_ANALOG_RIGHT,
                                                 RETRO_DEVICE_ID_ANALOG_X);
        m_joyAxes[joy][AXIS_RY] = input_state_cb(joy, RETRO_DEVICE_ANALOG,
                                                 RETRO_DEVICE_INDEX_ANALOG_RIGHT,
                                                 RETRO_DEVICE_ID_ANALOG_Y);

        // Triggers as analog axes (0..32767). Frontends with only digital L2/R2 report 0 or
        // 32767 here, so this is safe either way. L2 -> ZAXIS, R2 -> RZAXIS, matching how
        // Supermodel names trigger axes elsewhere.
        m_joyAxes[joy][AXIS_Z]  = input_state_cb(joy, RETRO_DEVICE_ANALOG,
                                                 RETRO_DEVICE_INDEX_ANALOG_BUTTON,
                                                 RETRO_DEVICE_ID_JOYPAD_L2);
        m_joyAxes[joy][AXIS_RZ] = input_state_cb(joy, RETRO_DEVICE_ANALOG,
                                                 RETRO_DEVICE_INDEX_ANALOG_BUTTON,
                                                 RETRO_DEVICE_ID_JOYPAD_R2);

        // ----- D-Pad -----
        bool d_up    = input_state_cb(joy, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP);
        bool d_down  = input_state_cb(joy, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN);
        bool d_left  = input_state_cb(joy, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT);
        bool d_right = input_state_cb(joy, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT);

        // CInputSystem POV directions are ordered Up, Down, Left, Right.
        // Keep this array in the same order: IsJoyPOVInDir() indexes it with
        // the POV_* constants declared by Supermodel.
        m_joyPOV[joy][0] = d_up;    // Up
        m_joyPOV[joy][1] = d_down;  // Down
        m_joyPOV[joy][2] = d_left;  // Left
        m_joyPOV[joy][3] = d_right; // Right

        // ----- Buttons -----
        // Standard game buttons (0-3: B, A, Y, X)
        static const int game_buttons[4] =
        {
            RETRO_DEVICE_ID_JOYPAD_B,      // Button 0 (BUTTON1)
            RETRO_DEVICE_ID_JOYPAD_A,      // Button 1 (BUTTON2)
            RETRO_DEVICE_ID_JOYPAD_Y,      // Button 2 (BUTTON3)
            RETRO_DEVICE_ID_JOYPAD_X,      // Button 3 (BUTTON4)
        };

        for (int b = 0; b < 4; b++)
        {
            m_joyButtons[joy][b] =
                input_state_cb(joy, RETRO_DEVICE_JOYPAD, 0, game_buttons[b]);
        }

        // Shoulder/trigger buttons (4-7: L, R, L2, R2)
        m_joyButtons[joy][4] = input_state_cb(joy, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L);
        m_joyButtons[joy][5] = input_state_cb(joy, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R);
        m_joyButtons[joy][6] = input_state_cb(joy, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2);
        m_joyButtons[joy][7] = input_state_cb(joy, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2);

        // Coin/Start buttons (8-9)
        // Button 8: START button -> maps to InputStart1/InputStart2
        // Button 9: SELECT button -> maps to InputCoin1/InputCoin2
        m_joyButtons[joy][8] = input_state_cb(joy, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START);
        m_joyButtons[joy][9] = input_state_cb(joy, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT);

        // L3/R3 are reserved as remappable cabinet slots, but stay completely
        // inactive in the default detected profile. The user must explicitly
        // select the "+ Test/Service slots" device type for this port.
        m_joyButtons[joy][14] = g_cabinet_controls_enabled[joy]
            ? input_state_cb(joy, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3)
            : 0;
        m_joyButtons[joy][15] = g_cabinet_controls_enabled[joy]
            ? input_state_cb(joy, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3)
            : 0;
        
        // **FIX: Map D-Pad AND Analog Stick to buttons 10-13**
        // This makes both the analog stick and D-Pad work for movement!
        m_joyButtons[joy][10] = (y < -THRESHOLD) || d_up;      // Up - ANALOG + DIGITAL
        m_joyButtons[joy][11] = (y >  THRESHOLD) || d_down;    // Down - ANALOG + DIGITAL
        m_joyButtons[joy][12] = (x < -THRESHOLD) || d_left;    // Left - ANALOG + DIGITAL
        m_joyButtons[joy][13] = (x >  THRESHOLD) || d_right;   // Right - ANALOG + DIGITAL

    }

    // The four logical gear inputs are deliberately kept behind virtual
    // joystick buttons. RetroArch always exposes the same four remappable
    // right-stick half-axes; this decoder alone decides whether they select
    // gears directly or describe an H-gate position.
    constexpr unsigned kGearButtonFirst = 16; // JOY1_BUTTON17 -> array index 16
    constexpr int kStandardThreshold = 21845; // approximately 2/3 axis travel
    constexpr int kHGateEnterThreshold = 16000;
    constexpr int kHGateReleaseThreshold = 12000;
    const int rightX = m_joyAxes[0][AXIS_RX];
    const int rightY = m_joyAxes[0][AXIS_RY];

    if (g_options.four_speed_shifter == FourSpeedShifter::Standard)
    {
        m_hGatePosition = 0;
        m_joyButtons[0][kGearButtonFirst + 0] = rightY < -kStandardThreshold; // Gear 1: Up
        m_joyButtons[0][kGearButtonFirst + 1] = rightY >  kStandardThreshold; // Gear 2: Down
        m_joyButtons[0][kGearButtonFirst + 2] = rightX < -kStandardThreshold; // Gear 3: Left
        m_joyButtons[0][kGearButtonFirst + 3] = rightX >  kStandardThreshold; // Gear 4: Right
    }
    else
    {
        unsigned candidate = 0;
        if (rightX < -kHGateEnterThreshold)
        {
            if (rightY < -kHGateEnterThreshold) candidate = 1;      // Up/Left
            else if (rightY > kHGateEnterThreshold) candidate = 2; // Down/Left
        }
        else if (rightX > kHGateEnterThreshold)
        {
            if (rightY < -kHGateEnterThreshold) candidate = 3;      // Up/Right
            else if (rightY > kHGateEnterThreshold) candidate = 4; // Down/Right
        }

        // Retain the active diagonal inside a smaller release threshold. This
        // prevents noisy axes from repeatedly releasing and pressing the same
        // gear near a quadrant boundary, while returning to center still
        // releases all virtual buttons. CGearShift4Input retains the selected
        // gear after that release, as a physical shifter would.
        if (candidate == 0 && m_hGatePosition != 0)
        {
            const bool left = rightX < -kHGateReleaseThreshold;
            const bool right = rightX > kHGateReleaseThreshold;
            const bool up = rightY < -kHGateReleaseThreshold;
            const bool down = rightY > kHGateReleaseThreshold;
            const bool stillInPosition =
                (m_hGatePosition == 1 && left && up) ||
                (m_hGatePosition == 2 && left && down) ||
                (m_hGatePosition == 3 && right && up) ||
                (m_hGatePosition == 4 && right && down);
            if (stillInPosition)
                candidate = m_hGatePosition;
        }
        m_hGatePosition = candidate;
        for (unsigned gear = 1; gear <= 4; ++gear)
            m_joyButtons[0][kGearButtonFirst + gear - 1] =
                m_hGatePosition == gear;
    }

    // ----- Virtual gun cursors (Supermodel MOUSE5 and MOUSE6) -----
    // MOUSE5/MOUSE6 combine the sources selected by Gun Input Mode. Mouse +
    // Analog Stick is the Hybrid path with Lightgun input excluded. Dedicated
    // Mouse mode consumes only RetroMouse relative movement and buttons;
    // existing Lightgun and Hybrid behavior retains its raw-mouse fallback.
    const int minX = static_cast<int>(m_dispX);
    const int minY = static_cast<int>(m_dispY);
    const int maxX = minX + static_cast<int>(m_dispW) - 1;
    const int maxY = minY + static_cast<int>(m_dispH) - 1;
    const auto stickDelta = [](int16_t axis) -> int
    {
        constexpr int deadZone = 6000;
        constexpr int maxSpeed = 12;
        const int magnitude = std::abs(static_cast<int>(axis));
        if (magnitude <= deadZone)
            return 0;
        const float normalized = static_cast<float>(magnitude - deadZone) /
                                 static_cast<float>(32767 - deadZone);
        const int speed = 1 + static_cast<int>(std::lround(
            normalized * static_cast<float>(maxSpeed - 1)));
        return axis < 0 ? -speed : speed;
    };

    for (int port = 0; port < 2; ++port)
    {
        const int lightgunDev = port + 2;
        const int cursorDev = port + 4;
        const bool allowLightgun =
            g_options.gun_input == GunInput::Hybrid ||
            g_options.gun_input == GunInput::Lightgun;
        const bool allowMouse =
            g_options.gun_input == GunInput::Hybrid ||
            g_options.gun_input == GunInput::Lightgun ||
            g_options.gun_input == GunInput::MouseAnalog ||
            g_options.gun_input == GunInput::Mouse;
        const bool allowAnalog =
            g_options.gun_input == GunInput::Hybrid ||
            g_options.gun_input == GunInput::MouseAnalog ||
            g_options.gun_input == GunInput::AnalogSticks;
        if (!m_gunCursorInitialized[port])
        {
            m_gunCursorX[port] = minX + static_cast<int>(m_dispW) / 2;
            m_gunCursorY[port] = minY + static_cast<int>(m_dispH) / 2;
            m_gunCursorInitialized[port] = true;
        }

        const bool lightgunValid = allowLightgun &&
            !lightgunOffscreen[port] &&
            lightgunX[port] != INT16_MIN && lightgunY[port] != INT16_MIN;
        const bool lightgunMoved = lightgunValid &&
            (!m_lightgunPositionInitialized[port] ||
             lightgunX[port] != m_previousLightgunX[port] ||
             lightgunY[port] != m_previousLightgunY[port]);
        lightgunPositionMoved[port] = lightgunMoved;
        const bool lightgunAction = allowLightgun && lightgunValid &&
            (m_mouseButtons[lightgunDev][0] ||
             m_mouseButtons[lightgunDev][2] ||
             m_mouseButtons[lightgunDev][3] ||
             m_mouseButtons[lightgunDev][4]);
        const bool rawMouseMoved = allowMouse &&
            (mouseDeltaX[port] != 0 || mouseDeltaY[port] != 0);
        const int stickX = stickDelta(m_joyAxes[port][AXIS_X]);
        const int stickY = stickDelta(m_joyAxes[port][AXIS_Y]);

        if (g_options.gun_input == GunInput::Mouse)
        {
            if (rawMouseMoved)
            {
                m_gunCursorX[port] += mouseDeltaX[port];
                m_gunCursorY[port] += mouseDeltaY[port];
            }
        }
        else if (g_options.gun_input == GunInput::Lightgun)
        {
            if (lightgunValid)
            {
                m_gunCursorX[port] = m_mouseAxes[lightgunDev][AXIS_X];
                m_gunCursorY[port] = m_mouseAxes[lightgunDev][AXIS_Y];
            }
            else if (rawMouseMoved)
            {
                m_gunCursorX[port] += mouseDeltaX[port];
                m_gunCursorY[port] += mouseDeltaY[port];
            }
        }
        else if (lightgunMoved || lightgunAction)
        {
            m_gunCursorX[port] = m_mouseAxes[lightgunDev][AXIS_X];
            m_gunCursorY[port] = m_mouseAxes[lightgunDev][AXIS_Y];
        }
        else if (rawMouseMoved)
        {
            m_gunCursorX[port] += mouseDeltaX[port];
            m_gunCursorY[port] += mouseDeltaY[port];
        }
        else
        {
            if (allowAnalog)
            {
                m_gunCursorX[port] += stickX;
                m_gunCursorY[port] += stickY;
            }
        }

        m_gunCursorX[port] = std::clamp(m_gunCursorX[port], minX, maxX);
        m_gunCursorY[port] = std::clamp(m_gunCursorY[port], minY, maxY);
        m_mouseAxes[cursorDev][AXIS_X] = m_gunCursorX[port];
        m_mouseAxes[cursorDev][AXIS_Y] = m_gunCursorY[port];
        m_mouseAxes[cursorDev][AXIS_Z] = 0;
        m_mouseWheelDir[cursorDev] = 0;
        m_mouseIsAbsolute[cursorDev] = true;

        m_mouseButtons[cursorDev][0] =
            (allowLightgun && m_mouseButtons[lightgunDev][0]) ||
            (allowMouse && m_mouseButtons[port][0]) ||
            (allowAnalog && m_joyButtons[port][0]);
        m_mouseButtons[cursorDev][1] = false;
        m_mouseButtons[cursorDev][2] =
            (allowLightgun && m_mouseButtons[lightgunDev][2]) ||
            (allowMouse && m_mouseButtons[port][2]) ||
            (allowAnalog && m_joyButtons[port][1]);
        m_mouseButtons[cursorDev][3] =
            (allowLightgun && m_mouseButtons[lightgunDev][3]) ||
            (allowMouse && m_mouseButtons[port][3]);
        m_mouseButtons[cursorDev][4] =
            (allowLightgun && m_mouseButtons[lightgunDev][4]) ||
            (allowMouse && m_mouseButtons[port][4]);

        m_previousLightgunX[port] = lightgunX[port];
        m_previousLightgunY[port] = lightgunY[port];
        m_lightgunPositionInitialized[port] = true;
    }

    // ----- Star Wars Trilogy analog joystick (Supermodel MOUSE7) -----
    // Supermodel's traditional JOY1_X/Y,MOUSE_X/Y mapping is ambiguous in a
    // libretro core: the joystick is absolute, while RetroMouse supplies
    // relative deltas which we accumulate into a position. Once the mouse has
    // left centre, its retained position reappears whenever the stick returns
    // to neutral and makes the game's calibration centre unstable.
    //
    // Present one absolute virtual axis to Supermodel instead. Star Wars
    // Trilogy Input Mode selects the accepted sources and, in Hybrid mode, the last source
    // actually moved owns it. Mouse movement adjusts the current position;
    // moving the stick switches back to its absolute position, including its
    // stable centre when released. No values from the devices are mixed.
    constexpr int analogJoystickDev = 6;
    if (!m_analogJoystickCursorInitialized)
    {
        m_analogJoystickCursorX = minX + static_cast<int>(m_dispW) / 2;
        m_analogJoystickCursorY = minY + static_cast<int>(m_dispH) / 2;
        m_analogJoystickCursorInitialized = true;
    }

    const bool analogMouseMoved = mouseDeltaX[0] != 0 || mouseDeltaY[0] != 0;
    constexpr int analogSwitchDeadZone = 6000;
    const bool analogStickMoved =
        std::abs(static_cast<int>(m_joyAxes[0][AXIS_X])) > analogSwitchDeadZone ||
        std::abs(static_cast<int>(m_joyAxes[0][AXIS_Y])) > analogSwitchDeadZone;
    switch (g_options.star_wars_input)
    {
    case StarWarsInput::AnalogSticks:
        m_analogJoystickSource = 0;
        break;
    case StarWarsInput::Mouse:
        m_analogJoystickSource = 1;
        break;
    case StarWarsInput::Hybrid:
        if (analogMouseMoved)
            m_analogJoystickSource = 1;
        else if (analogStickMoved)
            m_analogJoystickSource = 0;
        break;
    }

    if (m_analogJoystickSource == 0)
    {
        const auto axisToDisplay = [](int16_t axis, int minimum,
                                      unsigned extent) -> int
        {
            if (extent <= 1)
                return minimum;
            const float normalized =
                (static_cast<float>(axis) + 32768.0f) / 65535.0f;
            return minimum + static_cast<int>(std::lround(
                normalized * static_cast<float>(extent - 1)));
        };
        m_analogJoystickCursorX = axisToDisplay(
            m_joyAxes[0][AXIS_X], minX, m_dispW);
        m_analogJoystickCursorY = axisToDisplay(
            m_joyAxes[0][AXIS_Y], minY, m_dispH);
    }
    else if (m_analogJoystickSource == 1 && analogMouseMoved)
    {
        m_analogJoystickCursorX += mouseDeltaX[0];
        m_analogJoystickCursorY += mouseDeltaY[0];
    }
    m_analogJoystickCursorX = std::clamp(
        m_analogJoystickCursorX, minX, maxX);
    m_analogJoystickCursorY = std::clamp(
        m_analogJoystickCursorY, minY, maxY);
    // The Upright cabinet reverses the horizontal electrical range relative
    // to Deluxe. Apply the cabinet-specific orientation to the shared virtual
    // output so mouse and joypad modes remain consistent.
    m_mouseAxes[analogJoystickDev][AXIS_X] =
        g_star_wars_invert_x && g_options.star_wars_upright_x_inversion
        ? minX + (maxX - m_analogJoystickCursorX)
        : m_analogJoystickCursorX;
    m_mouseAxes[analogJoystickDev][AXIS_Y] = m_analogJoystickCursorY;
    m_mouseAxes[analogJoystickDev][AXIS_Z] = 0;
    m_mouseWheelDir[analogJoystickDev] = 0;
    m_mouseIsAbsolute[analogJoystickDev] = true;
    const bool analogAllowMouse =
        g_options.star_wars_input == StarWarsInput::Hybrid ||
        g_options.star_wars_input == StarWarsInput::Mouse;
    const bool analogAllowStick =
        g_options.star_wars_input == StarWarsInput::Hybrid ||
        g_options.star_wars_input == StarWarsInput::AnalogSticks;
    m_mouseButtons[analogJoystickDev][0] =
        (analogAllowMouse && m_mouseButtons[0][0]) ||
        (analogAllowStick && m_joyButtons[0][0]);
    m_mouseButtons[analogJoystickDev][1] = false;
    m_mouseButtons[analogJoystickDev][2] =
        (analogAllowMouse && m_mouseButtons[0][2]) ||
        (analogAllowStick && m_joyButtons[0][1]);
    m_mouseButtons[analogJoystickDev][3] =
        (analogAllowMouse && m_mouseButtons[0][3]) ||
        (analogAllowStick && m_joyButtons[0][2]);
    m_mouseButtons[analogJoystickDev][4] =
        (analogAllowMouse && m_mouseButtons[0][4]) ||
        (analogAllowStick && m_joyButtons[0][3]);

    return true;
}

bool CLibretroInputSystem::IsJoyButPressed(int joyNum, int butNum) const
{
    if (joyNum < 0 || joyNum >= 2) return false;
    if (butNum < 0 || butNum >= NUM_JOY_BUTTONS) return false;

    return m_joyButtons[joyNum][butNum] != 0;
}


int CLibretroInputSystem::GetJoyAxisValue(int joyNum, int axisNum) const
{
    if (joyNum < 0 || joyNum >= 2) return 0;
    if (axisNum < 0 || axisNum >= NUM_JOY_AXES) return 0;
    return m_joyAxes[joyNum][axisNum];
}

bool CLibretroInputSystem::IsJoyPOVInDir(int joyNum, int /*povNum*/, int povDir) const
{
    if (joyNum < 0 || joyNum >= 2) return false;
    if (povDir < 0 || povDir > 3) return false;

    return m_joyPOV[joyNum][povDir] != 0;
}


int CLibretroInputSystem::GetKeyIndex(const char *keyName)
{
    if (!keyName) return -1;
    
    // Map Supermodel KEY_* to libretro RETROK_* codes
    if (std::strcmp(keyName, "KEY_1") == 0) return RETROK_1;
    if (std::strcmp(keyName, "KEY_2") == 0) return RETROK_2;
    if (std::strcmp(keyName, "KEY_3") == 0) return RETROK_3;
    if (std::strcmp(keyName, "KEY_4") == 0) return RETROK_4;
    if (std::strcmp(keyName, "KEY_5") == 0) return RETROK_5;  // Service A
    if (std::strcmp(keyName, "KEY_6") == 0) return RETROK_6;  // Test A
    if (std::strcmp(keyName, "KEY_7") == 0) return RETROK_7;  // Service B
    if (std::strcmp(keyName, "KEY_8") == 0) return RETROK_8;  // Test B
    if (std::strcmp(keyName, "KEY_UP") == 0) return RETROK_UP;
    if (std::strcmp(keyName, "KEY_DOWN") == 0) return RETROK_DOWN;
    if (std::strcmp(keyName, "KEY_LEFT") == 0) return RETROK_LEFT;
    if (std::strcmp(keyName, "KEY_RIGHT") == 0) return RETROK_RIGHT;
    if (std::strcmp(keyName, "KEY_A") == 0) return RETROK_a;
    if (std::strcmp(keyName, "KEY_S") == 0) return RETROK_s;
    if (std::strcmp(keyName, "KEY_D") == 0) return RETROK_d;
    if (std::strcmp(keyName, "KEY_F") == 0) return RETROK_f;
    if (std::strcmp(keyName, "KEY_Q") == 0) return RETROK_q;
    if (std::strcmp(keyName, "KEY_W") == 0) return RETROK_w;
    if (std::strcmp(keyName, "KEY_E") == 0) return RETROK_e;
    if (std::strcmp(keyName, "KEY_R") == 0) return RETROK_r;
    if (std::strcmp(keyName, "KEY_H") == 0) return RETROK_h;
    if (std::strcmp(keyName, "KEY_T") == 0) return RETROK_t;
    if (std::strcmp(keyName, "KEY_X") == 0) return RETROK_x;
    if (std::strcmp(keyName, "KEY_Y") == 0) return RETROK_y;
    if (std::strcmp(keyName, "KEY_Z") == 0) return RETROK_z;
    if (std::strcmp(keyName, "KEY_SPACE") == 0) return RETROK_SPACE;
    
    return -1;
}

bool CLibretroInputSystem::IsKeyPressed(int kbdNum, int keyIndex) const
{
    if (kbdNum != 0) return false;

    if (keyIndex >= 0 && keyIndex < 512)
        return m_keyState[keyIndex];

    return false;
}


bool CLibretroInputSystem::IsMouseButPressed(int mseNum, int butNum) const
{
    if (butNum < 0 || butNum >= NUM_MOUSE_BUTTONS) return false;
    if (mseNum == ANY_MOUSE)
    {
        for (int dev = 0; dev < kMouseDeviceCount; ++dev)
            if (m_mouseButtons[dev][butNum])
                return true;
        return false;
    }
    if (mseNum < 0 || mseNum >= kMouseDeviceCount) return false;
    return m_mouseButtons[mseNum][butNum];
}

int CLibretroInputSystem::GetMouseAxisValue(int mseNum, int axisNum) const
{
    if (mseNum == ANY_MOUSE)
    {
        // For absolute axes, return the one that is currently being used.
        // For simplicity, prioritize lightguns if they moved significantly, otherwise mouse 0.
        return m_mouseAxes[0][axisNum];
    }
    if (mseNum < 0 || mseNum >= kMouseDeviceCount) return 0;
    if (axisNum < 0 || axisNum >= NUM_MOUSE_AXES) return 0;
    return m_mouseAxes[mseNum][axisNum];
}

int CLibretroInputSystem::GetMouseWheelDir(int mseNum) const
{
    if (mseNum == ANY_MOUSE)
    {
        for (int dev = 0; dev < kMouseDeviceCount; ++dev)
            if (m_mouseWheelDir[dev] != 0)
                return m_mouseWheelDir[dev];
        return 0;
    }
    if (mseNum < 0 || mseNum >= kMouseDeviceCount) return 0;
    return m_mouseWheelDir[mseNum];
}

void CLibretroInputSystem::SetMouseVisibility(bool /*visible*/)
{
}

const MouseDetails *CLibretroInputSystem::GetMouseDetails(int mseNum)
{
    static MouseDetails d[kMouseDeviceCount];
    static bool initialized = false;

    if (!initialized) {
        for (int i = 0; i < 2; ++i)
        {
            std::memset(&d[i], 0, sizeof(d[i]));
            snprintf(d[i].name, MAX_NAME_LENGTH, "Libretro Mouse %d", i + 1);
            d[i].isAbsolute = false;
        }
        for (int i = 0; i < 2; ++i)
        {
            const int dev = i + 2;
            std::memset(&d[dev], 0, sizeof(d[dev]));
            snprintf(d[dev].name, MAX_NAME_LENGTH, "Libretro Lightgun %d", i + 1);
            d[dev].isAbsolute = true;
        }
        for (int i = 0; i < 2; ++i)
        {
            const int dev = i + 4;
            std::memset(&d[dev], 0, sizeof(d[dev]));
            snprintf(d[dev].name, MAX_NAME_LENGTH,
                     "Libretro Gun Cursor %d", i + 1);
            d[dev].isAbsolute = true;
        }
        std::memset(&d[6], 0, sizeof(d[6]));
        snprintf(d[6].name, MAX_NAME_LENGTH,
                 "Libretro Analog Joystick Cursor");
        d[6].isAbsolute = true;
        initialized = true;
    }
    
    if (mseNum < 0 || mseNum >= kMouseDeviceCount) return nullptr;
    return &d[mseNum];
}

bool CLibretroInputSystem::ProcessForceFeedbackCmd(int joyNum, int /*axisNum*/, ForceFeedbackCmd ffCmd)
{
    if (!m_rumbleInterface.set_rumble_state)
    {
        return false;
    }

    // If the user disabled FFB in the menu, or we have no interface, abort early
    if (!m_ffbEnabled || !m_rumbleInterface.set_rumble_state)
        return false;
        
    // 2. Map the float force (-1.0 to 1.0) to Libretro's uint16_t (0 to 65535)
    // Most gamepads only support vibration strength, not direction, so we use absolute value.
    uint16_t strength = (uint16_t)(std::min(std::abs(ffCmd.force), 1.0f) * 65535);

    switch (ffCmd.id)
    {
        case FFConstantForce:
        case FFSelfCenter:
            // High torque effects go to the Strong (Low-Frequency) motor
            m_rumbleInterface.set_rumble_state(joyNum, RETRO_RUMBLE_STRONG, strength);
            break;

        case FFVibrate:
        case FFFriction:
            // High frequency effects go to the Weak (High-Frequency) motor
            m_rumbleInterface.set_rumble_state(joyNum, RETRO_RUMBLE_WEAK, strength);
            break;

        case FFStop:
            m_rumbleInterface.set_rumble_state(joyNum, RETRO_RUMBLE_STRONG, 0);
            m_rumbleInterface.set_rumble_state(joyNum, RETRO_RUMBLE_WEAK, 0);
            break;

        default:
            return false;
    }

    return true;
}

void CLibretroInputSystem::StopAllRumble()
{
    if (!m_rumbleInterface.set_rumble_state)
        return;

    for (unsigned port = 0; port < 2; ++port)
    {
        m_rumbleInterface.set_rumble_state(port, RETRO_RUMBLE_STRONG, 0);
        m_rumbleInterface.set_rumble_state(port, RETRO_RUMBLE_WEAK, 0);
    }
}

const KeyDetails *CLibretroInputSystem::GetKeyDetails(int /*kbdNum*/)
{
    static KeyDetails d{};
    std::strncpy(d.name, "Libretro Keyboard", MAX_NAME_LENGTH);
    return &d;
}

const JoyDetails *CLibretroInputSystem::GetJoyDetails(int joyNum)
{
    static JoyDetails d[2];
    static bool initialized = false;

    if (!initialized) {
        for (int i = 0; i < 2; i++) {
            std::memset(&d[i], 0, sizeof(d[i]));
            std::strncpy(d[i].name, "Libretro Joypad", MAX_NAME_LENGTH);
            d[i].numButtons = 32;
            d[i].numAxes = 6;   // left stick, right stick, and the two triggers
            d[i].numPOVs = 1;
            d[i].hasAxis[AXIS_X]  = true;   // left stick
            d[i].hasAxis[AXIS_Y]  = true;
            d[i].hasAxis[AXIS_Z]  = true;   // L2
            d[i].hasAxis[AXIS_RX] = true;   // right stick
            d[i].hasAxis[AXIS_RY] = true;
            d[i].hasAxis[AXIS_RZ] = true;   // R2
            d[i].hasFFeedback = true;
            for (int a = 0; a < NUM_JOY_AXES; a++)
                d[i].axisHasFF[a] = true;
        }
        initialized = true;
    }
    return &d[joyNum];
}

bool CLibretroInputSystem::InitializeSystem()
{
    return true;
}
bool CLibretroInputSystem::Initialize()
{
    bool result = CInputSystem::Initialize(); // let base do its thing
    return result;
}
const char *CLibretroInputSystem::GetKeyName(int /*keyIndex*/) { return "NONE"; }
