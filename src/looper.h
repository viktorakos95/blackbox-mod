/* Live looper: what the Looper page (solo.c, looper_page.c) needs from the engine (looper.c). */
#pragma once

#define LOOPER_TRACKS 4

enum { LOOPER_EMPTY, LOOPER_REC, LOOPER_PLAY, LOOPER_DUB, LOOPER_CLEARING, LOOPER_UNDOING };

/* Touch events the page sends; the engine times holds, double taps and latches against its audio clock. */
enum { LOOPER_EV_REC_DOWN, LOOPER_EV_REC_UP, LOOPER_EV_MUTE_DOWN, LOOPER_EV_MUTE_UP, LOOPER_EV_REVERSE,
       LOOPER_EV_HALF };

/* Per-track parameters (looper_set_param). */
enum { LOOPER_P_FILT, LOOPER_P_CRUNCH, LOOPER_P_SEND_D, LOOPER_P_SEND_R, LOOPER_PARAMS };

/* Global options (looper_set_opt): the first four are choices (whole numbers), the rest amounts 0..1. */
enum { LOOPER_O_LEN, LOOPER_O_SYNC, LOOPER_O_QUANT, LOOPER_O_SRC, LOOPER_O_DTIME,
       LOOPER_O_DFB, LOOPER_O_DRET, LOOPER_O_RSIZE, LOOPER_O_RRET, LOOPER_OPTS };
enum { LOOPER_LEN_FOLLOW, LOOPER_LEN_MULT, LOOPER_LEN_FREE };      /* LOOPER_O_LEN */
enum { LOOPER_SRC_INPUT, LOOPER_SRC_MIX };                         /* LOOPER_O_SRC */

struct looper_info {
    int mode, muted, reversed, latched, undo, armed, half;
    float level, pan, filt, crunch, send_d, send_r;
    float progress;                               /* playhead 0..1; while a first take records: fraction of the memory */
};

int looper_ready(void);
void looper_event(int track, int ev);
void looper_set_level(int track, float level);
void looper_set_pan(int track, float pan);
void looper_set_param(int track, int param, float v);   /* LOOPER_P_*: filt -1..1, the others 0..1 */
float looper_get_param(int track, int param);
void looper_set_opt(int opt, float v);
float looper_get_opt(int opt);
void looper_clear_all(void);
void looper_track(int track, struct looper_info *out);
float looper_progress(void);                     /* master loop playhead 0..1 (0 = none), or first-take fill */
float looper_bpm(void);                          /* tempo the clock features use, 0 = not read yet */
unsigned looper_len(void);                       /* master loop length in frames, 0 = no loop yet */
unsigned looper_ticks(void);                     /* audio blocks since boot */
unsigned looper_running(void);                   /* 1 while the Blackbox transport has been playing */
