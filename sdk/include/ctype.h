/* sdk/include/ctype.h — KAPI <ctype.h>. ASCII-only, all static inline (no libkapi TU). */
#ifndef KAPI_CTYPE_H
#define KAPI_CTYPE_H

static inline int isdigit(int c){ return c >= '0' && c <= '9'; }
static inline int isupper(int c){ return c >= 'A' && c <= 'Z'; }
static inline int islower(int c){ return c >= 'a' && c <= 'z'; }
static inline int isalpha(int c){ return isupper(c) || islower(c); }
static inline int isalnum(int c){ return isalpha(c) || isdigit(c); }
static inline int isspace(int c){ return c == ' ' || (c >= '\t' && c <= '\r'); }
static inline int isxdigit(int c){ return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static inline int ispunct(int c){ return c > ' ' && c < 0x7f && !isalnum(c); }
static inline int iscntrl(int c){ return (unsigned)c < ' ' || c == 0x7f; }
static inline int isprint(int c){ return c >= ' ' && c < 0x7f; }
static inline int isgraph(int c){ return c > ' ' && c < 0x7f; }
static inline int toupper(int c){ return islower(c) ? c - 'a' + 'A' : c; }
static inline int tolower(int c){ return isupper(c) ? c - 'A' + 'a' : c; }

#endif /* KAPI_CTYPE_H */
