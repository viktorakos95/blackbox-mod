/* SAMPLR: a sample played from the touch screen (waveform, slicer, tape ...). The state lives in the looper's spare
 * effect memory block (looper_scratch(9)); samplr_run mixes the voices into the Out 1 bus after the looper. */
#pragma once
#include <stdint.h>

#define SM_COLS   150            /* waveform overview columns */
#define SM_VOICES 4              /* one per finger */
#define SM_TMP    2112           /* source frames one voice reads per block (8x pitch up of 256 frames + margin) */
#define SM_PADS   16

enum { SM_SLICER, SM_TAPE, SM_MODES };

struct smvoice {
    volatile uint32_t cmd, seen;            /* trigger sequence: the GUI task bumps cmd after writing the c_ fields */
    int32_t c_pos, c_start, c_end;
    float c_rate, c_gain;
    uint8_t c_loop, c_gate, held, rel;
    uint8_t on, loop, slice, _p;
    int32_t ipos, start, end;
    float frac, rate, gain, env;            /* rate and gain are rewritten live by the GUI (tape) */
};

struct sm {
    uint32_t magic;
    uint8_t mode, gate, nslice, npads;
    uint8_t sel, ov_ok, mono;
    uint8_t dbg[3];                         /* pads with rec[8] set, with an id, with a live sample */
    uint8_t pad_row[SM_PADS], pad_col[SM_PADS];
    uint16_t pad_id[SM_PADS];
    int32_t id, len, hz;
    float ratio, vol;
    uint32_t ov_t;
    int16_t fx0[SM_VOICES];                 /* GUI: where each finger went down (tape) */
    struct smvoice v[SM_VOICES];
    uint16_t filled;                        /* overview columns complete */
    uint8_t ofill[SM_COLS];
    float omn[SM_COLS], omx[SM_COLS];
    int8_t ov[2][SM_COLS];                  /* per column: lowest and highest sample, -127..127 */
    float tl[SM_TMP], tr[SM_TMP];
};

struct sm *samplr(void);                    /* 0 until the looper's memory is up */
void samplr_run(float *bl, float *br, int n);
void samplr_refresh(unsigned ticks);        /* call while the tab shows: fills the waveform as a streamed sample loads */
void samplr_enter(void);                    /* the tab was opened: look at the pads again */
void samplr_leave(void);                    /* the tab was left: let go of everything */
void samplr_select(int delta);              /* previous / next loaded pad sample */
void samplr_set_mode(int m);
void samplr_toggle_gate(void);
void samplr_set_slices(int n);
void samplr_touch(int kind, int id, int fx, int fy);   /* kind 0 down, 1 move, 2 up; fx, fy 0..1023 inside the waveform */
void samplr_name(char *out, int max);       /* the selected sample's name for the page */
uint32_t samplr_sig(void);
