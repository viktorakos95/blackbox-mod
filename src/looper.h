/* Live looper: what the Mixer screen's Looper mode (solo.c) needs from the engine (looper.c). */
#pragma once

#define LOOPER_TRACKS 4

enum { LOOPER_EMPTY, LOOPER_REC, LOOPER_PLAY, LOOPER_DUB, LOOPER_CLEARING };
enum { LOOPER_CMD_REC = 1, LOOPER_CMD_MUTE, LOOPER_CMD_CLEAR };

struct looper_info {
    int mode, muted, busy;
    float level;
};

int looper_ready(void);
void looper_command(int track, int cmd);
void looper_set_level(int track, float level);
void looper_track(int track, struct looper_info *out);
float looper_progress(void);
