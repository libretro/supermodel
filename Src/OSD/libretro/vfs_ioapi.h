#ifndef INCLUDED_VFS_IOAPI_H
#define INCLUDED_VFS_IOAPI_H

#include "libretro.h"
#include "Pkgs/ioapi.h"

// Populates a zlib_filefunc64_def backed by the libretro VFS interface.
// Call before opening any ZIP archive via minizip.
void fill_retro_vfs_filefunc64(zlib_filefunc64_def *pDef, struct retro_vfs_interface *vfs);

#endif  // INCLUDED_VFS_IOAPI_H
