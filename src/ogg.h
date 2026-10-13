// Ogg Vorbis (stb_vorbis) y Opus (libopus en coma fija + lector Ogg propio), en su propia unidad de compilación.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct Ogg Ogg;
Ogg *ogg_open(const char *path, unsigned *rate, unsigned *ch, unsigned long long *total);
size_t ogg_read(Ogg *o, int32_t *buf, size_t n);   // frames como enteros de 32 bit alineados a la izquierda
int ogg_seek(Ogg *o, unsigned long long frame);
void ogg_close(Ogg *o);
