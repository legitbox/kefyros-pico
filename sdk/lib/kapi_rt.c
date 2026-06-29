/* sdk/lib/kapi_rt.c — libkapi core: the runtime hook, heap, and freestanding mem/str funcs.
 *
 * Built with -ffreestanding -fno-builtin so the compiler does NOT turn these loops back
 * into self-calls. malloc forwards to the kernel arena heap (k->mem); the mem and str funcs
 * are plain C and also back the implicit calls the compiler emits for struct copies, etc.
 * ARM EABI aliases (__aeabi_memcpy/memset/...) are provided too — codegen emits those. */
#include "kapi_rt.h"
#include <stddef.h>
#include <stdint.h>

const kapi *g_k = 0;
void kapi_rt_init(const kapi *k){ g_k = k; }

/* ===================== heap (forward to the kernel arena) ===================== */
void *malloc(size_t n){ return g_k->mem->alloc(n); }
void  free(void *p){ g_k->mem->free(p); }
void *realloc(void *p, size_t n){ return g_k->mem->realloc(p, n); }
void *calloc(size_t nmemb, size_t size){
    size_t n = nmemb * size;
    if(size && n / size != nmemb) return 0;        /* overflow */
    void *p = g_k->mem->alloc(n);
    if(p){ unsigned char *b = p; for(size_t i = 0; i < n; i++) b[i] = 0; }
    return p;
}

/* ===================== mem* ===================== */
void *memcpy(void *dst, const void *src, size_t n){
    unsigned char *d = dst; const unsigned char *s = src;
    while(n--) *d++ = *s++;
    return dst;
}
void *memmove(void *dst, const void *src, size_t n){
    unsigned char *d = dst; const unsigned char *s = src;
    if(d == s || n == 0) return dst;
    if(d < s){ while(n--) *d++ = *s++; }
    else { d += n; s += n; while(n--) *--d = *--s; }
    return dst;
}
void *memset(void *dst, int c, size_t n){
    unsigned char *d = dst; while(n--) *d++ = (unsigned char)c; return dst;
}
int memcmp(const void *a, const void *b, size_t n){
    const unsigned char *x = a, *y = b;
    for(; n; n--, x++, y++) if(*x != *y) return *x - *y;
    return 0;
}
void *memchr(const void *s, int c, size_t n){
    const unsigned char *p = s;
    for(; n; n--, p++) if(*p == (unsigned char)c) return (void*)p;
    return 0;
}

/* ===================== str* ===================== */
size_t strlen(const char *s){ const char *p = s; while(*p) p++; return (size_t)(p - s); }
int strcmp(const char *a, const char *b){
    while(*a && *a == *b){ a++; b++; } return (unsigned char)*a - (unsigned char)*b;
}
int strncmp(const char *a, const char *b, size_t n){
    for(; n; n--, a++, b++){ if(*a != *b) return (unsigned char)*a - (unsigned char)*b; if(!*a) break; }
    return 0;
}
char *strcpy(char *dst, const char *src){ char *d = dst; while((*d++ = *src++)); return dst; }
char *strncpy(char *dst, const char *src, size_t n){
    char *d = dst; size_t i = 0;
    for(; i < n && src[i]; i++) d[i] = src[i];
    for(; i < n; i++) d[i] = 0;
    return dst;
}
char *strcat(char *dst, const char *src){ char *d = dst; while(*d) d++; while((*d++ = *src++)); return dst; }
char *strncat(char *dst, const char *src, size_t n){
    char *d = dst; while(*d) d++;
    while(n-- && *src) *d++ = *src++;
    *d = 0; return dst;
}
char *strchr(const char *s, int c){
    for(;; s++){ if(*s == (char)c) return (char*)s; if(!*s) return 0; }
}
char *strrchr(const char *s, int c){
    const char *last = 0; for(;; s++){ if(*s == (char)c) last = s; if(!*s) return (char*)last; }
}
char *strstr(const char *hay, const char *needle){
    if(!*needle) return (char*)hay;
    for(; *hay; hay++){
        const char *h = hay, *n = needle;
        while(*h && *n && *h == *n){ h++; n++; }
        if(!*n) return (char*)hay;
    }
    return 0;
}

/* ===================== numeric conversions ===================== */
int  abs(int v){ return v < 0 ? -v : v; }
long labs(long v){ return v < 0 ? -v : v; }

double strtod(const char *s, char **end){ return g_k->math->parse_double(s, end); }
double atof(const char *s){ return g_k->math->parse_double(s, 0); }

long strtol(const char *s, char **end, int base){
    const char *p = s; while(*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
    int neg = 0; if(*p == '+' || *p == '-'){ neg = (*p == '-'); p++; }
    if((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')){ p += 2; base = 16; }
    else if(base == 0 && p[0] == '0'){ base = 8; }
    else if(base == 0){ base = 10; }
    long acc = 0;
    for(;; p++){
        int c = *p, d;
        if(c >= '0' && c <= '9') d = c - '0';
        else if(c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else if(c >= 'A' && c <= 'Z') d = c - 'A' + 10;
        else break;
        if(d >= base) break;
        acc = acc * base + d;
    }
    if(end) *end = (char*)p;
    return neg ? -acc : acc;
}
unsigned long strtoul(const char *s, char **end, int base){ return (unsigned long)strtol(s, end, base); }
int  atoi(const char *s){ return (int)strtol(s, 0, 10); }
long atol(const char *s){ return strtol(s, 0, 10); }

/* ===================== ARM EABI mem aliases (codegen emits these) ===================== */
void *__aeabi_memcpy(void *d, const void *s, size_t n){ return memcpy(d, s, n); }
void *__aeabi_memcpy4(void *d, const void *s, size_t n){ return memcpy(d, s, n); }
void *__aeabi_memcpy8(void *d, const void *s, size_t n){ return memcpy(d, s, n); }
void *__aeabi_memmove(void *d, const void *s, size_t n){ return memmove(d, s, n); }
void *__aeabi_memmove4(void *d, const void *s, size_t n){ return memmove(d, s, n); }
void *__aeabi_memmove8(void *d, const void *s, size_t n){ return memmove(d, s, n); }
/* note EABI arg order: (dest, n, c) for memset; memclr is zero-fill */
void  __aeabi_memset(void *d, size_t n, int c){ memset(d, c, n); }
void  __aeabi_memset4(void *d, size_t n, int c){ memset(d, c, n); }
void  __aeabi_memset8(void *d, size_t n, int c){ memset(d, c, n); }
void  __aeabi_memclr(void *d, size_t n){ memset(d, 0, n); }
void  __aeabi_memclr4(void *d, size_t n){ memset(d, 0, n); }
void  __aeabi_memclr8(void *d, size_t n){ memset(d, 0, n); }
