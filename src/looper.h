/* Live looper: what the Looper page (solo.c) needs from the engine (looper.c). */
#pragma once

#define LOOPER_TRACKS 4

enum { LOOPER_EMPTY, LOOPER_REC, LOOPER_PLAY, LOOPER_DUB, LOOPER_CLEARING, LOOPER_UNDOING };

/* Touch events the page sends; the engine times holds, double taps and latches against its audio clock. */
enum { LOOPER_EV_REC_DOWN, LOOPER_EV_REC_UP, LOOPER_EV_MUTE_DOWN, LOOPER_EV_MUTE_UP, LOOPER_EV_REVERSE };

struct looper_info {
    int mode, muted, reversed, latched, undo;
    float level, pan;
};

int looper_ready(void);
void looper_event(int track, int ev);
void looper_set_level(int track, float level);
void looper_set_pan(int track, float pan);
void looper_track(int track, struct looper_info *out);
float looper_progress(void);
unsigned looper_len(void);                       /* loop length in frames, 0 = no loop yet */
unsigned looper_ticks(void);                     /* audio blocks since boot */
