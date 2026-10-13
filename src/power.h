// Política de energía sin hardware: cuándo apagar la pantalla y cuándo iniciar el apagado automático.
// El apagado automático solo corre sin reproducir; su límite depende de si hay enlace Bluetooth. Tocar, pulsar teclas,
// reproducir o cambiar el estado del enlace reinicia la cuenta.
#pragma once
#include <stdint.h>

typedef struct { uint32_t quiet_since; int last_link; } PwState;
typedef struct { uint32_t now, idle; int playing, link, scr_off, scr_to_s, off_disc_min, off_conn_min; } PwIn;
enum { PW_SCREEN_OFF = 1, PW_COUNTDOWN = 2 };

static int pw_decide(PwState *s, const PwIn *in) {
	int r = 0;
	if (in->playing || in->idle < 1000 || in->link != s->last_link) s->quiet_since = in->now;
	s->last_link = in->link;
	int lim = in->link ? in->off_conn_min : in->off_disc_min;
	if (lim && !in->playing && in->now - s->quiet_since > (uint32_t)lim * 60000) r |= PW_COUNTDOWN;
	if (!in->scr_off && in->scr_to_s && in->idle > (uint32_t)in->scr_to_s * 1000) r |= PW_SCREEN_OFF;
	return r;
}
