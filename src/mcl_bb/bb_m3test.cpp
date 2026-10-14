/*
 * M3 hardware self test: poll-driven so the emulator test can run it with a model clock and a model Machinedrum.
 *   1. LOOP  : sends one system-exclusive frame with the non-commercial id 0x7d (F0 7D 'M' 'C' 'L' seq <96 bytes> F7), 103 bytes, and waits up to
 *              150 ms for the identical frame on the receive side. With a cable from MIDI out to MIDI in it must come back byte-exact.
 *   2. MD    : Machinedrum status request for the current kit (F0 00 20 3C 02 00 70 02 F7), answer F0 00 20 3C 02 00 72 02 <kit> F7 within 150 ms.
 *   3. DUMP  : if the Machinedrum answered, asks for that kit twice (F0 00 20 3C 02 00 53 <kit> F7), takes the F0 .. F7 frames that start with
 *              F0 00 20 3C 02 00 52 within 800 ms each, and compares length and CRC-32 of the two. Both must be byte-identical.
 *   4. BURST : only if the Machinedrum did not answer: 16 short drum-range notes on channels 1..4 (see S_BURST). Audible proof that MIDI out reaches it.
 * Only the id 0x7d frame is anything but Elektron-specific data, so other gear on the jack ignores it.
 */
#include "bb_m3test.h"
#include "bb_midi.h"

enum { S_START, S_LOOP_WAIT, S_MD_SEND, S_MD_WAIT, S_DUMP_SEND, S_DUMP_WAIT, S_BURST, S_DONE };
static uint8_t state;
static uint8_t dump_n;
static uint32_t t0;
static m3_result res;
static uint8_t loop_frame[103];
static uint8_t frame[2048];
static uint32_t flen;
static uint8_t in_frame;
static uint32_t first_crc, first_len;
static uint32_t burst_i, burst_off_at;
static uint8_t burst_pending;

static uint32_t crc32(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xffffffffu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return ~c;
}

/* collects F0 .. F7 frames from the receive ring; returns the length of a finished frame (in `frame`) or 0 */
static uint32_t poll_frame(void)
{
    int b;
    while ((b = bb_midi_rx_get()) >= 0) {
        res.rx_bytes++;
        if (b >= 0xf8)
            continue;                                   /* real-time bytes may sit anywhere */
        if (b == 0xf0) {
            in_frame = 1;
            flen = 0;
        }
        if (!in_frame)
            continue;
        if (flen < sizeof frame)
            frame[flen++] = (uint8_t)b;
        else
            flen = sizeof frame + 1;                    /* too long: stays flagged */
        if (b == 0xf7) {
            in_frame = 0;
            return flen;
        }
    }
    return 0;
}

static void send(const uint8_t *p, uint32_t n)
{
    if (bb_midi_tx(p, n))
        res.tx_bytes += n;
}

static const uint8_t MD_HDR[6] = {0xf0, 0x00, 0x20, 0x3c, 0x02, 0x00};

static int md_frame(uint8_t id)
{
    if (flen < 9 || flen > sizeof frame)
        return 0;
    for (int i = 0; i < 6; i++)
        if (frame[i] != MD_HDR[i])
            return 0;
    return frame[6] == id;
}

extern "C" {

void bb_m3_reset(void)
{
    state = S_START;
    dump_n = 0;
    in_frame = 0;
    flen = 0;
    res = m3_result();
}

const struct m3_result *bb_m3_result(void) { return &res; }

int bb_m3_step(uint32_t now)
{
    switch (state) {
    case S_START: {
        bb_midi_claim(1);
        if (!bb_midi_owner()) {                     /* the stock firmware was in the middle of a send: say so, don't pretend nothing answered */
            res.loop = res.md = 3;
            state = S_DONE;
            return 0;
        }
        while (bb_midi_rx_get() >= 0) {}
        loop_frame[0] = 0xf0;
        loop_frame[1] = 0x7d;
        loop_frame[2] = 'M';
        loop_frame[3] = 'C';
        loop_frame[4] = 'L';
        loop_frame[5] = 1;
        uint32_t x = 0x1234567u;
        for (int i = 6; i < 102; i++) {
            x = x * 1103515245u + 12345u;
            loop_frame[i] = (uint8_t)((x >> 16) & 0x7f);
        }
        loop_frame[102] = 0xf7;
        in_frame = 0;
        send(loop_frame, sizeof loop_frame);
        t0 = now;
        state = S_LOOP_WAIT;
        return 0;
    }
    case S_LOOP_WAIT: {
        uint32_t n = poll_frame();
        if (n) {
            int same = n == sizeof loop_frame;
            for (uint32_t i = 0; same && i < n; i++)
                same = frame[i] == loop_frame[i];
            if (same) {
                res.loop = 1;
                state = S_MD_SEND;
            }
        }
        if (state == S_LOOP_WAIT && now - t0 > 150000u) {
            res.loop = 2;
            state = S_MD_SEND;
        }
        return 0;
    }
    case S_MD_SEND: {
        static const uint8_t req[9] = {0xf0, 0x00, 0x20, 0x3c, 0x02, 0x00, 0x70, 0x02, 0xf7};
        in_frame = 0;
        while (bb_midi_rx_get() >= 0) {}
        send(req, sizeof req);
        t0 = now;
        state = S_MD_WAIT;
        return 0;
    }
    case S_MD_WAIT: {
        uint32_t n = poll_frame();
        if (n && md_frame(0x72) && frame[7] == 0x02 && n == 10) {
            res.md = 1;
            res.kit = frame[8] & 0x7f;
            state = S_DUMP_SEND;
        } else if (now - t0 > 150000u) {
            res.md = 2;
            res.burst = 1;                               /* no answer: play something the ear can judge (see below) */
            burst_i = 0;
            t0 = now;
            state = S_BURST;
        }
        return 0;
    }
    case S_BURST: {
        /* 16 short notes, one every 100 ms (note-on, note-off 70 ms later): channels 1..4, notes 36 / 40 / 44 / 48. A Machinedrum on its default
         * base channel plays drum hits for these. Hearing them proves the Blackbox's MIDI out reaches the Machinedrum, whatever it does with sysex. */
        uint32_t due = t0 + burst_i * 100000u;
        if (burst_i < 16 && now >= due) {
            uint8_t on[3] = {(uint8_t)(0x90 | (burst_i / 4)), (uint8_t)(36 + 4 * (burst_i % 4)), 110};
            send(on, 3);
            burst_off_at = now + 70000u;
            burst_i++;
            burst_pending = 1;
        }
        if (burst_pending && now >= burst_off_at) {
            uint8_t off[3] = {(uint8_t)(0x80 | ((burst_i - 1) / 4)), (uint8_t)(36 + 4 * ((burst_i - 1) % 4)), 0};
            send(off, 3);
            burst_pending = 0;
        }
        if (burst_i >= 16 && !burst_pending)
            state = S_DONE;
        return 0;
    }
    case S_DUMP_SEND: {
        uint8_t req[9] = {0xf0, 0x00, 0x20, 0x3c, 0x02, 0x00, 0x53, res.kit, 0xf7};
        in_frame = 0;
        while (bb_midi_rx_get() >= 0) {}
        send(req, sizeof req);
        t0 = now;
        state = S_DUMP_WAIT;
        return 0;
    }
    case S_DUMP_WAIT: {
        uint32_t n = poll_frame();
        if (n && md_frame(0x52)) {
            res.dump_len = (uint16_t)n;
            res.dump_crc = crc32(frame, n);
            if (dump_n == 0) {
                first_crc = res.dump_crc;
                first_len = n;
                dump_n = 1;
                state = S_DUMP_SEND;
            } else {
                res.dump = (res.dump_crc == first_crc && n == first_len && n > 1000) ? 1 : 2;
                state = S_DONE;
            }
        } else if (now - t0 > 800000u) {
            res.dump = dump_n ? 2 : 3;
            state = S_DONE;
        }
        return 0;
    }
    default:
        return 1;
    }
}

}
