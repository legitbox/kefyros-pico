/* sdk/include/stdlib.h — KAPI freestanding <stdlib.h>. malloc family forwards to k->mem;
 * strtod/atof forward to k->math->parse_double; the rest are small local impls (kapi_rt.c). */
#ifndef KAPI_STDLIB_H
#define KAPI_STDLIB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void  *malloc(size_t n);
void  *calloc(size_t nmemb, size_t size);
void  *realloc(void *p, size_t n);
void   free(void *p);

int    abs(int v);
long   labs(long v);

double atof(const char *s);
int    atoi(const char *s);
long   atol(const char *s);
double strtod(const char *s, char **end);
long   strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);

#ifdef __cplusplus
}
#endif
#endif /* KAPI_STDLIB_H */
