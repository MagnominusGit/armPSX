/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "psx.h"

struct Psx* psxCreate(void) {
	struct Psx* p = calloc(1, sizeof *p);
	GTE_Init();
	p->logCd = getenv("PSX_CDLOG") != NULL;
	p->logDma = getenv("PSX_DMALOG") != NULL;
	p->logGpu = getenv("PSX_GPULOG") != NULL;
	p->ttyOut = getenv("PSX_TTY") != NULL;
	const char* jit = getenv("PSX_JIT");
	if (!jit || strcmp(jit, "0")) {
		jitCreate(p);
	}
	return p;
}

bool psxLoadBios(struct Psx* p, const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	size_t n = fread(p->bios, 1, BIOS_SIZE, f);
	fclose(f);
	return n == BIOS_SIZE;
}

void psxReset(struct Psx* p) {
	cpuReset(p);
	jitFlush(p);
	GTE_Power();
	memset(p->ram, 0, sizeof p->ram);
	memset(p->scratch, 0, sizeof p->scratch);
	memset(p->memControl, 0, sizeof p->memControl);
	p->ramSize = 0;
	p->iStat = p->iMask = 0;
	memset(&p->dma, 0, sizeof p->dma);
	p->dma.dpcr = 0x07654321;
	memset(p->timer, 0, sizeof p->timer);
	p->timersAt = p->cycles;
	gpuReset(p);
	spuReset(p);
	cdromReset(p);
	mdecReset(p);
	p->sio.mode = p->sio.control = p->sio.baud = 0;
	p->sio.txPending = p->sio.txBusy = p->sio.rxAvail = p->sio.irq = false;
	p->sio.dsrAt = p->sio.dsrUntil = 0;
	p->nextEvent = p->cycles;
}

void psxSchedule(struct Psx* p, int64_t at) {
	if (at < p->nextEvent) {
		p->nextEvent = at;
	}
}

void psxIrq(struct Psx* p, int irq) {
	p->iStat |= 1 << irq;
	psxSchedule(p, p->cycles);
}

// Brings everything besides the CPU up to the CPU's cycle; each part schedules its next event.
void psxEvents(struct Psx* p) {
	p->nextEvent = p->cycles + 4096;
	gpuRun(p);
	dmaRun(p);
	timersRun(p);
	sioRun(p);
	cdromRun(p);
	spuRun(p);
}

void psxRunFrame(struct Psx* p) {
	p->gpu.frameDone = false;
	p->frameStart = p->cycles;
	int64_t limit = p->cycles + CPU_HZ / 10;
	while (!p->gpu.frameDone && p->cycles < limit) {
		int64_t until = p->nextEvent;
		cpuRun(p, until > p->cycles ? until : p->cycles + 1);
	}
	p->frames++;
}

// A PS-X EXE copied in once the BIOS reaches the shell, as the shell itself would load it.
bool psxLoadExe(struct Psx* p, const uint8_t* data, size_t size) {
	if (size < 0x800 || memcmp(data, "PS-X EXE", 8)) {
		return false;
	}
	uint32_t pc, gp, dest, length, sp, spSize;
	memcpy(&pc, data + 0x10, 4);
	memcpy(&gp, data + 0x14, 4);
	memcpy(&dest, data + 0x18, 4);
	memcpy(&length, data + 0x1C, 4);
	memcpy(&sp, data + 0x30, 4);
	memcpy(&spSize, data + 0x34, 4);
	if (length > size - 0x800) {
		length = size - 0x800;
	}
	for (uint32_t i = 0; i < length; i++) {
		p->ram[(dest + i) & (RAM_SIZE - 1)] = data[0x800 + i];
	}
	p->cpu.pc = pc;
	p->cpu.next = pc + 4;
	p->cpu.r[28] = gp;
	if (sp) {
		p->cpu.r[29] = p->cpu.r[30] = sp + spSize;
	}
	p->exe = NULL;
	return true;
}
