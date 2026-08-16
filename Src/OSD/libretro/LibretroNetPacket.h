#ifndef INCLUDED_LIBRETRO_NETPACKET_H
#define INCLUDED_LIBRETRO_NETPACKET_H

#include "libretro.h"

// The libretro-common snapshot bundled by the original core predates the
// netpacket API. Keep the compatibility definitions local so the whole header
// does not have to be replaced merely to use one backwards-compatible
// environment callback.
#ifndef RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE
#define RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE 78

#define RETRO_NETPACKET_UNRELIABLE  0
#define RETRO_NETPACKET_RELIABLE    (1 << 0)
#define RETRO_NETPACKET_UNSEQUENCED (1 << 1)
#define RETRO_NETPACKET_FLUSH_HINT  (1 << 2)
#define RETRO_NETPACKET_BROADCAST   0xFFFF

typedef void (RETRO_CALLCONV *retro_netpacket_send_t)(
   int flags, const void *buf, size_t len, uint16_t client_id);
typedef void (RETRO_CALLCONV *retro_netpacket_poll_receive_t)(void);
typedef void (RETRO_CALLCONV *retro_netpacket_start_t)(
   uint16_t client_id, retro_netpacket_send_t send_fn,
   retro_netpacket_poll_receive_t poll_receive_fn);
typedef void (RETRO_CALLCONV *retro_netpacket_receive_t)(
   const void *buf, size_t len, uint16_t client_id);
typedef void (RETRO_CALLCONV *retro_netpacket_stop_t)(void);
typedef void (RETRO_CALLCONV *retro_netpacket_poll_t)(void);
typedef bool (RETRO_CALLCONV *retro_netpacket_connected_t)(uint16_t client_id);
typedef void (RETRO_CALLCONV *retro_netpacket_disconnected_t)(uint16_t client_id);

struct retro_netpacket_callback
{
   retro_netpacket_start_t start;
   retro_netpacket_receive_t receive;
   retro_netpacket_stop_t stop;
   retro_netpacket_poll_t poll;
   retro_netpacket_connected_t connected;
   retro_netpacket_disconnected_t disconnected;
   const char *protocol_version;
};
#endif

#endif
