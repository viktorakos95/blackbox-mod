/*
 * Trigger2MIDI on the Blackbox: the detector (src/t2m.c) on the two audio inputs, MIDI out through the stock MIDI port,
 * and optionally a pad of the Blackbox itself as a target. Original 1010music Blackbox, firmware 3.1.9.
 *
 * Input: looper_in (src/looper.c, the input stage's tail call) hands every 256-frame block's input L / R floats here
 * (full scale +-1.0), before the render. Each input has its own detector, settings and output port.
 *
 * MIDI out (stock, BoomboxFramework MidiPort.cpp): the port manager at 0x24002e88 holds a queue per UART (the MIDI jack
 * is USART6 at 31250 baud, set up at 0x08044da0) and a ring for USB. FUN_08044a0e(mgr, msg, uart, usb) puts one message
 * on the UART queue and / or the USB ring; the engine's own note, CC and clock output calls it the same way from the
 * render (0x0804d004, 0x0804d1e2), and the render sends the queues out (FUN_08044ad8 @0x0804ced8) in the same audio
 * block. A message is 5 bytes: status, data 1, data 2, unused, length.
 *
 * A pad of the Blackbox: the Keys screen and the pads play a pad with the engine command 0x42 (note on) / 0x43 (note off),
 * built by FUN_0804c61c / FUN_0804c5d4 and queued for the engine (FUN_08051738, a queue only the GUI task may write). The
 * render drains that queue in the audio task and calls FUN_080520ec(obj, command) for each command. This file is also in
 * the audio task, so it builds the same 24-byte command and calls the dispatcher directly: no queue, no second writer.
 * Which pad: the stock note-on's pad argument is learned (t2m_learn, a thunk on FUN_0804c61c): arm LEARN on the page and
 * hit a pad. EXPERIMENTAL: untested on hardware, off until a pad has been learned.
 *
 * Memory: the settings and the two detectors (about 1.6 KB) come from the stock SDRAM allocator once at boot (from
 * looper_boot, after the sample pool has its memory, so nothing else is allocating then), only if well over that is
 * free (the allocator halts when it runs out). The pointer lives in the backup SRAM at 0x38800980 (free: seqfix.c's
 * clock ends at 0x38800910, fx2.c starts at 0x38800a00). Settings are the patch's defaults at every boot.
 */
#include <stdint.h>

#include "t2m.h"
#include "t2m_bb.h"

#define FN(addr) ((addr) | 1u)
typedef void *(*alloc_fn)(uint32_t bytes);
typedef uint32_t (*free_fn)(void);
typedef void (*midi_send_fn)(void *mgr, const uint8_t *msg, int uart, int usb);
typedef void (*dispatch_fn)(void *obj, const void *cmd);

#define fw_alloc     ((alloc_fn)FN(0x08044404))
#define fw_ext_free  ((free_fn)FN(0x080443f0))
#define fw_midi_send ((midi_send_fn)FN(0x08044a0e))
#define fw_dispatch  ((dispatch_fn)FN(0x080520ec))
#define MIDI_MGR     ((void *)0x24002e88u)
#define ENGINE       0x2400a9c0u         /* the engine pointer the Keys screen passes to FUN_0804c61c; +0x9a74 holds the command queue's object */

struct t2m_root {
    uint32_t magic;
    struct t2m_mem *mem;
};
#define R ((volatile struct t2m_root *)0x38800980u)
#define MAGIC 0x54324d32u                /* "T2M2" */
#define SPARE (256u * 1024u)             /* leave at least this much in the allocator */

void bkp_enable(void);

static const t2m_params defaults = {
    .sens = 2.0f, .thresh = 2.0f, .retrig = 8.0f, .mask_ms = 15.0f, .scan_ms = 2.0f, .curve = 0.6f,
    .strict_db = 8.0f, .speed_ms = 3.0f, .spec_strict = 0.15f, .spec_range = 0.15f, .floor_db = -60.0f, .len_ms = 50,
    .note_on = 1, .note = 38, .vel_mode = 0, .vel_fixed = 100, .len_mode = 0,
    .cc_on = 0, .cc_num = 1, .cc_mode = 0, .cc_fixed = 100, .channel = 1,
};

void t2m_boot(void)
{
    bkp_enable();
    R->magic = 0;
    R->mem = 0;
    uint32_t need = sizeof(struct t2m_mem);
    if (fw_ext_free() < need + SPARE)
        return;                                 /* no trigger inputs this boot rather than a halt */
    struct t2m_mem *m = (struct t2m_mem *)fw_alloc(need);
    if (!m)
        return;
    uint8_t *b = (uint8_t *)m;
    for (uint32_t i = 0; i < need; i++)
        b[i] = 0;
    for (int k = 0; k < T2M_INPUTS; k++) {
        m->p[k] = defaults;
        m->port[k] = T2M_PORT_TRS | T2M_PORT_USB;
        m->pad_note[k] = 60;
        m->scan_saved[k] = defaults.scan_ms;
        m->pad_held[k] = -1;
        t2m_reset(&m->st[k]);
    }
    clkmon_boot(&m->clk);                       /* the cycle counter and the clock monitor's interrupt block */
    m->on[0] = 1;                               /* input L on, R off */
    R->mem = m;
    R->magic = MAGIC;
}

struct t2m_mem *t2m_mem(void)
{
    return R->magic == MAGIC ? R->mem : 0;
}

/* ---- MIDI out */
static void midi_out(const struct t2m_mem *m, int k, uint8_t status, uint8_t d1, uint8_t d2)
{
    uint8_t msg[5] = {status, d1, d2, 0, 3};
    fw_midi_send(MIDI_MGR, msg, m->port[k] & T2M_PORT_TRS, m->port[k] & T2M_PORT_USB);
}

/* ---- a pad of the Blackbox: the engine command 0x42 / 0x43 exactly as FUN_0804c61c / FUN_0804c5d4 build it (24 bytes:
 * id, 7 zero bytes, pad argument at +8, note +0xc, a4 = 0 at +0x10, half 1 at +0x14, velocity half at +0x16) */
static void pad_cmd(uint32_t pad, unsigned note, unsigned vel, int on)
{
    if (note > 127)
        return;
    uint8_t *obj = *(uint8_t *volatile *)(ENGINE + 0x9000u + 0xa74u);
    if (!obj || ((uintptr_t)obj & 3) || (uintptr_t)obj < 0x20000000u || (uintptr_t)obj >= 0xd0000000u)
        return;                                 /* the engine is not up */
    uint32_t cmd[6] = {0, 0, pad, note, 0, 0};
    ((uint8_t *)cmd)[0] = on ? 0x42 : 0x43;
    ((uint16_t *)cmd)[10] = 1;
    ((uint16_t *)cmd)[11] = (uint16_t)vel;
    fw_dispatch(obj, cmd);
}

static void pad_event(struct t2m_mem *m, int k, int on, unsigned vel)
{
    if (on) {
        pad_cmd(m->pad_id[k], m->pad_note[k], vel, 1);
        m->pad_held[k] = (int8_t)m->pad_note[k];
        m->pad_sent[k]++;
    } else if (m->pad_held[k] >= 0) {
        pad_cmd(m->pad_id[k], (unsigned)m->pad_held[k], 0, 0);
        m->pad_held[k] = -1;
    }
}

/* Every stock note-on to a pad (hardware pad hits, the Keys screen): the pad hit that LEARN waits for. GUI task. */
void t2m_learn(void *engine, uint32_t pad, uint32_t note, uint32_t a4)
{
    (void)engine;
    (void)a4;
    struct t2m_mem *m = t2m_mem();
    if (!m || !m->learn || note > 127)
        return;
    int k = m->learn - 1;
    m->learn = 0;
    if (k < 0 || k >= T2M_INPUTS)
        return;
    m->pad_id[k] = pad;
    m->pad_note[k] = (uint8_t)note;
    __asm volatile("dmb" ::: "memory");
    m->pad_on[k] = 1;                           /* last: the audio task sees a complete target */
}

static void record(struct t2m_mem *m, int k, uint8_t vel, uint8_t note)
{
    uint8_t i = m->hist_n[k];
    m->hist[k][i].blk = m->blocks;
    m->hist[k][i].vel = vel;
    m->hist[k][i].note = note;
    m->hist_n[k] = (uint8_t)((i + 1) % T2M_HIST);
    m->hits[k]++;
    m->last_vel[k] = vel;
    m->last_note[k] = note;
    m->hit_blk[k] = m->blocks;
}

/* From looper_in, once per audio block, before the render. */
void t2m_audio(const float *l, const float *r, int frames)
{
    struct t2m_mem *m = t2m_mem();
    if (!m || frames <= 0 || !l || !r)
        return;
    const float *in[T2M_INPUTS] = {l, r};
    m->blocks++;
    clkmon_block(&m->clk);
    for (int k = 0; k < T2M_INPUTS; k++) {
        t2m_state *t = &m->st[k];
        float pk = 0.f, g = m->p[k].sens;
        for (int i = 0; i < frames; i++) {
            float x = in[k][i] * g;
            x = x < 0.f ? -x : x;
            pk = x > pk ? x : pk;
        }
        m->peak[k] = pk > m->peak[k] * 0.9f ? pk : m->peak[k] * 0.9f;   /* for the page's meter */
        uint8_t ch = (uint8_t)((m->p[k].channel - 1) & 15);
        if (!m->on[k]) {
            if (t->held >= 0 || t->n_offs) {                            /* switched off: end what sounds */
                if (m->p[k].note_on) {
                    if (t->held >= 0)
                        midi_out(m, k, 0x80 | ch, (uint8_t)t->held, 0);
                    for (uint32_t j = 0; j < t->n_offs; j++)
                        midi_out(m, k, 0x80 | ch, t->offs[j].note, 0);
                }
                t2m_reset(t);
            }
            if (m->pad_held[k] >= 0)
                pad_event(m, k, 0, 0);
            continue;
        }
        if (!m->pad_on[k] && m->pad_held[k] >= 0)
            pad_event(m, k, 0, 0);                                      /* the pad target was switched off while a note sounded */
        t2m_params pe = m->p[k];
        pe.note_on = 1;                          /* every hit gives a note-on event (the history, the pad and the page need it);
                                                    the MIDI note itself is sent only when Note Out is on */
        uint32_t n = t2m_process(t, &pe, in[k], (uint32_t)frames);
        for (uint32_t j = 0; j < n; j++) {
            const t2m_event *e = &t->events[j];
            unsigned hi = e->status & 0xf0u;
            if (hi == 0xb0) {
                midi_out(m, k, e->status, e->d1, e->d2);
                continue;
            }
            if (m->p[k].note_on)
                midi_out(m, k, e->status, e->d1, e->d2);
            if (hi == 0x90) {
                record(m, k, e->d2, e->d1);
                if (m->pad_on[k])
                    pad_event(m, k, 1, e->d2);
            } else if (m->pad_on[k] || m->pad_held[k] >= 0) {
                pad_event(m, k, 0, 0);
            }
        }
    }
}
