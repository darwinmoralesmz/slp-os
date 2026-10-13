// Decodificación de Ogg Vorbis y Opus (mono o estéreo). Opus sale siempre a 48 kHz.
#define STB_VORBIS_HEADER_ONLY
#include "../third_party/stb_vorbis.c"
#include <opus.h>
#include "oggdemux.h"
#include "ogg.h"

#define OPUS_MAX_FRAME 5760   // 120 ms a 48 kHz
#define OPUS_PREROLL 3840     // 80 ms para que el decodificador converja tras un salto

struct Ogg {
	stb_vorbis *vb;
	float *fb;
	FILE *f; OgR r; OpusDecoder *od;
	unsigned ch, preskip, discard, pn, po;
	long data_off;
	unsigned long long total, emitted;
	unsigned char *pkt; short *pcm;
};

static unsigned le16(const unsigned char *p) { return p[0] | p[1] << 8; }

static Ogg *opus_start(FILE *f, const unsigned char *head, int hl, long fsize, unsigned *rate, unsigned *ch, unsigned long long *total) {
	Ogg *o = calloc(1, sizeof *o);
	if (!o) return NULL;
	o->f = f;
	if (hl < 19 || head[8] != 1 || (head[18] && head[9] > 2) || head[9] < 1 || head[9] > 2) { free(o); return NULL; }
	o->ch = head[9]; o->preskip = le16(head + 10);
	int err = 0;
	o->od = opus_decoder_create(48000, o->ch, &err);
	if (!o->od || err) { free(o); return NULL; }
	opus_decoder_ctl(o->od, OPUS_SET_GAIN((short)le16(head + 16)));
	int64_t g = og_last_granule(f, fsize);
	o->total = g > (int64_t)o->preskip ? g - o->preskip : 0;
	o->pkt = malloc(1 << 16); o->pcm = malloc(OPUS_MAX_FRAME * 2 * sizeof *o->pcm);
	if (!o->pkt || !o->pcm) { opus_decoder_destroy(o->od); free(o->pkt); free(o->pcm); free(o); return NULL; }
	o->discard = o->preskip;
	*rate = 48000; *ch = o->ch; *total = o->total;
	return o;
}

Ogg *ogg_open(const char *path, unsigned *rate, unsigned *ch, unsigned long long *total) {
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long fsize = ftell(f);
	fseek(f, 0, SEEK_SET);
	OgR r;
	unsigned char head[64];
	og_init(&r, f);
	int hl = og_packet(&r, head, sizeof head);
	if (hl >= 19 && !memcmp(head, "OpusHead", 8)) {
		unsigned char tags[8];
		og_packet(&r, tags, sizeof tags);   // OpusTags (puede ser más largo que el búfer: se descarta)
		long off = ftell(f);
		Ogg *o = opus_start(f, head, hl, fsize, rate, ch, total);
		if (!o) { og_free(&r); fclose(f); return NULL; }
		o->r = r; o->data_off = off;
		fseek(f, off, SEEK_SET);
		o->r.nseg = o->r.seg_i = 0;
		return o;
	}
	og_free(&r);
	if (hl < 7 || memcmp(head, "\x01vorbis", 7)) { fclose(f); return NULL; }
	fclose(f);
	Ogg *o = calloc(1, sizeof *o);
	int err = 0;
	if (!o || !(o->vb = stb_vorbis_open_filename(path, &err, NULL))) { free(o); return NULL; }
	stb_vorbis_info vi = stb_vorbis_get_info(o->vb);
	if (vi.channels < 1 || vi.channels > 2 || vi.sample_rate < 1 || !(o->fb = malloc(1024 * 2 * sizeof(float)))) { stb_vorbis_close(o->vb); free(o); return NULL; }
	o->ch = vi.channels;
	*rate = vi.sample_rate; *ch = o->ch; *total = stb_vorbis_stream_length_in_samples(o->vb);
	return o;
}

static size_t opus_read(Ogg *o, int32_t *buf, size_t n) {
	size_t out = 0;
	while (out < n && !(o->total && o->emitted >= o->total)) {
		if (o->po >= o->pn) {
			int len = og_packet(&o->r, o->pkt, 1 << 16);
			if (len < 0) break;
			if (len == 0) continue;
			int got = opus_decode(o->od, o->pkt, len, o->pcm, OPUS_MAX_FRAME, 0);
			if (got <= 0) continue;
			o->pn = got; o->po = o->discard < o->pn ? o->discard : o->pn;
			o->discard -= o->po;
			continue;
		}
		unsigned take = o->pn - o->po;
		if (take > n - out) take = n - out;
		if (o->total && take > o->total - o->emitted) take = o->total - o->emitted;
		for (unsigned i = 0; i < take * o->ch; i++) buf[out * o->ch + i] = (int32_t)o->pcm[o->po * o->ch + i] * 65536;
		o->po += take; out += take; o->emitted += take;
	}
	return out;
}

size_t ogg_read(Ogg *o, int32_t *buf, size_t n) {
	if (o->od) return opus_read(o, buf, n);
	if (n > 1024) n = 1024;
	int got = stb_vorbis_get_samples_float_interleaved(o->vb, o->ch, o->fb, n * o->ch);
	for (int i = 0; i < got * (int)o->ch; i++) {
		float v = o->fb[i] * 2147483648.0f;
		buf[i] = v >= 2147483647.0f ? 2147483647 : v <= -2147483648.0f ? INT32_MIN : (int32_t)v;
	}
	return got;
}

int ogg_seek(Ogg *o, unsigned long long frame) {
	if (!o->od) return stb_vorbis_seek(o->vb, frame) ? 0 : -1;
	unsigned long long abs = frame + o->preskip, pre = abs > OPUS_PREROLL ? abs - OPUS_PREROLL : 0;
	int64_t last_g = 0, best_g = 0;
	long best_off = o->data_off;
	fseek(o->f, o->data_off, SEEK_SET);
	while (!og_page_at(&o->r, 0)) {
		if (!o->r.cont && last_g >= 0 && (unsigned long long)last_g <= pre) { best_off = o->r.page_off; best_g = last_g; }
		if (o->r.granule >= 0) { last_g = o->r.granule; if ((unsigned long long)last_g > pre) break; }
	}
	fseek(o->f, best_off, SEEK_SET);
	o->r.nseg = o->r.seg_i = 0;
	opus_decoder_ctl(o->od, OPUS_RESET_STATE);
	o->pn = o->po = 0;
	o->discard = abs - best_g;
	o->emitted = frame;
	return 0;
}

void ogg_close(Ogg *o) {
	if (o->od) { opus_decoder_destroy(o->od); og_free(&o->r); fclose(o->f); free(o->pkt); free(o->pcm); }
	else { stb_vorbis_close(o->vb); free(o->fb); }
	free(o);
}
