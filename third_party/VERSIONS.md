# Dependencias de terceros

Incluidas en el repositorio (solo cabeceras):

| Archivo | Versión | Licencia | Uso |
|---|---|---|---|
| `dr_flac.h` | dr_flac 0.13.4 | dominio público / MIT-0 | decodificador FLAC |
| `minimp3.h`, `minimp3_ex.h` | lieff/minimp3 (rama master, 2026-10) | CC0 | decodificador MP3 (+ `mp3dec_ex` con búsqueda) |
| `minimp4.h` | lieff/minimp4 (rama master, 2026-10) | CC0 | demultiplexor MP4/M4A |
| `stb_image.h` | stb_image 2.30 | dominio público | JPEG/PNG (portadas) |
| `stb_vorbis.c` | stb_vorbis (rama master, 2026-10) | dominio público | decodificador Ogg Vorbis |
| `dr_wav.h` | dr_wav (rama master, 2026-10) | dominio público / MIT-0 | decodificador WAV |

Descargadas por `scripts/setup.sh` (no incluidas por tamaño o licencia):

| Componente | Versión | Licencia |
|---|---|---|
| LVGL | v9.2.2 | MIT |
| FAAD2 | commit `521d105979795b887ea11e1f36565e97a8fbfc53` (2026-10-01) | **GPLv2** (uso no GPL "prohibido"; ver docs/PROBLEMAS.md) |
| libopus | v1.5.2 (solo decodificador, coma fija) | BSD | Opus |
| Toolchain `mipsel-linux-musl-cross` | GCC 11.2.1 (musl.cc, 2021-11-23) | varias |
| squashfs-tools | 4.7.5 | GPL |

Las fuentes de interfaz (`ui/fonts/*.c`) están ya generadas con `lv_font_conv` 1.5.3 a partir de Montserrat Medium,
unscii-8 y FontAwesome 5 (incluidas en `lvgl/scripts/built_in_font/`).

`src/alac.c`: decodificador ALAC de David Hammerton (2005, licencia MIT) tomado de `lib/rbcodec/codecs/libalac` de Rockbox y adaptado (tablas en TLS, comprobaciones de tamaño).
