// Banco de pruebas de carga de la interfaz (decodificación JPEG, lista con miniaturas, transiciones y efectos).
// Uso: lvgl_bench [escenario...] (decode scroll slide fx idle); sin argumentos corre todos. Imágenes en /data/mnt/r1os/dev/bench/img.
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "../third_party/stb_image.h"
#include "lvgl.h"
#include "src/draw/lv_image_decoder_private.h"

#define IMG "P:/data/mnt/r1os/dev/bench/img/"
static const lv_font_t *font = &lv_font_montserrat_20;

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static double cpu(void) { struct timespec t; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static uint32_t tick_ms(void) { return (uint32_t)(now() * 1000); }
static int rss_kb(void) { FILE *f = fopen("/proc/self/status", "r"); char l[128]; int kb = 0; while (f && fgets(l, sizeof l, f)) if (!strncmp(l, "VmRSS:", 6)) kb = atoi(l + 6); if (f) fclose(f); return kb; }

// Métricas de renderizado de pantalla.
static int frames; static double t_start, ref_t0, ref_sum, ref_max;
static void on_refr_start(lv_event_t *e) { ref_t0 = now(); }
static void on_refr_ready(lv_event_t *e) { double d = (now() - ref_t0) * 1000; ref_sum += d; if (d > ref_max) ref_max = d; frames++; }
static double cpu0;
static void metrics_reset(void) { frames = 0; ref_sum = ref_max = 0; t_start = now(); cpu0 = cpu(); }
static void metrics_print(const char *name) {
	double dt = now() - t_start;
	printf("%-8s %5.1f fps | render medio %6.1f ms, max %6.1f ms | CPU %3.0f%% | RSS %d kB\n", name, frames / dt, frames ? ref_sum / frames : 0, ref_max, 100 * (cpu() - cpu0) / dt, rss_kb());
	fflush(stdout);
}

static void run_for(double secs) {
	double end = now() + secs;
	while (now() < end) { uint32_t w = lv_timer_handler(); usleep((w > 10 ? 10 : w + 1) * 1000); }
}

static void clear(void) { lv_anim_delete(NULL, NULL); lv_obj_clean(lv_screen_active()); lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x202428), 0); }

static void scn_decode(void) {
	const char *files[] = {"t1.jpg", "cover1.jpg", "cover_big.jpg"};
	lv_image_decoder_args_t args = {.no_cache = true};
	for (int i = 0; i < 3; i++) {
		char path[160];
		snprintf(path, sizeof path, IMG "%s", files[i]);
		double sum = 0; int ok = 0, w = 0, h = 0;
		for (int k = 0; k < 4; k++) {
			lv_image_decoder_dsc_t dsc;
			double t0 = now();
			lv_result_t r = lv_image_decoder_open(&dsc, path, &args);
			double d = (now() - t0) * 1000;
			if (r == LV_RESULT_OK) { ok = 1; w = dsc.header.w; h = dsc.header.h; sum += d; lv_image_decoder_close(&dsc); }
		}
		printf("decode   %-14s %s %dx%d: %6.1f ms por imagen (RGB %d kB) | RSS %d kB\n", files[i], ok ? "ok" : "FALLO", w, h, ok ? sum / 4 : 0.0, w * h * 3 / 1024, rss_kb());
	}
	fflush(stdout);
}

// Decodifica una vez (stb_image), reduce por promedio de área a w x h y deja la imagen en RGB565 en RAM.
static lv_image_dsc_t *load_cover(const char *path, int w, int h, double *ms) {
	double t0 = now();
	int sw, sh, n;
	unsigned char *src = stbi_load(path, &sw, &sh, &n, 3);
	if (!src) return NULL;
	uint16_t *px = malloc(w * h * 2);
	for (int y = 0; y < h; y++) {
		int y0 = y * sh / h, y1 = (y + 1) * sh / h; if (y1 <= y0) y1 = y0 + 1;
		for (int x = 0; x < w; x++) {
			int x0 = x * sw / w, x1 = (x + 1) * sw / w; if (x1 <= x0) x1 = x0 + 1;
			unsigned r = 0, g = 0, b = 0, c = 0;
			for (int yy = y0; yy < y1; yy++) for (int xx = x0; xx < x1; xx++) { unsigned char *p = src + (yy * sw + xx) * 3; r += p[0]; g += p[1]; b += p[2]; c++; }
			r /= c; g /= c; b /= c;
			px[y * w + x] = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3;
		}
	}
	stbi_image_free(src);
	lv_image_dsc_t *d = calloc(1, sizeof *d);
	d->header.magic = LV_IMAGE_HEADER_MAGIC; d->header.cf = LV_COLOR_FORMAT_RGB565; d->header.w = w; d->header.h = h; d->header.stride = w * 2;
	d->data_size = w * h * 2; d->data = (const uint8_t *)px;
	if (ms) *ms = (now() - t0) * 1000;
	return d;
}

static void scn_load(void) {
	const char *files[] = {"cover1.jpg", "cover_big.jpg"};
	for (int i = 0; i < 2; i++) {
		char path[160]; snprintf(path, sizeof path, "/data/mnt/r1os/dev/bench/img/%s", files[i]);
		double ms = 0; lv_image_dsc_t *d = load_cover(path, 440, 440, &ms);
		printf("load     %-14s -> 440x440 RGB565: %6.1f ms (decodificar + reducir) %s\n", files[i], ms, d ? "ok" : "FALLO");
		fflush(stdout);
	}
}

static void scroll_cb(void *o, int32_t v) { lv_obj_scroll_to_y(o, v, LV_ANIM_OFF); }
static lv_image_dsc_t *thumb_ram[12];
static void scn_scroll(int scaled) {
	clear();
	if (scaled >= 3 && !thumb_ram[0]) for (int i = 0; i < 12; i++) { char p[100]; snprintf(p, sizeof p, "/data/mnt/r1os/dev/bench/img/t%d.jpg", i + 1); thumb_ram[i] = load_cover(p, 64, 64, NULL); }
	lv_obj_t *list = lv_obj_create(lv_screen_active());
	lv_obj_set_size(list, scaled >= 2 ? 440 : 480, scaled >= 2 ? 336 : 800);
	if (scaled >= 2) lv_obj_set_pos(list, 20, 40);
	lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(list, 4, 0);
	for (int i = 0; i < 200; i++) {
		lv_obj_t *row = lv_obj_create(list);
		if (scaled == 4) lv_obj_remove_style_all(row);
		lv_obj_set_size(row, lv_pct(100), 76);
		lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_t *img = lv_image_create(row);
		char path[100];
		snprintf(path, sizeof path, IMG "%s%d.jpg", scaled == 2 ? "tp" : "t", i % 12 + 1);
		if (scaled >= 3) lv_image_set_src(img, thumb_ram[i % 12]); else lv_image_set_src(img, path);
		if (scaled == 1) { lv_image_set_scale(img, 256 * 64 / 480); lv_image_set_inner_align(img, LV_IMAGE_ALIGN_TOP_LEFT); lv_obj_set_size(img, 64, 64); }
		lv_obj_align(img, LV_ALIGN_LEFT_MID, 0, 0);
		lv_obj_t *l = lv_label_create(row);
		lv_label_set_text_fmt(l, "Pista %d - Titulo de ejemplo", i + 1);
		lv_obj_set_style_text_font(l, font, 0);
		lv_obj_align(l, LV_ALIGN_LEFT_MID, 80, 0);
	}
	lv_anim_t a; lv_anim_init(&a);
	lv_anim_set_var(&a, list); lv_anim_set_exec_cb(&a, scroll_cb);
	lv_anim_set_values(&a, 0, 76 * 60); lv_anim_set_time(&a, 3000); lv_anim_set_playback_time(&a, 3000);
	lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
	lv_anim_start(&a);
	lv_timer_handler(); metrics_reset(); run_for(12); metrics_print(scaled == 4 ? "scrollFlat" : scaled == 3 ? "scrollRAM" : scaled == 2 ? "scrollPS" : scaled ? "scrollSC" : "scroll");
}

static void tx_cb(void *o, int32_t v) { lv_obj_set_x(o, v); }
static int flat_bg;
static lv_image_dsc_t *ram_img[2];
static lv_obj_t *panel(int x, const char *cover, int cw) {
	lv_obj_t *p = lv_obj_create(lv_screen_active());
	lv_obj_set_pos(p, x, 0); lv_obj_set_size(p, 480, 800);
	lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_bg_color(p, lv_color_hex(0x30343a), 0);
	if (!flat_bg) { lv_obj_set_style_bg_grad_color(p, lv_color_hex(0x101214), 0); lv_obj_set_style_bg_grad_dir(p, LV_GRAD_DIR_VER, 0); }
	lv_obj_t *img = lv_image_create(p);
	char path[100]; snprintf(path, sizeof path, IMG "%s", cover);
	if (cw == 0) lv_image_set_src(img, ram_img[x ? 1 : 0]); else lv_image_set_src(img, path);
	if (cw != 440 && cw != 0) lv_image_set_scale(img, 256 * 440 / cw);
	lv_obj_set_size(img, 440, 440);
	lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 30);
	lv_obj_t *l = lv_label_create(p); lv_label_set_text(l, cover); lv_obj_set_style_text_font(l, font, 0); lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 500);
	return p;
}
static void scn_slide_ram(void) {
	clear();
	flat_bg = 1;
	ram_img[0] = load_cover("/data/mnt/r1os/dev/bench/img/cover1.jpg", 440, 440, NULL);
	ram_img[1] = load_cover("/data/mnt/r1os/dev/bench/img/cover2.jpg", 440, 440, NULL);
	lv_obj_t *a = panel(0, "ram", 0), *b = panel(480, "ram", 0);
	lv_anim_t an; lv_anim_init(&an);
	lv_anim_set_var(&an, a); lv_anim_set_exec_cb(&an, tx_cb); lv_anim_set_values(&an, 0, -480);
	lv_anim_set_time(&an, 400); lv_anim_set_playback_time(&an, 400); lv_anim_set_repeat_delay(&an, 200); lv_anim_set_playback_delay(&an, 200); lv_anim_set_repeat_count(&an, LV_ANIM_REPEAT_INFINITE);
	lv_anim_start(&an);
	lv_anim_set_var(&an, b); lv_anim_set_values(&an, 480, 0); lv_anim_start(&an);
	lv_timer_handler(); metrics_reset(); run_for(10); metrics_print("slideRAM");
}

static void scn_slide(int pre, int flat) {
	clear();
	flat_bg = flat;
	lv_obj_t *a = panel(0, pre ? "c1_440.jpg" : "cover1.jpg", pre ? 440 : 600), *b = panel(480, pre ? "c2_440.jpg" : "cover2.jpg", pre ? 440 : 600);
	lv_anim_t an; lv_anim_init(&an);
	lv_anim_set_var(&an, a); lv_anim_set_exec_cb(&an, tx_cb); lv_anim_set_values(&an, 0, -480);
	lv_anim_set_time(&an, 400); lv_anim_set_playback_time(&an, 400); lv_anim_set_repeat_delay(&an, 200); lv_anim_set_playback_delay(&an, 200); lv_anim_set_repeat_count(&an, LV_ANIM_REPEAT_INFINITE);
	lv_anim_start(&an);
	lv_anim_set_var(&an, b); lv_anim_set_values(&an, 480, 0); lv_anim_start(&an);
	lv_timer_handler(); metrics_reset(); run_for(10); metrics_print(pre ? (flat ? "slideFl" : "slidePS") : "slide");
}

static void op_cb(void *o, int32_t v) { lv_obj_set_style_opa(o, v, 0); }
static void scn_fx(void) {
	clear();
	lv_obj_t *p = panel(0, "cover1.jpg", 600);
	lv_obj_t *card = lv_obj_create(p);
	lv_obj_set_size(card, 400, 160); lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, -40);
	lv_obj_set_style_radius(card, 28, 0); lv_obj_set_style_bg_color(card, lv_color_hex(0xFFFFFF), 0); lv_obj_set_style_bg_opa(card, 160, 0);
	lv_obj_set_style_shadow_width(card, 40, 0); lv_obj_set_style_shadow_opa(card, 120, 0); lv_obj_set_style_shadow_offset_y(card, 12, 0);
	lv_obj_t *img = lv_obj_get_child(p, 0);
	lv_obj_set_style_radius(img, 40, 0); lv_obj_set_style_clip_corner(img, true, 0);
	lv_anim_t an; lv_anim_init(&an);
	lv_anim_set_var(&an, card); lv_anim_set_exec_cb(&an, op_cb); lv_anim_set_values(&an, 60, 255); lv_anim_set_time(&an, 700); lv_anim_set_playback_time(&an, 700); lv_anim_set_repeat_count(&an, LV_ANIM_REPEAT_INFINITE);
	lv_anim_start(&an);
	lv_timer_handler(); metrics_reset(); run_for(10); metrics_print("fx");
}

static void scn_idle(void) {
	clear();
	lv_obj_t *l = lv_label_create(lv_screen_active()); lv_label_set_text(l, "reposo"); lv_obj_center(l);
	lv_timer_handler(); metrics_reset(); run_for(5); metrics_print("idle");
}

int main(int argc, char **argv) {
	lv_init(); lv_tick_set_cb(tick_ms);
	lv_display_t *disp = lv_linux_fbdev_create();
	lv_linux_fbdev_set_file(disp, "/dev/fb0");
	lv_display_add_event_cb(disp, on_refr_start, LV_EVENT_REFR_START, NULL);
	lv_display_add_event_cb(disp, on_refr_ready, LV_EVENT_REFR_READY, NULL);
	int all = argc < 2;
	for (int i = 1; i < argc || (all && i == 1); i++) {
		const char *s = all ? "all" : argv[i];
		int a = !strcmp(s, "all");
		if (a || !strcmp(s, "idle")) scn_idle();
		if (a || !strcmp(s, "decode")) scn_decode();
		if (a || !strcmp(s, "scroll")) scn_scroll(0);
		if (a || !strcmp(s, "scrollsc")) scn_scroll(1);
		if (a || !strcmp(s, "slide")) scn_slide(0, 0);
		if (a || !strcmp(s, "slideps")) scn_slide(1, 0);
		if (a || !strcmp(s, "slidefl")) scn_slide(1, 1);
		if (a || !strcmp(s, "scrollps")) scn_scroll(2);
		if (a || !strcmp(s, "load")) scn_load();
		if (a || !strcmp(s, "scrollram")) scn_scroll(3);
		if (a || !strcmp(s, "scrollflat")) scn_scroll(4);
		if (a || !strcmp(s, "slideram")) scn_slide_ram();
		if (a || !strcmp(s, "fx")) scn_fx();
		if (all) break;
	}
	puts("FIN");
	return 0;
}
