// Force-included in every host TU: GNU extensions and POSIX bits MSVC lacks.
#pragma once
#define _CRT_SECURE_NO_WARNINGS 1
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define __attribute__(x)
#define __ATOMIC_RELAXED 0
#define __ATOMIC_ACQUIRE 0
#define __ATOMIC_RELEASE 0
#define __ATOMIC_ACQ_REL 0
#define __atomic_add_fetch(p, v, m) ((*(p)) += (v))
#define __atomic_sub_fetch(p, v, m) ((*(p)) -= (v))
#define __atomic_load_n(p, m) (*(p))
#define __atomic_store_n(p, v, m) ((*(p)) = (v))
#define __atomic_compare_exchange_n(p, e, d, w, s, f) \
    ((*(p) == *(e)) ? ((*(p) = (d)), 1) : ((*(e) = *(p)), 0))

size_t strlcpy(char *dst, const char *src, size_t size);
size_t strlcat(char *dst, const char *src, size_t size);
#define strncasecmp _strnicmp
#define strcasecmp _stricmp
int _strnicmp(const char *a, const char *b, size_t n);
int _stricmp(const char *a, const char *b);
struct tm *localtime_r(const time_t *t, struct tm *out);
struct tm *gmtime_r(const time_t *t, struct tm *out);
int setenv(const char *k, const char *v, int overwrite);
#define tzset _tzset
void _tzset(void);
char *strtok_r(char *s, const char *delim, char **save);
#define M_PI 3.14159265358979323846
