// Remuestreador polifásico estéreo (sinc con ventana Kaiser): entero 32 bit alineado a la izquierda -> int16 a otra tasa.
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define RS_MAXIN 1024
#define RS_MAXOUT 8192

typedef struct { int L, M, H, taps, cnt, ipos, ph; float *coef, *w[2]; } Rs;

static double rs_i0(double x) { double s = 1, t = 1; for (int k = 1; k < 40; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; }
static int rs_gcd(int a, int b) { while (b) { int t = a % b; a = b; b = t; } return a; }

static void rs_init(Rs *r, int in, int out) {
	int g = rs_gcd(in, out);
	r->L = out / g; r->M = in / g;
	double scale = in > out ? (double)out / in : 1.0;
	r->H = 16 * (in > out ? (in + out - 1) / out : 1);
	r->taps = 2 * r->H;
	double fc = 0.5 * scale * 0.96, beta = 8.6, i0b = rs_i0(beta);
	r->coef = malloc(sizeof(float) * r->L * r->taps);
	for (int p = 0; p < r->L; p++) {
		double sum = 0, frac = (double)p / r->L;
		for (int k = 0; k < r->taps; k++) {
			double x = (k - (r->H - 1)) - frac, u = x / r->H, h = 0;
			if (fabs(u) < 1) {
				double a = 2 * fc * x, sinc = fabs(a) < 1e-9 ? 1.0 : sin(M_PI * a) / (M_PI * a);
				h = 2 * fc * sinc * rs_i0(beta * sqrt(1 - u * u)) / i0b;
			}
			r->coef[p * r->taps + k] = (float)h;
			sum += h;
		}
		for (int k = 0; k < r->taps; k++) r->coef[p * r->taps + k] /= (float)sum;
	}
	for (int c = 0; c < 2; c++) r->w[c] = calloc(r->taps + RS_MAXIN + 8, sizeof(float));
	r->cnt = r->H - 1; r->ipos = r->H - 1; r->ph = 0;
}

static void rs_reset(Rs *r) {
	for (int c = 0; c < 2; c++) memset(r->w[c], 0, sizeof(float) * (r->taps + RS_MAXIN + 8));
	r->cnt = r->H - 1; r->ipos = r->H - 1; r->ph = 0;
}
static void rs_free(Rs *r) { free(r->coef); free(r->w[0]); free(r->w[1]); }

// in: n frames estéreo intercalados; out: hasta RS_MAXOUT frames int16 estéreo. Devuelve los frames producidos.
static int rs_process(Rs *r, const int32_t *in, int n, int16_t *out) {
	for (int i = 0; i < n; i++)
		for (int c = 0; c < 2; c++) r->w[c][r->cnt + i] = in[2 * i + c] * (1.0f / 2147483648.0f);
	r->cnt += n;
	int m = 0;
	while (r->ipos + r->H < r->cnt && m < RS_MAXOUT) {
		const float *h = r->coef + r->ph * r->taps;
		for (int c = 0; c < 2; c++) {
			const float *x = r->w[c] + r->ipos - r->H + 1;
			float acc = 0;
			for (int k = 0; k < r->taps; k++) acc += x[k] * h[k];
			acc *= 32767.0f;
			out[2 * m + c] = acc > 32767 ? 32767 : acc < -32768 ? -32768 : (int16_t)lrintf(acc);
		}
		m++;
		r->ph += r->M; r->ipos += r->ph / r->L; r->ph %= r->L;
	}
	int drop = r->ipos - (r->H - 1);
	if (drop > 0) {
		for (int c = 0; c < 2; c++) memmove(r->w[c], r->w[c] + drop, (r->cnt - drop) * sizeof(float));
		r->cnt -= drop; r->ipos -= drop;
	}
	return m;
}
