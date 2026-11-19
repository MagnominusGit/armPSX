/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "psx.h"

// The SPU's registers and sound RAM transfers. Voices and mixing come with the Mednafen port.

void spuReset(struct Psx* p) {
	memset(&p->spu, 0, sizeof p->spu);
}

uint16_t spuRead(struct Psx* p, uint32_t address) {
	struct Spu* s = &p->spu;
	uint32_t r = (address - 0x1F801C00) >> 1;
	switch (address) {
	case 0x1F801DAA: return s->control;
	case 0x1F801DAE: return s->stat;
	case 0x1F801DA6: return s->regs[r];
	case 0x1F801DA8: return 0;
	}
	if (r < 512) return s->regs[r];
	return 0;
}

void spuWrite(struct Psx* p, uint32_t address, uint16_t value) {
	struct Spu* s = &p->spu;
	uint32_t r = (address - 0x1F801C00) >> 1;
	if (r < 512) s->regs[r] = value;
	switch (address) {
	case 0x1F801DAA:
		s->control = value;
		s->stat = (s->stat & ~0x3Fu) | (value & 0x3F);
		s->stat = (s->stat & ~0x380u) | ((value & 0x30) << 3);
		break;
	case 0x1F801DA6:
		s->transferAddress = (uint32_t) value << 2;
		break;
	case 0x1F801DA8:
		s->ram[s->transferAddress & 0x3FFFF] = value;
		s->transferAddress = (s->transferAddress + 1) & 0x3FFFF;
		break;
	}
}

void spuDmaWrite(struct Psx* p, uint32_t value) {
	struct Spu* s = &p->spu;
	s->ram[s->transferAddress & 0x3FFFF] = value;
	s->ram[(s->transferAddress + 1) & 0x3FFFF] = value >> 16;
	s->transferAddress = (s->transferAddress + 2) & 0x3FFFF;
}

uint32_t spuDmaRead(struct Psx* p) {
	struct Spu* s = &p->spu;
	uint32_t v = s->ram[s->transferAddress & 0x3FFFF] | (uint32_t) s->ram[(s->transferAddress + 1) & 0x3FFFF] << 16;
	s->transferAddress = (s->transferAddress + 2) & 0x3FFFF;
	return v;
}

void spuRun(struct Psx* p) {
}
