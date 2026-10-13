// Pruebas del saneamiento de rutas del servidor web: gcc -I src src/webd_test.c -o /tmp/webd_test && /tmp/webd_test
#define _GNU_SOURCE
#include <assert.h>
#include <netinet/in.h>
#include <stdint.h>
#include <time.h>
#include "webd.h"
static uint32_t tick_ms(void) { return 0; }
static void web_api_get(const char *url, FILE *o) {}
static void web_api_rescan(void) {}

static const char *wp(const char *u) { static char b[256]; return web_path(u, b, sizeof b) ? b : NULL; }

int main(void) {
	assert(!strcmp(wp("Album/01%20Uno.flac"), "Album/01 Uno.flac"));
	assert(!strcmp(wp("a//b/c.mp3?x=1"), "a/b/c.mp3"));
	assert(!strcmp(wp("A%3A%2A%3F%22%3C%3E%7C%5Cb.mp3"), "A________b.mp3"));   // \ decodificado también pasa a "_"
	assert(!strcmp(wp("%F0%9D%96%87x.m4a"), "_x.m4a"));                         // carácter de 4 bytes
	assert(!strcmp(wp("%C3%91and%C3%BA/t.flac"), "Ñandú/t.flac"));
	assert(!strcmp(wp("dir. /t.flac"), "dir/t.flac"));
	assert(!wp("../etc/passwd"));
	assert(!wp("a/%2E%2E/b"));
	assert(!wp("a/./b"));
	assert(!wp(".r1/x"));
	assert(!wp(""));
	assert(!wp("/"));
	assert(!strcmp(wp("a%2Fb"), "a/b"));   // "/" codificado sigue siendo separador tras decodificar
	assert(!wp("a%2F..%2Fb"));
	char small[8];
	assert(!web_path("abcdefghij", small, sizeof small));
	char v[64];
	assert(web_arg("/api/tracks?q=Pink+Floyd%20x&o=200", "q", v, sizeof v) && !strcmp(v, "Pink Floyd x"));
	assert(web_arg("/api/tracks?q=Pink&o=200", "o", v, sizeof v) && !strcmp(v, "200"));
	assert(!web_arg("/api/tracks?qq=1", "q", v, sizeof v) && !v[0]);
	assert(!web_arg("/api/tracks", "q", v, sizeof v));
	puts("webd: ok");
	return 0;
}
