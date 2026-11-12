#include "thermal_palette.h"

typedef struct { float pos; uint8_t r , g , b;} stop_stage;

static const stop_stage iron_bow[] = {
	{0.00f,   0,   0,   0}, {0.20f,  40,   0, 110}, {0.40f, 160,  20, 130},
	{0.55f, 220,  50,  60}, {0.75f, 250, 150,  20}, {0.90f, 255, 230,  80},
	{1.00f, 255, 255, 255}
};

static const stop_stage blue_red_ori[] = {
		{0.00f, 0, 0, 255}, {1.0f, 255, 0, 0}
};

#define N(a) ((int)(sizeof(a)/sizeof(a[0])))

void palette_init(uint8_t lut[256][3], palette_t p){
	int n;
	const stop_stage *s;

	switch(p){
	case p_blue_red_ori:
		s = blue_red_ori; n = N(blue_red_ori); break;
	case p_iron_bow:
		s = iron_bow; n = N(iron_bow); break;
	}

	//mapped using segments

	int segment = 0;
	for(int i = 0; i < 256; i++){
		float fraction = (float) i / 256.0f;
		while(segment < n-2 && fraction > s[segment + 1].pos){
			segment++;
		}
		float span = s[segment + 1].pos - s[segment].pos;
		float subset = (fraction-s[segment].pos)/span;
		if (subset<0.0f){
			subset = 0.0f;
		}
		else if(subset>1.0f){
			subset = 1.0f;
		}
		lut[i][0] = (uint8_t)(s[segment].b + (s[segment + 1].b - s[segment].b)*subset + 0.5f);
		lut[i][1] = (uint8_t)(s[segment].g + (s[segment + 1].g - s[segment].g)*subset + 0.5f);
		lut[i][2] = (uint8_t)(s[segment].r + (s[segment + 1].r - s[segment].r)*subset + 0.5f);
	}
}
