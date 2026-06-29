/* sdk/include/stdio.h — KAPI freestanding <stdio.h>: just the buffer-formatting calls.
 * Implemented by libkapi's compact printf (kapi_printf.c); %f/%g/%e route to the kernel's
 * float formatter via k->math->fmt_double. No FILE* / stdout here (use k->sys->log). */
#ifndef KAPI_STDIO_H
#define KAPI_STDIO_H

#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

int snprintf(char *buf, size_t cap, const char *fmt, ...);
int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);
int sprintf(char *buf, const char *fmt, ...);
int vsprintf(char *buf, const char *fmt, va_list ap);

#ifdef __cplusplus
}
#endif
#endif /* KAPI_STDIO_H */
