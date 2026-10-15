#include "bb_sd.h"

#define HOOK extern "C" __attribute__((section(".hook"), used))
#define DWT_CYCCNT (*(volatile uint32_t *)0xe0001004u)
#define SYST_RVR   (*(volatile uint32_t *)0xe000e014u)

/* the stock FatFs (docs/mcl-m0-m1-results.md, M0 and M7) and FreeRTOS kernel entry points; all Thumb */
#define F_MOUNT     0x080864E8u
#define F_OPEN      0x08086534u
#define F_READ      0x08086866u
#define F_WRITE     0x08086A06u
#define F_SYNC      0x08086C28u
#define F_CLOSE     0x08086D76u
#define F_LSEEK     0x08086D96u
#define F_OPENDIR   0x080870F6u
#define F_CLOSEDIR  0x0808718Cu
#define F_READDIR   0x080871A0u
#define F_STAT      0x0808724Au
#define F_TRUNCATE  0x080873C8u
#define F_UNLINK    0x0808748Eu
#define F_MKDIR     0x08087558u
#define F_RENAME    0x080876C8u
#define Q_CREATE    0x080883BEu          /* xQueueGenericCreate(length, item size, type) */
#define Q_SEND      0x08088402u          /* xQueueGenericSend(queue, item, ticks, position) */
#define Q_RECEIVE   0x08088756u          /* xQueueGenericReceive(queue, item, ticks, peek): also what the streamer's loop calls */

typedef uint32_t (*fn4)(uint32_t, uint32_t, uint32_t, uint32_t);
static inline uint32_t stock(uint32_t addr, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0, uint32_t d = 0) { return ((fn4)(addr | 1u))(a, b, c, d); }

#define READY_MAGIC 0x53444d31u          /* "SDM1" */
static volatile uint32_t ready;

/* FatFs objects (sizes measured against the stock build: FIL 0x50, DIR 0x50 touched, FILINFO 8 + 2 + 2 + 1 + 13 + 256) */
struct Fil { uint8_t b[96] __attribute__((aligned(8))); };
struct Dir { uint8_t b[160] __attribute__((aligned(8))); };
static Fil fils[SD_FILES];
static Dir dirs[SD_DIRS];
static uint8_t fil_used[SD_FILES], dir_used[SD_DIRS];
static uint8_t info[320] __attribute__((aligned(8)));          /* FILINFO */
#define FIL_OBJSIZE(f) (*(volatile uint64_t *)((f).b + 0x10))
#define FIL_FPTR(f)    (*(volatile uint64_t *)((f).b + 0x38))
#define INFO_SIZE      (*(uint64_t *)info)
#define INFO_ATTR      (info[12])
#define INFO_NAME      ((const char *)(info + 26))

enum { OP_OPEN = 1, OP_CLOSE, OP_READ, OP_WRITE, OP_SEEK, OP_SYNC, OP_TRUNCATE, OP_OPENDIR, OP_READDIR, OP_CLOSEDIR, OP_STAT, OP_MKDIR, OP_UNLINK, OP_RENAME };
enum { ST_IDLE = 0, ST_POSTED = 1, ST_RUNNING = 3, ST_DONE = 2 };

static struct {
    volatile uint32_t state;
    volatile uint32_t abandoned;                                 /* the requester gave up waiting: nobody collects the result */
    uint32_t op, h, mode, n;
    void *buf;
    uint64_t pos;
    uint32_t ret, count;
    uint64_t size_out;
    uint8_t attr_out;
    char path[320], path2[320];
    char name_out[SD_NAME_MAX];
} req;
static uint32_t sem;                                              /* binary semaphore: the streamer gives it when a request is done */
static uint32_t st_requests, st_timeouts, st_wait_ms, st_call_us;
static uint32_t last_pump, worst_gap_cyc, gaps_100ms;           /* the streamer loop's own rhythm: time between two passes through the hook */

static inline uint32_t irq_off(void)
{
    uint32_t p;
    __asm volatile("mrs %0, primask\n cpsid i" : "=r"(p)::"memory");
    return p;
}
static inline void irq_restore(uint32_t p) { __asm volatile("msr primask, %0" ::"r"(p) : "memory"); }
static inline void barrier(void) { __asm volatile("dmb" ::: "memory"); }

HOOK void bb_sd_down(void) { ready = 0; }
HOOK int bb_sd_ready(void) { return ready == READY_MAGIC; }

HOOK void bb_sd_init(void)
{
    req.state = ST_IDLE;
    req.abandoned = 0;
    for (int i = 0; i < SD_FILES; i++)
        fil_used[i] = 0;
    for (int i = 0; i < SD_DIRS; i++)
        dir_used[i] = 0;
    sem = 0;
    last_pump = 0;
    barrier();
    ready = READY_MAGIC;                                         /* last: the streamer's hook looks at this */
}

/* ---- the streamer task's side ---- */
static void copy_name(char *dst, const char *src)
{
    uint32_t i = 0;
    for (; i < SD_NAME_MAX - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

static uint32_t execute(void)
{
    switch (req.op) {
    case OP_OPEN: {
        int h = (int)req.h;
        uint32_t r = stock(F_OPEN, (uint32_t)&fils[h], (uint32_t)req.path, req.mode);
        return r;
    }
    case OP_CLOSE: return stock(F_CLOSE, (uint32_t)&fils[req.h]);
    case OP_READ: {
        uint32_t got = 0;
        uint32_t r = stock(F_READ, (uint32_t)&fils[req.h], (uint32_t)req.buf, req.n, (uint32_t)&got);
        req.count = got;
        return r;
    }
    case OP_WRITE: {
        uint32_t wr = 0;
        uint32_t r = stock(F_WRITE, (uint32_t)&fils[req.h], (uint32_t)req.buf, req.n, (uint32_t)&wr);
        req.count = wr;
        return r;
    }
    case OP_SEEK: return stock(F_LSEEK, (uint32_t)&fils[req.h], 0, (uint32_t)req.pos, (uint32_t)(req.pos >> 32));
    case OP_SYNC: return stock(F_SYNC, (uint32_t)&fils[req.h]);
    case OP_TRUNCATE: return stock(F_TRUNCATE, (uint32_t)&fils[req.h]);
    case OP_OPENDIR: return stock(F_OPENDIR, (uint32_t)&dirs[req.h], (uint32_t)req.path);
    case OP_READDIR: {
        for (unsigned i = 0; i < sizeof info; i++)
            info[i] = 0;
        uint32_t r = stock(F_READDIR, (uint32_t)&dirs[req.h], (uint32_t)info);
        if (r == 0) {
            req.size_out = INFO_SIZE;
            req.attr_out = INFO_ATTR;
            copy_name(req.name_out, INFO_NAME);
        }
        return r;
    }
    case OP_CLOSEDIR: return stock(F_CLOSEDIR, (uint32_t)&dirs[req.h]);
    case OP_STAT: {
        for (unsigned i = 0; i < sizeof info; i++)
            info[i] = 0;
        uint32_t r = stock(F_STAT, (uint32_t)req.path, (uint32_t)info);
        if (r == 0) {
            req.size_out = INFO_SIZE;
            req.attr_out = INFO_ATTR;
            copy_name(req.name_out, INFO_NAME);
        }
        return r;
    }
    case OP_MKDIR: return stock(F_MKDIR, (uint32_t)req.path);
    case OP_UNLINK: return stock(F_UNLINK, (uint32_t)req.path);
    case OP_RENAME: return stock(F_RENAME, (uint32_t)req.path, (uint32_t)req.path2);
    }
    return SD_BAD_HANDLE;
}

HOOK void bb_sd_service(void)
{
    if (ready != READY_MAGIC || req.state != ST_POSTED)
        return;
    req.state = ST_RUNNING;
    uint32_t t0 = DWT_CYCCNT;
    req.ret = execute();
    uint32_t us = (uint32_t)((uint64_t)(DWT_CYCCNT - t0) * 1000000u / ((SYST_RVR + 1u) * 1000u));
    if (us > st_call_us)
        st_call_us = us;
    barrier();
    if (req.abandoned) {                                         /* nobody waits any more */
        req.abandoned = 0;
        req.state = ST_IDLE;
        return;
    }
    req.state = ST_DONE;
    if (sem)
        stock(Q_SEND, sem, 0, 0, 0);
}

HOOK uint32_t mcl_sd_pump(uint32_t queue, uint32_t item, uint32_t ticks, uint32_t peek)
{
    if (ready == READY_MAGIC) {
        uint32_t now = DWT_CYCCNT, gap = now - last_pump;
        if (last_pump && gap > worst_gap_cyc)
            worst_gap_cyc = gap;
        if (last_pump && gap > (SYST_RVR + 1u) * 100u)           /* 100 ms of ticks */
            gaps_100ms++;
        last_pump = now;
    }
    bb_sd_service();
    return stock(Q_RECEIVE, queue, item, ticks, peek);
}

/* ---- the GUI side ---- */
/* Posts the request filled in `req` and waits for the streamer. */
static uint32_t submit(void)
{
    if (ready != READY_MAGIC)
        return SD_NOT_READY;
    if (req.state == ST_DONE && req.abandoned) {                 /* the late result of a request we stopped waiting for */
        req.abandoned = 0;
        req.state = ST_IDLE;
    }
    if (req.state != ST_IDLE)
        return SD_BUSY;
    if (!sem)
        sem = stock(Q_CREATE, 1, 0, 3);
    if (!sem)
        return SD_NOT_READY;
    stock(Q_RECEIVE, sem, 0, 0, 0);                              /* an unclaimed give from an abandoned request must not wake us early */
    st_requests++;
    uint32_t t0 = DWT_CYCCNT;
    barrier();
    req.state = ST_POSTED;
    uint32_t done = 0;
    while (!done) {
        if (req.state == ST_DONE)
            break;
        if ((uint32_t)((uint64_t)(DWT_CYCCNT - t0) * 1000u / ((SYST_RVR + 1u) * 1000u)) >= SD_TIMEOUT_MS)
            break;
        stock(Q_RECEIVE, sem, 0, 50, 0);                         /* sleeps until the streamer gives the semaphore, at most 50 ms (a lost wake-up costs that) */
    }
    uint32_t ms = (uint32_t)((uint64_t)(DWT_CYCCNT - t0) * 1000u / ((SYST_RVR + 1u) * 1000u));
    if (ms > st_wait_ms)
        st_wait_ms = ms;
    if (req.state != ST_DONE) {
        st_timeouts++;
        req.abandoned = 1;                                       /* if it ever completes, the streamer frees the mailbox */
        return SD_TIMEOUT;
    }
    uint32_t r = req.ret;
    req.state = ST_IDLE;
    return r;
}

static void set_path(char *dst, const char *src)
{
    uint32_t i = 0;
    if (src[0] == '/') {                                         /* the stock volume is logical drive 0 */
        dst[i++] = '0';
        dst[i++] = ':';
    }
    for (uint32_t j = 0; src[j] && i < 319; j++)
        dst[i++] = src[j];
    dst[i] = 0;
}

HOOK int bb_sd_open(const char *path, uint32_t mode)
{
    int h = -1;
    for (int i = 0; i < SD_FILES; i++)
        if (!fil_used[i]) {
            h = i;
            break;
        }
    if (h < 0)
        return -(int)SD_BUSY;
    for (unsigned i = 0; i < sizeof fils[h].b; i++)
        fils[h].b[i] = 0;
    req.op = OP_OPEN;
    req.h = (uint32_t)h;
    req.mode = mode;
    set_path(req.path, path);
    uint32_t r = submit();
    if (r)
        return -(int)r;
    fil_used[h] = 1;
    return h;
}

static int fok(int h) { return h >= 0 && h < SD_FILES && fil_used[h]; }

HOOK uint32_t bb_sd_close(int h)
{
    if (!fok(h))
        return SD_BAD_HANDLE;
    req.op = OP_CLOSE;
    req.h = (uint32_t)h;
    uint32_t r = submit();
    if (r != SD_TIMEOUT && r != SD_BUSY)
        fil_used[h] = 0;                                         /* (even when FatFs reported an error: the object is not usable) */
    return r;
}

HOOK int32_t bb_sd_read(int h, void *buf, uint32_t n)
{
    if (!fok(h))
        return -(int32_t)SD_BAD_HANDLE;
    uint32_t total = 0;
    uint8_t *p = (uint8_t *)buf;
    while (total < n) {
        uint32_t c = n - total > 511u ? 511u : n - total;        /* never 512 or more in one FatFs call: see the header */
        req.op = OP_READ;
        req.h = (uint32_t)h;
        req.buf = p + total;
        req.n = c;
        req.count = 0;
        uint32_t r = submit();
        if (r)
            return -(int32_t)r;
        total += req.count;
        if (req.count < c)
            break;                                               /* end of file */
    }
    return (int32_t)total;
}

HOOK int32_t bb_sd_write(int h, const void *buf, uint32_t n)
{
    if (!fok(h))
        return -(int32_t)SD_BAD_HANDLE;
    uint32_t total = 0;
    const uint8_t *p = (const uint8_t *)buf;
    while (total < n) {
        uint32_t c = n - total > 511u ? 511u : n - total;
        req.op = OP_WRITE;
        req.h = (uint32_t)h;
        req.buf = (void *)(p + total);
        req.n = c;
        req.count = 0;
        uint32_t r = submit();
        if (r)
            return -(int32_t)r;
        total += req.count;
        if (req.count < c)
            return -(int32_t)SD_DENIED;                          /* a short write without an error: the volume is full */
    }
    return (int32_t)total;
}

HOOK uint32_t bb_sd_seek(int h, uint64_t pos)
{
    if (!fok(h))
        return SD_BAD_HANDLE;
    req.op = OP_SEEK;
    req.h = (uint32_t)h;
    req.pos = pos;
    return submit();
}

HOOK uint32_t bb_sd_sync(int h)
{
    if (!fok(h))
        return SD_BAD_HANDLE;
    req.op = OP_SYNC;
    req.h = (uint32_t)h;
    return submit();
}

HOOK uint32_t bb_sd_truncate(int h)
{
    if (!fok(h))
        return SD_BAD_HANDLE;
    req.op = OP_TRUNCATE;
    req.h = (uint32_t)h;
    return submit();
}

/* size and position are plain fields of the FIL object (no FatFs call; nothing else touches it while MCL does not have a request posted) */
HOOK uint64_t bb_sd_size(int h) { return fok(h) ? FIL_OBJSIZE(fils[h]) : 0; }
HOOK uint64_t bb_sd_tell(int h) { return fok(h) ? FIL_FPTR(fils[h]) : 0; }

static uint32_t finish_ent(uint32_t r, BbDirEnt *out)
{
    if (r == 0 && out) {
        out->size = req.size_out;
        out->attr = req.attr_out;
        copy_name(out->name, req.name_out);
    }
    return r;
}

HOOK uint32_t bb_sd_stat(const char *path, BbDirEnt *out)
{
    req.op = OP_STAT;
    set_path(req.path, path);
    return finish_ent(submit(), out);
}

HOOK uint32_t bb_sd_mkdir(const char *path)
{
    req.op = OP_MKDIR;
    set_path(req.path, path);
    return submit();
}

HOOK uint32_t bb_sd_unlink(const char *path)
{
    req.op = OP_UNLINK;
    set_path(req.path, path);
    return submit();
}

HOOK uint32_t bb_sd_rename(const char *from, const char *to)
{
    req.op = OP_RENAME;
    set_path(req.path, from);
    set_path(req.path2, to);
    return submit();
}

HOOK int bb_sd_opendir(const char *path)
{
    int d = -1;
    for (int i = 0; i < SD_DIRS; i++)
        if (!dir_used[i]) {
            d = i;
            break;
        }
    if (d < 0)
        return -(int)SD_BUSY;
    for (unsigned i = 0; i < sizeof dirs[d].b; i++)
        dirs[d].b[i] = 0;
    req.op = OP_OPENDIR;
    req.h = (uint32_t)d;
    set_path(req.path, path);
    uint32_t r = submit();
    if (r)
        return -(int)r;
    dir_used[d] = 1;
    return d;
}

HOOK uint32_t bb_sd_readdir(int d, BbDirEnt *out)
{
    if (d < 0 || d >= SD_DIRS || !dir_used[d])
        return SD_BAD_HANDLE;
    req.op = OP_READDIR;
    req.h = (uint32_t)d;
    return finish_ent(submit(), out);
}

HOOK uint32_t bb_sd_closedir(int d)
{
    if (d < 0 || d >= SD_DIRS || !dir_used[d])
        return SD_BAD_HANDLE;
    req.op = OP_CLOSEDIR;
    req.h = (uint32_t)d;
    uint32_t r = submit();
    if (r != SD_TIMEOUT && r != SD_BUSY)
        dir_used[d] = 0;
    return r;
}

HOOK uint32_t bb_sd_stat_get(int which)
{
    switch (which) {
    case 0: return st_requests;
    case 1: return st_timeouts;
    case 2: return st_wait_ms;
    case 3: return st_call_us;
    case 4: return (uint32_t)((uint64_t)worst_gap_cyc * 1000u / ((SYST_RVR + 1u) * 1000u));
    default: return gaps_100ms;
    }
}
