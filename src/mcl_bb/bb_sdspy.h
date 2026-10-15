/* MCL on the Blackbox, milestone M7 (recon): who uses the SD card, from which task, and do two tasks ever use it at once?
 * Wrappers around the stock FatFs entry points and the card driver's read / write / ioctl (patches/mclspy.py points the stock call sites and the driver table at them).
 * Pure counting and timing; every call is passed on unchanged. Not in normal images. */
#pragma once
#include <stdint.h>

enum { SPY_OPEN, SPY_READ, SPY_WRITE, SPY_CLOSE, SPY_SEEK, SPY_SYNC, SPY_MOUNT, SPY_FUNCS };
enum { SPY_DEFAULT, SPY_AUDIO, SPY_PCM, SPY_OTHER, SPY_TASKS };
enum { SPY_FAT, SPY_DISK, SPY_LAYERS };
enum { SPY_DISK_RD, SPY_DISK_WR, SPY_DISK_IO, SPY_DISK_FUNCS };

extern "C" {
uint32_t bb_sdspy_count(int task, int fn);              /* FatFs calls of `fn` from `task` */
uint32_t bb_sdspy_disk(int task, int fn);               /* card driver calls (read / write / ioctl) from `task` */
uint32_t bb_sdspy_overlaps(int layer);                  /* entries while ANOTHER task was inside the same layer */
uint32_t bb_sdspy_pair(int layer);                      /* the first overlap: (entering task << 4) | task already inside, 0xff = none */
uint32_t bb_sdspy_worst_us(int layer);                  /* longest single call, microseconds */
void bb_sdspy_enter(int layer, int task);               /* exposed for the tests: what the wrappers do around the stock call */
void bb_sdspy_leave(int layer, int task);
int bb_sdspy_task(void);                                /* the calling task: 0 defaultTask, 1 audioTask, 2 pcmStreamer, 3 other */
}
