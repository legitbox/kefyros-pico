// apps/calc_geom.h — PicoGeo dynamic geometry engine header.
// Interactive 2D coordinate geometry: vertices, lines, circles, midpoints,
// direct manipulation (grab & drag), snapping cursor, and measurements.
#ifndef KF_CALC_GEOM_H
#define KF_CALC_GEOM_H
#include <stdint.h>

void calc_geom_open(void);
void calc_geom_key(uint8_t key, int mods, int pressed);
int  calc_geom_tick(void);

#endif
