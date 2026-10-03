#pragma once
#include <stddef.h>
#define RAND_MAX 0x7fffffff
#ifdef __cplusplus
extern "C" {
#endif
int rand(void);
int abs(int);
#ifdef __cplusplus
}
#endif
