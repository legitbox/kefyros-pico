/* sdk/include/kapi_rt.h — the KAPI standard-library runtime hook.
 *
 * libkapi (malloc/printf/math/string) forwards through the kapi vtable, so it needs the
 * `kapi*` the kernel handed app_main(). An app MUST call kapi_rt_init(k) as the FIRST
 * statement of app_main() — before any malloc/snprintf/sin/strtod — to arm it. */
#ifndef KAPI_RT_H
#define KAPI_RT_H

#include "kapi.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const kapi *g_k;            /* the live vtable; valid after kapi_rt_init() */
void kapi_rt_init(const kapi *k);  /* arm the stdlib; call first in app_main()    */

#ifdef __cplusplus
}
#endif
#endif /* KAPI_RT_H */
