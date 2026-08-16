#include "vfs_ioapi.h"

static struct retro_vfs_interface *s_vfs = nullptr;

static voidpf ZCALLBACK vfs_open64(voidpf /*opaque*/, const void *filename, int mode)
{
    unsigned access = (mode & ZLIB_FILEFUNC_MODE_WRITE)
        ? RETRO_VFS_FILE_ACCESS_READ_WRITE
        : RETRO_VFS_FILE_ACCESS_READ;
    return (voidpf)s_vfs->open((const char *)filename, access, RETRO_VFS_FILE_ACCESS_HINT_NONE);
}

static uLong ZCALLBACK vfs_read(voidpf /*opaque*/, voidpf stream, void *buf, uLong size)
{
    int64_t n = s_vfs->read((struct retro_vfs_file_handle *)stream, buf, size);
    return (uLong)(n < 0 ? 0 : n);
}

static uLong ZCALLBACK vfs_write(voidpf /*opaque*/, voidpf stream, const void *buf, uLong size)
{
    int64_t n = s_vfs->write((struct retro_vfs_file_handle *)stream, buf, size);
    return (uLong)(n < 0 ? 0 : n);
}

static ZPOS64_T ZCALLBACK vfs_tell64(voidpf /*opaque*/, voidpf stream)
{
    int64_t pos = s_vfs->tell((struct retro_vfs_file_handle *)stream);
    return (ZPOS64_T)(pos < 0 ? 0 : pos);
}

static long ZCALLBACK vfs_seek64(voidpf /*opaque*/, voidpf stream, ZPOS64_T offset, int origin)
{
    // ZLIB_FILEFUNC_SEEK_{SET,CUR,END} == RETRO_VFS_SEEK_POSITION_{START,CURRENT,END} (0,1,2)
    return s_vfs->seek((struct retro_vfs_file_handle *)stream, (int64_t)offset, origin);
}

static int ZCALLBACK vfs_close(voidpf /*opaque*/, voidpf stream)
{
    return s_vfs->close((struct retro_vfs_file_handle *)stream);
}

static int ZCALLBACK vfs_error(voidpf /*opaque*/, voidpf /*stream*/)
{
    return 0;
}

void fill_retro_vfs_filefunc64(zlib_filefunc64_def *pDef, struct retro_vfs_interface *vfs)
{
    s_vfs             = vfs;
    pDef->zopen64_file = vfs_open64;
    pDef->zread_file   = vfs_read;
    pDef->zwrite_file  = vfs_write;
    pDef->ztell64_file = vfs_tell64;
    pDef->zseek64_file = vfs_seek64;
    pDef->zclose_file  = vfs_close;
    pDef->zerror_file  = vfs_error;
    pDef->opaque       = nullptr;
}
