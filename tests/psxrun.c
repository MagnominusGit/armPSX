/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#include "psx.h"
#include "disc.h"
#include "gles.h"
#include "glr.h"

// psxrun bios frames [exe]: runs headless; PSXRUN_SHOTS=dir/prefix saves pictures at PSXRUN_AT=frame,frame...
// PSXRUN_PRESS="from-to:buttons,..." holds pad 1 buttons (hex, pad order: 0x0008 Start, 0x4000 Cross,
// 0x0010 Up, 0x0040 Down, ...) over those frames; "every/len@from:buttons" repeats from a frame on.


// PSXRUN_PROF=1: samples where the emulation's thread is every 100 us of its CPU time (the drawing
// and disc threads aren't sampled), then prints the guest instructions the recompiled code spent it
// on and the host addresses outside it (for nm).
static uintptr_t profSamples[1 << 22];
static volatile int profCount;
static timer_t profTimer;

static void profHandler(int sig, siginfo_t* info, void* context) {
#if defined(__x86_64__)
	uintptr_t ip = ((ucontext_t*) context)->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
	uintptr_t ip = ((ucontext_t*) context)->uc_mcontext.pc;
#else
	uintptr_t ip = 0;
#endif
	if (profCount < (int) (sizeof profSamples / sizeof *profSamples)) profSamples[profCount++] = ip;
}

static void profStart(void) {
	struct sigaction sa = { 0 };
	sa.sa_sigaction = profHandler;
	sa.sa_flags = SA_SIGINFO | SA_RESTART;
	sigaction(SIGPROF, &sa, NULL);
	struct sigevent ev = { 0 };
	ev.sigev_notify = SIGEV_THREAD_ID;
	ev.sigev_signo = SIGPROF;
	ev._sigev_un._tid = (pid_t) syscall(SYS_gettid);
	struct itimerspec t = { { 0, 100000 }, { 0, 100000 } };
	if (timer_create(CLOCK_THREAD_CPUTIME_ID, &ev, &profTimer) == 0) timer_settime(profTimer, 0, &t, NULL);
}

static int countDesc(const void* a, const void* b) {
	const uint64_t* x = a;
	const uint64_t* y = b;
	return x[1] < y[1] ? 1 : x[1] > y[1] ? -1 : 0;
}

static void profReport(struct Psx* p) {
	timer_delete(profTimer);
	int n = profCount, inJit = 0;
	// guest opcode classes in recompiled code, and host addresses elsewhere (by 64-byte bucket)
	static uint64_t ops[64][2], host[1 << 16][2];
	int hosts = 0, guests = 0, versions[3] = { 0 };
	static uint64_t guest[4096][2];
	for (int i = 0; i < 64; i++) ops[i][0] = i;
	for (int i = 0; i < n; i++) {
		uint32_t instr;
		int version = 0;
		uint32_t pc = jitDescribe(p, (const void*) profSamples[i], &instr, &version);
		if (pc) {
			inJit++;
			if (pc > 1) versions[version]++;
			if (pc > 1) {
				int g;
				for (g = 0; g < guests && guest[g][0] != pc; g++) {}
				if (g == guests && guests < 4096) guest[guests++][0] = pc;
				if (g < 4096) guest[g][1]++;
			}
			ops[pc == 1 ? 63 : instr >> 26][1]++;
			continue;
		}
		uintptr_t a = profSamples[i];
		int k;
		for (k = 0; k < hosts && host[k][0] != a; k++) {}
		if (k == hosts && hosts < (1 << 16)) host[hosts++][0] = a;
		if (k < (1 << 16)) host[k][1]++;
	}
	qsort(ops, 64, sizeof ops[0], countDesc);
	fprintf(stderr, "prof versions: generic %d clean %d careful %d\n", versions[0], versions[1], versions[2]);
	fprintf(stderr, "prof: %d samples, %d in recompiled code, profReport at %lx\n", n, inJit, (unsigned long) (uintptr_t) profReport);
	for (int i = 0; i < 12 && ops[i][1]; i++) fprintf(stderr, "prof op %02x %.1f%%\n", (int) ops[i][0], 100.0 * ops[i][1] / n);
	qsort(guest, guests, sizeof guest[0], countDesc);
	for (int i = 0; i < 30 && i < guests; i++) fprintf(stderr, "prof guest %08lx %lu\n", (unsigned long) guest[i][0], (unsigned long) guest[i][1]);
	qsort(host, hosts, sizeof host[0], countDesc);
	for (int i = 0; i < hosts; i++) fprintf(stderr, "prof host %lx %lu\n", (unsigned long) host[i][0], (unsigned long) host[i][1]);
}

// The buttons held on a frame from PSXRUN_PRESS.
static uint16_t pressed(const char* script, int frame) {
	uint16_t held = 0;
	const char* s = script;
	while (s && *s) {
		int a = 0, b = 0, c = 0;
		unsigned mask = 0;
		if (sscanf(s, "%d/%d@%d:%x", &a, &b, &c, &mask) == 4) {
			if (frame >= c && (frame - c) % a < b) held |= mask;
		} else if (sscanf(s, "%d-%d:%x", &a, &b, &mask) == 3) {
			if (frame >= a && frame <= b) held |= mask;
		}
		s = strchr(s, ',');
		if (s) s++;
	}
	return held;
}

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

static void saveVram(const uint16_t* pixels, const char* path) {
	FILE* f = fopen(path, "wb");
	if (!f) return;
	fprintf(f, "P6\n1024 512\n255\n");
	for (int i = 0; i < 1024 * 512; i++) {
		uint16_t c = pixels[i];
		uint8_t rgb[3] = { (uint8_t) ((c & 31) << 3), (uint8_t) (((c >> 5) & 31) << 3), (uint8_t) (((c >> 10) & 31) << 3) };
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
}

// (Run where the renderer's GL is, with gpuGlRun.)
static void saveGlPicture(struct Glr* glr, void* arg) {
	const char* path = arg;
	int w, h;
	glrPictureFramebuffer(glr, &w, &h);
	uint32_t* pixels = malloc((size_t) w * h * 4);
	FILE* f = pixels ? fopen(path, "wb") : NULL;
	if (f) {
		glrReadPicture(glr, pixels);
		fprintf(f, "P6\n%d %d\n255\n", w, h);
		for (int i = 0; i < w * h; i++) {
			uint8_t rgb[3] = { pixels[i] >> 16, pixels[i] >> 8, pixels[i] };
			fwrite(rgb, 1, 3, f);
		}
		fclose(f);
	}
	free(pixels);
}

static void readGlVram(struct Glr* glr, void* vram) {
	glrReadVram(glr, vram);
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
		p->region = discRegion(p->disc);
		p->pal = p->region == 'E';
		image = NULL;
	}
	sioFormatCard(&p->sio.card[0]);
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
	struct timespec wallStart;
	clock_gettime(CLOCK_MONOTONIC, &wallStart);
	if (getenv("PSXRUN_WATCH")) p->watch = strtoul(getenv("PSXRUN_WATCH"), NULL, 16);
	if (getenv("PSXRUN_PCSTOP")) {
		p->pcRing = calloc(256, 4);
		p->pcStop = strtoul(getenv("PSXRUN_PCSTOP"), NULL, 16);
	}
	// PSX_RENDERER=gl (or vulkan, its GL done with Vulkan): the GL renderer draws a picture as well (PSX_SCALE times), saved next to the
	// software one (prefix-gl-frame.ppm); PSXRUN_GLVRAM=file saves its VRAM at the end.
	struct Gles* gles = NULL;
	const char* renderer = getenv("PSX_RENDERER");
	if (renderer && (!strcmp(renderer, "gl") || !strcmp(renderer, "vulkan"))) {
		char error[256] = "";
		glesUseVulkan(!strcmp(renderer, "vulkan"));
		gles = glesCreateContext(error, sizeof error);
		// The renderer on a thread of its own unless PSX_GL_THREAD=0
		bool threaded = !getenv("PSX_GL_THREAD") || strcmp(getenv("PSX_GL_THREAD"), "0");
		p->softPicture = shots != NULL;
		if (gles && threaded) glesRelease(gles);
		if (!gles || !gpuGlStart(p, gles, getenv("PSX_SCALE") ? atoi(getenv("PSX_SCALE")) : 1, threaded, error, sizeof error)) {
			fprintf(stderr, "GL renderer: %s\n", error);
			return 1;
		}
		fprintf(stderr, "GL renderer on %s\n", glesRenderer(gles));
	}
	if (getenv("PSXRUN_PROF")) profStart();
	int traceAt = getenv("PSXRUN_TRACE_AT") ? atoi(getenv("PSXRUN_TRACE_AT")) : -1;
	for (int i = 1; i <= frames; i++) {
		p->sio.pad[0].buttons = pressed(getenv("PSXRUN_PRESS"), i);
		if (i == traceAt) p->traceIo = getenv("PSXRUN_TRACE_N") ? atoi(getenv("PSXRUN_TRACE_N")) : 200;
		// PSXRUN_GLLOG=file: the GL primitives of the last frame; with PSXRUN_GLCHECK=n, of the first
		// frame after which GL's VRAM has more than n pixels unlike the software renderer's (then stops).
		const char* glLog = getenv("PSXRUN_GLLOG");
		bool glCheck = p->gpuGl && getenv("PSXRUN_GLCHECK");
		if (glLog && (i == frames || glCheck)) glrLogFile = fopen(glLog, "w");
		psxRunFrame(p);
		struct Glr* glr = gpuGlFrame(p);
		if (glrLogFile) {
			fclose(glrLogFile);
			glrLogFile = NULL;
		}
		if (glCheck) {
			static uint16_t glVram[1024 * 512];
			gpuGlRun(p, readGlVram, glVram);
			const uint16_t* sw = gpuVram(p);
			int differ = 0;
			for (int k = 0; k < 1024 * 512; k++) differ += (glVram[k] & 0x7FFF) != (sw[k] & 0x7FFF);
			if (differ > atoi(getenv("PSXRUN_GLCHECK"))) {
				fprintf(stderr, "frame %d: %d pixels differ\n", i, differ);
				saveVram(sw, "glcheck-sw.ppm");
				saveVram(glVram, "glcheck-gl.ppm");
				break;
			}
		}
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
				if (glr) {
					snprintf(path, sizeof path, "%s-gl-%05d.ppm", shots, i);
					gpuGlRun(p, saveGlPicture, path);
				}
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
	gpuSync(p);
	double cpu = (double) (clock() - start) / CLOCKS_PER_SEC;
	struct timespec wallEnd;
	clock_gettime(CLOCK_MONOTONIC, &wallEnd);
	double secs = (double) (wallEnd.tv_sec - wallStart.tv_sec) + (wallEnd.tv_nsec - wallStart.tv_nsec) / 1e9;
	const char* vram = getenv("PSXRUN_VRAM");
	if (vram) saveVram(gpuVram(p), vram);
	if (p->gpuGl && getenv("PSXRUN_GLVRAM")) {
		static uint16_t glVram[1024 * 512];
		gpuGlRun(p, readGlVram, glVram);
		saveVram(glVram, getenv("PSXRUN_GLVRAM"));
	}
	if (wav) {
		uint32_t riff = wavBytes + 36;
		fseek(wav, 4, SEEK_SET);
		fwrite(&riff, 4, 1, wav);
		fseek(wav, 40, SEEK_SET);
		fwrite(&wavBytes, 4, 1, wav);
		fclose(wav);
	}
	if (p->gpuGl && getenv("PSXRUN_GLSTATS")) {
		uint64_t st[GLR_STATS];
		glrStats(gpuGlFrame(p), st);
		double f = st[GLR_FRAMES] ? (double) st[GLR_FRAMES] : 1;
		fprintf(stderr, "gl per frame: %.1f flushes, %.1f draws, %.0f vertices, %.1f sample copies, %.1f fills, %.1f copies, %.1f uploads\n",
		        st[GLR_FLUSHES] / f, st[GLR_DRAWS] / f, st[GLR_VERTICES] / f, st[GLR_SYNCS] / f, st[GLR_FILLS] / f, st[GLR_COPIES] / f, st[GLR_UPLOADS] / f);
	}
	if (getenv("PSXRUN_PROF")) profReport(p);
	if (getenv("PSXRUN_JITDUMP")) jitDump(p, getenv("PSXRUN_JITDUMP"));
	if (getenv("PSXRUN_JITSTATS")) {
		uint64_t s[6];
		jitStats(p, s);
		fprintf(stderr, "jit: %llu entries, %llu declined, %llu compiled, %llu stale, %llu uncached, %llu flushes\n",
			(unsigned long long) s[0], (unsigned long long) s[1], (unsigned long long) s[2], (unsigned long long) s[3], (unsigned long long) s[4], (unsigned long long) s[5]);
	}
	fprintf(stderr, "%d frames in %.2f s (%.1f fps), %.2f s CPU, pc %08x\n", frames, secs, frames / secs, cpu, p->cpu.pc);
	if (getenv("PSXRUN_STATE")) {
		fprintf(stderr, "sr %08x cause %08x epc %08x istat %03x imask %03x dicr %08x\n", p->cpu.sr, p->cpu.cause, p->cpu.epc, p->iStat, p->iMask, p->dma.dicr);
		fprintf(stderr, "gpu inCommand %d code %08x fifo %d drawTime %d dmaControl %u fifo0 %08x rw %u,%u %ux%u cur %u,%u\n", p->gpu.inCommand, p->gpu.inCommandCode, p->gpu.fifoCount, p->gpu.drawTime, p->gpu.dmaControl, p->gpu.fifo[p->gpu.fifoRead & 31], p->gpu.rwX, p->gpu.rwY, p->gpu.rwW, p->gpu.rwH, p->gpu.rwCurX, p->gpu.rwCurY);
		for (int c = 0; c < 7; c++) fprintf(stderr, "dma%d chcr %08x madr %06x bcr %08x doneAt %lld\n", c, p->dma.chcr[c], p->dma.madr[c], p->dma.bcr[c], (long long) p->dma.doneAt[c]);
		for (int i = 0; i < 16; i++) {
			cpuRun(p, p->cycles + 1);
			fprintf(stderr, "pc %08x ", p->cpu.pc);
		}
		fprintf(stderr, "\n");
	}
	if (shots && !at) {
		char path[512];
		snprintf(path, sizeof path, "%s-%05d.ppm", shots, frames);
		savePpm(p, path);
	}
	gpuFree(p);
	return 0;
}
