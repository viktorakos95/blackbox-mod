/* Blackbox port: no C library. Declarations only, so anything actually called fails to link instead of pulling libm. */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
float sinf(float), cosf(float), tanf(float), expf(float), logf(float), log10f(float), log2f(float), powf(float, float);
float sqrtf(float), floorf(float), ceilf(float), roundf(float), fmodf(float, float), tanhf(float), atanf(float);
double sin(double), cos(double), exp(double), log(double), log10(double), pow(double, double), sqrt(double);
double floor(double), fabs(double), tanh(double);
long lrintf(float);
float frexpf(float, int *), ldexpf(float, int), exp2f(float), fminf_(float);
#ifdef __cplusplus
}
#endif
#define fabsf(x) __builtin_fabsf(x)
#define fminf(a, b) __builtin_fminf(a, b)
#define fmaxf(a, b) __builtin_fmaxf(a, b)
#define M_PI 3.14159265358979323846
