#include "bb_sdtest.h"

#include "bb_sd.h"

#define HOOK extern "C" __attribute__((section(".hook"), used))
#define DWT_CYCCNT (*(volatile uint32_t *)0xe0001004u)
#define SYST_RVR   (*(volatile uint32_t *)0xe000e014u)

static char lines[4][24];
static char big_name[SD_NAME_MAX];
static uint64_t big_size;
static uint8_t buf[4096];

static uint32_t ms_since(uint32_t t0) { return (uint32_t)((uint64_t)(DWT_CYCCNT - t0) * 1000u / ((SYST_RVR + 1u) * 1000u)); }

struct Line {
    char *p;
    int n;
    explicit Line(char *dst) : p(dst), n(0) { p[0] = 0; }
    Line &s(const char *t)
    {
        while (*t && n < 21)
            p[n++] = *t++;
        p[n] = 0;
        return *this;
    }
    Line &u(uint32_t v)
    {
        char t[12];
        int k = 0;
        do
            t[k++] = (char)('0' + v % 10), v /= 10;
        while (v);
        while (k && n < 21)
            p[n++] = t[--k];
        p[n] = 0;
        return *this;
    }
};

static uint32_t crc_update(uint32_t c, const uint8_t *d, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return c;
}

HOOK const char *bb_sdtest_line(int row) { return row >= 0 && row < 4 ? lines[row] : ""; }

HOOK void bb_sdtest_list(void)
{
    Line a(lines[0]), b(lines[1]);
    big_name[0] = 0;
    big_size = 0;
    int d = bb_sd_opendir("/");
    if (d < 0) {
        a.s("LS ERR ").u((uint32_t)-d);
        return;
    }
    uint32_t files = 0, dirs = 0;
    static BbDirEnt ent;
    for (int guard = 0; guard < 2000; guard++) {
        uint32_t r = bb_sd_readdir(d, &ent);
        if (r || !ent.name[0])
            break;
        if (ent.attr & SD_ATTR_DIR) {
            dirs++;
        } else {
            files++;
            if (ent.size > big_size && ent.size <= 0x7fffffffu) {
                big_size = ent.size;
                for (int i = 0; i < SD_NAME_MAX; i++) {
                    big_name[i] = ent.name[i];
                    if (!ent.name[i])
                        break;
                }
            }
        }
    }
    bb_sd_closedir(d);
    a.s("LS F").u(files).s(" D").u(dirs);
    b.s("BIG ").u((uint32_t)(big_size >> 10)).s("K ").s(big_name);
}

HOOK void bb_sdtest_read(void)
{
    Line a(lines[1]), b(lines[2]);
    if (!big_name[0]) {
        a.s("RD: LIST FIRST");
        return;
    }
    char path[SD_NAME_MAX + 2];
    int k = 0;
    path[k++] = '/';
    for (int i = 0; big_name[i] && k < SD_NAME_MAX; i++)
        path[k++] = big_name[i];
    path[k] = 0;
    uint32_t want = big_size > 262144u ? 262144u : (uint32_t)big_size;
    int h = bb_sd_open(path, SD_READ);
    if (h < 0) {
        a.s("RD OPEN ERR ").u((uint32_t)-h);
        return;
    }
    uint32_t t0 = DWT_CYCCNT, crc1 = 0xffffffffu, left = want;
    while (left) {
        int32_t n = bb_sd_read(h, buf, left > 4096u ? 4096u : left);
        if (n <= 0) {
            a.s("RD1 ERR ").u((uint32_t)-n);
            bb_sd_close(h);
            return;
        }
        crc1 = crc_update(crc1, buf, (uint32_t)n);
        left -= (uint32_t)n;
    }
    uint32_t ms1 = ms_since(t0);
    bb_sd_seek(h, 0);
    uint32_t crc2 = 0xffffffffu;
    left = want;
    while (left) {
        int32_t n = bb_sd_read(h, buf, left > 100u ? 100u : left);
        if (n <= 0) {
            a.s("RD2 ERR ").u((uint32_t)-n);
            bb_sd_close(h);
            return;
        }
        crc2 = crc_update(crc2, buf, (uint32_t)n);
        left -= (uint32_t)n;
    }
    bb_sd_close(h);
    a.s("RD ").u(want >> 10).s("K ").u(ms1).s("ms ").s(crc1 == crc2 ? "OK" : "CRC!");
    b.s("KB/S ").u(ms1 ? (uint32_t)((uint64_t)want * 1000u / 1024u / ms1) : 0);
}

HOOK void bb_sdtest_write(void)
{
    Line a(lines[2]), b(lines[3]);
    const uint32_t total = 131072u;
    uint32_t t0 = DWT_CYCCNT, seed = 12345u;
    int h = bb_sd_open("/BBSD.TMP", SD_CREATE_ALWAYS | SD_WRITE | SD_READ);
    if (h < 0) {
        a.s("WR OPEN ERR ").u((uint32_t)-h);
        return;
    }
    for (uint32_t done = 0; done < total; done += 4096u) {
        for (uint32_t i = 0; i < 4096u; i++) {
            seed = seed * 1664525u + 1013904223u;
            buf[i] = (uint8_t)(seed >> 24);
        }
        int32_t n = bb_sd_write(h, buf, 4096u);
        if (n != 4096) {
            a.s("WR ERR ").u((uint32_t)-n).s(" AT ").u(done >> 10).s("K");
            bb_sd_close(h);
            return;
        }
    }
    uint32_t r = bb_sd_close(h);
    uint32_t wms = ms_since(t0);
    if (r) {
        a.s("WR CLOSE ERR ").u(r);
        return;
    }
    BbDirEnt ent;
    r = bb_sd_stat("/BBSD.TMP", &ent);
    if (r || ent.size != total) {
        a.s("WR SIZE ").u(r ? 0xffffu : (uint32_t)ent.size);
        return;
    }
    t0 = DWT_CYCCNT;
    h = bb_sd_open("/BBSD.TMP", SD_READ);
    if (h < 0) {
        a.s("RB OPEN ERR ").u((uint32_t)-h);
        return;
    }
    seed = 12345u;
    uint32_t bad = 0;
    for (uint32_t done = 0; done < total; done += 4096u) {
        int32_t n = bb_sd_read(h, buf, 4096u);
        if (n != 4096) {
            bad = done + 1;
            break;
        }
        for (uint32_t i = 0; i < 4096u && !bad; i++) {
            seed = seed * 1664525u + 1013904223u;
            if (buf[i] != (uint8_t)(seed >> 24))
                bad = done + i + 1;
        }
        if (bad)
            break;
    }
    bb_sd_close(h);
    uint32_t rms = ms_since(t0);
    r = bb_sd_unlink("/BBSD.TMP");
    uint32_t gone = bb_sd_stat("/BBSD.TMP", &ent);
    a.s("WR ").u(total >> 10).s("K ").u(wms).s("ms RB ").u(rms).s("ms");
    b.s(bad ? "VERIFY FAIL AT " : "VERIFY OK ").u(bad).s(r == 0 && gone == SD_NO_FILE ? " DEL OK" : " DEL ERR");
}
