#define MINIMP4_IMPLEMENTATION
#include "../third_party/minimp4.h"
#include "../third_party/faad2/include/neaacdec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aac.h"
#include "alac.h"

// Retardo del codificador/decodificador AAC (el inicio útil de la pista, igual que ffmpeg).
#define AAC_DELAY 1088

struct Aac {
	FILE *f; MP4D_demux_t mp4; int track; unsigned frame, frames, ch, pend_n, pend_off, discard;
	NeAACDecHandle faad; unsigned char *buf; int32_t *pend;
	alac_file *alac; int32_t (*abuf)[ALAC_BLOCKSIZE]; unsigned alac_len;   // ALAC: decodificador, buffer por canal y muestras por trama
};

// minimp4 lee la cabecera byte a byte; sobre el archivo cada lectura es una llamada al sistema (un M4A tardaba 200-500 ms en abrir
// en el R1). Se carga la caja moov entera en memoria y las lecturas dentro de ella salen de ahí.
typedef struct { FILE *f; unsigned char *moov; int64_t off, len; } Src;
static int file_read(FILE *f, int64_t offset, void *buffer, size_t size) {
	return fseeko(f, offset, SEEK_SET) || fread(buffer, 1, size, f) != size;
}
static int mp4_read(int64_t offset, void *buffer, size_t size, void *token) {
	Src *s = token;
	if (s->moov && offset >= s->off && offset + (int64_t)size <= s->off + s->len) { memcpy(buffer, s->moov + (offset - s->off), size); return 0; }
	return file_read(s->f, offset, buffer, size);
}
static void load_moov(Src *s, int64_t fsize) {
	int64_t pos = 0;
	unsigned char h[16];
	while (pos + 8 <= fsize && !file_read(s->f, pos, h, 8)) {
		uint64_t sz = (uint32_t)h[0] << 24 | h[1] << 16 | h[2] << 8 | h[3];
		if (sz == 1) { if (file_read(s->f, pos + 8, h + 8, 8)) return; sz = (uint64_t)((uint32_t)h[8] << 24 | h[9] << 16 | h[10] << 8 | h[11]) << 32 | ((uint32_t)h[12] << 24 | h[13] << 16 | h[14] << 8 | h[15]); }
		else if (sz == 0) sz = fsize - pos;
		if (sz < 8 || sz > (uint64_t)(fsize - pos)) return;
		if (!memcmp(h + 4, "moov", 4)) {
			if (sz > (32u << 20)) return;
			s->moov = malloc(sz);
			if (s->moov && file_read(s->f, pos, s->moov, sz)) { free(s->moov); s->moov = NULL; return; }
			s->off = pos; s->len = sz;
			return;
		}
		pos += sz;
	}
}

// Busca en la caja moov la entrada de muestras "alac" (cookie de 36 bytes) y la deja en cookie con 12 bytes de relleno delante.
static int find_alac(const unsigned char *moov, int64_t len, unsigned char *cookie) {
	static const unsigned char pat[12] = {0, 0, 0, 0x24, 'a', 'l', 'a', 'c', 0, 0, 0, 0};
	for (int64_t i = 0; moov && i + 36 <= len; i++)
		if (!memcmp(moov + i, pat, 12)) { memset(cookie, 0, 12); memcpy(cookie + 12, moov + i, 36); return 1; }
	return 0;
}

Aac *aac_open(const char *path, unsigned *rate, unsigned *ch_out, unsigned long long *total) {
	Aac *a = calloc(1, sizeof *a);
	a->f = fopen(path, "rb");
	if (!a->f) goto fail;
	fseeko(a->f, 0, SEEK_END);
	int64_t size = ftello(a->f);
	Src src = {a->f, NULL, 0, 0};
	load_moov(&src, size);
	int ok = MP4D_open(&a->mp4, mp4_read, &src, size);
	unsigned char cookie[48];
	int is_alac = ok && find_alac(src.moov, src.len, cookie);
	free(src.moov);
	if (!ok) goto fail;
	a->track = -1;
	for (unsigned i = 0; i < a->mp4.track_count; i++) {
		MP4D_track_t *c = &a->mp4.track[i];
		int aac = c->object_type_indication == MP4_OBJECT_TYPE_AUDIO_ISO_IEC_14496_3;
		if (c->handler_type == MP4D_HANDLER_TYPE_SOUN && aac != is_alac) { a->track = i; break; }
	}
	if (a->track < 0) goto fail;
	MP4D_track_t *t = &a->mp4.track[a->track];
	if (is_alac) {
		unsigned len = cookie[24] << 24 | cookie[25] << 16 | cookie[26] << 8 | cookie[27];
		unsigned bits = cookie[29], ch = cookie[33], kb = cookie[32];
		unsigned r = cookie[44] << 24 | cookie[45] << 16 | cookie[46] << 8 | cookie[47];
		if (!len || len > ALAC_BLOCKSIZE || (bits != 16 && bits != 24) || kb > 31 || ch < 1 || ch > 2 || !r || !t->timescale) goto fail;
		a->alac = calloc(1, sizeof *a->alac);
		a->abuf = malloc(2 * sizeof *a->abuf);
		a->buf = malloc((1 << 16) + 8);
		a->pend = malloc(sizeof(int32_t) * ALAC_BLOCKSIZE * 2);
		if (!a->alac || !a->abuf || !a->buf || !a->pend) goto fail;
		alac_set_info(a->alac, (char *)cookie);
		*rate = r; *ch_out = a->ch = ch;
		a->frames = t->sample_count; a->alac_len = len;
		*total = ((unsigned long long)t->duration_hi << 32 | t->duration_lo) * r / t->timescale;
		return a;
	}
	a->faad = NeAACDecOpen();
	NeAACDecConfigurationPtr cfg = NeAACDecGetCurrentConfiguration(a->faad);
	cfg->outputFormat = FAAD_FMT_16BIT;
	NeAACDecSetConfiguration(a->faad, cfg);
	unsigned long r; unsigned char c;
	if (NeAACDecInit2(a->faad, t->dsi, t->dsi_bytes, &r, &c) < 0 || !r || !c) goto fail;
	*rate = r; *ch_out = a->ch = c;
	a->frames = t->sample_count;
	*total = (unsigned long long)t->sample_count * 1024 - AAC_DELAY;
	a->discard = AAC_DELAY;
	a->buf = malloc(1 << 16);
	a->pend = malloc(sizeof(int32_t) * 4096 * 2);
	return a;
fail:
	free(a->alac); free(a->abuf); free(a->buf); free(a->pend);
	if (a->faad) NeAACDecClose(a->faad);
	if (a->mp4.track) MP4D_close(&a->mp4);
	if (a->f) fclose(a->f);
	free(a);
	return NULL;
}

size_t aac_read(Aac *a, int32_t *buf, size_t n) {
	size_t out = 0;
	while (out < n) {
		if (a->pend_off == a->pend_n) {
			if (a->frame >= a->frames) break;
			unsigned fb, ts, dur;
			MP4D_file_offset_t ofs = MP4D_frame_offset(&a->mp4, a->track, a->frame++, &fb, &ts, &dur);
			if (fb > (1u << 16) || file_read(a->f, ofs, a->buf, fb)) break;
			unsigned frames;
			if (a->alac) {
				memset(a->buf + fb, 0, 8);   // el decodificador lee unos bytes de más
				int m = alac_decode_frame(a->alac, a->buf, a->abuf);
				if (m <= 0) continue;
				frames = m;
				for (unsigned i = 0; i < frames; i++) for (unsigned c = 0; c < a->ch; c++) a->pend[i * a->ch + c] = a->abuf[c][i] * 8;   // 29 bit -> 32 bit
			} else {
				NeAACDecFrameInfo fi;
				short *pcm = NeAACDecDecode(a->faad, &fi, a->buf, fb);
				if (fi.error || !pcm || fi.channels != a->ch || fi.channels > 2) continue;   // trama dañada o con otro número de canales
				frames = fi.samples / fi.channels;
				if (frames > 4096) frames = 4096;
				for (unsigned i = 0; i < frames * fi.channels; i++) a->pend[i] = (int32_t)pcm[i] * 65536;
			}
			a->pend_n = frames; a->pend_off = 0;
			unsigned drop = a->discard < frames ? a->discard : frames;
			a->pend_off = drop; a->discard -= drop;
			if (a->pend_off == a->pend_n) continue;
		}
		unsigned take = a->pend_n - a->pend_off;
		if (take > n - out) take = n - out;
		memcpy(buf + out * a->ch, a->pend + a->pend_off * a->ch, take * a->ch * sizeof(int32_t));
		a->pend_off += take; out += take;
	}
	return out;
}

int aac_seek(Aac *a, unsigned long long frame) {
	if (a->alac) {
		a->frame = frame / a->alac_len;
		a->discard = frame - (unsigned long long)a->frame * a->alac_len;
		a->pend_n = a->pend_off = 0;
		return 0;
	}
	unsigned long long q = frame + AAC_DELAY;
	unsigned f = q / 1024;
	a->frame = f;
	// tras reiniciar, la primera trama emite lo anterior a "f" con el solapamiento sin cebar: se descarta
	a->discard = f ? 1024 + (q - (unsigned long long)f * 1024) : q;
	a->pend_n = a->pend_off = 0;
	NeAACDecPostSeekReset(a->faad, a->frame);
	return 0;
}

void aac_close(Aac *a) {
	if (a->faad) NeAACDecClose(a->faad);
	free(a->alac); free(a->abuf);
	MP4D_close(&a->mp4); fclose(a->f); free(a->buf); free(a->pend); free(a);
}
