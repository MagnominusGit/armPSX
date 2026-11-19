/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "psx.h"

// The seven DMA channels: 0 MDEC in, 1 MDEC out, 2 GPU, 3 CD-ROM, 4 SPU, 5 expansion, 6 ordering
// table clear. A transfer runs whole when it starts and holds the CPU for its words.

enum { MDEC_IN, MDEC_OUT, GPU, CDROM, SPU, PIO, OTC };

static void updateIrq(struct Psx* p) {
	struct Dma* d = &p->dma;
	bool before = d->dicr >> 31;
	bool master = (d->dicr & 0x8000) || ((d->dicr & 0x800000) && (d->dicr >> 16 & d->dicr >> 24 & 0x7F));
	d->dicr = (d->dicr & 0x7FFFFFFF) | (uint32_t) master << 31;
	if (master && !before) {
		psxIrq(p, IRQ_DMA);
	}
}

static uint32_t ramRead(struct Psx* p, uint32_t address) {
	uint32_t v;
	memcpy(&v, p->ram + (address & 0x1FFFFC), 4);
	return v;
}

static void ramWrite(struct Psx* p, uint32_t address, uint32_t v) {
	memcpy(p->ram + (address & 0x1FFFFC), &v, 4);
}

static uint32_t deviceRead(struct Psx* p, int channel) {
	switch (channel) {
	case MDEC_OUT: { uint32_t offset; return mdecDmaRead(p, &offset); }
	case GPU: return gpuRead(p, 0x1F801810);
	case CDROM: return cdromDmaRead(p);
	case SPU: return spuDmaRead(p);
	}
	return 0xFFFFFFFF;
}

static void deviceWrite(struct Psx* p, int channel, uint32_t v) {
	switch (channel) {
	case MDEC_IN: mdecDmaWrite(p, v); break;
	case GPU: gpuGp0(p, v); break;
	case SPU: spuDmaWrite(p, v); break;
	}
}

// Clock cost of a word on each channel and of each block or list node (Mednafen's dma.cpp).
static int wordCost(int channel, uint32_t chcr) {
	int extra = channel == CDROM ? 8 : channel == SPU ? 47 : 0;
	int chop = chcr & 0x100 ? 7 : 0;
	return 1 + (extra > chop ? extra : chop);
}

static void finish(struct Psx* p, int channel) {
	struct Dma* d = &p->dma;
	d->chcr[channel] &= ~0x11000000u;
	if (d->dicr & 1u << (16 + channel)) {
		d->dicr |= 1u << (24 + channel);
	}
	updateIrq(p);
}

static void mdecPump(struct Psx* p);

static void run(struct Psx* p, int channel) {
	struct Dma* d = &p->dma;
	uint32_t chcr = d->chcr[channel];
	uint32_t address = d->madr[channel] & 0xFFFFFC;
	int step = chcr & 2 ? -4 : 4;
	bool toDevice = chcr & 1;
	int sync = chcr >> 9 & 3;
	uint32_t words = 0;
	int64_t clocks = 0;
	if (channel == OTC) {
		uint32_t count = d->bcr[channel] & 0xFFFF;
		if (count == 0) count = 0x10000;
		for (uint32_t i = 0; i < count; i++) {
			ramWrite(p, address, i == count - 1 ? 0xFFFFFF : (address - 4) & 0x1FFFFC);
			address -= 4;
		}
		words = count;
	} else if (sync == 2) {
		// Linked list (GPU): each node is a header (next address, word count) and its words.
		int guard = 0;
		for (;;) {
			uint32_t header = ramRead(p, address);
			uint32_t n = header >> 24;
			for (uint32_t i = 0; i < n; i++) {
				deviceWrite(p, channel, ramRead(p, address + 4 + i * 4));
			}
			words += n + 1;
			clocks += (n ? 15 : 10) + n * wordCost(channel, chcr);
			address = header & 0xFFFFFF;
			if ((address & 0x800000) || ++guard > 0x100000) {
				break;
			}
		}
		d->madr[channel] = 0xFFFFFF;
	} else if ((channel == MDEC_IN || channel == MDEC_OUT) && sync == 1) {
		uint32_t size = d->bcr[channel] & 0xFFFF, blocks = d->bcr[channel] >> 16;
		d->remaining[channel] = (size ? size : 0x10000) * (blocks ? blocks : 0x10000);
		d->cursor[channel] = address;
		d->bcr[channel] &= 0xFFFF;
		mdecPump(p);
		return;
	} else {
		uint32_t count;
		if (sync == 0) {
			count = d->bcr[channel] & 0xFFFF;
			if (count == 0) count = 0x10000;
		} else {
			uint32_t size = d->bcr[channel] & 0xFFFF, blocks = d->bcr[channel] >> 16;
			count = (size ? size : 0x10000) * (blocks ? blocks : 0x10000);
			d->bcr[channel] &= 0xFFFF;
		}
		for (uint32_t i = 0; i < count; i++) {
			if (toDevice) {
				deviceWrite(p, channel, ramRead(p, address));
			} else if (channel == MDEC_OUT) {
				uint32_t offset;
				uint32_t v = mdecDmaRead(p, &offset);
				ramWrite(p, address + (offset << 2), v);
			} else {
				ramWrite(p, address, deviceRead(p, channel));
			}
			address += step;
		}
		if (sync == 1) {
			d->madr[channel] = address & 0xFFFFFF;
			uint32_t blocks = count / ((d->bcr[channel] & 0xFFFF) ? (d->bcr[channel] & 0xFFFF) : 0x10000);
			clocks += blocks * 7;
		}
		words = count;
		clocks += (int64_t) count * wordCost(channel, chcr);
	}
	if (channel == OTC) {
		clocks = words;
	}
	if (sync == 0 || (channel == GPU && sync == 1)) {
		// Immediate transfers hold the CPU; so, nearly, do GPU block transfers (Mednafen steals
		// cycles from every CPU read while one runs).
		p->cycles += clocks;
		finish(p, channel);
	} else {
		// Block and linked-list transfers run beside the CPU; the channel is busy until they're done.
		d->doneAt[channel] = p->cycles + clocks;
		psxSchedule(p, d->doneAt[channel]);
	}
}

// The MDEC's channels move words only while the MDEC takes or gives them; a waiting channel stays busy
// and goes on when the other side makes room.
static void mdecPump(struct Psx* p) {
	struct Dma* d = &p->dma;
	bool progress = true;
	while (progress) {
		progress = false;
		for (int channel = MDEC_OUT; channel >= MDEC_IN; channel--) {
			uint32_t moved = 0;
			int step = d->chcr[channel] & 2 ? -4 : 4;
			while (d->remaining[channel]) {
				if (channel == MDEC_IN) {
					if (!mdecCanWrite(p)) break;
					mdecDmaWrite(p, ramRead(p, d->cursor[channel]));
				} else {
					if (!mdecCanRead(p)) break;
					uint32_t offset;
					uint32_t v = mdecDmaRead(p, &offset);
					ramWrite(p, d->cursor[channel] + (offset << 2), v);
				}
				d->cursor[channel] += step;
				d->remaining[channel]--;
				moved++;
			}
			if (moved) {
				progress = true;
				d->madr[channel] = d->cursor[channel] & 0xFFFFFF;
				if (!d->remaining[channel]) {
					d->doneAt[channel] = p->cycles + (int64_t) moved * wordCost(channel, d->chcr[channel]) + 7;
					psxSchedule(p, d->doneAt[channel]);
				}
			}
		}
	}
}

void dmaRun(struct Psx* p) {
	if (p->dma.remaining[MDEC_IN] || p->dma.remaining[MDEC_OUT]) {
		mdecPump(p);
	}
	struct Dma* d = &p->dma;
	for (int i = 0; i < 7; i++) {
		if (d->doneAt[i]) {
			if (p->cycles >= d->doneAt[i]) {
				d->doneAt[i] = 0;
				finish(p, i);
			} else {
				psxSchedule(p, d->doneAt[i]);
			}
		}
	}
}

static void maybeStart(struct Psx* p, int channel) {
	struct Dma* d = &p->dma;
	uint32_t chcr = d->chcr[channel];
	if (!(d->dpcr & 8u << (channel * 4)) || !(chcr & 0x01000000) || d->remaining[channel]) {
		return;
	}
	if (d->doneAt[channel]) {
		// Started again while the last transfer's time hasn't run out: its data has moved already.
		d->doneAt[channel] = 0;
		finish(p, channel);
		d->chcr[channel] = chcr;
	}
	if ((chcr >> 9 & 3) == 0 && !(chcr & 0x10000000)) {
		return;
	}
	run(p, channel);
}

uint32_t dmaRead(struct Psx* p, uint32_t address) {
	struct Dma* d = &p->dma;
	int channel = address >> 4 & 7;
	if (channel == 7) {
		return address & 4 ? d->dicr : d->dpcr;
	}
	switch (address & 0xC) {
	case 0: return d->madr[channel];
	case 4: return d->bcr[channel];
	case 8: return d->chcr[channel];
	}
	return 0;
}

void dmaWrite(struct Psx* p, uint32_t address, uint32_t value) {
	struct Dma* d = &p->dma;
	int channel = address >> 4 & 7;
	if (channel == 7) {
		if (address & 4) {
			d->dicr = (d->dicr & 0xFF000000) & ~(value & 0x7F000000);
			d->dicr |= value & 0x00FF803F;
			updateIrq(p);
		} else {
			d->dpcr = value;
			for (int i = 0; i < 7; i++) maybeStart(p, i);
		}
		return;
	}
	switch (address & 0xC) {
	case 0: d->madr[channel] = value & 0xFFFFFF; break;
	case 4: d->bcr[channel] = value; break;
	case 8:
		if (getenv("PSX_DMALOG")) fprintf(stderr, "dma%d chcr=%08x madr=%06x bcr=%08x at %lld doneAt %lld dpcr %08x\n", channel, value, d->madr[channel], d->bcr[channel], (long long) p->cycles, (long long) d->doneAt[channel], d->dpcr);
		if (channel == OTC) {
			value = (value & 0x51000000) | 2;
		}
		d->chcr[channel] = value & 0x71770703;
		if (!(value & 0x01000000)) {
			d->remaining[channel] = 0;
			d->doneAt[channel] = 0;
		}
		maybeStart(p, channel);
		break;
	}
}
