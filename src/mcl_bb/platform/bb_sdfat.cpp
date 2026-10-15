/* MCL's SdFat API (FsFile, SdFat_ / SD) over the Blackbox SD layer (src/mcl_bb/bb_sd.cpp, milestone M7): every call is made by the streamer task through the mailbox, one FatFs call
 * at a time, so MCL's file work never runs concurrently with the stock firmware's own. Paths are relative to the card root; the drive prefix is added by the SD layer.
 * The class declarations are src/platform/desktop_common/SdFat.h (an int32 handle, a name, a path, two flags). */
#include "SdFat.h"
#include "bb_sd.h"

#include <string.h>

namespace {
char cwd[128] = "/";

void full_path(const char *p, char *out, size_t cap)
{
    size_t n = 0;
    if (!p || !*p)
        p = ".";
    if (p[0] != '/') {                                           /* relative: against the current directory */
        for (size_t i = 0; cwd[i] && n + 1 < cap; i++)
            out[n++] = cwd[i];
        if (n && out[n - 1] != '/' && n + 1 < cap)
            out[n++] = '/';
        if (p[0] == '.' && !p[1])
            p = "";
    }
    for (size_t i = 0; p[i] && n + 1 < cap; i++)
        out[n++] = p[i];
    out[n] = 0;
}

/* stat that also knows the root directory (FatFs has no entry for it) */
bool stat_ent(const char *full, BbDirEnt *ent)
{
    if (full[0] == '/' && !full[1]) {
        ent->size = 0;
        ent->attr = SD_ATTR_DIR;
        ent->name[0] = 0;
        return true;
    }
    return bb_sd_stat(full, ent) == SD_OK;
}

const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

void set_name_path(FsFile *f, const char *path)
{
    strncpy(f->path_, path, sizeof f->path_ - 1);
    f->path_[sizeof f->path_ - 1] = 0;
    strncpy(f->name_, base_name(path), sizeof f->name_ - 1);
    f->name_[sizeof f->name_ - 1] = 0;
}
}

FsFile &FsFile::operator=(FsFile &&other) noexcept
{
    if (this != &other) {
        close();
        handle_ = other.handle_;
        memcpy(name_, other.name_, sizeof name_);
        memcpy(path_, other.path_, sizeof path_);
        open_ = other.open_;
        is_dir_ = other.is_dir_;
        other.handle_ = -1;
        other.open_ = false;
        other.is_dir_ = false;
        other.path_[0] = 0;
    }
    return *this;
}

bool FsFile::open(const char *path, uint8_t mode)
{
    close();
    char full[128];
    full_path(path, full, sizeof full);
    BbDirEnt ent;
    bool exists = stat_ent(full, &ent);
    if (exists && (ent.attr & SD_ATTR_DIR)) {
        int d = bb_sd_opendir(full);
        if (d < 0)
            return false;
        handle_ = d;
        is_dir_ = true;
        open_ = true;
        set_name_path(this, full);
        return true;
    }
    const bool want_create = (mode & O_CREAT) != 0, want_excl = (mode & O_EXCL) != 0, want_trunc = (mode & O_TRUNC) != 0, want_append = (mode & O_APPEND) != 0;
    if (want_excl && exists)
        return false;
    if (!exists && !want_create)
        return false;
    uint32_t fa = 0;
    if (mode & O_READ)
        fa |= SD_READ;
    if (mode & O_WRITE)
        fa |= SD_WRITE;
    if (!exists)
        fa |= SD_CREATE_NEW;
    else if (want_trunc)
        fa |= SD_CREATE_ALWAYS;
    int h = bb_sd_open(full, fa);
    if (h < 0)
        return false;
    handle_ = h;
    is_dir_ = false;
    open_ = true;
    set_name_path(this, full);
    if (want_append)
        bb_sd_seek(h, bb_sd_size(h));
    return true;
}

bool FsFile::openNext(FsFile *dir, uint8_t mode)
{
    if (!dir || !dir->open_ || !dir->is_dir_)
        return false;
    BbDirEnt ent;
    if (bb_sd_readdir(dir->handle_, &ent) != SD_OK || !ent.name[0])
        return false;
    char child[128];
    size_t n = 0;
    for (size_t i = 0; dir->path_[i] && n + 1 < sizeof child; i++)
        child[n++] = dir->path_[i];
    if (n && child[n - 1] != '/' && n + 1 < sizeof child)
        child[n++] = '/';
    for (size_t i = 0; ent.name[i] && n + 1 < sizeof child; i++)
        child[n++] = ent.name[i];
    child[n] = 0;
    close();
    if (ent.attr & SD_ATTR_DIR) {
        int d = bb_sd_opendir(child);
        if (d < 0)
            return false;
        handle_ = d;
        is_dir_ = true;
    } else {
        uint32_t fa = 0;
        if (mode & O_READ)
            fa |= SD_READ;
        if (mode & O_WRITE)
            fa |= SD_WRITE;
        int h = bb_sd_open(child, fa ? fa : SD_READ);
        if (h < 0)
            return false;
        handle_ = h;
        is_dir_ = false;
    }
    open_ = true;
    set_name_path(this, child);
    return true;
}

bool FsFile::close()
{
    if (open_) {
        if (is_dir_)
            bb_sd_closedir(handle_);
        else
            bb_sd_close(handle_);
    }
    handle_ = -1;
    open_ = false;
    is_dir_ = false;
    name_[0] = 0;
    path_[0] = 0;
    return true;
}

int FsFile::read(void *buf, size_t n)
{
    if (!open_ || is_dir_)
        return -1;
    int32_t r = bb_sd_read(handle_, buf, (uint32_t)n);
    return r < 0 ? -1 : (int)r;
}

int FsFile::write(const void *buf, size_t n)
{
    if (!open_ || is_dir_)
        return -1;
    int32_t r = bb_sd_write(handle_, buf, (uint32_t)n);
    return r < 0 ? -1 : (int)r;
}

bool FsFile::seekSet(uint32_t pos) { return open_ && !is_dir_ && bb_sd_seek(handle_, pos) == SD_OK; }
uint32_t FsFile::curPosition() const { return open_ && !is_dir_ ? (uint32_t)bb_sd_tell(handle_) : 0; }
uint32_t FsFile::fileSize() const { return open_ && !is_dir_ ? (uint32_t)bb_sd_size(handle_) : 0; }

bool FsFile::rewind()
{
    if (!open_)
        return false;
    if (is_dir_) {                                               /* a directory starts again by being opened again */
        char p[sizeof path_];
        memcpy(p, path_, sizeof p);
        bb_sd_closedir(handle_);
        int d = bb_sd_opendir(p);
        if (d < 0) {
            open_ = false;
            handle_ = -1;
            return false;
        }
        handle_ = d;
        return true;
    }
    return seekSet(0);
}

size_t FsFile::getName(char *dst, size_t cap) const
{
    if (!dst || !cap)
        return 0;
    size_t n = strlen(name_);
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, name_, n);
    dst[n] = 0;
    return n;
}

bool FsFile::sync() { return open_ && !is_dir_ && bb_sd_sync(handle_) == SD_OK; }

bool FsFile::truncate(uint32_t length)
{
    if (!open_ || is_dir_ || length > bb_sd_size(handle_))
        return false;
    return bb_sd_seek(handle_, length) == SD_OK && bb_sd_truncate(handle_) == SD_OK;
}

bool FsFile::remove()
{
    if (!open_ || is_dir_)
        return false;
    char p[sizeof path_];
    memcpy(p, path_, sizeof p);
    close();
    return bb_sd_unlink(p) == SD_OK;
}

bool FsFile::preAllocate(uint32_t) { return open_ && !is_dir_; }         /* contiguous pre-allocation is an optimisation: writes extend the file as they come */

bool SdFat::exists(const char *path)
{
    char full[128];
    full_path(path, full, sizeof full);
    BbDirEnt ent;
    return stat_ent(full, &ent);
}

bool SdFat::mkdir(const char *path, bool recursive)
{
    char full[128];
    full_path(path, full, sizeof full);
    if (recursive) {
        for (char *p = full + 1; *p; p++)
            if (*p == '/') {
                *p = 0;
                uint32_t r = bb_sd_mkdir(full);
                *p = '/';
                if (r != SD_OK && r != SD_EXIST)
                    return false;
            }
    }
    uint32_t r = bb_sd_mkdir(full);
    return r == SD_OK || r == SD_EXIST;
}

bool SdFat::rmdir(const char *path)
{
    char full[128];
    full_path(path, full, sizeof full);
    return bb_sd_unlink(full) == SD_OK;
}

bool SdFat::remove(const char *path) { return rmdir(path); }

bool SdFat::rename(const char *a, const char *b)
{
    if (!a || !b)
        return false;
    char fa[128], fb[128];
    full_path(a, fa, sizeof fa);
    full_path(b, fb, sizeof fb);
    return bb_sd_rename(fa, fb) == SD_OK;
}

bool SdFat::chdir(const char *path)
{
    if (!path)
        return false;
    char full[128];
    full_path(path, full, sizeof full);
    BbDirEnt ent;
    if (!stat_ent(full, &ent) || !(ent.attr & SD_ATTR_DIR))
        return false;
    strncpy(cwd, full, sizeof cwd - 1);
    cwd[sizeof cwd - 1] = 0;
    return true;
}

void mcl_desktop_set_sd_root(const char *) {}
