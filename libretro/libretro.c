/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libretro.h"
#include "psx.h"
#include "disc.h"

static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_log_printf_t log_cb;

static struct Psx* psx;
static char gamePath[4096];
static int discs, discIndex;
static bool trayOpen;
static uint32_t* picture;
static int16_t audio[4096 * 2];

static void logf_(enum retro_log_level level, const char* fmt, ...) {
}

unsigned retro_api_version(void) {
	return RETRO_API_VERSION;
}

void retro_get_system_info(struct retro_system_info* info) {
	memset(info, 0, sizeof *info);
	info->library_name = "armPSX";
	info->library_version = "0.1";
	info->valid_extensions = "cue|chd|m3u|exe";
	info->need_fullpath = true;
	info->block_extract = false;
}

void retro_get_system_av_info(struct retro_system_av_info* info) {
	bool pal = psx && psx->pal;
	info->geometry.base_width = 320;
	info->geometry.base_height = pal ? 288 : 240;
	info->geometry.max_width = 768;
	info->geometry.max_height = 576;
	info->geometry.aspect_ratio = 4.0f / 3.0f;
	// Mednafen's rates: 53693182 / (3412.5 * 262.5) NTSC, 53203425 / (3405 * 312.5) PAL
	info->timing.fps = pal ? 53203425.0 / (3405.0 * 312.5) : 53693182.0 / (3412.5 * 262.5);
	info->timing.sample_rate = 44100;
}

// Disc control: the discs of an .m3u, swapped with the tray open.
static bool setEject(bool eject) {
	trayOpen = eject;
	if (!psx) return true;
	if (eject) {
		cdromSetDisc(psx, NULL, true);
	} else {
		discClose(psx->disc);
		psx->disc = discOpenIndex(gamePath, discIndex);
		cdromSetDisc(psx, psx->disc, false);
	}
	return true;
}

static bool getEject(void) {
	return trayOpen;
}

static unsigned getImageIndex(void) {
	return discIndex;
}

static bool setImageIndex(unsigned index) {
	if (!trayOpen || (int) index >= discs) return false;
	discIndex = index;
	return true;
}

static unsigned getNumImages(void) {
	return discs;
}

static bool replaceImage(unsigned index, const struct retro_game_info* info) {
	return false;
}

static bool addImage(void) {
	return false;
}

void retro_set_environment(retro_environment_t cb) {
	environ_cb = cb;
	static struct retro_log_callback logging;
	log_cb = cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging) ? logging.log : logf_;
	bool noGame = false;
	cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &noGame);
	static struct retro_disk_control_callback disk = {
		setEject, getEject, getImageIndex, setImageIndex, getNumImages, replaceImage, addImage,
	};
	cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE, &disk);
	static const struct retro_controller_description pads[] = { { "PlayStation Controller", RETRO_DEVICE_JOYPAD } };
	static const struct retro_controller_info ports[] = { { pads, 1 }, { pads, 1 }, { NULL, 0 } };
	cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*) ports);
	static const struct retro_variable options[] = {
		{ "armpsx_mode", "Mode (restart); Fast|Balanced|Exact" },
		{ NULL, NULL },
	};
	cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*) options);
}

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) {}
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }
void retro_set_controller_port_device(unsigned port, unsigned device) {}

void retro_init(void) {
	picture = calloc(768 * 576, 4);
}

void retro_deinit(void) {
	free(picture);
	picture = NULL;
}

// The first BIOS found for the disc's region, in the system folder.
static bool loadBios(char region) {
	static const char* const america[] = { "scph5501.bin", "SCPH5501.BIN", "scph7001.bin", "scph1001.bin", "SCPH1001.BIN", "scph101.bin", "scph7003.bin", NULL };
	static const char* const europe[] = { "scph5502.bin", "SCPH5502.BIN", "scph7002.bin", "scph1002.bin", "SCPH1002.BIN", "scph102.bin", NULL };
	static const char* const japan[] = { "scph5500.bin", "SCPH5500.BIN", "scph7000.bin", "scph1000.bin", "SCPH1000.BIN", "scph100.bin", NULL };
	static const char* const any[] = { "psxonpsp660.bin", "PSXONPSP660.BIN", "scph5501.bin", "scph5502.bin", "scph5500.bin", NULL };
	const char* dir = NULL;
	if (!environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) || !dir) return false;
	const char* const* lists[2] = { region == 'E' ? europe : region == 'I' ? japan : america, any };
	for (int l = 0; l < 2; l++) {
		for (int i = 0; lists[l][i]; i++) {
			char path[4096];
			snprintf(path, sizeof path, "%s/%s", dir, lists[l][i]);
			if (psxLoadBios(psx, path)) {
				log_cb(RETRO_LOG_INFO, "armPSX: BIOS %s\n", path);
				return true;
			}
		}
	}
	return false;
}

static bool hasExtension(const char* path, const char* ext) {
	size_t n = strlen(path), e = strlen(ext);
	return n > e && !strcasecmp(path + n - e, ext);
}

bool retro_load_game(const struct retro_game_info* game) {
	if (!game || !game->path) return false;
	enum retro_pixel_format format = RETRO_PIXEL_FORMAT_XRGB8888;
	if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format)) return false;
	psx = psxCreate();
	struct retro_variable mode = { "armpsx_mode", NULL };
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &mode) && mode.value) psx->mode = psxMode(mode.value);
	snprintf(gamePath, sizeof gamePath, "%s", game->path);
	if (hasExtension(game->path, ".exe")) {
		FILE* f = fopen(game->path, "rb");
		if (!f) return false;
		fseek(f, 0, SEEK_END);
		psx->exeSize = ftell(f);
		fseek(f, 0, SEEK_SET);
		psx->exe = malloc(psx->exeSize);
		if (fread(psx->exe, 1, psx->exeSize, f) != psx->exeSize) {
			fclose(f);
			return false;
		}
		fclose(f);
		discs = 0;
	} else {
		discs = discCount(game->path);
		discIndex = 0;
		psx->disc = discOpenIndex(game->path, 0);
		if (!psx->disc) {
			log_cb(RETRO_LOG_ERROR, "armPSX: can't read %s\n", game->path);
			return false;
		}
		psx->region = discRegion(psx->disc);
	}
	psx->pal = psx->region == 'E';
	if (!loadBios(psx->region)) {
		log_cb(RETRO_LOG_ERROR, "armPSX: no BIOS in the system folder (scph5501.bin, scph5502.bin, scph5500.bin, ...)\n");
		return false;
	}
	sioFormatCard(&psx->sio.card[0]);
	psxReset(psx);
	return true;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info* info, size_t num) {
	return false;
}

void retro_unload_game(void) {
	if (!psx) return;
	discClose(psx->disc);
	free(psx->exe);
	jitFree(psx);
	gpuFree(psx);
	free(psx);
	psx = NULL;
}

unsigned retro_get_region(void) {
	return psx && psx->pal ? RETRO_REGION_PAL : RETRO_REGION_NTSC;
}

static uint16_t readPad(unsigned port) {
	static const unsigned ids[16] = {
		RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_L3, RETRO_DEVICE_ID_JOYPAD_R3, RETRO_DEVICE_ID_JOYPAD_START,
		RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_RIGHT, RETRO_DEVICE_ID_JOYPAD_DOWN, RETRO_DEVICE_ID_JOYPAD_LEFT,
		RETRO_DEVICE_ID_JOYPAD_L2, RETRO_DEVICE_ID_JOYPAD_R2, RETRO_DEVICE_ID_JOYPAD_L, RETRO_DEVICE_ID_JOYPAD_R,
		RETRO_DEVICE_ID_JOYPAD_X, RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_Y,
	};
	uint16_t buttons = 0;
	for (int i = 0; i < 16; i++) {
		if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, ids[i])) buttons |= 1 << i;
	}
	return buttons;
}

void retro_run(void) {
	input_poll_cb();
	psx->sio.pad[0].buttons = readPad(0);
	psx->sio.pad[1].buttons = readPad(1);
	psxRunFrame(psx);
	int w, h;
	gpuDisplay(psx, picture, &w, &h);
	if (w != 320 || h != (psx->pal ? 288 : 240)) {
		struct retro_game_geometry g = { w, h, 768, 576, 4.0f / 3.0f };
		environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &g);
	}
	video_cb(picture, w, h, w * 4);
	int n = spuSamples(audio);
	if (n > 0) audio_batch_cb(audio, n);
}

void retro_reset(void) {
	psxReset(psx);
}

size_t retro_serialize_size(void) { return 0; }
bool retro_serialize(void* data, size_t size) { return false; }
bool retro_unserialize(const void* data, size_t size) { return false; }
void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char* code) {}

void* retro_get_memory_data(unsigned id) {
	if (!psx) return NULL;
	switch (id) {
	case RETRO_MEMORY_SAVE_RAM: return psx->sio.card[0].data;
	case RETRO_MEMORY_SYSTEM_RAM: return psx->ram;
	}
	return NULL;
}

size_t retro_get_memory_size(unsigned id) {
	switch (id) {
	case RETRO_MEMORY_SAVE_RAM: return sizeof psx->sio.card[0].data;
	case RETRO_MEMORY_SYSTEM_RAM: return RAM_SIZE;
	}
	return 0;
}
