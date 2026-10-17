// Force-included in every host TU: GNU extensions MSVC lacks, and stdout/stderr redirected so
// devconsole.c can install its own stream (picolibc FDEV_SETUP_STREAM) without touching the CRT.
#pragma once
#define _CRT_SECURE_NO_WARNINGS 1
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define __attribute__(x)

size_t strlcpy(char *dst, const char *src, size_t size);
size_t strlcat(char *dst, const char *src, size_t size);

// picolibc custom streams: the device stream is a dummy here; tests feed out_putc() directly
#define FDEV_SETUP_STREAM(put, get, flush, flags) { 0 }
#define _FDEV_SETUP_WRITE 0
extern FILE *g_host_stdout, *g_host_stderr;
#undef stdout
#undef stderr
#define stdout g_host_stdout
#define stderr g_host_stderr
int host_fflush(FILE *f);
#define fflush(f) host_fflush(f)
