#pragma once

#include <cstddef>
#include <cstdint>

#include "Game.h"

namespace LibretroInputProfiles
{
    enum class Family
    {
        Gun,
        AnalogJoystick,
        Fishing,
        JoystickStandard,
        MagicalTruck,
        Ski,
        JoystickTwin,
        Driving,
    };

    struct Profile
    {
        uint32_t inputs;
        Family family;
        const char *name;
        unsigned players;
    };

    // Games.xml is effectively a closed catalog. Keep its fourteen control
    // signatures explicit: this makes additions visible in review and avoids
    // silently assigning a plausible but incorrect cabinet layout.
    inline constexpr Profile kProfiles[] = {
        {
            Game::INPUT_ANALOG_GUN1 | Game::INPUT_ANALOG_GUN2,
            Family::Gun,
            "Gun",
            2,
        },
        {
            Game::INPUT_ANALOG_JOYSTICK,
            Family::AnalogJoystick,
            "Special: Star Wars Trilogy Arcade",
            1,
        },
        {
            Game::INPUT_FISHING,
            Family::Fishing,
            "Special: Fishing",
            1,
        },
        {
            Game::INPUT_JOYSTICK1 | Game::INPUT_JOYSTICK2 | Game::INPUT_FIGHTING,
            Family::JoystickStandard,
            "Joystick (Standard): Fighting",
            2,
        },
        {
            Game::INPUT_JOYSTICK1 | Game::INPUT_JOYSTICK2 | Game::INPUT_SOCCER,
            Family::JoystickStandard,
            "Joystick (Standard): Soccer",
            2,
        },
        {
            Game::INPUT_JOYSTICK1 | Game::INPUT_SPIKEOUT,
            Family::JoystickStandard,
            "Joystick (Standard): Fighting (Spikeout)",
            1,
        },
        {
            Game::INPUT_MAGTRUCK,
            Family::MagicalTruck,
            "Special: Magical Truck Adventure",
            2,
        },
        {
            Game::INPUT_SKI,
            Family::Ski,
            "Special: Ski Champ",
            1,
        },
        {
            Game::INPUT_TWIN_JOYSTICKS,
            Family::JoystickTwin,
            "Joystick (Twin)",
            1,
        },
        {
            Game::INPUT_VEHICLE | Game::INPUT_SHIFT4 |
                Game::INPUT_HANDBRAKE | Game::INPUT_VIEWCHANGE,
            Family::Driving,
            "Driving: 4-Speed + VR1 + Handbrake",
            1,
        },
        {
            Game::INPUT_VEHICLE | Game::INPUT_SHIFT4 | Game::INPUT_VR4,
            Family::Driving,
            "Driving: 4-Speed + VR4",
            1,
        },
        {
            Game::INPUT_VEHICLE | Game::INPUT_SHIFTUPDOWN |
                Game::INPUT_HARLEY | Game::INPUT_VIEWCHANGE,
            Family::Driving,
            "Driving: Sequential + VR1 (Harley-Davidson)",
            1,
        },
        {
            Game::INPUT_VEHICLE | Game::INPUT_SHIFTUPDOWN |
                Game::INPUT_VIEWCHANGE,
            Family::Driving,
            "Driving: Sequential + VR1",
            1,
        },
        {
            Game::INPUT_VEHICLE | Game::INPUT_SHIFTUPDOWN | Game::INPUT_VR4,
            Family::Driving,
            "Driving: Sequential + VR4",
            1,
        },
    };

    inline constexpr std::size_t kProfileCount =
        sizeof(kProfiles) / sizeof(kProfiles[0]);

    static_assert(kProfileCount == 14, "The Model 3 control catalog has fourteen signatures");

    inline uint32_t NormalizeInputs(uint32_t inputs)
    {
        return inputs & ~static_cast<uint32_t>(Game::INPUT_COMMON);
    }

    inline const Profile *Find(uint32_t inputs)
    {
        const uint32_t normalized = NormalizeInputs(inputs);
        for (const Profile &profile : kProfiles)
        {
            if (profile.inputs == normalized)
                return &profile;
        }
        return nullptr;
    }
}
