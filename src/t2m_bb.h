/* Trigger2MIDI on the Blackbox: the two inputs' detectors and their settings (src/t2m_bb.c). */
#pragma once
#include <stdint.h>

#include "clkmon.h"
#include "t2m.h"

#define T2M_INPUTS   2
#define T2M_PORT_TRS 1
#define T2M_PORT_USB 2
#define T2M_HIST     24               /* hits kept per input for the page's history */
#define T2M_HIST_BLOCKS 750           /* the history shows the last 4 s (audio blocks of 5.33 ms) */

struct t2m_hit {
    uint32_t blk;                     /* the audio block of the hit */
    uint8_t vel, note, _r[2];
};

struct t2m_mem {
    uint8_t on[T2M_INPUTS];          /* input L, R */
    uint8_t port[T2M_INPUTS];        /* T2M_PORT_TRS | T2M_PORT_USB */
    uint8_t last_vel[T2M_INPUTS];    /* last hit's velocity */
    uint8_t last_note[T2M_INPUTS];   /* ... and the note (the MIDI note out) */
    uint32_t hits[T2M_INPUTS];       /* hits so far */
    float peak[T2M_INPUTS];          /* input level after the gain, decaying */
    uint32_t blocks;                 /* audio blocks since boot */
    uint32_t hit_blk[T2M_INPUTS];    /* the block of the last hit */
    /* a pad of the Blackbox itself as a target (experimental; off until a pad has been learned) */
    uint8_t pad_on[T2M_INPUTS];
    uint8_t pad_note[T2M_INPUTS];    /* the note sent to the pad */
    int8_t pad_held[T2M_INPUTS];     /* the pad note that sounds (for its note-off), -1 none */
    uint8_t learn;                   /* 0 off, else the input + 1 whose pad the next stock pad hit teaches */
    uint32_t pad_id[T2M_INPUTS];     /* the stock note-on's pad argument, as learned */
    uint32_t pad_sent[T2M_INPUTS];   /* pad note-ons sent (for the page) */
    float scan_saved[T2M_INPUTS];    /* SCAN before VEL went fixed (which sets it to 0); restored when VEL goes back to DYN */
    uint8_t hist_n[T2M_INPUTS];      /* the history's write position */
    uint8_t _r0[2];
    struct t2m_hit hist[T2M_INPUTS][T2M_HIST];
    struct {                         /* the TRIG page (src/t2m_page.c) */
        uint8_t in, row, sheet, _r;  /* the input shown, the row the knobs turn, the footer's sheet */
        int16_t kacc[4];             /* knob counts not yet a step */
    } ui;
    t2m_params p[T2M_INPUTS];
    t2m_state st[T2M_INPUTS];
    struct clkmon clk;               /* the MIDI clock monitor (src/clkmon.c) */
};

void t2m_boot(void);
struct t2m_mem *t2m_mem(void);       /* 0 if there was no memory for it this boot */
void t2m_audio(const float *l, const float *r, int frames);
void t2m_seq(uint32_t lo, int32_t hi);   /* the stock sequencer's musical time, from seq_play (src/seqfix.c) */
/* every stock note-on to a pad (hardware pad hits and the Keys screen), through src/t2m_thunk.S */
void t2m_learn(void *engine, uint32_t pad, uint32_t note, uint32_t a4);

/* the TRIG page (src/t2m_page.c), drawn and touched by src/looper_page.c */
void t2m_page_draw(int w, int foot);
void t2m_page_touch(int x, int d, int w, int foot);
void t2m_page_knob(int knob, int counts);
void t2m_page_sheet(int sheet);
int t2m_page_sheet_now(void);        /* the sheet shown */
uint32_t t2m_page_sig(void);
