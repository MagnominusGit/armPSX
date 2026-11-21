/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
// lrshot core game frames: runs a libretro core headless. LR_SHOTS=prefix LR_AT=f,f saves pictures,
// LR_RAM=prefix saves system RAM at the same frames, LR_OPT="key=value;key=value" sets options,
// SYSDIR is the system (BIOS) folder.
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "libretro.h"

static unsigned frame;
static const char* sysdir;
static unsigned format = RETRO_PIXEL_FORMAT_0RGB1555;
static char opts[4096];

static void logf_(enum retro_log_level level, const char* fmt, ...) {
	if (!getenv("LR_LOG")) return;
	va_list a;
	va_start(a, fmt);
	vfprintf(stderr, fmt, a);
	va_end(a);
}

static bool env(unsigned cmd, void* data) {
	switch (cmd) {
	case RETRO_ENVIRONMENT_GET_VARIABLE: {
		struct retro_variable* v = data;
		v->value = NULL;
		char key[256];
		snprintf(key, sizeof key, "%s=", v->key);
		char* at = strstr(opts, key);
		if (at && (at == opts || at[-1] == ';')) {
			static char value[256];
			at += strlen(key);
			size_t n = strcspn(at, ";");
			memcpy(value, at, n);
			value[n] = 0;
			v->value = value;
			return true;
		}
		return false;
	}
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		*(const char**) data = sysdir;
		return true;
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		((struct retro_log_callback*) data)->log = logf_;
		return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		format = *(enum retro_pixel_format*) data;
		return true;
	case RETRO_ENVIRONMENT_GET_CAN_DUPE:
		*(bool*) data = true;
		return true;
	}
	return false;
}

static const void* lastFrame;
static unsigned lastW, lastH;
static size_t lastPitch;
static uint8_t* copy;

static void video(const void* d, unsigned w, unsigned h, size_t pitch) {
	if (!d) return;
	if (!copy) copy = malloc(4096 * 2048 * 4);
	for (unsigned y = 0; y < h; y++) memcpy(copy + y * pitch, (const uint8_t*) d + y * pitch, w * (format == RETRO_PIXEL_FORMAT_XRGB8888 ? 4 : 2));
	lastFrame = copy;
	lastW = w;
	lastH = h;
	lastPitch = pitch;
}

static void savePpm(const char* path) {
	FILE* f = fopen(path, "wb");
	if (!f || !lastFrame) return;
	fprintf(f, "P6\n%u %u\n255\n", lastW, lastH);
	for (unsigned y = 0; y < lastH; y++) {
		for (unsigned x = 0; x < lastW; x++) {
			uint8_t rgb[3];
			const uint8_t* row = (const uint8_t*) lastFrame + y * lastPitch;
			if (format == RETRO_PIXEL_FORMAT_XRGB8888) {
				uint32_t p;
				memcpy(&p, row + x * 4, 4);
				rgb[0] = p >> 16; rgb[1] = p >> 8; rgb[2] = p;
			} else if (format == RETRO_PIXEL_FORMAT_RGB565) {
				uint16_t p;
				memcpy(&p, row + x * 2, 2);
				rgb[0] = (p >> 11) << 3; rgb[1] = ((p >> 5) & 63) << 2; rgb[2] = (p & 31) << 3;
			} else {
				uint16_t p;
				memcpy(&p, row + x * 2, 2);
				rgb[0] = ((p >> 10) & 31) << 3; rgb[1] = ((p >> 5) & 31) << 3; rgb[2] = (p & 31) << 3;
			}
			fwrite(rgb, 1, 3, f);
		}
	}
	fclose(f);
}

static void sample(int16_t l, int16_t r) {}
static size_t batch(const int16_t* d, size_t n) { return n; }
static void poll(void) {}
// LR_PRESS: the same script as psxrun's PSXRUN_PRESS, in PlayStation pad bits.
static uint16_t held;
static uint16_t pressed(const char* script, int frame) {
	uint16_t h = 0;
	const char* s = script;
	while (s && *s) {
		int a = 0, b = 0, c = 0;
		unsigned mask = 0;
		if (sscanf(s, "%d/%d@%d:%x", &a, &b, &c, &mask) == 4) {
			if (frame >= c && (frame - c) % a < b) h |= mask;
		} else if (sscanf(s, "%d-%d:%x", &a, &b, &mask) == 3) {
			if (frame >= a && frame <= b) h |= mask;
		}
		s = strchr(s, ',');
		if (s) s++;
	}
	return h;
}

static int16_t input(unsigned port, unsigned device, unsigned index, unsigned id) {
	static const uint16_t bit[16] = {
		[RETRO_DEVICE_ID_JOYPAD_SELECT] = 0x0001, [RETRO_DEVICE_ID_JOYPAD_L3] = 0x0002, [RETRO_DEVICE_ID_JOYPAD_R3] = 0x0004,
		[RETRO_DEVICE_ID_JOYPAD_START] = 0x0008, [RETRO_DEVICE_ID_JOYPAD_UP] = 0x0010, [RETRO_DEVICE_ID_JOYPAD_RIGHT] = 0x0020,
		[RETRO_DEVICE_ID_JOYPAD_DOWN] = 0x0040, [RETRO_DEVICE_ID_JOYPAD_LEFT] = 0x0080, [RETRO_DEVICE_ID_JOYPAD_L2] = 0x0100,
		[RETRO_DEVICE_ID_JOYPAD_R2] = 0x0200, [RETRO_DEVICE_ID_JOYPAD_L] = 0x0400, [RETRO_DEVICE_ID_JOYPAD_R] = 0x0800,
		[RETRO_DEVICE_ID_JOYPAD_X] = 0x1000, [RETRO_DEVICE_ID_JOYPAD_A] = 0x2000, [RETRO_DEVICE_ID_JOYPAD_B] = 0x4000,
		[RETRO_DEVICE_ID_JOYPAD_Y] = 0x8000,
	};
	if (port != 0 || device != RETRO_DEVICE_JOYPAD || id >= 16) return 0;
	return (held & bit[id]) != 0;
}

int main(int argc, char** argv) {
	if (argc < 4) {
		fprintf(stderr, "usage: lrshot core game frames\n");
		return 2;
	}
	sysdir = getenv("SYSDIR") ? getenv("SYSDIR") : ".";
	if (getenv("LR_OPT")) snprintf(opts, sizeof opts, "%s", getenv("LR_OPT"));
	void* core = dlopen(argv[1], RTLD_NOW);
	if (!core) {
		fprintf(stderr, "%s\n", dlerror());
		return 1;
	}
#define F(name) __typeof__(&name) p_##name = dlsym(core, #name)
	F(retro_set_environment); F(retro_set_video_refresh); F(retro_set_audio_sample); F(retro_set_audio_sample_batch);
	F(retro_set_input_poll); F(retro_set_input_state); F(retro_init); F(retro_load_game); F(retro_run);
	F(retro_get_memory_data); F(retro_get_memory_size);
	p_retro_set_environment(env);
	p_retro_init();
	p_retro_set_video_refresh(video);
	p_retro_set_audio_sample(sample);
	p_retro_set_audio_sample_batch(batch);
	p_retro_set_input_poll(poll);
	p_retro_set_input_state(input);
	struct retro_game_info game = { argv[2], NULL, 0, NULL };
	if (!p_retro_load_game(&game)) {
		fprintf(stderr, "load failed\n");
		return 1;
	}
	int frames = atoi(argv[3]);
	const char* shots = getenv("LR_SHOTS");
	const char* ram = getenv("LR_RAM");
	char list[1024];
	snprintf(list, sizeof list, ",%s,", getenv("LR_AT") ? getenv("LR_AT") : "");
	// LR_FROM=n: the speed counts from frame n on
	int from = getenv("LR_FROM") ? atoi(getenv("LR_FROM")) : 1;
	struct timespec start, end;
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (frame = 1; frame <= (unsigned) frames; frame++) {
		if ((int) frame == from) clock_gettime(CLOCK_MONOTONIC, &start);
		held = pressed(getenv("LR_PRESS"), frame);
		p_retro_run();
		char key[16];
		snprintf(key, sizeof key, ",%u,", frame);
		if (strstr(list, key)) {
			char path[512];
			if (shots) {
				snprintf(path, sizeof path, "%s-%05u.ppm", shots, frame);
				savePpm(path);
			}
			if (ram) {
				snprintf(path, sizeof path, "%s-%05u.ram", ram, frame);
				FILE* f = fopen(path, "wb");
				if (f) {
					fwrite(p_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM), 1, p_retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM), f);
					fclose(f);
				}
			}
		}
	}
	clock_gettime(CLOCK_MONOTONIC, &end);
	double secs = (double) (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
	int timed = frames - (from > 1 ? from - 1 : 0);
	fprintf(stderr, "%d frames in %.2f s (%.1f fps)\n", timed, secs, timed / secs);
	return 0;
}
