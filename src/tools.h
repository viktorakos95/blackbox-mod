#ifndef TOOLS_H
#define TOOLS_H
/* General settings of our machines, on the stock Tools pages (src/comp.c registers them, patches/tools.py shows them). */
enum { T_CLKPPQ = 0x1a0, T_CLKLVL, T_MONO, T_DRONEOUT, T_SAMPLROUT, T_FILTERS };
int tools_get(int id);   /* clock out: 0..6 (off, 1, 2, 4, 8, 12, 24 a quarter); level 50..1000 (0 = unregistered: 100 %); mono 0 / 1; outs 0..2; filters 0 = 12 dB, 1 = 24 dB */
#endif
