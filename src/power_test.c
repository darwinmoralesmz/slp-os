// Pruebas de la política de energía: gcc -I src src/power_test.c -o /tmp/power_test && /tmp/power_test
#include <assert.h>
#include <stdio.h>
#include "power.h"

#define MIN 60000u
static PwState st = {0, -1};
static PwIn in = {0, 0, 0, 0, 0, 30, 10, 30};
// Avanza el reloj sin actividad (idle crece con el tiempo) y evalúa.
static int at(uint32_t now, uint32_t idle_since) { in.now = now; in.idle = now - idle_since; return pw_decide(&st, &in); }

int main(void) {
	// Sin reproducir y sin Bluetooth: cuenta atrás a los 10 min; la pantalla se apaga a los 30 s.
	assert(at(0, 0) == 0);
	assert(at(29000, 0) == 0);
	assert(at(31000, 0) == PW_SCREEN_OFF);
	in.scr_off = 1;
	assert(at(9 * MIN + 59000, 0) == 0);
	assert(at(10 * MIN + 1000, 0) == PW_COUNTDOWN);
	// Tocar la pantalla (idle bajo) reinicia la cuenta.
	assert(at(10 * MIN + 2000, 10 * MIN + 1800) == 0);
	assert(at(20 * MIN, 10 * MIN + 1800) == 0);
	assert(at(20 * MIN + 3000, 10 * MIN + 1800) == PW_COUNTDOWN);
	// Reproduciendo no se apaga nunca, con la pantalla apagada o no.
	in.playing = 1;
	assert(at(10 * 60 * MIN, 0) == 0);
	// Al pausar la cuenta parte de cero.
	in.playing = 0;
	assert(at(10 * 60 * MIN + 1000, 0) == 0);
	assert(at(10 * 60 * MIN + 9 * MIN, 0) == 0);
	assert(at(10 * 60 * MIN + 10 * MIN + 2000, 0) == PW_COUNTDOWN);
	// Con enlace Bluetooth rige el otro límite (30 min) y el cambio de enlace reinicia la cuenta.
	uint32_t t = 20 * 60 * MIN;
	in.link = 1;
	assert(at(t, 0) == 0);
	assert(at(t + 29 * MIN, 0) == 0);
	assert(at(t + 30 * MIN + 1000, 0) == PW_COUNTDOWN);
	in.link = 0;
	assert(at(t + 30 * MIN + 2000, 0) == 0);
	assert(at(t + 40 * MIN + 3000, 0) == PW_COUNTDOWN);
	// Conectar con la pantalla apagada y sin reproducir: no pide encender nada, solo reinicia la cuenta.
	in.link = 1;
	assert(at(t + 45 * MIN, 0) == 0);
	// "Nunca" desactiva cada temporizador.
	in.off_conn_min = 0; in.off_disc_min = 0; in.scr_to_s = 0; in.scr_off = 0;
	assert(at(t + 1000 * MIN, 0) == 0);
	printf("ok\n");
	return 0;
}
