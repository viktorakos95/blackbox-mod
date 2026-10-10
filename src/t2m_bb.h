/* Trigger2MIDI on the Blackbox: the two inputs' detectors and their settings (src/t2m_bb.c). */
#pragma once
#include <stdint.h>

#include "t2m.h"

#define T2M_INPUTS   2
#define T2M_PORT_TRS 1
#define T2M_PORT_USB 2

struct t2m_mem {
    uint8_t on[T2M_INPUTS];          /* input L, R */
    uint8_t port[T2M_INPUTS];        /* T2M_PORT_TRS | T2M_PORT_USB */
    uint8_t last_vel[T2M_INPUTS];    /* for the page: last hit's velocity / CC value */
    uint32_t hits[T2M_INPUTS];       /* for the page: hits so far */
    float peak[T2M_INPUTS];          /* for the page: input level after the gain, decaying */
    t2m_params p[T2M_INPUTS];
    t2m_state st[T2M_INPUTS];
};

void t2m_boot(void);
struct t2m_mem *t2m_mem(void);       /* 0 if there was no memory for it this boot */
void t2m_audio(const float *l, const float *r, int frames);
