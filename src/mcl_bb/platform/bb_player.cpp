/* The SAMPLE button's file player, MCL's side (src/mcl_bb/bb_grid.h struct bb_player): parses a WAV file and keeps the ring filled from the
 * card in MCL's task (bb_player_pump, every pass of its loop); the audio stage in the cave plays it. 8 / 16 / 24 / 32-bit integer PCM and
 * 32-bit float, mono or stereo (more channels: the first two). */
#include "Arduino.h"
#include "SdFat.h"
#include "bb_grid.h"

#define HOOK extern "C" __attribute__((section(".hook"), used))

static int16_t ring[BB_PLAYER_RING * 2] __attribute__((aligned(8)));
static struct bb_player pl = {0, 0, 0, 0, 0, 0, 1.0f, ring};
static FsFile file;
static uint32_t data_off, data_len, frames, fpos;            /* the data chunk; frames in it; the next frame to read */
static uint16_t channels, bits, format;                       /* format 1 PCM, 3 float */
static uint8_t open_ok;

HOOK struct bb_player *bb_player(void) { return &pl; }

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

HOOK void bb_player_stop(void)
{
    pl.state = 0;
}

HOOK void bb_player_close(void)
{
    pl.state = 0;
    pl.end = 0;
    if (open_ok)
        file.close();
    open_ok = 0;
}

/* Opens name (relative to the current directory, as MCL's browser has it) and fills the ring. 1 if it is a WAV file we can play. */
HOOK int bb_player_open(const char *name)
{
    bb_player_close();
    if (!file.open(name, O_READ))
        return 0;
    open_ok = 1;
    uint8_t h[12];
    if (file.read(h, 12) != 12 || le32(h) != 0x46464952u || le32(h + 8) != 0x45564157u) {      /* "RIFF" .... "WAVE" */
        bb_player_close();
        return 0;
    }
    uint32_t at = 12, rate = 0;
    channels = bits = format = 0;
    data_off = data_len = 0;
    for (int n = 0; n < 32; n++) {                            /* the chunks: "fmt " and "data" */
        uint8_t c[8];
        if (!file.seekSet(at) || file.read(c, 8) != 8)
            break;
        uint32_t id = le32(c), len = le32(c + 4);
        if (id == 0x20746d66u && len >= 16) {                 /* "fmt " */
            uint8_t f[16];
            if (file.read(f, 16) != 16)
                break;
            format = le16(f);
            channels = le16(f + 2);
            rate = le32(f + 4);
            bits = le16(f + 14);
            if (format == 0xfffe && len >= 26) {              /* WAVE_FORMAT_EXTENSIBLE: the sub-format's first two bytes */
                uint8_t x[10];
                if (file.read(x, 10) == 10)
                    format = le16(x + 8);
            }
        } else if (id == 0x61746164u) {                       /* "data" */
            data_off = at + 8;
            data_len = len;
            break;
        }
        at += 8 + len + (len & 1);
    }
    uint32_t bpf = (uint32_t)channels * (bits / 8);
    if (!data_off || !rate || !channels || !bpf || !((format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) || (format == 3 && bits == 32))) {
        bb_player_close();
        return 0;
    }
    frames = data_len / bpf;
    fpos = 0;
    pl.rd = pl.wr = pl.frac = 0;
    pl.step = (uint32_t)(((uint64_t)rate << 16) / 48000u);
    pl.end = frames;
    return 1;
}

static int16_t sample(const uint8_t *p)
{
    if (format == 3) {
        float f;
        memcpy(&f, p, 4);
        f = f > 1.f ? 1.f : f < -1.f ? -1.f : f;
        return (int16_t)(f * 32767.f);
    }
    switch (bits) {
    case 8: return (int16_t)((p[0] - 128) << 8);
    case 16: return (int16_t)le16(p);
    case 24: return (int16_t)le16(p + 1);
    default: return (int16_t)le16(p + 2);
    }
}

/* Keeps the ring filled (MCL's task): up to 2 KB of the file a call, while there is room. */
HOOK void bb_player_pump(void)
{
    if (!open_ok || fpos >= frames)
        return;
    uint32_t bps = bits / 8, bpf = channels * bps;
    for (int k = 0; k < 8; k++) {
        uint32_t room = BB_PLAYER_RING - (pl.wr - pl.rd) - 2;   /* (the audio stage reads frame rd + 1 too) */
        if ((int32_t)room <= 0 || fpos >= frames)
            return;
        uint8_t buf[2048];
        uint32_t n = sizeof buf / bpf;
        if (n > room)
            n = room;
        if (n > frames - fpos)
            n = frames - fpos;
        if (!n || !file.seekSet(data_off + fpos * bpf) || file.read(buf, n * bpf) != (int)(n * bpf)) {
            frames = fpos;                                    /* (a read error: it ends here) */
            pl.end = fpos;
            return;
        }
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *p = buf + i * bpf;
            uint32_t w = (pl.wr + i) & (BB_PLAYER_RING - 1);
            ring[2 * w] = sample(p);
            ring[2 * w + 1] = channels > 1 ? sample(p + bps) : ring[2 * w];
        }
        __asm volatile("dmb" ::: "memory");
        pl.wr += n;
        fpos += n;
    }
}

/* From the start (the file must be open; the ring holds its beginning while nothing has been played). */
HOOK void bb_player_play(void)
{
    if (pl.end)
        pl.state = 1;
}
