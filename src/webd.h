// Servidor HTTP mínimo para gestionar la música por Wi-Fi (una conexión a la vez, "Connection: close").
//   GET /            página (WEB_PAGE)
//   GET /api/lib, /api/tracks?q=&o=&n=   JSON de la aplicación (web_api_get)
//   POST /api/rescan aplica los cambios en el R1 (web_api_rescan)
//   HEAD|GET|PUT|DELETE /music/<ruta>   archivo dentro de la carpeta de música; PUT escribe en <ruta>.part y renombra al terminar.
// Las rutas se decodifican y sanean como FAT: sin "..", sin segmentos vacíos y con \ : * ? " < > |, controles y caracteres de 4 bytes
// cambiados por "_".
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <unistd.h>

// Progreso para la interfaz: último archivo, bytes recibidos y total, archivos completos y último instante con tráfico.
static char web_name[128];
static volatile long long web_got, web_len;
static volatile int web_files;
static volatile uint32_t web_last;
static uint32_t tick_ms(void);
static void web_api_get(const char *url, FILE *o);
static void web_api_rescan(void);

static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1; }
// Valor decodificado del parámetro key de la consulta (?key=...&...; "+" = espacio). 0 si no está.
static int web_arg(const char *url, const char *key, char *out, size_t n) {
	const char *q = strchr(url, '?');
	size_t l = strlen(key), k = 0;
	for (const char *s = q; s; s = strchr(s + 1, '&')) {
		if (strncmp(s + 1, key, l) || s[1 + l] != '=') continue;
		for (s += l + 2; *s && *s != '&' && k + 1 < n; s++) {
			if (*s == '%' && hexv(s[1]) >= 0 && hexv(s[2]) >= 0) { out[k++] = hexv(s[1]) * 16 + hexv(s[2]); s += 2; }
			else out[k++] = *s == '+' ? ' ' : *s;
		}
		out[k] = 0;
		return 1;
	}
	out[0] = 0;
	return 0;
}
// URL (tras "/music/") -> ruta relativa saneada en out. Devuelve 0 si no es válida.
static int web_path(const char *url, char *out, size_t n) {
	char d[1024];
	size_t k = 0;
	for (const char *s = url; *s && *s != '?' && *s != '#' && k < sizeof d - 1; s++) {
		if (*s == '%' && hexv(s[1]) >= 0 && hexv(s[2]) >= 0) { d[k++] = hexv(s[1]) * 16 + hexv(s[2]); s += 2; }
		else d[k++] = *s;
	}
	d[k] = 0;
	size_t o = 0;
	char *sv, *seg;
	for (seg = strtok_r(d, "/", &sv); seg; seg = strtok_r(NULL, "/", &sv)) {
		size_t a = o;
		for (unsigned char *c = (unsigned char *)seg; *c; c++) {
			int len = *c >= 0xF0 ? 4 : 1;   // 4 bytes: FAT no los admite (PROBLEMAS #27)
			char ch = len == 4 || *c < 32 || *c == 127 || strchr("\\:*?\"<>|", *c) ? '_' : *c;
			while (len > 1 && c[1] >= 0x80 && c[1] < 0xC0) { c++; len--; }
			if (o + 2 >= n) return 0;
			out[o++] = ch;
		}
		while (o > a && (out[o - 1] == ' ' || out[o - 1] == '.')) o--;   // FAT quita puntos y espacios finales
		if (o == a || out[a] == '.') return 0;                               // vacío, "." , ".." u oculto
		out[o++] = '/';
	}
	if (!o) return 0;
	out[o - 1] = 0;
	return 1;
}

static void web_head(int fd, int code, const char *type, long long len) {
	const char *m = code == 200 ? "OK" : code == 201 ? "Created" : code == 204 ? "No Content" : code == 400 ? "Bad Request" : code == 404 ? "Not Found"
	              : code == 507 ? "Insufficient Storage" : "Error";
	char h[256];
	int k = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nConnection: close\r\nCache-Control: no-store\r\n", code, m);
	if (type) k += snprintf(h + k, sizeof h - k, "Content-Type: %s\r\n", type);
	if (len >= 0) k += snprintf(h + k, sizeof h - k, "Content-Length: %lld\r\n", len);
	k += snprintf(h + k, sizeof h - k, "\r\n");
	send(fd, h, k, MSG_NOSIGNAL);
}
static void web_file(int fd, const char *path, const char *type, int head) {
	int f = open(path, O_RDONLY);
	struct stat st;
	if (f < 0 || fstat(f, &st) || !S_ISREG(st.st_mode)) { if (f >= 0) close(f); web_head(fd, 404, NULL, 0); return; }
	web_head(fd, 200, type, st.st_size);
	char b[16384];
	ssize_t r;
	while (!head && (r = read(f, b, sizeof b)) > 0 && send(fd, b, r, MSG_NOSIGNAL) == r) {}
	close(f);
}
static void mkdirs(char *p) {   // crea las carpetas de una ruta de archivo
	for (char *s = strchr(p + 1, '/'); s; s = strchr(s + 1, '/')) { *s = 0; mkdir(p, 0755); *s = '/'; }
}
// Recibe el cuerpo (have bytes ya leídos en pre) en path.part y renombra. 0 si se cortó.
static int web_put(int fd, const char *root, const char *path, const char *rel, const char *pre, long long have, long long len) {
	struct statvfs v;
	if (!statvfs(root, &v) && (long long)v.f_bavail * v.f_bsize < len + (1 << 20)) { web_head(fd, 507, NULL, 0); return 1; }
	char part[1100];
	snprintf(part, sizeof part, "%s.part", path);
	mkdirs(part);
	int f = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (f < 0) { web_head(fd, 500, NULL, 0); return 1; }
	snprintf(web_name, sizeof web_name, "%s", rel);
	web_len = len; web_got = 0;
	if (have > len) have = len;
	int ok = write(f, pre, have) == have;
	web_got = have;
	static char b[65536];
	while (ok && web_got < len) {
		ssize_t r = recv(fd, b, len - web_got < (long long)sizeof b ? len - web_got : (long long)sizeof b, 0);
		if (r <= 0 || write(f, b, r) != r) ok = 0;
		else { web_got += r; web_last = tick_ms(); }
	}
	ok = !close(f) && ok && !rename(part, path);
	if (!ok) { unlink(part); web_len = 0; return 0; }
	web_files++; web_last = tick_ms(); web_len = 0;
	web_head(fd, 201, NULL, 0);
	return 1;
}
static void web_conn(int fd, const char *root, const char *page) {
	char h[4096], *e = NULL;
	int n = 0;
	while (n < (int)sizeof h - 1) {
		int r = recv(fd, h + n, sizeof h - 1 - n, 0);
		if (r <= 0) return;
		n += r; h[n] = 0;
		if ((e = strstr(h, "\r\n\r\n"))) break;
	}
	if (!e) { web_head(fd, 400, NULL, 0); return; }
	*e = 0;
	char m[8], url[2048], rel[1024], path[1100];
	if (sscanf(h, "%7s %2047s", m, url) != 2) { web_head(fd, 400, NULL, 0); return; }
	const char *cl = strcasestr(h, "\r\nContent-Length:");
	long long len = cl ? atoll(cl + 17) : -1;
	web_last = tick_ms();
	int get = !strcmp(m, "GET"), head = !strcmp(m, "HEAD");
	if (get && (!strcmp(url, "/") || !strcmp(url, "/index.html"))) { web_file(fd, page, "text/html; charset=utf-8", 0); return; }
	if (get && (!strcmp(url, "/api/lib") || !strncmp(url, "/api/tracks", 11))) {
		web_head(fd, 200, "application/json; charset=utf-8", -1);
		FILE *o = fdopen(dup(fd), "w");
		if (o) { web_api_get(url, o); fclose(o); }
		return;
	}
	if (!strcmp(m, "POST") && !strcmp(url, "/api/rescan")) { web_head(fd, 204, NULL, 0); web_api_rescan(); return; }
	if (strncmp(url, "/music/", 7) || !web_path(url + 7, rel, sizeof rel)) { web_head(fd, 404, NULL, 0); return; }
	snprintf(path, sizeof path, "%s/%s", root, rel);
	if (get || head) {
		size_t l = strlen(rel);
		int txt = (l > 4 && !strcasecmp(rel + l - 4, ".m3u")) || (l > 5 && !strcasecmp(rel + l - 5, ".m3u8"));
		web_file(fd, path, txt ? "text/plain; charset=utf-8" : "application/octet-stream", head);
	} else if (!strcmp(m, "PUT")) {
		if (len < 0) { web_head(fd, 400, NULL, 0); return; }
		if (strcasestr(h, "\r\nExpect: 100-continue")) send(fd, "HTTP/1.1 100 Continue\r\n\r\n", 25, MSG_NOSIGNAL);
		web_put(fd, root, path, rel, e + 4, n - (e + 4 - h), len);
	} else if (!strcmp(m, "DELETE")) {
		if (unlink(path)) { web_head(fd, 404, NULL, 0); return; }
		for (char *s = strrchr(path, '/'); s && s > path + strlen(root); s = strrchr(path, '/')) { *s = 0; if (rmdir(path)) break; }   // carpetas vacías
		web_head(fd, 204, NULL, 0);
	} else web_head(fd, 400, NULL, 0);
}
// Escucha en port mientras *on; root = carpeta de música, page = archivo HTML.
static void web_serve(int port, const char *root, const char *page, volatile int *on) {
	int ls = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0), one = 1;
	struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)};
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	if (ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 4)) { fprintf(stderr, "web: no se pudo escuchar en %d: %s\n", port, strerror(errno)); if (ls >= 0) close(ls); sleep(5); return; }
	fprintf(stderr, "web: escuchando en el puerto %d\n", port);
	while (*on) {
		struct pollfd p = {ls, POLLIN, 0};
		if (poll(&p, 1, 1000) <= 0) continue;
		int fd = accept4(ls, NULL, NULL, SOCK_CLOEXEC);
		if (fd < 0) continue;
		struct timeval tv = {20, 0};
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
		web_conn(fd, root, page);
		close(fd);
	}
	close(ls);
}
