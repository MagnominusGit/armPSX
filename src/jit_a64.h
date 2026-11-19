/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
// The AArch64 backend of jit.c, instruction for instruction what jit_x64.h emits.
//
// Registers: x19 = p, x20 = the frame's limit, x21 = p->cycles, x22 = the limit (the next event or
// the frame's limit, whichever is first), x23 = RAM, x24 = the block table, x25 = the scratchpad,
// x26 = cpu.absorb. w0-w13 are scratch; calls into C keep x19-x28.

#include "a64/emit.h"

enum { X0, X1, X2, X3, X4, X5, X6, X7, X8, X9, X10, X11, X12, X13, X16 = 16, X19 = 19, X20, X21, X22, X23, X24, X25, X26, X29 = 29, X30 };

#define OFF(f) ((unsigned) offsetof(struct Psx, f))
#define GPR_OFF(i) (OFF(cpu.r) + 4 * (unsigned) (i))
#define SCALED 0x1000

static void movI(struct Emitter* e, int reg, uint32_t v) {
	aMovImm(e, reg, v);
}

static void callC(struct Emitter* e, const void* f) {
	aMovImm64(e, X16, (uint64_t) (uintptr_t) f);
	aBlr(e, X16);
}

static void aCmp(struct Emitter* e, int rn, int rm) {
	aDp(e, A_SUBS, ZR, rn, rm, S_LSL, 0);
}

static void aCmpX(struct Emitter* e, int rn, int rm) {
	aDp(e, A_SUBS | W64, ZR, rn, rm, S_LSL, 0);
}

static void aCmpImm(struct Emitter* e, int rn, uint32_t imm) {
	aDpImm(e, A_SUBS_IMM, ZR, rn, imm);
}

static uint8_t* aCbzX(struct Emitter* e, int rt, const void* target) {
	uint8_t* site = emitSite(e, 0xB4000000 | rt);
	if (target && site + 4 <= e->end) {
		patchBranch(site, target);
	}
	return site;
}

// rd = rn op value, through tmp when the constant has no encoding.
static void logicImm(struct Emitter* e, uint32_t immOp, uint32_t regOp, int rd, int rn, uint32_t value, int tmp) {
	if (!aLogicImm(e, immOp, rd, rn, value)) {
		movI(e, tmp, value);
		aDp(e, regOp, rd, rn, tmp, S_LSL, 0);
	}
}

static void storeByteI(struct Emitter* e, unsigned off, uint32_t v) {
	if (v) {
		movI(e, X0, v);
		aStrb(e, X0, X19, off);
	} else {
		aStrb(e, ZR, X19, off);
	}
}

static void storeWordI(struct Emitter* e, unsigned off, uint32_t v) {
	if (v) {
		movI(e, X0, v);
		aStr(e, X0, X19, off);
	} else {
		aStr(e, ZR, X19, off);
	}
}

// x22 = the next event, or the frame's limit when that comes first.
static void loadLimit(struct Emitter* e) {
	aLdrX(e, X22, X19, OFF(nextEvent));
	aCmpX(e, X22, X20);
	aCselX(e, X22, X20, X22, C_GT);
}

// commitLoad for the load the instruction before left in flight (see jit_x64.h); w0 and w1 only.
static void commit(struct Emitter* e, int reg) {
	if (reg == NO_LOAD) {
		movI(e, X0, NO_LOAD);
		aStrb(e, X0, X19, OFF(cpu.readFudge));
		aLdrb(e, X0, X19, OFF(cpu.absorbWhich));
		aLogicImm(e, A_ORR_IMM, X0, X0, NO_LOAD & 0x1F);
		aStrb(e, X0, X19, OFF(cpu.absorbWhich));
		return;
	}
	if (reg == DYNAMIC) {
		aLdr(e, X0, X19, OFF(cpu.ldReg));
		aLdr(e, X1, X19, OFF(cpu.ldValue));
		aLdst(e, L_STR, X1, X19, X0, L_UXTW | SCALED);
		aLdrb(e, X1, X19, OFF(cpu.ldAbsorb));
		aLdst(e, L_STRB, X1, X26, X0, L_UXTW);
		aStrb(e, X0, X19, OFF(cpu.readFudge));
		aLogicImm(e, A_AND_IMM, X0, X0, 0x1F);
		aLdrb(e, X1, X19, OFF(cpu.absorbWhich));
		aDp(e, A_ORR, X1, X1, X0, S_LSL, 0);
		aStrb(e, X1, X19, OFF(cpu.absorbWhich));
	} else {
		aLdr(e, X0, X19, OFF(cpu.ldValue));
		aStr(e, X0, X19, GPR_OFF(reg));
		aLdrb(e, X0, X19, OFF(cpu.ldAbsorb));
		aStrb(e, X0, X26, reg);
		storeByteI(e, OFF(cpu.readFudge), reg);
		if (reg & 0x1F) {
			aLdrb(e, X0, X19, OFF(cpu.absorbWhich));
			logicImm(e, A_ORR_IMM, A_ORR, X0, X0, reg & 0x1F, X1);
			aStrb(e, X0, X19, OFF(cpu.absorbWhich));
		}
	}
	storeWordI(e, OFF(cpu.ldReg), NO_LOAD);
	aStr(e, ZR, X19, GPR_OFF(0));
}

static void uses(struct Emitter* e, int reg) {
	if (reg) {
		aStrb(e, ZR, X26, reg);
	}
}

static void loadGpr(struct Emitter* e, int host, int reg) {
	if (reg) {
		aLdr(e, host, X19, GPR_OFF(reg));
	} else {
		aMov(e, host, ZR);
	}
}

struct Compile {
	struct Emitter e;
	uint8_t* leave;
	uint8_t* stubSites[BLOCK_MAX];
	uint8_t* slows[BLOCK_MAX * 2];
	int slowCount;
};

// The instruction through cpuJitExecute; the block leaves if it has to.
static void helper(struct Compile* k, uint32_t pc, uint32_t instr, bool delay) {
	struct Emitter* e = &k->e;
	aStrX(e, X21, X19, OFF(cycles));
	aMovX(e, X0, X19);
	movI(e, X1, pc);
	movI(e, X2, instr);
	movI(e, X3, delay);
	callC(e, (const void*) cpuJitExecute);
	aLdrX(e, X21, X19, OFF(cycles));
	uint8_t* stay = aCbz(e, X0, NULL);
	aB(e, k->leave);
	patchHere(e, stay);
	loadLimit(e);
}

static void slowAt(struct Compile* k, uint8_t* site) {
	k->slows[k->slowCount++] = site;
}

// w9 = the RAM offset of the address in w10, or the slow path when it isn't RAM; the scratchpad is
// tried first and taken at *scratchSite. w2-w4 are scratch.
static void ramOffset(struct Compile* k, uint8_t** scratchSite) {
	struct Emitter* e = &k->e;
	aLogicImm(e, A_AND_IMM, X2, X10, 0x7FFFFC00);
	movI(e, X3, 0x1F800000);
	aCmp(e, X2, X3);
	uint8_t* notScratch = aBcond(e, C_NE, NULL);
	movI(e, X3, 0xA0000000);
	aCmp(e, X10, X3);
	*scratchSite = aBcond(e, C_LO, NULL);
	patchHere(e, notScratch);
	aLogicImm(e, A_AND_IMM, X9, X10, 0x1FFFFFFF);
	aCmpImm(e, X9, 0x800000);
	slowAt(k, aBcond(e, C_HS, NULL));
	aLsrImm(e, X3, X10, 29);
	movI(e, X4, 0x31);
	aDp(e, A_LSRV, X4, X4, X3, S_LSL, 0);
	slowAt(k, aTbz(e, X4, 0, NULL));
	aLogicImm(e, A_AND_IMM, X9, X9, RAM_SIZE - 1);
}

static void address(struct Compile* k, uint32_t instr, int width) {
	struct Emitter* e = &k->e;
	int rs = instr >> 21 & 31;
	int32_t imm = (int16_t) instr;
	loadGpr(e, X10, rs);
	if (imm) aAddImm(e, X10, X10, imm, X3);
	if (width > 1) {
		aLogicImm(e, A_ANDS_IMM, ZR, X10, width - 1);
		slowAt(k, aBcond(e, C_NE, NULL));
	}
}

// The load in flight's register after "if (c->ldReg == rt) c->ldReg = 0".
static int cancelSame(struct Emitter* e, int pending, int rt) {
	if (pending == DYNAMIC) {
		aLdr(e, X0, X19, OFF(cpu.ldReg));
		aCmpImm(e, X0, rt);
		uint8_t* other = aBcond(e, C_NE, NULL);
		aStr(e, ZR, X19, OFF(cpu.ldReg));
		patchHere(e, other);
		return DYNAMIC;
	}
	return pending == rt ? 0 : pending;
}

static void emitLoad(struct Compile* k, uint32_t pc, uint32_t instr, int pending) {
	struct Emitter* e = &k->e;
	int op = instr >> 26, rs = instr >> 21 & 31, rt = instr >> 16 & 31;
	int width = op == 0x23 ? 4 : (op == 0x21 || op == 0x25) ? 2 : 1;
	static const uint32_t loads[6] = { L_LDRSB, L_LDRSH, 0, L_LDR, L_LDRB, L_LDRH };
	address(k, instr, width);
	uint8_t* scratchSite;
	ramOffset(k, &scratchSite);
	uint8_t* done = NULL;
	for (int pass = 0; pass < 2; pass++) {
		if (pass == 1) {
			patchHere(e, scratchSite);
			aLogicImm(e, A_AND_IMM, X9, X10, 0x3FF);
		}
		aLdst(e, loads[op - 0x20], X5, pass == 0 ? X23 : X25, X9, L_UXTW);
		uses(e, rs);
		int reg = cancelSame(e, pending, rt);
		commit(e, reg);
		aLdrb(e, X0, X19, OFF(cpu.absorbWhich));
		aLdst(e, L_STRB, ZR, X26, X0, L_UXTW);
		aStrb(e, ZR, X19, OFF(cpu.absorbWhich));
		if (pass == 0) {
			if (reg == DYNAMIC) {
				aLdrb(e, X0, X19, OFF(cpu.readFudge));
				aLsrImm(e, X0, X0, 4);
				aLogicImm(e, A_AND_IMM, X0, X0, 2);
				aDp(e, A_ADD | W64, X21, X21, X0, S_LSL, 0);
				aDpImm(e, A_ADD_IMM | W64, X21, X21, 5);
			} else {
				aDpImm(e, A_ADD_IMM | W64, X21, X21, 5 + ((reg >> 4) & 2));
			}
			storeByteI(e, OFF(cpu.ldAbsorb), 5);
		} else {
			aStrb(e, ZR, X19, OFF(cpu.ldAbsorb));
		}
		aStr(e, X5, X19, OFF(cpu.ldValue));
		storeWordI(e, OFF(cpu.ldReg), rt);
		if (pass == 0) done = aB(e, NULL);
	}
	patchHere(e, done);
}

static void emitStore(struct Compile* k, uint32_t pc, uint32_t instr, int pending) {
	struct Emitter* e = &k->e;
	int op = instr >> 26, rs = instr >> 21 & 31, rt = instr >> 16 & 31;
	int width = op == 0x2B ? 4 : op == 0x29 ? 2 : 1;
	address(k, instr, width);
	aLdr(e, X0, X19, OFF(cpu.sr));
	slowAt(k, aTbnz(e, X0, 16, NULL));
	uint8_t* scratchSite;
	ramOffset(k, &scratchSite);
	aDp(e, A_ADD | W64, X11, X23, X9, S_LSL, 0);
	uint8_t* write = aB(e, NULL);
	patchHere(e, scratchSite);
	aLogicImm(e, A_AND_IMM, X9, X10, 0x3FF);
	aDp(e, A_ADD | W64, X11, X25, X9, S_LSL, 0);
	patchHere(e, write);
	loadGpr(e, X5, rt);
	uses(e, rs);
	uses(e, rt);
	commit(e, pending);
	if (width == 4) aStr(e, X5, X11, 0);
	else if (width == 2) aStrh(e, X5, X11, 0);
	else aStrb(e, X5, X11, 0);
}

static bool emitAlu(struct Compile* k, uint32_t pc, uint32_t instr, int pending) {
	struct Emitter* e = &k->e;
	int op = instr >> 26, rs = instr >> 21 & 31, rt = instr >> 16 & 31, rd = instr >> 11 & 31;
	int sa = instr >> 6 & 31, fn = instr & 63;
	uint32_t imm = (uint32_t) (int32_t) (int16_t) instr, immu = instr & 0xFFFF;
	int dst;
	if (op == 0) {
		dst = rd;
		switch (fn) {
		case 0x00: case 0x02: case 0x03:
			loadGpr(e, X7, rt);
			if (!sa) aMov(e, X5, X7);
			else if (fn == 0) aLslImm(e, X5, X7, sa);
			else if (fn == 2) aLsrImm(e, X5, X7, sa);
			else aAsrImm(e, X5, X7, sa);
			uses(e, rt); uses(e, rd);
			break;
		case 0x04: case 0x06: case 0x07:
			loadGpr(e, X7, rt);
			loadGpr(e, X6, rs);
			aDp(e, fn == 4 ? A_LSLV : fn == 6 ? A_LSRV : A_ASRV, X5, X7, X6, S_LSL, 0);
			uses(e, rs); uses(e, rt); uses(e, rd);
			break;
		case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: {
			loadGpr(e, X6, rs);
			loadGpr(e, X7, rt);
			static const uint32_t alu[8] = { A_ADDS, A_ADD, A_SUBS, A_SUB, A_AND, A_ORR, A_EOR, A_ORR };
			aDp(e, alu[fn & 7], X5, X6, X7, S_LSL, 0);
			if (fn == 0x20 || fn == 0x22) slowAt(k, aBcond(e, C_VS, NULL));
			if (fn == 0x27) aMvn(e, X5, X5);
			uses(e, rs); uses(e, rt); uses(e, rd);
			break;
		}
		case 0x2A: case 0x2B:
			loadGpr(e, X6, rs);
			loadGpr(e, X7, rt);
			aCmp(e, X6, X7);
			aCset(e, X5, fn == 0x2A ? C_LT : C_LO);
			uses(e, rs); uses(e, rt); uses(e, rd);
			break;
		case 0x11: case 0x13:
			loadGpr(e, X5, rs);
			uses(e, rs);
			commit(e, pending);
			aStr(e, X5, X19, GPR_OFF(fn == 0x11 ? 32 : 33));
			return true;
		default:
			return false;
		}
	} else {
		dst = rt;
		switch (op) {
		case 0x08:
			loadGpr(e, X6, rs);
			movI(e, X7, imm);
			aDp(e, A_ADDS, X5, X6, X7, S_LSL, 0);
			slowAt(k, aBcond(e, C_VS, NULL));
			break;
		case 0x09:
			loadGpr(e, X6, rs);
			aAddImm(e, X5, X6, (int32_t) imm, X7);
			break;
		case 0x0A: case 0x0B:
			loadGpr(e, X6, rs);
			movI(e, X7, imm);
			aCmp(e, X6, X7);
			aCset(e, X5, op == 0x0A ? C_LT : C_LO);
			break;
		case 0x0C: case 0x0D: case 0x0E:
			loadGpr(e, X6, rs);
			if (!immu) {
				if (op == 0x0C) aMov(e, X5, ZR);
				else aMov(e, X5, X6);
			} else {
				static const uint32_t imms[3] = { A_AND_IMM, A_ORR_IMM, A_EOR_IMM }, regs[3] = { A_AND, A_ORR, A_EOR };
				logicImm(e, imms[op - 0x0C], regs[op - 0x0C], X5, X6, immu, X7);
			}
			break;
		case 0x0F:
			movI(e, X5, immu << 16);
			uses(e, rt);
			goto result;
		default:
			return false;
		}
		uses(e, rs); uses(e, rt);
	}
result:
	commit(e, pending);
	if (dst) aStr(e, X5, X19, GPR_OFF(dst));
	return true;
}

static void emitBranch(struct Compile* k, uint32_t pc, uint32_t instr, int pending) {
	struct Emitter* e = &k->e;
	int op = instr >> 26, rs = instr >> 21 & 31, rt = instr >> 16 & 31, rd = instr >> 11 & 31;
	uint32_t after = pc + 8, target = pc + 4 + ((uint32_t) (int32_t) (int16_t) instr << 2);
	uint32_t jump = ((pc + 4) & 0xF0000000) | (instr & 0x3FFFFFF) << 2;
	if (op == 0x02 || op == 0x03) {
		if (op == 0x03) uses(e, 31);
		commit(e, pending);
		if (op == 0x03) storeWordI(e, GPR_OFF(31), after);
		storeWordI(e, OFF(cpu.next), jump);
		return;
	}
	if (op == 0x00) {
		loadGpr(e, X5, rs);
		uses(e, rs);
		uses(e, rd);
		commit(e, pending);
		if ((instr & 63) == 0x09 && rd) storeWordI(e, GPR_OFF(rd), after);
		aStr(e, X5, X19, OFF(cpu.next));
		return;
	}
	loadGpr(e, X6, rs);
	if (op == 0x04 || op == 0x05) loadGpr(e, X7, rt);
	uses(e, rs);
	bool link = op == 0x01 && (rt & 0x1E) == 0x10;
	if (op == 0x04 || op == 0x05) uses(e, rt);
	if (link) uses(e, 31);
	commit(e, pending);
	if (link) storeWordI(e, GPR_OFF(31), after);
	int cc;
	if (op == 0x04 || op == 0x05) {
		aCmp(e, X6, X7);
		cc = op == 0x04 ? C_EQ : C_NE;
	} else {
		aCmp(e, X6, ZR);
		cc = op == 0x06 ? C_LE : op == 0x07 ? C_GT : (rt & 1) ? C_PL : C_MI;
	}
	movI(e, X0, after);
	movI(e, X1, target);
	aCsel(e, X0, X1, X0, cc);
	aStr(e, X0, X19, OFF(cpu.next));
}

static bool emitGte(struct Compile* k, uint32_t pc, uint32_t instr, int pending) {
	struct Emitter* e = &k->e;
	int rs = instr >> 21 & 31, rt = instr >> 16 & 31, rd = instr >> 11 & 31;
	if (!(instr & 0x02000000) && rs != 0 && rs != 2 && rs != 4 && rs != 6) return false;
	aLdr(e, X0, X19, OFF(cpu.sr));
	slowAt(k, aTbz(e, X0, 30, NULL));
	if (instr & 0x02000000) {
		commit(e, pending);
		aLdrX(e, X0, X19, OFF(cpu.gteDone));
		aCmpX(e, X21, X0);
		aCselX(e, X21, X0, X21, C_LT);
		movI(e, X0, instr);
		callC(e, (const void*) GTE_Instruction);
		aSxtw(e, X0, X0);
		aDp(e, A_ADD | W64, X0, X0, X21, S_LSL, 0);
		aStrX(e, X0, X19, OFF(cpu.gteDone));
		return true;
	}
	if (rs == 0 || rs == 2) {
		commit(e, cancelSame(e, pending, rt));
		aLdrX(e, X0, X19, OFF(cpu.gteDone));
		aDp(e, A_SUBS | W64, X0, X0, X21, S_LSL, 0);
		aCselX(e, X0, X0, ZR, C_GT);
		aStrb(e, X0, X19, OFF(cpu.ldAbsorb));
		aDp(e, A_ADD | W64, X21, X21, X0, S_LSL, 0);
		movI(e, X0, rd);
		callC(e, (const void*) (rs == 0 ? GTE_ReadDR : GTE_ReadCR));
		aStr(e, X0, X19, OFF(cpu.ldValue));
		storeWordI(e, OFF(cpu.ldReg), rt);
		return true;
	}
	loadGpr(e, X5, rt);
	commit(e, pending);
	aLdrX(e, X0, X19, OFF(cpu.gteDone));
	aCmpX(e, X21, X0);
	aCselX(e, X21, X0, X21, C_LT);
	movI(e, X0, rd);
	aMov(e, X1, X5);
	callC(e, (const void*) (rs == 4 ? GTE_WriteDR : GTE_WriteCR));
	return true;
}

static uint8_t* emitBlock(struct Psx* p, struct Jit* j, struct Block* b, uint8_t* start, uint8_t* end) {
	uint32_t pc = b->pc;
	struct Compile k = { 0 };
	struct Emitter* e = &k.e;
	e->start = e->p = start;
	e->end = end;
	uint8_t* entry = e->p;
	k.leave = j->leave;
	int pending = DYNAMIC;
	for (int i = 0; i < b->count; i++) {
		uint32_t a = pc + 4 * i, instr = b->words[i];
		bool delay = b->delay && i == b->count - 1;
		b->at[i] = (uint16_t) (e->p - entry);
		// The limit, then the cycle the fetch takes unless a load's wait hides it
		aCmpX(e, X21, X22);
		k.stubSites[i] = aBcond(e, C_GE, NULL);
		aLdrb(e, X0, X19, OFF(cpu.absorbWhich));
		aLdst(e, L_LDRB, X1, X26, X0, L_UXTW);
		aCmpImm(e, X1, 0);
		aCset(e, X2, C_EQ);
		aDp(e, A_ADD | W64, X21, X21, X2, S_LSL, 0);
		aDpImm(e, A_SUB_IMM, X1, X1, 1);
		aDp(e, A_ADD, X1, X1, X2, S_LSL, 0);
		aLdst(e, L_STRB, X1, X26, X0, L_UXTW);
		int op = instr >> 26;
		k.slowCount = 0;
		bool inline_ = false;
		if (op >= 0x20 && op <= 0x25 && op != 0x22) {
			emitLoad(&k, a, instr, pending);
			inline_ = true;
		} else if (op == 0x28 || op == 0x29 || op == 0x2B) {
			emitStore(&k, a, instr, pending);
			inline_ = true;
		} else if (op == 0x12) {
			inline_ = emitGte(&k, a, instr, pending);
		} else if (kind(instr) == 0) {
			inline_ = emitAlu(&k, a, instr, pending);
		} else if (b->delay && i == b->count - 2) {
			emitBranch(&k, a, instr, pending);
			inline_ = true;
		}
		if (inline_) {
			if (k.slowCount) {
				uint8_t* over = aB(e, NULL);
				for (int s = 0; s < k.slowCount; s++) patchHere(e, k.slows[s]);
				helper(&k, a, instr, delay);
				patchHere(e, over);
			}
		} else {
			helper(&k, a, instr, delay);
		}
		pending = leavesLoad(instr);
	}
	// The end of the block: on to the next instruction, or where the branch sent the delay slot
	if (b->delay) {
		aLdr(e, X0, X19, OFF(cpu.next));
		aStr(e, X0, X19, OFF(cpu.pc));
		aDpImm(e, A_ADD_IMM, X0, X0, 4);
		aStr(e, X0, X19, OFF(cpu.next));
	} else {
		uint32_t endPc = pc + 4 * b->count;
		storeWordI(e, OFF(cpu.pc), endPc);
		storeWordI(e, OFF(cpu.next), endPc + 4);
	}
	aStrb(e, ZR, X19, OFF(cpu.branched));
	aB(e, j->dispatch);
	// Leaving before an instruction when the limit is reached
	for (int i = 0; i < b->count; i++) {
		patchHere(e, k.stubSites[i]);
		storeWordI(e, OFF(cpu.pc), pc + 4 * i);
		if (b->delay && i == b->count - 1) {
			storeByteI(e, OFF(cpu.branched), 1);
		} else {
			storeWordI(e, OFF(cpu.next), pc + 4 * i + 4);
			aStrb(e, ZR, X19, OFF(cpu.branched));
		}
		aB(e, k.leave);
	}
	return e->p > e->end ? NULL : e->p;
}

static void flushCode(uint8_t* start, uint8_t* end) {
	__builtin___clear_cache((char*) start, (char*) end);
}

// The way in, the way out and the dispatcher, as jit_x64.h has them.
static void emitShared(struct Psx* p, struct Jit* j) {
	struct Emitter e = { j->arena, j->arena, j->arena + ARENA_SIZE };
	j->enter = (void (*)(struct Psx*, int64_t, uint8_t*)) (void*) e.p;
	aPush(&e, X29, X30, 80);
	aStp(&e, X19, X20, 16);
	aStp(&e, X21, X22, 32);
	aStp(&e, X23, X24, 48);
	aStp(&e, X25, X26, 64);
	aMovX(&e, X19, X0);
	aMovX(&e, X20, X1);
	aLdrX(&e, X21, X19, OFF(cycles));
	aAddImmX(&e, X23, X19, OFF(ram));
	aMovImm64(&e, X24, (uint64_t) (uintptr_t) j->table);
	aAddImmX(&e, X25, X19, OFF(scratch));
	aAddImmX(&e, X26, X19, OFF(cpu.absorb));
	loadLimit(&e);
	aBr(&e, X2);
	j->leave = e.p;
	aStrX(&e, X21, X19, OFF(cycles));
	aLdp(&e, X19, X20, 16);
	aLdp(&e, X21, X22, 32);
	aLdp(&e, X23, X24, 48);
	aLdp(&e, X25, X26, 64);
	aPop(&e, X29, X30, 80);
	aRet(&e);
	j->dispatch = e.p;
	aCmpX(&e, X21, X22);
	aBcond(&e, C_GE, j->leave);
	aLdr(&e, X0, X19, OFF(cpu.pc));
	aLsrImm(&e, X1, X0, 2);
	aLogicImm(&e, A_AND_IMM, X1, X1, TABLE_SIZE - 1);
	aLdst(&e, L_LDRX, X2, X24, X1, L_UXTW | SCALED);
	aCbzX(&e, X2, j->leave);
	aLdr(&e, X3, X2, (unsigned) offsetof(struct Block, pc));
	aCmp(&e, X0, X3);
	aBcond(&e, C_NE, j->leave);
	aLdr(&e, X4, X19, OFF(cpu.icacheChanges));
	aLdr(&e, X5, X2, (unsigned) offsetof(struct Block, checked));
	aCmp(&e, X4, X5);
	uint8_t* go = aBcond(&e, C_EQ, NULL);
	// The cache changed since: the block's own lines decide
	aLsrImm(&e, X6, X3, 4);
	aLdr(&e, X7, X2, (unsigned) offsetof(struct Block, lines));
	aAddImmX(&e, X8, X2, (unsigned) offsetof(struct Block, gen));
	aAddImmX(&e, X11, X19, OFF(cpu.icacheGen));
	uint8_t* loop = e.p;
	aLogicImm(&e, A_AND_IMM, X9, X6, 255);
	aLdst(&e, L_LDR, X12, X11, X9, L_UXTW | SCALED);
	aLdr(&e, X13, X8, 0);
	aCmp(&e, X12, X13);
	aBcond(&e, C_NE, j->leave);
	aAddImmX(&e, X8, X8, 4);
	aDpImm(&e, A_ADD_IMM, X6, X6, 1);
	aDpImm(&e, A_SUBS_IMM, X7, X7, 1);
	aBcond(&e, C_NE, loop);
	aStr(&e, X4, X2, (unsigned) offsetof(struct Block, checked));
	patchHere(&e, go);
	aLdrX(&e, X3, X2, (unsigned) offsetof(struct Block, body));
	aBr(&e, X3);
	j->used = (size_t) (e.p - j->arena + 15) & ~(size_t) 15;
}
