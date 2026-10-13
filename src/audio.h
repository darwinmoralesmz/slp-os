// Decodificación (FLAC con dr_flac, WAV con dr_wav, MP3 con minimp3, M4A/ALAC en aac.c, Ogg Vorbis y Opus en ogg.c) y etapa de salida al DAC: S16/S24, mono a estéreo y sobremuestreo x2/x4.
#pragma once
#define DR_FLAC_IMPLEMENTATION
#include "../third_party/dr_flac.h"
#define DR_WAV_IMPLEMENTATION
#include "../third_party/dr_wav.h"
#define MINIMP3_IMPLEMENTATION
#include "../third_party/minimp3_ex.h"
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <strings.h>
#include <stdio.h>
#include "aac.h"
#include "ogg.h"
#include "pcm.h"
#include "resample.h"

typedef struct { drflac *fl; drwav *wv; mp3dec_ex_t mp3; Aac *aac; Ogg *og; unsigned rate, ch; unsigned long long total; int bits; } Dec;

static void dec_close(Dec *d);

// Abre el archivo y valida lo que luego se usa como divisor o índice; ante cualquier duda (archivo dañado) devuelve -1 sin dejar nada abierto.
static int dec_open(Dec *d, const char *p) {
	const char *ext = strrchr(p, '.');
	memset(d, 0, sizeof *d);
	if (ext && !strcasecmp(ext, ".flac")) {
		if (!(d->fl = drflac_open_file(p, 0))) return -1;
		d->rate = d->fl->sampleRate; d->ch = d->fl->channels; d->total = d->fl->totalPCMFrameCount;
		d->bits = d->fl->bitsPerSample > 16 ? 24 : 16;
	} else if (ext && (!strcasecmp(ext, ".m4a") || !strcasecmp(ext, ".mp4"))) {
		if (!(d->aac = aac_open(p, &d->rate, &d->ch, &d->total))) return -1;
		d->bits = 16;
	} else if (ext && (!strcasecmp(ext, ".ogg") || !strcasecmp(ext, ".oga") || !strcasecmp(ext, ".opus"))) {
		if (!(d->og = ogg_open(p, &d->rate, &d->ch, &d->total))) return -1;
		d->bits = 16;
	} else if (ext && !strcasecmp(ext, ".wav")) {
		if (!(d->wv = malloc(sizeof *d->wv))) return -1;
		if (!drwav_init_file(d->wv, p, NULL)) { free(d->wv); d->wv = NULL; return -1; }
		d->rate = d->wv->sampleRate; d->ch = d->wv->channels; d->total = d->wv->totalPCMFrameCount;
		d->bits = d->wv->bitsPerSample > 16 ? 24 : 16;
	} else if (ext && !strcasecmp(ext, ".mp3")) {
		if (mp3dec_ex_open(&d->mp3, p, MP3D_SEEK_TO_SAMPLE)) return -1;
		d->rate = d->mp3.info.hz; d->ch = d->mp3.info.channels; d->bits = 16;
		if (d->rate >= 1 && d->ch >= 1) d->total = d->mp3.samples / d->ch;
	} else return -1;
	if (d->rate >= 1 && d->ch >= 1 && d->ch <= 2) return 0;
	dec_close(d);
	return -1;
}

// Lee hasta n frames como enteros de 32 bit alineados a la izquierda; devuelve los frames leídos.
static size_t dec_read(Dec *d, int32_t *buf, size_t n) {
	if (d->fl) return drflac_read_pcm_frames_s32(d->fl, n, buf);
	if (d->aac) return aac_read(d->aac, buf, n);
	if (d->og) return ogg_read(d->og, buf, n);
	if (d->wv) return drwav_read_pcm_frames_s32(d->wv, n, buf);
	short tmp[PCM_PERIOD * 2];
	size_t got = mp3dec_ex_read(&d->mp3, tmp, n * d->ch);
	for (size_t i = 0; i < got; i++) buf[i] = (int32_t)tmp[i] * 65536;
	return got / d->ch;
}

// Salta al frame indicado; devuelve 0 si lo logra.
static int dec_seek(Dec *d, unsigned long long frame) {
	if (d->fl) return drflac_seek_to_pcm_frame(d->fl, frame) ? 0 : -1;
	if (d->aac) return aac_seek(d->aac, frame);
	if (d->og) return ogg_seek(d->og, frame);
	if (d->wv) return drwav_seek_to_pcm_frame(d->wv, frame) ? 0 : -1;
	return mp3dec_ex_seek(&d->mp3, frame * d->ch);
}

static void dec_close(Dec *d) {
	if (d->fl) drflac_close(d->fl);
	else if (d->aac) aac_close(d->aac);
	else if (d->og) ogg_close(d->og);
	else if (d->wv) { drwav_uninit(d->wv); free(d->wv); }
	else mp3dec_ex_close(&d->mp3);
}

static int out_bt_rate = 48000;   // frecuencia del PCM A2DP de bluealsa (se lee al conectar)

// bluealsa 4.1.1 no reinicia su reloj de ritmo cuando se queda sin datos (solo al abrir la PCM): tras un hueco envía lo
// "atrasado" más rápido que el tiempo real y el auricular suena acelerado. Este hilo rellena con silencio la tubería
// cuando baja de BT_PAD_MIN bytes (~85 ms) y nadie más está escribiendo, para que bluealsa nunca se quede vacío.
#define BT_PAD_MIN 16384
static pthread_mutex_t bt_wlock = PTHREAD_MUTEX_INITIALIZER;
static int bt_pad_fd = -1;
static volatile unsigned bt_padded;   // frames de silencio insertados
static void *bt_pad_thread(void *arg) {
	static const int16_t z[512 * 2] = {0};
	for (;;) {
		usleep(5000);
		if (pthread_mutex_trylock(&bt_wlock)) continue;
		int q;
		while (bt_pad_fd >= 0 && !ioctl(bt_pad_fd, FIONREAD, &q) && q < BT_PAD_MIN && write(bt_pad_fd, z, sizeof z) == sizeof z) bt_padded += 512;
		pthread_mutex_unlock(&bt_wlock);
	}
	return NULL;
}
static void bt_pad_set(int fd) { pthread_mutex_lock(&bt_wlock); bt_pad_fd = fd; pthread_mutex_unlock(&bt_wlock); }
typedef struct { int fd, up, bits24, bt, rs_in, rs_out; unsigned ch; int32_t prev[2]; FILE *fp; Rs rs; } Out;

static int out_open(Out *o, const Dec *d) {
	memset(o, 0, sizeof *o);
	o->ch = d->ch;
	// ponytail: S24_LE al DAC desactivado por defecto (posible cuelgue del kernel); activar con S24=1 tras aislarlo.
	o->bits24 = d->bits > 16 && getenv("S24");
	for (o->up = 1; o->up <= 4; o->up *= 2) {
		o->fd = pcm_open(d->rate * o->up, 2, o->bits24 ? SNDRV_PCM_FORMAT_S24_LE : SNDRV_PCM_FORMAT_S16_LE);
		if (o->fd >= 0) return 0;
	}
	return -1;
}

// Prepara el remuestreador para una pista nueva sobre la misma tubería (reutiliza los coeficientes si la tasa no cambia).
static void out_bt_track(Out *o, const Dec *d) {
	o->ch = d->ch;
	if (o->rs_in == (int)d->rate && o->rs_out == out_bt_rate) { rs_reset(&o->rs); return; }
	if (o->rs_in) rs_free(&o->rs);
	rs_init(&o->rs, d->rate, out_bt_rate);
	o->rs_in = d->rate; o->rs_out = out_bt_rate;
}

// Salida Bluetooth: PCM S16 estéreo a la frecuencia del A2DP (48 kHz por defecto) hacia "bluealsa-cli open RUTA" (sin libasound).
static int out_open_bt(Out *o, const Dec *d, const char *pcm_path) {
	char cmd[300];
	memset(o, 0, sizeof *o);
	o->bt = 1; o->ch = d->ch;
	snprintf(cmd, sizeof cmd, "bluealsa-cli open %s", pcm_path);
	if (!(o->fp = popen(cmd, "w"))) return -1;
	o->fd = fileno(o->fp);
	out_bt_track(o, d);
	bt_pad_set(o->fd);
	return 0;
}

static void out_close(Out *o) {
	if (o->bt) { bt_pad_set(-1); pclose(o->fp); if (o->rs_in) rs_free(&o->rs); }
	else pcm_close(o->fd);
}

// Convierte n frames y los escribe; devuelve underruns recuperados o -1 si falla.
static int out_write(Out *o, const int32_t *in, unsigned n) {
	if (o->bt) {
		static int32_t st[PCM_PERIOD * 2];
		static int16_t ob[RS_MAXOUT * 2];
		for (unsigned i = 0; i < n; i++) { st[2 * i] = in[i * o->ch]; st[2 * i + 1] = o->ch == 2 ? in[i * o->ch + 1] : in[i]; }
		int m = rs_process(&o->rs, st, n, ob);
		const char *p = (const char *)ob;
		size_t left = (size_t)m * 4;
		pthread_mutex_lock(&bt_wlock);
		while (left) {
			ssize_t w = write(o->fd, p, left);
			if (w <= 0) break;
			p += w; left -= w;
		}
		pthread_mutex_unlock(&bt_wlock);
		return left ? -1 : 0;
	}
	static int32_t b32[PCM_PERIOD * 4 * 2];
	int16_t *b16 = (int16_t *)b32;
	unsigned m = 0;
	if (o->up == 1) {   // sin sobremuestreo: solo conversión (evita las divisiones de 64 bit de la interpolación)
		for (unsigned i = 0; i < n; i++) {
			int32_t l = in[i * o->ch], r = o->ch == 2 ? in[i * o->ch + 1] : l;
			if (o->bits24) { b32[i * 2] = l >> 8; b32[i * 2 + 1] = r >> 8; }
			else { b16[i * 2] = l >> 16; b16[i * 2 + 1] = r >> 16; }
		}
		return pcm_write(o->fd, b32, n);
	}
	for (unsigned i = 0; i < n; i++) {
		int32_t l = in[i * o->ch], r = o->ch == 2 ? in[i * o->ch + 1] : l;
		for (int k = 1; k <= o->up; k++) {
			int32_t a = o->prev[0] + (int32_t)(((int64_t)(l - o->prev[0]) * k) / o->up);
			int32_t b = o->prev[1] + (int32_t)(((int64_t)(r - o->prev[1]) * k) / o->up);
			if (o->bits24) { b32[m * 2] = a >> 8; b32[m * 2 + 1] = b >> 8; }
			else { b16[m * 2] = a >> 16; b16[m * 2 + 1] = b >> 16; }
			m++;
		}
		o->prev[0] = l; o->prev[1] = r;
	}
	return pcm_write(o->fd, b32, m);
}
