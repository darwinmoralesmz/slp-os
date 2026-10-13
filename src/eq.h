// Ecualizador por perfiles: hasta tres biquads (RBJ) sobre muestras int32, con preamplificación para no saturar.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

enum { EQ_LOW, EQ_PEAK, EQ_HIGH };
typedef struct { int type; float f, g; } EqBand;
typedef struct { const char *name; EqBand b[3]; } EqPreset;
static const EqPreset EQ_P[] = {
	{"Plano", {{0}}},
	{"Graves", {{EQ_LOW, 100, 6}}},
	{"Agudos", {{EQ_HIGH, 8000, 5}}},
	{"Graves y agudos", {{EQ_LOW, 100, 5}, {EQ_HIGH, 8000, 4}}},
	{"Voces", {{EQ_LOW, 150, -3}, {EQ_PEAK, 2500, 3}}},
	{"Menos graves", {{EQ_LOW, 120, -6}}},
	{"Cálido", {{EQ_HIGH, 6000, -4}}},
};
#define EQ_N ((int)(sizeof EQ_P / sizeof EQ_P[0]))

typedef struct { int preset, nb; unsigned rate; float pre, c[3][5], z[3][2][2]; } Eq;

static void eq_set(Eq *e, int preset, unsigned rate) {
	memset(e, 0, sizeof *e);
	e->preset = preset; e->rate = rate;
	float top = 0;
	for (int i = 0; i < 3 && EQ_P[preset].b[i].g; i++) {
		const EqBand *b = &EQ_P[preset].b[i];
		float A = powf(10, b->g / 40), w = 2 * (float)M_PI * b->f / rate, cs = cosf(w), sn = sinf(w);
		float al = b->type == EQ_PEAK ? sn / 2 : sn / 2 * sqrtf(2), sa = 2 * sqrtf(A) * al;   // peak Q = 1; shelf S = 1
		float b0, b1, b2, a0, a1, a2;
		if (b->type == EQ_PEAK) { b0 = 1 + al * A; b1 = -2 * cs; b2 = 1 - al * A; a0 = 1 + al / A; a1 = -2 * cs; a2 = 1 - al / A; }
		else if (b->type == EQ_LOW) {
			b0 = A * ((A + 1) - (A - 1) * cs + sa); b1 = 2 * A * ((A - 1) - (A + 1) * cs); b2 = A * ((A + 1) - (A - 1) * cs - sa);
			a0 = (A + 1) + (A - 1) * cs + sa; a1 = -2 * ((A - 1) + (A + 1) * cs); a2 = (A + 1) + (A - 1) * cs - sa;
		} else {
			b0 = A * ((A + 1) + (A - 1) * cs + sa); b1 = -2 * A * ((A - 1) + (A + 1) * cs); b2 = A * ((A + 1) + (A - 1) * cs - sa);
			a0 = (A + 1) - (A - 1) * cs + sa; a1 = 2 * ((A - 1) - (A + 1) * cs); a2 = (A + 1) - (A - 1) * cs - sa;
		}
		float *c = e->c[e->nb++];
		c[0] = b0 / a0; c[1] = b1 / a0; c[2] = b2 / a0; c[3] = a1 / a0; c[4] = a2 / a0;
		if (b->g > top) top = b->g;
	}
	e->pre = powf(10, -top / 20);
}

// Filtra n frames de ch canales en el sitio.
static void eq_run(Eq *e, int32_t *s, unsigned n, unsigned ch) {
	if (!e->nb) return;
	for (unsigned i = 0; i < n * ch; i++) {
		unsigned k = i % ch;
		float x = s[i] * e->pre;
		for (int j = 0; j < e->nb; j++) {   // forma directa II transpuesta
			float *c = e->c[j], *z = e->z[j][k & 1];
			float y = c[0] * x + z[0];
			z[0] = c[1] * x - c[3] * y + z[1];
			z[1] = c[2] * x - c[4] * y;
			x = y;
		}
		s[i] = x >= 2147483520.f ? INT32_MAX : x <= -2147483648.f ? INT32_MIN : (int32_t)x;
	}
}
