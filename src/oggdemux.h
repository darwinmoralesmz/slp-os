// Lectura de páginas y paquetes Ogg (RFC 3533), compartida por las etiquetas (meta.h) y el decodificador (ogg.c).
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

typedef struct {
	FILE *f;
	unsigned char seg[255], *body;
	int nseg, seg_i, body_len, body_pos, cont;   // cont: la página continúa un paquete de la anterior
	int64_t granule;
	long page_off;   // posición de la página actual en el archivo
} OgR;

static void og_init(OgR *o, FILE *f) { memset(o, 0, sizeof *o); o->f = f; o->body = (unsigned char *)malloc(65536); }
static void og_free(OgR *o) { free(o->body); o->body = NULL; }

// Lee la cabecera de la página que empieza en la posición actual; con body = 1 también el cuerpo. Devuelve 0 si lo logra.
static int og_page_at(OgR *o, int body) {
	unsigned char h[27];
	o->page_off = ftell(o->f);
	if (fread(h, 1, 27, o->f) != 27 || memcmp(h, "OggS", 4) || h[4]) return -1;
	o->nseg = h[26];
	if (fread(o->seg, 1, o->nseg, o->f) != (size_t)o->nseg) return -1;
	o->body_len = 0;
	for (int i = 0; i < o->nseg; i++) o->body_len += o->seg[i];
	o->cont = h[5] & 1;
	o->granule = 0;
	for (int i = 7; i >= 0; i--) o->granule = o->granule << 8 | h[6 + i];
	o->seg_i = o->body_pos = 0;
	if (body) { if (fread(o->body, 1, o->body_len, o->f) != (size_t)o->body_len) return -1; }
	else fseek(o->f, o->body_len, SEEK_CUR);
	return 0;
}

// Siguiente paquete completo (se descartan los que no caben en cap). Devuelve su longitud o -1 al acabarse el archivo.
static int og_packet(OgR *o, unsigned char *buf, int cap) {
	int len = 0, drop = 0;
	for (;;) {
		if (o->seg_i >= o->nseg) {
			if (og_page_at(o, 1)) return -1;
			continue;
		}
		int s = o->seg[o->seg_i++];
		if (len + s <= cap) memcpy(buf + len, o->body + o->body_pos, s); else drop = 1;
		len += s; o->body_pos += s;
		if (s < 255) return drop ? 0 : len;
	}
}

// Granule de la última página del archivo (busca "OggS" en los últimos 64 KB); -1 si no hay.
static int64_t og_last_granule(FILE *f, long fsize) {
	unsigned char b[65536];
	long from = fsize > (long)sizeof b ? fsize - sizeof b : 0;
	if (fseek(f, from, SEEK_SET)) return -1;
	size_t n = fread(b, 1, sizeof b, f);
	for (long i = (long)n - 27; i >= 0; i--) {
		if (memcmp(b + i, "OggS", 4) || b[i + 4]) continue;
		int64_t g = 0;
		for (int k = 7; k >= 0; k--) g = g << 8 | b[i + 6 + k];
		return g;
	}
	return -1;
}
