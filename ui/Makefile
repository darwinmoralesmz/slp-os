# Compila r1 y el banco lvgl_bench, estático para MIPS32r2 (musl).
CC = ../tools/musl/mipsel-linux-musl-cross/bin/mipsel-linux-musl-gcc
STRIP = ../tools/musl/mipsel-linux-musl-cross/bin/mipsel-linux-musl-strip
CFLAGS = -Os -march=mips32r2 -fno-pie -ffunction-sections -fdata-sections -DLV_CONF_INCLUDE_SIMPLE -I. -I../lvgl -I../lvgl/src -I../src -Wno-unused-function -fwrapv -MMD -MP
LDFLAGS = -static -no-pie -Wl,--gc-sections
# Sin controladores ni aceleradores de otras plataformas.
LVGL_SRC = $(shell find ../lvgl/src -name '*.c' -not -path '*/drivers/nuttx/*' -not -path '*/drivers/glfw/*' -not -path '*/drivers/windows/*' -not -path '*/drivers/x11/*' -not -path '*/drivers/wayland/*' -not -path '*/drivers/libinput/*' -not -path '*/drivers/sdl/*' -not -path '*/draw/nxp/*' -not -path '*/draw/renesas/*' -not -path '*/draw/vg_lite/*')
LVGL_OBJ = $(patsubst ../%.c,build/c/%.o,$(LVGL_SRC))

all: lvgl_bench r1

build/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

lvgl_bench: build/bench.o $(LVGL_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ -lm -lpthread
	$(STRIP) $@

# Recompilar todo si cambia la configuración de LVGL.
$(LVGL_OBJ): lv_conf.h

# Códecs: FAAD2 (AAC), libopus en coma fija (solo decodificador), stb_vorbis y ALAC. Se compilan igual para el R1, el simulador y el fuzzing.
FAAD_SRC = $(wildcard ../third_party/faad2/libfaad/*.c)
OPUS_DIR = ../third_party/opus
OPUS_SRC = $(filter-out %/celt_encoder.c %/opus_custom_demo.c,$(wildcard $(OPUS_DIR)/celt/*.c)) $(wildcard $(OPUS_DIR)/silk/*.c $(OPUS_DIR)/silk/fixed/*.c) $(OPUS_DIR)/src/opus.c $(OPUS_DIR)/src/opus_decoder.c $(OPUS_DIR)/src/extensions.c
FAAD_FLAGS = -DHAVE_STDINT_H -DHAVE_STRING_H -DHAVE_MEMCPY -DHAVE_LRINTF -DPACKAGE_VERSION=\"2.11.1\" -I../third_party/faad2/include -I../third_party/faad2/libfaad -w
OPUS_FLAGS = -DOPUS_BUILD -DFIXED_POINT -DUSE_ALLOCA -DHAVE_LRINT -DHAVE_LRINTF -I$(OPUS_DIR)/include -I$(OPUS_DIR)/celt -I$(OPUS_DIR)/silk -I$(OPUS_DIR)/silk/fixed -I$(OPUS_DIR) -w
CODEC_C = $(FAAD_SRC) $(OPUS_SRC) ../src/aac.c ../src/ogg.c ../src/alac.c ../third_party/stb_vorbis.c
CODEC_OBJ = $(patsubst ../%.c,build/c/%.o,$(CODEC_C))
objs = $(foreach b,build build_sim build/fz,$(patsubst ../%.c,$b/c/%.o,$1))
$(call objs,$(FAAD_SRC) ../src/aac.c): XFLAGS = $(FAAD_FLAGS)
$(call objs,$(OPUS_SRC) ../src/ogg.c): XFLAGS = $(OPUS_FLAGS)
$(call objs,../src/alac.c): XFLAGS = -w
$(call objs,../third_party/stb_vorbis.c): XFLAGS = -w -DNDEBUG   # sin assert: un archivo dañado no debe abortar
build/c/%.o: ../%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(XFLAGS) -c $< -o $@
r1: $(CODEC_OBJ) build/r1.o build/fonts/ui_font_24.o build/fonts/ui_font_26.o build/fonts/ui_font_28.o build/fonts/ui_font_30.o build/fonts/ui_font_32.o build/fonts/ui_font_38.o build/fonts/ui_icons_26.o build/fonts/ui_icons_30.o build/fonts/ui_icons_34.o build/fonts/ui_icons_46.o $(LVGL_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ -lm -lpthread
	$(STRIP) $@

# Simulador en PC (SDL2): `make sim` y `./sim`. Pistas de prueba en ui/sim_music (ver sim_music.sh).
SIMCC = gcc
SIMFLAGS = -O1 -g -fwrapv -MMD -MP -DR1_SIM -DLV_CONF_INCLUDE_SIMPLE -I. -I../lvgl -I../lvgl/src -I../src -Wno-unused-function -w $(shell pkg-config --cflags sdl2)
SIM_OBJ = $(patsubst ../%.c,build_sim/c/%.o,$(LVGL_SRC) $(wildcard ../lvgl/src/drivers/sdl/*.c))
build_sim/%.o: %.c
	@mkdir -p $(dir $@)
	$(SIMCC) $(SIMFLAGS) -c $< -o $@
build_sim/c/%.o: ../%.c
	@mkdir -p $(dir $@)
	$(SIMCC) $(SIMFLAGS) $(XFLAGS) -c $< -o $@
$(SIM_OBJ): lv_conf.h
SIM_CODEC = $(patsubst build/%,build_sim/%,$(CODEC_OBJ))
sim: $(SIM_CODEC) build_sim/r1.o build_sim/fonts/ui_font_24.o build_sim/fonts/ui_font_26.o build_sim/fonts/ui_font_28.o build_sim/fonts/ui_font_30.o build_sim/fonts/ui_font_32.o build_sim/fonts/ui_font_38.o build_sim/fonts/ui_icons_26.o build_sim/fonts/ui_icons_30.o build_sim/fonts/ui_icons_34.o build_sim/fonts/ui_icons_46.o $(SIM_OBJ)
	$(SIMCC) -o $@ $^ -lm -lpthread $(shell pkg-config --libs sdl2)

# Arnés de fuzzing con ASAN/UBSAN en el PC (lo usa scripts/fuzz/run.sh).
FZFLAGS = -O1 -g -fsanitize=address,undefined -fno-sanitize=signed-integer-overflow,shift -fno-sanitize-recover=undefined -I../src -w -MMD -MP
build/fz/c/%.o: ../%.c
	@mkdir -p $(dir $@)
	gcc $(FZFLAGS) $(XFLAGS) -c $< -o $@
fz: build/fz/fz
build/fz/fz: $(patsubst build/%,build/fz/%,$(CODEC_OBJ)) build/fz/c/scripts/fuzz/fz.o
	gcc $(FZFLAGS) -o $@ $^ -lm

# Pruebas sin hardware: política de energía, curva de volumen y rutas del servidor web.
test:
	gcc -I../src ../src/power_test.c -o /tmp/power_test && /tmp/power_test
	gcc -I../src ../src/vol_test.c -o /tmp/vol_test && /tmp/vol_test
	gcc -I../src ../src/webd_test.c -o /tmp/webd_test && /tmp/webd_test
	gcc -I../src ../src/eq_test.c -o /tmp/eq_test -lm && /tmp/eq_test

# Dependencias de cabeceras generadas por -MMD (src/*.h, lv_conf.h, ...).
-include $(shell find build build_sim -name '*.d' 2>/dev/null)
