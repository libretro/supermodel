#include <compat/msvc.h>
#include <algorithm>
#include <math.h>
#include <chrono>
#include <rthreads/rthreads.h>
#include <streams/file_stream.h>
#include <file/file_path.h>
#include <string/stdstring.h>
#include <vector>
#include <stdarg.h>
#include <stdio.h>
#include <ctype.h>
#include <limits>
#include <string>
#include <zlib.h>
#include <Inputs/Inputs.h>
#include "libretro_cbs.h"
#include "LibretroTiming.h"
#include "Game.h"
#include "LibretroBlockFileMemory.h"
#include "InitialNvramTemplates.h"
#include "LibretroInputProfiles.h"
#include "LibretroNetBoard.h"
#include "LibretroNvramSettings.h"
#include "LibretroWrapper.h"
#include <GL/glew.h>
#include "../../Graphics/SuperAA.h"
#include "libretro_core_options.h"
#include "CLibretroInputSystem.h"
#include "CPU/PowerPC/ppc.h"
#include "libretroGui.h"
#include "../../Graphics/GLSLVersion.h"

// --- Global Variables ---
retro_video_refresh_t video_cb = NULL;
retro_environment_t environ_cb = NULL;
retro_audio_sample_t audio_cb = NULL;
retro_audio_sample_batch_t audio_batch_cb = NULL;
retro_input_poll_t input_poll_cb = NULL;
retro_input_state_t input_state_cb = NULL;
retro_log_printf_t log_cb;

struct retro_hw_render_callback hw_render;
struct retro_rumble_interface rumble;
struct retro_vfs_interface *g_vfs_interface = nullptr;
static LibretroWrapper wrapper = LibretroWrapper();
void set_input_descriptors(const Game *game);
void set_controller_info(const Game &game);

static constexpr unsigned kNoCabinetControlsDevice =
   RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 0);
bool g_cabinet_controls_enabled[2] = { true, true };
bool g_star_wars_invert_x = false;
static Game g_active_input_game;
static bool g_has_active_input_game = false;
static GunInput g_active_gun_input = GunInput::Hybrid;
static StarWarsInput g_active_star_wars_input = StarWarsInput::Hybrid;
static WidescreenMode g_active_widescreen_mode = WidescreenMode::Disabled;
static int g_network_board_option_visible = -1;
static int g_network_cabinets_option_visible = -1;
static constexpr unsigned kNvramSettingCount =
   static_cast<unsigned>(LibretroNvramSettings::Setting::Count);
struct NvramCoreOptionSet
{
   const char *game_name = nullptr;
   size_t definition_indices[kNvramSettingCount];
};

static std::vector<struct retro_core_option_v2_definition>
   g_registered_option_definitions;
static std::vector<std::vector<char>> g_nvram_option_key_storage;
static std::vector<std::vector<char>> g_nvram_default_label_storage;
static std::vector<NvramCoreOptionSet> g_nvram_option_sets;
static std::string g_visible_nvram_game;
static int g_visible_nvram_enabled = -1;

static void append_nvram_core_options(void)
{
   size_t game_count = 0;
   const auto *games = LibretroNvramSettings::GetSupportedGames(game_count);
   for (size_t game_index = 0; game_index < game_count; ++game_index)
   {
      Game game;
      game.name = games[game_index].name;
      if (games[game_index].parent)
         game.parent = games[game_index].parent;

      NvramCoreOptionSet option_set;
      option_set.game_name = games[game_index].name;
      std::fill_n(option_set.definition_indices, kNvramSettingCount,
                  std::numeric_limits<size_t>::max());

      for (unsigned i = 0; i < kNvramSettingCount; ++i)
      {
         const auto setting =
            static_cast<LibretroNvramSettings::Setting>(i);
         const auto *info = LibretroNvramSettings::GetSettingInfo(&game, setting);
         if (!info)
            continue;
         const char *default_value =
            LibretroNvramSettings::GetDefaultValue(&game, setting);
         if (!default_value)
            continue;

         g_nvram_option_key_storage.emplace_back(128, '\0');
         auto &key = g_nvram_option_key_storage.back();
         snprintf(key.data(), key.size(), "supermodel_nvram_%s_%s",
                  game.name.c_str(),
                  LibretroNvramSettings::OptionSuffix(setting));

         struct retro_core_option_v2_definition definition = {};
         definition.key = key.data();
         definition.desc = info->label;
         definition.info = info->description;
         definition.category_key = "system";
         const size_t capacity = sizeof(definition.values) /
                                 sizeof(definition.values[0]);
         const size_t value_count = std::min(info->valueCount, capacity - 1);
         for (size_t value = 0; value < value_count; ++value)
         {
            const char *label = info->values[value].label;
            if (strcmp(info->values[value].key, default_value) == 0)
            {
               const size_t label_size = strlen(label) + strlen(" (Default)") + 1;
               g_nvram_default_label_storage.emplace_back(label_size, '\0');
               auto &default_label = g_nvram_default_label_storage.back();
               snprintf(default_label.data(), default_label.size(),
                        "%s (Default)", label);
               label = default_label.data();
            }
            definition.values[value] = {
               info->values[value].key, label
            };
         }
         definition.default_value = default_value;
         option_set.definition_indices[i] =
            g_registered_option_definitions.size();
         g_registered_option_definitions.push_back(definition);
      }
      g_nvram_option_sets.push_back(option_set);
   }
}

static void build_core_option_definitions(void)
{
   if (!g_registered_option_definitions.empty())
      return;

   size_t game_count = 0;
   const auto *games = LibretroNvramSettings::GetSupportedGames(game_count);
   size_t nvram_option_count = 0;
   for (size_t game_index = 0; game_index < game_count; ++game_index)
   {
      Game game;
      game.name = games[game_index].name;
      if (games[game_index].parent)
         game.parent = games[game_index].parent;
      for (unsigned i = 0; i < kNvramSettingCount; ++i)
      {
         if (LibretroNvramSettings::GetSettingInfo(&game,
               static_cast<LibretroNvramSettings::Setting>(i)))
            ++nvram_option_count;
      }
   }

   size_t base_option_count = 0;
   for (const auto &definition : option_defs)
      if (definition.key)
         ++base_option_count;
   g_registered_option_definitions.reserve(
      base_option_count + nvram_option_count + 1);
   g_nvram_option_key_storage.reserve(nvram_option_count);
   g_nvram_default_label_storage.reserve(nvram_option_count);
   g_nvram_option_sets.reserve(game_count);

   for (const auto &definition : option_defs)
   {
      if (!definition.key)
         break;
      g_registered_option_definitions.push_back(definition);
      if (strcmp(definition.key, "supermodel_nvram_settings") == 0)
         append_nvram_core_options();
   }
   g_registered_option_definitions.push_back({});
}

static const NvramCoreOptionSet *active_nvram_option_set(void)
{
   if (!g_has_active_input_game)
      return nullptr;
   for (const auto &option_set : g_nvram_option_sets)
      if (g_active_input_game.name == option_set.game_name)
         return &option_set;
   return nullptr;
}

static const char *active_nvram_option_key(
      LibretroNvramSettings::Setting setting)
{
   const auto *option_set = active_nvram_option_set();
   if (!option_set)
      return nullptr;
   const size_t index =
      option_set->definition_indices[static_cast<unsigned>(setting)];
   return index == std::numeric_limits<size_t>::max()
      ? nullptr : g_registered_option_definitions[index].key;
}

static void register_core_options(void)
{
   build_core_option_definitions();
   struct retro_core_options_v2 options_v2 = {
      option_cats, g_registered_option_definitions.data()
   };
   environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options_v2);
}

static bool update_core_option_visibility(void)
{
   bool changed = false;
   const bool network_board_visible =
      g_has_active_input_game && g_active_input_game.netboard_present;
   if (g_network_board_option_visible != static_cast<int>(network_board_visible))
   {
      struct retro_core_option_display display = {
         "supermodel_network_board", network_board_visible
      };
      environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &display);
      g_network_board_option_visible = network_board_visible;
      changed = true;
   }
   if (g_network_cabinets_option_visible !=
       static_cast<int>(network_board_visible))
   {
      struct retro_core_option_display display = {
         "supermodel_network_cabinets", network_board_visible
      };
      environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &display);
      g_network_cabinets_option_visible = network_board_visible;
      changed = true;
   }

   const bool enabled =
      strcmp(option_get("supermodel_nvram_settings", "disabled"),
             "enabled") == 0;
   const auto *active_set = active_nvram_option_set();
   const std::string active_game = active_set ? active_set->game_name : "";
   if (g_visible_nvram_enabled != static_cast<int>(enabled) ||
       g_visible_nvram_game != active_game)
   {
      struct retro_core_option_display display = { nullptr, false };
      for (const auto &option_set : g_nvram_option_sets)
      {
         const bool game_visible =
            enabled && option_set.game_name == active_game;
         for (unsigned i = 0; i < kNvramSettingCount; ++i)
         {
            const size_t index = option_set.definition_indices[i];
            if (index == std::numeric_limits<size_t>::max())
               continue;
            display.key = g_registered_option_definitions[index].key;
            display.visible = game_visible;
            environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &display);
         }
      }

      g_visible_nvram_enabled = enabled;
      g_visible_nvram_game = active_game;
      changed = true;
   }

   return changed;
}

// GPU timer queries (double-buffered: write slot N, read slot N-1)
#if defined(CORE_GLES)
static GLuint s_gpuQuery[2]  = {0, 0};
#endif
static int    s_gpuSlot      = 0;
static float  s_gpuMs        = 0.0f;
static bool   s_gpuQueryOK   = false;

// Rolling frontend-side timings. CModel3's FrameTimings stop before the final
// blit, shader chain, frontend presentation and VSync wait.
static LibretroFrontendTimings s_frontendTimings;
static float s_accRun = 0.0f;
static float s_accCoreAndBlit = 0.0f;
static float s_accPresent = 0.0f;
static float s_accEngine = 0.0f;
static float s_accAudioSubmit = 0.0f;
static float s_accOverlay = 0.0f;
static float s_accBlit = 0.0f;
static float s_maxRun = 0.0f;
static float s_accFrameInterval = 0.0f;
static std::chrono::steady_clock::time_point s_previousFrameStart;
static bool s_havePreviousFrameStart = false;
static unsigned s_accPpc = 0;
static unsigned s_accRender = 0;
static unsigned s_renderedFrames = 0;
static unsigned s_timingFrames = 0;
static unsigned s_frameIntervals = 0;

// Path buffers
char retro_save_directory[4096];
char retro_base_directory[4096];

CoreOptions g_options = {
   /* initial_nvram_setup */ true,
   /* network_board       */ true,
   /* network_cabinets    */ 2,
   /* nvram_settings_enabled */ false,
   /* resolution_multiplier */ 1,
   /* renderer_3d          */ Renderer3D::New3D,
   /* quad_rendering       */ false,
   /* crt_colors           */ 0,
   /* supersampling        */ 1,
   /* upscale_mode          */ 2,
   /* widescreen_mode      */ WidescreenMode::Disabled,
   /* no_white_flash       */ false,
   /* av_timing_mode       */ AVTimingMode::Default60Hz,
   /* crosshairs           */ 0,
   /* force_feedback       */ true,
   /* steering_response    */ SteeringResponse::Linear,
   /* steering_output_range */ 100,
   /* accelerator_output_range_per_mille */ 1000,
   /* brake_output_range_per_mille */ 1000,
   /* sound_volume         */ 100,
   /* music_volume         */ 100,
   /* legacy_sound_dsp     */ false,
   /* ppc_frequency        */ 0,
   /* emulation_threading  */ EmulationThreading::MultiThreadedGPU,
   /* sound_enable         */ true,
   /* jit_enable           */
                              false,
   /* timing_overlay      */ false,
   /* gun_input           */ GunInput::Hybrid,
   /* star_wars_input     */ StarWarsInput::Hybrid,
   /* star_wars_upright_x_inversion */ true,
   /* four_speed_shifter  */ FourSpeedShifter::HGate,
};

static bool widescreen_enabled()
{
   return g_active_widescreen_mode != WidescreenMode::Disabled;
}

static bool wide_background_enabled()
{
   return g_active_widescreen_mode ==
          WidescreenMode::WidescreenWideBackground;
}

static unsigned scaled_native_dimension(unsigned value)
{
   const float multiplier = g_options.resolution_multiplier > 0.0f
      ? g_options.resolution_multiplier : 1.0f;
   return std::max(1u, static_cast<unsigned>(value * multiplier + 0.5f));
}

static unsigned widescreen_width(unsigned height)
{
   // Nearest integer to height * 16 / 9 without introducing a second scale
   // factor. Native 384-line output therefore becomes 683x384.
   return std::max(1u, (height * 16u + 4u) / 9u);
}

static void get_video_dimensions(unsigned &view_width,
                                 unsigned &view_height,
                                 unsigned &output_width,
                                 unsigned &output_height)
{
   view_width = scaled_native_dimension(496);
   view_height = scaled_native_dimension(384);
   output_height = view_height;
   output_width = widescreen_enabled()
      ? widescreen_width(output_height) : view_width;
}

static constexpr unsigned kMaximumVideoHeight = 384u * 4u;
static constexpr unsigned kMaximumVideoWidth =
   (kMaximumVideoHeight * 16u + 4u) / 9u;

// Optimization: Cache last known resolution to avoid redundant updates
static unsigned last_width = 0;
static unsigned last_height = 0;
#define NVRAM_BUFFER_SIZE (0x20000 + 2048) 
static uint8_t g_nvram_buffer[NVRAM_BUFFER_SIZE];
static constexpr int32_t NVRAM_FILE_VERSION = 0;
static constexpr const char* NVRAM_HEADER_BLOCK = "Supermodel NVRAM State";
static constexpr int32_t SAVE_STATE_FILE_VERSION = 6;
static constexpr const char* SAVE_STATE_HEADER_BLOCK = "Supermodel Save State";
static constexpr uint32_t SAVE_STATE_INTEGRITY_VERSION = 1;
static constexpr const char* SAVE_STATE_INTEGRITY_BLOCK = "Libretro Save State Integrity";
// Optimization: Cache save state size
static size_t g_cached_serialize_size = 0;
static bool g_first_run = true;
static bool g_nvram_initialized = false;
static bool g_context_ready = false;

// Exact curve incorporated by the Rad Mobile/Rad Rally MAME work. It is the
// System 32 FBNeo response retained there as the strongest fine-center preset.
static constexpr uint8_t kFBNeoLogarithmicSteeringCurve[0x100] = {
   0x00, 0x01, 0x13, 0x1d, 0x25, 0x2b, 0x2f, 0x33, 0x37, 0x3a, 0x3d, 0x3f, 0x41, 0x44, 0x46, 0x47,
   0x49, 0x4b, 0x4c, 0x4d, 0x4f, 0x50, 0x51, 0x52, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x59, 0x5a,
   0x5b, 0x5c, 0x5d, 0x5d, 0x5e, 0x5f, 0x60, 0x60, 0x61, 0x62, 0x62, 0x63, 0x63, 0x64, 0x65, 0x65,
   0x66, 0x66, 0x67, 0x67, 0x68, 0x68, 0x69, 0x69, 0x6a, 0x6a, 0x6b, 0x6b, 0x6c, 0x6c, 0x6c, 0x6d,
   0x6d, 0x6e, 0x6e, 0x6e, 0x6f, 0x6f, 0x70, 0x70, 0x70, 0x71, 0x71, 0x71, 0x72, 0x72, 0x72, 0x73,
   0x73, 0x73, 0x74, 0x74, 0x74, 0x75, 0x75, 0x75, 0x76, 0x76, 0x76, 0x76, 0x77, 0x77, 0x77, 0x78,
   0x78, 0x78, 0x78, 0x79, 0x79, 0x79, 0x79, 0x7a, 0x7a, 0x7a, 0x7a, 0x7b, 0x7b, 0x7b, 0x7b, 0x7c,
   0x7c, 0x7c, 0x7c, 0x7d, 0x7d, 0x7d, 0x7d, 0x7d, 0x7e, 0x7e, 0x7e, 0x7e, 0x7f, 0x7f, 0x7f, 0x80,
   0x80, 0x81, 0x81, 0x81, 0x82, 0x82, 0x82, 0x82, 0x83, 0x83, 0x83, 0x83, 0x83, 0x84, 0x84, 0x84,
   0x84, 0x85, 0x85, 0x85, 0x85, 0x86, 0x86, 0x86, 0x86, 0x87, 0x87, 0x87, 0x87, 0x88, 0x88, 0x88,
   0x88, 0x89, 0x89, 0x89, 0x8a, 0x8a, 0x8a, 0x8a, 0x8b, 0x8b, 0x8b, 0x8c, 0x8c, 0x8c, 0x8d, 0x8d,
   0x8d, 0x8e, 0x8e, 0x8e, 0x8f, 0x8f, 0x8f, 0x90, 0x90, 0x90, 0x91, 0x91, 0x92, 0x92, 0x92, 0x93,
   0x93, 0x94, 0x94, 0x94, 0x95, 0x95, 0x96, 0x96, 0x97, 0x97, 0x98, 0x98, 0x99, 0x99, 0x9a, 0x9a,
   0x9b, 0x9b, 0x9c, 0x9d, 0x9d, 0x9e, 0x9e, 0x9f, 0xa0, 0xa0, 0xa1, 0xa2, 0xa3, 0xa3, 0xa4, 0xa5,
   0xa6, 0xa7, 0xa7, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xae, 0xaf, 0xb0, 0xb1, 0xb3, 0xb4, 0xb5, 0xb7,
   0xb9, 0xba, 0xbc, 0xbf, 0xc1, 0xc3, 0xc6, 0xc9, 0xcd, 0xd1, 0xd5, 0xdb, 0xe3, 0xed, 0xff, 0xff,
};

static bool is_driving_profile(const Game &game)
{
   const LibretroInputProfiles::Profile *profile =
      LibretroInputProfiles::Find(game.inputs);
   return profile && profile->family == LibretroInputProfiles::Family::Driving;
}

static int scale_driving_pedal(UINT16 value, int output_range_per_mille)
{
   const int input = std::clamp(static_cast<int>(value), 0, 0xff);
   const int output_range = std::clamp(output_range_per_mille, 500, 1500);
   return std::clamp((input * output_range + 500) / 1000, 0, 0xff);
}

static void apply_driving_analog_ranges(const Game &game)
{
   if (!is_driving_profile(game) || !wrapper.Inputs)
      return;

   if (wrapper.Inputs->steering)
   {
      int target = std::clamp(static_cast<int>(wrapper.Inputs->steering->value), 0, 0xff);

      switch (g_options.steering_response)
      {
      case SteeringResponse::Progressive:
      {
         const int offset = target - 0x80;
         const int magnitude = std::abs(offset);
         const int side_range = offset < 0 ? 0x80 : 0x7f;
         const int curved = (magnitude * magnitude + side_range / 2) / side_range;
         target = 0x80 + (offset < 0 ? -curved : curved);
         break;
      }
      case SteeringResponse::FBNeoLogarithmic:
         target = kFBNeoLogarithmicSteeringCurve[target];
         break;
      case SteeringResponse::Linear:
         break;
      }

      // Below 100% this limits the emulated wheel range. Above 100% it acts as
      // saturation: full lock is reached before the physical stick's end stop.
      const int output_range = std::clamp(g_options.steering_output_range, 50, 150);
      target = std::clamp(0x80 + ((target - 0x80) * output_range) / 100, 0, 0xff);

      wrapper.Inputs->steering->value = static_cast<UINT16>(target);
   }

   if (wrapper.Inputs->accelerator)
      wrapper.Inputs->accelerator->value = static_cast<UINT16>(
         scale_driving_pedal(wrapper.Inputs->accelerator->value,
                             g_options.accelerator_output_range_per_mille));
   if (wrapper.Inputs->brake)
      wrapper.Inputs->brake->value = static_cast<UINT16>(
         scale_driving_pedal(wrapper.Inputs->brake->value,
                             g_options.brake_output_range_per_mille));
}

static void serialize_nvram(void)
{
   if (wrapper.getEmulator() == nullptr)
      return;

   memset(g_nvram_buffer, 0, sizeof(g_nvram_buffer));
   CBlockFileMemory memFile(g_nvram_buffer, NVRAM_BUFFER_SIZE);
   memFile.NewBlock(NVRAM_HEADER_BLOCK, "Supermodel Version " SUPERMODEL_VERSION);
   memFile.Write(&NVRAM_FILE_VERSION, sizeof(NVRAM_FILE_VERSION));
   memFile.Write(wrapper.getGame().name);
   wrapper.getEmulator()->SaveNVRAM(&memFile);
   memFile.Finish();
}

static bool unserialize_nvram(const char* source, bool allow_legacy_layout)
{
   CBlockFileMemory memFile(g_nvram_buffer, NVRAM_BUFFER_SIZE);

   // Current .srm files preserve the standalone .nv container. Accept the
   // older Libretro-only layout too, which began directly with the 93C46
   // block, so existing users do not lose their machine settings.
   if (memFile.FindBlock(NVRAM_HEADER_BLOCK) == Result::OKAY)
   {
      int32_t fileVersion = -1;
      if (memFile.Read(&fileVersion, sizeof(fileVersion)) != sizeof(fileVersion) ||
          fileVersion != NVRAM_FILE_VERSION)
      {
         log_cb(RETRO_LOG_ERROR, "[Supermodel] Incompatible NVRAM format in %s\n", source);
         return false;
      }
      log_cb(RETRO_LOG_INFO, "[Supermodel] Standalone-compatible NVRAM container found in %s\n", source);
   }
   else if (allow_legacy_layout && memFile.FindBlock("93C46") == Result::OKAY)
   {
      log_cb(RETRO_LOG_INFO, "[Supermodel] Legacy Libretro NVRAM container found in %s\n", source);
   }
   else
   {
      log_cb(RETRO_LOG_ERROR, "[Supermodel] Invalid NVRAM container in %s\n", source);
      return false;
   }

   wrapper.getEmulator()->LoadNVRAM(&memFile);
   return true;
}

enum class NvramPatchResult
{
   NotApplicable,
   Unchanged,
   Changed,
   Error,
};

static NvramPatchResult apply_nvram_selection(
      const LibretroNvramSettings::Selection &selection, const char *source)
{
   if (!selection.HasAny() || !g_has_active_input_game ||
       wrapper.getEmulator() == nullptr)
      return NvramPatchResult::NotApplicable;

   // Use Supermodel's public NVRAM block interface so backup RAM and all
   // EEPROM device state remain intact. The patcher changes only controlled
   // fields and regenerates the game-specific checksum when required.
   serialize_nvram();
   CBlockFileMemory reader(g_nvram_buffer, NVRAM_BUFFER_SIZE);
   uint16_t words[64];
   if (reader.FindBlock("93C46") != Result::OKAY ||
       reader.Read(words, sizeof(words)) != sizeof(words))
   {
      log_cb(RETRO_LOG_ERROR,
             "[Supermodel] Unable to read EEPROM for %s\n", source);
      return NvramPatchResult::Error;
   }
   std::vector<uint8_t> backup_ram(0x20000);
   if (reader.FindBlock("Backup RAM") != Result::OKAY ||
       reader.Read(backup_ram.data(),
                   static_cast<uint32_t>(backup_ram.size())) != backup_ram.size())
   {
      log_cb(RETRO_LOG_ERROR,
             "[Supermodel] Unable to read Backup RAM for %s\n", source);
      return NvramPatchResult::Error;
   }

   const LibretroNvramSettings::ApplyResult result =
      LibretroNvramSettings::Apply(g_active_input_game, words,
                                   backup_ram.data(), backup_ram.size(),
                                   selection);
   if (result == LibretroNvramSettings::ApplyResult::Unsupported)
      return NvramPatchResult::NotApplicable;
   if (result == LibretroNvramSettings::ApplyResult::InvalidLayout)
   {
      log_cb(RETRO_LOG_WARN,
             "[Supermodel] %s skipped: unexpected NVRAM layout for %s\n",
             source, g_active_input_game.name.c_str());
      return NvramPatchResult::Error;
   }
   if (result == LibretroNvramSettings::ApplyResult::Unchanged)
      return NvramPatchResult::Unchanged;

   CBlockFileMemory writer(g_nvram_buffer, NVRAM_BUFFER_SIZE);
   if (writer.FindBlock("93C46") != Result::OKAY)
      return NvramPatchResult::Error;
   writer.Write(words, sizeof(words));
   if (writer.FindBlock("Backup RAM") != Result::OKAY)
      return NvramPatchResult::Error;
   writer.Write(backup_ram.data(), static_cast<uint32_t>(backup_ram.size()));
   if (writer.HasError() || !unserialize_nvram(source, false))
   {
      log_cb(RETRO_LOG_ERROR,
             "[Supermodel] Unable to apply %s for %s\n",
             source, g_active_input_game.name.c_str());
      return NvramPatchResult::Error;
   }
   return NvramPatchResult::Changed;
}

static NvramPatchResult apply_initial_nvram_settings(void)
{
   if (!g_has_active_input_game || wrapper.getEmulator() == nullptr)
      return NvramPatchResult::NotApplicable;

   LibretroNvramSettings::Selection selection;
   const auto link_mode = LibretroNvramSettings::Setting::LinkMode;
   if (LibretroNvramSettings::GetSettingInfo(&g_active_input_game, link_mode))
      selection.values[static_cast<unsigned>(link_mode)] =
         LibretroNvramSettings::GetDefaultValue(&g_active_input_game,
                                                link_mode);

   const std::string &name = g_active_input_game.name;
   if (name == "swtrilgy" || name == "swtrilgya")
   {
      const auto cabinet = LibretroNvramSettings::Setting::Cabinet;
      selection.values[static_cast<unsigned>(cabinet)] = "upright";
   }

   if (!selection.HasAny())
      return NvramPatchResult::NotApplicable;

   const NvramPatchResult result = apply_nvram_selection(
      selection, "automatic initial NVRAM setup");
   if (result == NvramPatchResult::Changed ||
       result == NvramPatchResult::Unchanged)
   {
      log_cb(RETRO_LOG_INFO,
             "[Supermodel] Applied automatic initial NVRAM setup for %s\n",
             g_active_input_game.name.c_str());
   }
   return result;
}

static void update_star_wars_cabinet_orientation(void)
{
   g_star_wars_invert_x = false;
   const auto cabinet = LibretroNvramSettings::Setting::Cabinet;
   if (!g_has_active_input_game || wrapper.getEmulator() == nullptr ||
       !LibretroNvramSettings::IsValueSupported(
          &g_active_input_game, cabinet, "upright"))
      return;

   serialize_nvram();
   CBlockFileMemory reader(g_nvram_buffer, NVRAM_BUFFER_SIZE);
   uint16_t words[64];
   if (reader.FindBlock("93C46") != Result::OKAY ||
       reader.Read(words, sizeof(words)) != sizeof(words))
   {
      log_cb(RETRO_LOG_WARN,
             "[Supermodel] Unable to read Star Wars cabinet orientation\n");
      return;
   }

   g_star_wars_invert_x =
      LibretroNvramSettings::IsStarWarsUpright(&g_active_input_game, words);
   log_cb(RETRO_LOG_INFO,
          "[Supermodel] Star Wars cabinet: %s; analog X %s\n",
          g_star_wars_invert_x ? "Upright" : "Deluxe",
          g_star_wars_invert_x && g_options.star_wars_upright_x_inversion
             ? "inverted" : "normal");
}

static void initialize_new_nvram(void)
{
   // Export the pristine emulator state immediately so RetroArch always has a
   // valid save-RAM container, even when this game has no automatic settings.
   serialize_nvram();

   if (!g_options.initial_nvram_setup)
      return;

   const auto *initial =
      LibretroInitialNvram::Find(g_active_input_game.name);
   if (!initial)
      return;

   // A blank EEPROM has no game-owned structure for the field-level patcher
   // to validate. Seed only the 64 EEPROM words from this exact ROM set's
   // smoke-test NVRAM; retain the emulator's empty Backup RAM and controller
   // state, then apply the requested fields through the normal patch engine.
   CBlockFileMemory writer(g_nvram_buffer, NVRAM_BUFFER_SIZE);
   if (writer.FindBlock("93C46") != Result::OKAY)
   {
      log_cb(RETRO_LOG_WARN,
             "[Supermodel] Unable to seed initial EEPROM for %s\n",
             g_active_input_game.name.c_str());
      return;
   }
   writer.Write(initial->eeprom.data(),
                static_cast<uint32_t>(sizeof(uint16_t) * initial->eeprom.size()));
   if (writer.HasError() ||
       !unserialize_nvram("automatic initial NVRAM seed", false))
   {
      log_cb(RETRO_LOG_WARN,
             "[Supermodel] Unable to load initial EEPROM seed for %s\n",
             g_active_input_game.name.c_str());
      return;
   }

   const NvramPatchResult result = apply_initial_nvram_settings();
   if (result == NvramPatchResult::Error)
      log_cb(RETRO_LOG_WARN,
             "[Supermodel] Automatic initial NVRAM setup failed for %s\n",
             g_active_input_game.name.c_str());
}

static bool apply_configured_nvram_settings(void)
{
   if (!g_options.nvram_settings_enabled || !g_has_active_input_game ||
       wrapper.getEmulator() == nullptr)
      return false;

   LibretroNvramSettings::Selection selection;
   for (unsigned i = 0;
        i < static_cast<unsigned>(LibretroNvramSettings::Setting::Count); ++i)
   {
      const auto setting = static_cast<LibretroNvramSettings::Setting>(i);
      const char *key = active_nvram_option_key(setting);
      const char *default_value =
         LibretroNvramSettings::GetDefaultValue(&g_active_input_game, setting);
      const char *selected = key && default_value
         ? option_get(key, default_value) : nullptr;
      selection.values[i] = LibretroNvramSettings::IsValueSupported(
            &g_active_input_game, setting, selected)
         ? selected : default_value;
   }
   if (!selection.HasAny())
      return false;

   const NvramPatchResult result = apply_nvram_selection(
      selection, "game-aware NVRAM settings");
   if (result == NvramPatchResult::Changed)
      log_cb(RETRO_LOG_INFO,
             "[Supermodel] Applied configured NVRAM settings for %s\n",
             g_active_input_game.name.c_str());
   return result == NvramPatchResult::Changed;
}

static void build_native_nvram_path(char* path, size_t path_size)
{
   char filename[1024];
   snprintf(filename, sizeof(filename), "%s.nv", wrapper.getGame().name.c_str());
   fill_pathname_join(path, retro_save_directory, filename, path_size);
}

static bool native_nvram_exists(char* path, size_t path_size)
{
   build_native_nvram_path(path, path_size);
   RFILE* file = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ, 0);
   if (!file)
      return false;
   filestream_close(file);
   return true;
}

static bool import_native_nvram(const char* path)
{
   RFILE* file = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ, 0);
   if (!file)
      return false;

   const int64_t size = filestream_get_size(file);
   if (size <= 0 || size > static_cast<int64_t>(sizeof(g_nvram_buffer)))
   {
      log_cb(RETRO_LOG_ERROR,
             "[Supermodel] Native NVRAM has invalid size (%lld bytes): %s\n",
             static_cast<long long>(size), path);
      filestream_close(file);
      return false;
   }

   memset(g_nvram_buffer, 0, sizeof(g_nvram_buffer));
   const int64_t bytes_read = filestream_read(file, g_nvram_buffer, size);
   filestream_close(file);
   if (bytes_read != size)
   {
      memset(g_nvram_buffer, 0, sizeof(g_nvram_buffer));
      log_cb(RETRO_LOG_ERROR, "[Supermodel] Unable to read native NVRAM: %s\n", path);
      return false;
   }

   return true;
}

// PGO: defined only in -fprofile-generate builds (weak, so a normal build links
// fine and these are no-ops). RetroArch can tear the core down without running
// libgcov's destructors, which would silently drop the whole profile — so flush
// it explicitly at the points we know are reached.
#if defined(__GNUC__) && !defined(__APPLE__)
extern "C" void __gcov_dump(void) __attribute__((weak));

static void pgo_flush(void)
{
   if (__gcov_dump)
   {
      __gcov_dump();
      if (log_cb) log_cb(RETRO_LOG_INFO, "[PGO] profile flushed\n");
   }
}
#else
static void pgo_flush(void) {}
#endif

// --- Logging Helper ---
static void fallback_log(enum retro_log_level level, const char *fmt, ...)
{
   (void)level;
   va_list va;
   va_start(va, fmt);
   vfprintf(stderr, fmt, va);
   va_end(va);
}

// --- Core Lifecycle ---

void retro_init(void)
{
   struct retro_log_callback log;
   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
   else
      log_cb = fallback_log;

   const char *dir = NULL;

   // 1. Setup the core-specific system directory. Games.xml and other engine
   // assets live here; user preferences are provided by Libretro core options.
   if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir)
   {
      snprintf(retro_base_directory, sizeof(retro_base_directory), "%s/supermodel", dir);
   }
   else
   {
      snprintf(retro_base_directory, sizeof(retro_base_directory), "supermodel");
   }

   // 2. RetroArch may already return a core- or content-specific save path.
   // Do not append another Supermodel directory. SRAM is exposed through the
   // Libretro memory API and the frontend owns the actual .srm file.
   if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir)
   {
      snprintf(retro_save_directory, sizeof(retro_save_directory), "%s", dir);
   }
   else
   {
      snprintf(retro_save_directory, sizeof(retro_save_directory), "%s", retro_base_directory);
   }

   log_cb(RETRO_LOG_INFO, "[Supermodel] System Path: %s\n", retro_base_directory);
   log_cb(RETRO_LOG_INFO, "[Supermodel] Frontend Save Path: %s\n", retro_save_directory);

   if (CLibretroNetBoard::RegisterNetpacketInterface(environ_cb, log_cb))
      log_cb(RETRO_LOG_INFO,
             "[Supermodel] Libretro netpacket interface available.\n");
   else
      log_cb(RETRO_LOG_WARN,
             "[Supermodel] Libretro netpacket interface unavailable; linked cabinets are disabled.\n");

   bool can_dupe = true;
   environ_cb(RETRO_ENVIRONMENT_GET_CAN_DUPE, &can_dupe);

   // 3. Negotiate VFS — must happen before retro_load_game
   struct retro_vfs_interface_info vfs_info = { 1, nullptr };
   if (environ_cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfs_info))
       g_vfs_interface = vfs_info.iface;
}

void retro_deinit(void)
{
    CLibretroNetBoard::ShutdownNetpacketInterface();
    pgo_flush();
}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
   if (port >= 2)
      return;

   bool enabled;
   if (device == RETRO_DEVICE_JOYPAD)
      enabled = true;
   else if (device == kNoCabinetControlsDevice)
      enabled = false;
   else
      return;
   if (g_cabinet_controls_enabled[port] == enabled)
      return;

   g_cabinet_controls_enabled[port] = enabled;
   if (log_cb)
      log_cb(RETRO_LOG_INFO,
             "[Supermodel] Cabinet Test/Service slots %s on port %u\n",
             enabled ? "enabled" : "disabled", port + 1);

   if (g_has_active_input_game)
      set_input_descriptors(&g_active_input_game);
}

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "Supermodel";
   info->library_version  = "v0.3a-libretro";
   info->need_fullpath    = true;
   // GameLoader currently opens MAME-style ZIP sets. Do not advertise CHD or
   // 7z until the content path can actually consume those formats.
   info->valid_extensions = "zip";
   info->block_extract    = true;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   unsigned view_width, view_height, output_width, output_height;
   get_video_dimensions(view_width, view_height, output_width, output_height);
   info->geometry.base_width   = output_width;
   info->geometry.base_height  = output_height;
   info->geometry.max_width    = kMaximumVideoWidth;
   info->geometry.max_height   = kMaximumVideoHeight;
   info->geometry.aspect_ratio = widescreen_enabled()
      ? (16.0f / 9.0f) : (4.0f / 3.0f);

   info->timing.fps         = wrapper.GetFramesPerSecond();
   info->timing.sample_rate = LibretroTiming::kAudioSampleRate;
}

// --- OpenGL Context Management ---

void context_reset(void)
{
    auto emu = wrapper.getEmulator();
    if (!emu) return;

    emu->PauseThreads();
    g_context_ready = wrapper.InitGL();
    if (!g_context_ready)
        log_cb(RETRO_LOG_ERROR, "[Supermodel] OpenGL renderer initialization failed.\n");
    else
    {
        // The initial renderer already uses the selected core-option
        // resolution. Seed the cache so the first retro_run() does not send a
        // redundant SET_GEOMETRY that makes some frontends recreate GL.
        last_width = wrapper.getTotalXRes();
        last_height = wrapper.getTotalYRes();
    }

    // CRITICAL FIX: Force 1-byte alignment for textures.
    // This prevents the "split-screen" / ghosting on legacy drivers
    // when handling NPOT resolutions like 496x384.
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    Libretro_InitOverlay(Graphics::GLSLVersion::GetImGui().c_str());

    // GPU timer queries: EXT variant (GLES3 only), core variant TODO for desktop GL
    s_gpuQueryOK = false;
    s_gpuMs      = 0.0f;
    s_gpuSlot    = 0;
#if defined(CORE_GLES)
    if (glGenQueriesEXT && glBeginQueryEXT && glEndQueryEXT &&
        glGetQueryObjectuivEXT && glGetQueryObjectui64vEXT)
    {
        if (s_gpuQuery[0]) glDeleteQueriesEXT(2, s_gpuQuery);
        glGenQueriesEXT(2, s_gpuQuery);
        s_gpuQueryOK = (s_gpuQuery[0] != 0 && s_gpuQuery[1] != 0);
    }
#endif

    emu->ResumeThreads();
}

void context_destroy(void)
{
    g_context_ready = false;
#if defined(CORE_GLES)
    if (s_gpuQuery[0] && glDeleteQueriesEXT) { glDeleteQueriesEXT(2, s_gpuQuery); s_gpuQuery[0] = s_gpuQuery[1] = 0; }
#endif
    Libretro_ShutdownOverlay();
}

// --- Game Loading ---
bool retro_load_game(const struct retro_game_info *info)
{
   if (!info || !info->path || !info->path[0])
   {
      log_cb(RETRO_LOG_ERROR, "[Supermodel] A full content path is required.\n");
      return false;
   }

   // Reset all per-content state before the frontend loads save RAM for the
   // new game. Without this, a second game can inherit NVRAM and cached sizes
   // from the previous session when the core stays loaded.
   memset(g_nvram_buffer, 0, sizeof(g_nvram_buffer));
   g_cached_serialize_size = 0;
   g_first_run = true;
   g_nvram_initialized = false;
   last_width = 0;
   last_height = 0;
   g_context_ready = false;
   s_frontendTimings = {};
   s_accRun = s_accCoreAndBlit = s_accPresent = s_maxRun = 0.0f;
   s_accEngine = s_accAudioSubmit = s_accOverlay = s_accBlit = 0.0f;
   s_accFrameInterval = 0.0f;
   s_havePreviousFrameStart = false;
   s_accPpc = s_accRender = s_renderedFrames = s_timingFrames = 0;
   s_frameIntervals = 0;

   // The selected renderer determines which OpenGL profile the frontend must
   // create, so options have to be read before SET_HW_RENDER is negotiated.
   update_core_options();

   hw_render.context_reset   = context_reset;
   hw_render.context_destroy = context_destroy;
   hw_render.depth           = true;
   hw_render.stencil         = true;
   hw_render.bottom_left_origin = true;

#if defined(ANDROID) || defined(CORE_GLES)
   hw_render.context_type = RETRO_HW_CONTEXT_OPENGLES3;
   hw_render.version_major = 3;
   hw_render.version_minor = 0;
#else
   // Legacy3D uses fixed-function compatibility entry points. New3D uses a
   // core profile, raised from 4.1 to 4.5 when geometry-shader-based native
   // quad rendering is requested.
   const bool active_quad_rendering =
      g_options.renderer_3d == Renderer3D::New3D &&
      g_options.quad_rendering;
#ifdef HAVE_LEGACY3D
   if (g_options.renderer_3d == Renderer3D::Legacy3D)
   {
      hw_render.context_type = RETRO_HW_CONTEXT_OPENGL;
      hw_render.version_major = 0;
      hw_render.version_minor = 0;
   }
   else
#endif
   {
      hw_render.context_type = RETRO_HW_CONTEXT_OPENGL_CORE;
      hw_render.version_major = 4;
      hw_render.version_minor = active_quad_rendering ? 5 : 1;
   }
#endif
   log_cb(RETRO_LOG_INFO,
          "[Supermodel] Renderer context: %s, OpenGL %u.%u%s\n",
          g_options.renderer_3d == Renderer3D::Legacy3D ?
             "Legacy3D compatibility" : "New3D core",
          hw_render.version_major, hw_render.version_minor,
          g_options.renderer_3d == Renderer3D::New3D &&
             g_options.quad_rendering ?
                ", Quad Rendering enabled" : "");
   if (!environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render)) {
       log_cb(RETRO_LOG_ERROR, "[Supermodel] HW Render Context negotiation failed.\n");
       return false;
   }

   // 1. Attempt to get the Rumble Interface from the frontend
   if (environ_cb(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumble))
   {
      // Cast the base shared_ptr to the specific derived shared_ptr
      auto libretroInput = std::static_pointer_cast<CLibretroInputSystem>(wrapper.getInputSystem());
      
      if (libretroInput) {
         libretroInput->SetRumbleInterface(rumble);
         libretroInput->SetFFBEnabled(g_options.force_feedback);
      }
   }
   else
   {
        // Optional: Log that rumble is not supported by this frontend/controller
   }

   
   g_active_widescreen_mode = g_options.widescreen_mode;
   g_active_gun_input = g_options.gun_input;
   g_active_star_wars_input = g_options.star_wars_input;
#ifdef HAVE_PPC_JIT
   ppc_set_jit_enabled(g_options.jit_enable);
#endif
   wrapper.InitializePaths(retro_base_directory);
   wrapper.setHwRender(hw_render); 

   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
      return false;

   log_cb(RETRO_LOG_INFO, "[Supermodel] Loading ROM: %s\n", info->path);
      
   int emulation = wrapper.Emulate(info->path);
   if (emulation != 0) return false;
   const Game loaded_game = wrapper.getGame();
   g_active_input_game = loaded_game;
   g_has_active_input_game = true;
   update_core_option_visibility();
   set_input_descriptors(&loaded_game);
   set_controller_info(loaded_game);
   wrapper.SetWidescreen(widescreen_enabled(), wide_background_enabled());
   if (wrapper.SuperModelInit(wrapper.getGame()) != 0)
   {
      log_cb(RETRO_LOG_ERROR, "[Supermodel] Emulator initialization failed.\n");
      wrapper.ShutDownSupermodel();
      return false;
   }
   // Re-apply FFB state after full DriveBoard initialization
   auto libretroInput2 = std::static_pointer_cast<CLibretroInputSystem>(wrapper.getInputSystem());
   if (libretroInput2 && g_options.force_feedback) {
      libretroInput2->SetFFBEnabled(false);   // force a state transition
      libretroInput2->SetFFBEnabled(true);
   }

   return true;
}
void retro_unload_game(void)
{
   // Save NVRAM to buffer before shutdown (RetroArch will write it to .srm)
   if (g_nvram_initialized && wrapper.getEmulator() != nullptr)
   {
       log_cb(RETRO_LOG_INFO, "[Supermodel] Saving NVRAM to .srm file\n");
       
       serialize_nvram();
   }
   
   wrapper.ShutDownSupermodel();
   g_cached_serialize_size = 0;
   g_first_run = true;
   g_nvram_initialized = false;
   last_width = 0;
   last_height = 0;
   g_context_ready = false;
   g_has_active_input_game = false;
   g_star_wars_invert_x = false;
   update_core_option_visibility();
   set_input_descriptors(nullptr);
   pgo_flush();   // RetroArch may never call retro_deinit before exiting
}
void retro_run(void)
{
   const auto t_frame_start = std::chrono::steady_clock::now();
   if (s_havePreviousFrameStart)
   {
      s_accFrameInterval += std::chrono::duration<float, std::milli>(
          t_frame_start - s_previousFrameStart).count();
      ++s_frameIntervals;
   }
   s_previousFrameStart = t_frame_start;
   s_havePreviousFrameStart = true;

   // SET_HW_RENDER has no synchronous error return for context_reset(). Avoid
   // entering the engine with unattached renderers if GL/FBO setup failed.
   if (!g_context_ready)
   {
      if (video_cb)
         video_cb(nullptr, 0, 0, 0);
      return;
   }

   // Check if options were changed
   bool options_updated = false;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &options_updated) && options_updated)
   {
      float old_multiplier = g_options.resolution_multiplier;
      bool old_network_board = g_options.network_board;
      unsigned old_network_cabinets = g_options.network_cabinets;
      Renderer3D old_renderer_3d = g_options.renderer_3d;
      bool old_quad_rendering = g_options.quad_rendering;
      int old_crt_colors = g_options.crt_colors;
      int old_supersampling = g_options.supersampling;
      WidescreenMode old_widescreen_mode = g_options.widescreen_mode;
      bool old_no_white_flash = g_options.no_white_flash;
      AVTimingMode old_av_timing_mode = g_options.av_timing_mode;
      int old_upscale_mode = g_options.upscale_mode;
      bool old_legacy_sound_dsp = g_options.legacy_sound_dsp;
      unsigned old_crosshairs = g_options.crosshairs;
      GunInput old_gun_input = g_options.gun_input;
      StarWarsInput old_star_wars_input = g_options.star_wars_input;
      bool old_star_wars_upright_x_inversion =
         g_options.star_wars_upright_x_inversion;
      FourSpeedShifter old_four_speed_shifter =
         g_options.four_speed_shifter;
      EmulationThreading old_emulation_threading =
         g_options.emulation_threading;
#ifdef HAVE_PPC_JIT
      bool old_jit_enable = g_options.jit_enable;
#endif
      update_core_options();
      update_core_option_visibility();

      if (g_nvram_initialized && apply_configured_nvram_settings())
      {
         static const struct retro_message message = {
            "NVRAM settings saved. Restart content to apply them.", 180
         };
         environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, (void *)&message);
      }

      if (g_options.renderer_3d != old_renderer_3d ||
          g_options.quad_rendering != old_quad_rendering ||
          g_options.crt_colors != old_crt_colors ||
          g_options.supersampling != old_supersampling ||
          g_options.no_white_flash != old_no_white_flash)
      {
         static const struct retro_message message = {
            "Renderer and supersampling changes require restarting the content.", 240
         };
         environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, (void *)&message);
      }

      if (g_options.emulation_threading != old_emulation_threading)
      {
         static const struct retro_message message = {
            "Emulation Threading will apply after restarting the content.", 180
         };
         environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, (void *)&message);
      }

      if (g_options.network_board != old_network_board ||
          g_options.network_cabinets != old_network_cabinets)
      {
         static const struct retro_message message = {
            "Network Board settings will apply after restarting the content.", 180
         };
         environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, (void *)&message);
      }

#ifdef HAVE_PPC_JIT
      if (g_options.jit_enable != old_jit_enable)
      {
         static const struct retro_message message = {
            "ARM64 JIT Recompiler will apply after restarting the content.", 180
         };
         environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, (void *)&message);
      }
#endif

      if (g_options.widescreen_mode != old_widescreen_mode)
      {
         static const struct retro_message message = {
            "Widescreen Mode will apply after restarting the content.", 180
         };
         environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, (void *)&message);
      }

      if (g_options.av_timing_mode != old_av_timing_mode)
      {
         static const struct retro_message message = {
            "Model 3 Timing Mode will apply after restarting the content.", 180
         };
         environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, (void *)&message);
      }

      if (g_options.upscale_mode != old_upscale_mode ||
          g_options.legacy_sound_dsp != old_legacy_sound_dsp)
      {
         static const struct retro_message message = {
            "Renderer/audio engine changes will apply after restarting the content.", 180
         };
         environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, (void *)&message);
      }

      if (g_options.resolution_multiplier != old_multiplier)
      {
         last_width  = 0;
         last_height = 0;

         struct retro_system_av_info av_info;
         retro_get_system_av_info(&av_info);
         environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av_info);
      }

      if (g_options.gun_input != old_gun_input)
      {
         g_active_gun_input = g_options.gun_input;
         if (g_has_active_input_game)
         {
            set_input_descriptors(&g_active_input_game);
            set_controller_info(g_active_input_game);
         }
         if (log_cb)
            log_cb(RETRO_LOG_INFO,
                   "[Supermodel] Gun Input applied immediately.\n");
      }

      if (g_options.star_wars_input != old_star_wars_input)
      {
         g_active_star_wars_input = g_options.star_wars_input;
         if (g_has_active_input_game)
         {
            set_input_descriptors(&g_active_input_game);
            set_controller_info(g_active_input_game);
         }
         if (log_cb)
            log_cb(RETRO_LOG_INFO,
                   "[Supermodel] Star Wars Trilogy Input applied immediately.\n");
      }

      if (g_options.star_wars_upright_x_inversion !=
          old_star_wars_upright_x_inversion && log_cb)
      {
         log_cb(RETRO_LOG_INFO,
                "[Supermodel] Star Wars Upright X-axis inversion %s.\n",
                g_options.star_wars_upright_x_inversion
                   ? "enabled" : "disabled");
      }

      if (g_options.four_speed_shifter != old_four_speed_shifter)
      {
         if (g_has_active_input_game)
            set_input_descriptors(&g_active_input_game);
         if (log_cb)
            log_cb(RETRO_LOG_INFO,
                   "[Supermodel] 4-Speed Shifter applied immediately.\n");
      }

      if (g_options.crosshairs != old_crosshairs)
         wrapper.SetCrosshairs(g_options.crosshairs);

      wrapper.SetSoundVolume(g_options.sound_volume);
      wrapper.SetMusicVolume(g_options.music_volume);


      auto libretroInput = std::static_pointer_cast<CLibretroInputSystem>(wrapper.getInputSystem());
      if (libretroInput) {
         libretroInput->SetFFBEnabled(g_options.force_feedback);

         // If we just disabled it, kill any active vibration immediately
         if (!g_options.force_feedback)
            libretroInput->StopAllRumble();
      }
   }

   // NVRAM Loading: Do this on first frame, AFTER RetroArch has loaded .srm
   if (g_first_run)
   {
      g_first_run = false;

      // Apply initial volume settings now that the emulator is fully initialized
      wrapper.SetSoundVolume(g_options.sound_volume);
      wrapper.SetMusicVolume(g_options.music_volume);
        
      log_cb(RETRO_LOG_INFO, "[Supermodel] First frame - checking NVRAM buffer...\n");
        
      // Check if buffer has valid block file data (first 16 bytes shouldn't all be zero)
      bool has_nvram = false;
      for (int i = 0; i < 16 && !has_nvram; i++)
         has_nvram = (g_nvram_buffer[i] != 0);
        
      char native_nvram_path[4096];
      const bool has_native_nvram = native_nvram_exists(
            native_nvram_path, sizeof(native_nvram_path));

      if (has_nvram)
      {
         log_cb(RETRO_LOG_INFO, "[Supermodel] Loading NVRAM from frontend .srm save RAM\n");
         if (has_native_nvram)
            log_cb(RETRO_LOG_WARN,
                   "[Supermodel] Both .srm save RAM and native .nv found; using .srm and ignoring %s\n",
                   native_nvram_path);

         g_nvram_initialized = unserialize_nvram("frontend .srm save RAM", true);
      }
      else if (has_native_nvram)
      {
         log_cb(RETRO_LOG_INFO,
                "[Supermodel] No frontend .srm data; importing native NVRAM from %s\n",
                native_nvram_path);
         g_nvram_initialized = import_native_nvram(native_nvram_path) &&
               unserialize_nvram("native .nv file", false);
         if (!g_nvram_initialized)
         {
            memset(g_nvram_buffer, 0, sizeof(g_nvram_buffer));
            log_cb(RETRO_LOG_WARN,
                   "[Supermodel] Native .nv is invalid; ignoring it and initializing new .srm save RAM\n");
            g_nvram_initialized = true;
            initialize_new_nvram();
         }
      }
      else
      {
         log_cb(RETRO_LOG_INFO, "[Supermodel] No NVRAM data found, using defaults\n");
         g_nvram_initialized = true;
         initialize_new_nvram();
      }

      apply_configured_nvram_settings();
      update_star_wars_cabinet_orientation();
   }

    if (input_poll_cb) input_poll_cb();

   unsigned view_w, view_h, target_w, target_h;
   get_video_dimensions(view_w, view_h, target_w, target_h);

   // OPTIMIZATION: Only update screen size if it actually changed.
   if (target_w != last_width || target_h != last_height) {
      wrapper.UpdateScreenSize(view_w, view_h, target_w, target_h);
      
      struct retro_game_geometry geometry;
      geometry.base_width   = target_w;
      geometry.base_height  = target_h;
      geometry.max_width    = kMaximumVideoWidth;
      geometry.max_height   = kMaximumVideoHeight;
      geometry.aspect_ratio = widescreen_enabled()
         ? (16.0f / 9.0f) : (4.0f / 3.0f);

      environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geometry);
      
      last_width  = target_w;
      last_height = target_h;
   }

   Game game = wrapper.getGame();
   wrapper.Inputs->Poll(&game, wrapper.getXOffset(), wrapper.getYOffset(),
                        view_w, view_h);
   apply_driving_analog_ranges(game);

   GLuint sm_fbo = wrapper.getSuperModelFBO();

   {
      // Reset GL state, clear the FBO, and run emulation plus rendering.
      glBindFramebuffer(GL_FRAMEBUFFER, sm_fbo);

      glDisable(GL_SCISSOR_TEST);
      glDisable(GL_STENCIL_TEST);
      glEnable(GL_DEPTH_TEST);
      glDepthFunc(GL_LESS);
      glDepthMask(GL_TRUE);
      glDisable(GL_BLEND);
      glDisable(GL_CULL_FACE);
      glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

      glViewport(0, 0, target_w, target_h);
      // Render2D re-enables the scissor test after clearing its target. The
      // box is global GL state and must describe the supersampled render FBO,
      // exactly as standalone scales its scissor by aaValue. Leaving this at
      // base resolution clips a 2x source to its lower-left quarter before
      // SuperAA resolves it.
      const unsigned render_scale = static_cast<unsigned>(
         std::max(1, wrapper.getAaValue()));
      glScissor(0, 0, target_w * render_scale, target_h * render_scale);

      glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
      glClearDepth(1.0);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

#if defined(CORE_GLES)
      // Collect previous frame's GPU time (non-blocking: result from 1 frame ago)
      if (s_gpuQueryOK) {
          int readSlot = s_gpuSlot ^ 1;
          GLuint available = 0;
          glGetQueryObjectuivEXT(s_gpuQuery[readSlot], GL_QUERY_RESULT_AVAILABLE_EXT, &available);
          if (available) {
              GLuint64 elapsed = 0;
              glGetQueryObjectui64vEXT(s_gpuQuery[readSlot], GL_QUERY_RESULT_EXT, &elapsed);
              s_gpuMs = elapsed / 1000000.0f;
          }
          glBeginQueryEXT(GL_TIME_ELAPSED_EXT, s_gpuQuery[s_gpuSlot]);
      }
#endif

      wrapper.Supermodel(game);

#if defined(CORE_GLES)
      if (s_gpuQueryOK) {
          glEndQueryEXT(GL_TIME_ELAPSED_EXT);
          s_gpuSlot ^= 1;
      }
#endif

      glViewport(0, 0, target_w, target_h);
      // REMOVED: glFlush() - not needed before glBlitFramebuffer and causes unnecessary GPU stall

      // Draw diagnostics into Supermodel's own target. Drawing into the
      // frontend FBO after the blit is not portable: some hardware frontends
      // replace or resolve that target inside video_cb().
      const FrameTimings t = wrapper.GetTimings();
      const auto t_overlay_start = std::chrono::steady_clock::now();
      if (g_options.timing_overlay)
      {
         glBindFramebuffer(GL_FRAMEBUFFER, sm_fbo);
         Libretro_DrawTimingOverlay(t, s_frontendTimings,
                                    target_w, target_h,
                                    wrapper.GetFramesPerSecond(), s_gpuMs);
      }
      const auto t_overlay_end = std::chrono::steady_clock::now();

      // Blit from Supermodel FBO to RetroArch's framebuffer
      const auto t_blit_start = std::chrono::steady_clock::now();
      GLuint ra_fbo = wrapper.getHwRender().get_current_framebuffer();

      glBindFramebuffer(GL_READ_FRAMEBUFFER, sm_fbo);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ra_fbo);

      glDisable(GL_SCISSOR_TEST);
      glDisable(GL_STENCIL_TEST);
      glDisable(GL_DEPTH_TEST);

      glBlitFramebuffer(
          0, 0, target_w, target_h,
          0, 0, target_w, target_h,
          GL_COLOR_BUFFER_BIT,
          GL_LINEAR
      );

      glBindFramebuffer(GL_FRAMEBUFFER, ra_fbo);
      const auto t_blit_end = std::chrono::steady_clock::now();

      // Everything above (overlay + blit) plus video_cb below is invisible to
      // CModel3's own "Total" — and it is exactly what the standalone build does
      // not do. video_cb is where RetroArch runs its shader chain (a curved CRT
      // preset at display resolution is not free on V3D) and waits for vsync.
      const auto t_post = std::chrono::steady_clock::now();
      video_cb(RETRO_HW_FRAME_BUFFER_VALID, target_w, target_h, 0);
      const auto t_end = std::chrono::steady_clock::now();

      auto ms = [](auto a, auto b) {
         return std::chrono::duration<float, std::milli>(b - a).count();
      };

      const float emu     = ms(t_frame_start, t_post);
      const float present = ms(t_post, t_end);

      s_accCoreAndBlit += emu;
      s_accPresent += present;
      s_accEngine += wrapper.GetLastEngineMs();
      s_accAudioSubmit += wrapper.GetLastAudioSubmitMs();
      s_accOverlay += ms(t_overlay_start, t_overlay_end);
      s_accBlit += ms(t_blit_start, t_blit_end);
      s_accRun += emu + present;
      if (emu + present > s_maxRun) s_maxRun = emu + present;
      s_accPpc += t.ppcTicks;
      s_accRender += t.renderTicks;
      s_renderedFrames++;

      if (++s_timingFrames >= 60) {
         const float n = (float)s_timingFrames;
         s_frontendTimings.engineMs = s_accEngine / n;
         s_frontendTimings.audioSubmitMs = s_accAudioSubmit / n;
         s_frontendTimings.overlayMs = s_accOverlay / n;
         s_frontendTimings.blitMs = s_accBlit / n;
         s_frontendTimings.coreAndBlitMs = s_accCoreAndBlit / n;
         s_frontendTimings.otherMs = std::max(
             0.0f, s_frontendTimings.coreAndBlitMs
                 - s_frontendTimings.engineMs
                 - s_frontendTimings.audioSubmitMs
                 - s_frontendTimings.overlayMs
                 - s_frontendTimings.blitMs);
         s_frontendTimings.presentMs = s_accPresent / n;
         s_frontendTimings.retroRunMs = s_accRun / n;
         s_frontendTimings.worstRetroRunMs = s_maxRun;
         s_frontendTimings.actualFps =
             s_frameIntervals && s_accFrameInterval > 0.0f
                 ? 1000.0f / (s_accFrameInterval /
                              static_cast<float>(s_frameIntervals))
                 : 0.0f;
         if (g_options.timing_overlay)
            log_cb(RETRO_LOG_INFO,
                   "[Timing] avg over %u frames | PPC:%4.1f  Render:%4.1f (%u drawn)  "
                   "engine:%5.1f  audio:%4.1f  overlay:%4.1f  blit:%4.1f  other:%4.1f  "
                   "core+blit:%5.1f  present:%5.1f  retro_run:%5.1f  worst:%5.1f ms  "
                   "actual:%5.1f FPS  engine-cap:%5.1f FPS  callback-cap:%5.1f FPS\n",
                   s_timingFrames,
                   s_accPpc / n,
                   s_renderedFrames ? s_accRender / (float)s_renderedFrames : 0.0f,
                   s_renderedFrames,
                   s_frontendTimings.engineMs,
                   s_frontendTimings.audioSubmitMs,
                   s_frontendTimings.overlayMs,
                   s_frontendTimings.blitMs,
                   s_frontendTimings.otherMs,
                   s_frontendTimings.coreAndBlitMs,
                   s_frontendTimings.presentMs,
                   s_frontendTimings.retroRunMs,
                   s_frontendTimings.worstRetroRunMs,
                   s_frontendTimings.actualFps,
                   s_frontendTimings.engineMs > 0.0f
                       ? 1000.0f / s_frontendTimings.engineMs : 0.0f,
                   s_frontendTimings.retroRunMs > 0.0f
                       ? 1000.0f / s_frontendTimings.retroRunMs : 0.0f);
         s_timingFrames = s_renderedFrames = 0;
         s_accRun = s_accCoreAndBlit = s_accPresent = s_maxRun = 0.0f;
         s_accEngine = s_accAudioSubmit = s_accOverlay = s_accBlit = 0.0f;
         s_accFrameInterval = 0.0f;
         s_accPpc = s_accRender = 0;
         s_frameIntervals = 0;
      }
   }
}

// --- Save States ---

static void write_save_state(CBlockFile& state,
                             bool includeHeader,
                             bool includeIntegrity)
{
    if (includeHeader)
    {
        state.NewBlock(SAVE_STATE_HEADER_BLOCK,
                       "Supermodel Version " SUPERMODEL_VERSION);
        state.Write(&SAVE_STATE_FILE_VERSION, sizeof(SAVE_STATE_FILE_VERSION));
        state.Write(wrapper.getGame().name);
    }
    wrapper.getEmulator()->SaveState(&state);
    if (includeIntegrity)
    {
        const uint32_t emptyChecksum = 0;
        state.NewBlock(SAVE_STATE_INTEGRITY_BLOCK,
                       "Libretro state truncation and corruption detection");
        state.Write(&SAVE_STATE_INTEGRITY_VERSION,
                    sizeof(SAVE_STATE_INTEGRITY_VERSION));
        state.Write(&emptyChecksum, sizeof(emptyChecksum));
    }
}

size_t retro_serialize_size(void)
{
    if (g_cached_serialize_size > 0)
        return g_cached_serialize_size;
    
    if (wrapper.getEmulator() != nullptr)
    {
        CBlockFileCounter counter;
        write_save_state(counter, true, true);
        g_cached_serialize_size = counter.GetSize();
    }
    return g_cached_serialize_size;
}

bool retro_serialize(void* data, size_t size)
{
    if (!data || wrapper.getEmulator() == nullptr)
        return false;

    const size_t requiredSize = retro_serialize_size();
    if (requiredSize == 0 || size < requiredSize)
    {
        log_cb(RETRO_LOG_ERROR,
               "[Supermodel] Save State buffer is too small (%zu supplied, %zu required)\n",
               size, requiredSize);
        return false;
    }

    CBlockFileMemory mem(data, requiredSize);
    write_save_state(mem, true, true);
    if (!mem.Finish() || mem.GetOffset() != requiredSize)
    {
        log_cb(RETRO_LOG_ERROR, "[Supermodel] Failed to serialize complete Save State\n");
        return false;
    }

    CBlockFileMemory integrityLocator(
        static_cast<const void*>(data), requiredSize);
    if (integrityLocator.FindBlock(SAVE_STATE_INTEGRITY_BLOCK) != Result::OKAY)
        return false;
    uint32_t integrityVersion = 0;
    if (integrityLocator.Read(&integrityVersion, sizeof(integrityVersion)) !=
            sizeof(integrityVersion) ||
        integrityVersion != SAVE_STATE_INTEGRITY_VERSION)
        return false;

    const size_t checksumOffset = integrityLocator.GetOffset();
    if (checksumOffset > std::numeric_limits<uInt>::max())
        return false;
    uLong checksum = crc32(0L, Z_NULL, 0);
    checksum = crc32(checksum, static_cast<const Bytef*>(data),
                     static_cast<uInt>(checksumOffset));
    const uint32_t storedChecksum = static_cast<uint32_t>(checksum);
    std::memcpy(static_cast<uint8_t*>(data) + checksumOffset,
                &storedChecksum, sizeof(storedChecksum));
    return true;
}

bool retro_unserialize(const void* data, size_t size)
{
    if (!data || size == 0 || wrapper.getEmulator() == nullptr)
        return false;

    // Preflight the complete block layout before touching emulator state.
    // This prevents a truncated or structurally corrupt buffer from leaving
    // the running machine partially restored.
    CBlockFileCounter currentLayout;
    write_save_state(currentLayout, true, true);
    CBlockFileCounter standaloneLayout;
    write_save_state(standaloneLayout, true, false);
    CBlockFileCounter legacyLayout;
    write_save_state(legacyLayout, false, false);

    CBlockFileMemory mem(data, size);
    const bool hasCurrentLayout = mem.ValidateLayout(currentLayout.GetLayout());
    const bool hasStandaloneLayout = !hasCurrentLayout &&
        mem.ValidateLayout(standaloneLayout.GetLayout());
    const bool hasLegacyLayout = !hasCurrentLayout && !hasStandaloneLayout &&
        mem.ValidateLayout(legacyLayout.GetLayout());
    if (!hasCurrentLayout && !hasStandaloneLayout && !hasLegacyLayout)
    {
        log_cb(RETRO_LOG_ERROR,
               "[Supermodel] Save State has an invalid or incompatible block layout\n");
        return false;
    }

    if (hasCurrentLayout || hasStandaloneLayout)
    {
        if (mem.FindBlock(SAVE_STATE_HEADER_BLOCK) != Result::OKAY)
            return false;

        int32_t fileVersion = -1;
        const std::string& expectedGame = wrapper.getGame().name;
        std::vector<char> storedGame(expectedGame.size() + 1, '\0');
        if (mem.Read(&fileVersion, sizeof(fileVersion)) != sizeof(fileVersion) ||
            mem.Read(storedGame.data(), static_cast<uint32_t>(storedGame.size())) !=
                storedGame.size() ||
            mem.HasError() ||
            fileVersion != SAVE_STATE_FILE_VERSION ||
            storedGame.back() != '\0' ||
            expectedGame != storedGame.data())
        {
            log_cb(RETRO_LOG_ERROR,
                   "[Supermodel] Save State version or ROM set does not match the running content\n");
            return false;
        }

        if (hasCurrentLayout)
        {
            if (mem.FindBlock(SAVE_STATE_INTEGRITY_BLOCK) != Result::OKAY)
                return false;
            uint32_t integrityVersion = 0;
            uint32_t storedChecksum = 0;
            if (mem.Read(&integrityVersion, sizeof(integrityVersion)) !=
                    sizeof(integrityVersion) ||
                integrityVersion != SAVE_STATE_INTEGRITY_VERSION)
                return false;
            const size_t checksumOffset = mem.GetOffset();
            if (mem.Read(&storedChecksum, sizeof(storedChecksum)) !=
                    sizeof(storedChecksum))
                return false;
            if (checksumOffset > std::numeric_limits<uInt>::max())
                return false;
            uLong checksum = crc32(0L, Z_NULL, 0);
            checksum = crc32(checksum, static_cast<const Bytef*>(data),
                             static_cast<uInt>(checksumOffset));
            const uint32_t calculatedChecksum =
                static_cast<uint32_t>(checksum);
            if (storedChecksum != calculatedChecksum)
            {
                log_cb(RETRO_LOG_ERROR,
                       "[Supermodel] Save State checksum mismatch; state is truncated or corrupt\n");
                return false;
            }
        }
        else
        {
            log_cb(RETRO_LOG_INFO,
                   "[Supermodel] Loading standalone-compatible Save State without Libretro checksum\n");
        }
    }
    else
    {
        log_cb(RETRO_LOG_WARN,
               "[Supermodel] Loading legacy Libretro Save State without version or ROM-set metadata\n");
    }

    wrapper.getEmulator()->LoadState(&mem);
    if (mem.HasError())
    {
        log_cb(RETRO_LOG_ERROR, "[Supermodel] Save State data ended unexpectedly while loading\n");
        return false;
    }
    return true;
}

// --- Input Descriptors & Callbacks ---
void set_controller_info(const Game &game)
{
   static retro_controller_description descriptions[4];
   static retro_controller_info ports[3];
   static char cabinet_device_names[2][192];

   const LibretroInputProfiles::Profile *profile =
      LibretroInputProfiles::Find(game.inputs);
   const char *device_name = profile ? profile->name : "RetroPad (generic fallback)";
   if (profile && profile->family == LibretroInputProfiles::Family::Gun)
   {
      switch (g_active_gun_input)
      {
      case GunInput::Hybrid:
         device_name = "Gun";
         break;
      case GunInput::Lightgun:
         device_name = "Gun (Lightgun)";
         break;
      case GunInput::Mouse:
         device_name = "Gun (Mouse)";
         break;
      case GunInput::MouseAnalog:
         device_name = "Gun (Mouse + Analog Stick)";
         break;
      case GunInput::AnalogSticks:
         device_name = "Gun (Analog Sticks)";
         break;
      }
   }
   for (unsigned port = 0; port < 2; ++port)
   {
      const bool gameplay_port = !profile || port < profile->players;
      const char *base_name = gameplay_port ? device_name : "Common Controls B";
      snprintf(cabinet_device_names[port], sizeof(cabinet_device_names[port]),
               "%s + Test/Service slots", base_name);

      retro_controller_description *types = &descriptions[port * 2];
      // The base RetroPad is RetroArch's default device. Give it the complete
      // profile, including remappable Test/Service slots; the reduced profile
      // remains available as an explicit device subclass.
      types[0] = { cabinet_device_names[port], RETRO_DEVICE_JOYPAD };
      types[1] = { base_name, kNoCabinetControlsDevice };
      ports[port] = { types, 2 };
   }
   ports[2] = { nullptr, 0 };

   environ_cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, ports);
}

void set_input_descriptors(const Game *game)
{
   std::vector<retro_input_descriptor> desc;
   desc.reserve(40);

   const auto add = [&desc](unsigned port, unsigned device, unsigned index,
                           unsigned id, const char *description)
   {
      desc.push_back({ port, device, index, id, description });
   };

   const auto add_common = [&add](unsigned port, const char *start_description = "Start")
   {
      add(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, start_description);
      add(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Coin");
      if (g_cabinet_controls_enabled[port])
      {
         add(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,
             port == 0 ? "Test A" : "Test B");
         add(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,
             port == 0 ? "Service A" : "Service B");
      }
   };

   const auto add_standard_directions = [&add](unsigned port)
   {
      add(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Joystick Left");
      add(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Joystick Up");
      add(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Joystick Right");
      add(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Joystick Down");
      add(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
          RETRO_DEVICE_ID_ANALOG_X, "Joystick X");
      add(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
          RETRO_DEVICE_ID_ANALOG_Y, "Joystick Y");
   };

   const auto add_generic = [&add, &add_common, &add_standard_directions](unsigned port)
   {
      add_standard_directions(port);
      add(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Button 1");
      add(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Button 2");
      add(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Button 3");
      add(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "Button 4");
      add_common(port);
   };

   const LibretroInputProfiles::Profile *profile =
      game ? LibretroInputProfiles::Find(game->inputs) : nullptr;

   if (!profile)
   {
      add_generic(0);
      add_generic(1);
      if (game && log_cb)
      {
         log_cb(RETRO_LOG_WARN,
                "[Supermodel] Unknown control signature 0x%08X; using generic RetroPad descriptors.\n",
                LibretroInputProfiles::NormalizeInputs(game->inputs));
      }
   }
   else
   {
      // The Model 3 common input bank always contains A/B Start, Coin,
      // Test and Service lines, independently of the game's player controls.
      for (unsigned port = 0; port < 2; ++port)
      {
         const bool ski_player_one =
            profile->family == LibretroInputProfiles::Family::Ski && port == 0;
         const bool fishing_player_one =
            profile->family == LibretroInputProfiles::Family::Fishing && port == 0;
         add_common(port,
                    ski_player_one ? "Select 2 / Center (Red)" :
                    fishing_player_one ? "Cast (Red)" : "Start");
      }

      switch (profile->family)
      {
      case LibretroInputProfiles::Family::Gun:
         for (unsigned port = 0; port < 2; ++port)
         {
            switch (g_active_gun_input)
            {
            case GunInput::Hybrid:
               add(port, RETRO_DEVICE_LIGHTGUN, 0,
                   RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X, "Gun Yaw (Lightgun)");
               add(port, RETRO_DEVICE_LIGHTGUN, 0,
                   RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y, "Gun Pitch (Lightgun)");
               add(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                   RETRO_DEVICE_ID_ANALOG_X, "Gun Yaw (Analog Cursor)");
               add(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                   RETRO_DEVICE_ID_ANALOG_Y, "Gun Pitch (Analog Cursor)");
               add(port, RETRO_DEVICE_JOYPAD, 0,
                   RETRO_DEVICE_ID_JOYPAD_B, "Left Shot (Analog)");
               add(port, RETRO_DEVICE_JOYPAD, 0,
                   RETRO_DEVICE_ID_JOYPAD_A, "Right Shot (Analog)");
               add(port, RETRO_DEVICE_LIGHTGUN, 0,
                   RETRO_DEVICE_ID_LIGHTGUN_TRIGGER, "Left Shot (Lightgun)");
               add(port, RETRO_DEVICE_LIGHTGUN, 0,
                   RETRO_DEVICE_ID_LIGHTGUN_RELOAD, "Right Shot (Lightgun)");
               break;

            case GunInput::Lightgun:
               add(port, RETRO_DEVICE_LIGHTGUN, 0,
                   RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X, "Gun Yaw");
               add(port, RETRO_DEVICE_LIGHTGUN, 0,
                   RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y, "Gun Pitch");
               add(port, RETRO_DEVICE_LIGHTGUN, 0,
                   RETRO_DEVICE_ID_LIGHTGUN_TRIGGER, "Left Shot");
               add(port, RETRO_DEVICE_LIGHTGUN, 0,
                   RETRO_DEVICE_ID_LIGHTGUN_RELOAD, "Right Shot");
               break;

            case GunInput::Mouse:
               add(port, RETRO_DEVICE_MOUSE, 0,
                   RETRO_DEVICE_ID_MOUSE_X, "Gun Yaw");
               add(port, RETRO_DEVICE_MOUSE, 0,
                   RETRO_DEVICE_ID_MOUSE_Y, "Gun Pitch");
               add(port, RETRO_DEVICE_MOUSE, 0,
                   RETRO_DEVICE_ID_MOUSE_LEFT, "Left Shot");
               add(port, RETRO_DEVICE_MOUSE, 0,
                   RETRO_DEVICE_ID_MOUSE_RIGHT, "Right Shot");
               break;

            case GunInput::MouseAnalog:
               add(port, RETRO_DEVICE_MOUSE, 0,
                   RETRO_DEVICE_ID_MOUSE_X, "Gun Yaw (Mouse)");
               add(port, RETRO_DEVICE_MOUSE, 0,
                   RETRO_DEVICE_ID_MOUSE_Y, "Gun Pitch (Mouse)");
               add(port, RETRO_DEVICE_MOUSE, 0,
                   RETRO_DEVICE_ID_MOUSE_LEFT, "Left Shot (Mouse)");
               add(port, RETRO_DEVICE_MOUSE, 0,
                   RETRO_DEVICE_ID_MOUSE_RIGHT, "Right Shot (Mouse)");
               add(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                   RETRO_DEVICE_ID_ANALOG_X, "Gun Yaw (Analog Cursor)");
               add(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                   RETRO_DEVICE_ID_ANALOG_Y, "Gun Pitch (Analog Cursor)");
               add(port, RETRO_DEVICE_JOYPAD, 0,
                   RETRO_DEVICE_ID_JOYPAD_B, "Left Shot (Analog)");
               add(port, RETRO_DEVICE_JOYPAD, 0,
                   RETRO_DEVICE_ID_JOYPAD_A, "Right Shot (Analog)");
               break;

            case GunInput::AnalogSticks:
               add(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                   RETRO_DEVICE_ID_ANALOG_X, "Gun Yaw");
               add(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                   RETRO_DEVICE_ID_ANALOG_Y, "Gun Pitch");
               add(port, RETRO_DEVICE_JOYPAD, 0,
                   RETRO_DEVICE_ID_JOYPAD_B, "Left Shot");
               add(port, RETRO_DEVICE_JOYPAD, 0,
                   RETRO_DEVICE_ID_JOYPAD_A, "Right Shot");
               break;
            }
         }
         break;

      case LibretroInputProfiles::Family::AnalogJoystick:
         if (g_active_star_wars_input != StarWarsInput::Mouse)
         {
            add(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                RETRO_DEVICE_ID_ANALOG_X, "Analog Joystick X");
            add(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                RETRO_DEVICE_ID_ANALOG_Y, "Analog Joystick Y");
            add(0, RETRO_DEVICE_JOYPAD, 0,
                RETRO_DEVICE_ID_JOYPAD_B, "Trigger 1");
            add(0, RETRO_DEVICE_JOYPAD, 0,
                RETRO_DEVICE_ID_JOYPAD_A, "Event 1");
            add(0, RETRO_DEVICE_JOYPAD, 0,
                RETRO_DEVICE_ID_JOYPAD_Y, "Trigger 2");
            add(0, RETRO_DEVICE_JOYPAD, 0,
                RETRO_DEVICE_ID_JOYPAD_X, "Event 2");
         }
         if (g_active_star_wars_input != StarWarsInput::AnalogSticks)
         {
            add(0, RETRO_DEVICE_MOUSE, 0,
                RETRO_DEVICE_ID_MOUSE_X, "Analog Joystick X");
            add(0, RETRO_DEVICE_MOUSE, 0,
                RETRO_DEVICE_ID_MOUSE_Y, "Analog Joystick Y");
            add(0, RETRO_DEVICE_MOUSE, 0,
                RETRO_DEVICE_ID_MOUSE_LEFT, "Trigger 1");
            add(0, RETRO_DEVICE_MOUSE, 0,
                RETRO_DEVICE_ID_MOUSE_RIGHT, "Event 1");
            add(0, RETRO_DEVICE_MOUSE, 0,
                RETRO_DEVICE_ID_MOUSE_BUTTON_4, "Trigger 2");
            add(0, RETRO_DEVICE_MOUSE, 0,
                RETRO_DEVICE_ID_MOUSE_BUTTON_5, "Event 2");
         }
         break;

      case LibretroInputProfiles::Family::Fishing:
         add(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
             RETRO_DEVICE_ID_ANALOG_X, "Rod X");
         add(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
             RETRO_DEVICE_ID_ANALOG_Y, "Rod Y");
         add(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
             RETRO_DEVICE_ID_ANALOG_X, "Stick X");
         add(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
             RETRO_DEVICE_ID_ANALOG_Y, "Stick Y");
         add(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "Reel");
         add(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "Tension");
         add(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Cast (Red)");
         add(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Select (Yellow)");
         break;

      case LibretroInputProfiles::Family::JoystickStandard:
      {
         const char *actions[4] = { nullptr, nullptr, nullptr, nullptr };
         unsigned action_count = 0;
         if (profile->inputs & Game::INPUT_FIGHTING)
         {
            actions[0] = "Punch";
            actions[1] = "Kick";
            actions[2] = "Guard";
            actions[3] = "Escape";
            action_count = 4;
         }
         else if (profile->inputs & Game::INPUT_SOCCER)
         {
            actions[0] = "Short Pass";
            actions[1] = "Long Pass";
            actions[2] = "Shoot";
            action_count = 3;
         }
         else
         {
            actions[0] = "Shift";
            actions[1] = "Beat";
            actions[2] = "Charge";
            actions[3] = "Jump";
            action_count = 4;
         }

         static constexpr unsigned action_ids[4] = {
            RETRO_DEVICE_ID_JOYPAD_B,
            RETRO_DEVICE_ID_JOYPAD_A,
            RETRO_DEVICE_ID_JOYPAD_Y,
            RETRO_DEVICE_ID_JOYPAD_X,
         };
         for (unsigned port = 0; port < profile->players; ++port)
         {
            add_standard_directions(port);
            for (unsigned i = 0; i < action_count; ++i)
               add(port, RETRO_DEVICE_JOYPAD, 0, action_ids[i], actions[i]);
         }
         break;
      }

      case LibretroInputProfiles::Family::MagicalTruck:
         add(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
             RETRO_DEVICE_ID_ANALOG_Y, "Player 1 Lever");
         add(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,
             "Player 1 Foot Pedal");
         add(1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
             RETRO_DEVICE_ID_ANALOG_Y, "Player 2 Lever");
         add(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,
             "Player 2 Foot Pedal");
         break;

      case LibretroInputProfiles::Family::Ski:
         add(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
             RETRO_DEVICE_ID_ANALOG_X, "Ski X");
         add(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
             RETRO_DEVICE_ID_ANALOG_Y, "Ski Y");
         add(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "Pole Left");
         add(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "Pole Right");
         add(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,
             "Select 1 / Left (Blue)");
         add(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,
             "Select 2 / Center (Red)");
         add(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,
             "Select 3 / Right (Green)");
         break;

      case LibretroInputProfiles::Family::JoystickTwin:
         add(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
             RETRO_DEVICE_ID_ANALOG_X, "Left Joystick X");
         add(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
             RETRO_DEVICE_ID_ANALOG_Y, "Left Joystick Y");
         add(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
             RETRO_DEVICE_ID_ANALOG_X, "Right Joystick X");
         add(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
             RETRO_DEVICE_ID_ANALOG_Y, "Right Joystick Y");
         add(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,
             "Left Shot Trigger");
         add(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,
             "Right Shot Trigger");
         add(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "Left Turbo");
         add(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "Right Turbo");
         break;

      case LibretroInputProfiles::Family::Driving:
         add(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
             RETRO_DEVICE_ID_ANALOG_X, "Steering");
         add(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "Brake");
         add(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "Accelerator");
         add(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "Shift Down");
         add(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "Shift Up");

         if (profile->inputs & Game::INPUT_SHIFT4)
         {
            if (g_options.four_speed_shifter == FourSpeedShifter::HGate)
            {
               add(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
                   RETRO_DEVICE_ID_ANALOG_X, "H-Gate: Left / Right");
               add(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
                   RETRO_DEVICE_ID_ANALOG_Y, "H-Gate: Up / Down");
            }
            else
            {
               add(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
                   RETRO_DEVICE_ID_ANALOG_X,
                   "4-Speed: Gear 3 (Left) / Gear 4 (Right)");
               add(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
                   RETRO_DEVICE_ID_ANALOG_Y,
                   "4-Speed: Gear 1 (Up) / Gear 2 (Down)");
            }
            add(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,
                "4-Speed: Neutral");
         }
         if (profile->inputs & Game::INPUT_VR4)
         {
            add(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,
                "VR1 (Red)");
            add(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,
                "VR2 (Blue)");
            add(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,
                "VR3 (Yellow)");
            add(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,
                "VR4 (Green)");
         }
         else if (profile->inputs & Game::INPUT_VIEWCHANGE)
         {
            add(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "VR1");
         }
         if (profile->inputs & Game::INPUT_HANDBRAKE)
         {
            add(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Handbrake");
         }
         if (profile->inputs & Game::INPUT_HARLEY)
         {
            add(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Rear Brake");
            add(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Music Select");
         }
         break;
      }

      if (log_cb)
         log_cb(RETRO_LOG_INFO, "[Supermodel] Control profile: %s\n", profile->name);
   }

   desc.push_back({ 0, 0, 0, 0, nullptr });
   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc.data());
}

void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;

   // 1. VFS Interface
   struct retro_vfs_interface_info vfs_iface_info;
   vfs_iface_info.required_interface_version = 2;
   vfs_iface_info.iface                      = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfs_iface_info))
      filestream_vfs_init(&vfs_iface_info);

   register_core_options();

   struct retro_core_options_update_display_callback update_display_cb;
   update_display_cb.callback = update_core_option_visibility;
   environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK,
              &update_display_cb);
   g_visible_nvram_enabled = -1;
   g_visible_nvram_game.clear();
   g_network_board_option_visible = -1;
   g_network_cabinets_option_visible = -1;
   update_core_option_visibility();

   // 3. Variable Update Check
   bool dummy = false;
   environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &dummy);
   
   // 4. Input Descriptors
   set_input_descriptors(nullptr);
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
   audio_cb = cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
   audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
   input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
   input_state_cb = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
   video_cb = cb;
}

// --- Stubs (Unused) ---

void retro_reset(void) { wrapper.Reset(); }
bool retro_load_game_special(unsigned, const struct retro_game_info *, size_t) { return false; }
void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned, bool, const char *) {}
void* retro_get_memory_data(unsigned id)
{
    if (id == RETRO_MEMORY_SAVE_RAM)
    {
        // Before the first frame the frontend may be about to populate this
        // buffer from .srm. Only export emulator state after initial import.
        if (g_nvram_initialized && wrapper.getEmulator() != nullptr)
        {
            serialize_nvram();
        }
        return g_nvram_buffer;
    }
    return nullptr;
}

size_t retro_get_memory_size(unsigned id)
{
    return (id == RETRO_MEMORY_SAVE_RAM) ? NVRAM_BUFFER_SIZE : 0;
}
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
