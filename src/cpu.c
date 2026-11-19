/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "psx.h"

// The R3000A interpreter, with Mednafen's cycle model (cpu.cpp, Copyright (C) 2011-2016 Mednafen Team,
// GPL-2.0-or-later): one cycle an instruction, the instruction cache's fills, the bus's waits, and a
// load's wait hidden by the instructions after it until one uses the loaded register. A load's value
// lands one instruction late: each instruction reads its operands, then the load in flight is
// written, then the instruction's own result.

enum { EXC_INT = 0, EXC_ADEL = 4, EXC_ADES = 5, EXC_IBE = 6, EXC_DBE = 7, EXC_SYS = 8, EXC_BP = 9, EXC_RI = 10, EXC_CPU = 11, EXC_OV = 12 };
enum { NO_LOAD = 34 };

#define R p->cpu.r

static uint8_t multCycles[24];

static uint32_t exception(struct Psx* p, int code, uint32_t pc, uint32_t instr) {
	struct Cpu* c = &p->cpu;
	c->epc = pc;
	if (c->inDelay) {
		c->epc -= 4;
		c->tar = c->next;
	}
	c->sr = (c->sr & ~0x3F) | ((c->sr << 2) & 0x3F);
	c->cause = (c->cause & 0xFF00) | code << 2 | (uint32_t) c->inDelay << 31 | ((instr >> 26 & 3) << 28);
	c->inDelay = false;
	c->branched = false;
	return c->sr & 1 << 22 ? 0xBFC00180 : 0x80000080;
}

// DO_LDS
static inline void commitLoad(struct Cpu* c) {
	c->r[c->ldReg] = c->ldValue;
	c->absorb[c->ldReg] = c->ldAbsorb;
	c->readFudge = c->ldReg;
	c->absorbWhich |= c->ldReg & 0x1F;
	c->ldReg = NO_LOAD;
	c->r[0] = 0;
}

// The registers an instruction reads or writes stop hiding their loads' waits (r0's is kept).
static inline void uses(struct Cpu* c, uint32_t reg) {
	if (reg) c->absorb[reg] = 0;
}

static inline void setReg(struct Cpu* c, uint32_t reg, uint32_t value) {
	commitLoad(c);
	c->r[reg] = value;
	c->r[0] = 0;
}

static inline void load(struct Cpu* c, uint32_t reg, uint32_t value) {
	c->ldReg = reg;
	c->ldValue = value;
}

static uint32_t fetch(struct Psx* p, uint32_t address) {
	struct Cpu* c = &p->cpu;
	uint32_t* tag = &c->icache[(address & 0xFFC) >> 2].tag;
	if (*tag == address) {
		return c->icache[(address & 0xFFC) >> 2].data;
	}
	c->absorb[c->absorbWhich] = 0;
	c->absorbWhich = 0;
	uint32_t physical = address & 0x1FFFFFFF;
	if (physical >= 0x800000 && (physical < 0x1FC00000 || physical >= 0x1FC80000)) {
		// SPU and DMA registers read back like memory, so code runs from them (ps1-tests code-in-io).
		p->cycles += 4;
		return busRead32(p, address);
	}
	const uint8_t* memory = physical < 0x800000 ? p->ram + (physical & (RAM_SIZE - 1)) : p->bios + (physical & (BIOS_SIZE - 1));
	uint32_t instr;
	if (address >= 0xA0000000 || !(c->biu & 0x800)) {
		memcpy(&instr, memory, 4);
		p->cycles += 4;
		return instr;
	}
	// A miss fills the rest of the 4-word line from the missed word on.
	int line = (address & 0xFF0) >> 2, first = (address & 0xC) >> 2;
	c->icacheGen[line >> 2]++;
	c->icacheChanges++;
	p->cycles += 3;
	for (int i = 0; i < 4; i++) {
		c->icache[line + i].tag = (address & ~0xFu) | (uint32_t) i << 2 | (i < first ? 2 : 0);
		if (i >= first) {
			memcpy(&c->icache[line + i].data, memory - (first - i) * 4, 4);
			p->cycles++;
		}
	}
	return c->icache[line + first].data;
}

// The fetch the interpreter does, for the recompiler: a miss fills the line and takes its cycles.
uint32_t cpuFetch(struct Psx* p, uint32_t address) {
	return fetch(p, address);
}

static void isolatedWrite(struct Psx* p, uint32_t address, uint32_t value) {
	struct Cpu* c = &p->cpu;
	if (!(c->biu & 0x800)) {
		return;
	}
	c->icacheGen[(address & 0xFF0) >> 4]++;
	c->icacheChanges++;
	if (c->biu & 7) {
		uint32_t valid = c->biu & 4 ? (value << (address & 3) * 8) & 0xF : 0;
		int line = (address & 0xFF0) >> 2;
		for (int i = 0; i < 4; i++) {
			c->icache[line + i].tag = (valid & 1u << i ? 0 : 2) | (address & ~0xFu) | (uint32_t) i << 2;
		}
	} else {
		c->icache[(address & 0xFFC) >> 2].data = value << (address & 3) * 8;
	}
}

static inline bool scratchpad(uint32_t address) {
	return (address & 0x7FFFFC00) == 0x1F800000 && address < 0xA0000000;
}

// Every read ends the hiding of earlier loads; one not right after a load costs 2 cycles more; its
// wait, the bus's plus 2 (1 for LWC2), is what the next instructions may hide.
static inline bool beginRead(struct Psx* p, uint32_t address) {
	struct Cpu* c = &p->cpu;
	c->absorb[c->absorbWhich] = 0;
	c->absorbWhich = 0;
	if (scratchpad(address)) {
		c->ldAbsorb = 0;
		return true;
	}
	p->cycles += (c->readFudge >> 4) & 2;
	return false;
}

static uint32_t read32(struct Psx* p, uint32_t address) {
	uint32_t v;
	if (beginRead(p, address)) {
		memcpy(&v, p->scratch + (address & 0x3FF), 4);
		return v;
	}
	int64_t before = p->cycles;
	v = busRead32(p, address);
	p->cycles += 2;
	p->cpu.ldAbsorb = (uint8_t) (p->cycles - before);
	return v;
}

static uint32_t read16(struct Psx* p, uint32_t address) {
	if (beginRead(p, address)) {
		uint16_t v;
		memcpy(&v, p->scratch + (address & 0x3FF), 2);
		return v;
	}
	int64_t before = p->cycles;
	uint32_t v = busRead16(p, address);
	p->cycles += 2;
	p->cpu.ldAbsorb = (uint8_t) (p->cycles - before);
	return v;
}

static uint32_t read8(struct Psx* p, uint32_t address) {
	if (beginRead(p, address)) {
		return p->scratch[address & 0x3FF];
	}
	int64_t before = p->cycles;
	uint32_t v = busRead8(p, address);
	p->cycles += 2;
	p->cpu.ldAbsorb = (uint8_t) (p->cycles - before);
	return v;
}

static void write32(struct Psx* p, uint32_t address, uint32_t value) {
	if (p->cpu.sr & 0x10000) {
		isolatedWrite(p, address, value);
	} else if (scratchpad(address)) {
		memcpy(p->scratch + (address & 0x3FF), &value, 4);
	} else {
		busWrite32(p, address, value);
	}
}

static void write16(struct Psx* p, uint32_t address, uint32_t value) {
	if (p->cpu.sr & 0x10000) {
		isolatedWrite(p, address, value);
	} else if (scratchpad(address)) {
		memcpy(p->scratch + (address & 0x3FF), &value, 2);
	} else {
		busWrite16(p, address, value);
	}
}

static void write8(struct Psx* p, uint32_t address, uint32_t value) {
	if (p->cpu.sr & 0x10000) {
		isolatedWrite(p, address, value);
	} else if (scratchpad(address)) {
		p->scratch[address & 0x3FF] = value;
	} else {
		busWrite8(p, address, value);
	}
}

void cpuReset(struct Psx* p) {
	struct Cpu* c = &p->cpu;
	memset(c, 0, sizeof *c);
	c->pc = 0xBFC00000;
	c->next = c->pc + 4;
	c->sr = 1 << 22;
	c->cop0[15] = 0x00000002;
	c->ldReg = NO_LOAD;
	c->readFudge = NO_LOAD;
	for (int i = 0; i < 1024; i++) {
		c->icache[i].tag = 2;
	}
	for (int i = 0; i < 24; i++) {
		multCycles[i] = 7 + (i < 12 ? 4 : 0) + (i < 21 ? 3 : 0);
	}
}

static inline bool irqPending(struct Psx* p) {
	struct Cpu* c = &p->cpu;
	uint32_t cause = (c->cause & ~0x400u) | ((p->iStat & p->iMask) ? 0x400 : 0);
	c->cause = cause;
	return (c->sr & 1) && (c->sr & cause & 0xFF00);
}

bool cpuIrqPending(struct Psx* p) {
	return irqPending(p);
}

// MFHI/MFLO waiting on a multiply or divide; the wait's cycles still use up hidden load time.
static void waitMuldiv(struct Psx* p) {
	struct Cpu* c = &p->cpu;
	if (p->cycles < c->muldivDone) {
		if (p->cycles == c->muldivDone - 1) {
			c->muldivDone--;
		} else {
			do {
				if (c->absorb[c->absorbWhich]) c->absorb[c->absorbWhich]--;
				p->cycles++;
			} while (p->cycles < c->muldivDone);
		}
	}
}

// For the recompiler: MFHI/MFLO's wait, and DIV/DIVU's results.
void cpuJitWaitMuldiv(struct Psx* p) {
	waitMuldiv(p);
}

void cpuJitDivide(struct Psx* p, uint32_t s, uint32_t t, int sign) {
	if (sign) {
		if (t == 0) {
			R[32] = s;
			R[33] = (int32_t) s < 0 ? 1 : 0xFFFFFFFF;
		} else if (s == 0x80000000 && t == 0xFFFFFFFF) {
			R[32] = 0;
			R[33] = 0x80000000;
		} else {
			R[32] = (uint32_t) ((int32_t) s % (int32_t) t);
			R[33] = (uint32_t) ((int32_t) s / (int32_t) t);
		}
	} else if (t == 0) {
		R[32] = s;
		R[33] = 0xFFFFFFFF;
	} else {
		R[32] = s % t;
		R[33] = s / t;
	}
}

static void waitGte(struct Psx* p) {
	if (p->cycles < p->cpu.gteDone) {
		p->cycles = p->cpu.gteDone;
	}
}

static uint32_t cop0Read(struct Cpu* c, int reg) {
	switch (reg) {
	case 3: case 5: case 6: case 7: case 9: case 11: return c->cop0[reg];
	case 8: return c->badVaddr;
	case 12: return c->sr;
	case 13: return c->cause;
	case 14: return c->epc;
	case 15: return 0x00000002;
	}
	return 0;
}

static void cop0Write(struct Cpu* c, int reg, uint32_t value) {
	switch (reg) {
	case 3: case 5: case 7: case 9: case 11: c->cop0[reg] = value; break;
	case 12: c->sr = value & ~((3u << 26) | (3u << 23) | (3u << 6)); break;
	case 13: c->cause = (c->cause & ~0x300u) | (value & 0x300); break;
	}
}

static void tty(struct Psx* p, char ch) {
	if (ch == '\n' || p->ttyLength == (int) sizeof p->tty - 1) {
		p->tty[p->ttyLength] = 0;
		fprintf(stderr, "tty: %s\n", p->tty);
		p->ttyLength = 0;
	} else if (ch != '\r') {
		p->tty[p->ttyLength++] = ch;
	}
}

#define EXCEPTION(code) do { commitLoad(c); c->pc = exception(p, code, pc, instr); c->next = c->pc + 4; } while (0)

// Everything an instruction does after its fetch: pc moves on, then the instruction runs.
void cpuExecute(struct Psx* p, uint32_t pc, uint32_t instr) {
	struct Cpu* c = &p->cpu;
	uint32_t next = c->next;
	c->pc = next;
	c->next = next + 4;
	uint32_t rs = instr >> 21 & 31, rt = instr >> 16 & 31, rd = instr >> 11 & 31;
	uint32_t imm = (uint32_t) (int32_t) (int16_t) instr, immu = instr & 0xFFFF;
	uint32_t s = R[rs], t = R[rt];
	switch (instr >> 26) {
	case 0x00:
		switch (instr & 63) {
		case 0x00: uses(c, rt); uses(c, rd); setReg(c, rd, t << (instr >> 6 & 31)); break;
		case 0x02: uses(c, rt); uses(c, rd); setReg(c, rd, t >> (instr >> 6 & 31)); break;
		case 0x03: uses(c, rt); uses(c, rd); setReg(c, rd, (uint32_t) ((int32_t) t >> (instr >> 6 & 31))); break;
		case 0x04: uses(c, rs); uses(c, rt); uses(c, rd); setReg(c, rd, t << (s & 31)); break;
		case 0x06: uses(c, rs); uses(c, rt); uses(c, rd); setReg(c, rd, t >> (s & 31)); break;
		case 0x07: uses(c, rs); uses(c, rt); uses(c, rd); setReg(c, rd, (uint32_t) ((int32_t) t >> (s & 31))); break;
		case 0x08:
			uses(c, rs);
			uses(c, rd);
			commitLoad(c);
			c->next = s;
			c->branched = true;
			break;
		case 0x09:
			uses(c, rs);
			uses(c, rd);
			setReg(c, rd, next + 4);
			c->next = s;
			c->branched = true;
			break;
		case 0x0C: EXCEPTION(EXC_SYS); break;
		case 0x0D: EXCEPTION(EXC_BP); break;
		case 0x10:
			uses(c, rd);
			commitLoad(c);
			waitMuldiv(p);
			R[rd] = R[32];
			R[0] = 0;
			break;
		case 0x11: uses(c, rs); commitLoad(c); R[32] = s; break;
		case 0x12:
			uses(c, rd);
			commitLoad(c);
			waitMuldiv(p);
			R[rd] = R[33];
			R[0] = 0;
			break;
		case 0x13: uses(c, rs); commitLoad(c); R[33] = s; break;
		case 0x18: {
			uses(c, rs);
			uses(c, rt);
			int64_t r = (int64_t) (int32_t) s * (int32_t) t;
			uint32_t a = (s ^ (uint32_t) ((int32_t) s >> 31)) | 0x400;
			c->muldivDone = p->cycles + multCycles[__builtin_clz(a)];
			commitLoad(c);
			R[32] = (uint32_t) ((uint64_t) r >> 32);
			R[33] = (uint32_t) r;
			break;
		}
		case 0x19: {
			uses(c, rs);
			uses(c, rt);
			uint64_t r = (uint64_t) s * t;
			c->muldivDone = p->cycles + multCycles[__builtin_clz(s | 0x400)];
			commitLoad(c);
			R[32] = (uint32_t) (r >> 32);
			R[33] = (uint32_t) r;
			break;
		}
		case 0x1A:
			uses(c, rs);
			uses(c, rt);
			if (t == 0) {
				R[32] = s;
				R[33] = (int32_t) s < 0 ? 1 : 0xFFFFFFFF;
			} else if (s == 0x80000000 && t == 0xFFFFFFFF) {
				R[32] = 0;
				R[33] = 0x80000000;
			} else {
				R[32] = (uint32_t) ((int32_t) s % (int32_t) t);
				R[33] = (uint32_t) ((int32_t) s / (int32_t) t);
			}
			c->muldivDone = p->cycles + 37;
			commitLoad(c);
			break;
		case 0x1B:
			uses(c, rs);
			uses(c, rt);
			if (t == 0) {
				R[32] = s;
				R[33] = 0xFFFFFFFF;
			} else {
				R[32] = s % t;
				R[33] = s / t;
			}
			c->muldivDone = p->cycles + 37;
			commitLoad(c);
			break;
		case 0x20: {
			uses(c, rs); uses(c, rt); uses(c, rd);
			uint32_t r = s + t;
			if (~(s ^ t) & (s ^ r) & 0x80000000) {
				EXCEPTION(EXC_OV);
			} else {
				setReg(c, rd, r);
			}
			break;
		}
		case 0x21: uses(c, rs); uses(c, rt); uses(c, rd); setReg(c, rd, s + t); break;
		case 0x22: {
			uses(c, rs); uses(c, rt); uses(c, rd);
			uint32_t r = s - t;
			if ((s ^ t) & (s ^ r) & 0x80000000) {
				EXCEPTION(EXC_OV);
			} else {
				setReg(c, rd, r);
			}
			break;
		}
		case 0x23: uses(c, rs); uses(c, rt); uses(c, rd); setReg(c, rd, s - t); break;
		case 0x24: uses(c, rs); uses(c, rt); uses(c, rd); setReg(c, rd, s & t); break;
		case 0x25: uses(c, rs); uses(c, rt); uses(c, rd); setReg(c, rd, s | t); break;
		case 0x26: uses(c, rs); uses(c, rt); uses(c, rd); setReg(c, rd, s ^ t); break;
		case 0x27: uses(c, rs); uses(c, rt); uses(c, rd); setReg(c, rd, ~(s | t)); break;
		case 0x2A: uses(c, rs); uses(c, rt); uses(c, rd); setReg(c, rd, (int32_t) s < (int32_t) t); break;
		case 0x2B: uses(c, rs); uses(c, rt); uses(c, rd); setReg(c, rd, s < t); break;
		default: EXCEPTION(EXC_RI); break;
		}
		break;
	case 0x01: {
		// BLTZ/BGEZ, and their linking forms (rt 0x10/0x11)
		bool taken = ((int32_t) s < 0) ^ (rt & 1);
		bool link = (rt & 0x1E) == 0x10;
		uses(c, rs);
		if (link) uses(c, 31);
		commitLoad(c);
		if (link) {
			R[31] = next + 4;
		}
		if (taken) {
			c->next = next + (imm << 2);
		}
		c->branched = true;
		break;
	}
	case 0x02:
		commitLoad(c);
		c->next = (next & 0xF0000000) | (instr & 0x3FFFFFF) << 2;
		c->branched = true;
		break;
	case 0x03:
		uses(c, 31);
		setReg(c, 31, next + 4);
		c->next = (next & 0xF0000000) | (instr & 0x3FFFFFF) << 2;
		c->branched = true;
		break;
	case 0x04:
		uses(c, rs); uses(c, rt);
		commitLoad(c);
		if (s == t) c->next = next + (imm << 2);
		c->branched = true;
		break;
	case 0x05:
		uses(c, rs); uses(c, rt);
		commitLoad(c);
		if (s != t) c->next = next + (imm << 2);
		c->branched = true;
		break;
	case 0x06:
		uses(c, rs);
		commitLoad(c);
		if ((int32_t) s <= 0) c->next = next + (imm << 2);
		c->branched = true;
		break;
	case 0x07:
		uses(c, rs);
		commitLoad(c);
		if ((int32_t) s > 0) c->next = next + (imm << 2);
		c->branched = true;
		break;
	case 0x08: {
		uses(c, rs); uses(c, rt);
		uint32_t r = s + imm;
		if (~(s ^ imm) & (s ^ r) & 0x80000000) {
			EXCEPTION(EXC_OV);
		} else {
			setReg(c, rt, r);
		}
		break;
	}
	case 0x09: uses(c, rs); uses(c, rt); setReg(c, rt, s + imm); break;
	case 0x0A: uses(c, rs); uses(c, rt); setReg(c, rt, (int32_t) s < (int32_t) imm); break;
	case 0x0B: uses(c, rs); uses(c, rt); setReg(c, rt, s < imm); break;
	case 0x0C: uses(c, rs); uses(c, rt); setReg(c, rt, s & immu); break;
	case 0x0D: uses(c, rs); uses(c, rt); setReg(c, rt, s | immu); break;
	case 0x0E: uses(c, rs); uses(c, rt); setReg(c, rt, s ^ immu); break;
	case 0x0F: uses(c, rt); setReg(c, rt, immu << 16); break;
	case 0x10:
		if ((c->sr & 0x10000000) == 0 && (c->sr & 2)) {
			EXCEPTION(EXC_CPU);
			break;
		}
		switch (rs) {
		case 0x00:
			if (c->ldReg == rt) c->ldReg = 0;
			commitLoad(c);
			c->ldAbsorb = 0;
			load(c, rt, cop0Read(c, rd));
			break;
		case 0x04: commitLoad(c); cop0Write(c, rd, t); break;
		case 0x10:
			commitLoad(c);
			if ((instr & 63) == 0x10) {
				c->sr = (c->sr & ~0xFu) | ((c->sr >> 2) & 0xF);
			}
			break;
		default: commitLoad(c); break;
		}
		break;
	case 0x12:
		if (!(c->sr & 0x40000000)) {
			EXCEPTION(EXC_CPU);
			break;
		}
		if (instr & 0x02000000) {
			commitLoad(c);
			waitGte(p);
			c->gteDone = p->cycles + GTE_Instruction(instr);
			break;
		}
		switch (rs) {
		case 0x00: case 0x02:
			if (c->ldReg == rt) c->ldReg = 0;
			commitLoad(c);
			if (p->cycles < c->gteDone) {
				c->ldAbsorb = (uint8_t) (c->gteDone - p->cycles);
				p->cycles = c->gteDone;
			} else {
				c->ldAbsorb = 0;
			}
			load(c, rt, rs == 0 ? GTE_ReadDR(rd) : GTE_ReadCR(rd));
			break;
		case 0x04: commitLoad(c); waitGte(p); GTE_WriteDR(rd, t); break;
		case 0x06: commitLoad(c); waitGte(p); GTE_WriteCR(rd, t); break;
		default: commitLoad(c); break;
		}
		break;
	case 0x11:
	case 0x13:
		// COP1 and COP3 aren't fitted: nothing happens when they're marked usable.
		if (!(c->sr & 1u << (28 + (instr >> 26 & 3)))) {
			EXCEPTION(EXC_CPU);
		} else {
			commitLoad(c);
		}
		break;
	case 0x20: case 0x21: case 0x23: case 0x24: case 0x25: {
		uses(c, rs);
		uint32_t a = s + imm;
		int op = instr >> 26;
		if (((op == 0x21 || op == 0x25) && (a & 1)) || (op == 0x23 && (a & 3))) {
			c->badVaddr = a;
			EXCEPTION(EXC_ADEL);
			break;
		}
		if (c->ldReg == rt) c->ldReg = 0;
		commitLoad(c);
		uint32_t v;
		switch (op) {
		case 0x20: v = (uint32_t) (int32_t) (int8_t) read8(p, a); break;
		case 0x21: v = (uint32_t) (int32_t) (int16_t) read16(p, a); break;
		case 0x23: v = read32(p, a); break;
		case 0x24: v = read8(p, a); break;
		default: v = read16(p, a); break;
		}
		load(c, rt, v);
		break;
	}
	case 0x22: case 0x26: {
		uses(c, rs);
		uint32_t a = s + imm;
		uint32_t v = t;
		if (c->ldReg == rt) {
			v = c->ldValue;
			c->readFudge = 0;
		} else {
			commitLoad(c);
		}
		uint32_t w = read32(p, a & ~3u);
		int shift = (a & 3) * 8;
		if ((instr >> 26) == 0x22) {
			v = (v & (0x00FFFFFFu >> shift)) | (w << (24 - shift));
		} else {
			v = (v & ~(0xFFFFFFFFu >> shift)) | (w >> shift);
		}
		load(c, rt, v);
		break;
	}
	case 0x28: uses(c, rs); uses(c, rt); commitLoad(c); write8(p, s + imm, t); break;
	case 0x29: {
		uses(c, rs); uses(c, rt);
		uint32_t a = s + imm;
		if (a & 1) {
			c->badVaddr = a;
			EXCEPTION(EXC_ADES);
			break;
		}
		commitLoad(c);
		write16(p, a, t);
		break;
	}
	case 0x2A: case 0x2E: {
		uses(c, rs); uses(c, rt);
		uint32_t a = s + imm, base = a & ~3u, k = a & 3;
		commitLoad(c);
		if ((instr >> 26) == 0x2A) {
			// SWL: the word's bytes 0..k take t's top k+1 bytes
			for (uint32_t i = 0; i <= k; i++) write8(p, base + i, t >> (8 * (3 - k + i)));
		} else {
			// SWR: bytes k..3 take t's low 4-k bytes
			for (uint32_t i = k; i < 4; i++) write8(p, base + i, t >> (8 * (i - k)));
		}
		break;
	}
	case 0x2B: {
		uses(c, rs); uses(c, rt);
		uint32_t a = s + imm;
		if (a & 3) {
			c->badVaddr = a;
			EXCEPTION(EXC_ADES);
			break;
		}
		commitLoad(c);
		write32(p, a, t);
		break;
	}
	case 0x32: {
		uses(c, rs);
		if (!(c->sr & 0x40000000)) {
			EXCEPTION(EXC_CPU);
			break;
		}
		uint32_t a = s + imm;
		if (a & 3) {
			c->badVaddr = a;
			EXCEPTION(EXC_ADEL);
			break;
		}
		commitLoad(c);
		uint32_t v = read32(p, a);
		if (!scratchpad(a)) {
			p->cycles--;
			c->ldAbsorb--;
		}
		waitGte(p);
		GTE_WriteDR(rt, v);
		break;
	}
	case 0x3A: {
		uses(c, rs);
		if (!(c->sr & 0x40000000)) {
			EXCEPTION(EXC_CPU);
			break;
		}
		uint32_t a = s + imm;
		if (a & 3) {
			c->badVaddr = a;
			EXCEPTION(EXC_ADES);
			break;
		}
		commitLoad(c);
		waitGte(p);
		write32(p, a, GTE_ReadDR(rt));
		break;
	}
	case 0x30: case 0x31: case 0x33: case 0x38: case 0x39: case 0x3B:
		// LWC/SWC for coprocessors 0, 1 and 3: unusable faults, usable does nothing.
		if (!(c->sr & 1u << (28 + (instr >> 26 & 3)))) {
			EXCEPTION(EXC_CPU);
		} else {
			commitLoad(c);
		}
		break;
	default:
		EXCEPTION(EXC_RI);
		break;
	}
}

// One instruction: its fetch, the cycle it takes (or hides), a pending interrupt, then the instruction.
void cpuStep(struct Psx* p) {
	struct Cpu* c = &p->cpu;
	uint32_t pc = c->pc;
	if (__builtin_expect(p->pcRing != NULL, 0)) {
		p->pcRing[p->pcRingAt++ & 255] = pc;
		if (pc == p->pcStop) {
			for (int i = 0; i < 256; i++) fprintf(stderr, "%08x%c", p->pcRing[(p->pcRingAt + i) & 255], i % 16 == 15 ? '\n' : ' ');
			for (int i = 0x80 >> 2; i < 0x90 >> 2; i++) fprintf(stderr, "icache %03x tag %08x data %08x\n", i * 4, c->icache[i].tag, c->icache[i].data);
			fprintf(stderr, "biu %08x sr %08x\n", c->biu, c->sr);
			for (int i = 0; i < 32; i++) fprintf(stderr, "r%d=%08x%c", i, c->r[i], i % 8 == 7 ? '\n' : ' ');
			p->pcRing = NULL;
		}
	}
	if (__builtin_expect(p->exe != NULL, 0) && pc == 0x80030000) {
		psxLoadExe(p, p->exe, p->exeSize);
		return;
	}
	if (p->ttyOut) {
		if ((pc & 0x1FFFFFFF) == 0xB0 && c->r[9] == 0x3D) {
			tty(p, c->r[4]);
		} else if ((pc & 0x1FFFFFFF) == 0xA0 && c->r[9] == 0x3C) {
			tty(p, c->r[4]);
		}
	}
	uint32_t instr = 0;
	if (pc & 3) {
		c->badVaddr = pc;
		EXCEPTION(EXC_ADEL);
		return;
	}
	uint32_t physical = pc & 0x1FFFFFFF;
	if (physical >= 0x800000 && (physical < 0x1FC00000 || physical >= 0x1FC80000) &&
	    !(physical >= 0x1F801C00 && physical < 0x1F802000) && !(physical >= 0x1F801080 && physical < 0x1F801100)) {
		// Instruction fetches from anywhere else, the scratchpad included, are bus errors.
		EXCEPTION(EXC_IBE);
		return;
	}
	instr = fetch(p, pc);
	if (c->absorb[c->absorbWhich]) {
		c->absorb[c->absorbWhich]--;
	} else {
		p->cycles++;
	}
	c->inDelay = c->branched;
	c->branched = false;
	if (irqPending(p) && (instr >> 26) != 0x12) {
		EXCEPTION(EXC_INT);
		return;
	}
	cpuExecute(p, pc, instr);
}

// One instruction for the recompiler, which has done its fetch and cycle: the state as cpuStep leaves
// it, then the instruction. Nonzero when the block has to stop: the instruction went somewhere else,
// an interrupt is due, or the instruction cache changed.
int cpuJitExecute(struct Psx* p, uint32_t pc, uint32_t instr, int delay) {
	struct Cpu* c = &p->cpu;
	uint32_t changes = c->icacheChanges;
	c->inDelay = delay;
	c->branched = false;
	if (!delay) {
		c->next = pc + 4;
	}
	cpuExecute(p, pc, instr);
	return c->branched || delay || c->pc != pc + 4 || c->icacheChanges != changes || irqPending(p);
}

void cpuRun(struct Psx* p, int64_t until) {
	while (p->cycles < until) {
		if (p->cycles >= p->nextEvent) {
			psxEvents(p);
		}
		if (p->jit && !p->cpu.branched && !irqPending(p) && jitRun(p, until)) {
			continue;
		}
		cpuStep(p);
	}
}
