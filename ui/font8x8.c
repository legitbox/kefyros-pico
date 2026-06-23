// ui/font8x8.c — single definition of the 8x8 bitmap font used by the calc 2D/3D
// graph axis labels (apps/calc_graph*.c reference it as `extern char
// font8x8_basic[128][8]`). On the Linux build terminal.c defined it; the Pico
// build has no terminal, so this TU provides the one definition.
#include "font8x8/font8x8_basic.h"
