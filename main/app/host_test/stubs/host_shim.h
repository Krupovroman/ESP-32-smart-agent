// Force-included in every host TU: GNU extensions and POSIX bits MSVC lacks.
#pragma once
#define _CRT_SECURE_NO_WARNINGS 1
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define __attribute__(x)
size_t strlcpy(char *dst, const char *src, size_t size);
struct tm *localtime_r(const time_t *t, struct tm *out);
int setenv(const char *k, const char *v, int overwrite);
#define tzset _tzset
void _tzset(void);
