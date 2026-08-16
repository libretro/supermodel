#ifndef __LIBRETRO_CBS_H
#define __LIBRETRO_CBS_H

#include "libretro.h"

#ifdef __cplusplus
extern "C" {
#endif

extern retro_video_refresh_t video_cb;
extern retro_environment_t environ_cb;
extern retro_audio_sample_t audio_cb;
extern retro_audio_sample_batch_t audio_batch_cb;
extern retro_input_poll_t input_poll_cb;
extern retro_input_state_t input_state_cb;
extern retro_input_state_t dbg_input_state_cb;
extern struct retro_hw_render_callback hw_render;
extern struct retro_vfs_interface *g_vfs_interface;

#ifdef __cplusplus
}
#endif

#endif