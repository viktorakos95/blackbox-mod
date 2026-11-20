/* Shared by lira_pad.c (the page's engine instance) and lira_ui.inc (the page drawn by looper_page.c). */
#ifndef LIRA_UI_H
#define LIRA_UI_H
#include <stdint.h>

int lira_ui_get(int c);
void lira_ui_set(int c, int v);
int lira_ui_key_get(int k);
void lira_ui_key(int k, int on);
int lira_ui_momentary(int toggle);
int lira_ui_sel(int set);
int lira_ui_last(int id);
unsigned lira_ui_sig(void);
int lira_ui_drag(int id, int op, int kind, int idx, int y0, int v0);
int lira_ui_button(int idx, uint32_t now, uint32_t gap);
int lira_ui_chord_get(int id);
void lira_ui_chord_set(int id, int idx);
void lira_ui_action(int a);
int lira_ui_note(int v);
const char *lira_ui_space_name(void);
int lira_ui_flats(void);
int lira_ui_default(int c);
int lira_ui_tap(int id, uint32_t now, uint32_t window);
int lira_ui_accum(int knob, int counts, int thr);
void lira_ui_note_set(int v, int note);
void lira_leds(void);
unsigned lira_free_kb(void);
int lira_ui_load(int which);
int lira_ui_prev(int which);
int lira_ui_pitch(int v);
int lira_ui_tunepitch(int v);
void lira_ui_tune_step(int v, int dir);
void lira_ui_tune_detent(int v);
int lira_ui_tune_stuck(int v);
int lira_ui_pitch_stall(int r);
int lira_ui_pitch_unstall(int h);
void lira_ui_pitch_enc(int v, int delta);
void lira_ui_pitch_set(int v, int h);
void lira_page_run(float *l, float *r, int n);
int lira_ui_save(int op);                    /* SAVE TO PROJECT (lira_pad.c): 0 ask for it, 1 the state: 0 SAVE, 1 SAVING, 2 SAVED, 3 FAILED */
int lira_ui_learn(int op, int arg);          /* MIDI learn (lira_pad.c): 0 state, 1 toggle, 2 choose a control, 3 forget one, 4 forget all, 5 its controller + 1, 6 the control waiting */

int lira_ui_env_text(int c, char *b);       /* an ENV control's value as shown ("3MS", "0.6S", "X1.0"); 0 if c is not one */
int lira_ui_vel(int k);
void lira_ui_tapvel(int k, int v);           /* a plate touched at a height (0 bottom .. 1000 top): voice k's velocity for TAP */

#endif
