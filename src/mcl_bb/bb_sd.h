/* MCL on the Blackbox, milestone M7: the SD card through the STOCK FatFs, from the task the stock firmware uses for it.
 *
 * Findings (docs/mcl-m0-m1-results.md, M7): every FatFs call and every card transfer of the stock firmware is made by one task, pcmStreamer (nothing locks FatFs;
 * the card driver has ONE shared completion queue), so MCL's file work must run there too. The GUI side (MCL, UI task) posts a request into a one-slot mailbox and waits
 * on a semaphore; a hook in the streamer's loop (patches/mclsd.py, the call of its queue receive at 0x080743e8) runs it with the stock functions, one FatFs call per pass.
 *
 * DMA safety without touching the MPU: the stock FatFs build uses its volume window (inside the FATFS object, in non-cacheable memory) for every transfer of at most one
 * sector. Only a read or write of 512 bytes or more can make FatFs transfer whole sectors straight into the caller's buffer. So MCL never asks for more than 511 bytes in
 * one FatFs call (larger requests are split here), and its buffers and FIL / DIR objects can live in ordinary (cached) SDRAM.
 *
 * Return codes: 0 = FR_OK and the other FatFs FRESULTs (1..19), plus SD_BUSY / SD_TIMEOUT / SD_NOT_READY of ours. */
#pragma once
#include <stdint.h>

enum { SD_OK = 0, SD_NO_FILE = 4, SD_NO_PATH = 5, SD_DENIED = 7, SD_EXIST = 8, SD_BUSY = 100, SD_TIMEOUT = 101, SD_NOT_READY = 102, SD_BAD_HANDLE = 103 };
enum { SD_READ = 1, SD_WRITE = 2, SD_CREATE_NEW = 4, SD_CREATE_ALWAYS = 8, SD_OPEN_ALWAYS = 0x10, SD_APPEND = 0x30 };      /* the FatFs FA_ flags */
enum { SD_ATTR_DIR = 0x10 };
#define SD_FILES 4
#define SD_DIRS  2
#define SD_NAME_MAX 256
#define SD_TIMEOUT_MS 10000u

struct BbDirEnt {
    uint64_t size;
    uint8_t attr;
    char name[SD_NAME_MAX];
};

extern "C" {
void bb_sd_down(void);                                  /* first thing in mcl_boot: the streamer's hook off */
void bb_sd_init(void);                                  /* after .bss / heap: mailbox empty, hook live */
int bb_sd_ready(void);

/* the streamer task's side (patches/mclsd.py points the stock loop here) */
uint32_t mcl_sd_pump(uint32_t queue, uint32_t item, uint32_t ticks, uint32_t peek);     /* runs a posted request, then the stock queue receive it replaced */
void bb_sd_service(void);                               /* the request, if one is posted (exposed for the tests) */

/* the GUI side: everything blocks until the streamer has done it (at most SD_TIMEOUT_MS) */
int bb_sd_open(const char *path, uint32_t mode);        /* handle >= 0, or -(FRESULT) */
uint32_t bb_sd_close(int h);
int32_t bb_sd_read(int h, void *buf, uint32_t n);       /* bytes read (less than n at the end of the file), or -(FRESULT) */
int32_t bb_sd_write(int h, const void *buf, uint32_t n);
uint32_t bb_sd_seek(int h, uint64_t pos);
uint32_t bb_sd_sync(int h);
uint32_t bb_sd_truncate(int h);                         /* at the current position */
uint64_t bb_sd_size(int h);
uint64_t bb_sd_tell(int h);
uint32_t bb_sd_stat(const char *path, BbDirEnt *out);
uint32_t bb_sd_mkdir(const char *path);
uint32_t bb_sd_unlink(const char *path);                /* a file or an empty directory */
uint32_t bb_sd_rename(const char *from, const char *to);
int bb_sd_opendir(const char *path);                    /* handle >= 0, or -(FRESULT) */
uint32_t bb_sd_readdir(int d, BbDirEnt *out);           /* SD_OK; out->name[0] == 0 at the end */
uint32_t bb_sd_closedir(int d);

/* statistics: 0 requests, 1 timeouts, 2 largest wait (ms), 3 largest streamer-side time of one call (us), 4 longest gap between two passes of the streamer loop through the hook (ms), 5 how many gaps were over 100 ms */
uint32_t bb_sd_stat_get(int which);
}
