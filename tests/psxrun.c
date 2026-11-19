/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "psx.h"
#include "disc.h"

// psxrun bios frames [exe]: runs headless; PSXRUN_SHOTS=dir/prefix saves pictures at PSXRUN_AT=frame,frame...

static void savePpm(struct Psx* p, const char* path) {
	static uint32_t pixels[576 * 768];
	int w, h;
	gpuDisplay(p, pixels, &w, &h);
	FILE* f = fopen(path, "wb");
	if (!f) return;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (int i = 0; i < w * h; i++) {
		uint8_t rgb[3] = { pixels[i] >> 16, pixels[i] >> 8, pixels[i] };
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
}

int main(int argc, char** argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: psxrun bios frames [exe]\n");
		return 1;
	}
	struct Psx* p = psxCreate();
	if (!psxLoadBios(p, argv[1])) {
		fprintf(stderr, "can't read the BIOS %s\n", argv[1]);
		return 1;
	}
	const char* image = argc > 3 ? argv[3] : NULL;
	size_t n = image ? strlen(image) : 0;
	if (image && n > 4 && (!strcasecmp(image + n - 4, ".cue") || !strcasecmp(image + n - 4, ".chd") || !strcasecmp(image + n - 4, ".m3u"))) {
		p->disc = discOpen(image);
		if (!p->disc) {
			fprintf(stderr, "can't read the disc %s\n", image);
			return 1;
		}
		image = NULL;
	}
	psxReset(p);
	if (image) {
		FILE* f = fopen(image, "rb");
		if (!f) return 1;
		fseek(f, 0, SEEK_END);
		p->exeSize = ftell(f);
		fseek(f, 0, SEEK_SET);
		p->exe = malloc(p->exeSize);
		if (fread(p->exe, 1, p->exeSize, f) != p->exeSize) return 1;
		fclose(f);
	}
	int frames = atoi(argv[2]);
	const char* shots = getenv("PSXRUN_SHOTS");
	const char* at = getenv("PSXRUN_AT");
	const char* wavPath = getenv("PSXRUN_WAV");
	FILE* wav = wavPath ? fopen(wavPath, "wb") : NULL;
	uint32_t wavBytes = 0;
	if (wav) fwrite("RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x02\0\x44\xac\0\0\x10\xb1\x02\0\x04\0\x10\0data\0\0\0\0", 1, 44, wav);
	static int16_t audio[4096 * 2];
	clock_t start = clock();
	for (int i = 1; i <= frames; i++) {
		psxRunFrame(p);
		int n = spuSamples(audio);
		if (wav) {
			fwrite(audio, 4, n, wav);
			wavBytes += n * 4;
		}
		if (shots && at) {
			char key[16];
			snprintf(key, sizeof key, ",%d,", i);
			char list[1024];
			snprintf(list, sizeof list, ",%s,", at);
			if (strstr(list, key)) {
				char path[512];
				snprintf(path, sizeof path, "%s-%05d.ppm", shots, i);
				savePpm(p, path);
			}
		}
		const char* ram = getenv("PSXRUN_RAM");
		int every = getenv("PSXRUN_RAM_EVERY") ? atoi(getenv("PSXRUN_RAM_EVERY")) : 0;
		if (ram && every && i % every == 0) {
			char path[512];
			snprintf(path, sizeof path, "%s-%05d.ram", ram, i);
			FILE* f = fopen(path, "wb");
			if (f) {
				fwrite(p->ram, 1, RAM_SIZE, f);
				fclose(f);
			}
		}
	}
	double secs = (double) (clock() - start) / CLOCKS_PER_SEC;
	const char* vram = getenv("PSXRUN_VRAM");
	if (vram) {
		FILE* f = fopen(vram, "wb");
		if (f) {
			fprintf(f, "P6\n1024 512\n255\n");
			for (int i = 0; i < 1024 * 512; i++) {
				uint16_t c = p->gpu.vram[i];
				uint8_t rgb[3] = { (uint8_t) ((c & 31) << 3), (uint8_t) (((c >> 5) & 31) << 3), (uint8_t) (((c >> 10) & 31) << 3) };
				fwrite(rgb, 1, 3, f);
			}
			fclose(f);
		}
	}
	if (wav) {
		uint32_t riff = wavBytes + 36;
		fseek(wav, 4, SEEK_SET);
		fwrite(&riff, 4, 1, wav);
		fseek(wav, 40, SEEK_SET);
		fwrite(&wavBytes, 4, 1, wav);
		fclose(wav);
	}
	fprintf(stderr, "%d frames in %.2f s (%.1f fps), pc %08x\n", frames, secs, frames / secs, p->cpu.pc);
	if (shots && !at) {
		char path[512];
		snprintf(path, sizeof path, "%s-%05d.ppm", shots, frames);
		savePpm(p, path);
	}
	return 0;
}
