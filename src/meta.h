// Etiquetas, duración y formato: FLAC (STREAMINFO + comentarios Vorbis), Ogg Vorbis/Opus, WAV, M4A y MP3 (ID3v2). No carga las portadas embebidas.
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include "audio.h"
#include "oggdemux.h"

typedef struct { char title[128], artist[128], album[128], albumartist[128], fmt[24]; int dur, track, disc, bad, rate; } Meta;   // bad: el reproductor no puede abrirlo (más de 2 canales, > 192 kHz, códec desconocido)

static void meta_copy(char *dst, size_t n, const char *s, size_t len) {
	if (len >= n) len = n - 1;
	memcpy(dst, s, len);
	dst[len] = 0;
}

static void meta_khz(char *out, size_t n, unsigned rate) {
	if (rate % 1000 == 0) snprintf(out, n, "%u", rate / 1000);
	else snprintf(out, n, "%.1f", rate / 1000.0);
}

static uint32_t meta_le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// Deja la cadena como UTF-8 válido sin caracteres de control: bytes inválidos pasan a '?' y controles a espacio.
static void meta_clean(char *s) {
	unsigned char *p = (unsigned char *)s;
	while (*p) {
		if (*p < 0x20 || *p == 0x7f) { *p++ = ' '; continue; }
		int n = *p < 0x80 ? 1 : (*p >> 5) == 6 ? 2 : (*p >> 4) == 14 ? 3 : (*p >> 3) == 30 ? 4 : 0;
		int ok = n > 0;
		for (int i = 1; ok && i < n; i++) ok = (p[i] & 0xC0) == 0x80;
		if (ok && n == 2 && p[0] < 0xC2) ok = 0;
		if (ok) p += n; else *p++ = '?';
	}
}

// Entero no negativo acotado de un texto ("3", "3/12").
static int meta_num(const char *s) { long v = atol(s); return v < 0 ? 0 : v > 100000 ? 100000 : (int)v; }

// Salta una etiqueta ID3v2 al inicio (algunos FLAC la traen) y deja el archivo en el primer byte posterior.
static void meta_skip_id3(FILE *f) {
	unsigned char h[10];
	if (fread(h, 1, 10, f) == 10 && !memcmp(h, "ID3", 3)) fseek(f, 10 + ((h[6] & 127) << 21 | (h[7] & 127) << 14 | (h[8] & 127) << 7 | (h[9] & 127)) + (h[5] & 16 ? 10 : 0), SEEK_SET);
	else fseek(f, 0, SEEK_SET);
}

// Comentarios Vorbis (FLAC, Ogg Vorbis, Opus): longitud del fabricante, cantidad y pares CAMPO=valor.
static void meta_vcomment(const unsigned char *b, uint32_t len, Meta *m) {
	uint32_t vl, pos = 0, count = 0;
	if (len < 8) return;
	vl = meta_le32(b);
	if (vl <= len - 8) { pos = 4 + vl; count = meta_le32(b + pos); pos += 4; }
	for (uint32_t i = 0; i < count && pos + 4 <= len; i++) {
		uint32_t cl = meta_le32(b + pos);
		pos += 4;
		if (cl > len - pos) break;
		const char *c = (const char *)b + pos;
		if (cl > 6 && !strncasecmp(c, "TITLE=", 6)) meta_copy(m->title, sizeof m->title, c + 6, cl - 6);
		else if (cl > 7 && !strncasecmp(c, "ARTIST=", 7)) meta_copy(m->artist, sizeof m->artist, c + 7, cl - 7);
		else if (cl > 6 && !strncasecmp(c, "ALBUM=", 6)) meta_copy(m->album, sizeof m->album, c + 6, cl - 6);
		else if (cl > 12 && !strncasecmp(c, "ALBUMARTIST=", 12)) meta_copy(m->albumartist, sizeof m->albumartist, c + 12, cl - 12);
		else if (cl > 12 && !strncasecmp(c, "TRACKNUMBER=", 12)) { char t[16]; meta_copy(t, sizeof t, c + 12, cl - 12); m->track = meta_num(t); }
		else if (cl > 11 && !strncasecmp(c, "DISCNUMBER=", 11)) { char t[16]; meta_copy(t, sizeof t, c + 11, cl - 11); m->disc = meta_num(t); }
		pos += cl;
	}
}

static void meta_flac(FILE *f, Meta *m) {
	unsigned char h[4];
	meta_skip_id3(f);
	if (fread(h, 1, 4, f) != 4 || memcmp(h, "fLaC", 4)) return;
	for (int last = 0; !last && fread(h, 1, 4, f) == 4;) {
		last = h[0] >> 7;
		int type = h[0] & 0x7f;
		uint32_t len = h[1] << 16 | h[2] << 8 | h[3];
		if (type == 0 && len >= 18) {
			unsigned char b[34];
			if (fread(b, 1, len < 34 ? len : 34, f) < 18) return;
			unsigned rate = b[10] << 12 | b[11] << 4 | b[12] >> 4, bps = ((b[12] & 1) << 4 | b[13] >> 4) + 1;
			unsigned long long total = (unsigned long long)(b[13] & 15) << 32 | (uint32_t)b[14] << 24 | b[15] << 16 | b[16] << 8 | b[17];
			char k[16];
			meta_khz(k, sizeof k, rate);
			snprintf(m->fmt, sizeof m->fmt, "FLAC %u/%s", bps, k);
			m->dur = rate && total / rate < 360000 ? total / rate : 0;
			if (rate < 1 || rate > 192000 || ((b[12] >> 1 & 7) + 1) > 2) m->bad = 1;
			if (len > 34) fseek(f, len - 34, SEEK_CUR);
		} else if (type == 4 && len >= 8 && len < (1u << 20)) {
			unsigned char *b = malloc(len);
			if (!b || fread(b, 1, len, f) != len) { free(b); return; }
			meta_vcomment(b, len, m);
			free(b);
		} else fseek(f, len, SEEK_CUR);
	}
}

// Ogg Vorbis y Opus: cabecera de identificación, comentarios y duración por el granule de la última página.
static void meta_ogg(FILE *f, long fsize, Meta *m) {
	OgR r;
	unsigned char head[64];
	unsigned rate = 0, ch = 0;
	int opus = 0, pre = 0;
	og_init(&r, f);
	int hl = og_packet(&r, head, sizeof head);
	if (hl >= 19 && !memcmp(head, "OpusHead", 8)) { opus = 1; ch = head[9]; rate = 48000; pre = head[10] | head[11] << 8; }
	else if (hl >= 30 && !memcmp(head, "\x01vorbis", 7)) { ch = head[11]; rate = meta_le32(head + 12); }
	else { m->bad = 1; og_free(&r); return; }
	unsigned char *tags = malloc(1 << 19);
	int tl = tags ? og_packet(&r, tags, 1 << 19) : -1;
	int hdr = opus ? 8 : 7;
	if (tl > hdr + 8 && !memcmp(tags, opus ? "OpusTags" : "\x03vorbis", hdr)) meta_vcomment(tags + hdr, tl - hdr, m);
	free(tags);
	og_free(&r);
	int64_t g = og_last_granule(f, fsize);
	if (g > pre && rate) { int64_t d = (g - pre) / rate; m->dur = d < 360000 ? d : 0; }
	if (ch < 1 || ch > 2 || rate < 1 || rate > 192000) m->bad = 1;
	if (m->dur) snprintf(m->fmt, sizeof m->fmt, "%s %ldk", opus ? "Opus" : "Vorbis", fsize * 8 / m->dur / 1000);
	else snprintf(m->fmt, sizeof m->fmt, "%s", opus ? "Opus" : "Vorbis");
}

// WAV con dr_wav: formato, duración por tamaño de datos (acotado al archivo) y etiquetas INFO (INAM, IART, IPRD, ITRK).
static void meta_wav(const char *path, long fsize, Meta *m) {
	drwav w;
	if (!drwav_init_file_with_metadata(&w, path, 0, NULL)) { m->bad = 1; return; }
	for (unsigned i = 0; i < w.metadataCount; i++) {
		drwav_metadata *d = &w.pMetadata[i];
		const char *v = d->data.infoText.pString;
		if (!v) continue;
		size_t l = strnlen(v, d->data.infoText.stringLength + 1);
		if (d->type == drwav_metadata_type_list_info_title) meta_copy(m->title, sizeof m->title, v, l);
		else if (d->type == drwav_metadata_type_list_info_artist) meta_copy(m->artist, sizeof m->artist, v, l);
		else if (d->type == drwav_metadata_type_list_info_album) meta_copy(m->album, sizeof m->album, v, l);
		else if (d->type == drwav_metadata_type_list_info_tracknumber) { char t[16]; meta_copy(t, sizeof t, v, l); m->track = meta_num(t); }
	}
	uint64_t data = w.dataChunkDataSize, brate = w.fmt.avgBytesPerSec;
	if (!data || w.dataChunkDataPos + data > (uint64_t)fsize) data = fsize - w.dataChunkDataPos;
	if (brate && data && data / brate < 360000) m->dur = data / brate;
	char k[16];
	meta_khz(k, sizeof k, w.sampleRate);
	snprintf(m->fmt, sizeof m->fmt, "WAV %u/%s", w.bitsPerSample, k);
	if (w.channels < 1 || w.channels > 2 || w.sampleRate > 192000) m->bad = 1;
	drwav_uninit(&w);
}

// Texto de un frame ID3 (latin1, UTF-16 o UTF-8) a UTF-8.
static void meta_id3_text(char *dst, size_t n, const unsigned char *b, size_t len) {
	if (len < 2) return;
	int enc = b[0];
	const unsigned char *p = b + 1;
	len--;
	size_t o = 0;
	if (enc == 0) {
		for (size_t i = 0; i < len && p[i] && o + 3 < n; i++) {
			if (p[i] < 0x80) dst[o++] = p[i];
			else { dst[o++] = 0xC0 | p[i] >> 6; dst[o++] = 0x80 | (p[i] & 63); }
		}
	} else if (enc == 3) {
		for (size_t i = 0; i < len && p[i] && o + 1 < n; i++) dst[o++] = p[i];
	} else {
		int be = enc == 2;
		if (len >= 2 && p[0] == 0xFF && p[1] == 0xFE) { be = 0; p += 2; len -= 2; }
		else if (len >= 2 && p[0] == 0xFE && p[1] == 0xFF) { be = 1; p += 2; len -= 2; }
		for (size_t i = 0; i + 1 < len && o + 4 < n; i += 2) {
			unsigned c = be ? p[i] << 8 | p[i + 1] : p[i + 1] << 8 | p[i];
			if (!c) break;
			if (c < 0x80) dst[o++] = c;
			else if (c < 0x800) { dst[o++] = 0xC0 | c >> 6; dst[o++] = 0x80 | (c & 63); }
			else { dst[o++] = 0xE0 | c >> 12; dst[o++] = 0x80 | (c >> 6 & 63); dst[o++] = 0x80 | (c & 63); }
		}
	}
	dst[o] = 0;
}

static void meta_mp3(FILE *f, const char *path, long fsize, Meta *m) {
	unsigned char h[10];
	if (fread(h, 1, 10, f) == 10 && !memcmp(h, "ID3", 3)) {
		uint32_t tag = (h[6] & 127) << 21 | (h[7] & 127) << 14 | (h[8] & 127) << 7 | (h[9] & 127);
		uint64_t pos = 0;
		int v2 = h[3] == 2, fl = v2 ? 6 : 10;   // ID3v2.2: marcos de 6 bytes con identificador de 3
		while (pos + fl <= tag) {
			unsigned char fh[10];
			if (fread(fh, 1, fl, f) != (size_t)fl || !fh[0]) break;
			uint32_t sz = v2 ? (uint32_t)fh[3] << 16 | fh[4] << 8 | fh[5]
			             : h[3] == 4 ? (uint32_t)(fh[4] & 127) << 21 | (fh[5] & 127) << 14 | (fh[6] & 127) << 7 | (fh[7] & 127)
			                         : (uint32_t)fh[4] << 24 | fh[5] << 16 | fh[6] << 8 | fh[7];
			pos += fl + (uint64_t)sz;
			const char *id = (const char *)fh;
			char *dst = v2 ? (!memcmp(id, "TT2", 3) ? m->title : !memcmp(id, "TP1", 3) ? m->artist : !memcmp(id, "TAL", 3) ? m->album : !memcmp(id, "TP2", 3) ? m->albumartist : NULL)
			               : (!memcmp(id, "TIT2", 4) ? m->title : !memcmp(id, "TPE1", 4) ? m->artist : !memcmp(id, "TALB", 4) ? m->album : !memcmp(id, "TPE2", 4) ? m->albumartist : NULL);
			int num = v2 ? (!memcmp(id, "TRK", 3) ? 1 : !memcmp(id, "TPA", 3) ? 2 : 0) : (!memcmp(id, "TRCK", 4) ? 1 : !memcmp(id, "TPOS", 4) ? 2 : 0);
			size_t dn = dst == m->title ? sizeof m->title : sizeof m->artist;
			if (num && sz < 64) {
				unsigned char b[64]; char t[64];
				if (fread(b, 1, sz, f) != sz) break;
				meta_id3_text(t, sizeof t, b, sz);
				if (num == 1) m->track = meta_num(t); else m->disc = meta_num(t);
				continue;
			}
			if (dst && sz < 1024) {
				unsigned char b[1024];
				if (fread(b, 1, sz, f) != sz) break;
				meta_id3_text(dst, dn, b, sz);
			} else fseek(f, sz, SEEK_CUR);
		}
	}
	// ID3v1 (últimos 128 bytes) para los archivos antiguos sin ID3v2.
	if (!m->title[0] && fsize >= 128 && !fseek(f, fsize - 128, SEEK_SET)) {
		unsigned char t[128], b[32];
		if (fread(t, 1, 128, f) == 128 && !memcmp(t, "TAG", 3)) {
			for (int k = 0; k < 3; k++) {
				const unsigned char *s = t + 3 + 30 * k;
				int n = 30;
				while (n > 0 && (s[n - 1] == ' ' || !s[n - 1])) n--;
				b[0] = 0; memcpy(b + 1, s, n);
				meta_id3_text(k == 0 ? m->title : k == 1 ? m->artist : m->album, 64, b, n + 1);
			}
			if (!t[125] && t[126]) m->track = t[126];
		}
	}
	mp3dec_ex_t d;
	if (!mp3dec_ex_open(&d, path, 0)) {
		m->dur = d.info.hz && d.info.channels && d.samples / d.info.channels / d.info.hz < 360000 ? d.samples / d.info.channels / d.info.hz : 0;
		if (m->dur) snprintf(m->fmt, sizeof m->fmt, "MP3 %ldk", fsize * 8 / m->dur / 1000);
		else snprintf(m->fmt, sizeof m->fmt, "MP3");
		mp3dec_ex_close(&d);
	}
}

// M4A: recorre las cajas moov/udta/meta/ilst (©nam, ©ART, ©alb, aART, trkn, disk) y mvhd (duración).
static uint32_t meta_be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static void meta_m4a_item(FILE *f, long body, long end, const unsigned char *type, Meta *m, long *cov) {
	unsigned char h[16];
	fseek(f, body, SEEK_SET);
	if (end - body < 16 || fread(h, 1, 16, f) != 16 || memcmp(h + 4, "data", 4)) return;
	uint32_t dsz = meta_be32(h);
	if (!memcmp(type, "covr", 4)) { if (cov && !cov[1] && dsz > 16 && dsz <= (uint32_t)(end - body)) { cov[0] = body + 16; cov[1] = dsz - 16; } return; }   // portada incrustada: posición y tamaño
	long plen = (long)dsz - 16;
	if (plen <= 0 || plen > 400) return;
	unsigned char b[404];
	if (fread(b, 1, plen, f) != (size_t)plen) return;
	b[plen] = 0;
	if (type[0] == 0xA9 && !memcmp(type + 1, "nam", 3)) meta_copy(m->title, sizeof m->title, (char *)b, plen);
	else if (type[0] == 0xA9 && !memcmp(type + 1, "ART", 3)) meta_copy(m->artist, sizeof m->artist, (char *)b, plen);
	else if (type[0] == 0xA9 && !memcmp(type + 1, "alb", 3)) meta_copy(m->album, sizeof m->album, (char *)b, plen);
	else if (!memcmp(type, "aART", 4)) meta_copy(m->albumartist, sizeof m->albumartist, (char *)b, plen);
	else if (!memcmp(type, "trkn", 4) && plen >= 4) m->track = b[2] << 8 | b[3];
	else if (!memcmp(type, "disk", 4) && plen >= 4) m->disc = b[2] << 8 | b[3];
}

static void meta_m4a_walk(FILE *f, long start, long end, int in_ilst, int depth, Meta *m, long *cov) {
	long pos = start;
	while (pos + 8 <= end && depth < 8) {
		unsigned char h[16];
		fseek(f, pos, SEEK_SET);
		if (fread(h, 1, 8, f) != 8) return;
		uint64_t sz = meta_be32(h);
		long hdr = 8;
		if (sz == 1) { if (fread(h + 8, 1, 8, f) != 8) return; sz = (uint64_t)meta_be32(h + 8) << 32 | meta_be32(h + 12); hdr = 16; }
		else if (sz == 0) sz = end - pos;
		if (sz < (uint64_t)hdr) return;
		if (sz > (uint64_t)(end - pos)) sz = end - pos;
		long body = pos + hdr, bend = pos + (long)sz;
		if (bend > end) bend = end;
		if (in_ilst) meta_m4a_item(f, body, bend, h + 4, m, cov);
		else if (!memcmp(h + 4, "moov", 4) || !memcmp(h + 4, "udta", 4)) meta_m4a_walk(f, body, bend, 0, depth + 1, m, cov);
		else if (!memcmp(h + 4, "meta", 4)) meta_m4a_walk(f, body + 4, bend, 0, depth + 1, m, cov);
		else if (!memcmp(h + 4, "ilst", 4)) meta_m4a_walk(f, body, bend, 1, depth + 1, m, cov);
		else if (!memcmp(h + 4, "trak", 4) || !memcmp(h + 4, "mdia", 4) || !memcmp(h + 4, "minf", 4) || !memcmp(h + 4, "stbl", 4)) meta_m4a_walk(f, body, bend, 0, depth + 1, m, cov);
		else if (!memcmp(h + 4, "stsd", 4)) {   // primera entrada de muestras: AAC (mp4a) y ALAC con 1-2 canales (ALAC también 16 o 24 bit)
			unsigned char v[44];
			fseek(f, body, SEEK_SET);
			if (fread(v, 1, 44, f) == 44 && (!memcmp(v + 12, "alac", 4) || !memcmp(v + 12, "mp4a", 4))) {
				unsigned ch = v[32] << 8 | v[33], bits = v[34] << 8 | v[35];
				if (ch < 1 || ch > 2 || (!memcmp(v + 12, "alac", 4) && bits != 16 && bits != 24)) m->bad = 1;
				if (!memcmp(v + 12, "alac", 4)) snprintf(m->fmt, sizeof m->fmt, "ALAC %u", bits);
			}
		} else if (!memcmp(h + 4, "mdhd", 4)) {   // escala de tiempo del medio = frecuencia de muestreo
			unsigned char v[24];
			fseek(f, body, SEEK_SET);
			if (fread(v, 1, 24, f) == 24) { uint32_t ts = v[0] == 1 ? meta_be32(v + 20) : meta_be32(v + 12); if (ts && ts <= 768000) m->rate = ts; }
		} else if (!memcmp(h + 4, "mvhd", 4)) {
			unsigned char v[24];
			fseek(f, body, SEEK_SET);
			if (fread(v, 1, 24, f) == 24) {
				uint32_t ts = v[0] == 1 ? meta_be32(v + 20) : meta_be32(v + 12);
				uint32_t dur = v[0] == 1 ? 0 : meta_be32(v + 16);
				if (ts && dur && dur / ts < 360000) m->dur = dur / ts;
			}
		}
		pos = bend;
	}
}

static void meta_m4a(FILE *f, long fsize, Meta *m) {
	meta_m4a_walk(f, 0, fsize, 0, 0, m, NULL);
	if (m->rate > 192000) m->bad = 1;
	if (!strncmp(m->fmt, "ALAC", 4)) {   // ALAC: profundidad del stsd + frecuencia del mdhd
		char k[16];
		meta_khz(k, sizeof k, m->rate);
		size_t n = strlen(m->fmt);
		snprintf(m->fmt + n, sizeof m->fmt - n, "/%s", k);
		return;
	}
	if (m->dur) snprintf(m->fmt, sizeof m->fmt, "AAC %ldk", fsize * 8 / m->dur / 1000);
	else snprintf(m->fmt, sizeof m->fmt, "AAC");
}

static void meta_read(const char *path, const char *name, Meta *m) {
	memset(m, 0, sizeof *m);
	FILE *f = fopen(path, "rb");
	if (f) {
		const char *ext = strrchr(name, '.');
		fseek(f, 0, SEEK_END);
		long size = ftell(f);
		fseek(f, 0, SEEK_SET);
		if (ext && !strcasecmp(ext, ".flac")) meta_flac(f, m);
		else if (ext && (!strcasecmp(ext, ".m4a") || !strcasecmp(ext, ".mp4"))) meta_m4a(f, size, m);
		else if (ext && (!strcasecmp(ext, ".ogg") || !strcasecmp(ext, ".oga") || !strcasecmp(ext, ".opus"))) meta_ogg(f, size, m);
		else if (ext && !strcasecmp(ext, ".wav")) meta_wav(path, size, m);
		else meta_mp3(f, path, size, m);
		fclose(f);
	}
	meta_clean(m->title); meta_clean(m->artist); meta_clean(m->album); meta_clean(m->albumartist);
	if (!m->title[0]) {
		meta_copy(m->title, sizeof m->title, name, strlen(name));
		char *dot = strrchr(m->title, '.');
		if (dot) *dot = 0;
				meta_clean(m->title);
	}
}
