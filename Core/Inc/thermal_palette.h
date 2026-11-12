#ifndef THERMAL_PALETTE_H
#define THERMAL_PALETTE_H
#include <stdint.h>

typedef enum {
	p_iron_bow = 0,
	p_blue_red_ori
}palette_t;
// lut -> look up table
void palette_init(uint8_t lut[256][3], palette_t p);

#endif
