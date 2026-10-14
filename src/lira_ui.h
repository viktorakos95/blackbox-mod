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
void lira_page_run(float *l, float *r, int n);

#endif
