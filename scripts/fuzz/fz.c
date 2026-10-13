// Arnés de robustez: etiquetas + decodificación completa + salto, sobre el archivo dado.
#include <stdio.h>
#include <string.h>
#include "audio.h"
#include "meta.h"
int main(int c, char **v) {
	Meta m;
	static int32_t buf[PCM_PERIOD * 2];
	const char *base = strrchr(v[1], '/');
	meta_read(v[1], base ? base + 1 : v[1], &m);
	Dec d;
	if (dec_open(&d, v[1])) return 0;
	unsigned long long fr = 0; size_t n;
	while ((n = dec_read(&d, buf, PCM_PERIOD)) && fr < 50000000ULL) fr += n;
	if (!dec_seek(&d, d.rate * 1ULL)) dec_read(&d, buf, PCM_PERIOD);
	if (!dec_seek(&d, d.total / 2)) dec_read(&d, buf, PCM_PERIOD);
	dec_close(&d);
	return 0;
}
