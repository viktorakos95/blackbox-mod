/* munchi_port.h -- what the Blackbox port changes under Munchi Delay's sources (src/munchi/tempo, MIT, see LICENSE):
 * the delay buffers hold 16-bit samples (12 dB of headroom above full scale), and there is no C library, so random
 * numbers and sines come from here. */
#pragma once
#include <stdint.h>

typedef int16_t mbuf_t;
#define MBUF_SCALE 8192.f

static inline float mbuf_load(mbuf_t v)
{
    return (float)v * (1.f / MBUF_SCALE);
}

static inline mbuf_t mbuf_store(float x)
{
    x *= MBUF_SCALE;
    x = x < -32768.f ? -32768.f : x > 32767.f ? 32767.f : x;
    return (mbuf_t)(int)x;
}

#define MUNCHI_SPREAD (*(volatile float *)0x38800b18u)   /* Width: scale of TEMPO's stereo read offsets (backup SRAM) */

#define MUNCHI_RAND_MAX 0x7fffffff
#define MUNCHI_RNG (*(volatile uint32_t *)0x38800b10u)     /* backup SRAM */

static inline uint32_t munchi_rand(void)
{
    uint32_t x = MUNCHI_RNG;
    x = x * 1103515245u + 12345u;
    MUNCHI_RNG = x;
    return (x >> 1) & MUNCHI_RAND_MAX;
}

/* sin on [-pi, pi] after wrapping; ~1e-3 absolute error, plenty for crossfades and an LFO */
static inline float munchi_sinf(float x)
{
    const float pi = 3.14159265f, tau = 6.28318531f;
    if (x > pi || x < -pi) {
        float k = x * (1.f / tau);
        int i = (int)(k + (k >= 0.f ? .5f : -.5f));
        x -= (float)i * tau;
    }
    float y = 1.27323954f * x - .405284735f * x * (x < 0.f ? -x : x);
    return .225f * (y * (y < 0.f ? -y : y) - y) + y;
}

static inline float munchi_cosf(float x)
{
    return munchi_sinf(x + 1.57079633f);
}
