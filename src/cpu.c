/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "psx.h"

// The R3000A interpreter: one instruction a cycle, plus the instruction cache's fills and the bus's
// waits (timings per Mednafen). A load's value lands one instruction late: each instruction reads its
// operands, then the load in flight is written, then the instruction's own result.

enum { EXC_INT = 0, EXC_ADEL = 4, EXC_ADES = 5, EXC_IBE = 6, EXC_DBE = 7, EXC_SYS = 8, EXC_BP = 9, EXC_RI = 10, EXC_CPU = 11, EXC_OV = 12 };

#define R p->cpu.r

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

static inline void commitLoad(struct Cpu* c) {
	c->r[c->ldReg] = c->ldValue;
	c->ldReg = 0;
	c->r[0] = 0;
}

static inline void setReg(struct Cpu* c, uint32_t reg, uint32_t value) {
	commitLoad(c);
	c->r[reg] = value;
	c->r[0] = 0;
}

static inline void load(struct Cpu* c, uint32_t reg, uint32_t value) {
	commitLoad(c);
	c->ldReg = reg;
	c->ldValue = value;
}

static uint32_t fetch(struct Psx* p, uint32_t address) {
	struct Cpu* c = &p->cpu;
	uint32_t* tag = &c->icache[(address & 0xFFC) >> 2].tag;
	if (*tag == address) {
		return c->icache[(address & 0xFFC) >> 2].data;
	}
	uint32_t physical = address & 0x1FFFFFFF;
	const uint8_t* memory = physical < 0x800000 ? p->ram + (physical & (RAM_SIZE - 1)) : p->bios + (physical & (BIOS_SIZE - 1));
	if (physical >= 0x800000 && (physical < 0x1FC00000 || physical >= 0x1FC80000)) {
		return 0;
	}
	uint32_t instr;
	if (address >= 0xA0000000 || !(c->biu & 0x800)) {
		memcpy(&instr, memory, 4);
		p->cycles += 4;
		return instr;
	}
	// A miss fills the rest of the 4-word line from the missed word on.
	int line = (address & 0xFF0) >> 2, first = (address & 0xC) >> 2;
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

static void isolatedWrite(struct Psx* p, uint32_t address, uint32_t value) {
	struct Cpu* c = &p->cpu;
	if (!(c->biu & 0x800)) {
		return;
	}
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

static uint32_t read32(struct Psx* p, uint32_t address) {
	if (scratchpad(address)) {
		uint32_t v;
		memcpy(&v, p->scratch + (address & 0x3FF), 4);
		return v;
	}
	return busRead32(p, address);
}

static uint32_t read16(struct Psx* p, uint32_t address) {
	if (scratchpad(address)) {
		uint16_t v;
		memcpy(&v, p->scratch + (address & 0x3FF), 2);
		return v;
	}
	return busRead16(p, address);
}

static uint32_t read8(struct Psx* p, uint32_t address) {
	if (scratchpad(address)) {
		return p->scratch[address & 0x3FF];
	}
	return busRead8(p, address);
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
	for (int i = 0; i < 1024; i++) {
		c->icache[i].tag = 2;
	}
}

static inline bool irqPending(struct Psx* p) {
	struct Cpu* c = &p->cpu;
	uint32_t cause = (c->cause & ~0x400u) | ((p->iStat & p->iMask) ? 0x400 : 0);
	c->cause = cause;
	return (c->sr & 1) && (c->sr & cause & 0xFF00);
}

static void waitMuldiv(struct Psx* p) {
	if (p->cycles < p->cpu.muldivDone) {
		p->cycles = p->cpu.muldivDone;
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
	case 6 + 32: return c->tar;
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
		if (getenv("PSX_TTY")) {
			fprintf(stderr, "tty: %s\n", p->tty);
		}
		p->ttyLength = 0;
	} else if (ch != '\r') {
		p->tty[p->ttyLength++] = ch;
	}
}

void cpuRun(struct Psx* p, int64_t until) {
	struct Cpu* c = &p->cpu;
	while (p->cycles < until) {
		if (p->cycles >= p->nextEvent) {
			psxEvents(p);
		}
		uint32_t pc = c->pc;
		if ((pc & 0x1FFFFFFF) == 0xB0 && c->r[9] == 0x3D) {
			tty(p, c->r[4]);
		} else if ((pc & 0x1FFFFFFF) == 0xA0 && c->r[9] == 0x3C) {
			tty(p, c->r[4]);
		}
		if (pc & 3) {
			c->badVaddr = pc;
			commitLoad(c);
			c->pc = exception(p, EXC_ADEL, pc, 0);
			c->next = c->pc + 4;
			continue;
		}
		uint32_t instr = fetch(p, pc);
		p->cycles++;
		c->inDelay = c->branched;
		c->branched = false;
		if (irqPending(p) && (instr >> 26) != 0x12) {
			commitLoad(c);
			c->pc = exception(p, EXC_INT, pc, instr);
			c->next = c->pc + 4;
			continue;
		}
		uint32_t next = c->next;
		c->pc = next;
		c->next = next + 4;
		uint32_t rs = instr >> 21 & 31, rt = instr >> 16 & 31, rd = instr >> 11 & 31;
		uint32_t imm = (uint32_t) (int32_t) (int16_t) instr, immu = instr & 0xFFFF;
		uint32_t s = R[rs], t = R[rt];
		switch (instr >> 26) {
		case 0x00:
			switch (instr & 63) {
			case 0x00: setReg(c, rd, t << (instr >> 6 & 31)); break;
			case 0x02: setReg(c, rd, t >> (instr >> 6 & 31)); break;
			case 0x03: setReg(c, rd, (uint32_t) ((int32_t) t >> (instr >> 6 & 31))); break;
			case 0x04: setReg(c, rd, t << (s & 31)); break;
			case 0x06: setReg(c, rd, t >> (s & 31)); break;
			case 0x07: setReg(c, rd, (uint32_t) ((int32_t) t >> (s & 31))); break;
			case 0x08:
				commitLoad(c);
				c->next = s;
				c->branched = true;
				break;
			case 0x09:
				setReg(c, rd, next + 4);
				c->next = s;
				c->branched = true;
				break;
			case 0x0C:
				commitLoad(c);
				c->pc = exception(p, EXC_SYS, pc, instr);
				c->next = c->pc + 4;
				break;
			case 0x0D:
				commitLoad(c);
				c->pc = exception(p, EXC_BP, pc, instr);
				c->next = c->pc + 4;
				break;
			case 0x10: waitMuldiv(p); setReg(c, rd, R[32]); break;
			case 0x11: commitLoad(c); R[32] = s; break;
			case 0x12: waitMuldiv(p); setReg(c, rd, R[33]); break;
			case 0x13: commitLoad(c); R[33] = s; break;
			case 0x18: {
				int64_t r = (int64_t) (int32_t) s * (int32_t) t;
				commitLoad(c);
				R[32] = (uint32_t) ((uint64_t) r >> 32);
				R[33] = (uint32_t) r;
				uint32_t a = (int32_t) s < 0 ? ~s : s;
				c->muldivDone = p->cycles + (a < 0x800 ? 6 : a < 0x100000 ? 9 : 13);
				break;
			}
			case 0x19: {
				uint64_t r = (uint64_t) s * t;
				commitLoad(c);
				R[32] = (uint32_t) (r >> 32);
				R[33] = (uint32_t) r;
				c->muldivDone = p->cycles + (s < 0x800 ? 6 : s < 0x100000 ? 9 : 13);
				break;
			}
			case 0x1A:
				commitLoad(c);
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
				c->muldivDone = p->cycles + 36;
				break;
			case 0x1B:
				commitLoad(c);
				if (t == 0) {
					R[32] = s;
					R[33] = 0xFFFFFFFF;
				} else {
					R[32] = s % t;
					R[33] = s / t;
				}
				c->muldivDone = p->cycles + 36;
				break;
			case 0x20: {
				uint32_t r = s + t;
				if (~(s ^ t) & (s ^ r) & 0x80000000) {
					commitLoad(c);
					c->pc = exception(p, EXC_OV, pc, instr);
					c->next = c->pc + 4;
				} else {
					setReg(c, rd, r);
				}
				break;
			}
			case 0x21: setReg(c, rd, s + t); break;
			case 0x22: {
				uint32_t r = s - t;
				if ((s ^ t) & (s ^ r) & 0x80000000) {
					commitLoad(c);
					c->pc = exception(p, EXC_OV, pc, instr);
					c->next = c->pc + 4;
				} else {
					setReg(c, rd, r);
				}
				break;
			}
			case 0x23: setReg(c, rd, s - t); break;
			case 0x24: setReg(c, rd, s & t); break;
			case 0x25: setReg(c, rd, s | t); break;
			case 0x26: setReg(c, rd, s ^ t); break;
			case 0x27: setReg(c, rd, ~(s | t)); break;
			case 0x2A: setReg(c, rd, (int32_t) s < (int32_t) t); break;
			case 0x2B: setReg(c, rd, s < t); break;
			default:
				commitLoad(c);
				c->pc = exception(p, EXC_RI, pc, instr);
				c->next = c->pc + 4;
				break;
			}
			break;
		case 0x01: {
			// BLTZ/BGEZ, and with bit 20 set the "AL" forms; only rt 0x10/0x11 link (others decode by bits 16 and 20)
			bool taken = ((int32_t) s < 0) ^ (rt & 1);
			commitLoad(c);
			if ((rt & 0x1E) == 0x10) {
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
			setReg(c, 31, next + 4);
			c->next = (next & 0xF0000000) | (instr & 0x3FFFFFF) << 2;
			c->branched = true;
			break;
		case 0x04:
			commitLoad(c);
			if (s == t) c->next = next + (imm << 2);
			c->branched = true;
			break;
		case 0x05:
			commitLoad(c);
			if (s != t) c->next = next + (imm << 2);
			c->branched = true;
			break;
		case 0x06:
			commitLoad(c);
			if ((int32_t) s <= 0) c->next = next + (imm << 2);
			c->branched = true;
			break;
		case 0x07:
			commitLoad(c);
			if ((int32_t) s > 0) c->next = next + (imm << 2);
			c->branched = true;
			break;
		case 0x08: {
			uint32_t r = s + imm;
			if (~(s ^ imm) & (s ^ r) & 0x80000000) {
				commitLoad(c);
				c->pc = exception(p, EXC_OV, pc, instr);
				c->next = c->pc + 4;
			} else {
				setReg(c, rt, r);
			}
			break;
		}
		case 0x09: setReg(c, rt, s + imm); break;
		case 0x0A: setReg(c, rt, (int32_t) s < (int32_t) imm); break;
		case 0x0B: setReg(c, rt, s < imm); break;
		case 0x0C: setReg(c, rt, s & immu); break;
		case 0x0D: setReg(c, rt, s | immu); break;
		case 0x0E: setReg(c, rt, s ^ immu); break;
		case 0x0F: setReg(c, rt, immu << 16); break;
		case 0x10:
			if ((c->sr & 0x10000000) == 0 && (c->sr & 2)) {
				commitLoad(c);
				c->pc = exception(p, EXC_CPU, pc, instr);
				c->next = c->pc + 4;
				break;
			}
			switch (rs) {
			case 0x00: load(c, rt, cop0Read(c, rd)); break;
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
				commitLoad(c);
				c->pc = exception(p, EXC_CPU, pc, instr);
				c->next = c->pc + 4;
				break;
			}
			if (instr & 0x02000000) {
				commitLoad(c);
				waitGte(p);
				c->gteDone = p->cycles + GTE_Instruction(instr);
				break;
			}
			switch (rs) {
			case 0x00: waitGte(p); load(c, rt, GTE_ReadDR(rd)); break;
			case 0x02: waitGte(p); load(c, rt, GTE_ReadCR(rd)); break;
			case 0x04: commitLoad(c); waitGte(p); GTE_WriteDR(rd, t); break;
			case 0x06: commitLoad(c); waitGte(p); GTE_WriteCR(rd, t); break;
			default: commitLoad(c); break;
			}
			break;
		case 0x11:
		case 0x13:
			commitLoad(c);
			c->pc = exception(p, EXC_CPU, pc, instr);
			c->next = c->pc + 4;
			break;
		case 0x20: {
			uint32_t a = s + imm;
			load(c, rt, (uint32_t) (int32_t) (int8_t) read8(p, a));
			break;
		}
		case 0x21: {
			uint32_t a = s + imm;
			if (a & 1) {
				c->badVaddr = a;
				commitLoad(c);
				c->pc = exception(p, EXC_ADEL, pc, instr);
				c->next = c->pc + 4;
				break;
			}
			load(c, rt, (uint32_t) (int32_t) (int16_t) read16(p, a));
			break;
		}
		case 0x22: case 0x26: {
			uint32_t a = s + imm;
			uint32_t v = c->ldReg == rt ? c->ldValue : t;
			if (c->ldReg != rt) commitLoad(c);
			uint32_t w = read32(p, a & ~3u);
			int shift = (a & 3) * 8;
			if ((instr >> 26) == 0x22) {
				v = (v & (0x00FFFFFFu >> shift)) | (w << (24 - shift));
			} else {
				v = (v & ~(0xFFFFFFFFu >> shift)) | (w >> shift);
			}
			c->ldReg = rt;
			c->ldValue = v;
			break;
		}
		case 0x23: {
			uint32_t a = s + imm;
			if (a & 3) {
				c->badVaddr = a;
				commitLoad(c);
				c->pc = exception(p, EXC_ADEL, pc, instr);
				c->next = c->pc + 4;
				break;
			}
			load(c, rt, read32(p, a));
			break;
		}
		case 0x24: {
			uint32_t a = s + imm;
			load(c, rt, read8(p, a));
			break;
		}
		case 0x25: {
			uint32_t a = s + imm;
			if (a & 1) {
				c->badVaddr = a;
				commitLoad(c);
				c->pc = exception(p, EXC_ADEL, pc, instr);
				c->next = c->pc + 4;
				break;
			}
			load(c, rt, read16(p, a));
			break;
		}
		case 0x28: commitLoad(c); write8(p, s + imm, t); break;
		case 0x29: {
			uint32_t a = s + imm;
			commitLoad(c);
			if (a & 1) {
				c->badVaddr = a;
				c->pc = exception(p, EXC_ADES, pc, instr);
				c->next = c->pc + 4;
				break;
			}
			write16(p, a, t);
			break;
		}
		case 0x2A: case 0x2E: {
			uint32_t a = s + imm;
			commitLoad(c);
			uint32_t w = read32(p, a & ~3u);
			int shift = (a & 3) * 8;
			if ((instr >> 26) == 0x2A) {
				w = (w & ~(0xFFFFFFFFu >> (24 - shift))) | (t >> (24 - shift));
				if (shift == 24) w = t;
			} else {
				w = (w & ~(0xFFFFFFFFu << shift)) | (t << shift);
			}
			write32(p, a & ~3u, w);
			break;
		}
		case 0x2B: {
			uint32_t a = s + imm;
			commitLoad(c);
			if (a & 3) {
				c->badVaddr = a;
				c->pc = exception(p, EXC_ADES, pc, instr);
				c->next = c->pc + 4;
				break;
			}
			write32(p, a, t);
			break;
		}
		case 0x32: {
			uint32_t a = s + imm;
			commitLoad(c);
			if (a & 3) {
				c->badVaddr = a;
				c->pc = exception(p, EXC_ADEL, pc, instr);
				c->next = c->pc + 4;
				break;
			}
			uint32_t v = read32(p, a);
			waitGte(p);
			GTE_WriteDR(rt, v);
			break;
		}
		case 0x3A: {
			uint32_t a = s + imm;
			commitLoad(c);
			if (a & 3) {
				c->badVaddr = a;
				c->pc = exception(p, EXC_ADES, pc, instr);
				c->next = c->pc + 4;
				break;
			}
			waitGte(p);
			write32(p, a, GTE_ReadDR(rt));
			break;
		}
		case 0x30: case 0x31: case 0x33: case 0x38: case 0x39: case 0x3B:
			commitLoad(c);
			c->pc = exception(p, EXC_CPU, pc, instr);
			c->next = c->pc + 4;
			break;
		default:
			commitLoad(c);
			c->pc = exception(p, EXC_RI, pc, instr);
			c->next = c->pc + 4;
			break;
		}
	}
}
