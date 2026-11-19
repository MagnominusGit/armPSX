/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <string.h>
#include "psx.h"

// Physical memory map; the CPU's segments mirror it. Wait states per Mednafen. Byte and halfword
// stores reach I/O registers with the CPU register's whole value, as the I/O bus carries it all
// (ps1-tests io-access-bitwidth on hardware).

static inline uint32_t physical(uint32_t address) {
	static const uint32_t mask[8] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x7FFFFFFF, 0x1FFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF };
	return address & mask[address >> 29];
}

static uint32_t ioReadInner(struct Psx* p, uint32_t a, int size);

static uint32_t ioRead(struct Psx* p, uint32_t a, int size) {
	uint32_t v = ioReadInner(p, a, size);
	if (p->traceIo > 0) {
		p->traceIo--;
		fprintf(stderr, "io r%d %08x = %08x pc %08x\n", size * 8, a, v, p->cpu.pc);
	}
	return v;
}

static uint32_t ioReadInner(struct Psx* p, uint32_t a, int size) {
	if (p->cycles >= p->nextEvent) {
		psxEvents(p);
	}
	if (a >= 0x1F801C00 && a < 0x1F802000) {
		p->cycles += size == 4 ? 36 : 16;
		uint32_t v = spuRead(p, a & ~1u);
		if (size == 4) {
			v |= (uint32_t) spuRead(p, (a & ~1u) + 2) << 16;
		}
		return a & 1 ? v >> 8 : v;
	}
	if (a >= 0x1F801800 && a < 0x1F801810) {
		p->cycles += size == 1 ? 6 : size == 2 ? 12 : 24;
		uint32_t v = cdromRead(p, a & 3);
		if (size > 1) {
			v |= (uint32_t) cdromRead(p, a & 3) << 8;
		}
		if (size > 2) {
			v |= (uint32_t) cdromRead(p, a & 3) << 16 | (uint32_t) cdromRead(p, a & 3) << 24;
		}
		return v;
	}
	p->cycles++;
	uint32_t shift = (a & 3) * 8;
	if (a >= 0x1F801810 && a < 0x1F801818) {
		return gpuRead(p, a & ~3u) >> shift;
	}
	if (a >= 0x1F801820 && a < 0x1F801828) {
		return mdecRead(p, a & ~3u) >> shift;
	}
	if (a >= 0x1F801000 && a < 0x1F801024) {
		return p->memControl[(a & 0x1F) >> 2] >> shift;
	}
	if (a >= 0x1F801060 && a < 0x1F801064) {
		return p->ramSize >> shift;
	}
	if (a >= 0x1F801040 && a < 0x1F801060) {
		return sioRead(p, a, size);
	}
	if (a >= 0x1F801070 && a < 0x1F801078) {
		return (a & 4 ? p->iMask : p->iStat) >> (shift & 8);
	}
	if (a >= 0x1F801080 && a < 0x1F801100) {
		return dmaRead(p, a & ~3u) >> shift;
	}
	if (a >= 0x1F801100 && a < 0x1F801140) {
		return timerRead(p, a & ~3u) >> shift;
	}
	if (a >= 0x1F802000) {
		return 0xFFFFFFFF;
	}
	return 0;
}

static void ioWrite(struct Psx* p, uint32_t a, uint32_t v, int size) {
	if (p->traceIo > 0) {
		p->traceIo--;
		fprintf(stderr, "io w%d %08x = %08x pc %08x\n", size * 8, a, v, p->cpu.pc);
	}
	if (p->cycles >= p->nextEvent) {
		psxEvents(p);
	}
	if (a >= 0x1F801C00 && a < 0x1F802000) {
		spuWrite(p, a & ~1u, v);
		if (size == 4) {
			spuWrite(p, (a & ~1u) + 2, v >> 16);
		}
		return;
	}
	if (a >= 0x1F801800 && a < 0x1F801810) {
		cdromWrite(p, a & 3, v);
		return;
	}
	uint32_t shift = (a & 3) * 8;
	if (a >= 0x1F801810 && a < 0x1F801818) {
		gpuWrite(p, a & ~3u, v << shift);
	} else if (a >= 0x1F801820 && a < 0x1F801828) {
		mdecWrite(p, a & ~3u, v << shift);
	} else if (a >= 0x1F801000 && a < 0x1F801024) {
		p->memControl[(a & 0x1F) >> 2] = v << shift;
	} else if (a >= 0x1F801060 && a < 0x1F801064) {
		p->ramSize = v << shift;
	} else if (a >= 0x1F801040 && a < 0x1F801060) {
		sioWrite(p, a, v, size);
	} else if (a >= 0x1F801070 && a < 0x1F801078) {
		if (a & 4) {
			p->iMask = (v << shift) & 0x7FF;
		} else {
			p->iStat &= v << shift;
		}
		if (p->cycles + 1 < p->nextEvent) {
			p->nextEvent = p->cycles + 1;
		}
	} else if (a >= 0x1F801080 && a < 0x1F801100) {
		dmaWrite(p, a & ~3u, v << shift);
	} else if (a >= 0x1F801100 && a < 0x1F801140) {
		timerWrite(p, a & ~3u, v << shift);
	} else if (a >= 0x1F802000 && a < 0x1F802100) {
		if (a == 0x1F802023 || a == 0x1F802080) {
			// DUART / PCSX debug output
		}
	}
}

uint32_t busRead32(struct Psx* p, uint32_t address) {
	uint32_t a = physical(address);
	if (a < 0x800000) {
		p->cycles += 3;
		uint32_t v;
		memcpy(&v, p->ram + (a & (RAM_SIZE - 1)), 4);
		return v;
	}
	if (a >= 0x1FC00000 && a < 0x1FC80000) {
		uint32_t v;
		memcpy(&v, p->bios + (a & (BIOS_SIZE - 1)), 4);
		return v;
	}
	if (a >= 0x1F801000 && a < 0x1F803000) {
		return ioRead(p, a, 4);
	}
	if (a == 0xFFFE0130) {
		return p->cpu.biu;
	}
	if (a >= 0x1F000000 && a < 0x1F800000) {
		return 0xFFFFFFFF;
	}
	return 0;
}

uint16_t busRead16(struct Psx* p, uint32_t address) {
	uint32_t a = physical(address);
	if (a < 0x800000) {
		p->cycles += 3;
		uint16_t v;
		memcpy(&v, p->ram + (a & (RAM_SIZE - 1)), 2);
		return v;
	}
	if (a >= 0x1FC00000 && a < 0x1FC80000) {
		uint16_t v;
		memcpy(&v, p->bios + (a & (BIOS_SIZE - 1)), 2);
		return v;
	}
	if (a >= 0x1F801000 && a < 0x1F803000) {
		return ioRead(p, a, 2);
	}
	if (a >= 0x1F000000 && a < 0x1F800000) {
		return 0xFFFF;
	}
	return 0;
}

uint8_t busRead8(struct Psx* p, uint32_t address) {
	uint32_t a = physical(address);
	if (a < 0x800000) {
		p->cycles += 3;
		return p->ram[a & (RAM_SIZE - 1)];
	}
	if (a >= 0x1FC00000 && a < 0x1FC80000) {
		return p->bios[a & (BIOS_SIZE - 1)];
	}
	if (a >= 0x1F801000 && a < 0x1F803000) {
		return ioRead(p, a, 1);
	}
	if (a >= 0x1F000000 && a < 0x1F800000) {
		return 0xFF;
	}
	return 0;
}

void busWrite32(struct Psx* p, uint32_t address, uint32_t value) {
	uint32_t a = physical(address);
	if (a < 0x800000) {
		memcpy(p->ram + (a & (RAM_SIZE - 1)), &value, 4);
	} else if (a >= 0x1F801000 && a < 0x1F803000) {
		ioWrite(p, a, value, 4);
	} else if (a == 0xFFFE0130) {
		uint32_t old = p->cpu.biu;
		p->cpu.biu = value & ~0x440u;
		if ((old ^ p->cpu.biu) & 0x800) {
			for (int i = 0; i < 1024; i++) {
				p->cpu.icache[i].tag |= 2;
			}
		}
	}
}

void busWrite16(struct Psx* p, uint32_t address, uint32_t value) {
	uint32_t a = physical(address);
	if (a < 0x800000) {
		uint16_t h = value;
		memcpy(p->ram + (a & (RAM_SIZE - 1)), &h, 2);
	} else if (a >= 0x1F801000 && a < 0x1F803000) {
		ioWrite(p, a, value, 2);
	}
}

void busWrite8(struct Psx* p, uint32_t address, uint32_t value) {
	uint32_t a = physical(address);
	if (a < 0x800000) {
		p->ram[a & (RAM_SIZE - 1)] = value;
	} else if (a >= 0x1F801000 && a < 0x1F803000) {
		ioWrite(p, a, value, 1);
	}
}
