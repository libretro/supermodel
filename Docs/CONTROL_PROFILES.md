# Libretro control profiles

The core resolves the control signature declared by each game in `Games.xml`
when content is loaded. The catalog is intentionally explicit because the
Model 3 game list is effectively complete.

| `Games.xml` input signature | Libretro profile |
|---|---|
| `analog_gun1 + analog_gun2` | Gun |
| `analog_joystick` | Special: Star Wars Trilogy Arcade |
| `fishing` | Special: Fishing |
| `joystick1 + joystick2 + fighting` | Joystick (Standard): Fighting |
| `joystick1 + joystick2 + soccer` | Joystick (Standard): Soccer |
| `joystick1 + spikeout` | Joystick (Standard): Fighting (Spikeout) |
| `magtruck` | Special: Magical Truck Adventure |
| `ski` | Special: Ski Champ |
| `twin_joysticks` | Joystick (Twin) |
| `vehicle + shift4 + handbrake + viewchange` | Driving: 4-Speed + VR1 + Handbrake |
| `vehicle + shift4 + vr4` | Driving: 4-Speed + VR4 |
| `vehicle + shiftupdown + harley + viewchange` | Driving: Sequential + VR1 (Harley-Davidson) |
| `vehicle + shiftupdown + viewchange` | Driving: Sequential + VR1 |
| `vehicle + shiftupdown + vr4` | Driving: Sequential + VR4 |

## Implementation rules

- A shared family is a physical RetroPad layout, not a union of logical
  cabinet inputs.
- Fighting, Soccer, and Spikeout share the Joystick (Standard) layout but bind
  only their own actions.
- Driving variants share the same base implementation. `shift4`,
  `shiftupdown`, `viewchange`, `vr4`, `handbrake`, and `harley` enable only the
  corresponding controls.
- A recognized 4-speed driving profile maps L/R to sequential shift and uses
  the four remappable half-axis inputs as an H-gate by default:
  Up/Left selects Gear 1, Down/Left Gear 2, Up/Right Gear 3, and Down/Right
  Gear 4. The `4-Speed Shifter` core option can switch to `Standard`, where
  each half-axis directly selects one gear. The option and its input
  descriptions update immediately.
- Driving profiles expose only the linear steering axis to RetroArch. Digital
  sources such as the D-pad can be assigned to it using the frontend's native
  remapping, without a second digital steering path inside the core.
- Driving tuning options remain visible for every game so their availability
  is predictable in RetroArch, but the core applies them only to recognized
  Driving profiles. Steering processing occurs after Libretro polling and
  before Model 3 reads the 8-bit steering ADC. It incorporates the Rad
  Mobile/Rad Rally MAME response work: Linear, Progressive, and FBNeo
  Logarithmic response and 50-150% output range. `Linear / 100%` is a
  transparent default and the processed position is always applied directly.
- Output ranges above 100% saturate at the ADC limits, allowing full lock to be
  reached with less physical stick travel. Values below 100% reduce the
  emulated wheel's maximum excursion. The 63% preset maps the full physical
  axis to the exact `30-80-D0` hexadecimal range.
- Accelerator and brake have independent output-range options governed by the
  same Driving-only application rule. Both are unipolar `00-FF` axes and
  default to a transparent 100%. Their
  75.3% preset maps full physical travel to the exact `00-C0` range.
- Single-view driving cabinets map VR1 to D-Pad Up. The Sega Rally 2-style
  profile additionally maps Handbrake to South.
- All 4-speed profiles expose Neutral on West, alongside the right-stick
  four-way gate and the L/R sequential controls.
- Four-button VR cabinets use the D-Pad as a POV: VR1 Red/Up, VR2 Blue/Down,
  VR3 Yellow/Left, and VR4 Green/Right. This arrangement follows the button
  order reported by Daytona USA 2's Service menu.
- Harley-Davidson maps VR1 to D-Pad Up, Rear Brake to South, and Music Select to
  East.
- Magical Truck exposes one cabinet side per RetroPad: each player's centered
  analog lever uses the left stick Y axis and each foot pedal uses South.
- Ski Champ maps the poles to L/R, Select 1 / Left (Blue) to West, the single
  Select 2 / Center (Red) command to both South and Start, and Select 3 /
  Right (Green) to East.
- Fishing maps Cast (Red) to South and Start, Select (Yellow) to East, Reel to
  R2, and Tension to L2. Tension is used by the Japan Standard `getbass` set;
  Reel is an analog `00-FF` speed value on all fishing cabinets.
- Twin-stick cabinets map the left/right shot triggers to L2/R2 and the
  left/right turbo buttons to L/R.
- Gun games default to `Standard`, combining Lightgun, Mouse, and Analog Stick.
  Absolute RetroLightgun coordinates, relative RetroMouse movement, and each RetroPad's
  left stick update one shared cursor per player. Dedicated Lightgun, Mouse,
  and Analog Stick modes restrict polling to the selected source. Mouse
  + Analog Stick keeps both relative cursor sources active without consuming
  RetroLightgun coordinates.
- Standard-mode shot inputs are deliberately parallel: Lightgun
  Trigger/Reload and RetroPad South/East are Left/Right Shot. Lightgun Aux A is accepted as an
  additional Right Shot binding. `IS_OFFSCREEN` is position state and is never
  treated as a pressed button.
- The core option can restrict the profile to Lightgun, Mouse, Mouse + Analog
  Stick, or Analog Sticks. It is applied immediately; content does not need to
  be reloaded.
- P1 and P2 remain logically independent. Actual two-mouse or two-lightgun
  operation still depends on the frontend input driver; RetroArch's macOS
  `cocoa` driver currently reports neither multi-mouse nor native lightgun
  support.
- Open validation item: test independent P1/P2 coordinates and buttons with
  two physical lightguns on a system/frontend input driver that supports
  multiple mice (for example Windows `winraw` or Linux `udev`/`x11`). Run the
  test for both `Gun` and `Gun + Test/Service slots` device variants.
- Supermodel's native `game.inputs` mask remains the final polling gate. A
  frontend `.rmp` can change physical bindings but cannot enable an input group
  absent from `Games.xml`.
- RetroArch input descriptors are regenerated after content identification and
  list only controls belonging to the resolved profile.
- RetroArch controller information is regenerated at the same time, so the
  current Port Device displays the exact recognized profile name instead of
  the generic `RetroPad` label.
- The default device variant on each port is the complete detected profile
  with `+ Test/Service slots`: Test uses L3 and Service uses R3 as ordinary
  remappable Libretro slots. The profile without these slots remains available
  as an explicit alternative device selected by the user.
- An unknown signature receives the generic RetroPad fallback and a warning in
  the frontend log. It is never assigned to the closest known profile.
