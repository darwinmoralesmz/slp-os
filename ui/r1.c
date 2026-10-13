// Reproductor R1: interfaz táctil a pantalla completa (barra de estado, franja de reproducción, lista con arrastre,
// barra inferior de pestañas y panel rápido) en tres aspectos.
// Botones físicos: Vol+/Vol- (115/114), Encendido = play/pausa (116), Siguiente (163), Anterior (164).
#define _GNU_SOURCE
#include <dirent.h>
#include <linux/input.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <utime.h>
#include <time.h>
#include "audio.h"
#include "icons.h"
#include "lvgl.h"
#include "meta.h"
#include "power.h"
#include "vol_table.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "../third_party/stb_image.h"

extern lv_font_t ui_font_24, ui_font_26, ui_font_28, ui_font_30, ui_font_32, ui_font_38, ui_icons_26, ui_icons_30, ui_icons_34, ui_icons_46;

#ifdef R1_SIM
#define MUSIC_DIR "sim_music"
#define BT_DIR "sim_bt"
#define DATA_DIR "/tmp/r1sim"
#define OS_DIR "/tmp/r1sim"
#define CACHE_DIR "/tmp/r1sim/cache"
#else
#define MUSIC_DIR "/data/mnt/sd_0/music"
#define OS_DIR "/data/mnt/r1os"   // sistema: 2.ª partición de la SD (o sd_0/r1os si hay una sola); la música va aparte
#define BT_DIR OS_DIR "/bt"
#define DATA_DIR "/usr/data"
#define CACHE_DIR OS_DIR "/cache"
#endif
#define STATE_FILE DATA_DIR "/r1_state.txt"
#ifdef R1_SIM
#define FONT_DIR "/usr/share/fonts/"
#else
#define FONT_DIR OS_DIR "/fonts/"
#endif
#ifndef MAX_TRACKS
#define MAX_TRACKS 20000   // índices de pista en short
#endif
#define MAX_PLAYLISTS 64
#define MAX_PL_TRACKS 5000

#define WV_N 110   // barras de la forma de onda (4 px cada una = 440 px)
typedef struct { char *file; Meta m; int fav, album; long size, mtime; unsigned char wv[WV_N], wv_ok; } Track;   // wv_ok: 1 = calculada, 2 = falló
typedef struct { char name[128], artist[128]; int count, artist_i; } Album;
typedef struct { char name[128]; int albums, count; } Artist;
typedef struct { char name[96], rel[256]; int n, cap; short *tr; } Playlist;   // rel: archivo .m3u/.m3u8 dentro de music/
static Track *tracks;
static int ntracks, tcap;
static Album *albums;
static int nalbums;
static Artist *artists;
static int nartists;
static Playlist pls[MAX_PLAYLISTS];
static int npl;
static char m3u_files[MAX_PLAYLISTS][256];
static int nm3u;

// Estado compartido entre el hilo de audio y la interfaz.
static volatile int seek_req = -1, req_track = -1, cur_track = -1, paused, pos_s, total_s, vol = 20, shuf, rep;
static volatile int usb_hold;   // 1 = la partición de música se entrega al PC: ningún hilo abre archivos de ella
enum { US_IDLE, US_ASK, US_CHARGE, US_BUSY, US_DISK };
static volatile int usb_st, usb_conf;   // usb_conf: cable conectado a un PC (UDC "configured")
static void usb_now(void);
static int ctl_fd;

static void apply_volume(void) {
	for (int id = 1; id <= 2; id++) {
		struct snd_ctl_elem_value ev;
		memset(&ev, 0, sizeof ev);
		ev.id.numid = id;
		ev.value.integer.value[0] = vol_reg(vol);
		ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &ev);
	}
}
static void mute_and_exit(int sig) { vol = 0; apply_volume(); _exit(0); }

// ---- Cuarentena: si un archivo bloquea la aplicación (señal fatal), su ruta se anota en cache/bad.txt y no se vuelve a abrir ----
static __thread char cur_file[300];   // archivo que procesa este hilo (ruta relativa); vacío si ninguno
static void work_on(const char *f) { snprintf(cur_file, sizeof cur_file, "%s", f ? f : ""); }
static void crash_handler(int sig) {
	if (cur_file[0]) {
		int fd = open(CACHE_DIR "/bad.txt", O_WRONLY | O_APPEND | O_CREAT, 0644);
		if (fd >= 0) { if (write(fd, cur_file, strlen(cur_file)) > 0) (void)!write(fd, "\n", 1); close(fd); }
	}
	vol = 0; apply_volume();
	_exit(128 + sig);
}
static char *badlist;   // contenido de bad.txt rodeado de saltos de línea
static void bad_load(void) {
	FILE *f = fopen(CACHE_DIR "/bad.txt", "r");
	if (!f) return;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n > 0 && n < (1 << 20) && (badlist = malloc(n + 3))) {
		badlist[0] = '\n';
		n = fread(badlist + 1, 1, n, f);
		badlist[n + 1] = '\n'; badlist[n + 2] = 0;
	}
	fclose(f);
}
static int is_bad(const char *file) {
	char k[300];
	snprintf(k, sizeof k, "\n%s\n", file);
	return badlist && strstr(badlist, k);
}

// Salida Bluetooth: estado compartido.
static volatile int out_mode;   // salida elegida: 0 = jack, 1 = Bluetooth (aunque no esté conectado)
static volatile int jack_in = 1, jack_ev, sim_jack = 1;   // jack_in: hay algo en el conector de 3,5 mm; jack_ev: 1 = se quitó (en pausa), 2 = se puso
#define NO_OUT() (!out_mode && !jack_in)   // salida por jack elegida y nada conectado
// Avisos flotantes (conexiones, salida, biblioteca): cualquier hilo los deja en el buzón y la interfaz los dibuja (toast_show).
static char toast_ico[8], toast_title[64], toast_sub[96];
static volatile int toast_seq, toast_warn;
// Idioma de la interfaz (lang= en el estado; 0 español, 1 inglés). T() traduce un texto o plantilla de printf de la tabla de ui/i18n.h;
// lo que no está en la tabla (títulos, nombres) sale tal cual. Se aplica donde el texto llega a la pantalla (label, set_text, row_add,
// flash, avisos) y en las plantillas de snprintf.
#include "i18n.h"
static int lang;
static int i18n_cmp(const void *k, const void *e) { return strcmp(k, *(const char *const *)e); }
static const char *T(const char *s) {
	if (!lang || !s || !*s) return s;
	const char *const (*e)[2] = bsearch(s, I18N, sizeof I18N / sizeof *I18N, sizeof *I18N, i18n_cmp);
	return e ? (*e)[1] : s;
}
static void toast_post(const char *ico, const char *title, const char *sub, int warn) {
	snprintf(toast_ico, sizeof toast_ico, "%s", ico); snprintf(toast_title, sizeof toast_title, "%s", T(title)); snprintf(toast_sub, sizeof toast_sub, "%s", T(sub));
	toast_warn = warn;
	toast_seq++;
}
static char out_info[96];       // salida de la pista que suena, para la ficha técnica
static volatile unsigned start_ms;   // ms desde que se pidió la pista hasta su primer audio
static volatile int loading;    // 1 desde que se pide una pista hasta su primer audio
static volatile int bt_wait;    // 1 mientras se espera a que vuelva el enlace Bluetooth
static volatile int resume_at = -1, resume_track = -1;
static char bt_pcm[160];        // ruta bluealsa del A2DP conectado ("" si no hay)
static volatile int bt_dirty, bt_busy, bt_vol_pending;
static char bt_msg[48];
typedef struct { char mac[18], name[64]; int paired, connected; } BtDev;
static BtDev btd[16];
static int nbt;
static volatile int bt_on = 1;      // radio encendida (se recuerda en el estado: bton=)
static char bt_cur_mac[18];         // dispositivo con enlace A2DP
static volatile uint32_t bt_quiet;  // hasta este instante (ms) no se avisa de caídas de enlace: las provocó el usuario
static BtDev bt_arg;                // dispositivo sobre el que actúa el trabajo en curso
static void save_state(void);
static const char *bt_name_of(const char *mac) { for (int i = 0; i < nbt; i++) if (!strcmp(btd[i].mac, mac)) return btd[i].name; return T("Auriculares"); }

// ---- ADB (opción explícita; se recuerda para el siguiente arranque) ----
static volatile int adb_on;
static void *adb_job(void *arg) {
	int on = (long)arg;
	system(on ? OS_DIR "/usb_disk.sh adb" : "/etc/init.d/adb/S440adb stop; " OS_DIR "/usb_disk.sh prepare");
	FILE *f = fopen(DATA_DIR "/r1_adb", "w");
	if (f) { fputs(on ? "1\n" : "0\n", f); fclose(f); }
	adb_on = on; bt_dirty = 1;
	return NULL;
}
static void adb_toggle(void) {
	pthread_t t;
	pthread_create(&t, NULL, adb_job, (void *)(long)!adb_on);
	pthread_detach(t);
}

// ---- Wi-Fi a demanda y clima ----
// Con el clima activado (wifi= en el estado), cada 30 min se enciende el Wi-Fi con la red guardada, se pide el tiempo a Open-Meteo (con la
// ubicación aproximada por IP de ip-api.com, una vez por arranque) y se apaga. Nunca mientras el Bluetooth conecta o suena: comparten
// radio y antena. Ambos por HTTP plano: el wget del sistema no tiene TLS. Solo un adorno en la barra.
static volatile int wifi_on, wifi_up, wx_ok, wx_code, wx_day, wx_temp;
static volatile int wlan_on;
static char wx_city[48], wlan_ip[16];
static uint32_t tick_ms(void);
static int wget_text(const char *url, char *out, size_t n) {
	char cmd[300];
	snprintf(cmd, sizeof cmd, "wget -q -T 10 -O - '%s' 2>/dev/null", url);
	FILE *p = popen(cmd, "r");
	size_t k = p ? fread(out, 1, n - 1, p) : 0;
	if (p) pclose(p);
	out[k] = 0;
	return k > 0;
}
static int json_num(const char *j, const char *key, float *v) {   // primer "key": número tras el objeto "current" (o en todo el texto)
	char k[40];
	const char *c = strstr(j, "\"current\":{");
	snprintf(k, sizeof k, "\"%s\":", key);
	const char *a = strstr(c ? c : j, k);
	return a && sscanf(a + strlen(k), "%f", v) == 1;
}
// Símbolo del código WMO (Noto Sans Symbols lo dibuja); de noche, luna en lugar de sol.
static const char *wx_sym(int c, int day) {
	if (c == 0) return day ? "\xE2\x98\x80" : "\xE2\x98\xBE";            // ☀ ☾
	if (c <= 2) return day ? "\xE2\x9B\x85" : "\xE2\x98\xBE";            // ⛅
	if (c == 3) return "\xE2\x98\x81";                                      // ☁
	if (c == 45 || c == 48) return "\xF0\x9F\x8C\xAB";                      // 🌫
	if (c >= 71 && c <= 77) return "\xE2\x9D\x84";                          // ❄
	if (c == 85 || c == 86) return "\xE2\x9D\x84";
	if (c >= 95) return "\xE2\x9B\x88";                                      // ⛈
	return "\xE2\x98\x82";                                                  // ☂ llovizna y lluvia
}
static void *wx_thread(void *arg) {
	float lat = 0, lon = 0;
	int have_geo = 0;
	uint32_t next = 0;
	char buf[1500];
	for (;;) {
		sleep(5);
		if (!wifi_on || (int)(tick_ms() - next) < 0) continue;
		int keep = wlan_on;   // Wi-Fi encendido en Ajustes: se usa la conexión y no se apaga
		if (keep && !wlan_ip[0]) continue;
		if (bt_busy || (bt_pcm[0] && cur_track >= 0 && !paused)) continue;   // Bluetooth conectando o sonando: se espera (comparten antena)
		next = tick_ms() + 300000;   // reintento a los 5 min si algo falla
		wifi_up = 1; bt_dirty = 1;
		int up = keep || system(BT_DIR "/wifi.sh up") == 0;
		if (!up) { if (!wlan_on) system(BT_DIR "/wifi.sh down"); wifi_up = 0; bt_dirty = 1; continue; }
		if (!have_geo && wget_text("http://ip-api.com/line/?fields=status,city,lat,lon", buf, sizeof buf)) {
			char st[16], city[48];
			if (sscanf(buf, "%15s %47[^\n] %f %f", st, city, &lat, &lon) == 4 && !strcmp(st, "success")) { have_geo = 1; snprintf(wx_city, sizeof wx_city, "%s", city); }
		}
		char url[200];
		snprintf(url, sizeof url, "http://api.open-meteo.com/v1/forecast?latitude=%.3f&longitude=%.3f&current=temperature_2m,weather_code,is_day", lat, lon);
		float t, c, d;
		if (have_geo && wget_text(url, buf, sizeof buf) && json_num(buf, "temperature_2m", &t) && json_num(buf, "weather_code", &c) && json_num(buf, "is_day", &d)) {
			wx_temp = (int)lroundf(t); wx_code = (int)c; wx_day = d > 0.5f; wx_ok = 1; bt_dirty = 1;
			fprintf(stderr, "clima: %s %d °C, código %d, %s\n", wx_city, wx_temp, wx_code, wx_day ? "día" : "noche");
			next = tick_ms() + 1800000;
		}
		if (!keep && !wlan_on) system(BT_DIR "/wifi.sh down");
		wifi_up = 0; bt_dirty = 1;
	}
}
static void wx_text(char *b, size_t n) {   // "☁ 9°" o vacío
	if (wifi_on && wx_ok) snprintf(b, n, "%s %d°", wx_sym(wx_code, wx_day), wx_temp); else b[0] = 0;
}

// ---- Wi-Fi: redes y música por la red ----
// wlan= en el estado mantiene el Wi-Fi encendido (se levanta al arrancar). wlan_thread hace los trabajos de Ajustes > Wi-Fi (buscar, conectar,
// olvidar) con wpa_cli por medio de wifi.sh y cada 5 s lee el estado; sin IP reintenta cada 30 s. web= activa el servidor de webd.h mientras
// hay IP: subir y borrar música y editar listas desde el navegador (página en r1os/web/index.html).
#include "webd.h"
#ifdef R1_SIM
#define WEB_PORT 8080
#define WEB_PAGE "../sd/r1os/web/index.html"
#else
#define WEB_PORT 80
#define WEB_PAGE OS_DIR "/web/index.html"
#endif
typedef struct { char ssid[100]; int sig, id, sec, cur; } WNet;   // id = red guardada o -1; sig en dBm (0 = fuera de alcance)
static WNet wn[24];
static int nwn;
static volatile int web_on, wlan_busy, web_rescan;
static char wlan_ssid[100], wlan_msg[140];
static int wlan_rssi;
enum { WJ_NONE, WJ_SCAN, WJ_CONNECT, WJ_FORGET, WJ_POWER };
static volatile int wj_op;
static int wj_arg;
static WNet wj_net;
static char wj_psk[64];
static int wcli(char *out, size_t n, const char *args) {
	char cmd[400], tmp[64];
	if (!out) { out = tmp; n = sizeof tmp; }
	snprintf(cmd, sizeof cmd, BT_DIR "/wifi.sh cli %s 2>/dev/null", args);
	FILE *p = popen(cmd, "r");
	size_t k = p ? fread(out, 1, n - 1, p) : 0;
	if (p) pclose(p);
	out[k] = 0;
	return k;
}
static void ssid_raw(const char *s, char *o, size_t n) {   // deshace el escape de wpa_cli (\xNN, \\, \")
	size_t k = 0;
	for (; *s && k + 1 < n; s++) {
		if (*s == '\\' && s[1] == 'x' && hexv(s[2]) >= 0 && hexv(s[3]) >= 0) { o[k++] = hexv(s[2]) * 16 + hexv(s[3]); s += 3; }
		else if (*s == '\\' && s[1]) { s++; o[k++] = *s == 'n' ? '\n' : *s == 't' ? '\t' : *s == 'r' ? '\r' : *s == 'e' ? 27 : *s; }
		else o[k++] = *s;
	}
	o[k] = 0;
}
static int kv(const char *b, const char *k, char *o, size_t n) {   // valor de "k" al inicio de una línea
	size_t l = strlen(k);
	for (const char *s = b; s; ) {
		if (!strncmp(s, k, l)) { size_t m = strcspn(s + l, "\n"); if (m >= n) m = n - 1; memcpy(o, s + l, m); o[m] = 0; return 1; }
		s = strchr(s, '\n');
		if (s) s++;
	}
	o[0] = 0;
	return 0;
}
static void wlan_status(void) {
	char b[1024], v[100], ip[16] = "", ss[100] = "";
	wcli(b, sizeof b, "status");
	if (strstr(b, "wpa_state=COMPLETED")) {
		kv(b, "ip_address=", ip, sizeof ip);
		if (kv(b, "ssid=", v, sizeof v)) ssid_raw(v, ss, sizeof ss);
		wcli(b, sizeof b, "signal_poll");
		if (kv(b, "RSSI=", v, sizeof v)) wlan_rssi = atoi(v);
	}
	if (strcmp(ip, wlan_ip) || strcmp(ss, wlan_ssid)) { snprintf(wlan_ip, sizeof wlan_ip, "%s", ip); snprintf(wlan_ssid, sizeof wlan_ssid, "%s", ss); bt_dirty = 1; }
}
static int wnet_cmp(const void *a, const void *b) {
	const WNet *x = a, *y = b;
	int kx = x->cur * 1000 + (x->sig ? 200 + x->sig : x->id >= 0 ? 50 : 0), ky = y->cur * 1000 + (y->sig ? 200 + y->sig : y->id >= 0 ? 50 : 0);
	return ky - kx;
}
// Redes guardadas (list_networks) y visibles (scan_results), sin repetir nombres; la actual primero y luego por señal.
static void wlan_list(int scan) {
	static char b[8192];
	WNet t[24];
	int n = 0;
	if (scan) { wcli(NULL, 0, "scan"); sleep(4); }
	wcli(b, sizeof b, "list_networks");
	char *sv, *l = strtok_r(b, "\n", &sv);   // la primera línea es la cabecera
	while ((l = strtok_r(NULL, "\n", &sv)) && n < 24) {
		char *f[4] = {0};
		for (int k = 0; k < 4; k++) f[k] = strsep(&l, "\t");
		if (!f[1] || !f[1][0]) continue;
		memset(&t[n], 0, sizeof *t);
		ssid_raw(f[1], t[n].ssid, sizeof t[n].ssid);
		t[n].id = atoi(f[0]); t[n].sec = 1; t[n].cur = f[3] && strstr(f[3], "[CURRENT]");
		n++;
	}
	wcli(b, sizeof b, "scan_results");
	l = strtok_r(b, "\n", &sv);
	while ((l = strtok_r(NULL, "\n", &sv))) {
		char *f[5] = {0}, ss[100];
		for (int k = 0; k < 5; k++) f[k] = strsep(&l, "\t");
		if (!f[4] || !f[4][0]) continue;   // oculta
		ssid_raw(f[4], ss, sizeof ss);
		int k = 0, sig = atoi(f[2]);
		while (k < n && strcmp(t[k].ssid, ss)) k++;
		if (k == n) { if (n == 24) continue; memset(&t[n], 0, sizeof *t); snprintf(t[n].ssid, sizeof t[n].ssid, "%s", ss); t[n].id = -1; n++; }
		if (!t[k].sig || sig > t[k].sig) t[k].sig = sig < 0 ? sig : -1;
		t[k].sec = strstr(f[3], "WPA") || strstr(f[3], "RSN") || strstr(f[3], "SAE") || strstr(f[3], "WEP");
	}
	qsort(t, n, sizeof *t, wnet_cmp);
	memcpy(wn, t, n * sizeof *t);
	nwn = n; bt_dirty = 1;
}
static void wlan_connect(WNet w, const char *psk) {
	char a[400], b[32], hex[201];
	int id = w.id, fresh = id < 0;
	snprintf(wlan_msg, sizeof wlan_msg, T("Conectando a %s..."), w.ssid); bt_dirty = 1;
	if (fresh) {
		wcli(b, sizeof b, "add_network");
		if (b[0] < '0' || b[0] > '9') { toast_post(ICO_WIFI, "No se pudo conectar", w.ssid, 1); return; }
		id = atoi(b);
		for (int k = 0; w.ssid[k] && k < 100; k++) sprintf(hex + 2 * k, "%02x", (unsigned char)w.ssid[k]);   // en hex: sin problemas de comillas
		snprintf(a, sizeof a, "set_network %d ssid %s", id, hex); wcli(NULL, 0, a);
		if (w.sec) {   // psk entre comillas dobles para wpa_cli y simples para el shell
			int k = snprintf(a, sizeof a, "set_network %d psk '\"", id);
			for (const char *c = psk; *c && k < (int)sizeof a - 8; c++) k += *c == '\'' ? snprintf(a + k, sizeof a - k, "'\\''") : snprintf(a + k, sizeof a - k, "%c", *c);
			snprintf(a + k, sizeof a - k, "\"'");
		} else snprintf(a, sizeof a, "set_network %d key_mgmt NONE", id);
		wcli(NULL, 0, a);
	}
	snprintf(a, sizeof a, "select_network %d", id); wcli(NULL, 0, a);   // desactiva las demás hasta enable_network all
	int ok = system(BT_DIR "/wifi.sh renew") == 0;
	if (!ok && fresh) { snprintf(a, sizeof a, "remove_network %d", id); wcli(NULL, 0, a); }
	wcli(NULL, 0, "enable_network all");
	wcli(NULL, 0, "save_config");
	toast_post(ICO_WIFI, ok ? "Wi-Fi conectado" : "No se pudo conectar", ok ? w.ssid : fresh && w.sec ? "Revisa la contraseña" : w.ssid, !ok);
}
static int web_active(void) { return web_last && tick_ms() - web_last < 60000; }   // tráfico en el último minuto
static int bt_audio(void) { return bt_pcm[0] && out_mode && cur_track >= 0 && !paused; }   // suena por Bluetooth
static void *wlan_thread(void *arg) {
	uint32_t poll_t = 0, up_t = 0;
#ifndef R1_SIM
	for (int i = 0; i < 60 && wlan_on && access("/sys/class/net/wlan0", F_OK); i++) sleep(1);   // al arrancar, boot.sh carga el driver
#endif
	for (;;) {
		int op = wj_op;
		if (op) {
			char a[40];
			switch (op) {
			case WJ_POWER:
				wlan_on = wj_arg; save_state();
				if (wj_arg && bt_pcm[0]) toast_post(ICO_WIFI, "Wi-Fi y Bluetooth comparten antena", "Transferir mientras suena puede cortar el audio", 0);
				if (!wj_arg) { system(BT_DIR "/wifi.sh down"); wlan_ip[0] = wlan_ssid[0] = 0; nwn = 0; }
				up_t = 0;
				break;
			case WJ_SCAN: snprintf(wlan_msg, sizeof wlan_msg, "%s", T("Buscando...")); bt_dirty = 1; wlan_list(1); break;
			case WJ_CONNECT: wlan_connect(wj_net, wj_psk); memset(wj_psk, 0, sizeof wj_psk); wlan_status(); break;
			case WJ_FORGET:
				snprintf(a, sizeof a, "remove_network %d", wj_net.id); wcli(NULL, 0, a);
				wcli(NULL, 0, "save_config");
				toast_post(ICO_WIFI, "Red olvidada", wj_net.ssid, 0);
				break;
			}
			wlan_msg[0] = 0;
			if (wlan_on) { wlan_status(); wlan_list(0); }
			wj_op = 0; wlan_busy = 0; bt_dirty = 1;
		}
		if (wlan_on && (int)(tick_ms() - poll_t) >= 0) {
			poll_t = tick_ms() + 5000;
			wlan_status();
			if (!wlan_ip[0] && (int)(tick_ms() - up_t) >= 0 && !bt_busy && !bt_audio()) {   // reintentos: no mientras el Bluetooth conecta o suena
				up_t = tick_ms() + 30000;
				snprintf(wlan_msg, sizeof wlan_msg, "%s", T("Conectando...")); bt_dirty = 1;
				system(BT_DIR "/wifi.sh up");
				wlan_msg[0] = 0;
				wlan_status(); wlan_list(0);
			}
		}
		usleep(300000);
	}
}
static void wlan_job(int op, const WNet *w, int arg, const char *psk) {
	if (wlan_busy) { toast_post(ICO_WIFI, "Wi-Fi ocupado", "Espera a que termine", 1); return; }
	wlan_busy = 1;
	if (w) wj_net = *w;
	snprintf(wj_psk, sizeof wj_psk, "%s", psk ? psk : "");
	wj_arg = arg; wj_op = op;
}
static void *web_thread(void *arg) {
	for (;;) if (web_on && wlan_on && wlan_ip[0]) web_serve(WEB_PORT, MUSIC_DIR, WEB_PAGE, &web_on); else sleep(1);
}

static void json_str(FILE *o, const char *s) {
	fputc('"', o);
	for (; *s; s++) { unsigned char c = *s; if (c == '"' || c == '\\') fprintf(o, "\\%c", c); else if (c < 32) fprintf(o, "\\u%04x", c); else fputc(c, o); }
	fputc('"', o);
}
// /api/lib: resumen (cantidad, carpetas de primer nivel, listas, espacio, idioma, auriculares Bluetooth). /api/tracks?q=&o=&n=: pistas en orden
// de carpeta que contienen todas las palabras de q (sin mayúsculas ni acentos), desde o y como máximo n (tope WEB_PAGE_MAX): la biblioteca
// entera nunca viaja de una vez.
enum { WEB_PAGE_MAX = 2000 };
static int lib_cut;
static void fold(char *d, size_t n, const char *s);
static void web_track(FILE *o, const Track *t) {
	fputc('[', o);
	json_str(o, t->file); fputc(',', o); json_str(o, t->m.title); fputc(',', o); json_str(o, t->m.artist); fputc(',', o); json_str(o, t->m.album);
	fprintf(o, ",%d]", t->m.dur);
}
static void web_api_get(const char *url, FILE *o) {
	if (!strncmp(url, "/api/lib", 8)) {
		struct statvfs v;
		fprintf(o, "{\"count\":%d,\"max\":%d,\"cut\":%d,\"maxList\":%d,\"pageMax\":%d,\"dirs\":[", ntracks, MAX_TRACKS, lib_cut, MAX_PL_TRACKS, WEB_PAGE_MAX);
		const char *prev = "";
		size_t pl = 0;
		for (int i = 0, nd = 0; i < ntracks && nd < 300; i++) {   // tracks está ordenado por ruta: las carpetas salen seguidas
			const char *f = tracks[i].file, *sl = strchr(f, '/');
			if (!sl || ((size_t)(sl - f) == pl && !strncmp(f, prev, pl))) continue;
			char d[256];
			snprintf(d, sizeof d, "%.*s", (int)(sl - f), f);
			fputs(nd++ ? "," : "", o); json_str(o, d);
			prev = f; pl = sl - f;
		}
		fputs("],\"lists\":[", o);
		for (int k = 0; k < npl; k++) { fputs(k ? ",[" : "[", o); json_str(o, pls[k].rel); fputc(',', o); json_str(o, pls[k].name); fprintf(o, ",%d]", pls[k].n); }
		fprintf(o, "],\"free\":%lld,\"lang\":\"%s\",\"bt\":%d}", statvfs(MUSIC_DIR, &v) ? -1LL : (long long)v.f_bavail * v.f_bsize, lang ? "en" : "es", bt_pcm[0] != 0);
		return;
	}
	char q[200], a[16], w[8][64], raw[700], f[700], *sv, *tok;
	int nw = 0, off = web_arg(url, "o", a, sizeof a) ? atoi(a) : 0, n = web_arg(url, "n", a, sizeof a) ? atoi(a) : 200, total = 0, k = 0;
	if (off < 0) off = 0;
	if (n < 1 || n > WEB_PAGE_MAX) n = WEB_PAGE_MAX;
	web_arg(url, "q", raw, sizeof raw);
	fold(q, sizeof q, raw);
	for (tok = strtok_r(q, " ", &sv); tok && nw < 8; tok = strtok_r(NULL, " ", &sv)) snprintf(w[nw++], sizeof w[0], "%s", tok);
	fputs("{\"tracks\":[", o);
	for (int i = 0; i < ntracks; i++) {
		const Track *t = &tracks[i];
		if (nw) {
			snprintf(raw, sizeof raw, "%s\1%s\1%s\1%s", t->m.title, t->m.artist, t->m.album, t->file);
			fold(f, sizeof f, raw);
			int ok = 1;
			for (int j = 0; j < nw && ok; j++) ok = strstr(f, w[j]) != NULL;
			if (!ok) continue;
		}
		if (total++ < off || k >= n) continue;
		if (k++) fputc(',', o);
		web_track(o, t);
	}
	fprintf(o, "],\"total\":%d,\"o\":%d}", total, off);
}
static void web_api_rescan(void) { web_rescan = 1; }


static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }
static uint32_t tick_ms(void) { return now_us() / 1000; }
static void set_volume(int v) { vol = v < 0 ? 0 : v > 100 ? 100 : v; apply_volume(); bt_vol_pending = 1; }

// Pista siguiente según aleatorio/repetir; -1 si se acaba la lista.
// ---- Cola de reproducción ----
// qa = orden de reproducción; qb = orden original del contexto (para quitar el aleatorio). Se modifican bajo qlock:
// el hilo de audio avanza al terminar cada pista. Al agotarse, "cont" decide qué suena después.
#define MAX_Q (2 * MAX_TRACKS)
enum { CONT_STOP, CONT_ALBUM, CONT_RANDOM };
static const char *CONT_N[] = {"Detener", "Siguiente álbum", "Aleatorio"};
static short qa[MAX_Q], qb[MAX_Q];
static int qn, qbn, qpos = -1, cont;
static pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;
static int album_cmp(const void *a, const void *b);

static void q_shuffle(short *a, int n) { for (int i = n - 1; i > 0; i--) { int j = rand() % (i + 1); short t = a[i]; a[i] = a[j]; a[j] = t; } }
// Descarta lo ya reproducido para hacer sitio a n pistas.
static void q_room(int n) {
	int over = qn + n - MAX_Q;
	if (over > qpos) over = qpos;
	if (over <= 0) return;
	memmove(qa, qa + over, (qn - over) * sizeof qa[0]);
	qn -= over; qpos -= over;
}
static void q_add_l(int t, int next) {
	if (qn >= MAX_Q) q_room(1);
	if (qn >= MAX_Q) return;
	int at = next && qpos >= 0 ? qpos + 1 : qn, bat = qbn;
	if (qbn < MAX_Q) {
		if (next && qpos >= 0) for (int i = 0; i < qbn; i++) if (qb[i] == qa[qpos]) { bat = i + 1; break; }
		memmove(qb + bat + 1, qb + bat, (qbn - bat) * sizeof qb[0]); qb[bat] = t; qbn++;
	}
	memmove(qa + at + 1, qa + at, (qn - at) * sizeof qa[0]); qa[at] = t; qn++;
}
static void q_remove_l(int pos) {
	if (pos <= qpos || pos >= qn) return;
	int t = qa[pos];
	memmove(qa + pos, qa + pos + 1, (qn - pos - 1) * sizeof qa[0]); qn--;
	for (int i = 0; i < qbn; i++) if (qb[i] == t) { memmove(qb + i, qb + i + 1, (qbn - i - 1) * sizeof qb[0]); qbn--; break; }
}
static void q_append_l(const int *l, int n) {
	if (qn + n > MAX_Q) q_room(n);
	if (qn + n > MAX_Q) n = MAX_Q - qn;
	int from = qn;
	for (int i = 0; i < n; i++) { qa[qn++] = l[i]; if (qbn < MAX_Q) qb[qbn++] = l[i]; }
	if (shuf) q_shuffle(qa + from, n);
}
// Cola agotada: añade el álbum siguiente al último de la cola, o 20 pistas al azar.
static void q_more_l(void) {
	int *idx = malloc(MAX_TRACKS * sizeof *idx), n = 0;
	if (!idx) return;
	if (cont == CONT_ALBUM && nalbums) {
		int a = (tracks[qa[qn - 1]].album + 1) % nalbums;
		for (int i = 0; i < ntracks; i++) if (tracks[i].album == a) idx[n++] = i;
		qsort(idx, n, sizeof idx[0], album_cmp);
	} else if (cont == CONT_RANDOM && ntracks) {
		int want = ntracks > 20 ? 20 : ntracks > 1 ? ntracks - 1 : 1;
		while (n < want) {
			int t = rand() % ntracks, dup = ntracks > 1 && t == qa[qn - 1];
			for (int i = 0; i < n; i++) dup |= idx[i] == t;
			if (!dup) idx[n++] = t;
		}
	}
	if (n) q_append_l(idx, n);
	free(idx);
}
// Empieza una cola nueva con la lista indicada (mezclada si "shuf") y devuelve la pista que debe sonar.
static int q_set(const short *l, int n, int start) {
	pthread_mutex_lock(&qlock);
	if (n > MAX_Q) n = MAX_Q;
	if (start >= n) start = 0;
	memcpy(qa, l, n * sizeof qa[0]); memcpy(qb, l, n * sizeof qb[0]);
	qn = qbn = n; qpos = start;
	if (shuf && n > 1) { short t = qa[0]; qa[0] = qa[start]; qa[start] = t; q_shuffle(qa + 1, n - 1); qpos = 0; }
	int r = n ? qa[qpos] : -1;
	pthread_mutex_unlock(&qlock);
	return r;
}
// Siguiente pista, o -1 para detener. at_end: terminó sola (respeta repetir pista y la detención); si no, fue un salto manual.
// peek: solo consulta la que sonará, sin avanzar la cola (-1 si no se sabe).
static int q_next(int at_end, int peek) {
	int r = -1;
	pthread_mutex_lock(&qlock);
	if (qn > 0 && qpos >= 0) {
		if (at_end && rep == 2) r = qa[qpos];
		else {
			if (qpos + 1 >= qn && rep != 1) q_more_l();
			if (qpos + 1 < qn) r = qa[peek ? qpos + 1 : ++qpos];
			else if (peek) r = rep == 1 && !shuf ? qa[0] : -1;
			else if (rep == 1 || !at_end) { if (shuf && rep == 1) q_shuffle(qa, qn); qpos = 0; r = qa[0]; }
		}
	}
	pthread_mutex_unlock(&qlock);
	return r;
}
static int q_prev(void) {
	int r = -1;
	pthread_mutex_lock(&qlock);
	if (qn > 0) { if (qpos > 0) qpos--; else if (rep == 1) qpos = qn - 1; else qpos = 0; r = qa[qpos]; }
	pthread_mutex_unlock(&qlock);
	return r;
}
static int q_jump(int pos) {
	int r = -1;
	pthread_mutex_lock(&qlock);
	if (pos >= 0 && pos < qn) { qpos = pos; r = qa[pos]; }
	pthread_mutex_unlock(&qlock);
	return r;
}
// Tras cambiar "shuf": mezcla lo que falta por sonar o restaura el orden original del contexto.
static void q_reorder(void) {
	pthread_mutex_lock(&qlock);
	if (qn > 0 && qpos >= 0) {
		if (shuf) q_shuffle(qa + qpos + 1, qn - qpos - 1);
		else {
			int t = qa[qpos];
			memcpy(qa, qb, qbn * sizeof qa[0]); qn = qbn; qpos = 0;
			for (int i = 0; i < qn; i++) if (qa[i] == t) { qpos = i; break; }
		}
	}
	pthread_mutex_unlock(&qlock);
}

// La tubería Bluetooth se mantiene abierta entre pistas (abrirla y cerrarla en cada pista retrasaba el inicio).
static Out po;
static int po_open;
static char po_pcm[160];
static uint32_t po_used;   // último instante con audio real en la tubería
static void bt_out_close(void) { if (po_open) { out_close(&po); po_open = 0; } }
// Deja la tubería lista para la pista d (la reabre si cambió el dispositivo o se cerró por inactividad); 0 si queda abierta.
static int bt_out_open(const Dec *d) {
	if (po_open && strcmp(po_pcm, bt_pcm)) bt_out_close();
	if (po_open) { out_bt_track(&po, d); return 0; }
	if (out_open_bt(&po, d, bt_pcm)) return -1;
	snprintf(po_pcm, sizeof po_pcm, "%s", bt_pcm); po_open = 1; po_used = tick_ms();
	return 0;
}
// En pausa o sin pista el relleno mantiene a bluealsa en hora; pasados 10 s se cierra (al reabrir, bluealsa reinicia su reloj).
static void bt_out_idle(void) { if (po_open && tick_ms() - po_used > 10000) bt_out_close(); }

static void set_out_info(int mode, const Dec *d, const Out *jack) {
	if (mode) snprintf(out_info, sizeof out_info, T("Bluetooth · 48 kHz%s"), d->rate != 48000 ? T(" (remuestreado)") : "");
	else snprintf(out_info, sizeof out_info, T("Jack 3.5 mm · %s · %.4g kHz%s%s"), jack->bits24 ? "24 bit" : "16 bit", d->rate * jack->up / 1000.0, jack->up > 1 ? T(" (sobremuestreo)") : "", d->bits > 16 && !jack->bits24 ? T("\nDe 24 a 16 bit") : "");
}

// Apertura anticipada de la pista siguiente en un hilo aparte: abrir un M4A cuesta 200-500 ms en el R1 y el búfer del DAC solo aguanta ~90 ms.
typedef struct { Dec d; char path[640]; int track, active, ok; pthread_t th; } Pre;
static Pre pre;
static void *pre_open_fn(void *a) { Pre *p = a; work_on(tracks[p->track].file); p->ok = dec_open(&p->d, p->path) == 0; work_on(NULL); return NULL; }
static void pre_start(int track) {
	pre.track = track; pre.ok = 0;
	snprintf(pre.path, sizeof pre.path, MUSIC_DIR "/%s", tracks[track].file);
	pre.active = pthread_create(&pre.th, NULL, pre_open_fn, &pre) == 0;
}
static void pre_wait(void) { if (pre.active) { pthread_join(pre.th, NULL); pre.active = 0; } }
static void pre_discard(void) { pre_wait(); if (pre.ok) { dec_close(&pre.d); pre.ok = 0; } }

// Reproducción sin cortes: ~5 s antes del final abre la pista siguiente y, si el formato de salida coincide (siempre en Bluetooth;
// en el jack, misma frecuencia y profundidad), sigue escribiendo en la misma salida sin cerrarla.
static void *audio_thread(void *arg) {
	static int32_t buf[PCM_PERIOD * 2];
	setpriority(PRIO_PROCESS, syscall(SYS_gettid), -10);   // la interfaz no debe dejar sin datos a la salida
	pthread_t pad;
	pthread_create(&pad, NULL, bt_pad_thread, NULL);   // hereda la prioridad
	for (;;) {
		if (usb_hold) { loading = 0; usleep(50000); continue; }
		int t = __atomic_exchange_n(&req_track, -1, __ATOMIC_SEQ_CST);
		if (t < 0) { loading = 0; work_on(NULL); bt_out_idle(); usleep(20000); continue; }
		loading = 1;
		uint32_t t0 = tick_ms();
		cur_track = t; pos_s = 0; total_s = 0;
		work_on(tracks[t].file);
#ifdef R1_SIM
		if (getenv("R1_CRASH") && strstr(tracks[t].file, getenv("R1_CRASH"))) raise(SIGSEGV);   // prueba de la cuarentena
#endif
		// Salida Bluetooth elegida pero sin enlace: esperar a que vuelva (o a que se cambie de salida o de pista).
		bt_wait = 0;
		while (out_mode && !bt_pcm[0] && req_track < 0) { bt_wait = 1; usleep(100000); }
		bt_wait = 0;
		if (req_track >= 0) continue;
		char path[640];
		snprintf(path, sizeof path, MUSIC_DIR "/%s", tracks[t].file);
		Dec d;
		Out jack, *o = &jack;
		int mode = out_mode && bt_pcm[0];
		if (dec_open(&d, path)) { fprintf(stderr, "audio: no se pudo abrir %s\n", tracks[t].file); req_track = q_next(1, 0); usleep(200000); continue; }
		uint32_t t1 = tick_ms();
		int bad;
		if (mode) { o = &po; bad = bt_out_open(&d); }
		else { bt_out_close(); bad = out_open(&jack, &d); }
		if (bad) { fprintf(stderr, "audio: salida no disponible para %s (%u Hz)\n", tracks[t].file, d.rate); dec_close(&d); req_track = q_next(1, 0); usleep(200000); continue; }
		uint32_t t2 = tick_ms();
		if (!mode) apply_volume();
		set_out_info(mode, &d, &jack);
		total_s = d.total / d.rate;
		unsigned long long done = 0;
		size_t n;
		int ended = 0, failed = 0, first = 1, pre_tried = 0, next_pick = -1;
		// Vigilancia de velocidad: avance de la pista frente al reloj (sano = 1,00; fuera de 0,96-1,04 se registra).
		uint32_t sp_t0 = 0, t_first = 0;
		unsigned long long sp_d0 = 0;

		seek_req = resume_at >= 0 && resume_track == t ? resume_at : -1;
		resume_at = -1;
		while (req_track < 0 && !usb_hold && (out_mode && bt_pcm[0] ? 1 : 0) == mode) {
			int sk = seek_req;
			if (sk >= 0) {
				seek_req = -1;
				if (!dec_seek(&d, (unsigned long long)sk * d.rate)) {
					done = (unsigned long long)sk * d.rate; pos_s = sk; sp_t0 = 0; t_first = tick_ms();
					if (!mode) { ioctl(o->fd, SNDRV_PCM_IOCTL_DROP); ioctl(o->fd, SNDRV_PCM_IOCTL_PREPARE); }
				} else {   // tras un salto fallido el decodificador queda en un estado inválido: se descarta la pista
					fprintf(stderr, "audio: salto fallido en %s, se omite\n", tracks[t].file);
					next_pick = q_next(1, 0); ended = 1; break;
				}
			}
			if (paused) { sp_t0 = 0; t_first = 0; if (mode) bt_out_idle(); usleep(20000); continue; }
			if (!(n = dec_read(&d, buf, PCM_PERIOD))) {
				int nx = req_track < 0 ? q_next(1, 0) : -1;
				if (nx >= 0) pre_wait();
				if (nx >= 0 && pre.ok && pre.track == nx && (mode || (pre.d.rate == d.rate && (pre.d.bits > 16) == (d.bits > 16)))) {
					dec_close(&d); d = pre.d; pre.ok = 0; pre_tried = 0;
					t = nx; cur_track = nx; pos_s = 0; done = 0; total_s = d.total / d.rate; o->ch = d.ch; work_on(tracks[t].file); sp_t0 = 0; t_first = tick_ms();
					if (mode) out_bt_track(o, &d);
					set_out_info(mode, &d, &jack);
					fprintf(stderr, "audio: pista %d encadenada\n", t);
					continue;
				}
				next_pick = nx; ended = 1; break;
			}
			if (mode && !po_open && bt_out_open(&d)) { failed = 1; break; }
			int u = out_write(o, buf, n);
			if (u < 0) { failed = 1; break; }
			if (mode) po_used = tick_ms();
			if (first) { first = 0; loading = 0; start_ms = tick_ms() - t0; t_first = tick_ms(); fprintf(stderr, "audio: pista %d, %u Hz %uch %s up=%d, abrir %u ms, salida %u ms, primer audio %u ms\n", t, d.rate, d.ch, mode ? "bt" : "jack", mode ? 0 : jack.up, t1 - t0, t2 - t1, tick_ms() - t2); }
			done += n;
			pos_s = done / d.rate;
			{
				uint32_t nowt = tick_ms();
				if (!t_first) t_first = nowt;
				if (!sp_t0 && nowt - t_first > 2500) { sp_t0 = nowt; sp_d0 = done; }
				else if (sp_t0 && nowt - sp_t0 >= 4000) {
					double r = (double)(done - sp_d0) / d.rate * 1000.0 / (nowt - sp_t0);
					if (r > 1.04 || r < 0.96) fprintf(stderr, "audio: VELOCIDAD %.2fx en pista %d (%u Hz %uch %s up=%d, formato %s)\n", r, t, d.rate, d.ch, mode ? "bt" : "jack", mode ? 0 : jack.up, tracks[t].m.fmt);
					static unsigned padded;
					if (bt_padded != padded) { fprintf(stderr, "bt: relleno de silencio, %u ms en total\n", bt_padded / (out_bt_rate / 1000)); padded = bt_padded; }
					sp_t0 = nowt; sp_d0 = done;
				}
			}
			if (!pre_tried && d.total && req_track < 0 && done + 5ULL * d.rate >= d.total) {
				int nt = q_next(1, 1);
				pre_tried = 1;
				if (nt >= 0) pre_start(nt);
			}
		}
		pre_discard();
		if (mode) { if (failed || !out_mode || !bt_pcm[0]) bt_out_close(); }
		else {
			if (!ended) ioctl(o->fd, SNDRV_PCM_IOCTL_DROP); else ioctl(o->fd, SNDRV_PCM_IOCTL_DRAIN);
			out_close(o);
		}
		dec_close(&d);
		if (usb_hold && !ended) { paused = 1; continue; }   // se cierra el archivo; la app se reinicia al volver del modo disco
		if (req_track < 0) {
			if (ended) { if (next_pick >= 0) req_track = next_pick; else paused = 1; }
			else { resume_at = pos_s; resume_track = t; if (failed) usleep(300000); req_track = t; }
		}
	}
}

// Conector de 3,5 mm: /sys/class/switch/headset/state (0 = vacío, otro valor = auriculares o auriculares con micrófono).
// Se filtra el rebote del contacto (el estado debe mantenerse 300 ms). Al quitarlos se pausa la música.
static int jack_read(void) {
#ifdef R1_SIM
	return sim_jack;
#else
	FILE *f = fopen("/sys/class/switch/headset/state", "r");
	int v = 1;   // si no se puede leer se supone conectado: no bloquear la reproducción
	if (f) { if (fscanf(f, "%d", &v) != 1) v = 1; fclose(f); }
	return v != 0;
#endif
}
static void *jack_watch(void *arg) {
	int cand = jack_read(), n = 0;
	jack_in = cand;
	for (;;) {
		usleep(100000);
		int v = jack_read();
		if (v != jack_in) {
			if (v == cand) n++; else { cand = v; n = 1; }
			if (n < 3) continue;
			jack_in = v;
			fprintf(stderr, "jack: %s\n", v ? "conectado" : "desconectado");
			if (!v && !out_mode && cur_track >= 0 && !paused) paused = 1;
			jack_ev = v ? 2 : 1;
		} else cand = v, n = 0;
	}
}

// ---- Salida Bluetooth ----
// ¿Hay un enlace ACL activo con ese dispositivo? (hcitool con: barato y exacto; bluealsa mantiene el PCM aunque el enlace caiga.)
// acl recibe las MAC con enlace (separadas por espacios).
static int acl_up(const char *mac, char *acl, size_t n) {
	FILE *p = popen("hcitool con 2>/dev/null", "r");
	char line[200];
	int up = 0;
	acl[0] = 0;
	while (p && fgets(line, sizeof line, p)) {
		const char *a = strstr(line, "ACL ");
		if (!a) continue;
		size_t l = strlen(acl);
		if (l + 19 < n) snprintf(acl + l, n - l, "%.17s ", a + 4);
		if (mac[0] && !strncasecmp(a + 4, mac, 17)) up = 1;
	}
	if (p) pclose(p);
	return up;
}

// bluealsa se cuelga si el enlace cae mientras suena: sus órdenes no vuelven nunca (to.sh las corta con 137); también puede faltar. Se reinicia y se
// reconecta el último dispositivo; matar "bluealsa-cli open" libera al hilo de audio si estaba bloqueado escribiendo.
static int bt_hung(int st) {
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 137) return 0;
	static uint32_t last;
	fprintf(stderr, "bt: bluealsa no responde, se reinicia\n");
	if (!last || tick_ms() - last > 60000) toast_post(ICO_BT, "Reiniciando Bluetooth", "El servicio de audio dejó de responder", 1);
	last = tick_ms();
	system(BT_DIR "/bt_heal.sh");
	return 1;
}
// Códec A2DP: el bluealsa de HiBy ofrece SBC, AAC, aptX, aptX-HD, LDAC y UAT; se usan los que el auricular admita ("Available codecs").
// Preferencia en el estado (btcodec=; vacío = automático: el mejor de la lista BT_BEST). Se aplica al tomar la PCM, con bluealsa-cli codec.
static char bt_codecs[80], bt_codec[16], bt_codec_pref[16];
// Opciones con advertencia (Ajustes > Bluetooth): calidad LDAC (bt_env.sh la pasa a bluealsa al arrancarlo) y ahorro del enlace (sniff).
static int ldac_q, sniff_m;
static const char *LDAC_N[] = {"Auto", "990k", "660k", "330k"};
static const char *LDAC_S[] = {"Se adapta a la señal", "Máxima: puede cortarse", "Puede cortarse lejos", "La más estable"};
static const char *SNIFF_N[] = {"Auto", "Siempre", "Nunca"};
static const char *SNIFF_S[] = {"Recomendado", "Menos zumbido; corta", "Zumbido en reposo"};   // Auto: sniff solo en reposo
static volatile int bt_recodec, bt_codec_msg;
static const char *const BT_BEST[] = {"LDAC", "aptX-HD", "aptX", "AAC", "SBC"};
static int has_word(const char *list, const char *w) {
	size_t n = strlen(w);
	for (const char *p = list; (p = strstr(p, w)); p += n) if ((p == list || p[-1] == ' ') && (!p[n] || p[n] == ' ')) return 1;
	return 0;
}
static void bt_read_info(const char *pcm) {
	char q[220], l[120];
	snprintf(q, sizeof q, BT_DIR "/to.sh 4 bluealsa-cli info %s 2>/dev/null", pcm);
	FILE *p = popen(q, "r");
	while (p && fgets(l, sizeof l, p)) {
		int hz;
		l[strcspn(l, "\n")] = 0;
		if (sscanf(l, " Sampling: %d Hz", &hz) == 1 && hz >= 8000 && hz <= 96000) out_bt_rate = hz;
		else if (!strncmp(l, "Available codecs: ", 18)) snprintf(bt_codecs, sizeof bt_codecs, "%s", l + 18);
		else if (!strncmp(l, "Selected codec: ", 16)) snprintf(bt_codec, sizeof bt_codec, "%s", l + 16);
	}
	if (p) pclose(p);
}
static void bt_apply_codec(const char *pcm) {
	const char *want = bt_codec_pref[0] && has_word(bt_codecs, bt_codec_pref) ? bt_codec_pref : NULL;
	for (unsigned i = 0; !want && i < sizeof BT_BEST / sizeof *BT_BEST; i++) if (has_word(bt_codecs, BT_BEST[i])) want = BT_BEST[i];
	if (!want || !strcmp(want, bt_codec)) return;
	char q[260];
	snprintf(q, sizeof q, BT_DIR "/to.sh 10 bluealsa-cli codec %s %s >/dev/null 2>&1", pcm, want);
	system(q);
	bt_read_info(pcm);
	fprintf(stderr, "bt: códec %s (pedido %s)\n", bt_codec, want);
}
// Cada 2 s: si hay enlace nuevo busca su PCM A2DP en bluealsa y selecciona la salida Bluetooth; si el enlace cae
// conserva la elección (la música espera) y a los 45 s sin enlace pasa al jack en pausa.
static void *bt_watch(void *arg) {
	char mac[18] = "";
	uint32_t lost_since = 0;
	for (;;) {
		char acl[120];
		int up = acl_up(mac, acl, sizeof acl);
		uint32_t now = tick_ms();
		if (bt_pcm[0] && !up) {
			fprintf(stderr, "bt: enlace caído (%s)\n", mac);
			int quiet = (int)(now - bt_quiet) < 0;
			bt_pcm[0] = 0; mac[0] = 0; bt_cur_mac[0] = 0; lost_since = quiet ? 0 : now; bt_dirty = 1;
			if (!quiet) toast_post(ICO_BT, "Bluetooth desconectado", cur_track >= 0 && !paused ? "La música espera la reconexión" : "Buscando de nuevo el dispositivo", 1);
		} else if (!bt_pcm[0] && acl[0] && !bt_recodec) {
			char cur[160] = "", line[200], last[24] = "";
			FILE *lf = fopen(DATA_DIR "/bt_last", "r");
			if (lf) { if (fgets(last, sizeof last, lf)) last[strcspn(last, "\n")] = 0; fclose(lf); }
			FILE *p = popen(BT_DIR "/to.sh 4 bluealsa-cli list-pcms 2>/dev/null", "r");
			while (p && fgets(line, sizeof line, p)) {   // la PCM de un dispositivo con enlace (bluealsa conserva las de enlaces caídos); primero el último elegido
				const char *dv = strstr(line, "dev_");
				if (!dv || !strstr(line, "a2dpsrc/sink")) continue;
				char m[18];
				snprintf(m, sizeof m, "%.17s", dv + 4);
				for (char *c = m; *c; c++) if (*c == '_') *c = ':';
				if (!strcasestr(acl, m) || (cur[0] && strcasecmp(m, last))) continue;
				line[strcspn(line, "\n")] = 0; snprintf(cur, sizeof cur, "%s", line);
			}
			if (p && bt_hung(pclose(p))) { sleep(2); continue; }
			const char *d = strstr(cur, "dev_");
			if (d) {
				snprintf(mac, sizeof mac, "%.17s", d + 4);
				for (char *c = mac; *c; c++) if (*c == '_') *c = ':';
				snprintf(bt_cur_mac, sizeof bt_cur_mac, "%s", mac);
				bt_read_info(cur);   // frecuencia con que bluealsa espera los datos y códecs; luego el códec preferido
				bt_apply_codec(cur);
				fprintf(stderr, "bt: PCM A2DP a %d Hz, %s (admite %s)\n", out_bt_rate, bt_codec, bt_codecs);
				strcpy(bt_pcm, cur);
				fprintf(stderr, "bt: enlace nuevo %s\n", mac);
				out_mode = 1; lost_since = 0; bt_dirty = 1; bt_vol_pending = 1;
				if (bt_codec_msg) { char m[48], a[100]; bt_codec_msg = 0; snprintf(m, sizeof m, T("Códec: %s"), bt_codec); snprintf(a, sizeof a, T("Admite: %s"), bt_codecs); toast_post(ICO_BT, m, a, 0); }
				else toast_post(ICO_BT, "Bluetooth conectado", "La música sale por Bluetooth", 0);
				FILE *f = fopen(DATA_DIR "/bt_last", "w");
				if (f) { fprintf(f, "%s\n", mac); fclose(f); }
			}
		}
		{   // sniff (bajo consumo del enlace) solo sin audio: transmitiendo corta el sonido; en reposo evita el zumbido del equipo (PROBLEMAS #34)
			static int sniff = -1;
			int want = sniff_m == 1 ? 1 : sniff_m == 2 ? 0 : !po_open;
			if (want != sniff) {
				FILE *f = fopen("/sys/kernel/debug/bluetooth/hci0/idle_timeout", "w");
				if (f) { fputs(want ? "2000" : "0", f); fclose(f); sniff = want; fprintf(stderr, "bt: sniff %s\n", want ? "activado" : "desactivado"); }
			}
		}
		static uint32_t probe;
#ifndef R1_SIM
		if (bt_on && !bt_busy && now - probe > 10000 && now > 60000) {   // tras el arranque de la pila (bt_start.sh tarda ~20 s)   // cada 10 s: bluealsa y el agente vivos y respondiendo (si no, 137 y se reinician)
			probe = now;
			FILE *p = popen("pidof bt-agent >/dev/null && " BT_DIR "/to.sh 4 bluealsa-cli list-pcms 2>/dev/null || exit 137", "r");
			char line[200], extra[18] = "";
			while (p && fgets(line, sizeof line, p)) {   // otro dispositivo de audio con enlace mientras suena el elegido: se desconecta (se repartirían la radio)
				const char *dv = strstr(line, "dev_");
				if (!dv || !strstr(line, "a2dpsrc/sink") || !bt_pcm[0]) continue;
				char m[18];
				snprintf(m, sizeof m, "%.17s", dv + 4);
				for (char *c = m; *c; c++) if (*c == '_') *c = ':';
				if (strcasestr(acl, m) && strcasecmp(m, bt_cur_mac)) snprintf(extra, sizeof extra, "%s", m);
			}
			if (p && bt_hung(pclose(p))) { bt_pcm[0] = 0; mac[0] = 0; bt_cur_mac[0] = 0; bt_dirty = 1; }
			else if (extra[0]) {
				char q[120];
				fprintf(stderr, "bt: se desconecta %s (ya suena %s)\n", extra, bt_cur_mac);
				snprintf(q, sizeof q, BT_DIR "/to.sh 8 bluetoothctl disconnect %s >/dev/null 2>&1", extra);
				system(q);
			}
		}
#endif
		if (!bt_pcm[0] && out_mode && lost_since && cur_track >= 0 && !paused && now - lost_since > 45000) { out_mode = 0; paused = 1; lost_since = 0; bt_dirty = 1; toast_post(ICO_HEADPHONES, "Sin Bluetooth", "Salida por jack, en pausa", 1); }
		sleep(2);
	}
}

// Reintenta conectar el último dispositivo: cada 8 s el primer minuto y luego cada 30 s.
static void *bt_reconnect(void *arg) {
	uint32_t t0 = tick_ms();
	for (;;) {
		sleep(8);
		if (bt_pcm[0]) { t0 = tick_ms(); continue; }
		uint32_t el = tick_ms() - t0;
		static uint32_t last_try;
		if (bt_busy || !bt_on || access(DATA_DIR "/bt_last", R_OK)) continue;
		if (el > 60000 && tick_ms() - last_try < 30000) continue;
		last_try = tick_ms();
		system(BT_DIR "/bt_quick.sh");
	}
}

static void bt_load_list(void) {
	FILE *p = popen(BT_DIR "/bt_list.sh", "r");
	BtDev tmp[16];
	int n = 0;
	char line[200];
	while (p && n < 16 && fgets(line, sizeof line, p)) {
		char *f[4], *c;
		int k = 0;
		line[strcspn(line, "\n")] = 0;
		f[k++] = line;
		for (c = line; *c && k < 4; c++) if (*c == '|') { *c = 0; f[k++] = c + 1; }
		if (k < 4) continue;
		snprintf(tmp[n].mac, sizeof tmp[n].mac, "%s", f[0]);
		snprintf(tmp[n].name, sizeof tmp[n].name, "%s", f[3]);
		tmp[n].paired = atoi(f[1]); tmp[n].connected = atoi(f[2]);
		n++;
	}
	if (p) pclose(p);
	memcpy(btd, tmp, sizeof tmp);
	nbt = n; bt_dirty = 1;
}

enum { BJ_LIST, BJ_SCAN, BJ_CONNECT, BJ_DISCONNECT, BJ_FORGET, BJ_POWER, BJ_QUICK, BJ_CODEC, BJ_HEAL };
// Trabajos Bluetooth en un hilo (los scripts bloquean): lista, búsqueda, conectar/emparejar, desconectar, olvidar, encender o apagar, y el
// atajo del panel (reconecta el último dispositivo o desconecta el actual). d = dispositivo; arg = 1/0 para BJ_POWER.
static int bt_op, bt_arg_i;
static void bt_user_down(void) {   // el usuario cortó la salida Bluetooth: no queda salida, se pausa
	bt_quiet = tick_ms() + 8000;
	out_mode = 0;
	if (cur_track >= 0 && !paused) paused = 1;
}
static void *bt_job(void *unused) {
	int op = bt_op, arg = bt_arg_i;
	BtDev d = bt_arg;
	char cmd[200];
	bt_busy = 1;
	switch (op) {
	case BJ_SCAN: strcpy(bt_msg, "Buscando..."); bt_dirty = 1; system(BT_DIR "/bt_scan.sh"); break;
	case BJ_CONNECT:
		snprintf(bt_msg, sizeof bt_msg, d.paired ? T("Conectando...") : T("Emparejando...")); bt_dirty = 1;
		snprintf(cmd, sizeof cmd, BT_DIR "/bt_connect.sh %s", d.mac);
		int rc = system(cmd);
		if (rc && WEXITSTATUS(rc) == 2) toast_post(ICO_BT, "No se encontró el dispositivo", "Ponlo en modo emparejamiento y reintenta", 1);
		else if (rc) toast_post(ICO_BT, d.paired ? "No se pudo conectar" : "No se pudo emparejar", d.name, 1);
		break;
	case BJ_QUICK:   // atajo del panel: conectado = desconectar; si no, reconectar el último (encendiendo la radio si hace falta)
		if (!bt_pcm[0]) {
			if (access(DATA_DIR "/bt_last", R_OK)) { toast_post(ICO_BT, "Sin dispositivo guardado", "Empareja uno en Ajustes > Bluetooth", 1); break; }
			if (!bt_on) { bt_on = 1; save_state(); system(BT_DIR "/bt_power.sh on"); }
			toast_post(ICO_BT, "Conectando...", "Buscando el último dispositivo", 0);
			system(BT_DIR "/bt_quick.sh");
			for (int i = 0; i < 12 && !bt_pcm[0]; i++) sleep(1);
			if (!bt_pcm[0]) toast_post(ICO_BT, "No se encontró el dispositivo", "Enciéndelo y acércalo", 1);
			break;
		}
		snprintf(d.mac, sizeof d.mac, "%s", bt_cur_mac); snprintf(d.name, sizeof d.name, "%s", bt_name_of(bt_cur_mac));
		// fall through
	case BJ_DISCONNECT:
		strcpy(bt_msg, "Desconectando..."); bt_dirty = 1;
		if (!strcmp(d.mac, bt_cur_mac)) bt_user_down();
		snprintf(cmd, sizeof cmd, BT_DIR "/bt_disconnect.sh %s", d.mac);
		system(cmd);
		toast_post(ICO_BT, "Desconectado", d.name, 0);
		break;
	case BJ_FORGET:
		strcpy(bt_msg, "Olvidando..."); bt_dirty = 1;
		if (!strcmp(d.mac, bt_cur_mac)) bt_user_down();
		snprintf(cmd, sizeof cmd, BT_DIR "/bt_forget.sh %s", d.mac);
		system(cmd);
		toast_post(ICO_BT, "Dispositivo olvidado", d.name, 0);
		break;
	case BJ_HEAL:
		strcpy(bt_msg, "Reiniciando..."); bt_dirty = 1;
		bt_recodec = 1; bt_codec_msg = 1; bt_pcm[0] = 0;
		sleep(1);
		system(BT_DIR "/bt_heal.sh");
		bt_recodec = 0;
		for (int i = 0; i < 15 && !bt_pcm[0]; i++) sleep(1);
		break;
	case BJ_CODEC:   // suelta la PCM: el audio cierra su tubería y bt_watch la vuelve a tomar aplicando el códec preferido
		if (!bt_pcm[0]) break;
		bt_recodec = 1; bt_codec_msg = 1; bt_pcm[0] = 0; bt_dirty = 1;
		sleep(1);
		bt_recodec = 0;
		for (int i = 0; i < 12 && !bt_pcm[0]; i++) sleep(1);
		break;
	case BJ_POWER:
		bt_on = arg; save_state();
		strcpy(bt_msg, arg ? "Encendiendo..." : "Apagando..."); bt_dirty = 1;
		if (!arg && bt_cur_mac[0]) bt_user_down();
		system(arg ? BT_DIR "/bt_power.sh on" : BT_DIR "/bt_power.sh off");
		toast_post(ICO_BT, arg ? "Bluetooth encendido" : "Bluetooth apagado", arg ? "Se reconectará solo al último dispositivo" : "Sin conexiones inalámbricas", 0);
		if (arg) system(BT_DIR "/bt_quick.sh");
		break;
	}
	bt_msg[0] = 0;
	bt_load_list();
	bt_busy = 0;
	return NULL;
}
static void bt_start_job(int op, const BtDev *d, int arg) {
	if (bt_busy) return;
	bt_busy = 1;
	bt_op = op; bt_arg_i = arg;
	memset(&bt_arg, 0, sizeof bt_arg);
	if (d) bt_arg = *d;
	pthread_t t;
	pthread_create(&t, NULL, bt_job, NULL);
	pthread_detach(t);
}

// Volumen del enlace A2DP (0..127), con curva cuadrática; como máximo cada 150 ms (hilo propio: system() bloquea).
static void *bt_volume_thread(void *arg) {
	for (;;) {
		usleep(150000);
		if (!bt_vol_pending || !out_mode || !bt_pcm[0]) continue;
		bt_vol_pending = 0;
		char cmd[300];
		snprintf(cmd, sizeof cmd, BT_DIR "/to.sh 4 bluealsa-cli volume %s %d >/dev/null 2>&1 &", bt_pcm, (int)(127.0f * vol / 100.0f * vol / 100.0f + 0.5f));
		system(cmd);
	}
}

static int cmp_track(const void *a, const void *b) { return strcmp(((const Track *)a)->file, ((const Track *)b)->file); }

static int ext_is(const char *name, const char *ext) { const char *x = strrchr(name, '.'); return x && !strcasecmp(x, ext); }
static int is_audio(const char *name) {
	static const char *const E[] = {".flac", ".mp3", ".m4a", ".ogg", ".oga", ".opus", ".wav"};
	for (unsigned i = 0; i < sizeof E / sizeof *E; i++) if (ext_is(name, E[i])) return 1;
	return 0;
}

static void *grow(void *p, size_t n) {
	p = realloc(p, n);
	if (!p) { fprintf(stderr, "sin memoria\n"); abort(); }
	return p;
}

// Firma de la biblioteca (cantidad, tamaños y fechas de pistas y listas): si cambia, hay música nueva o quitada.
typedef struct { long n, size, mtime; } Sig;
static Sig lib_sig;
static volatile int lib_dirty;   // 1 = el disco cambió respecto a lo cargado, 2 = ya se avisó
static int lib_new, lib_upd, lib_gone, lib_cut, lib_unsup;   // lib_cut: pistas que no caben; lib_unsup: archivos que no se pueden reproducir
static char lib_msg[80];   // aviso del arranque (se muestra en la barra de estado)

// Progreso del primer indexado: scan_ui (la pantalla) se llama como mucho cada 200 ms y solo si el escaneo pasa de 400 ms.
static void (*scan_ui)(const char *);
static uint64_t scan_t0, scan_last;
static void scan_tick(const char *what, int a, int b) {
	char m[80];
	uint64_t t = now_us();
	if (!scan_ui || t - scan_t0 < 400000 || t - scan_last < 200000) return;
	scan_last = t;
	if (b) snprintf(m, sizeof m, "%s\n%d / %d", what, a, b); else snprintf(m, sizeof m, "%s\n%d", what, a);
	scan_ui(m);
}

// Recorre MUSIC_DIR y sus subcarpetas (hasta 5 niveles). Con collect acumula las pistas (FLAC, MP3, M4A, Ogg, Opus, WAV) y las listas .m3u/.m3u8;
// siempre suma la firma.
static void scan_dir(const char *rel, int depth, int collect, Sig *sg) {
	char dpath[320];
	snprintf(dpath, sizeof dpath, MUSIC_DIR "%s%s", rel[0] ? "/" : "", rel);
	DIR *dir = opendir(dpath);
	struct dirent *e;
	while (dir && (e = readdir(dir))) {
		if (e->d_name[0] == '.') continue;
		char r[256], full[600];
		if (snprintf(r, sizeof r, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name) >= (int)sizeof r) continue;   // ruta demasiado larga para el caché
		snprintf(full, sizeof full, MUSIC_DIR "/%s", r);
		struct stat st;
		if (stat(full, &st)) continue;
		if (S_ISDIR(st.st_mode)) { if (depth < 5) scan_dir(r, depth + 1, collect, sg); continue; }
		int audio = is_audio(e->d_name), list = ext_is(e->d_name, ".m3u") || ext_is(e->d_name, ".m3u8");
		if (!audio && !list) continue;
		sg->n++; sg->size += st.st_size; sg->mtime += st.st_mtime;
		if (collect && !(sg->n & 63)) scan_tick("Buscando música", sg->n, 0);
		if (!collect) continue;
		if (audio && is_bad(r)) continue;
		if (audio && ntracks >= MAX_TRACKS) lib_cut++;
		else if (audio) {
			if (ntracks == tcap) { tcap = tcap ? tcap * 2 : 256; tracks = grow(tracks, tcap * sizeof *tracks); }
			Track *t = &tracks[ntracks++];
			memset(t, 0, sizeof *t);
			t->file = strdup(r); t->size = st.st_size; t->mtime = st.st_mtime;
			if (!t->file) abort();
		}
		else if (list && nm3u < MAX_PLAYLISTS) snprintf(m3u_files[nm3u++], sizeof m3u_files[0], "%s", r);
	}
	if (dir) closedir(dir);
}

// Artista de agrupación: el artista del álbum si es coherente con el de la pista (igual, contenido o "Varios artistas");
// si no (p. ej. el nombre del usuario que descargó los archivos), el de la pista.
static const char *track_artist(const Track *t) {
	const char *aa = t->m.albumartist, *ar = t->m.artist;
	if (!aa[0]) return ar;
	if (!ar[0] || strcasestr(ar, aa) || strcasestr(aa, ar)) return aa;
	if (!strncasecmp(aa, "various", 7) || !strncasecmp(aa, "varios", 6) || !strcasecmp(aa, "va") || !strcasecmp(aa, "v.a.")) return aa;
	return ar;
}

static uint32_t fnv2(const char *a, const char *b) {
	uint32_t h = 2166136261u;
	for (; *a; a++) h = (h ^ (uint8_t)*a) * 16777619u;
	h = (h ^ 0xff) * 16777619u;
	for (; *b; b++) h = (h ^ (uint8_t)*b) * 16777619u;
	return h;
}

// Agrupa pistas en álbumes (álbum + artista) y álbumes en artistas, por orden de aparición, con tablas hash: O(pistas).
static void build_indexes(void) {
	int hn = 16, acap = 0, rcap = 0;
	while (hn < 2 * ntracks) hn *= 2;
	int *ha = malloc(hn * sizeof *ha), *hr = malloc(hn * sizeof *hr);
	if (!ha || !hr) { free(ha); free(hr); return; }
	memset(ha, -1, hn * sizeof *ha); memset(hr, -1, hn * sizeof *hr);
	for (int i = 0; i < ntracks; i++) {
		const char *al = tracks[i].m.album[0] ? tracks[i].m.album : "Sin álbum", *ar = track_artist(&tracks[i]);
		uint32_t h = fnv2(al, ar) & (hn - 1);
		int a;
		while ((a = ha[h]) >= 0 && (strcmp(albums[a].name, al) || strcmp(albums[a].artist, ar))) h = (h + 1) & (hn - 1);
		if (a < 0) {
			if (nalbums == acap) { acap = acap ? acap * 2 : 64; albums = grow(albums, acap * sizeof *albums); }
			a = ha[h] = nalbums++;
			snprintf(albums[a].name, sizeof albums[a].name, "%s", al); snprintf(albums[a].artist, sizeof albums[a].artist, "%s", ar);
			albums[a].count = 0;
			uint32_t g = fnv2(ar, "") & (hn - 1);
			int k;
			while ((k = hr[g]) >= 0 && strcmp(artists[k].name, ar)) g = (g + 1) & (hn - 1);
			if (k < 0) {
				if (nartists == rcap) { rcap = rcap ? rcap * 2 : 64; artists = grow(artists, rcap * sizeof *artists); }
				k = hr[g] = nartists++;
				snprintf(artists[k].name, sizeof artists[k].name, "%s", ar);
				artists[k].albums = artists[k].count = 0;
			}
			albums[a].artist_i = k; artists[k].albums++;
		}
		tracks[i].album = a; albums[a].count++; artists[albums[a].artist_i].count++;
	}
	free(ha); free(hr);
}

// Índice de la pista con esa ruta (las pistas están ordenadas por ruta), o -1.
static int track_by_file(const char *f) {
	int lo = 0, hi = ntracks - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2, c = strcmp(tracks[mid].file, f);
		if (!c) return mid;
		if (c < 0) lo = mid + 1; else hi = mid - 1;
	}
	return -1;
}

// Resuelve "./", "../" y "\" (listas de Windows) en una ruta relativa a music/.
static void path_norm(char *p) {
	char out[400], *seg, *sv;
	int o = 0;
	for (char *c = p; *c; c++) if (*c == '\\') *c = '/';
	out[0] = 0;
	for (seg = strtok_r(p, "/", &sv); seg; seg = strtok_r(NULL, "/", &sv)) {
		if (!strcmp(seg, ".")) continue;
		if (!strcmp(seg, "..")) { while (o > 0 && out[o - 1] != '/') o--; if (o > 0) o--; out[o] = 0; continue; }
		o += snprintf(out + o, sizeof out - o, "%s%s", o ? "/" : "", seg);
		if (o >= (int)sizeof out) { o = sizeof out - 1; break; }
	}
	strcpy(p, out);
}
static void pl_add(Playlist *p, int ti) {
	if (p->n >= MAX_PL_TRACKS) return;
	if (p->n == p->cap) { p->cap = p->cap ? p->cap * 2 : 64; p->tr = grow(p->tr, p->cap * sizeof *p->tr); }
	p->tr[p->n++] = ti;
}
static void parse_m3u(const char *rel) {
	char path[320], dir[256], line[400];
	snprintf(path, sizeof path, MUSIC_DIR "/%s", rel);
	snprintf(dir, sizeof dir, "%s", rel);
	char *sl = strrchr(dir, '/');
	if (sl) *sl = 0; else dir[0] = 0;
	FILE *f = fopen(path, "r");
	if (!f || npl >= MAX_PLAYLISTS) { if (f) fclose(f); return; }
	Playlist *p = &pls[npl++];
	memset(p, 0, sizeof *p);
	snprintf(p->rel, sizeof p->rel, "%s", rel);
	snprintf(p->name, sizeof p->name, "%s", strrchr(rel, '/') ? strrchr(rel, '/') + 1 : rel);
	char *dot = strrchr(p->name, '.');
	if (dot && (!strcasecmp(dot, ".m3u") || !strcasecmp(dot, ".m3u8"))) *dot = 0;
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (!strncmp(line, "\xEF\xBB\xBF", 3)) memmove(line, line + 3, strlen(line + 3) + 1);   // BOM de UTF-8
		if (!strncmp(line, "#PLAYLIST:", 10)) snprintf(p->name, sizeof p->name, "%s", line + 10);
		if (!line[0] || line[0] == '#') continue;
		char full[400];
		snprintf(full, sizeof full, "%s%s%s", line[0] == '/' ? "" : dir, dir[0] && line[0] != '/' ? "/" : "", line);
		path_norm(full);
		int ti = track_by_file(full);
		if (ti >= 0) pl_add(p, ti);
	}
	fclose(f);
}

// Cierra un archivo escrito como tmp y lo pone en su sitio solo si se escribió entero.
static void commit_tmp(FILE *f, const char *tmp, const char *path) {
	int ok = !ferror(f);
	if (fclose(f)) ok = 0;
	if (ok) rename(tmp, path); else remove(tmp);
}

// ---- Caché de la biblioteca (SD): etiquetas ya leídas por archivo, válidas mientras no cambien tamaño ni fecha ----
// Al arrancar solo se leen las etiquetas de las pistas nuevas o modificadas; el resto sale del archivo. El archivo está ordenado
// por ruta igual que las pistas, así que se recorre en paralelo sin cargarlo entero en RAM.
typedef struct { char file[256]; long size, mtime; Meta m; } CRec;
typedef struct { char magic[4]; unsigned ver, rsz, n; } CHdr;
#define CACHE_VER 3
typedef struct { FILE *f; unsigned n; CRec cur; int have; } CIter;
static void cache_open(CIter *c) {
	CHdr h;
	memset(c, 0, sizeof *c);
	c->f = fopen(CACHE_DIR "/library.bin", "rb");
	if (!c->f) return;
	if (fread(&h, sizeof h, 1, c->f) == 1 && !memcmp(h.magic, "R1LB", 4) && h.ver == CACHE_VER && h.rsz == sizeof(CRec)) c->n = h.n;
	else { fclose(c->f); c->f = NULL; }
}
// Registro del archivo indicado (o NULL); descarta los anteriores en orden alfabético.
static CRec *cache_find(CIter *c, const char *file) {
	while (c->f) {
		if (!c->have) {
			if (fread(&c->cur, sizeof c->cur, 1, c->f) != 1) { fclose(c->f); c->f = NULL; break; }
			c->cur.file[sizeof c->cur.file - 1] = 0;
			c->have = 1;
		}
		int d = strcmp(c->cur.file, file);
		if (d == 0) return &c->cur;
		if (d > 0) return NULL;
		c->have = 0;
	}
	return NULL;
}
// Guarda las n primeras pistas (ya leídas).
static void cache_save(int n) {
	CHdr h = {{'R', '1', 'L', 'B'}, CACHE_VER, sizeof(CRec), n};
	mkdir(CACHE_DIR, 0755);
	FILE *f = fopen(CACHE_DIR "/library.bin.tmp", "wb");
	if (!f) return;
	fwrite(&h, sizeof h, 1, f);
	for (int i = 0; i < n; i++) {
		CRec r;
		memset(&r, 0, sizeof r);
		snprintf(r.file, sizeof r.file, "%s", tracks[i].file);
		r.size = tracks[i].size; r.mtime = tracks[i].mtime; r.m = tracks[i].m;
		fwrite(&r, sizeof r, 1, f);
	}
	commit_tmp(f, CACHE_DIR "/library.bin.tmp", CACHE_DIR "/library.bin");
}

// ---- Forma de onda (caché en la SD): picos por tramo de cada pista, calculados en segundo plano ----
typedef struct { char file[256]; long size, mtime; unsigned char wv[WV_N]; } WRec;
static void wave_load(void) {
	FILE *f = fopen(CACHE_DIR "/wave.bin", "rb");
	CHdr h;
	if (!f) return;
	if (fread(&h, sizeof h, 1, f) == 1 && !memcmp(h.magic, "R1WV", 4) && h.ver == CACHE_VER && h.rsz == sizeof(WRec)) {
		WRec r;
		while (fread(&r, sizeof r, 1, f) == 1) {
			int ti = track_by_file(r.file);
			if (ti >= 0 && tracks[ti].size == r.size && tracks[ti].mtime == r.mtime) { memcpy(tracks[ti].wv, r.wv, WV_N); tracks[ti].wv_ok = 1; }
		}
	}
	fclose(f);
}
static void wave_save(void) {
	int n = 0;
	for (int i = 0; i < ntracks; i++) n += tracks[i].wv_ok == 1;
	CHdr h = {{'R', '1', 'W', 'V'}, CACHE_VER, sizeof(WRec), n};
	mkdir(CACHE_DIR, 0755);
	FILE *f = fopen(CACHE_DIR "/wave.bin.tmp", "wb");
	if (!f) return;
	fwrite(&h, sizeof h, 1, f);
	for (int i = 0; i < ntracks; i++) {
		if (tracks[i].wv_ok != 1) continue;
		WRec r;
		memset(&r, 0, sizeof r);
		snprintf(r.file, sizeof r.file, "%s", tracks[i].file);
		r.size = tracks[i].size; r.mtime = tracks[i].mtime; memcpy(r.wv, tracks[i].wv, WV_N);
		fwrite(&r, sizeof r, 1, f);
	}
	commit_tmp(f, CACHE_DIR "/wave.bin.tmp", CACHE_DIR "/wave.bin");
}

// Firma de la partición de música: bloques libres y fecha de la carpeta. Si no cambió desde el último recorrido, la biblioteca
// sale entera del caché sin leer directorios (leer un directorio FAT en frío cuesta ~3 ms por archivo). Con el sistema y el caché
// en otra partición, solo cambia cuando cambia la música. "Actualizar" (R1_FULLSCAN) siempre recorre.
static unsigned long long vol_sig(void) {
	struct statvfs v;
	struct stat st;
	if (statvfs(MUSIC_DIR, &v) || stat(MUSIC_DIR, &st)) return 0;
	return (unsigned long long)v.f_bfree << 32 ^ (unsigned long long)st.st_mtime ^ 1;
}
static unsigned long long boot_vsig;
static int fast_load(void) {
	FILE *f = fopen(CACHE_DIR "/fast.txt", "r");
	char l[300];
	unsigned long long sg = 0;
	int ok = f && fscanf(f, "sig=%llu n=%ld size=%ld mtime=%ld\n", &sg, &lib_sig.n, &lib_sig.size, &lib_sig.mtime) == 4 && sg == boot_vsig && sg;
	while (ok && fgets(l, sizeof l, f) && nm3u < MAX_PLAYLISTS) { l[strcspn(l, "\n")] = 0; if (!strncmp(l, "m3u=", 4)) snprintf(m3u_files[nm3u++], sizeof m3u_files[0], "%s", l + 4); }
	if (f) fclose(f);
	if (!ok) { nm3u = 0; lib_sig = (Sig){0, 0, 0}; return 0; }
	CIter ci;
	cache_open(&ci);
	CRec r;
	for (unsigned i = 0; ci.f && i < ci.n && ntracks < MAX_TRACKS && fread(&r, sizeof r, 1, ci.f) == 1; i++) {
		if (ntracks == tcap) { tcap = tcap ? tcap * 2 : 256; tracks = grow(tracks, tcap * sizeof *tracks); }
		Track *t = &tracks[ntracks++];
		memset(t, 0, sizeof *t);
		r.file[sizeof r.file - 1] = 0;
		t->file = strdup(r.file); t->size = r.size; t->mtime = r.mtime; t->m = r.m;
		if (!t->file) abort();
	}
	if (ci.f) fclose(ci.f);
	return ntracks > 0 || !lib_sig.n;
}
static void fast_save(void) {
	FILE *f = fopen(CACHE_DIR "/fast.txt.tmp", "w");
	if (!f) return;
	fprintf(f, "sig=%llu n=%ld size=%ld mtime=%ld\n", boot_vsig, lib_sig.n, lib_sig.size, lib_sig.mtime);
	for (int i = 0; i < nm3u; i++) fprintf(f, "m3u=%s\n", m3u_files[i]);
	commit_tmp(f, CACHE_DIR "/fast.txt.tmp", CACHE_DIR "/fast.txt");
}
static void scan_music(void) {
	uint64_t t0 = now_us(), t1, t2, t3;
	CIter ci;
	scan_t0 = t0;
	boot_vsig = vol_sig();
	if (!getenv("R1_FULLSCAN") && fast_load()) {
		t1 = now_us();
		int k = 0;
		for (int i = 0; i < ntracks; i++) { if (tracks[i].m.bad) { free(tracks[i].file); lib_unsup++; continue; } tracks[k++] = tracks[i]; }
		ntracks = k;
		wave_load();
		build_indexes();
		for (int i = 0; i < nm3u; i++) parse_m3u(m3u_files[i]);
		fprintf(stderr, "biblioteca: sin cambios en la partición, cargada del caché en %.0f ms (%d pistas)\n", (now_us() - t1) / 1e3, ntracks);
		return;
	}
	unsetenv("R1_FULLSCAN");
	cache_open(&ci);
	unsigned had = ci.n;
	t1 = now_us();
	scan_dir("", 0, 1, &lib_sig);
	t2 = now_us();
	qsort(tracks, ntracks, sizeof tracks[0], cmp_track);
	t3 = now_us();
	int hit = 0;
	uint64_t last_save = now_us();
	for (int i = 0; i < ntracks; i++) {
		CRec *c = cache_find(&ci, tracks[i].file);
		if (c && c->size == tracks[i].size && c->mtime == tracks[i].mtime) { tracks[i].m = c->m; hit++; continue; }
		char p[600];
		snprintf(p, sizeof p, MUSIC_DIR "/%s", tracks[i].file);
		const char *base = strrchr(tracks[i].file, '/');
		work_on(tracks[i].file);
		meta_read(p, base ? base + 1 : tracks[i].file, &tracks[i].m);
		work_on(NULL);
		if (c) lib_upd++; else lib_new++;
		scan_tick("Indexando música", i, ntracks);
#ifdef R1_SIM
		if (getenv("R1_SCAN_SLOW")) usleep(atoi(getenv("R1_SCAN_SLOW")));   // prueba de la pantalla de progreso
#endif
		if (now_us() - last_save > 30000000) { cache_save(i + 1); last_save = now_us(); }   // ponytail: un corte a mitad solo conserva lo ya leído
	}
	if (ci.f) fclose(ci.f);
	lib_gone = (int)had - hit - lib_upd;
	if (lib_new || lib_upd || lib_gone || !had) cache_save(ntracks);
	fast_save();
	int k = 0;
	for (int i = 0; i < ntracks; i++) {   // quita lo que el reproductor no puede abrir (ALAC, más de 2 canales, > 192 kHz)
		if (tracks[i].m.bad) { free(tracks[i].file); lib_unsup++; continue; }
		tracks[k++] = tracks[i];
	}
	ntracks = k;
	wave_load();
	char *m = lib_msg;
	size_t left = sizeof lib_msg;
	int w;
	if (had && (lib_new || lib_upd || lib_gone) && (w = snprintf(m, left, T("Música +%d ~%d -%d"), lib_new, lib_upd, lib_gone)) > 0) { m += w; left -= w; }
	if (lib_cut) snprintf(m, left, T("%sMáximo %d pistas"), m == lib_msg ? "" : " · ", MAX_TRACKS);
	else if (lib_unsup) snprintf(m, left, T("%s%d sin soporte"), m == lib_msg ? "" : " · ", lib_unsup);
	uint64_t t4 = now_us();
	build_indexes();
	uint64_t t5 = now_us();
	for (int i = 0; i < nm3u; i++) parse_m3u(m3u_files[i]);
	fprintf(stderr, "biblioteca: caché %.0f ms, directorios %.0f ms, orden %.0f ms, etiquetas %.0f ms, índices %.0f ms, listas %.0f ms; sin soporte %d, sobrantes %d\n", (t1 - t0) / 1e3, (t2 - t1) / 1e3, (t3 - t2) / 1e3, (t4 - t3) / 1e3, (t5 - t4) / 1e3, (now_us() - t5) / 1e3, lib_unsup, lib_cut);
}

// ---- Listas propias: archivos .m3u8 en music/Listas (también se ven y editan desde el PC) ----
// Cada cambio reescribe el archivo (tmp + rename) con rutas relativas a su carpeta y renueva la firma de la partición,
// para que guardar una lista no se tome como música nueva.
#define LIST_DIR "Listas"
static void pl_touch(void) {
	nm3u = 0;
	for (int k = 0; k < npl && nm3u < MAX_PLAYLISTS; k++) snprintf(m3u_files[nm3u++], sizeof m3u_files[0], "%s", pls[k].rel);
	boot_vsig = vol_sig();
	fast_save();
}
static int pl_save(int k) {
	Playlist *p = &pls[k];
	char path[320], tmp[330], up[64] = "";
	snprintf(path, sizeof path, MUSIC_DIR "/%s", p->rel);
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	for (const char *c = p->rel; *c; c++) if (*c == '/' && strlen(up) + 3 < sizeof up) strcat(up, "../");
	FILE *f = fopen(tmp, "w");
	if (!f) return 0;
	fprintf(f, "#EXTM3U\n#PLAYLIST:%s\n", p->name);
	for (int i = 0; i < p->n; i++) {
		const Track *t = &tracks[p->tr[i]];
		fprintf(f, "#EXTINF:%d,%s - %s\n%s%s\n", t->m.dur, t->m.artist, t->m.title, up, t->file);
	}
	commit_tmp(f, tmp, path);
	sync();
	pl_touch();
	return 1;
}
// Crea una lista vacía con el nombre dado (caracteres que FAT no admite pasan a "_"); devuelve su índice o -1.
static int pl_new(const char *name) {
	char fn[96], rel[256], path[320];
	struct stat st;
	if (npl >= MAX_PLAYLISTS || !name[0]) return -1;
	snprintf(fn, sizeof fn, "%s", name);
	for (char *c = fn; *c; c++) if (strchr("\\/:*?\"<>|", *c) || (unsigned char)*c < 32) *c = '_';
	mkdir(MUSIC_DIR "/" LIST_DIR, 0755);
	for (int i = 1; i < 100; i++) {
		if (i == 1) snprintf(rel, sizeof rel, LIST_DIR "/%s.m3u8", fn); else snprintf(rel, sizeof rel, LIST_DIR "/%s %d.m3u8", fn, i);
		snprintf(path, sizeof path, MUSIC_DIR "/%s", rel);
		if (stat(path, &st)) break;
	}
	Playlist *p = &pls[npl];
	memset(p, 0, sizeof *p);
	snprintf(p->name, sizeof p->name, "%s", name);
	snprintf(p->rel, sizeof p->rel, "%s", rel);
	npl++;
	if (!pl_save(npl - 1)) { npl--; return -1; }
	return npl - 1;
}
static void pl_delete(int k) {
	char path[320];
	snprintf(path, sizeof path, MUSIC_DIR "/%s", pls[k].rel);
	unlink(path);
	sync();
	free(pls[k].tr);
	memmove(&pls[k], &pls[k + 1], (npl - k - 1) * sizeof *pls);
	npl--;
	pl_touch();
}

static char **g_argv;

// ---- Batería (AXP2101 por I2C) y filtro digital del CS43131 ----
// Registro 0x64: tensión de carga (0x05 = 4,40 V de fábrica; 0x04 = 4,35; 0x03 = 4,20; 0x02 = 4,10). Solo se escriben estos valores,
// nunca por encima del de fábrica. Datos del fork bahusoid/rockbox (docs/ROCKBOX.md), verificados leyendo 0x64 = 0x05 y 0x62 = 0x0d.
static const unsigned char CHG_REG[4] = {0x05, 0x04, 0x03, 0x02};
static const char *CHG_N[4] = {"Sin límite", "4,35 V", "4,20 V", "4,10 V"};
static const char *DF_N[5] = {"Nítido", "Suave", "Nítido corto", "Suave corto", "Muy suave"};   // Digital Filter 0..4
static int chglim, dfilt;
static int axp_reg(unsigned char reg, int val) {   // val < 0: leer
#ifdef R1_SIM
	static int fake = 0x05;
	if (val >= 0) fake = val;
	return fake;
#else
	int fd = open("/dev/i2c-0", O_RDWR), r = -1;
	if (fd < 0) return -1;
	if (ioctl(fd, 0x0706 /* I2C_SLAVE_FORCE */, 0x34) == 0) {
		if (val >= 0) { unsigned char b[2] = {reg, (unsigned char)val}; r = write(fd, b, 2) == 2 ? 0 : -1; }
		else { unsigned char v; if (write(fd, &reg, 1) == 1 && read(fd, &v, 1) == 1) r = v; }
	}
	close(fd);
	return r;
#endif
}
static void chg_apply(void) { axp_reg(0x64, CHG_REG[chglim]); }
static int chg_current(void) { int v = axp_reg(0x64, -1); for (int i = 0; i < 4; i++) if (v == CHG_REG[i]) return i; return -1; }
static void df_apply(void) {
	struct snd_ctl_elem_value ev;
	memset(&ev, 0, sizeof ev);
	ev.id.numid = 4;   // "Digital Filter"
	ev.value.integer.value[0] = dfilt;
	if (ctl_fd >= 0) ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &ev);
}
static int battery_int(const char *name) {
	char p[96];
	snprintf(p, sizeof p, "/sys/class/power_supply/battery/%s", name);
	FILE *f = fopen(p, "r");
	int v = -1;
	if (f) { if (fscanf(f, "%d", &v) != 1) v = -1; fclose(f); }
	return v;
}
static int battery_charging(void) {
	FILE *f = fopen("/sys/class/power_supply/battery/status", "r");
	char s[16] = "";
	if (f) { if (!fgets(s, sizeof s, f)) s[0] = 0; fclose(f); }
	return !strncmp(s, "Charging", 8);
}
static int on_charger(void) {
	FILE *f = fopen("/sys/class/power_supply/battery/status", "r");
	char s[16] = "";
	if (f) { if (!fgets(s, sizeof s, f)) s[0] = 0; fclose(f); }
	return !strncmp(s, "Charging", 8) || !strncmp(s, "Full", 4);
}
// Aplica los cambios del disco reiniciando la aplicación (el arranque con caché es rápido). Los descriptores se marcan
// para cerrarse al ejecutar; si el exec falla la aplicación sigue como estaba.
static void lib_restart(void) {
	save_state();
	for (int fd = 3; fd < 256; fd++) fcntl(fd, F_SETFD, FD_CLOEXEC);
	execv("/proc/self/exe", g_argv);
}
// Decodifica la pista completa y deja en wv la envolvente de picos (WV_N barras, raíz cuadrada para que lo suave se vea).
// Los picos se acumulan por ventanas de 1/8 s; si pasan de 8192 se juntan de a dos, así no hace falta conocer la duración.
static int wave_calc(int ti) {
	Dec d;
	char path[320];
	static int32_t buf[PCM_PERIOD * 2];
	snprintf(path, sizeof path, MUSIC_DIR "/%s", tracks[ti].file);
	if (dec_open(&d, path)) return 0;
	unsigned short *w = malloc(8192 * sizeof *w);
	if (!w) { dec_close(&d); return 0; }
	unsigned nw = 0, win = d.rate / 8 ? d.rate / 8 : 1, fill = 0;
	uint64_t sq = 0;
	size_t n;
	while ((n = dec_read(&d, buf, PCM_PERIOD))) {
		for (size_t i = 0; i < n; i++) {
			for (unsigned c = 0; c < d.ch; c++) { int v = buf[i * d.ch + c] >> 16; sq += (uint64_t)(v * v); }
			if (++fill == win) {
				if (nw == 8192) { for (unsigned k = 0; k < 4096; k++) w[k] = sqrtf(((float)w[2 * k] * w[2 * k] + (float)w[2 * k + 1] * w[2 * k + 1]) / 2); nw = 4096; win *= 2; }
				w[nw++] = sqrtf((float)sq / (fill * d.ch)); fill = 0; sq = 0;
			}
		}
		usleep(1500);
		while (loading || req_track >= 0) usleep(50000);
		if (usb_hold) { dec_close(&d); free(w); return -1; }
		if (cur_track != ti && cur_track >= 0 && !tracks[cur_track].wv_ok) { dec_close(&d); free(w); return -1; }   // ahora suena otra: va primero
	}
	if (fill) w[nw++] = sqrtf((float)sq / (fill * d.ch));
	dec_close(&d);
	// Barras = nivel RMS (dB) del tramo, estirado entre los percentiles 10 y 98 de la propia pista para que se vea su dinámica
	// aunque esté masterizada muy alta.
	int ok = nw > 0;
	float db[WV_N], srt[WV_N];
	for (int b = 0; ok && b < WV_N; b++) {
		unsigned lo = (unsigned long long)b * nw / WV_N, hi = (unsigned long long)(b + 1) * nw / WV_N, m = 0;
		if (hi <= lo) hi = lo + 1;
		for (unsigned i = lo; i < hi && i < nw; i++) if (w[i] > m) m = w[i];
		db[b] = m ? 20 * log10f(m / 32768.0f) : -60;
		if (db[b] < -60) db[b] = -60;
		srt[b] = db[b];
	}
	if (ok) {
		for (int i = 1; i < WV_N; i++) { float v = srt[i]; int j = i - 1; while (j >= 0 && srt[j] > v) { srt[j + 1] = srt[j]; j--; } srt[j + 1] = v; }
		float lo = srt[WV_N / 10], hi = srt[WV_N * 98 / 100];
		if (hi - lo < 6) lo = hi - 6;
		for (int b = 0; b < WV_N; b++) {
			float v = (db[b] - lo) / (hi - lo);
			tracks[ti].wv[b] = (unsigned char)(255.0f * (v < 0.04f ? 0.04f : v > 1 ? 1 : v));
		}
	}
	free(w);
	return ok;
}
// Hilo de baja prioridad: calcula la forma de onda de la pista actual y de la siguiente; el resto de la biblioteca solo con el cargador
// conectado y sin reproducir (gasta batería y sus lecturas de la SD competían con la pista que suena: cortes por Bluetooth).
static void *wave_thread(void *arg) {
	setpriority(PRIO_PROCESS, syscall(SYS_gettid), 19);
	if (!getenv("R1_WAVE_NOW")) sleep(20);
	for (;;) {
		if (usb_hold) { sleep(1); continue; }
		int ti = cur_track, nx = qpos + 1 < qn ? qa[qpos + 1] : -1;
		if (ti < 0 || tracks[ti].wv_ok) ti = nx;
		if ((ti < 0 || tracks[ti].wv_ok) && ((on_charger() && (paused || cur_track < 0)) || getenv("R1_WAVE_NOW"))) { ti = -1; for (int i = 0; i < ntracks; i++) if (!tracks[i].wv_ok) { ti = i; break; } }
		else if (ti >= 0 && tracks[ti].wv_ok) ti = -1;
		if (ti < 0) { sleep(10); continue; }
		work_on(tracks[ti].file);
		int ok = wave_calc(ti);
		work_on(NULL);
		if (ok < 0) continue;
		tracks[ti].wv_ok = ok ? 1 : 2;
		if (ok) wave_save();
	}
}

// Cada 60 s compara la firma del disco con la del arranque; si difiere avisa de que hay cambios.
// Vigilante: compara la firma de la partición cada 20 s (no recorre carpetas).
static void *lib_watch(void *arg) {
	for (;;) {
		sleep(20);
		if (!lib_dirty && !usb_hold && !web_active() && vol_sig() != boot_vsig) { lib_dirty = 1; bt_dirty = 1; }
	}
}

// ---- Tema: modo claro u oscuro y un acento que también tiñe fondos, tarjetas, líneas y textos secundarios ----
// a1 = acento principal (títulos, botón central, progreso); a2 = secundario (artista, modos activos); el fondo se tiñe con un tercer tono.
typedef struct { uint32_t bg, bg2, fg, dim, a1, onsel, line, a2; } Skin;
static Skin SK;
enum { RAD = 14, RM = 6, NACC = 20, ACC_AUTO = 5 };   // radio de esquinas, margen de filas; acento 5 = Auto (se guarda por número)
// Acentos algo más saturados que la guía y fondos más profundos: la pantalla TFT eleva los negros y apaga los colores.
// 0-11: un color (el secundario y el fondo salen de él); 12-19: paletas de acento, secundario y tono de fondo.
static const uint32_t ACC[NACC][3] = {{0x2a6df0}, {0xff6a2e}, {0x2fcb8a}, {0x8f7bff}, {0xffb81c}, {0}, {0xf0414f}, {0xff5fa2}, {0x22c3e6}, {0x1fb5a6}, {0xa6d930}, {0x5b5ff0},
	{0xff7a45, 0xff4f9a, 0x6a2a8c}, {0x2fd3e6, 0x4f7bff, 0x123c6e}, {0x8fd14f, 0xe8c547, 0x1f4d2a}, {0xff4fd8, 0x45e0ff, 0x2a1460},
	{0xffc247, 0xff6a3d, 0x5a2a12}, {0xff5a6e, 0xffb38a, 0x5c1a2a}, {0x9fb4ff, 0xd8a8ff, 0x2a2f4a}, {0x3fe0b0, 0xffd166, 0x0f3d3a}};
static const char *ACC_N[NACC] = {"Azul", "Naranja", "Verde", "Violeta", "Ámbar", "Auto", "Rojo", "Rosa", "Cian", "Turquesa", "Lima", "Índigo",
	"Atardecer", "Océano", "Bosque", "Neón", "Desierto", "Vino", "Lavanda", "Trópico"};
static int light, acc_i = 1, theme_gen = 1;
// Paleta de la carátula actual: (matiz principal + 1) | (secundario + 1) << 9 | (tono de fondo + 1) << 18; 0 = sin color.
static volatile int auto_hue;
#define PAL_H(p, k) ((int)((p) >> (9 * (k)) & 511) - 1)

// Acento automático: matiz de la carátula con saturación y brillo fijos según el modo.
static uint32_t hsv_rgb(int h, float s, float v) {
	float c = v * s, x = c * (1 - fabsf(fmodf(h / 60.0f, 2) - 1)), m = v - c, r = 0, g = 0, b = 0;
	switch (h / 60 % 6) { case 0: r = c; g = x; break; case 1: r = x; g = c; break; case 2: g = c; b = x; break; case 3: g = x; b = c; break; case 4: r = x; b = c; break; default: r = c; b = x; }
	return (uint32_t)((r + m) * 255) << 16 | (uint32_t)((g + m) * 255) << 8 | (uint32_t)((b + m) * 255);
}
static uint32_t mix(uint32_t a, uint32_t b, int t) {   // a con t/256 de b
	uint32_t r = 0;
	for (int s = 0; s < 24; s += 8) r |= (((a >> s & 255) * (256 - t) + (b >> s & 255) * t) >> 8) << s;
	return r;
}
// Saturación del acento +10 % (menos que el +15 % de las carátulas).
static uint32_t saturate(uint32_t c) {
	int r = c >> 16 & 255, g = c >> 8 & 255, b = c & 255, l = (r * 77 + g * 151 + b * 28) >> 8, v[3] = {r, g, b};
	for (int k = 0; k < 3; k++) { v[k] = l + (v[k] - l) * 141 / 128; v[k] = v[k] < 0 ? 0 : v[k] > 255 ? 255 : v[k]; }
	return (uint32_t)v[0] << 16 | v[1] << 8 | v[2];
}
static void theme_apply(void) {
	int h1 = PAL_H(auto_hue, 0), h2 = PAL_H(auto_hue, 1), h3 = PAL_H(auto_hue, 2);
	float v = light ? 0.8f : 0.97f;
	uint32_t a, a2, t;
	if (acc_i == ACC_AUTO && h1 >= 0) {
		a = hsv_rgb(h1, 0.72f, v);
		a2 = h2 >= 0 ? hsv_rgb(h2, 0.6f, v) : hsv_rgb((h1 + 330) % 360, 0.45f, v);
		t = h3 >= 0 ? hsv_rgb(h3, 0.7f, 0.75f) : a;
	} else if (acc_i == ACC_AUTO) {   // carátula en blanco y negro o sin carátula: neutro
		a = light ? 0x34343c : 0xe4e4ea; a2 = light ? 0x60606a : 0xa8a8b4; t = 0x5a5a66;
	} else {
		const uint32_t *p = ACC[acc_i];
		a = p[0]; a2 = p[1] ? p[1] : mix(p[0], light ? 0x000000 : 0xffffff, 90); t = p[2] ? p[2] : p[0];
	}
	a = saturate(a); a2 = saturate(a2);
	uint32_t on = ((a >> 16 & 255) * 299 + (a >> 8 & 255) * 587 + (a & 255) * 114) / 1000 > 140 ? 0x0c0c10 : 0xffffff;
	if (light) SK = (Skin){mix(0xf7f7f3, t, 18), mix(0xe4e5e9, t, 44), 0x111113, mix(0x585c64, t, 44), a, on, mix(0xc4c7cd, t, 64), mix(a2, 0x000000, 40)};
	else SK = (Skin){mix(0x08080b, t, 20), mix(0x1a1a21, t, 40), 0xfcfcfd, mix(0xb6b6c2, t, 38), a, on, mix(0x2e2e38, t, 66), a2};
	theme_gen++;
}
#define C(x) lv_color_hex(x)
// Tamaños de texto de la guía: 24 secundario, 26 pestañas y duraciones, 28 artista, 30 filas, 32 títulos, 38 reproducción.
#define F24 (&ui_font_24)
#define F26 (&ui_font_26)
#define F28 (&ui_font_28)
#define F30 (&ui_font_30)
#define F32 (&ui_font_32)
#define F38 (&ui_font_38)

// ---- Geometría (480x800): barra superior 58, seis filas de 92, mini-reproductor 96 y barra de secciones 84 ----
enum { W = 480, H = 800, BAR_H = 58, NP_H = 90, NPB_H = 6, MINI_H = NP_H + NPB_H, NAV_H = 84, ROW_H = 92, ROW_S = 110,
       VIS = 6, NTAB = 3 };

enum { P_MUSIC, P_SONGS, P_QUEUE, P_PLAYER, P_LISTS, P_FAVS, P_SET, P_BT, P_ALBUMS, P_ALBUM, P_ARTISTS, P_ARTIST, P_PLAYLIST, P_SOUND, P_DISPLAY, P_SYSTEM, P_USB, P_POWER, P_PLAY, P_BATT, P_LIB, P_SEARCH, P_PICK, P_NEWLIST, P_WIFI, P_WIFIPW };
static const char *titles[] = {"Música", "Canciones", "Cola", "Reproduciendo", "Listas", "Favoritas", "Ajustes", "Bluetooth", "Álbumes", "Álbum", "Artistas", "Artista", "Lista", "Sonido", "Pantalla", "Sistema", "USB", "Energía", "Reproducción", "Batería", "Biblioteca", "Buscar", "Añadir a lista", "Nueva lista", "Wi-Fi", "Contraseña"};
static int ctx_album, ctx_artist, ctx_pl;
static int press_row = -1, nav_dir;   // fila pulsada, sentido de la transición
static int rh = ROW_H;                              // alto de fila de la pantalla actual
typedef struct { int id, sel, off; } Pg;            // off: desplazamiento de la lista en píxeles
static Pg stk[10];
static int sp;
static Pg *cur(void) { return &stk[sp]; }
static void push(int id) { if (sp < 9) { sp++; stk[sp] = (Pg){id, 0, 0}; nav_dir = 1; } }
static int kind(void) { return cur()->id; }

static void render(void);
static void refresh(lv_timer_t *t);
static int bat_chg = -1;   // 1 si carga; -1 fuerza releerlo
static void screen_set(int on);
static void power_menu_open(void);
static void panel_close(void);
static void panel_open(void);
static void panel_drag(lv_event_code_t c, int base, int dy);
static void panel_card_event(lv_event_t *e);
static void do_select(void);
static void slide(lv_obj_t *o, int from, int to, int ms, int vertical);

static lv_obj_t *pm_root, *np_img, *np_disc;
static lv_obj_t *np, *np_cover, *np_title, *np_artist, *np_pp, *np_fill, *bar, *bar_l, *bar_r, *bar_back, *bar_ico, *bar_fav, *bar_q, *body, *pn_root, *pn_vol, *pn_bri, *pn_hdr, *pn_card;
static lv_obj_t *hit, *pl_pp, *navbar, *actbar, *pz, *nav_ico[NTAB], *nav_lbl[NTAB];
static void sel_bar_build(void);
static lv_obj_t *pl_wv;
static int wv_split;   // barras de la forma de onda ya reproducidas
static const unsigned char *wv_data;
static lv_obj_t *pl_art, *pl_title, *pl_artist, *pl_album, *pl_fill, *pl_knob, *pl_t1, *pl_t2, *pl_next;
static int hint_until;
static char flash_msg[80];

// Registro de LVGL sin el aviso de cada carácter que falta en una fuente de respaldo (se busca en la siguiente).
static void lv_log_cb(lv_log_level_t l, const char *m) { if (!strstr(m, "cache not allocated")) fputs(m, stderr); }
static void set_text(lv_obj_t *o, const char *t) { t = T(t); if (o && strcmp(lv_label_get_text(o), t)) lv_label_set_text(o, t); }
static void fmt_time(char *out, size_t n, int s) { snprintf(out, n, "%d:%02d", s / 60, s % 60); }

static lv_obj_t *rect(lv_obj_t *p, int x, int y, int w, int h, uint32_t col) {
	lv_obj_t *o = lv_obj_create(p);
	lv_obj_remove_style_all(o);
	lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_pos(o, x, y);
	lv_obj_set_size(o, w, h);
	lv_obj_set_style_bg_color(o, C(col), 0);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
	return o;
}
static lv_obj_t *label(lv_obj_t *p, const lv_font_t *f, uint32_t col, const char *t) {
	lv_obj_t *l = lv_label_create(p);
	lv_obj_set_style_text_font(l, f, 0);
	lv_obj_set_style_text_color(l, C(col), 0);
	lv_label_set_text(l, T(t));
	return l;
}
static void pressfx(lv_obj_t *o, uint32_t col) { lv_obj_set_style_bg_color(o, C(col), LV_STATE_PRESSED); }
static void border(lv_obj_t *o, int w, uint32_t col, lv_border_side_t side) {
	lv_obj_set_style_border_width(o, w, 0);
	lv_obj_set_style_border_color(o, C(col), 0);
	lv_obj_set_style_border_side(o, side, 0);
}

// ---- Aviso flotante ----
// Tarjeta bajo la barra de estado con icono, título y detalle; entra deslizando (120 ms), dura 2,8 s y se cierra al tocarla.
static lv_obj_t *toast_o;
static uint32_t toast_until;
static int toast_shown;
static void toast_hide(void) { if (toast_o) { lv_obj_delete(toast_o); toast_o = NULL; } }
static void toast_click(lv_event_t *e) { toast_hide(); }
static void toast_tick(void) {
	if (toast_shown != toast_seq) {
		toast_shown = toast_seq;
		toast_hide();
		toast_o = rect(lv_screen_active(), 16, 10, W - 32, 82, SK.bg2);
		lv_obj_set_style_radius(toast_o, 16, 0);
		border(toast_o, 2, toast_warn ? 0xffb81c : SK.a1, LV_BORDER_SIDE_FULL);
		lv_obj_add_flag(toast_o, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_event_cb(toast_o, toast_click, LV_EVENT_CLICKED, NULL);
		lv_obj_t *ic = label(toast_o, &ui_icons_34, toast_warn ? 0xffb81c : SK.a1, toast_ico);
		lv_obj_align(ic, LV_ALIGN_LEFT_MID, 18, 0);
		lv_obj_t *t = label(toast_o, F28, SK.fg, toast_title);
		lv_obj_t *d = label(toast_o, F24, SK.dim, toast_sub);
		lv_obj_set_size(t, W - 32 - 92, lv_font_get_line_height(F28)); lv_obj_set_size(d, W - 32 - 92, lv_font_get_line_height(F24));
		lv_label_set_long_mode(t, LV_LABEL_LONG_DOT); lv_label_set_long_mode(d, LV_LABEL_LONG_DOT);
		lv_obj_set_pos(t, 76, 8);
		lv_obj_set_pos(d, 76, 44);
		slide(toast_o, -82, 10, 120, 1);
		toast_until = tick_ms() + 2800;
	}
	if (toast_o) lv_obj_move_foreground(toast_o);   // por encima de paneles y capas que se creen después
	if (toast_o && (int)(tick_ms() - toast_until) > 0) toast_hide();
}

// ---- Carátulas ----
// Un hilo busca la portada de la pista actual (cover/folder.jpg|png en su carpeta, o la incrustada en FLAC/MP3), la decodifica
// una sola vez y la deja ya escalada en RGB565 (72 px para la franja, 480 px para el reproductor). La interfaz solo la muestra.
enum { COV_S = 72, COV_B = 480 };
static volatile int cov_want = -1, pend_ready, pend_hue;
static uint16_t *pend_s, *pend_b, *cov_s_px, *cov_b_px;
static lv_image_dsc_t cov_s_dsc, cov_b_dsc;

static unsigned char *read_file(const char *path, size_t *n) {
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char *b = sz > 0 && sz < (8 << 20) ? malloc(sz) : NULL;
	if (b && fread(b, 1, sz, f) != (size_t)sz) { free(b); b = NULL; }
	fclose(f);
	*n = b ? sz : 0;
	return b;
}
static uint32_t be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

// Imagen del bloque PICTURE de un FLAC.
static unsigned char *flac_cover(const char *path, size_t *n) {
	FILE *f = fopen(path, "rb");
	unsigned char h[4];
	unsigned char *out = NULL;
	if (!f || fread(h, 1, 4, f) != 4 || memcmp(h, "fLaC", 4)) { if (f) fclose(f); return NULL; }
	for (int last = 0; !last && !out && fread(h, 1, 4, f) == 4;) {
		last = h[0] >> 7;
		uint32_t len = h[1] << 16 | h[2] << 8 | h[3];
		if ((h[0] & 0x7f) != 6 || len > (8u << 20)) { fseek(f, len, SEEK_CUR); continue; }
		unsigned char *b = malloc(len);
		if (!b || fread(b, 1, len, f) != len) { free(b); break; }
		uint32_t o = 4, ml, dl;
		if (len < 32) { free(b); break; }
		ml = be32(b + o); o += 4 + ml;
		if (o + 4 <= len) { dl = be32(b + o); o += 4 + dl + 16; }
		if (o + 4 <= len) { uint32_t il = be32(b + o); o += 4; if (il && o + il <= len) { out = malloc(il); if (out) { memcpy(out, b + o, il); *n = il; } } }
		free(b);
	}
	fclose(f);
	return out;
}

// Imagen del primer marco APIC de una etiqueta ID3v2.3/2.4.
static unsigned char *mp3_cover(const char *path, size_t *n) {
	FILE *f = fopen(path, "rb");
	unsigned char h[10];
	unsigned char *out = NULL;
	if (!f || fread(h, 1, 10, f) != 10 || memcmp(h, "ID3", 3) || h[3] < 3) { if (f) fclose(f); return NULL; }
	int ver = h[3];
	uint32_t tag = (h[6] & 0x7f) << 21 | (h[7] & 0x7f) << 14 | (h[8] & 0x7f) << 7 | (h[9] & 0x7f), pos = 0;
	while (pos + 10 < tag && !out && fread(h, 1, 10, f) == 10) {
		if (!h[0]) break;
		uint32_t sz = ver == 4 ? (h[4] & 0x7f) << 21 | (h[5] & 0x7f) << 14 | (h[6] & 0x7f) << 7 | (h[7] & 0x7f) : be32(h + 4);
		pos += 10 + sz;
		if (memcmp(h, "APIC", 4) || sz > (8u << 20)) { fseek(f, sz, SEEK_CUR); continue; }
		unsigned char *b = malloc(sz);
		if (!b || fread(b, 1, sz, f) != sz) { free(b); break; }
		uint32_t i = 1;
		while (i < sz && b[i]) i++;
		i += 2;
		if (b[0] == 1 || b[0] == 2) { while (i + 1 < sz && (b[i] || b[i + 1])) i += 2; i += 2; }
		else { while (i < sz && b[i]) i++; i++; }
		if (i < sz) { out = malloc(sz - i); if (out) { memcpy(out, b + i, sz - i); *n = sz - i; } }
		free(b);
	}
	fclose(f);
	return out;
}

// Imagen incrustada en un M4A (caja covr de ilst).
static unsigned char *m4a_cover(const char *path, size_t *n) {
	FILE *f = fopen(path, "rb");
	Meta m;
	long cov[2] = {0, 0};
	unsigned char *out = NULL;
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	meta_m4a_walk(f, 0, size, 0, 0, &m, cov);
	if (cov[1] > 0 && cov[1] < (8 << 20) && !fseek(f, cov[0], SEEK_SET) && (out = malloc(cov[1]))) {
		if (fread(out, 1, cov[1], f) == (size_t)cov[1]) *n = cov[1]; else { free(out); out = NULL; }
	}
	fclose(f);
	return out;
}

// Paleta de una imagen RGB (ver auto_hue): histograma de 24 sectores de matiz. El principal y el secundario (a 45° o más, con al menos
// un cuarto de su peso) pesan saturación² × brillo: el color más vivo. El tono de fondo pesa saturación × área: el color que más ocupa.
static int dominant_hue(const unsigned char *px, int w, int h) {
	float bin[24] = {0}, area[24] = {0}, tot = 0, atot = 0;
	int n = 0;
	for (int y = 0; y < h; y += 6) for (int x = 0; x < w; x += 6, n++) {
		const unsigned char *p = px + (y * w + x) * 3;
		int mx = p[0] > p[1] ? p[0] : p[1], mn = p[0] < p[1] ? p[0] : p[1];
		if (p[2] > mx) mx = p[2];
		if (p[2] < mn) mn = p[2];
		float v = mx / 255.0f, s = mx ? (float)(mx - mn) / mx : 0;
		if (s < 0.12f || v < 0.12f) continue;
		float d = mx - mn, hh = mx == p[0] ? fmodf((p[1] - p[2]) / d, 6) : mx == p[1] ? (p[2] - p[0]) / d + 2 : (p[0] - p[1]) / d + 4;
		if (hh < 0) hh += 6;
		int k = (int)(hh * 4) % 24;
		area[k] += s; atot += s;
		if (s < 0.25f || v < 0.2f) continue;
		bin[k] += s * s * v; tot += s * s * v;
	}
	if (!n || tot < n * 0.003f) return 0;   // casi gris
	float sc[24], asc[24];
	for (int k = 0; k < 24; k++) { sc[k] = bin[k] + 0.5f * (bin[(k + 1) % 24] + bin[(k + 23) % 24]); asc[k] = area[k] + 0.5f * (area[(k + 1) % 24] + area[(k + 23) % 24]); }
	int b1 = 0, b2 = -1, b3 = 0;
	for (int k = 0; k < 24; k++) { if (sc[k] > sc[b1]) b1 = k; if (asc[k] > asc[b3]) b3 = k; }
	for (int k = 0; k < 24; k++) {
		int dk = abs(k - b1); if (dk > 12) dk = 24 - dk;
		if (dk >= 3 && sc[k] >= sc[b1] * 0.25f && (b2 < 0 || sc[k] > sc[b2])) b2 = k;
	}
	int h3 = atot > n * 0.02f ? b3 * 15 + 7 : -1;
	return (b1 * 15 + 8) | (b2 >= 0 ? b2 * 15 + 8 : 0) << 9 | (h3 + 1) << 18;
}

// Ajuste de color de las carátulas para la pantalla TFT: saturación +15 % y sombras algo más profundas (gamma 1,08).
static unsigned char grade_lut[256];
static void grade(unsigned *r, unsigned *g, unsigned *b) {
	if (!grade_lut[255]) for (int i = 0; i < 256; i++) grade_lut[i] = (unsigned char)(255.0f * powf(i / 255.0f, 1.08f) + 0.5f);
	int l = (*r * 77 + *g * 151 + *b * 28) >> 8, c[3] = {*r, *g, *b};
	for (int k = 0; k < 3; k++) { int v = l + ((c[k] - l) * 147 >> 7); c[k] = grade_lut[v < 0 ? 0 : v > 255 ? 255 : v]; }
	*r = c[0]; *g = c[1]; *b = c[2];
}
// Recorta al cuadrado central y reduce por promedio de área a s x s en RGB565.
static uint16_t *scale565(const unsigned char *px, int w, int h, int s) {
	uint16_t *o = malloc(s * s * 2);
	int side = w < h ? w : h, x0 = (w - side) / 2, y0 = (h - side) / 2;
	for (int dy = 0; o && dy < s; dy++) {
		int ya = y0 + dy * side / s, yb = y0 + (dy + 1) * side / s;
		if (yb <= ya) yb = ya + 1;
		for (int dx = 0; dx < s; dx++) {
			int xa = x0 + dx * side / s, xb = x0 + (dx + 1) * side / s;
			if (xb <= xa) xb = xa + 1;
			unsigned r = 0, g = 0, b = 0, n = 0;
			for (int y = ya; y < yb; y++) for (int x = xa; x < xb; x++) { const unsigned char *p = px + (y * w + x) * 3; r += p[0]; g += p[1]; b += p[2]; n++; }
			r /= n; g /= n; b /= n;
			grade(&r, &g, &b);
			o[dy * s + dx] = (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3);
		}
	}
	return o;
}

static void cover_dir(int t, char *dir, size_t n) {
	snprintf(dir, n, MUSIC_DIR "/%s", tracks[t].file);
	char *sl = strrchr(dir, '/');
	if (sl) *sl = 0;
}
static const char *cover_names[] = {"cover.jpg", "folder.jpg", "front.jpg", "album.jpg", "Cover.jpg", "Folder.jpg", "cover.png", "folder.png"};

// Caché de carátulas (SD): miniatura, versión grande y matiz ya calculados. Vale mientras no cambie la fuente
// (el archivo de portada o la carpeta); evita decodificar y reducir la imagen cada vez.
typedef struct { char magic[4]; unsigned ver; int hue; long size, mtime; } ArtHdr;
static void cover_src(int t, long *size, long *mtime) {
	char dir[400], path[480];
	struct stat st;
	cover_dir(t, dir, sizeof dir);
	*size = *mtime = 0;
	for (int i = 0; i < 8; i++) {
		snprintf(path, sizeof path, "%s/%s", dir, cover_names[i]);
		if (!stat(path, &st)) { *size = st.st_size; *mtime = st.st_mtime; return; }
	}
	if (!stat(dir, &st)) *mtime = st.st_mtime;
}
static int art_read(const char *path, long size, long mtime) {
	FILE *f = fopen(path, "rb");
	ArtHdr h;
	if (!f) return 0;
	uint16_t *s = malloc(COV_S * COV_S * 2), *b = malloc(COV_B * COV_B * 2);
	int ok = s && b && fread(&h, sizeof h, 1, f) == 1 && !memcmp(h.magic, "R1AC", 4) && h.ver == 3 && h.size == size && h.mtime == mtime
		&& fread(s, 2, COV_S * COV_S, f) == COV_S * COV_S && fread(b, 2, COV_B * COV_B, f) == COV_B * COV_B;
	fclose(f);
	if (ok) { pend_hue = h.hue; pend_s = s; pend_b = b; } else { free(s); free(b); }
	return ok;
}
static void art_write(const char *path, long size, long mtime) {
	char tmp[640];
	ArtHdr h = {{'R', '1', 'A', 'C'}, 3, pend_hue, size, mtime};
	mkdir(CACHE_DIR, 0755); mkdir(CACHE_DIR "/art", 0755);
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	FILE *f = fopen(tmp, "wb");
	if (!f) return;
	fwrite(&h, sizeof h, 1, f); fwrite(pend_s, 2, COV_S * COV_S, f); fwrite(pend_b, 2, COV_B * COV_B, f);
	commit_tmp(f, tmp, path);
}
// Tope de la caché de carátulas (~470 KB cada una): al pasarlo se borran las de uso más antiguo hasta quedar en el 80 %.
// Leer una carátula renueva su fecha, así que quedan las que se escuchan.
#define ART_CAP (300L << 20)
typedef struct { long mtime, size; char name[24]; } ArtEnt;
static int art_cmp(const void *a, const void *b) { long d = ((const ArtEnt *)a)->mtime - ((const ArtEnt *)b)->mtime; return d < 0 ? -1 : d > 0; }
static void art_trim(void) {
	DIR *d = opendir(CACHE_DIR "/art");
	struct dirent *e;
	ArtEnt *v = NULL;
	int n = 0, cap = 0;
	long total = 0;
	char p[300];
	while (d && (e = readdir(d))) {
		struct stat st;
		if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof v->name) continue;
		snprintf(p, sizeof p, CACHE_DIR "/art/%s", e->d_name);
		if (stat(p, &st)) continue;
		if (n == cap) { cap = cap ? cap * 2 : 256; v = grow(v, cap * sizeof *v); }
		v[n].mtime = st.st_mtime; v[n].size = st.st_size; snprintf(v[n].name, sizeof v->name, "%s", e->d_name);
		total += st.st_size; n++;
	}
	if (d) closedir(d);
	if (total > ART_CAP) {
		int gone = 0;
		qsort(v, n, sizeof *v, art_cmp);
		for (int i = 0; i < n && total > ART_CAP / 10 * 8; i++) { snprintf(p, sizeof p, CACHE_DIR "/art/%s", v[i].name); if (!unlink(p)) { total -= v[i].size; gone++; } }
		fprintf(stderr, "carátulas: caché sobre el tope, %d borradas (quedan %ld MB)\n", gone, total >> 20);
	}
	free(v);
}
static void cover_load(int t, const char *key) {
	char dir[400], path[480], cp[160];
	size_t n = 0;
	unsigned char *img = NULL;
	long ssz, smt;
	unsigned long long hs = 1469598103934665603ULL;
	for (const char *k = key; *k; k++) hs = (hs ^ (unsigned char)*k) * 1099511628211ULL;
	snprintf(cp, sizeof cp, CACHE_DIR "/art/%016llx.cov", hs);
	cover_src(t, &ssz, &smt);
	if (art_read(cp, ssz, smt)) { utime(cp, NULL); return; }
	cover_dir(t, dir, sizeof dir);
	snprintf(path, sizeof path, MUSIC_DIR "/%s", tracks[t].file);
	if (ext_is(path, ".flac")) img = flac_cover(path, &n);
	else if (ext_is(path, ".mp3")) img = mp3_cover(path, &n);
	else if (ext_is(path, ".m4a") || ext_is(path, ".mp4")) img = m4a_cover(path, &n);
	for (int i = 0; i < 8 && !img; i++) { snprintf(path, sizeof path, "%s/%s", dir, cover_names[i]); img = read_file(path, &n); }
	if (!img) return;
	int w, h, c;
	if (stbi_info_from_memory(img, n, &w, &h, &c) && w <= 1600 && h <= 1600) {
		unsigned char *px = stbi_load_from_memory(img, n, &w, &h, &c, 3);
		if (px) { pend_hue = dominant_hue(px, w, h); pend_s = scale565(px, w, h, COV_S); pend_b = scale565(px, w, h, COV_B); stbi_image_free(px); if (pend_s && pend_b) { static int writes; art_write(cp, ssz, smt); if (!(++writes & 15)) art_trim(); } }
	}
	free(img);
}
static void *cover_thread(void *arg) {
	char have[500] = "";
	art_trim();
	for (;;) {
		usleep(100000);
		int t = cov_want;
		if (t < 0 || pend_ready || usb_hold) continue;
		cov_want = -1;
		char key[500], dir[400];
		cover_dir(t, dir, sizeof dir);
		snprintf(key, sizeof key, "%s|%s|%s", dir, tracks[t].m.album, track_artist(&tracks[t]));
		if (!strcmp(key, have)) continue;
		snprintf(have, sizeof have, "%s", key);
		pend_s = pend_b = NULL; pend_hue = 0;
		uint32_t c0 = tick_ms();
		cover_load(t, key);
		fprintf(stderr, "carátula: %s en %u ms, paleta %d/%d/%d\n", pend_s ? "lista" : "sin imagen", tick_ms() - c0, PAL_H(pend_hue, 0), PAL_H(pend_hue, 1), PAL_H(pend_hue, 2));
		pend_ready = 1;
	}
}
static void cover_show(void);
static void cov_dsc(lv_image_dsc_t *d, uint16_t *px, int s) {
	memset(d, 0, sizeof *d);
	d->header.magic = LV_IMAGE_HEADER_MAGIC;
	d->header.cf = LV_COLOR_FORMAT_RGB565;
	d->header.w = d->header.h = s;
	d->header.stride = s * 2;
	d->data_size = s * s * 2;
	d->data = (const uint8_t *)px;
}
// Carátula del reproductor con el fundido ya aplicado a los píxeles (arriba se oscurece para los iconos; abajo se funde con el
// fondo del tema bajo el título). Se recalcula al cambiar de carátula o de tema; dibujarla no cuesta más que una imagen opaca.
enum { FADE_TOP = 120, FADE_Y0 = 250 };
static uint16_t *cov_k_px;
static int cov_k_gen;
static uint16_t px_mix(uint16_t p, uint32_t c, int t) {   // p con t/256 de c (RGB888)
	int r = (p >> 11) << 3, g = (p >> 5 & 63) << 2, b = (p & 31) << 3;
	r += (((int)(c >> 16 & 255) - r) * t) >> 8; g += (((int)(c >> 8 & 255) - g) * t) >> 8; b += (((int)(c & 255) - b) * t) >> 8;
	return (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
}
static uint16_t *cover_baked(void) {
	if (!cov_b_px) return NULL;
	if (cov_k_px && cov_k_gen == theme_gen) return cov_k_px;
	if (!cov_k_px && !(cov_k_px = malloc(COV_B * COV_B * 2))) return cov_b_px;
	cov_k_gen = theme_gen;
	for (int y = 0; y < COV_B; y++) {
		int t = 0;
		uint32_t c = SK.bg;
		if (y < FADE_TOP) { int u = (FADE_TOP - y) * 256 / FADE_TOP; t = u * u / 256 * 150 / 256; c = 0; }   // hasta 60 % de negro arriba
		else if (y >= FADE_Y0) { int u = (y - FADE_Y0) * 256 / (COV_B - FADE_Y0); t = u * u * (768 - 2 * u) / 65536; }   // curva suave hasta el fondo
		uint16_t *row = cov_k_px + y * COV_B, *src = cov_b_px + y * COV_B;
		if (!t) { memcpy(row, src, COV_B * 2); continue; }
		for (int x = 0; x < COV_B; x++) row[x] = px_mix(src[x], c, t > 256 ? 256 : t);
	}
	return cov_k_px;
}

// ---- Brillo ----
#ifdef R1_SIM
#define BL_FILE "/tmp/r1_backlight"
#else
#define BL_FILE "/sys/class/backlight/backlight_pwm0/brightness"
#endif
enum { BL_MIN = 1, BL_MAX = 100 };   // el controlador acepta 101, pero con 101 la luz se apaga; 1 = más bajo sin apagar
static int bright = 60;
static int scr_off;   // 1 = stand-by: pantalla y táctil apagados, la reproducción y el Bluetooth siguen
static void bl_write(int v) {
	FILE *f = fopen(BL_FILE, "w");
	if (f) { fprintf(f, "%d\n", v); fclose(f); }
}
static void set_bright(int v) {
	bright = v < BL_MIN ? BL_MIN : v > BL_MAX ? BL_MAX : v;
	if (!scr_off) bl_write(bright);
}

// Tiempos de energía (índices guardados en el estado). Apagado automático: solo corre sin reproducir.
static const int SCR_TO[] = {15, 30, 60, 120, 300, 0};   // segundos hasta apagar la pantalla
static const char *SCR_TO_N[] = {"15 s", "30 s", "1 min", "2 min", "5 min", "Nunca"};
static const int OFF_MIN[] = {5, 10, 15, 30, 60, 0};     // minutos hasta apagar el equipo
static const char *OFF_N[] = {"5 min", "10 min", "15 min", "30 min", "1 h", "Nunca"};
static int scr_to_i = 1, off_disc_i = 1, off_conn_i = 3;
static int boot_armed;
static uint32_t app_t0;
static lv_indev_t *touch_dev;
#ifndef R1_SIM
// Táctil hyn_ts (protocolo multitáctil A, coordenadas ya en píxeles). Cada lectura vacía la cola del kernel y entrega solo
// el estado actual: lo que no se alcanzó a procesar se descarta, no se acumula. Si en la cola hubo un levantamiento, esta
// lectura informa suelto y la siguiente el toque nuevo (dos toques no se funden en un arrastre). Solo cuenta el primer
// contacto de cada cuadro. En stand-by se lee igual pero se informa suelto.
static int tp_fd = -1, tp_x, tp_y, tp_down;
static void touch_read(lv_indev_t *d, lv_indev_data_t *data) {
	struct input_event e;
	int up = 0, skip = 0;
	while (read(tp_fd, &e, sizeof e) == sizeof e) {
		if (e.type == EV_SYN) {
			if (e.code == SYN_MT_REPORT) skip = 1;
			else if (e.code == SYN_REPORT) skip = 0;
			else if (e.code == SYN_DROPPED) up = 1;
		} else if (e.type == EV_ABS && !skip) {
			if (e.code == ABS_MT_POSITION_X) tp_x = e.value < 0 ? 0 : e.value >= W ? W - 1 : e.value;
			else if (e.code == ABS_MT_POSITION_Y) tp_y = e.value < 0 ? 0 : e.value >= H ? H - 1 : e.value;
		} else if (e.type == EV_KEY && e.code == BTN_TOUCH) {
			if (!e.value) up = 1;
			tp_down = e.value != 0;
		}
	}
	data->point.x = tp_x;
	data->point.y = tp_y;
	data->state = tp_down && !up && !scr_off ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
#endif
// El deslizador de brillo es perceptual: porcentaje -> valor del controlador con curva potencial (más resolución en lo oscuro).
#define BL_GAMMA 2.5f
static int bl_from_pct(int p) { return BL_MIN + (int)((BL_MAX - BL_MIN) * powf(p / 100.0f, BL_GAMMA) + 0.5f); }
static int bright_pct(void) { return (int)(100 * powf((float)(bright - BL_MIN) / (BL_MAX - BL_MIN), 1 / BL_GAMMA) + 0.5f); }

// ---- Persistencia ----
static int sort_s, sort_a, sort_r;   // orden de canciones, álbumes y artistas (ver SORT_*_N)
static void save_state(void) {
	FILE *f = fopen(STATE_FILE, "w");
	if (!f) return;
	fprintf(f, "light=%d\nacc=%d\nvol=%d\nshuf=%d\nrep=%d\ncont=%d\nchglim=%d\ndfilt=%d\nbright=%d\nscrto=%d\noffd=%d\noffc=%d\nsorts=%d\nsorta=%d\nsortr=%d\nbton=%d\nwifi=%d\nwlan=%d\nweb=%d\nbtcodec=%s\nldacq=%d\nsniff=%d\nlang=%d\n", light, acc_i, vol, shuf, rep, cont, chglim, dfilt, bright, scr_to_i, off_disc_i, off_conn_i, sort_s, sort_a, sort_r, bt_on, wifi_on, wlan_on, web_on, bt_codec_pref, ldac_q, sniff_m, lang);
	for (int i = 0; i < ntracks; i++) if (tracks[i].fav) fprintf(f, "fav=%s\n", tracks[i].file);
	fclose(f);
}
static void load_state(void) {
	FILE *f = fopen(STATE_FILE, "r");
	char l[200];
	while (f && fgets(l, sizeof l, f)) {
		l[strcspn(l, "\n")] = 0;
		if (!strncmp(l, "light=", 6)) light = atoi(l + 6) != 0;
		else if (!strncmp(l, "acc=", 4)) acc_i = atoi(l + 4) % NACC;
		else if (!strncmp(l, "vol=", 4)) vol = atoi(l + 4);
		else if (!strncmp(l, "shuf=", 5)) shuf = atoi(l + 5);
		else if (!strncmp(l, "rep=", 4)) rep = atoi(l + 4) % 3;
		else if (!strncmp(l, "cont=", 5)) cont = atoi(l + 5) % 3;
		else if (!strncmp(l, "chglim=", 7)) chglim = atoi(l + 7) & 3;
		else if (!strncmp(l, "dfilt=", 6)) dfilt = atoi(l + 6) % 5;
		else if (!strncmp(l, "bright=", 7)) set_bright(atoi(l + 7));
		else if (!strncmp(l, "scrto=", 6)) scr_to_i = atoi(l + 6) % 6;
		else if (!strncmp(l, "offd=", 5)) off_disc_i = atoi(l + 5) % 6;
		else if (!strncmp(l, "offc=", 5)) off_conn_i = atoi(l + 5) % 6;
		else if (!strncmp(l, "bton=", 5)) bt_on = atoi(l + 5) != 0;
		else if (!strncmp(l, "wifi=", 5)) wifi_on = atoi(l + 5) != 0;
		else if (!strncmp(l, "wlan=", 5)) wlan_on = atoi(l + 5) != 0;
		else if (!strncmp(l, "web=", 4)) web_on = atoi(l + 4) != 0;
		else if (!strncmp(l, "ldacq=", 6)) ldac_q = atoi(l + 6) & 3;
		else if (!strncmp(l, "sniff=", 6)) sniff_m = atoi(l + 6) % 3;
		else if (!strncmp(l, "lang=", 5)) lang = atoi(l + 5) == 1;
		else if (!strncmp(l, "btcodec=", 8)) { snprintf(bt_codec_pref, sizeof bt_codec_pref, "%s", l + 8); bt_codec_pref[strcspn(bt_codec_pref, "\r\n")] = 0; }
		else if (!strncmp(l, "sorts=", 6)) sort_s = atoi(l + 6) % 5;
		else if (!strncmp(l, "sorta=", 6)) sort_a = atoi(l + 6) % 4;
		else if (!strncmp(l, "sortr=", 6)) sort_r = atoi(l + 6) % 3;
		else if (!strncmp(l, "fav=", 4)) { int ti = track_by_file(l + 4); if (ti >= 0) tracks[ti].fav = 1; }
	}
	if (f) fclose(f);
	if (vol < 0 || vol > 100) vol = 20;
	theme_apply();
}

// ---- Orden de las listas ----
// Claves de orden: minúsculas, sin acentos latinos y con los números rellenos a 6 cifras ("Pista 2" < "Pista 10").
static const char SORT_S_N[5][12] = {"Carpeta", "Título", "Artista", "Álbum", "Reciente"};
static const char SORT_A_N[4][12] = {"Carpeta", "Nombre", "Artista", "Reciente"};
static const char SORT_R_N[3][12] = {"Carpeta", "Nombre", "Más pistas"};
static int *ord_s, *ord_a, *ord_r;   // índices de pistas, álbumes y artistas en el orden elegido
static char **sk_key;
static const char L1[] = "aaaaaaaceeeeiiiidnooooo*ouuuuytsaaaaaaaceeeeiiiidnooooo/ouuuuyty";   // U+00C0..U+00FF sin acento
// Minúsculas y sin acentos latinos (para buscar).
static void fold(char *d, size_t n, const char *s) {
	size_t o = 0;
	while (*s && o + 1 < n) {
		if ((unsigned char)s[0] == 0xC3 && ((unsigned char)s[1] & 0xC0) == 0x80) { d[o++] = L1[(unsigned char)s[1] - 0x80]; s += 2; }
		else { d[o++] = *s >= 'A' && *s <= 'Z' ? *s + 32 : *s; s++; }
	}
	d[o] = 0;
}
static void key_text(char *dst, size_t n, const char *src) {
	size_t o = 0;
	while (*src && o + 8 < n) {
		if (*src >= '0' && *src <= '9') {
			int d = 0;
			while (src[d] >= '0' && src[d] <= '9') d++;
			for (int k = d; k < 6; k++) dst[o++] = '0';
			for (int k = 0; k < d && o + 2 < n; k++) dst[o++] = src[k];
			src += d;
		} else if ((unsigned char)src[0] == 0xC3 && ((unsigned char)src[1] & 0xC0) == 0x80) { dst[o++] = L1[(unsigned char)src[1] - 0x80]; src += 2; }
		else { dst[o++] = *src >= 'A' && *src <= 'Z' ? *src + 32 : *src; src++; }
	}
	dst[o++] = 1;
	dst[o] = 0;
}
static int sk_cmp(const void *a, const void *b) {
	int x = *(const int *)a, y = *(const int *)b, c = strcmp(sk_key[x], sk_key[y]);
	return c ? c : x - y;
}
// Ordena ord[0..n) con la clave que construye key(i, mode, out); mode 0 deja el orden de carpeta.
static void sort_make(int *ord, int n, int mode, void (*key)(int, int, char *, size_t)) {
	for (int i = 0; i < n; i++) ord[i] = i;
	if (!mode || n < 2) return;
	sk_key = malloc(n * sizeof *sk_key);
	if (!sk_key) return;
	for (int i = 0; i < n; i++) { char k[420]; k[0] = 0; key(i, mode, k, sizeof k); sk_key[i] = strdup(k); }
	qsort(ord, n, sizeof *ord, sk_cmp);
	for (int i = 0; i < n; i++) free(sk_key[i]);
	free(sk_key); sk_key = NULL;
}
static void key_desc(char *out, size_t n, long v) { snprintf(out, n, "%012ld", 999999999999L - v); }
static void key_song(int i, int mode, char *out, size_t n) {
	const Track *t = &tracks[i];
	char a[200], b[200];
	switch (mode) {
	case 1: key_text(out, n, t->m.title); break;
	case 2: key_text(a, sizeof a, t->m.artist[0] ? t->m.artist : track_artist(t)); key_text(b, sizeof b, t->m.album); snprintf(out, n, "%s%s%03d%03d", a, b, t->m.disc, t->m.track); break;
	case 3: key_text(a, sizeof a, t->m.album); snprintf(out, n, "%s%03d%03d", a, t->m.disc, t->m.track); break;
	default: key_desc(out, n, t->mtime);
	}
}
static long *alb_mtime;
static void key_album(int i, int mode, char *out, size_t n) {
	char a[200], b[200];
	if (mode == 1) { key_text(a, sizeof a, albums[i].name); snprintf(out, n, "%s", a); }
	else if (mode == 2) { key_text(a, sizeof a, albums[i].artist); key_text(b, sizeof b, albums[i].name); snprintf(out, n, "%s%s", a, b); }
	else key_desc(out, n, alb_mtime[i]);
}
static void key_artist(int i, int mode, char *out, size_t n) {
	if (mode == 1) key_text(out, n, artists[i].name); else key_desc(out, n, artists[i].count);
}
static void sort_all(void) {
	ord_s = grow(ord_s, (ntracks + 1) * sizeof *ord_s);
	ord_a = grow(ord_a, (nalbums + 1) * sizeof *ord_a);
	ord_r = grow(ord_r, (nartists + 1) * sizeof *ord_r);
	sort_make(ord_s, ntracks, sort_s, key_song);
	alb_mtime = calloc(nalbums + 1, sizeof *alb_mtime);
	if (alb_mtime) for (int i = 0; i < ntracks; i++) if (tracks[i].album >= 0 && tracks[i].mtime > alb_mtime[tracks[i].album]) alb_mtime[tracks[i].album] = tracks[i].mtime;
	sort_make(ord_a, nalbums, alb_mtime ? sort_a : (sort_a == 3 ? 0 : sort_a), key_album);
	free(alb_mtime); alb_mtime = NULL;
	sort_make(ord_r, nartists, sort_r, key_artist);
}

// ---- Contenido de listas ----
// Las listas largas (canciones, álbumes, artistas, lista, favoritas, cola) no guardan filas: lz es el arreglo de elementos y cada fila se
// compone al dibujarla (rowp). Las pantallas de ajustes y similares sí construyen su arreglo rows.
typedef struct { char l[100], sub[96], d[28]; int idx, q, chev, hl; const char *ic; uint32_t icc; } Row;   // icc: color del icono (0 = atenuado)   // q: posición en la cola (-1 fuera de ella)
static Row *rows;
static int nrows, rows_cap;
static const int *lz;           // elementos de la lista larga actual (NULL en pantallas con filas propias)
static int *lz_own, lz_q0;      // lz_own: copia propia que se libera al reconstruir; lz_q0: posición en la cola del primer elemento
static int lz_kind;

static void row_add(const char *l, const char *sub, const char *d, int idx, int chev) {
	if (nrows == rows_cap) { rows_cap = rows_cap ? rows_cap * 2 : 256; rows = grow(rows, rows_cap * sizeof *rows); }
	Row *r = &rows[nrows++];
	snprintf(r->l, sizeof r->l, "%s", T(l)); snprintf(r->sub, sizeof r->sub, "%s", T(sub)); snprintf(r->d, sizeof r->d, "%s", T(d));
	r->idx = idx; r->q = -1; r->chev = chev; r->hl = 0; r->ic = NULL; r->icc = 0;
}
static void row_ic(const char *ic) { rows[nrows - 1].ic = ic; }
static int album_cmp(const void *a, const void *b) {
	const Track *x = &tracks[*(const int *)a], *y = &tracks[*(const int *)b];
	if (x->m.disc != y->m.disc) return x->m.disc - y->m.disc;
	if (x->m.track != y->m.track) return x->m.track - y->m.track;
	return strcmp(x->file, y->file);
}
static int nfav(void) { int n = 0; for (int i = 0; i < ntracks; i++) n += tracks[i].fav; return n; }

// Compone la fila de la lista larga i en r.
static void lz_fill(int i, Row *r) {
	int v = lz[i];
	memset(r, 0, sizeof *r);
	r->idx = v; r->q = -1;
	if (v < 0) {   // primera fila del artista
		snprintf(r->l, sizeof r->l, T("Todas las canciones")); snprintf(r->sub, sizeof r->sub, T("%d pistas · reproducir"), artists[ctx_artist].count); r->ic = ICO_PLAY;
		return;
	}
	switch (lz_kind) {
	case P_ALBUMS: case P_ARTIST:
		snprintf(r->d, sizeof r->d, "%d", albums[v].count); snprintf(r->l, sizeof r->l, "%s", albums[v].name); snprintf(r->sub, sizeof r->sub, "%s", albums[v].artist);
		break;
	case P_ARTISTS: {
		int na = artists[v].albums;
		snprintf(r->l, sizeof r->l, "%s", artists[v].name); snprintf(r->sub, sizeof r->sub, "%d %s", na, na == 1 ? T("álbum") : T("álbumes")); snprintf(r->d, sizeof r->d, "%d", artists[v].count);
		break;
	}
	default:   // canciones, álbum, favoritas, lista y cola
		fmt_time(r->d, sizeof r->d, tracks[v].m.dur);
		snprintf(r->l, sizeof r->l, "%s", tracks[v].m.title);
		snprintf(r->sub, sizeof r->sub, "%s", lz_kind == P_ALBUM ? tracks[v].m.fmt : tracks[v].m.album[0] ? tracks[v].m.album : tracks[v].m.artist);
	}
}
// Fila i de la pantalla actual. En listas largas se devuelve un hueco de un anillo: vale hasta 16 llamadas más.
static const Row *rowp(int i) {
	static Row ring[16];
	static int next;
	if (!lz) return &rows[i];
	Row *r = &ring[next++ & 15];
	lz_fill(i, r);
	return r;
}
static int row_idx(int i) { return lz ? lz[i] : rows[i].idx; }
static int row_q(int i) { return lz ? (lz_kind == P_QUEUE ? lz_q0 + i : -1) : rows[i].q; }
static int row_hl(int i) { return lz ? ((lz_kind == P_SONGS || lz_kind == P_FAVS || lz_kind == P_QUEUE || lz_kind == P_ALBUM || lz_kind == P_PLAYLIST) && cur_track >= 0 && lz[i] == cur_track) : rows[i].hl; }

// ---- Selección múltiple ----
// Pulsación larga en cualquier elemento lo selecciona (en todas las listas); después tocar marca o desmarca y otra pulsación larga marca
// el rango desde la última tocada. Elemento = tipo << 24 | índice: 1 artista, 2 álbum, 3 pista, 4 lista, 5 posición en la cola,
// 6 posición en la lista abierta. Las acciones se aplican en el orden de la pantalla; álbumes y artistas aportan todas sus pistas.
enum { SEL_MAX = 2000, SEL_TRACKS = 5000 };
static int sel_on, sel_n, sel_page, sel_depth, sel_last = -1, sel_v[SEL_MAX];
static unsigned list_gen = 1;   // sube cuando cambia el contenido de las filas (obliga a redibujarlas)
static int row_item(int i) {
	if (i < 0 || i >= nrows) return -1;
	int v = row_idx(i);
	switch (kind()) {
	case P_SEARCH: return v >= 1 << 24 ? v : -1;
	case P_QUEUE: return 5 << 24 | row_q(i);
	case P_PLAYLIST: return 6 << 24 | i;
	case P_SONGS: case P_FAVS: case P_ALBUM: return v >= 0 ? 3 << 24 | v : -1;
	case P_ALBUMS: case P_ARTIST: return v >= 0 ? 2 << 24 | v : -1;
	case P_ARTISTS: return v >= 0 ? 1 << 24 | v : -1;
	case P_LISTS: return v >= 0 ? 4 << 24 | v : -1;
	}
	return -1;
}
static int sel_find(int it) { for (int k = 0; k < sel_n; k++) if (sel_v[k] == it) return k; return -1; }
static void sel_clear(void) { sel_on = sel_n = 0; sel_last = -1; }
static void sel_add(int it) { if (it >= 0 && sel_n < SEL_MAX && sel_find(it) < 0) sel_v[sel_n++] = it; }
static void sel_toggle(int row) {
	int it = row_item(row), k = sel_find(it);
	if (it < 0) return;
	if (k >= 0) sel_v[k] = sel_v[--sel_n]; else sel_add(it);
	sel_last = row;
}
static void sel_range(int row) {
	int a = sel_last < 0 ? row : sel_last, b = row;
	if (a > b) { int t = a; a = b; b = t; }
	for (int i = a; i <= b && sel_n < SEL_MAX; i++) sel_add(row_item(i));
	sel_last = row;
}
static void sel_start(int row) { sel_clear(); sel_on = 1; sel_page = kind(); sel_depth = sp; sel_add(row_item(row)); sel_last = row; }

// ---- Elegir lista: lo elegido (pista, álbum o artista) queda pendiente mientras se elige o se crea la lista ----
static int *pick_l, pick_n;
static char nq[64];   // nombre de la lista nueva mientras se escribe
static char wq[64];   // contraseña de la red pw_net mientras se escribe
static WNet pw_net;
static void flash(const char *m);
static void pick_start(int *l, int n) { free(pick_l); pick_l = l; pick_n = n; }
static void pick_apply(int k) {
	char m[140];
	for (int i = 0; i < pick_n; i++) pl_add(&pls[k], pick_l[i]);
	if (!pl_save(k)) snprintf(m, sizeof m, T("No se pudo guardar la lista"));
	else if (pick_n == 1) snprintf(m, sizeof m, T("Añadida a %s"), pls[k].name);
	else snprintf(m, sizeof m, T("%d pistas añadidas a %s"), pick_n, pls[k].name);
	flash(m);
	pick_start(NULL, 0);
}

// ---- Búsqueda ----
// Coincide si todas las palabras de la consulta aparecen (sin mayúsculas ni acentos). Resultados: artistas, álbumes y pistas;
// idx = tipo << 24 | índice (1 artista, 2 álbum, 3 pista).
// ponytail: recorre y normaliza toda la biblioteca en cada tecla; precalcular las claves si pasa de ~10 000 pistas.
static char sq[64];   // consulta tal como se escribe
static int kb_on;     // teclado visible en la búsqueda
enum { KB_H = 380, KB_R = 92 };   // alto del teclado y de cada fila (tecla de 84 px)
static lv_obj_t *kb_root;
static void layout(void);
static void bar_title_set(void);
static char sw[8][64];
static int nsw;
static int words_in(const char *s) {
	char f[400];
	fold(f, sizeof f, s);
	for (int k = 0; k < nsw; k++) if (!strstr(f, sw[k])) return 0;
	return 1;
}
static void search_rows(void) {
	char q[64], b[400], *tok, *sv;
	fold(q, sizeof q, sq);
	nsw = 0;
	for (tok = strtok_r(q, " ", &sv); tok && nsw < 8; tok = strtok_r(NULL, " ", &sv)) snprintf(sw[nsw++], sizeof sw[0], "%s", tok);
	if (!nsw) { row_add("Escribe para buscar", "Canciones, álbumes y artistas", "", -1, 0); return; }
	for (int k = 0, n = 0; k < nartists && n < 5; k++) {
		int a = ord_r[k];
		if (!words_in(artists[a].name)) continue;
		snprintf(b, sizeof b, T("Artista · %d %s"), artists[a].albums, artists[a].albums == 1 ? T("álbum") : T("álbumes"));
		row_add(artists[a].name, b, "", 1 << 24 | a, 1); row_ic(ICO_HEADPHONES); rows[nrows - 1].icc = SK.a2; n++;
	}
	for (int k = 0, n = 0; k < nalbums && n < 8; k++) {
		int a = ord_a[k];
		snprintf(b, sizeof b, "%s\1%s", albums[a].name, albums[a].artist);
		if (!words_in(b)) continue;
		snprintf(b, sizeof b, T("Álbum · %s"), albums[a].artist);
		row_add(albums[a].name, b, "", 2 << 24 | a, 1); row_ic(ICO_DISC); rows[nrows - 1].icc = SK.a1; n++;
	}
	for (int k = 0, n = 0; k < ntracks && n < 100; k++) {
		const Track *t = &tracks[ord_s[k]];
		snprintf(b, sizeof b, "%s\1%s\1%s", t->m.title, t->m.artist, t->m.album);
		if (!words_in(b)) continue;
		char d[16], sub[260];
		fmt_time(d, sizeof d, t->m.dur);
		snprintf(sub, sizeof sub, "%s%s%s", t->m.artist, t->m.artist[0] && t->m.album[0] ? " · " : "", t->m.album);
		row_add(t->m.title, sub, d, 3 << 24 | ord_s[k], 0); row_ic(ICO_MUSIC);
		rows[nrows - 1].hl = ord_s[k] == cur_track; n++;
	}
	if (!nrows) row_add("Sin resultados", sq, "", -1, 0);
}

static void build_rows(void) {
	char b[40];
	int k0 = kind();
	nrows = 0;
	lz = NULL; free(lz_own); lz_own = NULL; lz_kind = k0;
	switch (k0) {
	case P_ALBUMS: lz = ord_a; nrows = nalbums; break;
	case P_ARTISTS: lz = ord_r; nrows = nartists; break;
	case P_SONGS: lz = ord_s; nrows = ntracks; break;
	case P_ARTIST: case P_ALBUM: case P_FAVS: {
		int cap = k0 == P_ARTIST ? nalbums : k0 == P_ALBUM ? albums[ctx_album].count : ntracks, n = 0;
		lz_own = malloc((cap + 2) * sizeof *lz_own);
		if (k0 == P_ARTIST && lz_own) lz_own[n++] = -1;
		for (int i = 0; lz_own && i < (k0 == P_ARTIST ? nalbums : ntracks); i++) {
			if (k0 == P_ARTIST) { int a = ord_a[i]; if (albums[a].artist_i == ctx_artist) lz_own[n++] = a; }
			else if (k0 == P_ALBUM) { if (tracks[i].album == ctx_album) lz_own[n++] = i; }
			else if (tracks[i].fav) lz_own[n++] = i;
		}
		if (k0 == P_ALBUM && lz_own) qsort(lz_own, n, sizeof *lz_own, album_cmp);
		lz = lz_own; nrows = n;
		if (k0 == P_FAVS && !n) { lz = NULL; row_add("Sin favoritos", "Usa el corazón en Reproduciendo", "", -1, 0); }
		break;
	}
	case P_PLAYLIST: {
		lz_own = malloc((pls[ctx_pl].n + 1) * sizeof *lz_own);
		for (int k = 0; lz_own && k < pls[ctx_pl].n; k++) lz_own[k] = pls[ctx_pl].tr[k];
		lz = lz_own; nrows = lz_own ? pls[ctx_pl].n : 0;
		break;
	}
	case P_QUEUE:
		pthread_mutex_lock(&qlock);
		nrows = qn - qpos - 1;
		if (nrows < 0) nrows = 0;
		lz_own = malloc((nrows + 1) * sizeof *lz_own);
		for (int k = 0; lz_own && k < nrows; k++) lz_own[k] = qa[qpos + 1 + k];
		lz_q0 = qpos + 1;
		pthread_mutex_unlock(&qlock);
		lz = lz_own;
		if (!nrows) { lz = NULL; snprintf(b, sizeof b, T("Al terminar: %s"), CONT_N[cont]); row_add(T("Cola vacía"), b, "", -1, 0); }
		break;
	case P_SEARCH: search_rows(); break;
	case P_MUSIC:
		row_add("Buscar", "Artista, álbum o canción", "", 6, 1); row_ic(ICO_SEARCH);
		snprintf(b, sizeof b, "%d", ntracks); row_add(T("Canciones"), "", b, 0, 1); row_ic(ICO_MUSIC);
		snprintf(b, sizeof b, "%d", nalbums); row_add(T("Álbumes"), "", b, 1, 1); row_ic(ICO_DISC);
		snprintf(b, sizeof b, "%d", nartists); row_add(T("Artistas"), "", b, 2, 1); row_ic(ICO_HEADPHONES);
		row_add("Todo aleatorio", "Toda la música mezclada", "", 5, 0); row_ic(ICO_SHUFFLE);
		break;
	case P_LISTS: case P_PICK: {   // Favoritas es la primera lista (las pistas marcadas con el corazón)
		int nf = 0;
		for (int k = 0; k < ntracks; k++) nf += tracks[k].fav;
		snprintf(b, sizeof b, "%d", nf); row_add(T("Favoritas"), k0 == P_PICK ? T("Marcar con el corazón") : "", b, -3, k0 == P_LISTS); row_ic(ICO_HEART);
		row_add("Nueva lista", k0 == P_PICK ? "Con lo elegido" : "Se guarda en music/Listas", "", -2, 0); row_ic(ICO_LISTUL);
		for (int k = 0; k < npl; k++) { snprintf(b, sizeof b, "%d", pls[k].n); row_add(pls[k].name, "", b, k, k0 == P_LISTS); }
		break;
	}
	case P_NEWLIST:
		snprintf(b, sizeof b, "%d %s", pick_n, pick_n == 1 ? T("pista para añadir") : T("pistas para añadir"));
		row_add("Escribe el nombre", pick_n ? b : "Toca la marca para crearla", "", -1, 0);
		break;
	case P_SET:
		row_add("Sonido", "", out_mode ? (bt_pcm[0] ? "Bluetooth" : "BT sin enlace") : jack_in ? "Jack 3.5" : "Jack vacío", 0, 1); row_ic(ICO_VOLUME);
		row_add("Reproducción", "", "", 5, 1); row_ic(ICO_SHUFFLE);
		row_add("Bluetooth", "", bt_pcm[0] ? "Conectado" : "", 1, 1); row_ic(ICO_BT);
		row_add("Wi-Fi", wlan_on && wlan_ip[0] && web_on ? "Servidor activo" : "", !wlan_on ? "No" : wlan_ip[0] ? wlan_ssid : "Sin conexión", 6, 1); row_ic(ICO_WIFI);
		snprintf(b, sizeof b, "%s", ACC_N[acc_i]); row_add(T("Pantalla"), "", b, 2, 1); row_ic(ICO_PALETTE);
		snprintf(b, sizeof b, "%d", ntracks); row_add(T("Biblioteca"), lib_dirty ? T("Hay cambios sin aplicar") : T("Orden y actualización"), b, 3, 1); row_ic(ICO_MUSIC);
		row_add("USB", "", adb_on ? "ADB" : "Transferir", 7, 1); row_ic(ICO_TRANSFER);
		row_add("Sistema", "", "v0.1", 4, 1); row_ic(ICO_INFO);
		break;
	case P_LIB:
		snprintf(b, sizeof b, T("%d pistas"), ntracks);
		row_add("Actualizar", lib_dirty ? "Hay cambios: toca aquí" : "Buscar música nueva", b, 0, 0); row_ic(ICO_MUSIC);
		row_add("Canciones", "Orden de la lista", SORT_S_N[sort_s], 1, 0); row_ic(ICO_SLIDERS);
		row_add("Álbumes", "Orden de la lista", SORT_A_N[sort_a], 2, 0); row_ic(ICO_SLIDERS);
		row_add("Artistas", "Orden de la lista", SORT_R_N[sort_r], 3, 0); row_ic(ICO_SLIDERS);
		row_add("Formatos", "FLAC MP3 AAC ALAC OGG Opus WAV", "", -1, 0); row_ic(ICO_INFO);
		break;
	case P_BATT: {
		int mv = battery_int("voltage_now"), cur = chg_current();
		snprintf(b, sizeof b, "%d,%02d V", mv / 1000000, mv / 10000 % 100);
		row_add("Estado", battery_charging() ? "Conectada al cargador" : "", battery_int("capacity") >= 0 ? (battery_charging() ? "Cargando" : "En uso") : "", -1, 0); row_ic(ICO_BATTERY);
		row_add("Tensión", "", mv > 0 ? b : "", -1, 0); row_ic(ICO_INFO);
		row_add("Límite de carga", cur >= 0 && cur != chglim ? "El cargador usa otro valor" : "Máx. de fábrica 4,40 V", CHG_N[chglim], 2, 0); row_ic(ICO_POWER);
		break;
	}
	case P_PLAY:
		row_add("Al terminar la cola", "Cuando no queda nada por sonar", CONT_N[cont], 0, 0); row_ic(ICO_LISTUL);
		row_add("Aleatorio", "Mezcla lo que falta por sonar", shuf ? "Sí" : "No", 1, 0); row_ic(ICO_SHUFFLE);
		row_add("Repetir", "", rep == 0 ? "No" : rep == 1 ? "Todo" : "Pista", 2, 0); row_ic(ICO_REPEAT);
		break;
	case P_SOUND:
		row_add("Salida", "Toca para cambiar", out_mode ? (bt_pcm[0] ? "Bluetooth" : "BT sin enlace") : jack_in ? "Jack 3.5" : "Jack vacío", 0, 0); row_ic(ICO_HEADPHONES);
		row_add("Filtro digital", "Respuesta del DAC", DF_N[dfilt], 1, 0); row_ic(ICO_SLIDERS);
		break;
	case P_DISPLAY:
		row_add("Modo", "", light ? "Claro" : "Oscuro", 0, 0); row_ic(ICO_SUN);
		row_add("Color", "", ACC_N[acc_i], 1, 0); row_ic(ICO_PALETTE);
		snprintf(b, sizeof b, "%d%%", bright_pct()); row_add(T("Brillo"), T("Toca para subir"), b, 2, 0); row_ic(ICO_SLIDERS);
		row_add("Apagar pantalla", "Tras inactividad", SCR_TO_N[scr_to_i], 3, 0); row_ic(ICO_MOON);
		row_add(lang ? "Language" : "Idioma", "Español · English", lang ? "English" : "Español", 4, 0); row_ic(ICO_INFO);
		break;
	case P_SYSTEM:
		row_add("Energía", "", "", 0, 1); row_ic(ICO_POWER);
		row_add("Batería", "", "", 2, 1); row_ic(ICO_BATTERY);
		row_add("Versión", "", "0.1", -1, 0); row_ic(ICO_INFO);
		break;
	case P_POWER:
		row_add("Apagado sin conexión", "Sin reproducir ni Bluetooth", OFF_N[off_disc_i], 0, 0); row_ic(ICO_POWER);
		row_add("Apagado con conexión", "Sin reproducir, con Bluetooth", OFF_N[off_conn_i], 1, 0); row_ic(ICO_BT);
		row_add("Apagar o reiniciar", "", "", 2, 1); row_ic(ICO_POWER);
		break;
	case P_USB: {   // un modo u otro: con ADB la tarjeta no se entrega al PC
		int p2 = !access("/dev/mmcblk0p2", F_OK);
		row_add("Modo", adb_on ? "Depuración; el PC no ve la tarjeta" : "El PC ve la música como una unidad", adb_on ? "ADB" : "Transferir", 0, 0); row_ic(adb_on ? ICO_COG : ICO_TRANSFER);
		if (!adb_on) {
			row_add("Transferir ahora", !p2 ? "Hace falta particionar la SD" : usb_conf ? "PC conectado: toca para empezar" : "Conecta el cable al PC", "", 1, 0); row_ic(ICO_TRANSFER);
			rows[nrows - 1].hl = p2 && usb_conf;
		}
		break;
	}
	case P_WIFI:
		if (wlan_on && bt_pcm[0]) { row_add("Antena compartida", "El audio BT puede cortarse", "", -1, 0); row_ic(ICO_INFO); }
		row_add("Wi-Fi", !wlan_on ? "Apagado" : wlan_msg[0] ? wlan_msg : wlan_ip[0] ? wlan_ssid : "Sin conexión", wlan_on ? "Sí" : "No", 0, 0); row_ic(ICO_WIFI);
		if (wlan_on) {
			char sub[200], val[32] = "";
			if (!web_on) snprintf(sub, sizeof sub, "%s", T("Subir música y editar listas desde el navegador"));
			else if (!wlan_ip[0]) snprintf(sub, sizeof sub, "%s", T("Esperando conexión"));
			else if (web_len) { snprintf(sub, sizeof sub, T("Recibiendo %s"), web_name); snprintf(val, sizeof val, "%d%%", (int)(web_got * 100 / web_len)); }
			else snprintf(sub, sizeof sub, WEB_PORT == 80 ? "http://%s" : "http://%s:%d", wlan_ip, WEB_PORT);
			if (web_on && !val[0]) snprintf(val, sizeof val, web_files ? T("%d recibidos") : "Sí", web_files);
			row_add("Música por Wi-Fi", sub, web_on ? val : "No", 1, 0); row_ic(ICO_TRANSFER);
			row_add(wlan_busy && wlan_msg[0] ? wlan_msg : "Buscar redes", "", "", 2, 0); row_ic(ICO_SEARCH);
			for (int i = 0; i < nwn; i++) {
				const WNet *w = &wn[i];
				snprintf(b, sizeof b, "%d dBm", w->sig);
				row_add(w->ssid, w->cur ? (wlan_ip[0] ? "Conectada" : "Conectando...") : w->id >= 0 ? (w->sig ? "Guardada" : "Guardada · fuera de alcance") : w->sec ? "Protegida" : "Abierta", w->sig ? b : "", 100 + i, 0);
				row_ic(w->sec ? ICO_LOCK : ICO_WIFI);
				rows[nrows - 1].hl = w->cur && wlan_ip[0];
			}
		}
		{ char w[32]; wx_text(w, sizeof w); row_add("Clima", !wifi_on ? (wlan_on ? "Consulta cada 30 min" : "Wi-Fi un momento cada 30 min") : wifi_up ? "Consultando por Wi-Fi..." : wx_city[0] ? wx_city : "Esperando", wifi_on ? (w[0] ? w : "Sí") : "No", 3, 0); row_ic(ICO_SUN); }
		break;
	case P_WIFIPW:
		row_add(pw_net.ssid, "Escribe la contraseña y confirma", "", -1, 0); row_ic(ICO_LOCK);
		break;
	case P_BT:
		row_add("Bluetooth", bt_on ? (bt_pcm[0] ? bt_name_of(bt_cur_mac) : "Sin conexión") : "Radio apagada", bt_on ? "Sí" : "No", 0, 0); row_ic(ICO_BT);
		if (!bt_on) { row_add("Apagado", "Enciéndelo para conectar auriculares", "", -1, 0); row_ic(ICO_INFO); break; }
		{ char sub[100], val[40];
		  if (bt_pcm[0]) snprintf(sub, sizeof sub, T("Admite: %s"), bt_codecs); else snprintf(sub, sizeof sub, T("Se aplica al conectar"));
		  snprintf(val, sizeof val, "%s%s%s", bt_codec_pref[0] ? bt_codec_pref : "Auto", bt_pcm[0] && bt_codec[0] ? " · " : "", bt_pcm[0] ? bt_codec : "");
		  row_add("Códec", sub, val, 50, 0); row_ic(ICO_SLIDERS); }
		row_add("Calidad LDAC", LDAC_S[ldac_q], LDAC_N[ldac_q], 51, 0); row_ic(ICO_SLIDERS);
		row_add("Ahorro del enlace", SNIFF_S[sniff_m], SNIFF_N[sniff_m], 52, 0); row_ic(ICO_POWER);
		row_add(bt_msg[0] ? bt_msg : "Buscar dispositivos", bt_msg[0] ? "" : "Busca durante 10 s", "", 1, 0); row_ic(ICO_SEARCH);
		for (int i = 0; i < nbt; i++) { row_add(btd[i].name, btd[i].connected ? "Conectado" : btd[i].paired ? "Emparejado" : "Nuevo: toca para emparejar", "", 2 + i, 1); row_ic(ICO_HEADPHONES); }
		break;
	}
	if (k0 == P_BT) for (int i = 0; i < nbt && 2 + i < nrows; i++) rows[2 + i].hl = btd[i].connected;
}

static void flash(const char *m) { snprintf(flash_msg, sizeof flash_msg, "%s", T(m)); hint_until = tick_ms() + 1300; }

static void seek_to(int sec);
static void start_track(int t) { if (NO_OUT()) { toast_post(ICO_HEADPHONES, "Sin auriculares", "Conecta unos para reproducir", 1); return; } if (t >= 0 && t < ntracks) { paused = 0; cur_track = t; pos_s = 0; loading = 1; req_track = t; } }
// Cola nueva con todas las pistas de la lista visible (o de la biblioteca si row < 0) empezando por la fila elegida.
static void play_rows(int row) {
	short *l = malloc((nrows + 1) * sizeof *l);
	int n = 0, st = 0;
	if (!l) return;
	for (int i = 0; i < nrows; i++) if (row_idx(i) >= 0) { if (i == row) st = n; l[n++] = row_idx(i); }
	start_track(q_set(l, n, st));
	free(l);
}
static int coll_tracks(int is_artist, int idx, int **out);
// Álbum abierto desde un artista: la cola es todo el artista (álbumes en orden) empezando por la pista elegida.
static void play_artist(int ti) {
	int *l, n = coll_tracks(1, ctx_artist, &l), st = 0;
	short *s = n ? malloc(n * sizeof *s) : NULL;
	for (int k = 0; s && k < n; k++) { s[k] = l[k]; if (l[k] == ti) st = k; }
	if (s) start_track(q_set(s, n, st));
	free(s); free(l);
}
static void play_lib(int first) {
	short *l = malloc((ntracks + 1) * sizeof *l);
	if (!l) return;
	for (int i = 0; i < ntracks; i++) l[i] = i;
	start_track(q_set(l, ntracks, first));
	free(l);
}
static void toggle_pause(void) {
	if (paused && NO_OUT()) { toast_post(ICO_HEADPHONES, "Sin auriculares", "Conecta unos para reproducir", 1); return; }
	if (cur_track >= 0) { paused = !paused; return; }
	int t = q_jump(0);
	if (t >= 0) start_track(t); else play_lib(0);
}
static void go(int d) {
	if (!ntracks) return;
	if (d < 0 && cur_track >= 0 && pos_s > 3) { seek_to(0); return; }
	int t = qn > 0 ? (d > 0 ? q_next(0, 0) : q_prev()) : -1;
	if (t >= 0) start_track(t); else play_lib(d > 0 ? 0 : ntracks - 1);
}

// ---- Interacción ----
static void panel_close(void) { if (pn_root) { lv_obj_delete(pn_root); pn_root = pn_vol = pn_bri = pn_hdr = pn_card = NULL; } }
static void back(void) { if (sel_on) { sel_clear(); render(); return; } if (sp > 0) { sp--; nav_dir = -1; render(); } }
// Secciones de la barra inferior; nav_root abre una como raíz.
static const int tabs[NTAB] = {P_MUSIC, P_LISTS, P_SET};
static const char *tab_name[NTAB] = {"Música", "Listas", "Ajustes"};
static const char *tab_ico[NTAB] = {ICO_MUSIC, ICO_LISTUL, ICO_COG};
static void nav_root(int id) { sel_clear(); sp = 0; stk[0] = (Pg){id, 0, 0}; nav_dir = 0; if (id == P_SEARCH) kb_on = 1; render(); }

// Revisa el disco ahora: si hay cambios reinicia para aplicarlos.
static void lib_check(void) {
	Sig sg = {0, 0, 0};
	scan_dir("", 0, 0, &sg);
	if (sg.n != lib_sig.n || sg.size != lib_sig.size || sg.mtime != lib_sig.mtime) { flash("Actualizando biblioteca..."); refresh(NULL); lv_refr_now(NULL); setenv("R1_FULLSCAN", "1", 1); lib_restart(); }
	else { flash("Sin cambios en la biblioteca"); boot_vsig = vol_sig(); lib_dirty = 0; fast_save(); }   // otro archivo cambió la firma: se acepta
}
static void bt_dev_dialog(int i);
static void wifi_net_dialog(int i);
static void acc_open(void);
static void do_select(void) {
	Pg *c = cur();
	build_rows();
	int i = c->sel < nrows ? row_idx(c->sel) : -1;
	switch (kind()) {
	case P_ALBUMS: if (i >= 0) { ctx_album = i; push(P_ALBUM); } break;
	case P_ARTISTS: if (i >= 0) { ctx_artist = i; push(P_ARTIST); } break;
	case P_ARTIST: if (i >= 0) { ctx_album = i; push(P_ALBUM); } else if (c->sel == 0) play_artist(-1); break;
	case P_MUSIC: {
		static const int to[3] = {P_SONGS, P_ALBUMS, P_ARTISTS};
		if (i == 6) { sq[0] = 0; kb_on = 1; push(P_SEARCH); }
		else if (i == 5) { shuf = 1; save_state(); play_lib(rand() % (ntracks ? ntracks : 1)); push(P_PLAYER); }
		else if (i >= 0 && i < 3) push(to[i]);
		break;
	}
	case P_SEARCH:
		if (i >> 24 == 1) { ctx_artist = i & 0xffffff; push(P_ARTIST); }
		else if (i >> 24 == 2) { ctx_album = i & 0xffffff; push(P_ALBUM); }
		else if (i >> 24 == 3) {   // cola: las pistas encontradas, desde la elegida
			short *l = malloc(nrows * sizeof *l);
			int n = 0, st = 0;
			for (int k = 0; l && k < nrows; k++) if (rows[k].idx >> 24 == 3) { if (k == c->sel) st = n; l[n++] = rows[k].idx & 0xffffff; }
			if (l) { start_track(q_set(l, n, st)); free(l); push(P_PLAYER); }
		}
		break;
	case P_SONGS: case P_FAVS: case P_QUEUE: case P_ALBUM: case P_PLAYLIST:
		if (c->id == P_QUEUE) { int t = q_jump(row_q(c->sel)); if (t >= 0) { start_track(t); c->off = 0; } }
		else if (i >= 0) play_rows(c->sel);
		break;
	case P_LISTS: case P_PICK:
		if (i == -2) { nq[0] = 0; kb_on = 1; push(P_NEWLIST); }
		else if (i == -3 && kind() == P_LISTS) push(P_FAVS);
		else if (i == -3) {   // añadir a Favoritas = marcar con el corazón
			for (int k = 0; k < pick_n; k++) tracks[pick_l[k]].fav = 1;
			save_state(); flash(pick_n == 1 ? "Favorito añadido" : "Añadidas a Favoritas"); pick_start(NULL, 0); back(); return;
		}
		else if (i >= 0 && kind() == P_LISTS) { ctx_pl = i; push(P_PLAYLIST); }
		else if (i >= 0) { pick_apply(i); back(); return; }
		break;
	case P_WIFI:
		if (i == 0) wlan_job(WJ_POWER, NULL, !wlan_on, NULL);
		else if (i == 1) { web_on = !web_on; save_state(); }
		else if (i == 2) wlan_job(WJ_SCAN, NULL, 0, NULL);
		else if (i == 3) { wifi_on = !wifi_on; save_state(); flash(wifi_on ? "Clima activado" : "Clima desactivado"); }
		else if (i >= 100 && i - 100 < nwn) wifi_net_dialog(i - 100);
		break;
	case P_SET: if (i == 6) { push(P_WIFI); break; } if (i == 7) { push(P_USB); break; } if (i == 0) push(P_SOUND); else if (i == 1) { push(P_BT); bt_start_job(BJ_LIST, NULL, 0); } else if (i == 2) push(P_DISPLAY); else if (i == 4) push(P_SYSTEM); else if (i == 5) push(P_PLAY); else if (i == 3) push(P_LIB); break;
	case P_SOUND: if (i == 1) { dfilt = (dfilt + 1) % 5; df_apply(); save_state(); } else if (i == 0) { if (bt_pcm[0] || out_mode) { out_mode = !out_mode; bt_vol_pending = 1; } else flash("Sin Bluetooth conectado"); } break;
	case P_DISPLAY:
		if (i == 0) { light = !light; theme_apply(); }
		else if (i == 1) { acc_open(); return; }
		else if (i == 2) { int p = bright_pct(); set_bright(bl_from_pct(p >= 100 ? 0 : p + 20 > 100 ? 100 : p + 20)); }
		else if (i == 3) scr_to_i = (scr_to_i + 1) % 6;
		else if (i == 4) { lang = !lang; theme_gen++; }   // reconstruye la interfaz en el idioma nuevo
		save_state();
		break;
	case P_PLAY:
		if (i == 0) cont = (cont + 1) % 3;
		else if (i == 1) { shuf = !shuf; q_reorder(); }
		else if (i == 2) rep = (rep + 1) % 3;
		save_state();
		break;
	case P_LIB:
		if (i == 0) lib_check();
		else if (i >= 1 && i <= 3) { if (i == 1) sort_s = (sort_s + 1) % 5; else if (i == 2) sort_a = (sort_a + 1) % 4; else sort_r = (sort_r + 1) % 3; sort_all(); save_state(); }
		break;
	case P_BATT: if (i == 2) { chglim = (chglim + 1) & 3; chg_apply(); save_state(); } break;
	case P_SYSTEM: if (i == 2) push(P_BATT); else if (i == 0) push(P_POWER); break;
	case P_POWER:
		if (i == 0) off_disc_i = (off_disc_i + 1) % 6; else if (i == 1) off_conn_i = (off_conn_i + 1) % 6;
		else if (i == 2) { power_menu_open(); break; }
		save_state();
		break;
	case P_USB: if (i == 0) { adb_toggle(); flash(adb_on ? "Cambiando a transferir..." : "Encendiendo ADB..."); } else if (i == 1) usb_now(); break;
	case P_BT:
		if (i == 0) bt_start_job(BJ_POWER, NULL, !bt_on);
		else if (i == 1) bt_start_job(BJ_SCAN, NULL, 0);
		else if (i == 50) {   // siguiente códec: Auto y luego los que admite el auricular conectado (o todos si no hay)
			int n = sizeof BT_BEST / sizeof *BT_BEST, k = -1;
			for (int j = 0; j < n; j++) if (!strcmp(bt_codec_pref, BT_BEST[j])) k = j;
			do k++; while (k < n && bt_pcm[0] && !has_word(bt_codecs, BT_BEST[k]));
			snprintf(bt_codec_pref, sizeof bt_codec_pref, "%s", k < n ? BT_BEST[k] : "");
			save_state();
			bt_start_job(BJ_CODEC, NULL, 0);
		}
		else if (i == 51) { ldac_q = (ldac_q + 1) % 4; save_state(); bt_start_job(BJ_HEAL, NULL, 0); }   // reinicia bluealsa con la calidad nueva
		else if (i == 52) { sniff_m = (sniff_m + 1) % 3; save_state(); }
		else if (i >= 2 && i - 2 < nbt) bt_dev_dialog(i - 2);
		break;
	}
	render();
}

// Capa táctil de la lista (persistente): arrastre vertical = desplazar con inercia (el horizontal se ignora); toque = elegir;
// pulsación larga = acciones. Las filas se recrean al desplazar, por eso los gestos se resuelven aquí.
static int dragging, axis, drag_x0, drag_y0, drag_last, drag_row, lp_done;
static uint32_t drag_t, press_t0;
static void sheet_open(int row);
static void coll_open(int row);
static void list_dialog(int row);
static float vel;
static lv_timer_t *fling_t;
static void redraw_list(void);
static int is_coll_page(void) { int k = kind(); return k == P_ALBUMS || k == P_ARTISTS || k == P_ARTIST; }
static int is_track_page(void) { int k = kind(); return k == P_SONGS || k == P_FAVS || k == P_QUEUE || k == P_ALBUM || k == P_PLAYLIST; }
static int max_off(void) { int m = nrows * rh - (int)lv_obj_get_height(body); return m < 0 ? 0 : m; }
static void fling_stop(void) { if (fling_t) { lv_timer_delete(fling_t); fling_t = NULL; } }
static uint32_t fling_t0;
static float fling_acc;
static void fling_cb(lv_timer_t *t) {
	uint32_t now = tick_ms(), dt = now - fling_t0;
	fling_t0 = now;
	if (dt > 100) dt = 100;
	Pg *g = cur();
	fling_acc += vel * dt;
	int step = (int)fling_acc, off = g->off + step, m = max_off();
	fling_acc -= step;
	g->off = off < 0 ? 0 : off > m ? m : off;
	vel *= expf(-(float)dt / 450.0f);
	redraw_list();
	if (fabsf(vel) < 0.03f || g->off == 0 || g->off == m) fling_stop();
}
static int sw_track(int row);
static void swipe_move(int row, int dx);
static void swipe_end(int row, int dx);
static int row_at(int y) { return (y - (int)lv_obj_get_y(body) + cur()->off) / rh; }
static int touch_log;   // R1_TOUCHLOG=1: registra toques y ciclos lentos de la interfaz en stderr
static void hit_event(lv_event_t *e) {
	lv_event_code_t c = lv_event_get_code(e);
	lv_point_t p;
	lv_indev_get_point(lv_indev_active(), &p);
	if (touch_log && (c == LV_EVENT_PRESSED || c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST)) fprintf(stderr, "touch: %s x=%d y=%d fila=%d desplaz=%d\n", c == LV_EVENT_PRESSED ? "pulsa" : c == LV_EVENT_RELEASED ? "suelta" : "perdido", (int)p.x, (int)p.y, row_at(p.y), cur()->off);
	Pg *g = cur();
	if (c == LV_EVENT_PRESSED) {
		fling_stop();
		drag_x0 = p.x; drag_y0 = drag_last = p.y; drag_t = press_t0 = tick_ms(); axis = dragging = lp_done = 0; vel = 0;
		drag_row = row_at(p.y);
		press_row = drag_row < nrows ? drag_row : -1;
		redraw_list();
	} else if (c == LV_EVENT_PRESSING) {
		int dx = p.x - drag_x0, dy = p.y - drag_y0;
		if (!axis && (abs(dx) > 16 || abs(dy) > 16)) {
			axis = abs(dx) > abs(dy) ? 2 : 1; dragging = 1; press_row = -1;
			if (axis == 1 && kind() == P_SEARCH && kb_on) { kb_on = 0; layout(); bar_title_set(); }
		}
		if (!axis && !lp_done && row_item(drag_row) >= 0 && tick_ms() - press_t0 > 500) {   // pulsación larga: seleccionar (o marcar el rango)
			lp_done = 1; press_row = -1;
			if (sel_on) sel_range(drag_row); else { sel_start(drag_row); kb_on = 0; }
			list_gen++; render();
			return;
		}
		if (axis == 2 && !sel_on && sw_track(drag_row) >= 0) { swipe_move(drag_row, dx); return; }
		if (axis == 1 && abs(p.y - drag_last) >= 3) {
			uint32_t now = tick_ms();
			vel = vel * 0.5f + 0.5f * (float)(drag_last - p.y) / (float)(now > drag_t ? now - drag_t : 1);
			drag_t = now;
			int off = g->off + (drag_last - p.y), m = max_off();
			g->off = off < 0 ? 0 : off > m ? m : off;
			drag_last = p.y;
			redraw_list();
		}
	} else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) {
		int rel = c == LV_EVENT_RELEASED;
		press_row = -1;
		if (lp_done) { redraw_list(); return; }
		if (axis == 2 && !sel_on && sw_track(drag_row) >= 0) { swipe_end(drag_row, rel ? p.x - drag_x0 : 0); return; }
		if (rel && !dragging && sel_on) {   // en modo selección el toque marca o desmarca
			if (row_item(drag_row) >= 0) { sel_toggle(drag_row); if (!sel_n) sel_clear(); list_gen++; render(); } else redraw_list();
		} else if (rel && !dragging && drag_row >= 0 && drag_row < nrows) {
			g->sel = drag_row; do_select();
		} else if (rel && axis == 1 && tick_ms() - drag_t < 80 && fabsf(vel) > 0.15f) { fling_t0 = tick_ms(); fling_acc = 0; vel *= 1.2f; fling_t = lv_timer_create(fling_cb, 16, NULL); }
		else redraw_list();
	}
}

static void tap_gesture(void);
static void info_open(int ti);
// Mini-reproductor: tocar a la izquierda abre el reproductor, a la derecha es el gesto de toques (el panel se abre desde la esquina, pz_event).
static void np_event(lv_event_t *e) {
	lv_event_code_t c = lv_event_get_code(e);
	lv_point_t p;
	lv_indev_get_point(lv_indev_active(), &p);
	if (c == LV_EVENT_CLICKED && !pn_root) {
		if (p.x > W - 110) tap_gesture();
		else { push(P_PLAYER); render(); }
	}
}
static void bar_back_event(lv_event_t *e) { back(); }
static void nav_event(lv_event_t *e) { panel_close(); nav_root(tabs[(int)(intptr_t)lv_event_get_user_data(e)]); }
static void side_event(lv_event_t *e) {
	switch ((int)(intptr_t)lv_event_get_user_data(e)) {
	case 0: if (cur_track >= 0) { tracks[cur_track].fav = !tracks[cur_track].fav; save_state(); flash(tracks[cur_track].fav ? "Favorito añadido" : "Favorito quitado"); } break;
	case 1: shuf = !shuf; q_reorder(); save_state(); flash(shuf ? "Aleatorio activado" : "Aleatorio apagado"); break;
	case 2: rep = (rep + 1) % 3; save_state(); { static const char *n[] = {"apagado", "todo", "pista"}; char m[40]; snprintf(m, sizeof m, T("Repetir: %s"), n[rep]); flash(m); } break;
	case 3: push(P_QUEUE); break;
	case 4: go(-1); break;
	case 5: toggle_pause(); break;
	case 6: go(1); break;
	}
	render();
}
// ---- Gesto por número de toques: 1 pausa, 2 siguiente, 3 anterior (misma acción con jack o Bluetooth) ----
static int taps;
static void tap_cb(lv_timer_t *t) {
	int n = taps;
	taps = 0;
	lv_timer_pause(t);
	if (n == 1) toggle_pause();
	else if (n == 2) go(1);
	else if (n == 3) go(-1);
	if (n && n < 4) render();
}
static void tap_gesture(void) {
	static lv_timer_t *t;
	taps++;
	if (!t) t = lv_timer_create(tap_cb, 300, NULL);
	lv_timer_resume(t);
	lv_timer_reset(t);
}
// Carátula y títulos: arrastrar en horizontal mueve la carátula y cambia de pista al soltar (más de un cuarto del ancho o gesto rápido);
// hacia abajo cierra el reproductor, hacia arriba abre la cola; un toque (menos de 16 px) pausa o reanuda.
static void cover_event(lv_event_t *e) {
	static int x0, y0, axis, done;
	static uint32_t t0;
	lv_event_code_t c = lv_event_get_code(e);
	lv_point_t p;
	lv_indev_get_point(lv_indev_active(), &p);
	int dx = p.x - x0, dy = p.y - y0, base = (W - COV_B) / 2;
	if (c == LV_EVENT_PRESSED) { x0 = p.x; y0 = p.y; t0 = tick_ms(); axis = done = 0; }
	else if (c == LV_EVENT_PRESSING && !done) {
		if (!axis && (abs(dx) > 16 || abs(dy) > 16)) axis = abs(dx) > abs(dy) ? 1 : 2;
		if (!axis && cur_track >= 0 && tick_ms() - t0 > 600) { done = 1; info_open(cur_track); return; }
		if (axis == 1 && pl_art) lv_obj_set_x(pl_art, base + dx);
		else if (axis == 2 && abs(dy) > 90) { done = 1; if (dy > 0) back(); else { push(P_QUEUE); render(); } }
	} else if (c == LV_EVENT_RELEASED && !done) {
		int dt = tick_ms() - t0 + 1;
		if (axis == 1 && (abs(dx) > COV_B / 4 || (abs(dx) > 40 && abs(dx) * 1000 / dt > 600))) {
			go(dx < 0 ? 1 : -1);
			render();
			if (pl_art) slide(pl_art, base + (dx < 0 ? 140 : -140), base, 110, 0);
		} else if (axis == 1) { if (pl_art) slide(pl_art, base + dx, base, 90, 0); }
		else if (!axis) tap_gesture();
	}
}
// Salta a un segundo concreto de la pista actual (la interfaz lo refleja al instante).
static void seek_to(int sec) {
	int dur = total_s ? total_s : cur_track >= 0 ? tracks[cur_track].m.dur : 0;
	if (cur_track < 0 || dur <= 0) return;
	sec = sec < 0 ? 0 : sec >= dur ? dur - 1 : sec;
	seek_req = sec; pos_s = sec;
	char t1[16], t2[16], m[48];
	fmt_time(t1, sizeof t1, sec); fmt_time(t2, sizeof t2, dur);
	snprintf(m, sizeof m, "%s / %s", t1, t2);
	flash(m);
}
static void seek_by(int delta) { seek_to((seek_req >= 0 ? seek_req : pos_s) + delta); }

static void change_volume(int delta) { set_volume(vol + delta); char m[40]; snprintf(m, sizeof m, T("volumen %d"), vol); flash(m); }

static int in_fd[2];
static uint32_t key_t0[2], key_last[2];
static int key_down[2], key_fd[2], key_seeking[2], pw_down, pw_long;
static uint32_t pw_t0, pw_tap;   // pw_tap: momento de una pulsación corta pendiente (pantalla encendida)
// Estado real de una tecla (por si se perdió el evento de soltado).
static int key_pressed(int fd, int code) {
	unsigned char b[KEY_MAX / 8 + 1];
	memset(b, 0, sizeof b);
	if (ioctl(fd, EVIOCGKEY(sizeof b), b) < 0) return 1;
	return b[code / 8] >> (code % 8) & 1;
}
// Botones del auricular Bluetooth (AVRCP): aparecen como dispositivos event4+ mientras está conectado.
static int av_fd[6] = {-1, -1, -1, -1, -1, -1};
static void avrcp_scan(void) {
#ifdef R1_SIM
	return;
#endif
	for (int i = 0; i < 6; i++) {
		char p[32], name[80] = "";
		if (av_fd[i] >= 0) continue;
		snprintf(p, sizeof p, "/dev/input/event%d", 4 + i);
		int fd = open(p, O_RDONLY | O_NONBLOCK);
		if (fd < 0) continue;
		if (ioctl(fd, EVIOCGNAME(sizeof name), name) >= 0 && strstr(name, "AVRCP")) av_fd[i] = fd; else close(fd);
	}
}
static void avrcp_read(void) {
	static uint32_t scan_t;
	uint32_t now = tick_ms();
	if (now - scan_t > 3000) { scan_t = now; avrcp_scan(); }
	for (int i = 0; i < 6; i++) {
		struct input_event e;
		ssize_t r;
		if (av_fd[i] < 0) continue;
		while ((r = read(av_fd[i], &e, sizeof e)) == sizeof e) {
			if (e.type != EV_KEY || e.value != 1) continue;
			switch (e.code) {
			case KEY_PLAYCD: case KEY_PAUSECD: case KEY_PLAYPAUSE: case KEY_PLAY: case KEY_PAUSE: toggle_pause(); break;
			case KEY_NEXTSONG: go(1); break;
			case KEY_PREVIOUSSONG: go(-1); break;
			case KEY_FASTFORWARD: seek_by(10); break;
			case KEY_REWIND: seek_by(-10); break;
			}
		}
		if (r < 0 && errno != EAGAIN) { close(av_fd[i]); av_fd[i] = -1; }   // el auricular se desconectó
	}
}
// Botón de encendido: una pulsación enciende la pantalla al instante o la apaga tras 400 ms; dos seguidas (también justo al
// encenderla) llevan a Música y cierran paneles y diálogos. Mantenido: menú de apagado.
static void dlg_close(void);
static void sheet_close(void);
static void power_tap(void) {
	static uint32_t wake;
	uint32_t now = tick_ms();
	if (scr_off) { screen_set(1); pw_tap = 0; wake = now; return; }
	if (!pw_tap && now - wake > 400) { pw_tap = now ? now : 1; return; }
	pw_tap = 0; wake = 0;
	panel_close(); dlg_close(); sheet_close();
	nav_root(P_MUSIC);
}
static void handle_keys(void) {
	avrcp_read();
	for (int i = 0; i < 2; i++) {
		struct input_event e;
		while (read(in_fd[i], &e, sizeof e) == sizeof e) {
			if (e.type != EV_KEY) continue;
			if (e.code == KEY_NEXTSONG || e.code == KEY_PLAYPAUSE) {
				int k = e.code == KEY_NEXTSONG ? 0 : 1;
				if (e.value == 1) { key_down[k] = 1; key_fd[k] = i; key_t0[k] = tick_ms(); key_last[k] = 0; key_seeking[k] = 0; }
				else if (e.value == 0) { if (key_down[k] && !key_seeking[k]) go(k ? -1 : 1); key_down[k] = 0; }
				continue;
			}
			lv_display_trigger_activity(NULL);
			if (e.code == KEY_POWER) {
				if (e.value == 1) { pw_down = 1; pw_long = 0; pw_t0 = tick_ms(); }
				else if (e.value == 0) { if (pw_down && !pw_long) power_tap(); pw_down = 0; }
				continue;
			}
			if (e.value != 1) continue;
			if (e.code == KEY_VOLUMEUP) change_volume(2);
			else if (e.code == KEY_VOLUMEDOWN) change_volume(-2);
		}
	}
	uint32_t now = tick_ms();
	if (pw_down && !pw_long && now - pw_t0 > 900) { pw_long = 1; pw_tap = 0; screen_set(1); power_menu_open(); }
	if (pw_tap && now - pw_tap > 400) { pw_tap = 0; screen_set(0); }
	for (int k = 0; k < 2; k++)
		if (key_down[k] && now - key_t0[k] > 600 && now - key_last[k] >= 250) {
			if (!key_pressed(in_fd[key_fd[k]], k ? KEY_PLAYPAUSE : KEY_NEXTSONG)) { key_down[k] = 0; continue; }
			if (!key_seeking[k]) fprintf(stderr, "tecla %s mantenida: salto de pista\n", k ? "anterior" : "siguiente");
			key_seeking[k] = 1; key_last[k] = now;
			seek_by((k ? -1 : 1) * (now - key_t0[k] > 2500 ? 15 : 5));
		}
}

// ---- Dibujo ----
// Tocar o arrastrar la barra de progreso (línea de 440 px desde x = 20): la perilla sigue al dedo y el salto se hace al soltar.
static int scrub = -1;
static void bar_event(lv_event_t *e) {
	lv_event_code_t c = lv_event_get_code(e);
	if (c == LV_EVENT_RELEASED) { if (scrub >= 0) seek_to(scrub); scrub = -1; return; }
	if (c == LV_EVENT_PRESS_LOST) { scrub = -1; return; }
	if (c != LV_EVENT_PRESSED && c != LV_EVENT_PRESSING) return;
	lv_point_t p;
	lv_indev_get_point(lv_indev_active(), &p);
	int dur = total_s ? total_s : cur_track >= 0 ? tracks[cur_track].m.dur : 0;
	if (dur <= 0) return;
	int x = p.x < 20 ? 0 : p.x > 460 ? 440 : p.x - 20;
	scrub = dur * x / 440;
	if (scrub >= dur) scrub = dur - 1;
	refresh(NULL);
}

// Deslizador de volumen (0) o brillo (1): el valor sale de la posición del toque dentro del objeto.
static void slider_event(lv_event_t *e) {
	lv_event_code_t c = lv_event_get_code(e);
	if (c == LV_EVENT_RELEASED) { save_state(); return; }
	if (c != LV_EVENT_PRESSED && c != LV_EVENT_PRESSING) return;
	lv_point_t p;
	lv_area_t a;
	lv_indev_get_point(lv_indev_active(), &p);
	lv_obj_get_coords(lv_event_get_target_obj(e), &a);
	int v = (p.x - a.x1) * 100 / (a.x2 - a.x1 + 1);
	v = v < 0 ? 0 : v > 100 ? 100 : v;
	if ((int)(intptr_t)lv_event_get_user_data(e) == 0) set_volume(v); else set_bright(bl_from_pct(v));
	refresh(NULL);
}

// Pool de filas: se crean una vez y se reutilizan al desplazar; solo se actualiza el contenido de la fila que entra.
enum { NSLOT = VIS + 1 };
typedef struct { lv_obj_t *row, *ic, *l, *s, *d, *box, *chk; int i, sel, pr, y; unsigned gen; } Slot;   // box/chk: casilla del modo selección
static Slot slots[NSLOT];
static lv_obj_t *sw_bg, *sw_ic;   // fondo que asoma bajo una fila arrastrada en horizontal (gesto lateral)
static Slot *slot_of(int row) { Slot *o = &slots[(row % NSLOT + NSLOT) % NSLOT]; return row >= 0 && o->row && o->i == row ? o : NULL; }
// Gesto lateral en filas de canciones (fuera del modo selección): la fila sigue al dedo; pasado un tercio del ancho, a la derecha se
// añade al final de la cola y a la izquierda se quita (solo en la cola, una lista propia y Favoritas). Mismo sentido, misma acción.
static int sw_track(int row) {
	int it = row_item(row), ty = it >> 24, v = it & 0xffffff;
	if (it < 0) return -1;
	return ty == 3 ? v : ty == 5 ? qa[v] : ty == 6 ? pls[ctx_pl].tr[v] : -1;
}
static int sw_can_remove(void) { int k = kind(); return k == P_QUEUE || k == P_PLAYLIST || k == P_FAVS; }
static void swipe_move(int row, int dx) {
	Slot *o = slot_of(row);
	if (!o || !sw_bg) return;
	if (dx < 0 && !sw_can_remove()) dx /= 4;   // sin acción a la izquierda: resistencia
	int arm = abs(dx) > W / 3;
	lv_obj_set_y(sw_bg, o->y);
	lv_obj_remove_flag(sw_bg, LV_OBJ_FLAG_HIDDEN);
	lv_obj_set_style_bg_color(sw_bg, C(arm ? (dx > 0 ? SK.a1 : 0xd2453c) : SK.bg2), 0);
	set_text(sw_ic, dx > 0 ? ICO_PLUS : ICO_TRASH);
	lv_obj_set_style_text_color(sw_ic, C(arm ? SK.onsel : SK.dim), 0);
	lv_obj_align(sw_ic, dx > 0 ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID, dx > 0 ? 28 : -28, 0);
	lv_obj_set_x(o->row, RM + dx);
}
static void swipe_end(int row, int dx) {
	Slot *o = slot_of(row);
	int t = sw_track(row), k = kind();
	if (sw_bg) lv_obj_add_flag(sw_bg, LV_OBJ_FLAG_HIDDEN);
	if (o) lv_obj_set_x(o->row, RM);
	if (t < 0 || abs(dx) <= W / 3 || (dx < 0 && !sw_can_remove())) return;
	if (dx > 0) { pthread_mutex_lock(&qlock); q_append_l(&t, 1); pthread_mutex_unlock(&qlock); flash("Añadida a la cola"); return; }
	if (k == P_QUEUE) { pthread_mutex_lock(&qlock); q_remove_l(row_q(row)); pthread_mutex_unlock(&qlock); flash("Quitada de la cola"); }
	else if (k == P_FAVS) { tracks[t].fav = 0; save_state(); flash("Favorito quitado"); }
	else if (k == P_PLAYLIST) {
		Playlist *p = &pls[ctx_pl];
		if (row < p->n) { memmove(p->tr + row, p->tr + row + 1, (p->n - row - 1) * sizeof *p->tr); p->n--; }
		flash(pl_save(ctx_pl) ? "Quitada de la lista" : "No se pudo guardar la lista");
	}
	render();
}
static void pool_reset(void) { memset(slots, 0, sizeof slots); sw_bg = sw_ic = NULL; }
static void pool_create(void) {
	sw_bg = rect(body, RM, 0, W - 2 * RM, rh, SK.bg2);   // primero: queda detrás de las filas
	lv_obj_set_style_radius(sw_bg, RAD, 0);
	sw_ic = label(sw_bg, &ui_icons_34, SK.onsel, "");
	lv_obj_add_flag(sw_bg, LV_OBJ_FLAG_HIDDEN);
	for (int k = 0; k < NSLOT; k++) {
		Slot *o = &slots[k];
		o->i = -1;
		o->row = rect(body, RM, 0, W - 2 * RM, rh, SK.bg);
		o->ic = label(o->row, &ui_icons_30, SK.dim, "");
		o->l = label(o->row, F30, SK.fg, "");
		lv_label_set_long_mode(o->l, LV_LABEL_LONG_DOT);
		o->s = label(o->row, F24, SK.dim, "");
		lv_label_set_long_mode(o->s, LV_LABEL_LONG_DOT);
		o->d = label(o->row, F26, SK.dim, "");
		o->box = rect(o->row, 20, 0, 36, 36, SK.bg);
		lv_obj_set_style_radius(o->box, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_opa(o->box, LV_OPA_TRANSP, 0);
		o->chk = label(o->box, &ui_icons_26, SK.onsel, ICO_CHECK);
		lv_obj_center(o->chk);
	}
}

static void slot_bind(Slot *o, int i, int selected) {
	const Row *r = rowp(i);
	lv_obj_set_size(o->row, W - 2 * RM, rh);
	lv_obj_set_style_bg_color(o->row, C(selected ? SK.a1 : i == press_row ? SK.bg2 : SK.bg), 0);
	lv_obj_set_style_radius(o->row, selected ? RAD : 0, 0);
	border(o->row, selected ? 0 : 1, SK.bg2, LV_BORDER_SIDE_BOTTOM);
	uint32_t fg = selected ? SK.onsel : SK.fg, dim = selected ? SK.onsel : SK.dim;
	char d[40];
	snprintf(d, sizeof d, "%s%s", r->d, r->chev ? (r->d[0] ? "  ›" : "›") : "");
	int has_sub = r->sub[0], th = lv_font_get_line_height(F30), sh = lv_font_get_line_height(F24);
	int pick = sel_on && row_item(i) >= 0, chosen = pick && sel_find(row_item(i)) >= 0;
	int rw = d[0] ? lv_text_get_width(d, strlen(d), F26, 0) + 28 : 0, lx = 20 + (r->ic || pick ? 52 : 0);
	int ty = has_sub ? (rh - th - sh - 2) / 2 : (rh - th) / 2;
	if (pick) {   // casilla: rellena con el acento si está elegida, solo el borde si no
		lv_obj_set_style_bg_color(o->box, C(SK.a1), 0);
		lv_obj_set_style_bg_opa(o->box, chosen ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
		border(o->box, 2, chosen ? SK.a1 : SK.dim, LV_BORDER_SIDE_FULL);
		lv_obj_update_flag(o->chk, LV_OBJ_FLAG_HIDDEN, !chosen);
		lv_obj_align(o->box, LV_ALIGN_LEFT_MID, 18, 0);
		lv_obj_remove_flag(o->box, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(o->ic, LV_OBJ_FLAG_HIDDEN);
	} else lv_obj_add_flag(o->box, LV_OBJ_FLAG_HIDDEN);
	if (r->ic && !pick) { set_text(o->ic, r->ic); lv_obj_set_style_text_color(o->ic, C(r->icc && !selected ? r->icc : dim), 0); lv_obj_align(o->ic, LV_ALIGN_LEFT_MID, 20, 0); lv_obj_remove_flag(o->ic, LV_OBJ_FLAG_HIDDEN); }
	else lv_obj_add_flag(o->ic, LV_OBJ_FLAG_HIDDEN);
	set_text(o->l, r->l);
	lv_obj_set_style_text_color(o->l, C(fg), 0);
	lv_obj_set_size(o->l, W - 2 * RM - lx - 8 - rw, th);
	lv_obj_align(o->l, LV_ALIGN_TOP_LEFT, lx, ty);
	if (has_sub) {
		set_text(o->s, r->sub);
		lv_obj_set_style_text_color(o->s, C(dim), 0);
		lv_obj_set_size(o->s, W - 2 * RM - lx - 8 - rw, sh);
		lv_obj_align(o->s, LV_ALIGN_TOP_LEFT, lx, ty + th + 2);
		lv_obj_remove_flag(o->s, LV_OBJ_FLAG_HIDDEN);
	} else lv_obj_add_flag(o->s, LV_OBJ_FLAG_HIDDEN);
	if (d[0]) { set_text(o->d, d); lv_obj_set_style_text_color(o->d, C(dim), 0); lv_obj_align(o->d, LV_ALIGN_RIGHT_MID, -20, 0); lv_obj_remove_flag(o->d, LV_OBJ_FLAG_HIDDEN); }
	else lv_obj_add_flag(o->d, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *centered(lv_obj_t *l, int y, int h) {
	lv_obj_set_size(l, 448, h);
	lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
	lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_pos(l, 16, y);
	return l;
}

// Reproducción (pantalla completa, la barra superior flota encima): carátula de 480 px fundida con el fondo, título, artista y álbum
// sobre el fundido, progreso 90 px y controles 120 px.
// En pausa: insignia de pausa sobre la carátula, progreso en gris y botón central en Play.
// Forma de onda: una barra de 3 px cada 4 px, acento las ya reproducidas y gris las demás.
static void wv_draw(lv_event_t *e) {
	lv_layer_t *ly = lv_event_get_layer(e);
	lv_area_t a;
	lv_draw_rect_dsc_t d;
	lv_obj_get_coords(lv_event_get_target_obj(e), &a);
	lv_draw_rect_dsc_init(&d);
	d.radius = 1;
	for (int i = 0; i < WV_N; i++) {
		int h = 4 + wv_data[i] * 42 / 255, y = a.y1 + 24 - h / 2;
		lv_area_t r = {a.x1 + i * 4, y, a.x1 + i * 4 + 2, y + h - 1};
		d.bg_color = i < wv_split ? C(paused ? SK.dim : SK.a1) : lv_color_mix(C(SK.dim), C(SK.bg), 90);
		lv_draw_rect(ly, &d, &r);
	}
}
static lv_obj_t *ctl_btn(int x, int w, const lv_font_t *f, uint32_t col, const char *ico, int id) {
	lv_obj_t *b = rect(body, x, 662, w, 120, SK.bg);
	lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
	pressfx(b, SK.bg2);
	lv_obj_add_event_cb(b, side_event, LV_EVENT_CLICKED, (void *)(intptr_t)id);
	lv_obj_t *l = label(b, f, col, ico);
	lv_obj_center(l);
	return l;
}
static void draw_player_page(void) {
	Track *t = cur_track >= 0 ? &tracks[cur_track] : NULL;
	int stop = cur_track < 0 || paused;
	lv_obj_t *art = rect(body, 0, 0, COV_B, COV_B, SK.bg);
	pl_art = art;
	uint16_t *k = cover_baked();
	if (k) {
		cov_dsc(&cov_b_dsc, k, COV_B);
		lv_image_set_src(lv_image_create(art), &cov_b_dsc);
	} else { lv_obj_t *d = label(art, &ui_icons_46, SK.a1, ICO_DISC); lv_obj_align(d, LV_ALIGN_CENTER, 0, -30); }
	if (t && paused) {
		lv_obj_t *bd = rect(art, (COV_B - 104) / 2, 150, 104, 104, SK.bg2);
		lv_obj_set_style_radius(bd, LV_RADIUS_CIRCLE, 0);
		border(bd, 2, SK.line, LV_BORDER_SIDE_FULL);
		lv_obj_center(label(bd, &ui_icons_46, SK.fg, ICO_PAUSE));
	}
	char al[200];
	snprintf(al, sizeof al, "%s%s%s", t ? t->m.album : "", t && t->m.album[0] && t->m.fmt[0] ? " · " : "", t ? t->m.fmt : "");
	pl_title = centered(label(body, F38, SK.a1, t ? t->m.title : "Sin reproducción"), 422, lv_font_get_line_height(F38));
	lv_label_set_long_mode(pl_title, LV_LABEL_LONG_SCROLL_CIRCULAR);   // el título largo se desplaza; el resto, en la ficha técnica
	pl_artist = centered(label(body, F28, SK.a2, t ? t->m.artist : ""), 472, lv_font_get_line_height(F28));
	pl_album = centered(label(body, F24, SK.dim, al), 510, lv_font_get_line_height(F24));
	lv_obj_t *gz = rect(body, 0, BAR_H, W, 544 - BAR_H, SK.bg);
	lv_obj_set_style_bg_opa(gz, LV_OPA_TRANSP, 0);
	lv_obj_add_flag(gz, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(gz, cover_event, LV_EVENT_ALL, NULL);
	lv_obj_t *ph = rect(body, 0, 552, W, 90, SK.bg);
	lv_obj_add_flag(ph, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(ph, bar_event, LV_EVENT_ALL, NULL);
	uint32_t pc = paused ? SK.dim : SK.a1;
	if (t && t->wv_ok == 1) {
		pl_wv = rect(ph, 20, 5, 440, 48, SK.bg);
		wv_data = t->wv; wv_split = 0;
		lv_obj_add_event_cb(pl_wv, wv_draw, LV_EVENT_DRAW_MAIN, NULL);
	} else {
		lv_obj_t *pb = rect(ph, 20, 24, 440, 10, SK.bg2);
		lv_obj_set_style_radius(pb, 5, 0);
		pl_fill = rect(pb, 0, 0, 0, 10, pc);
		lv_obj_set_style_radius(pl_fill, 5, 0);
		pl_knob = rect(ph, 7, 16, 26, 26, pc);
		lv_obj_set_style_radius(pl_knob, LV_RADIUS_CIRCLE, 0);
	}
	pl_t1 = label(ph, F24, SK.fg, "0:00"); lv_obj_set_pos(pl_t1, 20, 54);
	pl_t2 = label(ph, F24, SK.dim, "-0:00"); lv_obj_align(pl_t2, LV_ALIGN_TOP_RIGHT, -20, 54);
	ctl_btn(0, 96, &ui_icons_34, shuf ? SK.a2 : SK.dim, ICO_SHUFFLE, 1);
	ctl_btn(96, 96, &ui_icons_46, SK.fg, ICO_PREV, 4);
	ctl_btn(288, 96, &ui_icons_46, SK.fg, ICO_NEXT, 6);
	ctl_btn(384, 96, &ui_icons_34, rep ? SK.a2 : SK.dim, ICO_REPEAT, 2);
	lv_obj_t *pp = rect(body, (W - 104) / 2, 670, 104, 104, SK.a1);
	lv_obj_set_style_radius(pp, LV_RADIUS_CIRCLE, 0);
	lv_obj_add_flag(pp, LV_OBJ_FLAG_CLICKABLE);
	pressfx(pp, SK.dim);
	lv_obj_add_event_cb(pp, side_event, LV_EVENT_CLICKED, (void *)(intptr_t)5);
	pl_pp = label(pp, &ui_icons_46, SK.onsel, stop ? ICO_PLAY : ICO_PAUSE);
	lv_obj_center(pl_pp);
	if (stop) lv_obj_set_x(pl_pp, 4);   // centro óptico del triángulo
}

// ---- Animaciones (cortas; solo posición, sin desvanecidos que obliguen a recomponer pantalla completa) ----
static void anim_x(void *o, int32_t v) { lv_obj_set_x(o, v); }
static void anim_y(void *o, int32_t v) { lv_obj_set_y(o, v); }
static void slide(lv_obj_t *o, int from, int to, int ms, int vertical) {
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, o);
	lv_anim_set_values(&a, from, to);
	lv_anim_set_duration(&a, ms);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_set_exec_cb(&a, vertical ? anim_y : anim_x);
	lv_anim_start(&a);
}

// Arriba el mini-reproductor y la barra de título; abajo la barra de secciones. Reproduciendo y el teclado ocultan el mini-reproductor y las secciones.
static void layout(void) {
	int id = cur()->id, pl = id == P_PLAYER, sr = id == P_SEARCH || id == P_NEWLIST || id == P_WIFIPW, kb = sr && (kb_on || id != P_SEARCH), bh = H - BAR_H - (pl ? 0 : kb ? KB_H : MINI_H + NAV_H);   // con el teclado no hay mini-reproductor arriba
	rh = id == P_SET ? ROW_S : ROW_H;
	lv_obj_update_flag(bar_fav, LV_OBJ_FLAG_HIDDEN, !pl);
	lv_obj_update_flag(bar_q, LV_OBJ_FLAG_HIDDEN, !pl);
	lv_obj_update_flag(hit, LV_OBJ_FLAG_HIDDEN, pl);
	lv_obj_update_flag(np, LV_OBJ_FLAG_HIDDEN, pl || kb);
	lv_obj_update_flag(navbar, LV_OBJ_FLAG_HIDDEN, pl || kb || sel_on);
	lv_obj_update_flag(actbar, LV_OBJ_FLAG_HIDDEN, !sel_on);
	lv_obj_set_height(pz, pl || kb ? BAR_H : MINI_H + BAR_H);   // en Reproduciendo y con teclado solo la barra de arriba
	lv_obj_update_flag(bar_r, LV_OBJ_FLAG_HIDDEN, (pl || sr) && !sel_on);
	if (kb_root) lv_obj_update_flag(kb_root, LV_OBJ_FLAG_HIDDEN, !kb);
	int top = pl || kb ? 0 : MINI_H;
	lv_obj_set_y(bar, top);
	lv_obj_set_pos(body, 0, pl ? 0 : top + BAR_H); lv_obj_set_height(body, pl ? H : bh);   // en Reproduciendo la barra flota sobre la carátula
	lv_opa_t bo = pl ? LV_OPA_TRANSP : LV_OPA_COVER;
	lv_obj_set_style_bg_opa(bar, bo, 0); lv_obj_set_style_bg_opa(bar_back, bo, 0); lv_obj_set_style_bg_opa(bar_fav, bo, 0); lv_obj_set_style_bg_opa(bar_q, bo, 0);
	lv_obj_set_style_border_width(bar, pl ? 0 : 1, 0);
	lv_obj_set_pos(hit, 0, top + BAR_H); lv_obj_set_height(hit, bh);
}

static void redraw_list(void) {
	Pg *c = cur();
	if (!slots[0].row) pool_create();
	int first = c->off / rh, frac = c->off % rh;
	for (int k = 0; k < NSLOT; k++) {
		int i = first + k;
		Slot *o = &slots[i % NSLOT];
		if (i >= nrows) { lv_obj_add_flag(o->row, LV_OBJ_FLAG_HIDDEN); o->i = -1; continue; }
		int y = k * rh - frac, sel = row_hl(i), pr = i == press_row;
		if (o->i != i || o->sel != sel || o->pr != pr || o->gen != list_gen) {
			slot_bind(o, i, sel);
			o->i = i; o->sel = sel; o->pr = pr; o->gen = list_gen;
		}
		lv_obj_remove_flag(o->row, LV_OBJ_FLAG_HIDDEN);
		if (o->y != y) { lv_obj_set_y(o->row, y); o->y = y; }
	}
}

// Título de la barra superior; los avisos breves (volumen, carga, Bluetooth) lo reemplazan en color de acento.
static void bar_title_set(void) {
	int fl = hint_until && (int)(tick_ms() - hint_until) < 0, pl = cur()->id == P_PLAYER, x = sp > 0 || pl ? 72 : 20;
	const char *m = fl ? flash_msg : bt_wait ? "Bluetooth: reconectando..." : loading && !paused ? "Cargando..." : NULL;
	int id = cur()->id;
	char qb[72];
	const char *qs = id == P_NEWLIST ? nq : id == P_WIFIPW ? wq : sq;
	snprintf(qb, sizeof qb, "%s%s", qs, kb_on || id == P_NEWLIST || id == P_WIFIPW ? "|" : "");
	char st[48];
	snprintf(st, sizeof st, T("%d seleccionadas"), sel_n);
	const char *t = sel_on ? st : id == P_ARTIST ? artists[ctx_artist].name : id == P_ALBUM ? albums[ctx_album].name : id == P_PLAYLIST ? pls[ctx_pl].name
	              : id == P_SEARCH ? (sq[0] ? qb : "Buscar...") : id == P_NEWLIST ? (nq[0] ? qb : "Nombre de la lista") : id == P_WIFIPW ? (wq[0] ? qb : "Contraseña") : titles[id];
	set_text(bar_l, m ? m : pl ? "" : t);
	lv_color_t col = C(m ? SK.a1 : (id == P_SEARCH && !sq[0]) || (id == P_NEWLIST && !nq[0]) || (id == P_WIFIPW && !wq[0]) ? SK.dim : SK.fg);
	if (!lv_color_eq(lv_obj_get_style_text_color(bar_l, 0), col)) lv_obj_set_style_text_color(bar_l, col, 0);
	const char *r = lv_label_get_text(bar_r);
	int rw = pl ? 136 : (id == P_SEARCH || id == P_NEWLIST || id == P_WIFIPW) && !sel_on ? 12 : r[0] ? lv_text_get_width(r, strlen(r), F26, 0) + 32 : 12, w = W - x - rw;
	if ((int)lv_obj_get_width(bar_l) != w) lv_obj_set_width(bar_l, w);
}

// ---- Teclado de la búsqueda (380 px abajo, en lugar del mini-reproductor y la barra de secciones) ----
// Letras con Ñ, o números y signos con la tecla 123. Borrar quita un carácter; mantenido, toda la consulta.
// Ocultar (o desplazar los resultados) lo esconde; tocar la barra superior lo vuelve a mostrar.
static int kb_num;
static void search_update(void) { cur()->off = 0; build_rows(); list_gen++; redraw_list(); bar_title_set(); }
static void kb_create(void);
// Lista nueva: crea la lista con el nombre escrito (y lo pendiente) y vuelve adonde se estaba.
static void newlist_done(void) {
	char *a = nq, *z = nq + strlen(nq);
	while (*a == ' ') a++;
	while (z > a && z[-1] == ' ') *--z = 0;
	if (!*a) { flash("Escribe un nombre"); return; }
	int k = pl_new(a);
	if (k < 0) { flash("No se pudo crear la lista"); return; }
	if (pick_n) pick_apply(k); else flash("Lista creada");
	sp--;
	if (sp > 0 && stk[sp].id == P_PICK) sp--;
	kb_on = 0;
	render();
}
// Teclado compartido: edita la consulta (Buscar) o el nombre (Nueva lista). La tecla de la esquina oculta el teclado o crea la lista.
// Contraseña escrita: conecta a pw_net y vuelve a la lista de redes.
static void wifi_pw_done(void) {
	if (strlen(wq) < 8) { flash("Mínimo 8 caracteres"); return; }
	wlan_job(WJ_CONNECT, &pw_net, 0, wq);
	memset(wq, 0, sizeof wq);
	kb_on = 0; sp--; render();
}
static int kb_up;   // mayúsculas (o, con 123, más signos)
static void kb_edit(const char *k, int clear) {
	int nl = cur()->id == P_NEWLIST, pw = cur()->id == P_WIFIPW;
	char *q = nl ? nq : pw ? wq : sq;
	size_t n = strlen(q);
	if (clear) q[0] = 0;
	else if (*k == '\b') { while (n && ((unsigned char)q[n - 1] & 0xC0) == 0x80) n--; if (n) q[n - 1] = 0; }
	else if (*k == 1) { if (nl) newlist_done(); else if (pw) wifi_pw_done(); else { kb_on = 0; render(); } return; }
	else if (*k == 2 || *k == 3) { if (*k == 2) kb_num = !kb_num; else kb_up = !kb_up; lv_obj_delete_async(kb_root); kb_create(); return; }
	else if (n + strlen(k) < 64) strcat(q, k);
	if (nl || pw) bar_title_set(); else search_update();
}
static void kb_event(lv_event_t *e) {
	const char *k = lv_event_get_user_data(e);
	lv_event_code_t c = lv_event_get_code(e);
	if (*k == '\b' && c == LV_EVENT_LONG_PRESSED) kb_edit(k, 1);
	else if (c == LV_EVENT_CLICKED) kb_edit(k, 0);
}
static void kb_key(int x, int y, int w, const char *ins, const lv_font_t *f, const char *txt, int on) {
	lv_obj_t *b = rect(kb_root, x + 3, y, w - 6, KB_R - 8, on ? SK.a1 : SK.bg2);
	lv_obj_set_style_radius(b, 10, 0);
	lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
	pressfx(b, SK.a1);
	lv_obj_add_event_cb(b, kb_event, LV_EVENT_CLICKED, (void *)ins);
	lv_obj_add_event_cb(b, kb_event, LV_EVENT_LONG_PRESSED, (void *)ins);
	lv_obj_center(label(b, f, SK.fg, txt));
}
static void kb_create(void) {
	static const char *L[3][10] = {{"q", "w", "e", "r", "t", "y", "u", "i", "o", "p"}, {"a", "s", "d", "f", "g", "h", "j", "k", "l", "ñ"},
	                               {"z", "x", "c", "v", "b", "n", "m"}};
	static const char *N[3][10] = {{"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"}, {"-", "'", "&", ".", ",", "(", ")", "!", "?", "/"},
	                               {"+", ":", ";", "#", "@", "_", "\""}};
	static const char *S[3][10] = {{"[", "]", "{", "}", "%", "^", "*", "=", "$", "~"}, {"\\", "|", "<", ">", "`", "¡", "¿", "€", "£", "°"},
	                               {"á", "é", "í", "ó", "ú", "ü", "ç"}};
	static char U[3][10][4];
	kb_root = rect(lv_obj_get_parent(body), 0, H - KB_H, W, KB_H, SK.bg);
	border(kb_root, 1, SK.line, LV_BORDER_SIDE_TOP);
	for (int r = 0; r < 3; r++)
		for (int i = 0; i < 10 && L[r][i]; i++) {
			const char *k = kb_num ? (kb_up ? S : N)[r][i] : L[r][i];
			if (!kb_num && kb_up) {
				snprintf(U[r][i], sizeof U[r][i], "%s", strcmp(k, "ñ") ? k : "Ñ");
				if (U[r][i][0] >= 'a' && U[r][i][0] <= 'z') U[r][i][0] -= 32;
				k = U[r][i];
			}
			kb_key(i * 48, 10 + r * KB_R, 48, k, F38, k, 0);
		}
	int done = cur()->id == P_NEWLIST || cur()->id == P_WIFIPW;
	kb_key(7 * 48, 10 + 2 * KB_R, 3 * 48, "\b", &ui_icons_34, ICO_BACK, 0);
	kb_key(0, 10 + 3 * KB_R, 96, "\2", F26, kb_num ? "abc" : "123", 0);
	kb_key(96, 10 + 3 * KB_R, 72, "\3", F26, kb_num ? "#+=" : "Aa", kb_up);
	kb_key(168, 10 + 3 * KB_R, 192, " ", F26, "espacio", 0);
	kb_key(360, 10 + 3 * KB_R, 120, "\1", &ui_icons_34, done ? ICO_CHECK : ICO_DOWN, 0);
}

static int body_player;   // 1 = el cuerpo contiene el reproductor, 0 = el pool de filas
static void render(void) {
	Pg *c = cur();
	static int kb_page;
	int kbp = c->id == P_SEARCH || c->id == P_NEWLIST || c->id == P_WIFIPW;
	if (kb_root && (!kbp || kb_page != c->id)) { lv_obj_delete(kb_root); kb_root = NULL; }
	if (kbp && !kb_root) { kb_create(); kb_page = c->id; }
	if (sel_on && (sp != sel_depth || c->id != sel_page)) sel_clear();   // la selección vive en su pantalla
	layout();
	sel_bar_build();
	list_gen++;
	pl_art = pl_title = pl_fill = pl_knob = pl_wv = pl_pp = NULL;
	set_text(bar_ico, sel_on ? ICO_XMARK : c->id == P_PLAYER ? ICO_DOWN : ICO_BACK);
	int has_back = sp > 0 || c->id == P_PLAYER || sel_on;
	if (has_back) lv_obj_remove_flag(bar_back, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(bar_back, LV_OBJ_FLAG_HIDDEN);
	lv_obj_set_x(bar_l, has_back ? 72 : 20);
	for (int i = 0; i < NTAB; i++) {
		lv_color_t col = C(tabs[i] == stk[0].id ? SK.a1 : SK.dim);
		lv_obj_set_style_text_color(nav_ico[i], col, 0);
		lv_obj_set_style_text_color(nav_lbl[i], col, 0);
	}
	lv_obj_set_style_text_color(lv_obj_get_child(bar_fav, 0), C(cur_track >= 0 && tracks[cur_track].fav ? SK.a1 : SK.fg), 0);
	bar_title_set();
	if (c->id != P_PLAYER) {
		if (body_player) { lv_obj_clean(body); pool_reset(); body_player = 0; }   // quita los objetos del reproductor
		build_rows();
		if (c->off > max_off()) c->off = max_off();
		redraw_list();
	} else { lv_obj_clean(body); pool_reset(); draw_player_page(); body_player = 1; }
	if (nav_dir) { slide(body, nav_dir * 40, 0, 70, 0); nav_dir = 0; }
}

// ---- Panel rápido (se abre al tocar o arrastrar hacia abajo la barra de estado o la franja de reproducción) ----
// Toque: conectar el último dispositivo o desconectar el actual (enciende la radio si estaba apagada); pulsación larga: pantalla de Bluetooth.
static void panel_tile_event(lv_event_t *e) {
	panel_close();
	if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
		nav_root(P_SET); push(P_BT); bt_start_job(BJ_LIST, NULL, 0);
		render();
	} else bt_start_job(BJ_QUICK, NULL, 0);
}
static void panel_settings_event(lv_event_t *e) { panel_close(); nav_root(P_SET); }
static void panel_scrim_event(lv_event_t *e) { panel_close(); }
static void panel_standby_event(lv_event_t *e) { panel_close(); screen_set(0); }
static void panel_power_event(lv_event_t *e) { panel_close(); power_menu_open(); }
static void panel_tile(lv_obj_t *p, int x, int y, const char *ico, const char *t, const char *sub, int on) {
	lv_obj_t *o = rect(p, x, y, W - 28, 118, on ? SK.a1 : SK.bg);
	lv_obj_set_style_radius(o, RAD, 0);
	border(o, 1, on ? SK.a1 : SK.line, LV_BORDER_SIDE_FULL);
	lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(o, panel_tile_event, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(o, panel_tile_event, LV_EVENT_LONG_PRESSED, NULL);
	if (!on) pressfx(o, SK.line);
	uint32_t fg = on ? SK.onsel : SK.fg, dim = on ? SK.onsel : SK.dim;
	lv_obj_set_pos(label(o, &ui_icons_30, fg, ico), 18, 22);
	lv_obj_t *l = label(o, F28, fg, t), *s = label(o, F24, dim, sub);
	lv_label_set_long_mode(l, LV_LABEL_LONG_DOT); lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
	lv_obj_set_size(l, W - 28 - 78, lv_font_get_line_height(F28)); lv_obj_set_pos(l, 60, 20);
	lv_obj_set_size(s, W - 28 - 36, lv_font_get_line_height(F24)); lv_obj_set_pos(s, 18, 70);
}
static lv_obj_t *panel_slider(lv_obj_t *p, int y, const char *ico, int kind) {
	lv_obj_set_pos(label(p, &ui_icons_30, SK.fg, ico), 18, y + 16);
	lv_obj_t *h = rect(p, 70, y, 392, 64, SK.bg2);
	lv_obj_add_flag(h, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(h, slider_event, LV_EVENT_ALL, (void *)(intptr_t)kind);
	lv_obj_t *b = rect(h, 0, 26, 392, 12, SK.bg);
	lv_obj_set_style_radius(b, 6, 0);
	lv_obj_t *f = rect(b, 0, 0, 0, 12, SK.a1);
	lv_obj_set_style_radius(f, 6, 0);
	return f;
}
static lv_obj_t *panel_icon(lv_obj_t *p, const char *ico, int x, lv_event_cb_t cb) {
	lv_obj_t *l = label(p, &ui_icons_34, SK.dim, ico);
	lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_ADV_HITTEST);
	lv_obj_set_ext_click_area(l, 22);
	lv_obj_set_pos(l, x, 410);
	lv_obj_add_event_cb(l, cb, LV_EVENT_CLICKED, NULL);
	return l;
}
// Velo negro semitransparente que recibe el toque de cierre.
static lv_obj_t *veil(int y, int opa, lv_event_cb_t cb, void *ud) {
	lv_obj_t *v = rect(lv_screen_active(), 0, y, W, H - y, 0x000000);
	lv_obj_set_style_bg_opa(v, opa, 0);
	lv_obj_add_flag(v, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(v, cb, LV_EVENT_CLICKED, ud);
	return v;
}
// ---- Diálogo de acciones (Bluetooth): tarjeta con hasta 3 botones; Cancelar o tocar fuera cierra ----
enum { DA_CANCEL, DA_WCONNECT, DA_WFORGET, DA_CONNECT, DA_DISCONNECT, DA_FORGET_ASK, DA_FORGET, DA_PLAY_C, DA_NEXT_C, DA_ADD_C, DA_USB_DISK, DA_USB_CHARGE, DA_LIST_C, DA_DEL_LIST,
       DA_SEL_REMOVE, DA_SEL_QUEUE };
static void sel_confirmed(int act);
static void usb_choice(int disk);
static lv_obj_t *dl_root;
static int dl_ids[5], dl_coll_artist, dl_coll_idx;
static BtDev dl_dev;
static WNet dl_net;
static void dlg_close(void) { if (dl_root) { lv_obj_delete(dl_root); dl_root = NULL; } }
static void dlg_open(const char *title, const char *sub, int n, const char *const *names, const char *const *icos, const int *ids);
static void coll_action(int act);
static void dlg_event(lv_event_t *e) {
	int k = (int)(intptr_t)lv_event_get_user_data(e), act = k >= 0 ? dl_ids[k] : DA_CANCEL;
	dlg_close();
	switch (act) {
	case DA_USB_DISK: case DA_USB_CHARGE: usb_choice(act == DA_USB_DISK); return;
	case DA_CONNECT: bt_start_job(BJ_CONNECT, &dl_dev, 0); break;
	case DA_WCONNECT: wlan_job(WJ_CONNECT, &dl_net, 0, NULL); break;
	case DA_WFORGET: wlan_job(WJ_FORGET, &dl_net, 0, NULL); break;
	case DA_DISCONNECT: bt_start_job(BJ_DISCONNECT, &dl_dev, 0); break;
	case DA_FORGET: bt_start_job(BJ_FORGET, &dl_dev, 0); break;
	case DA_PLAY_C: case DA_NEXT_C: case DA_ADD_C: case DA_LIST_C: coll_action(act); break;
	case DA_DEL_LIST: if (dl_coll_idx < npl) { pl_delete(dl_coll_idx); flash("Lista borrada"); } break;
	case DA_SEL_REMOVE: case DA_SEL_QUEUE: sel_confirmed(act); break;
	case DA_FORGET_ASK: {
		char t[100];
		snprintf(t, sizeof t, T("¿Olvidar %s?"), dl_dev.name);
		const char *nm[2] = {"Olvidar", "Cancelar"}, *ic[2] = {ICO_TRASH, ICO_BACK};
		const int id[2] = {DA_FORGET, DA_CANCEL};
		dlg_open(t, "Habrá que volver a emparejarlo", 2, nm, ic, id);
		return;
	}
	}
	render();
}
// Tarjeta centrada de los diálogos y sus botones (icono + texto); hl = destacado con el acento, dim = atenuado (Cancelar).
static lv_obj_t *card(lv_obj_t *p, int y, int h) {
	lv_obj_t *d = rect(p, 40, y, 400, h, SK.bg2);
	lv_obj_set_style_radius(d, RAD, 0);
	border(d, 1, SK.line, LV_BORDER_SIDE_FULL);
	lv_obj_add_flag(d, LV_OBJ_FLAG_CLICKABLE);
	return d;
}
static void card_btn(lv_obj_t *d, int y, int h, const char *ico, const char *name, const lv_font_t *f, lv_event_cb_t cb, int i, int hl, int dim) {
	lv_obj_t *b = rect(d, 16, y, 368, h, hl ? SK.a1 : SK.bg);
	lv_obj_set_style_radius(b, RAD, 0);
	border(b, 1, hl ? SK.a1 : SK.line, LV_BORDER_SIDE_FULL);
	lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_bg_color(b, C(SK.line), LV_STATE_PRESSED);
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
	lv_obj_align(label(b, &ui_icons_30, hl ? SK.onsel : dim ? SK.dim : SK.a1, ico), LV_ALIGN_LEFT_MID, 22, 0);
	lv_obj_t *l = label(b, f, hl ? SK.onsel : dim ? SK.dim : SK.fg, name);
	lv_obj_set_width(l, 368 - 74 - 16); lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
	lv_obj_align(l, LV_ALIGN_LEFT_MID, 74, 0);
}
static void dlg_open(const char *title, const char *sub, int n, const char *const *names, const char *const *icos, const int *ids) {
	dlg_close();
	for (int i = 0; i < n; i++) dl_ids[i] = ids[i];
	int h = 24 + 40 + 64 + n * 96 + 12, y = (H - h) / 2;
	dl_root = veil(0, LV_OPA_70, dlg_event, (void *)(intptr_t)-1);
	lv_obj_t *d = card(dl_root, y, h);
	lv_obj_t *t = label(d, F28, SK.fg, title);
	lv_obj_set_size(t, 352, lv_font_get_line_height(F28)); lv_label_set_long_mode(t, LV_LABEL_LONG_DOT); lv_obj_set_pos(t, 24, 22);
	lv_obj_t *s = label(d, F24, SK.dim, sub);
	lv_obj_set_size(s, 352, 2 * lv_font_get_line_height(F24)); lv_label_set_long_mode(s, LV_LABEL_LONG_DOT); lv_obj_set_pos(s, 24, 62);
	for (int i = 0; i < n; i++) card_btn(d, 128 + i * 96, 84, icos[i], names[i], F28, dlg_event, i, 0, i == n - 1 && ids[i] == DA_CANCEL);
}
// Pistas de un álbum (índice de álbum) o de un artista (índice de artista) en orden de reproducción; devuelve la cantidad.
static int coll_tracks(int is_artist, int idx, int **out) {
	int cap = is_artist ? artists[idx].count : albums[idx].count, n = 0;
	int *l = malloc((cap + 1) * sizeof *l);
	*out = l;
	if (!l) return 0;
	for (int k = 0; k < (is_artist ? nalbums : 1); k++) {
		int a = is_artist ? ord_a[k] : idx, start = n;
		if (is_artist && albums[a].artist_i != idx) continue;
		for (int i = 0; i < ntracks && n < cap; i++) if (tracks[i].album == a) l[n++] = i;
		qsort(l + start, n - start, sizeof *l, album_cmp);
	}
	return n;
}
// Selector de acento: muestras de color en rejilla; "A" = Auto (color de la carátula).
static lv_obj_t *ac_root;
static void acc_event(lv_event_t *e) {
	int k = (int)(intptr_t)lv_event_get_user_data(e);
	if (ac_root) { lv_obj_delete(ac_root); ac_root = NULL; }
	if (k >= 0) { acc_i = k; theme_apply(); save_state(); }
	render();
}
static void acc_open(void) {
	ac_root = veil(0, LV_OPA_70, acc_event, (void *)(intptr_t)-1);
	lv_obj_t *d = card(ac_root, 70, 660);
	lv_obj_set_pos(label(d, F30, SK.fg, "Color"), 24, 20);
	static const char ORD[NACC] = {6, 7, 1, 4, 10, 2, 9, 8, 0, 11, 3, ACC_AUTO, 12, 13, 14, 15, 16, 17, 18, 19};   // por matiz, Auto y las paletas
	for (int k = 0; k < NACC; k++) {
		int i = ORD[k];
		lv_obj_t *b = rect(d, 28 + k % 4 * 90, 84 + k / 4 * 112, 76, 76, i == ACC_AUTO ? SK.bg : ACC[i][2] ? ACC[i][2] : ACC[i][0]);
		lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
		if (ACC[i][1]) {   // paleta: acento y secundario como dos puntos sobre el tono de fondo
			for (int j = 0; j < 2; j++) { lv_obj_t *c = rect(b, 14 + j * 24, 26, 24, 24, ACC[i][j]); lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0); lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE); }
		}
		if (i == acc_i || i == ACC_AUTO) border(b, i == acc_i ? 5 : 2, i == acc_i ? SK.fg : SK.line, LV_BORDER_SIDE_FULL);
		lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_event_cb(b, acc_event, LV_EVENT_CLICKED, (void *)(intptr_t)i);
		if (i == ACC_AUTO) lv_obj_center(label(b, F30, SK.fg, "A"));
	}
}
static void coll_action(int act) {
	int *l, n = coll_tracks(dl_coll_artist, dl_coll_idx, &l);
	if (!n) { free(l); return; }
	if (act == DA_LIST_C) { pick_start(l, n); push(P_PICK); return; }
	char m[64];
	if (act == DA_PLAY_C || (act == DA_NEXT_C && (qpos < 0 || cur_track < 0))) {
		short *s = malloc(n * sizeof *s);
		if (s) { for (int i = 0; i < n; i++) s[i] = l[i]; start_track(q_set(s, n, 0)); free(s); }
	} else {
		pthread_mutex_lock(&qlock);
		if (act == DA_NEXT_C) for (int i = n - 1; i >= 0; i--) q_add_l(l[i], 1);
		else q_append_l(l, n);
		pthread_mutex_unlock(&qlock);
		snprintf(m, sizeof m, act == DA_NEXT_C ? T("%d pistas suenan a continuación") : T("%d pistas añadidas a la cola"), n);
		flash(m);
	}
	free(l);
}
// Pulsación larga en un álbum o artista: reproducir todo, reproducir a continuación o añadir al final de la cola.
static void coll_dialog(int is_artist, int idx) {
	int n;
	char t[100], s[96];
	if (idx < 0) return;
	dl_coll_artist = is_artist; dl_coll_idx = idx;
	if (is_artist) { n = artists[idx].count; snprintf(t, sizeof t, "%s", artists[idx].name); snprintf(s, sizeof s, T("%d %s · %d pistas"), artists[idx].albums, artists[idx].albums == 1 ? T("álbum") : T("álbumes"), n); }
	else { n = albums[idx].count; snprintf(t, sizeof t, "%s", albums[idx].name); snprintf(s, sizeof s, T("%s · %d pistas"), albums[idx].artist, n); }
	const char *nm[5] = {"Reproducir", "Reproducir luego", "Añadir al final", "Añadir a lista", "Cancelar"}, *ic[5] = {ICO_PLAY, ICO_NEXT, ICO_LISTUL, ICO_LISTUL, ICO_BACK};
	const int id[5] = {DA_PLAY_C, DA_NEXT_C, DA_ADD_C, DA_LIST_C, DA_CANCEL};
	dlg_open(t, s, 5, nm, ic, id);
}
// Pulsación larga en una lista: borrarla (se borra su archivo .m3u8).
static void list_dialog(int row) {
	int k = row_idx(row);
	char s[140];
	if (k < 0 || k >= npl) return;
	dl_coll_idx = k;
	snprintf(s, sizeof s, T("%d pistas · %s"), pls[k].n, pls[k].rel);
	const char *nm[2] = {"Borrar lista", "Cancelar"}, *ic[2] = {ICO_TRASH, ICO_BACK};
	const int id[2] = {DA_DEL_LIST, DA_CANCEL};
	dlg_open(pls[k].name, s, 2, nm, ic, id);
}
static void coll_open(int row) { coll_dialog(kind() == P_ARTISTS, row_idx(row)); }
// Acciones de un dispositivo de la lista: nuevo = confirmar el emparejamiento; ya emparejado = conectar o desconectar y olvidar.
static void bt_dev_dialog(int i) {
	dl_dev = btd[i];
	if (!dl_dev.paired) {
		const char *nm[2] = {"Emparejar", "Cancelar"}, *ic[2] = {ICO_BT, ICO_BACK};
		const int id[2] = {DA_CONNECT, DA_CANCEL};
		dlg_open(dl_dev.name, "Se emparejará y quedará guardado para conectarse solo", 2, nm, ic, id);
	} else {
		const char *nm[3] = {dl_dev.connected ? "Desconectar" : "Conectar", "Olvidar dispositivo", "Cancelar"}, *ic[3] = {dl_dev.connected ? ICO_POWER : ICO_BT, ICO_TRASH, ICO_BACK};
		const int id[3] = {dl_dev.connected ? DA_DISCONNECT : DA_CONNECT, DA_FORGET_ASK, DA_CANCEL};
		dlg_open(dl_dev.name, dl_dev.connected ? "Conectado" : "Emparejado", 3, nm, ic, id);
	}
}
// Red Wi-Fi: nueva abierta = conectar; nueva protegida = pedir la contraseña; guardada = conectar u olvidar.
static void wifi_net_dialog(int i) {
	dl_net = wn[i];
	if (dl_net.id < 0 && dl_net.sec) { pw_net = dl_net; wq[0] = 0; kb_on = 1; push(P_WIFIPW); return; }
	if (dl_net.id < 0) { wlan_job(WJ_CONNECT, &dl_net, 0, NULL); return; }
	const char *nm[3] = {"Conectar", "Olvidar red", "Cancelar"}, *ic[3] = {ICO_WIFI, ICO_TRASH, ICO_BACK};
	const int id[3] = {DA_WCONNECT, DA_WFORGET, DA_CANCEL};
	int c = dl_net.cur && wlan_ip[0];
	dlg_open(dl_net.ssid, c ? "Conectada" : "Guardada", 3 - c, nm + c, ic + c, id + c);
}
// ---- Hoja de acciones de una pista (pulsación larga) y ficha técnica ----
static lv_obj_t *sh_root;
static int sh_ti, sh_q, sh_row;
static void sheet_close(void) { if (sh_root) { lv_obj_delete(sh_root); sh_root = NULL; } }
static void sheet_do(int k);
static void sheet_event(lv_event_t *e) { sheet_do((int)(intptr_t)lv_event_get_user_data(e)); }
static void sheet_do(int k) {
	int ti = sh_ti, qp = sh_q;
	sheet_close();
	switch (k) {
	case 0: pthread_mutex_lock(&qlock); q_add_l(ti, 1); pthread_mutex_unlock(&qlock); flash("Suena a continuación"); break;
	case 1:
		pthread_mutex_lock(&qlock);
		if (qp >= 0) q_remove_l(qp); else q_add_l(ti, 0);
		pthread_mutex_unlock(&qlock);
		flash(qp >= 0 ? "Quitada de la cola" : "Añadida a la cola");
		break;
	case 2: tracks[ti].fav = !tracks[ti].fav; save_state(); flash(tracks[ti].fav ? "Favorito añadido" : "Favorito quitado"); break;
	case 3: if (kind() == P_ALBUM) { coll_dialog(0, tracks[ti].album); return; } ctx_album = tracks[ti].album; push(P_ALBUM); break;
	case 4: info_open(ti); return;
	case 5: { int *l = malloc(sizeof *l); if (l) { l[0] = ti; pick_start(l, 1); push(P_PICK); } break; }
	case 6: {
		Playlist *p = &pls[ctx_pl];
		if (sh_row >= 0 && sh_row < p->n) { memmove(p->tr + sh_row, p->tr + sh_row + 1, (p->n - sh_row - 1) * sizeof *p->tr); p->n--; flash(pl_save(ctx_pl) ? "Quitada de la lista" : "No se pudo guardar la lista"); }
		break;
	}
	}
	render();
}
static void sheet_open(int row) {
	if (sh_root) return;
	sh_ti = row_idx(row); sh_q = row_q(row); sh_row = row;
	int n = kind() == P_PLAYLIST ? 7 : 6, h = 70 + n * 88 + 8;
	sh_root = veil(0, LV_OPA_70, sheet_event, (void *)(intptr_t)-1);
	lv_obj_t *d = card(sh_root, (H - h) / 2, h);
	lv_obj_t *t = label(d, F28, SK.dim, tracks[sh_ti].m.title);
	lv_obj_set_width(t, 352); lv_label_set_long_mode(t, LV_LABEL_LONG_DOT); lv_obj_set_pos(t, 24, 20);
	const char *name[7] = {"Reproducir luego", sh_q >= 0 ? "Quitar de la cola" : "Añadir al final", tracks[sh_ti].fav ? "Quitar favorita" : "Marcar favorita", kind() == P_ALBUM ? "Álbum completo" : "Ver álbum", "Ficha técnica", "Añadir a lista", "Quitar de esta lista"};
	static const char *ico[7] = {ICO_NEXT, ICO_LISTUL, ICO_HEART, ICO_DISC, ICO_INFO, ICO_LISTUL, ICO_BACK};
	for (int i = 0; i < n; i++) card_btn(d, 70 + i * 88, 78, ico[i], name[i], F28, sheet_event, i, 0, 0);
}
// Clave a la izquierda y valor a la derecha; si el valor no cabe, va completo en las líneas siguientes.
static void info_kv(lv_obj_t *d, int *y, const char *k, const char *v) {
	lv_point_t sz;
	v = T(v);
	lv_obj_set_pos(label(d, F24, SK.dim, k), 24, *y);
	lv_obj_t *l = label(d, F24, SK.fg, v);
	int one = lv_text_get_width(v, strlen(v), F24, 0) <= 250;
	if (one) { lv_obj_set_width(l, 250); lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0); lv_obj_set_pos(l, 166, *y); *y += 42; return; }
	lv_obj_set_width(l, 392); lv_obj_set_pos(l, 24, *y + 34);
	lv_text_get_size(&sz, v, F24, 0, 0, 392, LV_TEXT_FLAG_NONE);
	*y += 34 + sz.y + 12;
}
// Datos de la pista (etiquetas, archivo) y, si es la que suena, de la salida y del tiempo de inicio. Tocar en cualquier parte cierra.
static void info_open(int ti) {
	if (sh_root) sheet_close();
	const Track *t = &tracks[ti];
	char b[160];
	sh_root = veil(0, LV_OPA_70, sheet_event, (void *)(intptr_t)-1);
	lv_obj_t *d = rect(sh_root, 20, 70, 440, 660, SK.bg2);
	lv_obj_set_style_radius(d, RAD, 0);
	border(d, 1, SK.line, LV_BORDER_SIDE_FULL);
	lv_obj_add_flag(d, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);   // se desplaza si el texto no cabe; tocarla cierra
	lv_obj_add_event_cb(d, sheet_event, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
	lv_obj_set_scrollbar_mode(d, LV_SCROLLBAR_MODE_OFF);
	lv_obj_t *h = label(d, F28, SK.a1, t->m.title);
	lv_point_t sz;
	lv_obj_set_width(h, 392); lv_obj_set_pos(h, 24, 20);
	lv_text_get_size(&sz, t->m.title, F28, 0, 0, 392, LV_TEXT_FLAG_NONE);
	int y = 20 + sz.y + 22;
	info_kv(d, &y, "Formato", t->m.fmt);
	fmt_time(b, sizeof b, t->m.dur); info_kv(d, &y, "Duración", b);
	snprintf(b, sizeof b, "%.1f MB", t->size / 1048576.0); info_kv(d, &y, T("Tamaño"), b);
	if (t->m.dur) { snprintf(b, sizeof b, "%ld kbps", t->size * 8 / t->m.dur / 1000); info_kv(d, &y, T("Tasa media"), b); }
	if (t->m.track) { if (t->m.disc > 1) snprintf(b, sizeof b, T("Disco %d · Pista %d"), t->m.disc, t->m.track); else snprintf(b, sizeof b, T("Pista %d"), t->m.track); info_kv(d, &y, T("Posición"), b); }
	info_kv(d, &y, "Artista", t->m.artist);
	info_kv(d, &y, "Álbum", t->m.album);
	if (ti == cur_track && out_info[0]) {
		info_kv(d, &y, "Salida", "");
		lv_obj_t *o = label(d, F24, SK.fg, out_info);
		lv_obj_set_width(o, 392); lv_obj_set_pos(o, 24, y); y += 34 * (1 + (strchr(out_info, '\n') != NULL)) + 8;
		snprintf(b, sizeof b, "%u ms", start_ms); info_kv(d, &y, T("Inicio de pista"), b);
	}
	lv_obj_t *f = label(d, F24, SK.dim, t->file);
	lv_obj_set_width(f, 392); lv_obj_set_pos(f, 24, y + 10);
	lv_text_get_size(&sz, t->file, F24, 0, 0, 392, LV_TEXT_FLAG_NONE);
	y += 10 + sz.y + 24;
	if (y < 660) { lv_obj_set_height(d, y); lv_obj_set_y(d, (H - y) / 2); }
}

// ---- Acciones de la selección (barra inferior mientras dura) ----
// Según la pantalla: canciones, álbumes, artistas y búsqueda = Reproducir, Después, Lista, Favorita, Más; lista propia y Favoritas =
// Quitar, Reproducir, Lista, Más; cola = Quitar, Más; Listas = Borrar. Más: con un elemento, sus acciones de siempre; con varios, añadir al final.
enum { SA_PLAY, SA_NEXT, SA_LIST, SA_FAV, SA_MORE, SA_REMOVE };
static int sel_cmp(const void *a, const void *b) { int x = *(const int *)a, y = *(const int *)b; return x < y ? -1 : x > y; }
// Pistas de la selección en el orden de la pantalla (hasta SEL_TRACKS); devuelve cuántas.
static int sel_tracks(int **out) {
	int *sv = malloc(sel_n * sizeof *sv), *l = NULL, n = 0, cap = 0;
	if (!sv) { *out = NULL; return 0; }
	memcpy(sv, sel_v, sel_n * sizeof *sv);
	qsort(sv, sel_n, sizeof *sv, sel_cmp);
	for (int i = 0; i < nrows && n < SEL_TRACKS; i++) {
		int it = row_item(i);
		if (it < 0 || !bsearch(&it, sv, sel_n, sizeof *sv, sel_cmp)) continue;
		int ty = it >> 24, v = it & 0xffffff, one, *x = &one, m = 1;
		if (ty == 1 || ty == 2) m = coll_tracks(ty == 1, v, &x);
		else one = ty == 5 ? qa[v] : ty == 6 ? pls[ctx_pl].tr[v] : v;
		for (int k = 0; k < m && n < SEL_TRACKS; k++) { if (n == cap) { cap = cap ? cap * 2 : 256; l = grow(l, cap * sizeof *l); } l[n++] = x[k]; }
		if (x != &one) free(x);
	}
	free(sv);
	if (n >= SEL_TRACKS) flash("Máximo 5000 pistas");
	*out = l;
	return n;
}
static void sel_play(int next) {   // next: 0 reproducir ya, 1 a continuación, 2 al final de la cola
	int *l, n = sel_tracks(&l);
	char m[64];
	if (!n) { free(l); return; }
	if (next == 0 || qpos < 0 || cur_track < 0) {
		short *q = malloc(n * sizeof *q);
		if (q) { for (int i = 0; i < n; i++) q[i] = l[i]; start_track(q_set(q, n, 0)); free(q); }
	} else {
		pthread_mutex_lock(&qlock);
		if (next == 1) for (int i = n - 1; i >= 0; i--) q_add_l(l[i], 1); else q_append_l(l, n);
		pthread_mutex_unlock(&qlock);
		snprintf(m, sizeof m, next == 1 ? T("%d pistas suenan a continuación") : T("%d pistas añadidas a la cola"), n);
		flash(m);
	}
	free(l);
}
static void sel_do(int act) {
	int k = kind();
	if (act == SA_PLAY || act == SA_NEXT) { sel_play(act == SA_PLAY ? 0 : 1); sel_clear(); if (act == SA_PLAY) push(P_PLAYER); render(); return; }
	if (act == SA_LIST) { int *l, n = sel_tracks(&l); sel_clear(); if (n) { pick_start(l, n); push(P_PICK); } else free(l); render(); return; }
	if (act == SA_FAV) {   // todas favoritas: se quitan; si no, se marcan
		int *l, n = sel_tracks(&l), all = 1;
		for (int i = 0; i < n; i++) all &= tracks[l[i]].fav;
		for (int i = 0; i < n; i++) tracks[l[i]].fav = !all;
		free(l); save_state(); flash(all ? "Favorito quitado" : "Favorito añadido");
		sel_clear(); render(); return;
	}
	if (act == SA_REMOVE) {
		char t[120];
		const char *nm[2] = {k == P_LISTS ? "Borrar" : "Quitar", "Cancelar"}, *ic[2] = {ICO_TRASH, ICO_BACK};
		const int id[2] = {DA_SEL_REMOVE, DA_CANCEL};
		if (k == P_LISTS) snprintf(t, sizeof t, T("¿Borrar %d listas?"), sel_n);
		else if (k == P_PLAYLIST) snprintf(t, sizeof t, T("¿Quitar %d de %s?"), sel_n, pls[ctx_pl].name);
		else if (k == P_FAVS) snprintf(t, sizeof t, T("¿Quitar %d de Favoritas?"), sel_n);
		else snprintf(t, sizeof t, T("¿Quitar %d de la cola?"), sel_n);
		dlg_open(t, k == P_LISTS ? "Se borran sus archivos .m3u8" : "Las canciones siguen en la biblioteca", 2, nm, ic, id);
		return;
	}
	// Más: con un elemento, sus acciones de siempre; con varios, añadir al final de la cola
	if (sel_n > 1) {
		const char *nm[2] = {"Añadir al final", "Cancelar"}, *ic[2] = {ICO_LISTUL, ICO_BACK};
		const int id[2] = {DA_SEL_QUEUE, DA_CANCEL};
		char t[48];
		snprintf(t, sizeof t, T("%d seleccionadas"), sel_n);
		dlg_open(t, "", 2, nm, ic, id);
		return;
	}
	int it = sel_v[0], ty = it >> 24, v = it & 0xffffff, row = -1;
	for (int i = 0; i < nrows && row < 0; i++) if (row_item(i) == it) row = i;
	sel_clear(); render();
	if (row < 0) return;
	if (ty == 1 || ty == 2) coll_dialog(ty == 1, v);
	else if (ty == 4) list_dialog(row);
	else if (k == P_SEARCH) info_open(v);
	else sheet_open(row);
}
static void sel_confirmed(int act) {
	int k = kind();
	if (act == DA_SEL_QUEUE) { sel_play(2); sel_clear(); return; }
	qsort(sel_v, sel_n, sizeof *sel_v, sel_cmp);   // de mayor a menor posición: borrar no mueve las que faltan
	if (k == P_FAVS) { int *l, n = sel_tracks(&l); for (int i = 0; i < n; i++) tracks[l[i]].fav = 0; free(l); save_state(); flash("Favorito quitado"); }
	else if (k == P_LISTS) { for (int i = sel_n - 1; i >= 0; i--) if ((sel_v[i] & 0xffffff) < npl) pl_delete(sel_v[i] & 0xffffff); flash("Lista borrada"); }
	else if (k == P_PLAYLIST) {
		Playlist *p = &pls[ctx_pl];
		for (int i = sel_n - 1; i >= 0; i--) { int r = sel_v[i] & 0xffffff; if (r < p->n) { memmove(p->tr + r, p->tr + r + 1, (p->n - r - 1) * sizeof *p->tr); p->n--; } }
		flash(pl_save(ctx_pl) ? "Quitada de la lista" : "No se pudo guardar la lista");
	} else if (k == P_QUEUE) {
		pthread_mutex_lock(&qlock);
		for (int i = sel_n - 1; i >= 0; i--) q_remove_l(sel_v[i] & 0xffffff);
		pthread_mutex_unlock(&qlock);
		flash("Quitada de la cola");
	}
	sel_clear();
}
static void sel_bar_event(lv_event_t *e) { sel_do((int)(intptr_t)lv_event_get_user_data(e)); }
static void sel_bar_build(void) {
	static int built_for = -1;
	int k = kind(), want = sel_on ? k : -1;
	if (want == built_for && sel_on) return;
	built_for = want;
	lv_obj_clean(actbar);
	if (!sel_on) return;
	static const int A_DEF[] = {SA_PLAY, SA_NEXT, SA_LIST, SA_FAV, SA_MORE}, A_LIST[] = {SA_REMOVE, SA_PLAY, SA_LIST, SA_MORE},
	                 A_Q[] = {SA_REMOVE, SA_MORE}, A_PLS[] = {SA_REMOVE};
	static const char *const NM[] = {"Ahora", "Luego", "Lista", "Favorita", "Más", "Quitar"};
	static const char *const IC[] = {ICO_PLAY, ICO_NEXT, ICO_LISTUL, ICO_HEART, ICO_BARS, ICO_TRASH};
	const int *a = A_DEF;
	int n = 5;
	if (k == P_PLAYLIST || k == P_FAVS) { a = A_LIST; n = 4; } else if (k == P_QUEUE) { a = A_Q; n = 2; } else if (k == P_LISTS) { a = A_PLS; n = 1; }
	for (int i = 0; i < n; i++) {
		lv_obj_t *b = rect(actbar, i * W / n, 0, W / n, NAV_H, SK.bg2);
		lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
		pressfx(b, SK.line);
		lv_obj_add_event_cb(b, sel_bar_event, LV_EVENT_CLICKED, (void *)(intptr_t)a[i]);
		lv_obj_align(label(b, &ui_icons_34, a[i] == SA_REMOVE ? SK.a1 : SK.fg, IC[a[i]]), LV_ALIGN_TOP_MID, 0, 8);
		lv_obj_align(label(b, F24, SK.dim, k == P_LISTS && a[i] == SA_REMOVE ? "Borrar" : NM[a[i]]), LV_ALIGN_BOTTOM_MID, 0, -6);
	}
}

// Panel en dos niveles que sigue al dedo: el primero muestra la hora, la salida y el brillo; al seguir bajando aparecen el volumen,
// el Bluetooth y los accesos a ajustes, stand-by y apagado. Al soltar se ajusta al nivel más cercano (o se cierra).
enum { PN_HD = 56, PN_L1 = 186, PN_F = 478 };
static int pn_h;
static void pn_set_h(void *o, int32_t h) { pn_h = h; lv_obj_set_height(pn_card, h); }
static void pn_closed(lv_anim_t *a) { lv_async_call((lv_async_cb_t)panel_close, NULL); }
static void panel_to(int h) {
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, pn_card);
	lv_anim_set_values(&a, pn_h, h);
	lv_anim_set_duration(&a, 60 + abs(h - pn_h) / 3);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_set_exec_cb(&a, pn_set_h);
	if (!h) lv_anim_set_completed_cb(&a, pn_closed);
	lv_anim_start(&a);
}
static void panel_build(void) {
	pn_root = veil(0, LV_OPA_50, panel_scrim_event, NULL);
	lv_obj_t *p = pn_card = rect(pn_root, 0, 0, W, 0, SK.bg2), *hd = rect(p, 0, 0, W, PN_HD, SK.bg2);
	pn_h = 0;
	lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(p, panel_card_event, LV_EVENT_ALL, NULL);
	border(p, 1, SK.line, LV_BORDER_SIDE_BOTTOM);
	pn_hdr = label(hd, F26, SK.fg, "");
	lv_obj_set_width(pn_hdr, W - 40); lv_label_set_long_mode(pn_hdr, LV_LABEL_LONG_DOT);
	lv_obj_align(pn_hdr, LV_ALIGN_LEFT_MID, 20, 0);
	pn_bri = panel_slider(p, 84, ICO_SUN, 1);
	pn_vol = panel_slider(p, 182, ICO_VOLUME, 0);
	{
		char sub[96];
		if (!bt_on) snprintf(sub, sizeof sub, T("Apagado · toca para encender"));
		else if (bt_pcm[0]) snprintf(sub, sizeof sub, T("%s · toca: desconectar"), bt_name_of(bt_cur_mac));
		else snprintf(sub, sizeof sub, T("Sin conexión · toca: conectar"));
		panel_tile(p, 14, 268, ICO_BT, "Bluetooth", sub, bt_pcm[0]);
	}
	panel_icon(p, ICO_COG, 30, panel_settings_event);
	panel_icon(p, ICO_MOON, W - 30 - 34 - 110, panel_standby_event);
	panel_icon(p, ICO_POWER, W - 30 - 34, panel_power_event);
	lv_obj_t *hn = rect(p, 0, 0, 44, 5, SK.line);
	lv_obj_align(hn, LV_ALIGN_BOTTOM_MID, 0, -12);
	refresh(NULL);
}
// Arrastre desde la barra, el mini-reproductor o el propio panel: base = altura al empezar, dy = recorrido del dedo.
static void panel_drag(lv_event_code_t c, int base, int dy) {
	if (!pn_root) panel_build();
	lv_anim_delete(pn_card, NULL);
	if (c == LV_EVENT_PRESSING) { int h = base + dy; pn_set_h(NULL, h < 0 ? 0 : h > PN_F ? PN_F + (h - PN_F) / 4 : h); return; }
	int h = pn_h, up = dy < -24, down = dy > 24;   // al soltar: el sentido del gesto decide; si apenas se movió, el nivel más cercano
	int to = down ? (base < PN_L1 && h < PN_L1 + 60 ? PN_L1 : PN_F) : up ? (base > PN_L1 && h > PN_L1 - 60 ? PN_L1 : 0)
	       : h < PN_L1 / 2 ? 0 : h < (PN_L1 + PN_F) / 2 ? PN_L1 : PN_F;
	panel_to(to);
}
static void panel_open(void) { if (!pn_root) { panel_build(); panel_to(PN_L1); } }
static void panel_card_event(lv_event_t *e) {
	static int y0, base;
	lv_event_code_t c = lv_event_get_code(e);
	lv_point_t p;
	lv_indev_get_point(lv_indev_active(), &p);
	if (c == LV_EVENT_PRESSED) { y0 = p.y; base = pn_h; }
	else if (c == LV_EVENT_PRESSING || c == LV_EVENT_RELEASED) panel_drag(c, base, p.y - y0);
}

// Barra superior: en la búsqueda, tocarla muestra el teclado; en modo selección, "Todas" (a la derecha) marca todo lo de la lista.
static void st_event(lv_event_t *e) {
	lv_point_t p;
	lv_indev_get_point(lv_indev_active(), &p);
	if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
	if (sel_on && p.x > W - 170) { for (int i = 0; i < nrows && sel_n < SEL_MAX; i++) sel_add(row_item(i)); list_gen++; render(); }
	else if (cur()->id == P_SEARCH && !kb_on) { kb_on = 1; render(); }
}
// Esquina superior izquierda (en todas las pantallas): tirar hacia abajo despliega el panel rápido siguiendo el dedo. Un toque hace lo de
// debajo: abrir el reproductor (carátula del mini-reproductor), atrás (flecha) o mostrar el teclado de la búsqueda.
static void pz_event(lv_event_t *e) {
	static int y0, drag;
	lv_event_code_t c = lv_event_get_code(e);
	lv_point_t p;
	lv_indev_get_point(lv_indev_active(), &p);
	if (c == LV_EVENT_PRESSED) { y0 = p.y; drag = 0; }
	else if (c == LV_EVENT_PRESSING && (drag || p.y - y0 > 16)) { drag = 1; panel_drag(c, 0, p.y - y0); }
	else if (c == LV_EVENT_RELEASED && drag) panel_drag(c, 0, p.y - y0);
	else if (c == LV_EVENT_CLICKED && !drag && !pn_root) {
		int id = cur()->id;
		if (id == P_PLAYER || (!lv_obj_has_flag(bar_back, LV_OBJ_FLAG_HIDDEN) && p.y >= lv_obj_get_y(bar) && p.x < 72)) back();
		else if (p.y < lv_obj_get_y(bar)) { push(P_PLAYER); render(); }
		else if (id == P_SEARCH && !kb_on) { kb_on = 1; render(); }
	}
}

// ---- Energía ----
// Stand-by: apaga el brillo e ignora el táctil; el audio y el Bluetooth no se tocan. El botón de encendido lo alterna.
static void screen_set(int on) {
	if (on == !scr_off) return;
	scr_off = !on;
	if (scr_off) { panel_close(); if (pm_root) { lv_obj_delete(pm_root); pm_root = NULL; } }
	bl_write(on ? bright : 0);
	if (on) lv_display_trigger_activity(NULL);
}

// Flag de arranque (mtd5, vía bootmode.sh del sistema): armado = el próximo encendido inicia el slot B; desarmado = HiBy oficial.
// El slot B lo desarma en cada arranque; aquí se vuelve a armar cuando la aplicación lleva 30 s funcionando.
static void *boot_job(void *arg) {
	int on = (int)(intptr_t)arg;
#ifdef R1_SIM
	fprintf(stderr, "sim: flag de arranque %s\n", on ? "armado" : "desarmado");
#else
	system(on ? "/usr/bin/bootmode.sh Recovery >/dev/null 2>&1" : "/usr/bin/bootmode.sh >/dev/null 2>&1");
#endif
	return NULL;
}
static void boot_sync(void) {
	int want = tick_ms() - app_t0 > 30000;
	if (want == boot_armed) return;
	boot_armed = want;
	pthread_t t;
	pthread_create(&t, NULL, boot_job, (void *)(intptr_t)want);
	pthread_detach(t);
}

// Apaga o reinicia el equipo: "reboot" vuelve a HiBy (desarma el flag); "reboot-here" arma el flag y reinicia en este sistema.
// En el simulador solo termina el proceso.
static void power_run(const char *cmd) {
	vol = 0; apply_volume(); paused = 1;
	save_state();
	if (!strcmp(cmd, "reboot")) boot_job((void *)0);
	if (!strcmp(cmd, "reboot-here")) { boot_job((void *)1); cmd = "reboot"; }
#ifdef R1_SIM
	fprintf(stderr, "sim: %s\n", cmd);
	exit(0);
#else
	bl_write(0);
	system("sync");
	system(cmd);
	for (;;) sleep(1);
#endif
}
static void power_menu_close(void) { if (pm_root) { lv_obj_delete(pm_root); pm_root = NULL; } }
static void power_menu_event(lv_event_t *e) {
	int k = (int)(intptr_t)lv_event_get_user_data(e);
	power_menu_close();
	if (k == 0) power_run("poweroff");
	else if (k == 1) power_run("reboot-here");
	else if (k == 2) power_run("reboot");
	else if (k == 3) screen_set(0);
}
static void power_menu_open(void) {
	if (pm_root) return;
	panel_close();
	pm_root = veil(0, LV_OPA_70, power_menu_event, (void *)(intptr_t)4);
	lv_obj_t *d = card(pm_root, 124, 552);
	lv_obj_set_pos(label(d, F30, SK.fg, "Energía"), 24, 20);
	static const char *name[5] = {"Apagar", "Reiniciar", "Reiniciar en HiBy", "Apagar pantalla", "Cancelar"};
	static const char *ico[5] = {ICO_POWER, ICO_REPEAT, ICO_BACK, ICO_MOON, ICO_BACK};
	for (int i = 0; i < 5; i++) card_btn(d, 76 + i * 92, 80, ico[i], name[i], F30, power_menu_event, i, i == 0, 0);
}

// Aviso previo al apagado automático: enciende la pantalla y cuenta 30 s; tocar o pulsar una tecla cancela.
static lv_obj_t *cd_root, *cd_lbl;
static uint32_t cd_end;
static void cd_close(void) { if (cd_root) { lv_obj_delete(cd_root); cd_root = cd_lbl = NULL; } }
static void cd_cancel_event(lv_event_t *e) { cd_close(); lv_display_trigger_activity(NULL); }
static void cd_open(void) {
	if (cd_root) return;
	screen_set(1);
	cd_end = tick_ms() + 30000;
	cd_root = veil(0, LV_OPA_70, cd_cancel_event, NULL);
	lv_obj_t *d = rect(cd_root, 40, 260, 400, 280, SK.bg2);
	lv_obj_set_style_radius(d, RAD, 0);
	border(d, 1, SK.line, LV_BORDER_SIDE_FULL);
	lv_obj_t *t = label(d, F28, SK.fg, "Apagado automático"); lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 28);
	cd_lbl = label(d, F38, SK.a1, "30"); lv_obj_align(cd_lbl, LV_ALIGN_CENTER, 0, 6);
	t = label(d, F24, SK.dim, "Toca la pantalla para cancelar"); lv_obj_align(t, LV_ALIGN_BOTTOM_MID, 0, -26);
}

// ---- Modo disco USB (con ADB apagado; sd/r1os/usb_disk.sh) ----
// Al conectar un PC (USB "configured") se pregunta: Transferir música entrega la partición de música al PC (la reproducción se
// detiene; el sistema sigue en la 2.ª partición) y muestra "Conectado al PC"; al expulsar la unidad o desconectar el cable se
// vuelve a montar y la app se reinicia con un recorrido completo de la biblioteca. Solo cargar no vuelve a preguntar hasta desconectar.
static volatile int usb_restart;
static lv_obj_t *usb_root;
static int usb_busy(void) { return usb_st == US_BUSY || usb_st == US_DISK; }
static void read_str(const char *path, char *b, size_t n) {
	FILE *f = fopen(path, "r");
	b[0] = 0;
	if (f) { if (!fgets(b, n, f)) b[0] = 0; fclose(f); }
	b[strcspn(b, "\n")] = 0;
}
static void *usb_job(void *arg) {
	if (arg) {   // entregar
		usb_hold = 1;
		sleep(1);   // el audio y las formas de onda cierran sus archivos
		int rc = -1;
		for (int i = 0; i < 5 && rc; i++) { rc = system(OS_DIR "/usb_disk.sh export"); rc = WIFEXITED(rc) ? WEXITSTATUS(rc) : 1; if (rc == 2) break; if (rc) sleep(1); }
		if (!rc) { usb_st = US_DISK; return NULL; }
		usb_hold = 0; usb_st = US_CHARGE;
		toast_post(ICO_TRANSFER, "No se pudo conectar", rc == 2 ? "Hace falta particionar la SD" : "La tarjeta está en uso", 1);
	} else {     // recuperar
		int rc = system(OS_DIR "/usb_disk.sh release");
		if (rc) { usb_st = US_DISK; return NULL; }   // el PC aún no la expulsó
		usb_restart = 1;
	}
	return NULL;
}
static void usb_start(int disk) { pthread_t t; usb_st = US_BUSY; pthread_create(&t, NULL, usb_job, (void *)(intptr_t)disk); pthread_detach(t); }
// Opción explícita (Ajustes > USB > Transferir ahora): entrega la tarjeta sin esperar al diálogo de conexión.
static void usb_now(void) {
	if (access("/dev/mmcblk0p2", F_OK)) { toast_post(ICO_TRANSFER, "Hace falta particionar la SD", "El sistema no puede soltar su propia partición", 1); return; }
	if (!usb_conf) { flash("Conecta el cable al PC"); return; }
	if (usb_busy()) return;
	dlg_close(); paused = 1; usb_start(1);
}
static void usb_choice(int disk) {
	if (usb_st != US_ASK) return;
	if (!disk) { usb_st = US_CHARGE; return; }
	paused = 1;
	usb_start(1);
}
static void usb_noop(lv_event_t *e) {}
static void usb_overlay(int on) {
	if (!on) { if (usb_root) { lv_obj_delete(usb_root); usb_root = NULL; } return; }
	if (usb_root) return;
	usb_root = veil(0, LV_OPA_COVER, usb_noop, NULL);
	lv_obj_set_style_bg_color(usb_root, C(SK.bg), 0);
	lv_obj_t *ic = label(usb_root, &ui_icons_46, SK.a1, ICO_TRANSFER); lv_obj_align(ic, LV_ALIGN_CENTER, 0, -150);
	lv_obj_t *t = label(usb_root, F32, SK.fg, "Conectado al PC"); lv_obj_align(t, LV_ALIGN_CENTER, 0, -70);
	lv_obj_t *d = label(usb_root, F26, SK.dim, "Copia la música en la carpeta music de la unidad \"MUSICA\".\n\nPara volver, expulsa la unidad en el PC o desconecta el cable.");
	lv_obj_set_width(d, 400); lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_CENTER, 0); lv_obj_align(d, LV_ALIGN_CENTER, 0, 60);
}
static void usb_ask(void) {
	const char *nm[2] = {"Transferir música", "Solo cargar"}, *ic[2] = {ICO_TRANSFER, ICO_BATTERY};
	const int id[2] = {DA_USB_DISK, DA_USB_CHARGE};
	usb_st = US_ASK;
	screen_set(1);
	dlg_open("Conectado a un PC", "Transferir detiene la música mientras dura la copia", 2, nm, ic, id);
}
static void usb_tick(void) {
#ifdef R1_SIM
	return;
#endif
	char st[32], lun[64];
	if (usb_restart) { usb_restart = 0; setenv("R1_FULLSCAN", "1", 1); lib_restart(); }
	read_str("/sys/class/udc/13500000.otg_new/state", st, sizeof st);
	int conf = !strcmp(st, "configured");
	if (conf != usb_conf) { usb_conf = conf; bt_dirty = 1; }
	if (adb_on && !usb_busy()) { if (usb_st == US_ASK) dlg_close(); usb_st = US_IDLE; return; }
	read_str("/sys/kernel/config/usb_gadget/r1disk/functions/mass_storage.0/lun.0/file", lun, sizeof lun);
	switch (usb_st) {
	case US_IDLE:
		if (!conf) break;
		if (access("/dev/mmcblk0p2", F_OK)) { usb_st = US_CHARGE; break; }   // sin 2.ª partición no se puede entregar: no se pregunta
		usb_ask();
		break;
	case US_ASK: if (!conf) { dlg_close(); usb_st = US_IDLE; } else if (!dl_root) usb_st = US_CHARGE; break;
	case US_CHARGE: if (!conf) usb_st = US_IDLE; break;
	case US_DISK: if (!conf || !lun[0]) usb_start(0); break;
	}
	usb_overlay(usb_st == US_DISK);
}

// Cada 0,5 s: apaga la pantalla por inactividad y el equipo si lleva el tiempo configurado sin reproducir
// (con límite distinto según haya enlace Bluetooth o no). Reproducir, tocar o pulsar teclas reinicia la cuenta;
// conectar o desconectar el Bluetooth también, sin encender la pantalla ni iniciar la reproducción.
static void power_tick(void) {
	static uint32_t last;
	static PwState ps = {0, -1};
	uint32_t now = tick_ms();
	if (now - last < 500) return;
	last = now;
	boot_sync();
	usb_tick();
	PwIn in = {now, lv_display_get_inactive_time(NULL), (cur_track >= 0 && !paused) || usb_busy() || web_active(), bt_pcm[0] != 0, scr_off, SCR_TO[scr_to_i], OFF_MIN[off_disc_i], OFF_MIN[off_conn_i]};
	int d = pw_decide(&ps, &in);
	static uint32_t lg;
	if (now - lg > 60000) { lg = now; fprintf(stderr, "energía: inactivo %u s, sin reproducir %u s, reproduce %d, enlace %d, pantalla %s, límite %d min\n", in.idle / 1000, (now - ps.quiet_since) / 1000, in.playing, in.link, scr_off ? "apagada" : "encendida", in.link ? in.off_conn_min : in.off_disc_min); }
	if ((d & PW_SCREEN_OFF) && !pm_root && !cd_root) screen_set(0);
	if (cd_root) {
		if (in.playing || in.idle + 200 < now - (cd_end - 30000)) cd_close();   // actividad posterior al aviso
		else if ((int)(cd_end - now) <= 0) power_run("poweroff");
		else { char b[16]; snprintf(b, sizeof b, "%d s", (int)((cd_end - now + 999) / 1000)); set_text(cd_lbl, b); }
	} else if (d & PW_COUNTDOWN) cd_open();
}

// Actualiza solo lo que cambia (progreso, tiempos, título, batería, avisos).
static void refresh(lv_timer_t *t) {
	static int last_track = -2, last_p = -1, bat = -2, last_paused = -1;
	static uint32_t bat_t;
	int id = cur()->id;
	if (bt_dirty) { bt_dirty = 0; if (id == P_BT || id == P_SET || id == P_SOUND || id == P_USB || id == P_WIFI) render(); }
	if (id == P_WIFI && web_len) {   // progreso de la subida, cada segundo
		static uint32_t wt;
		if (tick_ms() - wt > 1000) { wt = tick_ms(); render(); }
	}
	if (web_rescan) { web_rescan = 0; lib_check(); }
	int dur = total_s ? total_s : cur_track >= 0 ? tracks[cur_track].m.dur : 0;
	int p = dur ? pos_s * 100 / dur : 0;
	if (cur_track != last_track) { last_track = cur_track; last_paused = paused; cov_want = cur_track; render(); }
	if (paused != last_paused) { last_paused = paused; if (id == P_PLAYER) render(); }
	if (pend_ready) {
		free(cov_s_px); free(cov_b_px);
		cov_s_px = pend_s; cov_b_px = pend_b; cov_k_gen = 0;
		int hue = pend_hue;
		pend_ready = 0;
		if (hue != auto_hue) { auto_hue = hue; if (acc_i == ACC_AUTO) theme_apply(); }
		cover_show();
		if (id == P_PLAYER) render();
	}
	if (tick_ms() - bat_t > 30000 || bat == -2) { bat_t = tick_ms(); bat = battery_int("capacity"); bat_chg = -1; }
	if (bat_chg < 0) bat_chg = battery_charging();
	char b[48], hm[8], bt[12];
	time_t now = time(NULL);
	struct tm *tm = localtime(&now);
	if (tm->tm_year + 1900 >= 2024) snprintf(hm, sizeof hm, "%02d:%02d", tm->tm_hour, tm->tm_min); else snprintf(hm, sizeof hm, "--:--");
	if (bat >= 0) snprintf(bt, sizeof bt, "%d%%", bat); else bt[0] = 0;
	char wx[24];
	wx_text(wx, sizeof wx);
	snprintf(b, sizeof b, "%s%s%s  %s", wx, wx[0] ? "  " : "", hm, bt);
	set_text(bar_r, sel_on ? "Todas" : b);
	if (pn_hdr) {
		char h[120];
		snprintf(h, sizeof h, "%s%s%s%s%s%s · %s", wx, wx[0] ? " · " : "", hm, bt[0] ? " · " : "", bt, bat_chg ? T(" cargando") : "", out_mode ? (bt_pcm[0] ? bt_name_of(bt_cur_mac) : T("BT sin enlace")) : jack_in ? T("Jack 3.5") : T("Jack vacío"));
		set_text(pn_hdr, h);
	}
	if (jack_ev) {
		if (jack_ev == 1) toast_post(ICO_HEADPHONES, "Auriculares quitados", out_mode ? "La salida es Bluetooth" : "Reproducción en pausa", !out_mode);
		else toast_post(ICO_HEADPHONES, "Auriculares conectados", out_mode ? "La salida sigue siendo Bluetooth" : "Toca play para continuar", 0);
		jack_ev = 0;
	}
	toast_tick();
	if (lib_msg[0]) { toast_post(ICO_MUSIC, "Biblioteca", lib_msg, 0); lib_msg[0] = 0; }
	if (lib_dirty == 1) { lib_dirty = 2; toast_post(ICO_MUSIC, "Música nueva en la tarjeta", "Ajustes > Biblioteca > Actualizar", 0); }
	bar_title_set();
	Track *tr = cur_track >= 0 ? &tracks[cur_track] : NULL;
	if (id != P_PLAYER) {
		char s[160];
		snprintf(s, sizeof s, "%s", tr ? tr->m.title : T("Sin reproducción"));
		set_text(np_title, s);
		snprintf(s, sizeof s, "%s%s%s", tr ? tr->m.artist : "", tr && tr->m.artist[0] && tr->m.album[0] ? " · " : "", tr ? tr->m.album : "");
		set_text(np_artist, s);
		lv_obj_set_y(np_title, (tr ? 10 : (NP_H - lv_font_get_line_height(F30)) / 2));
		set_text(np_pp, cur_track < 0 || paused ? ICO_PLAY : ICO_PAUSE);
		if (p != last_p) lv_obj_set_width(np_fill, W * p / 100);
	} else if (pl_title) {
		char t1[16], t2[16];
		int ps = scrub >= 0 ? scrub : pos_s, bw = dur ? 440 * ps / dur : 0;
		if (bw > 440) bw = 440;
		fmt_time(t1, sizeof t1, ps); snprintf(t2, sizeof t2, "-"); fmt_time(t2 + 1, sizeof t2 - 1, dur > ps ? dur - ps : 0);
		set_text(pl_t1, t1); set_text(pl_t2, t2);
		if (pl_fill) { lv_obj_set_width(pl_fill, bw); lv_obj_set_x(pl_knob, 7 + bw); }
		if (pl_wv && bw * WV_N / 440 != wv_split) { wv_split = bw * WV_N / 440; lv_obj_invalidate(pl_wv); }
		if (tr && tr->wv_ok == 1 && !pl_wv) render();
	}
	if (pn_root) { lv_obj_set_width(pn_vol, 392 * vol / 100); lv_obj_set_width(pn_bri, 392 * bright_pct() / 100); }
	last_p = p;
}

// Aplica la portada cargada a la franja de reproducción.
static void cover_show(void) {
	if (cov_s_px) { cov_dsc(&cov_s_dsc, cov_s_px, COV_S); lv_image_set_src(np_img, &cov_s_dsc); lv_obj_invalidate(np_img); lv_obj_remove_flag(np_img, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(np_disc, LV_OBJ_FLAG_HIDDEN); }
	else { lv_obj_add_flag(np_img, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(np_disc, LV_OBJ_FLAG_HIDDEN); }
}

static void build_ui(void) {
	lv_obj_t *scr = lv_screen_active();
	pn_root = pn_vol = pn_bri = pn_hdr = pn_card = pm_root = cd_root = cd_lbl = kb_root = NULL;
	lv_obj_clean(scr);
	pool_reset();
	body_player = 0;
	bat_chg = -1;
	lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_bg_color(scr, C(SK.bg), 0);
	lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_NONE, 0);
	lv_obj_t *area = rect(scr, 0, 0, W, H, SK.bg);

	// Barra superior (58 px): atrás, título o aviso, y a la derecha hora y batería (o favorito y cola en Reproduciendo)
	bar = rect(area, 0, MINI_H, W, BAR_H, SK.bg2);
	border(bar, 1, SK.line, LV_BORDER_SIDE_BOTTOM);
	lv_obj_add_flag(bar, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(bar, st_event, LV_EVENT_ALL, NULL);
	bar_back = rect(bar, 0, 0, 72, BAR_H, SK.bg2);
	lv_obj_add_flag(bar_back, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(bar_back, bar_back_event, LV_EVENT_CLICKED, NULL);
	pressfx(bar_back, SK.line);
	bar_ico = label(bar_back, &ui_icons_30, SK.a1, ICO_BACK); lv_obj_align(bar_ico, LV_ALIGN_LEFT_MID, 22, 0);
	bar_l = label(bar, F32, SK.fg, "");
	lv_obj_set_size(bar_l, W - 40, lv_font_get_line_height(F32)); lv_label_set_long_mode(bar_l, LV_LABEL_LONG_DOT);
	lv_obj_set_pos(bar_l, 20, (BAR_H - lv_font_get_line_height(F32)) / 2);
	bar_r = label(bar, F26, SK.dim, ""); lv_obj_align(bar_r, LV_ALIGN_RIGHT_MID, -20, 0);
	lv_obj_t *bb[2];
	for (int i = 0; i < 2; i++) {
		bb[i] = rect(bar, W - 136 + i * 68, 0, 68, BAR_H, SK.bg2);
		lv_obj_add_flag(bb[i], LV_OBJ_FLAG_CLICKABLE);
		pressfx(bb[i], SK.line);
		lv_obj_add_event_cb(bb[i], side_event, LV_EVENT_CLICKED, (void *)(intptr_t)(i ? 3 : 0));
		lv_obj_center(label(bb[i], &ui_icons_34, i ? SK.fg : SK.dim, i ? ICO_LISTUL : ICO_HEART));
	}
	bar_fav = bb[0]; bar_q = bb[1];

	body = rect(area, 0, MINI_H + BAR_H, W, H - BAR_H - MINI_H - NAV_H, SK.bg);
	hit = rect(area, 0, MINI_H + BAR_H, W, H - BAR_H - MINI_H - NAV_H, SK.bg);
	lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, 0);
	lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(hit, hit_event, LV_EVENT_ALL, NULL);

	// Barra de secciones (84 px abajo)
	navbar = rect(area, 0, H - NAV_H, W, NAV_H, SK.bg2);
	border(navbar, 1, SK.line, LV_BORDER_SIDE_TOP);
	for (int i = 0; i < NTAB; i++) {
		lv_obj_t *it = rect(navbar, i * (W / NTAB), 0, W / NTAB, NAV_H, SK.bg2);
		lv_obj_add_flag(it, LV_OBJ_FLAG_CLICKABLE);
		pressfx(it, SK.line);
		lv_obj_add_event_cb(it, nav_event, LV_EVENT_CLICKED, (void *)(intptr_t)i);
		nav_ico[i] = label(it, &ui_icons_34, SK.dim, tab_ico[i]); lv_obj_align(nav_ico[i], LV_ALIGN_TOP_MID, 0, 8);
		nav_lbl[i] = label(it, F24, SK.dim, tab_name[i]); lv_obj_align(nav_lbl[i], LV_ALIGN_BOTTOM_MID, 0, -6);
	}

	actbar = rect(area, 0, H - NAV_H, W, NAV_H, SK.bg2);   // acciones de la selección (en lugar de las secciones)
	border(actbar, 1, SK.line, LV_BORDER_SIDE_TOP);
	lv_obj_add_flag(actbar, LV_OBJ_FLAG_HIDDEN);

	// Mini-reproductor (96 px arriba: carátula de 72 y barra de progreso de 6 al pie)
	np = rect(area, 0, 0, W, MINI_H, SK.bg2);
	lv_obj_add_flag(np, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(np, np_event, LV_EVENT_ALL, NULL);
	pressfx(np, SK.line);
	lv_obj_t *npb = rect(np, 0, NP_H, W, NPB_H, SK.bg);
	np_fill = rect(npb, 0, 0, 0, NPB_H, SK.a1);
	np_cover = rect(np, 16, (NP_H - COV_S) / 2, COV_S, COV_S, SK.bg);
	lv_obj_set_style_radius(np_cover, RAD, 0);
	border(np_cover, 1, SK.line, LV_BORDER_SIDE_FULL);
	lv_obj_set_style_clip_corner(np_cover, true, 0);
	np_disc = label(np_cover, &ui_icons_34, SK.a1, ICO_DISC);
	lv_obj_center(np_disc);
	np_img = lv_image_create(np_cover);
	lv_obj_set_pos(np_img, -1, -1);
	cover_show();
	np_title = label(np, F30, SK.a1, ""); lv_obj_set_size(np_title, 290, lv_font_get_line_height(F30)); lv_label_set_long_mode(np_title, LV_LABEL_LONG_DOT); lv_obj_set_pos(np_title, 104, 10);
	np_artist = label(np, F24, SK.dim, ""); lv_obj_set_size(np_artist, 290, lv_font_get_line_height(F24)); lv_label_set_long_mode(np_artist, LV_LABEL_LONG_DOT); lv_obj_set_pos(np_artist, 104, 50);
	np_pp = label(np, &ui_icons_34, SK.fg, ICO_PLAY); lv_obj_align(np_pp, LV_ALIGN_TOP_RIGHT, -32, (NP_H - 34) / 2);
	lv_obj_move_foreground(bar);   // por encima del cuerpo: en Reproduciendo flota sobre la carátula
	pz = rect(area, 0, 0, 150, MINI_H + BAR_H, SK.bg);   // esquina del panel rápido, por encima de todo
	lv_obj_set_style_bg_opa(pz, LV_OPA_TRANSP, 0);
	lv_obj_add_flag(pz, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(pz, pz_event, LV_EVENT_ALL, NULL);

}

#ifdef R1_SIM
// Guion R1_SIM_SCRIPT (fichas separadas por espacios): t0..t3 secciones (Música, Buscar, Favoritas, Ajustes), mt:N canciones/álbumes/artistas, play, pl reproductor, pn/pc abrir/cerrar panel,
// bt, sys, adv, sel:N elegir la fila N, kq:texto consulta o nombre de lista (_ = espacio), kok tecla de la esquina, sha:N acción N de la hoja,
// lsd:N diálogo de la lista N, pld:N borrar la lista N, sh:N hoja de acciones de la fila N, info ficha técnica, shc cerrarla, scN desplazar N px, back, s:nombre captura en /tmp/r1sim/nombre.bgra, q salir.
// tp / tp:N = N toques (gesto: 1 pausa, 2 siguiente, 3 anterior).
static void sim_shot(const char *name) {
	lv_draw_buf_t *b = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_ARGB8888);
	char path[128];
	snprintf(path, sizeof path, "/tmp/r1sim/%s.bgra", name);
	FILE *f = fopen(path, "wb");
	if (f && b) { for (int y = 0; y < H; y++) fwrite((uint8_t *)b->data + y * b->header.stride, 4, W, f); }
	if (f) fclose(f);
	if (b) lv_draw_buf_destroy(b);
}
static void sim_script(void) {
	static char *tok, *save, *buf;
	static int wait = 30, started;
	if (!getenv("R1_SIM_SCRIPT")) return;
	if (!started) { started = 1; buf = strdup(getenv("R1_SIM_SCRIPT")); mkdir("/tmp/r1sim", 0755); tok = strtok_r(buf, " ", &save); }
	if (wait > 0) { wait--; return; }
	if (!tok) exit(0);
	wait = 12;
	if (tok[0] == 't' && tok[1] >= '0' && tok[1] <= '3') { panel_close(); nav_root(tabs[tok[1] == '3' ? 2 : tok[1] - '0']); }   // t3 = Ajustes (guiones antiguos)
	else if (!strncmp(tok, "lp:", 3)) { int r = atoi(tok + 3); if (row_item(r) >= 0) { if (sel_on) sel_range(r); else { sel_start(r); kb_on = 0; } list_gen++; render(); } }   // pulsación larga en la fila N
	else if (!strncmp(tok, "tg:", 3)) { sel_toggle(atoi(tok + 3)); if (!sel_n) sel_clear(); list_gen++; render(); }   // toque en selección
	else if (!strncmp(tok, "sa:", 3)) sel_do(atoi(tok + 3));   // acción de la selección (SA_*)
	else if (!strncmp(tok, "swm:", 4) || !strncmp(tok, "swe:", 4)) { int r = atoi(tok + 4), dx = atoi(strchr(tok + 4, ':') + 1); if (tok[2] == 'm') swipe_move(r, dx); else swipe_end(r, dx); }   // gesto lateral fila:desplazamiento
	else if (!strcmp(tok, "sall")) { for (int i = 0; i < nrows && sel_n < SEL_MAX; i++) sel_add(row_item(i)); list_gen++; render(); }
	else if (!strcmp(tok, "ts")) { panel_close(); nav_root(P_MUSIC); sq[0] = 0; kb_on = 1; push(P_SEARCH); render(); }
	else if (!strncmp(tok, "mt:", 3)) { static const int to[3] = {P_SONGS, P_ALBUMS, P_ARTISTS}; nav_root(P_MUSIC); push(to[atoi(tok + 3) % 3]); render(); }
	else if (!strcmp(tok, "play")) { play_lib(2); }
	else if (!strncmp(tok, "play:", 5)) play_lib(atoi(tok + 5));
	else if (!strncmp(tok, "sh:", 3)) sheet_open(atoi(tok + 3));
	else if (!strcmp(tok, "info")) { if (cur_track >= 0) info_open(cur_track); }
	else if (!strcmp(tok, "shc")) sheet_close();
	else if (!strcmp(tok, "pl")) { push(P_PLAYER); render(); }
	else if (!strcmp(tok, "pn")) panel_open();
	else if (!strcmp(tok, "pnf")) { panel_open(); panel_to(PN_F); }
	else if (!strcmp(tok, "pc")) panel_close();
	else if (!strcmp(tok, "wifi")) { nav_root(P_SET); push(P_WIFI); render(); }
	else if (!strcmp(tok, "kup")) kb_edit("\3", 0);
	else if (!strcmp(tok, "bt")) { nav_root(P_SET); push(P_BT); bt_start_job(BJ_LIST, NULL, 0); render(); }
	else if (!strcmp(tok, "sys")) { nav_root(P_SET); push(P_SYSTEM); render(); }
	else if (!strcmp(tok, "usb")) { nav_root(P_SET); push(P_USB); render(); }
	else if (!strcmp(tok, "back")) back();
	else if (!strcmp(tok, "usbq")) usb_ask();
	else if (!strcmp(tok, "usbd")) { dlg_close(); usb_overlay(1); }
	else if (!strcmp(tok, "kbn")) { kb_num = !kb_num; if (kb_root) { lv_obj_delete(kb_root); kb_root = NULL; } render(); }
	else if (!strncmp(tok, "kq:", 3)) { char *q = cur()->id == P_NEWLIST ? nq : cur()->id == P_WIFIPW ? wq : sq; snprintf(q, 64, "%s", tok + 3); for (char *u = q; *u; u++) if (*u == '_') *u = ' '; if (cur()->id == P_SEARCH) search_update(); else bar_title_set(); }
	else if (!strcmp(tok, "kok")) kb_edit("\1", 0);
	else if (!strncmp(tok, "sha:", 4)) sheet_do(atoi(tok + 4));
	else if (!strncmp(tok, "lsd:", 4)) list_dialog(atoi(tok + 4));
	else if (!strncmp(tok, "pld:", 4)) { dlg_close(); pl_delete(atoi(tok + 4)); render(); }
	else if (!strncmp(tok, "sel:", 4)) { cur()->sel = atoi(tok + 4); do_select(); }
	else if (!strcmp(tok, "pm")) power_menu_open();
	else if (!strcmp(tok, "link:on")) system("echo AA:BB:CC:DD:EE:01 > " DATA_DIR "/bt_link");
	else if (!strcmp(tok, "link:off")) system("rm -f " DATA_DIR "/bt_link");
	else if (!strcmp(tok, "pause")) paused = 1;
	else if (!strcmp(tok, "tp")) tap_gesture();
	else if (!strncmp(tok, "tp:", 3)) { int n = atoi(tok + 3); while (n-- > 0) tap_gesture(); }
	else if (!strcmp(tok, "skend")) seek_to(total_s - 5);
	else if (!strncmp(tok, "z:", 2)) sleep(atoi(tok + 2));
	else if (!strcmp(tok, "qu")) { push(P_QUEUE); render(); }
	else if (!strcmp(tok, "lib")) lib_check();
	else if (!strncmp(tok, "jack:", 5)) sim_jack = atoi(tok + 5);
	else if (!strncmp(tok, "sort", 4) && tok[5] == ':') { int v = atoi(tok + 6); if (tok[4] == 's') sort_s = v; else if (tok[4] == 'a') sort_a = v; else sort_r = v; sort_all(); render(); }
	else if (!strncmp(tok, "col:", 4)) coll_open(atoi(tok + 4));
	else if (!strncmp(tok, "acc:", 4)) { acc_i = atoi(tok + 4) % NACC; light = strchr(tok, 'l') != NULL; theme_apply(); }
	else if (!strcmp(tok, "accp")) acc_open();
	else if (!strncmp(tok, "dla:", 4)) coll_action(atoi(tok + 4));
	else if (!strncmp(tok, "btd:", 4)) bt_dev_dialog(atoi(tok + 4));
	else if (!strcmp(tok, "libp")) { nav_root(P_SET); push(P_LIB); render(); }
	else if (!strcmp(tok, "batt")) { nav_root(P_SET); push(P_SYSTEM); push(P_BATT); render(); }
	else if (!strcmp(tok, "snd")) { nav_root(P_SET); push(P_SOUND); render(); }
	else if (!strcmp(tok, "nx")) go(1);
	else if (!strcmp(tok, "pv")) go(-1);
	else if (!strcmp(tok, "endq")) { int t = q_next(1, 0); if (t >= 0) start_track(t); else paused = 1; }
	else if (!strncmp(tok, "cont:", 5)) cont = atoi(tok + 5);
	else if (!strncmp(tok, "shuf:", 5)) { shuf = atoi(tok + 5); q_reorder(); }
	else if (!strncmp(tok, "rep:", 4)) rep = atoi(tok + 4);
	else if (!strncmp(tok, "qj:", 3)) { int t = q_jump(atoi(tok + 3)); if (t >= 0) start_track(t); }
	else if (!strncmp(tok, "qadd:", 5)) { pthread_mutex_lock(&qlock); q_add_l(atoi(tok + 5), 0); pthread_mutex_unlock(&qlock); }
	else if (!strncmp(tok, "qnext:", 6)) { pthread_mutex_lock(&qlock); q_add_l(atoi(tok + 6), 1); pthread_mutex_unlock(&qlock); }
	else if (!strncmp(tok, "log:", 4)) { fprintf(stderr, "estado %s: pista=%d pos=%d/%d pausa=%d bt=%s salida=%d cola[%d/%d]:", tok + 4, cur_track, pos_s, total_s, paused, bt_pcm, out_mode, qpos, qn); for (int k = 0; k < qn && k < 40; k++) fprintf(stderr, " %d", qa[k]); fprintf(stderr, "\n"); }
	else if (!strcmp(tok, "off")) screen_set(0);
	else if (!strcmp(tok, "on")) screen_set(1);
	else if (!strcmp(tok, "energy")) { nav_root(P_SET); push(P_POWER); render(); }
	else if (!strncmp(tok, "sc", 2)) { cur()->off += atoi(tok + 2); render(); }
	else if (!strncmp(tok, "s:", 2)) sim_shot(tok + 2);
	else if (!strcmp(tok, "q")) exit(0);
	tok = strtok_r(NULL, " ", &save);
}
#endif

// Autotest de búsqueda (R1_TEST=1): reproduce, salta, retrocede y salta en pausa; escribe las posiciones por stderr.
static void *selftest(void *arg) {
	sleep(3);
	play_lib(atoi(getenv("R1_TEST")) - 1);
	sleep(3);
	fprintf(stderr, "t0: pista=%d pos=%d dur=%d\n", cur_track, pos_s, total_s);
	seek_to(100); sleep(2);
	fprintf(stderr, "tras seek_to(100): pos=%d (esperado ~102)\n", pos_s);
	seek_by(-30); sleep(2);
	fprintf(stderr, "tras seek_by(-30): pos=%d (esperado ~72)\n", pos_s);
	paused = 1; sleep(1);
	seek_to(200); sleep(1);
	fprintf(stderr, "en pausa tras seek_to(200): pos=%d (esperado 200)\n", pos_s);
	paused = 0; sleep(2);
	fprintf(stderr, "reanudado: pos=%d (esperado ~202)\n", pos_s);
	seek_to(total_s - 3); sleep(5);
	fprintf(stderr, "cerca del final: pista=%d pos=%d (esperado pista siguiente)\n", cur_track, pos_s);
	return NULL;
}

// Medición (R1_BENCH=1): desplaza la lista de arriba abajo sin parar y alterna entrar y salir de una pantalla; con R1_FPS=1 muestra FPS y CPU.
static void bench_step(void) {
	static uint32_t last, t0;
	static int dir = 1;
	uint32_t now = tick_ms();
	if (!t0) t0 = now;
	static int steps;
	static uint32_t rep0;
	if (now - rep0 >= 2000) { if (rep0) fprintf(stderr, "bench: %d pasos/s (objetivo 33)\n", steps / 2); rep0 = now; steps = 0; }
	static uint32_t pt;
	if (now - pt > 3000) {
		pt = now;
		if (pn_root) panel_close();
		else { uint32_t a = tick_ms(); panel_open(); fprintf(stderr, "bench: panel_open %u ms\n", tick_ms() - a); }
	}
	if (now - last < 30) return;
	last = now;
	steps++;
	if (now - t0 > 12000) { t0 = now; if (sp > 1) back(); else push(P_ALBUMS); return; }
	Pg *g = cur();
	int m = max_off();
	if (g->off + dir * 12 > m || g->off + dir * 12 < 0) dir = -dir;
	g->off += dir * 12;
	redraw_list();
}

// Pantalla del primer indexado (antes de crear la interfaz).
static lv_obj_t *scan_lbl;
static void scan_show(const char *m) {
	if (!scan_lbl) {
		lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), 0);
		scan_lbl = lv_label_create(lv_screen_active());
		lv_obj_set_style_text_font(scan_lbl, &ui_font_28, 0);
		lv_obj_set_style_text_color(scan_lbl, lv_color_white(), 0);
		lv_obj_set_style_text_align(scan_lbl, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_center(scan_lbl);
	}
	lv_label_set_text(scan_lbl, T(m));
	lv_obj_center(scan_lbl);
	lv_timer_handler();
}

int main(int argc, char **argv) {
	g_argv = argv;
#ifdef R1_SIM
	{	// Simulador: hcitool y bluealsa-cli de prueba (ui/sim_bin) y scripts de ui/sim_bt.
		char p[600], cwd[300];
		if (getcwd(cwd, sizeof cwd)) { snprintf(p, sizeof p, "%s/sim_bin:%s", cwd, getenv("PATH")); setenv("PATH", p, 1); }
		mkdir(DATA_DIR, 0755);
	}
#else
	setenv("PATH", OS_DIR "/bt/bin:/usr/bin:/bin:/usr/sbin:/sbin", 1);
	setenv("LD_LIBRARY_PATH", OS_DIR "/bt/lib:" OS_DIR "/lib", 1);
#endif
	app_t0 = tick_ms();
	touch_log = getenv("R1_TOUCHLOG") != NULL;
	signal(SIGPIPE, SIG_IGN);
	mkdir(CACHE_DIR, 0755);
	bad_load();
	signal(SIGSEGV, crash_handler); signal(SIGBUS, crash_handler); signal(SIGFPE, crash_handler); signal(SIGILL, crash_handler); signal(SIGABRT, crash_handler);
	srand(time(NULL));
	ctl_fd = open("/dev/snd/controlC0", O_RDWR);
	vol = 0; apply_volume(); vol = 20;
	signal(SIGTERM, mute_and_exit);
	signal(SIGINT, mute_and_exit);
	struct snd_ctl_elem_value port;
	memset(&port, 0, sizeof port);
	port.id.numid = 7;
	port.value.integer.value[0] = 2;
	ioctl(ctl_fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &port);
	in_fd[0] = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
	in_fd[1] = open("/dev/input/event2", O_RDONLY | O_NONBLOCK);

#ifdef R1_SIM
	{	// En el PC las fuentes quedan en memoria de solo lectura: se habilita escritura para asignar el respaldo.
		lv_font_t *fs[] = {&ui_font_24, &ui_font_26, &ui_font_28, &ui_font_30, &ui_font_32, &ui_font_38};
		for (int i = 0; i < 6; i++) mprotect((void *)((uintptr_t)fs[i] & ~4095UL), 8192, PROT_READ | PROT_WRITE);
	}
#endif
	lv_init();
	lv_log_register_print_cb(lv_log_cb);
	{	// Texto fuera de Latin-1, dibujado al vuelo desde la SD, en este orden: Noto Sans (latín extendido, cirílico, griego), Noto Sans CJK,
		// símbolos (flechas, estrellas, notas), matemáticas (letras estilizadas) y emoji monocromo. Sin los archivos queda Montserrat.
		static const char *const ff[] = {"noto/NotoSans-Medium.ttf", "noto-cjk/NotoSansCJK-Medium.ttc", "noto/NotoSansSymbols-Medium.ttf",
			"noto/NotoSansSymbols2-Regular.ttf", "noto/NotoSansMath-Regular.ttf", "noto/NotoEmoji.ttf"};
		static const int fc[] = {64, 128, 16, 16, 16, 16};   // glifos en caché por fuente
		lv_font_t *fs[] = {&ui_font_24, &ui_font_26, &ui_font_28, &ui_font_30, &ui_font_32, &ui_font_38};
		static const int px[] = {24, 26, 28, 30, 32, 38};
		for (int i = 0; i < 6; i++) {
			lv_font_t *last = fs[i];
			for (unsigned k = 0; k < sizeof ff / sizeof *ff; k++) {
				char path[160];
				snprintf(path, sizeof path, "P:" FONT_DIR "%s", ff[k]);
				lv_font_t *f = lv_tiny_ttf_create_file_ex(path, px[i], LV_FONT_KERNING_NONE, fc[k]);
				if (f) { last->fallback = f; last = f; }
			}
			last->fallback = &lv_font_montserrat_28;
		}
	}
	lv_tick_set_cb(tick_ms);
#ifdef R1_SIM
	lv_sdl_window_create(W, H);
	touch_dev = lv_sdl_mouse_create();
#else
	lv_display_t *disp = lv_linux_fbdev_create();
	lv_linux_fbdev_set_file(disp, "/dev/fb0");
	tp_fd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
	touch_dev = lv_indev_create();
	lv_indev_set_type(touch_dev, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb(touch_dev, touch_read);
#endif
	lv_timer_set_period(lv_indev_get_read_timer(touch_dev), 10);
	scan_ui = scan_show;
	uint64_t t0 = now_us();
	scan_music();
	fprintf(stderr, "biblioteca: %d pistas (%d nuevas, %d cambiadas, %d quitadas), %d álbumes, %d artistas, %d listas, escaneo %.2f s\n", ntracks, lib_new, lib_upd, lib_gone, nalbums, nartists, npl, (now_us() - t0) / 1e6);
	scan_ui = NULL;
	if (scan_lbl) { lv_obj_delete(scan_lbl); scan_lbl = NULL; }
	load_state();
	sort_all();
	if (chglim) chg_apply();
	df_apply();
	adb_on = access("/sys/kernel/config/usb_gadget/adb_demo", F_OK) == 0;
	set_volume(vol);
	build_ui();
	render();
	lv_timer_create(refresh, 100, NULL);

	pthread_t th;
	pthread_create(&th, NULL, audio_thread, NULL);
	pthread_create(&th, NULL, cover_thread, NULL);
	pthread_create(&th, NULL, lib_watch, NULL);
	pthread_create(&th, NULL, wave_thread, NULL);
	pthread_create(&th, NULL, wx_thread, NULL);
	pthread_create(&th, NULL, wlan_thread, NULL);
	pthread_create(&th, NULL, web_thread, NULL);
	pthread_create(&th, NULL, jack_watch, NULL);
	pthread_create(&th, NULL, bt_watch, NULL);
	pthread_create(&th, NULL, bt_reconnect, NULL);
	pthread_create(&th, NULL, bt_volume_thread, NULL);
	if (getenv("R1_TEST")) pthread_create(&th, NULL, selftest, NULL);
	if (getenv("R1_FPS")) lv_sysmon_show_performance(NULL); else lv_sysmon_hide_performance(NULL);
	if (getenv("R1_BENCH")) { sp = 0; stk[0] = (Pg){P_MUSIC, 0, 0}; push(P_SONGS); nav_dir = 0; render(); }
	int built_gen = theme_gen;
	for (;;) {
		uint32_t tl0 = tick_ms();
		handle_keys();
		power_tick();
		if (getenv("R1_BENCH")) bench_step();
#ifdef R1_SIM
		sim_script();
#endif
		if (built_gen != theme_gen) { built_gen = theme_gen; toast_hide(); build_ui(); render(); }
		uint32_t tl1 = tick_ms();
		if (scr_off) lv_indev_read(touch_dev);   // en stand-by no corre lv_timer_handler: vacía aquí la cola del táctil
		uint32_t wait = scr_off ? 50 : lv_timer_handler();
		if (touch_log && tick_ms() - tl0 > 50) fprintf(stderr, "ui: ciclo lento %u ms (teclas y energía %u, lvgl %u)\n", tick_ms() - tl0, tl1 - tl0, tick_ms() - tl1);
		usleep((wait > 20 ? 20 : wait + 1) * 1000);
	}
}
