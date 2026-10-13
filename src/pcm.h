// Salida PCM del CS43131 por ioctl directos (sin libasound): S16_LE o S24_LE, 4 periodos de 1024 frames.
#pragma once
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sound/asound.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define PCM_PERIOD 1024
#define PCM_PERIODS 4

static struct snd_pcm_hw_params pcm_hw;
#define PCM_MASK(p) pcm_hw.masks[(p) - SNDRV_PCM_HW_PARAM_FIRST_MASK]
#define PCM_IVAL(p) pcm_hw.intervals[(p) - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL]
#define PCM_FIX(p, v) (PCM_IVAL(p).min = PCM_IVAL(p).max = (v))

#ifdef R1_SIM
// Simulador: el DAC se sustituye por una tubería hacia R1_SIM_SOUND (p. ej. "aplay -q -t raw -f S16_LE -c 2 -r %u -") o,
// sin ella, por un descarte a ritmo real.
static FILE *sim_snd;
static unsigned sim_rate;
static int pcm_open(unsigned rate, unsigned ch, int format) {
	const char *c = getenv("R1_SIM_SOUND");
	char cmd[200];
	sim_rate = rate;
	if (c) { snprintf(cmd, sizeof cmd, c, rate); sim_snd = popen(cmd, "w"); }
	return open("/dev/null", O_WRONLY);
}
static int pcm_write(int fd, const void *buf, unsigned long n) {
	if (sim_snd) { fwrite(buf, 4, n, sim_snd); fflush(sim_snd); }
	else usleep(n * 1000000ULL / sim_rate);
	return 0;
}
static void pcm_close(int fd) { close(fd); if (sim_snd) { pclose(sim_snd); sim_snd = NULL; } }
#else
static int pcm_open(unsigned rate, unsigned ch, int format) {
	int fd = open("/dev/snd/pcmC0D0p", O_RDWR);
	if (fd < 0) return -1;
	memset(&pcm_hw, 0, sizeof pcm_hw);
	for (int i = 0; i < 3; i++) memset(pcm_hw.masks[i].bits, 0xff, sizeof pcm_hw.masks[i].bits);
	for (int i = 0; i < 12; i++) pcm_hw.intervals[i].max = UINT_MAX;
	pcm_hw.rmask = ~0u;
	memset(PCM_MASK(SNDRV_PCM_HW_PARAM_ACCESS).bits, 0, sizeof PCM_MASK(SNDRV_PCM_HW_PARAM_ACCESS).bits);
	PCM_MASK(SNDRV_PCM_HW_PARAM_ACCESS).bits[0] = 1u << SNDRV_PCM_ACCESS_RW_INTERLEAVED;
	memset(PCM_MASK(SNDRV_PCM_HW_PARAM_FORMAT).bits, 0, sizeof PCM_MASK(SNDRV_PCM_HW_PARAM_FORMAT).bits);
	PCM_MASK(SNDRV_PCM_HW_PARAM_FORMAT).bits[0] = 1u << format;
	PCM_FIX(SNDRV_PCM_HW_PARAM_CHANNELS, ch);
	PCM_FIX(SNDRV_PCM_HW_PARAM_RATE, rate);
	PCM_FIX(SNDRV_PCM_HW_PARAM_PERIOD_SIZE, PCM_PERIOD);
	PCM_FIX(SNDRV_PCM_HW_PARAM_PERIODS, PCM_PERIODS);
	if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &pcm_hw)) { close(fd); return -1; }
	unsigned long buf = PCM_IVAL(SNDRV_PCM_HW_PARAM_BUFFER_SIZE).min, boundary = buf;
	while (boundary * 2 <= LONG_MAX - buf) boundary *= 2;
	struct snd_pcm_sw_params sw = {.avail_min = PCM_PERIOD, .start_threshold = PCM_PERIOD * 2, .stop_threshold = boundary, .boundary = boundary, .proto = SNDRV_PCM_VERSION};
	if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw)) { close(fd); return -1; }
	if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE)) { close(fd); return -1; }
	return fd;
}

// Escribe n frames; devuelve cuántos underruns hubo que recuperar.
static int pcm_write(int fd, const void *buf, unsigned long n) {
	struct snd_xferi x = {.buf = (void *)buf, .frames = n};
	int underruns = 0;
	while (ioctl(fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x)) {
		if (errno != EPIPE) { perror("write"); return -1; }
		ioctl(fd, SNDRV_PCM_IOCTL_PREPARE);
		underruns++;
	}
	return underruns;
}
static void pcm_close(int fd) { close(fd); }
#endif
