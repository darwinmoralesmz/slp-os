// M4A/AAC: contenedor MP4 + FAAD2, en su propia unidad de compilación (minimp4 y minimp3 comparten nombres internos).
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct Aac Aac;
Aac *aac_open(const char *path, unsigned *rate, unsigned *ch, unsigned long long *total);
size_t aac_read(Aac *a, int32_t *buf, size_t n);   // frames estéreo/mono como enteros de 32 bit alineados a la izquierda
int aac_seek(Aac *a, unsigned long long frame);
void aac_close(Aac *a);
