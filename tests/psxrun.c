/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "psx.h"

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
	psxReset(p);
	if (argc > 3) {
		FILE* f = fopen(argv[3], "rb");
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
	clock_t start = clock();
	for (int i = 1; i <= frames; i++) {
		psxRunFrame(p);
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
	}
	double secs = (double) (clock() - start) / CLOCKS_PER_SEC;
	fprintf(stderr, "%d frames in %.2f s (%.1f fps), pc %08x\n", frames, secs, frames / secs, p->cpu.pc);
	if (shots && !at) {
		char path[512];
		snprintf(path, sizeof path, "%s-%05d.ppm", shots, frames);
		savePpm(p, path);
	}
	return 0;
}
