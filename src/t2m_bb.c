/*
 * Trigger2MIDI on the Blackbox: the detector (src/t2m.c) on the two audio inputs, MIDI out through the stock MIDI port.
 * Original 1010music Blackbox, firmware 3.1.9.
 *
 * Input: looper_in (src/looper.c, the input stage's tail call) hands every 256-frame block's input L / R floats here
 * (full scale +-1.0), before the render. Each input has its own detector, settings and output port.
 *
 * MIDI out (stock, BoomboxFramework MidiPort.cpp): the port manager at 0x24002e88 holds a queue per UART (two; the
 * MIDI jack is USART6 at 31250 baud, set up at 0x08044da0) and a ring for USB. FUN_08044a0e(mgr, msg, uart, usb) puts
 * one message on the UART queues (uart != 0) and / or the USB ring (usb != 0); the engine's own note, CC and clock
 * output calls it the same way from the render (0x0804d004, 0x0804d1e2). The render then sends the queues out
 * (FUN_08044ad8 @0x0804ced8) in the same audio block, so a hit goes out in the block it was found in.
 * A message is 5 bytes: status, data 1, data 2, unused, length.
 *
 * Memory: the settings and the two detectors (about 1 KB) come from the stock SDRAM allocator once at boot (from
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

#define fw_alloc     ((alloc_fn)FN(0x08044404))
#define fw_ext_free  ((free_fn)FN(0x080443f0))
#define fw_midi_send ((midi_send_fn)FN(0x08044a0e))
#define MIDI_MGR     ((void *)0x24002e88u)

struct t2m_root {
    uint32_t magic;
    struct t2m_mem *mem;
};
#define R ((volatile struct t2m_root *)0x38800980u)
#define MAGIC 0x54324d31u                /* "T2M1" */
#define SPARE (256u * 1024u)             /* leave at least this much in the allocator */

void bkp_enable(void);

static const t2m_params defaults = {
    .sens = 2.0f, .thresh = 2.0f, .retrig = 8.0f, .mask_ms = 15.0f, .scan_ms = 2.0f, .curve = 0.6f,
    .strict_db = 8.0f, .speed_ms = 3.0f, .spec_strict = 0.15f, .spec_range = 0.15f, .len_ms = 50,
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
        t2m_reset(&m->st[k]);
    }
    m->on[0] = 1;                               /* input L on, R off */
    R->mem = m;
    R->magic = MAGIC;
}

struct t2m_mem *t2m_mem(void)
{
    return R->magic == MAGIC ? R->mem : 0;
}

/* From looper_in, once per audio block, before the render. */
void t2m_audio(const float *l, const float *r, int frames)
{
    struct t2m_mem *m = t2m_mem();
    if (!m || frames <= 0 || !l || !r)
        return;
    const float *in[T2M_INPUTS] = {l, r};
    for (int k = 0; k < T2M_INPUTS; k++) {
        float pk = 0.f, g = m->p[k].sens;
        for (int i = 0; i < frames; i++) {
            float x = in[k][i] * g;
            x = x < 0.f ? -x : x;
            pk = x > pk ? x : pk;
        }
        m->peak[k] = pk > m->peak[k] * 0.9f ? pk : m->peak[k] * 0.9f;   /* for the page's meter */
        if (!m->on[k]) {
            if (m->st[k].held >= 0 || m->st[k].n_offs) {                /* switched off: end what sounds */
                t2m_state *t = &m->st[k];
                uint8_t ch = (uint8_t)((m->p[k].channel - 1) & 15);
                uint8_t msg[5] = {0, 0, 0, 0, 3};
                msg[0] = 0x80 | ch;
                if (t->held >= 0) {
                    msg[1] = (uint8_t)t->held;
                    fw_midi_send(MIDI_MGR, msg, m->port[k] & T2M_PORT_TRS, m->port[k] & T2M_PORT_USB);
                }
                for (uint32_t j = 0; j < t->n_offs; j++) {
                    msg[1] = t->offs[j].note;
                    fw_midi_send(MIDI_MGR, msg, m->port[k] & T2M_PORT_TRS, m->port[k] & T2M_PORT_USB);
                }
                t2m_reset(t);
            }
            continue;
        }
        t2m_state *t = &m->st[k];
        uint32_t n = t2m_process(t, &m->p[k], in[k], (uint32_t)frames);
        for (uint32_t j = 0; j < n; j++) {
            const t2m_event *e = &t->events[j];
            uint8_t msg[5] = {e->status, e->d1, e->d2, 0, 3};
            fw_midi_send(MIDI_MGR, msg, m->port[k] & T2M_PORT_TRS, m->port[k] & T2M_PORT_USB);
            if ((e->status & 0xf0) == 0x90) {
                m->hits[k]++;
                m->last_vel[k] = e->d2;
            } else if ((e->status & 0xf0) == 0xb0 && !m->p[k].note_on) {
                m->hits[k]++;
                m->last_vel[k] = e->d2;
            }
        }
    }
}
