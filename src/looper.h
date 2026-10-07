/* Live looper: what the Looper page (solo.c, looper_page.c) needs from the engine (looper.c). */
#pragma once

#define LOOPER_TRACKS 4

enum { LOOPER_EMPTY, LOOPER_REC, LOOPER_PLAY, LOOPER_DUB, LOOPER_CLEARING, LOOPER_UNDOING };

/* Touch events the page sends; the engine times holds, double taps and latches against its audio clock. */
enum { LOOPER_EV_REC_DOWN, LOOPER_EV_REC_UP, LOOPER_EV_MUTE_DOWN, LOOPER_EV_MUTE_UP, LOOPER_EV_REVERSE,
       LOOPER_EV_HALF, LOOPER_EV_UNDO };

/* Per-track parameters (looper_set_param). */
/* The Blooper-style controls: STAB wow / flutter / noise / dull, RPT how much of the old loop is kept per overdub pass
 * (1 = all), SPEED -1..1 (0 = normal, up to 2x, down through stopped to backwards), DROP random dropouts, TRIM the
 * play head loops 1/2 ... 1/64 of the loop (steps), STUT repeats a slice of a beat (left: what just played, right: what
 * comes next; -1..1), SCRM jumps between slices of the loop (left: random, right: a repeating sequence; -1..1), SWAP
 * mutes the old loop while overdubbing (the size is the fade time). */
enum { LOOPER_P_FILT, LOOPER_P_RES, LOOPER_P_CRUNCH, LOOPER_P_DRIVE, LOOPER_P_SEND_D, LOOPER_P_SEND_R,
       LOOPER_P_STAB, LOOPER_P_RPT, LOOPER_P_SPEED, LOOPER_P_DROP, LOOPER_P_TRIM, LOOPER_P_STUT, LOOPER_P_SCRM, LOOPER_P_SWAP, LOOPER_PARAMS };

/* Global options (looper_set_opt): the first five and LOOPER_O_ROUTE are choices (whole numbers), the rest amounts 0..1.
 * QUANT: 0 = 1/4, 1 = 1/8, 2 = 1/16, 3 = 1 bar. ROUTE: 0 = the Blackbox's own delay and reverb, 1 = the looper's own.
 * GAIN: the loop's make-up gain, 1 + 3 x amount (0 .. +12 dB). */
enum { LOOPER_O_LEN, LOOPER_O_SYNC, LOOPER_O_QUANT, LOOPER_O_SRC, LOOPER_O_DTIME,
       LOOPER_O_DFB, LOOPER_O_DRET, LOOPER_O_RSIZE, LOOPER_O_RRET, LOOPER_O_GAIN, LOOPER_O_ROUTE, LOOPER_O_FULL, LOOPER_O_HWBTN, LOOPER_O_MON, LOOPER_O_PITCH, LOOPER_OPTS };      /* HWBTN: STOP / PLAY: 0 = the looper only, 1 = the sequencer too (REC is always the looper's); MON: the input is monitored through the looper (0 = off); PITCH: 0 = tape (speed changes pitch), 1 = keep the pitch (granular) for SPEED and HALF */
enum { LOOPER_ROUTE_STOCK, LOOPER_ROUTE_OWN };
enum { LOOPER_LEN_FOLLOW, LOOPER_LEN_MULT, LOOPER_LEN_FREE };      /* LOOPER_O_LEN */
enum { LOOPER_SRC_INPUT, LOOPER_SRC_MIX };                         /* LOOPER_O_SRC */

enum { LOOPER_T_PLAY, LOOPER_T_STOP };

struct looper_info {
    int mode, muted, reversed, latched, undo, undo_kind, armed, half;       /* undo_kind: what UNDO would do next: 0 nothing, 1 take the last pass off, 2 delete the loop */
    float level, pan, filt, res, crunch, drive, send_d, send_r;
    float stab, rpt, speed, drop, trim, stut, scrm, swap;
    float progress;                               /* playhead 0..1; while a first take records: fraction of the memory */
};

int looper_ready(void);
void looper_event(int track, int ev);
void looper_set_level(int track, float level);
void looper_set_pan(int track, float pan);
void looper_set_param(int track, int param, float v);   /* LOOPER_P_*: filt and speed -1..1, the others 0..1 */
float looper_get_param(int track, int param);
void looper_set_opt(int opt, float v);
float looper_get_opt(int opt);
void looper_clear_all(void);
void looper_track(int track, struct looper_info *out);
float looper_progress(void);                     /* master loop playhead 0..1 (0 = none), or first-take fill */
float looper_bpm(void);                          /* tempo the clock features use, 0 = not read yet */
unsigned looper_len(void);                       /* master loop length in frames, 0 = no loop yet */
unsigned looper_ticks(void);                     /* audio blocks since boot */
unsigned looper_running(void);
void looper_transport(int cmd);                  /* LOOPER_T_PLAY: pause / resume the loops; LOOPER_T_STOP: stop a take, else pause and rewind */
int looper_paused(void);                   /* 1 while the Blackbox transport has been playing */
