#include "libretro.h"
#include "libretro_cbs.h"
#include <cstdlib>
#include <cstring>

// --- Core Options ---
static struct retro_core_option_v2_category option_cats[] = {
   {
      "system",
      "System",
      "Configure machine initialization, persistence, and networking."
   },
   {
      "video",
      "Video",
      "Configure graphics and rendering options."
   },
   {
      "audio", 
      "Audio",
      "Configure audio settings."
   },
   {
      "input",
      "Input",
      "Configure input and control settings."
   },
   {
      "cpu",
      "CPU",
      "Configure CPU performance and emulation settings."
   },
   { NULL, NULL, NULL },
};

static struct retro_core_option_v2_definition option_defs[] = {
   // System
   {
      "supermodel_initial_nvram_setup",
      "Automatic Initial NVRAM Setup",
      NULL,
      "When no frontend .srm or valid standalone .nv exists, initialize supported games with Single, Stand Alone, or No Link and set Star Wars Trilogy Arcade to the Upright cabinet. Other EEPROM fields retain the validated smoke-test values for that ROM set. Existing saves are never modified. Delete the game's .srm to regenerate this initial setup. Restart content to apply.",
      NULL,
      "system",
      {
         { "enabled",  NULL },
         { "disabled", NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "supermodel_network_board",
      "Network Board",
      NULL,
      "Connect the Model 3 network board, equivalent to the standalone Network setting. Some games expose their Network Assignments in the Service Menu only while the board is connected. Requires a content restart. Experimental linked play through RetroArch Netplay is available for Daytona USA 2, Harley-Davidson, Scud Race, and Sega Rally 2 families.",
      NULL,
      "system",
      {
         { "enabled",  "Connected" },
         { "disabled", "Disconnected" },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "supermodel_network_cabinets",
      "Linked Cabinets",
      NULL,
      "Set the total number of Model 3 cabinets expected in the RetroArch Netplay session. The host waits for this exact number before starting the emulated cabinet link. Every instance must use the same value. Two- and three-cabinet sessions have been tested; larger values remain experimental. Requires a content restart.",
      NULL,
      "system",
      {
         { "2",  "2 Cabinets (Default)" },
         { "3",  "3 Cabinets" },
         { "4",  "4 Cabinets" },
         { "5",  "5 Cabinets" },
         { "6",  "6 Cabinets" },
         { "7",  "7 Cabinets" },
         { "8",  "8 Cabinets" },
         { "9",  "9 Cabinets" },
         { "10", "10 Cabinets" },
         { "11", "11 Cabinets" },
         { "12", "12 Cabinets" },
         { "13", "13 Cabinets" },
         { "14", "14 Cabinets" },
         { "15", "15 Cabinets" },
         { "16", "16 Cabinets" },
         { NULL, NULL },
      },
      "2"
   },
   {
      "supermodel_nvram_settings",
      "NVRAM Settings",
      NULL,
      "Let RetroArch manage the supported NVRAM settings for each game. When enabled, every displayed value is applied at startup and overrides later Service Menu changes. When disabled, the core does not modify these fields. Settings saved for other games are never reused. Restart content to apply.",
      NULL,
      "system",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   // Video
   {
      "supermodel_resolution",
      "Internal Resolution",
      NULL,
      "Render at higher internal resolution for improved image quality. Lower values reduce GPU load. 'Half' (248x192) recommended for low-end systems.",
      NULL,
      "video",
      {
         { "half",   "Half (248x192) - Low-End Mobile" },
         { "native", "Native (496x384) - Recommended" },
         { "2x",     "2x (992x768)" },
         { "3x",     "3x (1488x1152)" },
         { "4x",     "4x (1984x1536)" },
         { NULL, NULL },
      },
      "native"
   },
#if defined(HAVE_LEGACY3D) && !defined(USE_LEGACY3D)
   {
      "supermodel_renderer_3d",
      "3D Renderer",
      NULL,
      "Select Supermodel's 3D engine. New3D is the current renderer. Legacy3D uses the desktop OpenGL compatibility profile and may perform better on older or integrated GPUs. Legacy3D is experimental and may crash the frontend or fail to initialize on unsupported OpenGL drivers. Takes effect after restarting content.",
      NULL,
      "video",
      {
         { "new3d",    "New3D (Default)" },
         { "legacy3d", "Legacy3D (Experimental)" },
         { NULL, NULL },
      },
      "new3d"
   },
#endif
#ifdef HAVE_QUAD_RENDERING
   {
      "supermodel_quad_rendering",
      "Quad Rendering",
      NULL,
      "Render Model 3 quadrilaterals as native quads through New3D's geometry-shader path instead of splitting them into triangles. Requires OpenGL 4.5 and takes effect after restarting content. Ignored by Legacy3D.",
      NULL,
      "video",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
#endif
#ifdef HAVE_CRT_COLOURS
   {
      "supermodel_crt_colors",
      "CRT Colour",
      NULL,
      "Apply Supermodel's native pre-output colour and gamma adaptation. This is distinct from a frontend CRT shader, and applies to New3D after restarting content. Ignored by Legacy3D.",
      NULL,
      "video",
      {
         { "0", "None (Default)" },
         { "1", "ARI/D93 (Japan)" },
         { "2", "PVM-20M2U/D93" },
         { "3", "BT.601 525/D93" },
         { "4", "BT.601 525/D65 (USA)" },
         { "5", "BT.601 625/D65 (Europe/Australia)" },
         { NULL, NULL },
      },
      "0"
   },
#endif
#ifdef HAVE_SUPERSAMPLING
   {
      "supermodel_supersampling",
      "Supersampling",
      NULL,
      "Render each output pixel from multiple internal samples using Supermodel's native supersampling pass. The scale applies on top of Internal Resolution: 2x uses 4 samples per pixel, 3x uses 9, and 8x uses 64. Higher values can require substantial GPU time and memory. Takes effect after restarting content.",
      NULL,
      "video",
      {
         { "1", "1x (Disabled, Default)" },
         { "2", "2x (4 Samples)" },
         { "3", "3x (9 Samples)" },
         { "4", "4x (16 Samples)" },
         { "5", "5x (25 Samples)" },
         { "6", "6x (36 Samples)" },
         { "7", "7x (49 Samples)" },
         { "8", "8x (64 Samples)" },
         { NULL, NULL },
      },
      "1"
   },
#endif
   {
      "supermodel_timing_overlay",
      "Frame Timing Overlay",
      NULL,
      "Show the per-frame timing overlay (PPC/Render/GPU/Total) and log timing averages. Costs an extra draw pass every frame, so leave it off unless you are profiling.",
      NULL,
      "video",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "supermodel_upscale_mode",
      "2D Layer Upscaling Filter",
      NULL,
      "Select the native Supermodel filter used to upscale internal 2D tile layers before they are composited with 3D. At native resolution Supermodel always uses Nearest. Takes effect after restarting content.",
      NULL,
      "video",
      {
         { "0", "Nearest" },
         { "1", "Biquintic" },
         { "2", "Bilinear (Default)" },
         { "3", "Bicubic" },
         { NULL, NULL },
      },
      "2"
   },
   {
      "supermodel_wide_screen",
      "Widescreen Mode",
      NULL,
      "Expand full-screen 3D viewports horizontally to render a true 16:9 field of view. 'Widescreen + Wide Background' also stretches the lower 2D background layer to fill the sides; HUD and upper overlays remain at their original aspect ratio. Takes effect after restarting content. May expose geometry or background artifacts in some games.",
      NULL,
      "video",
      {
         { "disabled", NULL },
         { "enabled", "Widescreen" },
         { "wide_background", "Widescreen + Wide Background" },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "supermodel_no_white_flash",
      "Disable White Flash",
      NULL,
      "Suppress the white flash normally shown when a game disables 3D rendering. Equivalent to standalone Supermodel's NoWhiteFlash setting. Requires restarting content.",
      NULL,
      "video",
      {
         { "disabled", "OFF (Default)" },
         { "enabled",  "ON" },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "supermodel_av_timing",
      "Model 3 Timing Mode",
      NULL,
      "Select the video cadence and matching 44.1 kHz audio packetization. Native Model 3 timing reports 57.524160 FPS and uses a fractional 766/767-sample audio cadence; a display capable of matching this refresh rate provides the smoothest result. Native timing requires a multi-threaded emulation mode for continuous audio. Requires restarting content.",
      NULL,
      "video",
      {
         { "60hz",   "60 Hz (Default)" },
         { "native", "57.524160 Hz (Native Model 3)" },
         { NULL, NULL },
      },
      "60hz"
   },
   {
      "supermodel_crosshairs",
      "Show Crosshair",
      NULL,
      "Select which native Supermodel vector crosshair is displayed in light gun games.",
      NULL,
      "input",
      {
         { "0", "Disabled" },
         { "1", "Player 1 Only" },
         { "2", "Player 2 Only" },
         { "3", "Players 1 & 2" },
         { NULL, NULL },
      },
      "0"
   },
   // Input
   {
      "supermodel_gun_input",
      "Gun Input Mode",
      NULL,
      "Input source for analog-gun games. Standard accepts RetroArch Lightgun, Mouse, and the left Analog Stick through one virtual cursor. Mouse + Analog Stick excludes Lightgun coordinates while retaining both relative cursor sources. Dedicated modes restrict input to the selected source. Changes take effect immediately.",
      NULL,
      "input",
      {
         { "hybrid",   "Standard" },
         { "lightgun", "Lightgun Only" },
         { "mouse_analog", "Mouse + Analog Stick" },
         { "mouse",    "Mouse Only" },
         { "analog",   "Analog Stick Only" },
         { NULL, NULL },
      },
      "hybrid"
   },
   {
      "supermodel_star_wars_input",
      "Star Wars Trilogy Input Mode",
      NULL,
      "Input source for the arcade analog joystick used by Star Wars Trilogy Arcade. Standard accepts the Mouse and left Analog Stick through one virtual control, with the last moved source taking priority. Changes take effect immediately.",
      NULL,
      "input",
      {
         { "hybrid", "Standard" },
         { "mouse",  "Mouse Only" },
         { "analog", "Analog Stick Only" },
         { NULL, NULL },
      },
      "hybrid"
   },
   {
      "supermodel_star_wars_upright_x_inversion",
      "Star Wars Trilogy Upright Mode X-Axis Inversion Fix",
      NULL,
      "Invert the horizontal analog input when Star Wars Trilogy Arcade's NVRAM cabinet type is Upright, matching the cabinet-specific axis orientation. Disable this when the frontend or input device already applies the required inversion. Changes take effect immediately.",
      NULL,
      "input",
      {
         { "enabled",  "ON" },
         { "disabled", "Disable" },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "supermodel_force_feedback",
      "Force Feedback",
      NULL,
      "Enable force feedback for racing games (requires compatible hardware).",
      NULL,
      "input",
      {
         { "enabled",  "ON (Default)" },
         { "disabled", "Disable" },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "supermodel_four_speed_shifter",
      "4-Speed Shifter",
      NULL,
      "Applied only to recognized 4-Speed driving games. Standard assigns each of the four remappable directional inputs directly to one gear. H-Gate interprets them as Up, Down, Left, and Right, selecting gears from the four diagonal positions. Neutral remains on West and L/R remain sequential shift controls. Changes take effect immediately.",
      NULL,
      "input",
      {
         { "h_gate",   "H-Gate Mode" },
         { "standard", "Standard" },
         { NULL, NULL },
      },
      "h_gate"
   },
   {
      "supermodel_steering_response",
      "Driving Steering Response",
      NULL,
      "Applied only to games recognized as Driving. Select the steering response curve; Progressive and FBNeo Logarithmic reduce sensitivity around the center while retaining the available output range.",
      NULL,
      "input",
      {
         { "linear",      "Linear" },
         { "progressive", "Progressive (Fine Center)" },
         { "fbneo",       "FBNeo Logarithmic (Fine Center)" },
         { NULL, NULL },
      },
      "linear"
   },
   {
      "supermodel_steering_output_range",
      "Driving Steering Output Range",
      NULL,
      "Applied only to games recognized as Driving. Scale steering around its center. Values below 100% reduce the emulated wheel range; values above 100% reach full lock with less physical stick travel.",
      NULL,
      "input",
      {
         { "50",  "50%" },
         { "60",  "60%" },
         { "63",  "63% (30-80-D0)" },
         { "70",  "70%" },
         { "80",  "80%" },
         { "90",  "90%" },
         { "100", "100%" },
         { "110", "110%" },
         { "120", "120%" },
         { "130", "130%" },
         { "140", "140%" },
         { "150", "150%" },
         { NULL, NULL },
      },
      "100"
   },
   {
      "supermodel_accelerator_output_range",
      "Driving Accelerator Output Range",
      NULL,
      "Applied only to games recognized as Driving. Scale the accelerator from its zero rest position. Values below 100% reduce the maximum emulated pedal output; values above 100% reach full output with less physical trigger travel.",
      NULL,
      "input",
      {
         { "50",   "50%" },
         { "60",   "60%" },
         { "70",   "70%" },
         { "75.3", "75.3% (00-C0)" },
         { "80",   "80%" },
         { "90",   "90%" },
         { "100",  "100%" },
         { "110",  "110%" },
         { "120",  "120%" },
         { "130",  "130%" },
         { "140",  "140%" },
         { "150",  "150%" },
         { NULL, NULL },
      },
      "100"
   },
   {
      "supermodel_brake_output_range",
      "Driving Brake Output Range",
      NULL,
      "Applied only to games recognized as Driving. Scale the brake from its zero rest position. Values below 100% reduce the maximum emulated pedal output; values above 100% reach full output with less physical trigger travel.",
      NULL,
      "input",
      {
         { "50",   "50%" },
         { "60",   "60%" },
         { "70",   "70%" },
         { "75.3", "75.3% (00-C0)" },
         { "80",   "80%" },
         { "90",   "90%" },
         { "100",  "100%" },
         { "110",  "110%" },
         { "120",  "120%" },
         { "130",  "130%" },
         { "140",  "140%" },
         { "150",  "150%" },
         { NULL, NULL },
      },
      "100"
   },
   // Audio
   {
      "supermodel_sound_volume",
      "Sound Volume",
      NULL,
      "Adjust overall sound volume.",
      NULL,
      "audio",
      {
         { "0",   "0%" },
         { "10",  "10%" },
         { "20",  "20%" },
         { "30",  "30%" },
         { "40",  "40%" },
         { "50",  "50%" },
         { "60",  "60%" },
         { "70",  "70%" },
         { "80",  "80%" },
         { "90",  "90%" },
         { "100", "100%" },
         { "110", "110%" },
         { "120", "120%" },
         { "130", "130%" },
         { "140", "140%" },
         { "150", "150%" },
         { "160", "160%" },
         { "170", "170%" },
         { "180", "180%" },
         { "190", "190%" },
         { "200", "200%" },
         { NULL, NULL },
      },
      "100"
   },
   {
      "supermodel_music_volume",
      "Music Volume",
      NULL,
      "Adjust DSB music volume (affects games with separate music board e.g. Sega Rally 2, Virtua Fighter 3).",
      NULL,
      "audio",
      {
         { "0",   "0%" },
         { "10",  "10%" },
         { "20",  "20%" },
         { "30",  "30%" },
         { "40",  "40%" },
         { "50",  "50%" },
         { "60",  "60%" },
         { "70",  "70%" },
         { "80",  "80%" },
         { "90",  "90%" },
         { "100", "100%" },
         { "110", "110%" },
         { "120", "120%" },
         { "130", "130%" },
         { "140", "140%" },
         { "150", "150%" },
         { "160", "160%" },
         { "170", "170%" },
         { "180", "180%" },
         { "190", "190%" },
         { "200", "200%" },
         { NULL, NULL },
      },
      "100"
   },
   {
      "supermodel_sound_enable",
      "Sound Enable",
      NULL,
      "Enable or disable sound emulation. Disabling sound can significantly improve performance on slow hardware.",
      NULL,
      "audio",
      {
         { "enabled",  NULL },
         { "disabled", NULL },
         { NULL, NULL },
      },
      "enabled"
   },
   {
      "supermodel_scsp_dsp",
      "SCSP DSP Engine",
      NULL,
      "Select the Sega Custom Sound Processor DSP implementation. The modern MAME-derived engine is recommended; use Legacy ElSemi for games with audio compatibility issues such as Fighting Vipers 2. Takes effect after restarting content.",
      NULL,
      "audio",
      {
         { "new",    "New (MAME, Default)" },
         { "legacy", "Legacy (ElSemi)" },
         { NULL, NULL },
      },
      "new"
   },
   // CPU
   {
      "supermodel_ppc_frequency",
      "Emulated PowerPC Frequency",
      NULL,
      "Set the Model 3's emulated PowerPC clock. 'Auto' follows the game's stepping (66/100/166 MHz). Lower values reduce emulated CPU work but may change game timing or behavior. Restart content to apply.",
      NULL,
      "cpu",
      {
         { "auto", "Auto (Game Default: 66/100/166 MHz)" },
         { "25",   "25 MHz" },
         { "33",   "33 MHz" },
         { "50",   "50 MHz" },
         { "66",   "66 MHz" },
         { "70",   "70 MHz" },
         { "100",  "100 MHz" },
         { "133",  "133 MHz" },
         { "166",  "166 MHz" },
         { "200",  "200 MHz" },
         { NULL, NULL },
      },
      "auto"
   },
   {
      "supermodel_emulation_threading",
      "Emulation Threading",
      NULL,
      "Select how Supermodel distributes emulation work across host CPU threads. Multi-threaded runs the sound and drive boards on worker threads; Multi-threaded + GPU also overlaps PowerPC emulation with graphics processing. This is independent of RetroArch's Threaded Video setting. Restart content to apply.",
      NULL,
      "cpu",
      {
         { "single",    "Single Thread" },
         { "multi",     "Multi-threaded" },
         { "multi_gpu", "Multi-threaded + GPU (Default)" },
         { NULL, NULL },
      },
      "multi_gpu"
   },
#ifdef HAVE_PPC_JIT
   {
      "supermodel_jit_enable",
      "ARM64 JIT Recompiler (Experimental)",
      NULL,
      "Enable the experimental ARM64 dynamic recompiler for the emulated PowerPC CPU. Disabled by default; the interpreter remains the correctness reference. Restart content to apply.",
      NULL,
      "cpu",
      {
         { "disabled", "Disabled (Interpreter, Default)" },
         { "enabled",  "Enabled (Experimental)" },
         { NULL, NULL },
      },
      "disabled"
   },
#endif
   { NULL, NULL, NULL, NULL, NULL, NULL, {{NULL, NULL}}, NULL },
};

// --- Helper: Read Core Option ---
static const char* option_get(const char* key, const char* default_value)
{
   struct retro_variable var = { key, NULL };
   if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      return var.value;
   return default_value;
}

// Cache for parsed options
#include "CoreOptionsTypes.h"

// --- Update Core Options ---
void update_core_options(void)
{
   g_options.initial_nvram_setup =
      strcmp(option_get("supermodel_initial_nvram_setup", "enabled"),
             "enabled") == 0;

   g_options.network_board =
      strcmp(option_get("supermodel_network_board", "enabled"),
             "enabled") == 0;

   g_options.network_cabinets = static_cast<unsigned>(
      atoi(option_get("supermodel_network_cabinets", "2")));
   if (g_options.network_cabinets < 2 || g_options.network_cabinets > 16)
      g_options.network_cabinets = 2;

   g_options.nvram_settings_enabled =
      strcmp(option_get("supermodel_nvram_settings", "disabled"),
             "enabled") == 0;

   const char* resolution = option_get("supermodel_resolution", "native");
   if (strcmp(resolution, "half") == 0)
      g_options.resolution_multiplier = 0.5f;
   else if (strcmp(resolution, "2x") == 0)
      g_options.resolution_multiplier = 2.0f;
   else if (strcmp(resolution, "3x") == 0)
      g_options.resolution_multiplier = 3.0f;
   else if (strcmp(resolution, "4x") == 0)
      g_options.resolution_multiplier = 4.0f;
   else
      g_options.resolution_multiplier = 1.0f;  // native

#if defined(USE_LEGACY3D)
   g_options.renderer_3d = Renderer3D::Legacy3D;
#elif defined(HAVE_LEGACY3D)
   g_options.renderer_3d =
      strcmp(option_get("supermodel_renderer_3d", "new3d"),
             "legacy3d") == 0
         ? Renderer3D::Legacy3D : Renderer3D::New3D;
#else
   g_options.renderer_3d = Renderer3D::New3D;
#endif

#ifdef HAVE_QUAD_RENDERING
   g_options.quad_rendering =
      strcmp(option_get("supermodel_quad_rendering", "disabled"),
             "enabled") == 0;
#else
   g_options.quad_rendering = false;
#endif

#ifdef HAVE_CRT_COLOURS
   g_options.crt_colors = atoi(option_get("supermodel_crt_colors", "0"));
   if (g_options.crt_colors < 0 || g_options.crt_colors > 5)
      g_options.crt_colors = 0;
#else
   g_options.crt_colors = 0;
#endif

#ifdef HAVE_SUPERSAMPLING
   g_options.supersampling =
      atoi(option_get("supermodel_supersampling", "1"));
   if (g_options.supersampling < 1 || g_options.supersampling > 8)
      g_options.supersampling = 1;
#else
   g_options.supersampling = 1;
#endif

   g_options.upscale_mode = atoi(option_get("supermodel_upscale_mode", "2"));
   if (g_options.upscale_mode < 0 || g_options.upscale_mode > 3)
      g_options.upscale_mode = 2;

   g_options.timing_overlay = strcmp(option_get("supermodel_timing_overlay", "disabled"), "enabled") == 0;

   {
      const char *widescreen = option_get(
         "supermodel_wide_screen", "disabled");
      g_options.widescreen_mode =
         strcmp(widescreen, "wide_background") == 0
            ? WidescreenMode::WidescreenWideBackground
         : strcmp(widescreen, "enabled") == 0
            ? WidescreenMode::Widescreen
            : WidescreenMode::Disabled;
   }
   g_options.no_white_flash =
      strcmp(option_get("supermodel_no_white_flash", "disabled"),
             "enabled") == 0;
   g_options.av_timing_mode =
      strcmp(option_get("supermodel_av_timing", "60hz"), "native") == 0
         ? AVTimingMode::Native57524Hz
         : AVTimingMode::Default60Hz;
   {
      const char *crosshairs = option_get("supermodel_crosshairs", "0");
      // Accept the values used by earlier development builds so an existing
      // .opt file cannot leave the option in an undefined state.
      g_options.crosshairs = strcmp(crosshairs, "enabled") == 0 ? 3u
                           : strcmp(crosshairs, "disabled") == 0 ? 0u
                           : static_cast<unsigned>(atoi(crosshairs)) & 3u;
   }

   g_options.sound_enable = strcmp(option_get("supermodel_sound_enable", "enabled"), "enabled") == 0;
   g_options.sound_volume = atoi(option_get("supermodel_sound_volume", "100"));
   g_options.music_volume = atoi(option_get("supermodel_music_volume", "100"));
   g_options.legacy_sound_dsp =
      strcmp(option_get("supermodel_scsp_dsp", "new"), "legacy") == 0;

   {
      const char *gun_input = option_get("supermodel_gun_input", "hybrid");
      g_options.gun_input = strcmp(gun_input, "lightgun") == 0 ? GunInput::Lightgun
                          : strcmp(gun_input, "mouse") == 0    ? GunInput::Mouse
                          : strcmp(gun_input, "mouse_analog") == 0
                                                               ? GunInput::MouseAnalog
                          : strcmp(gun_input, "analog") == 0   ? GunInput::AnalogSticks
                                                               : GunInput::Hybrid;
   }
   {
      const char *star_wars_input = option_get(
         "supermodel_star_wars_input", "hybrid");
      g_options.star_wars_input =
         strcmp(star_wars_input, "mouse") == 0
            ? StarWarsInput::Mouse
         : strcmp(star_wars_input, "analog") == 0
            ? StarWarsInput::AnalogSticks
            : StarWarsInput::Hybrid;
   }
   g_options.star_wars_upright_x_inversion =
      strcmp(option_get("supermodel_star_wars_upright_x_inversion", "enabled"),
             "enabled") == 0;
   g_options.four_speed_shifter =
      strcmp(option_get("supermodel_four_speed_shifter", "h_gate"),
             "h_gate") == 0
         ? FourSpeedShifter::HGate
         : FourSpeedShifter::Standard;
   g_options.force_feedback =
      strcmp(option_get("supermodel_force_feedback", "enabled"),
             "enabled") == 0;
   {
      const char *response = option_get("supermodel_steering_response", "linear");
      g_options.steering_response = strcmp(response, "progressive") == 0
                                      ? SteeringResponse::Progressive
                                   : strcmp(response, "fbneo") == 0
                                      ? SteeringResponse::FBNeoLogarithmic
                                      : SteeringResponse::Linear;
   }
   g_options.steering_output_range = atoi(option_get("supermodel_steering_output_range", "100"));
   {
      const char *accelerator_range = option_get("supermodel_accelerator_output_range", "100");
      const char *brake_range = option_get("supermodel_brake_output_range", "100");
      g_options.accelerator_output_range_per_mille =
         strcmp(accelerator_range, "75.3") == 0 ? 753 : atoi(accelerator_range) * 10;
      g_options.brake_output_range_per_mille =
         strcmp(brake_range, "75.3") == 0 ? 753 : atoi(brake_range) * 10;
   }

   // Parse PowerPC frequency option
   const char* ppc_freq = option_get("supermodel_ppc_frequency", "auto");
   if (strcmp(ppc_freq, "auto") == 0)
      g_options.ppc_frequency = 0;
   else
   {
      int mhz = atoi(ppc_freq);
      g_options.ppc_frequency = (mhz > 0) ? mhz : 0;
   }

   {
      const char *threading = option_get(
         "supermodel_emulation_threading", "multi_gpu");
      g_options.emulation_threading =
         strcmp(threading, "single") == 0
            ? EmulationThreading::SingleThread
         : strcmp(threading, "multi") == 0
            ? EmulationThreading::MultiThreaded
            : EmulationThreading::MultiThreadedGPU;
   }
   
#ifdef HAVE_PPC_JIT
   g_options.jit_enable = strcmp(option_get("supermodel_jit_enable", "disabled"), "enabled") == 0;
#else
   g_options.jit_enable = false;
#endif

}
