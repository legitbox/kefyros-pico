/* sdk/lib/kapi_printf.c — libkapi's compact snprintf/vsnprintf.
 *
 * Integers/strings/chars are formatted locally; %f/%g/%e (and %F/%G/%E) are handed to the
 * kernel's float formatter (k->math->fmt_double) so we don't bundle newlib's float printf.
 * Supports flags - + space 0, width (incl '*'), precision (incl '.*'), length h/hh/l/ll/z. */
#include "kapi_rt.h"
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

typedef struct { char *buf; size_t cap; size_t len; } ob;

static void oc(ob *o, char c){ if(o->len < o->cap) o->buf[o->len] = c; o->len++; }
static void os(ob *o, const char *s, int n){ for(int i = 0; i < n; i++) oc(o, s[i]); }

static void pad_emit(ob *o, const char *s, int n, int width, int left, char pad){
    int gap = width - n;
    if(!left) for(int i = 0; i < gap; i++) oc(o, pad);
    os(o, s, n);
    if(left) for(int i = 0; i < gap; i++) oc(o, ' ');
}

static int u_to_str(char *out, unsigned long long v, int base, int upper){
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24]; int n = 0;
    if(v == 0) tmp[n++] = '0';
    while(v){ tmp[n++] = dig[v % base]; v /= base; }
    for(int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap){
    ob o = { buf, cap, 0 };
    for(const char *p = fmt; *p; p++){
        if(*p != '%'){ oc(&o, *p); continue; }
        p++;
        int left = 0, plus = 0, space = 0, zero = 0;
        for(;;){
            if(*p == '-') left = 1; else if(*p == '+') plus = 1;
            else if(*p == ' ') space = 1; else if(*p == '0') zero = 1;
            else if(*p == '#') ; else break;
            p++;
        }
        int width = 0;
        if(*p == '*'){ width = va_arg(ap, int); p++; if(width < 0){ left = 1; width = -width; } }
        else while(*p >= '0' && *p <= '9'){ width = width * 10 + (*p - '0'); p++; }
        int prec = -1;
        if(*p == '.'){ p++; prec = 0;
            if(*p == '*'){ prec = va_arg(ap, int); p++; if(prec < 0) prec = -1; }
            else while(*p >= '0' && *p <= '9'){ prec = prec * 10 + (*p - '0'); p++; } }
        int lng = 0;                          /* 0 int, 1 long, 2 long long, 3 size_t */
        if(*p == 'h'){ p++; if(*p == 'h') p++; }
        else if(*p == 'l'){ p++; if(*p == 'l'){ p++; lng = 2; } else lng = 1; }
        else if(*p == 'z'){ p++; lng = 3; }
        char c = *p;
        char num[34];
        switch(c){
        case 'd': case 'i': {
            long long v = lng >= 2 ? va_arg(ap, long long)
                        : (lng == 1 || lng == 3) ? va_arg(ap, long) : va_arg(ap, int);
            int neg = v < 0;
            unsigned long long u = neg ? -(unsigned long long)v : (unsigned long long)v;
            int n = u_to_str(num, u, 10, 0);
            char sign = neg ? '-' : plus ? '+' : space ? ' ' : 0;
            char tmp[40]; int tn = 0;
            if(sign && !(zero && prec < 0)) tmp[tn++] = sign;   /* sign before zero-pad handled below */
            int zpad = prec > n ? prec - n : 0;
            for(int i = 0; i < zpad; i++) tmp[tn++] = '0';
            for(int i = 0; i < n; i++) tmp[tn++] = num[i];
            if(zero && !left && prec < 0){
                if(sign) oc(&o, sign);
                int gap = width - tn - (sign ? 1 : 0);
                for(int i = 0; i < gap; i++) oc(&o, '0');
                os(&o, tmp, tn);
            } else pad_emit(&o, tmp, tn, width, left, ' ');
            break; }
        case 'u': case 'x': case 'X': case 'o': {
            unsigned long long v = lng >= 2 ? va_arg(ap, unsigned long long)
                        : (lng == 1 || lng == 3) ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
            int base = c == 'o' ? 8 : (c == 'x' || c == 'X') ? 16 : 10;
            int n = u_to_str(num, v, base, c == 'X');
            int zpad = prec > n ? prec - n : 0;
            char tmp[40]; int tn = 0;
            for(int i = 0; i < zpad; i++) tmp[tn++] = '0';
            for(int i = 0; i < n; i++) tmp[tn++] = num[i];
            char pad = (zero && !left && prec < 0) ? '0' : ' ';
            pad_emit(&o, tmp, tn, width, left, pad);
            break; }
        case 'c': { char ch = (char)va_arg(ap, int); pad_emit(&o, &ch, 1, width, left, ' '); break; }
        case 's': { const char *s = va_arg(ap, const char*); if(!s) s = "(null)";
            int n = 0; while(s[n] && (prec < 0 || n < prec)) n++;
            pad_emit(&o, s, n, width, left, ' '); break; }
        case 'p': { void *pp = va_arg(ap, void*);
            num[0] = '0'; num[1] = 'x';
            int n = u_to_str(num + 2, (unsigned long long)(uintptr_t)pp, 16, 0);
            pad_emit(&o, num, n + 2, width, left, ' '); break; }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
            double v = va_arg(ap, double);
            char low = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
            char fs[48];
            int n = g_k->math->fmt_double(fs, sizeof fs, v, prec, low);
            if(n < 0) n = 0;
            if(n > (int)sizeof fs - 1) n = sizeof fs - 1;
            if(c >= 'A' && c <= 'Z') for(int i = 0; i < n; i++) if(fs[i] >= 'a' && fs[i] <= 'z') fs[i] -= 32;
            char pad = (zero && !left) ? '0' : ' ';
            if(pad == '0' && n > 0 && fs[0] == '-'){       /* keep '-' ahead of zero pad */
                oc(&o, '-'); int gap = width - n; for(int i = 0; i < gap; i++) oc(&o, '0');
                os(&o, fs + 1, n - 1);
            } else pad_emit(&o, fs, n, width, left, pad);
            break; }
        case '%': oc(&o, '%'); break;
        case 0:   p--; break;                              /* trailing '%' */
        default:  oc(&o, '%'); oc(&o, c); break;
        }
    }
    if(o.cap) o.buf[o.len < o.cap ? o.len : o.cap - 1] = 0;
    return (int)o.len;
}

int snprintf(char *buf, size_t cap, const char *fmt, ...){
    va_list ap; va_start(ap, fmt); int r = vsnprintf(buf, cap, fmt, ap); va_end(ap); return r;
}
int sprintf(char *buf, const char *fmt, ...){
    va_list ap; va_start(ap, fmt); int r = vsnprintf(buf, (size_t)-1, fmt, ap); va_end(ap); return r;
}
int vsprintf(char *buf, const char *fmt, va_list ap){ return vsnprintf(buf, (size_t)-1, fmt, ap); }
