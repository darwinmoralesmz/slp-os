// Prueba del ecualizador: Plano no cambia nada; Graves sube 60 Hz respecto a 5 kHz unos 6 dB.
#include <assert.h>
#include <stdio.h>
#include "eq.h"

static double gain_db(int preset, double f) {
	static int32_t s[44100];
	Eq e;
	eq_set(&e, preset, 44100);
	for (int i = 0; i < 44100; i++) s[i] = (int32_t)(0.25 * 2147483647.0 * sin(2 * M_PI * f * i / 44100));
	eq_run(&e, s, 44100, 1);
	double p = 0;
	for (int i = 22050; i < 44100; i++) p = fmax(p, fabs((double)s[i]));
	return 20 * log10(p / (0.25 * 2147483647.0));
}

int main(void) {
	assert(fabs(gain_db(0, 1000)) < 0.01);
	double d = gain_db(1, 60) - gain_db(1, 5000);
	assert(d > 5 && d < 6.5);
	assert(gain_db(1, 60) < 0.1);   // la preamplificación evita saturar
	printf("eq ok (graves +%.1f dB)\n", d);
	return 0;
}
