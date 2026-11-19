/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
// The x86-64 backend of jit.c.
//
// Registers: rbx = p, rbp = the frame's limit, r13 = p->cycles, r14 = the limit (the next event or
// the frame's limit, whichever is first), r12 = the block table, r15 = RAM.

#include "x64/emit.h"

#define P(f) M(RBX, (int32_t) offsetof(struct Psx, f))
#define GPR(i) M(RBX, (int32_t) (offsetof(struct Psx, cpu.r) + 4 * (i)))
#define ABSORB(i) M(RBX, (int32_t) (offsetof(struct Psx, cpu.absorb) + (i)))
#define ABSORB_AT(reg) MI(RBX, reg, 1, (int32_t) offsetof(struct Psx, cpu.absorb))

static void movI(struct Emitter* e, int reg, uint32_t v) {
	xMovIKeepFlags(e, reg, v);
}

static void callC(struct Emitter* e, const void* f) {
	xMovI64(e, RAX, (uint64_t) (uintptr_t) f);
	xCallR(e, RAX);
}

// r14 = the next event, or the frame's limit when that comes first.
static void loadLimit(struct Emitter* e) {
	xLoad(e, 8, R14, P(nextEvent));
	xAluRR(e, X_CMP, 8, R(R14), RBP);
	xCmov(e, CC_G, 8, R14, R(RBP));
}

// commitLoad for the load the instruction before left in flight: reg is its register, NO_LOAD for
// none, DYNAMIC when it isn't known here (the block's first instruction).
static void commit(struct Emitter* e, int reg) {
	if (reg == NO_LOAD) {
		xStoreI(e, 1, P(cpu.readFudge), NO_LOAD);
		xAluI(e, X_OR, 1, P(cpu.absorbWhich), NO_LOAD & 0x1F);
		return;
	}
	if (reg == DYNAMIC) {
		xLoad(e, 4, RAX, P(cpu.ldReg));
		xLoad(e, 4, R8, P(cpu.ldValue));
		xStore(e, 4, MI(RBX, RAX, 4, (int32_t) offsetof(struct Psx, cpu.r)), R8);
		xMovzx(e, R8, 1, P(cpu.ldAbsorb));
		xStore(e, 1, ABSORB_AT(RAX), R8);
		xStore(e, 1, P(cpu.readFudge), RAX);
		xAluI(e, X_AND, 4, R(RAX), 0x1F);
		xAluRR(e, X_OR, 1, P(cpu.absorbWhich), RAX);
	} else {
		xLoad(e, 4, RAX, P(cpu.ldValue));
		xStore(e, 4, GPR(reg), RAX);
		xMovzx(e, RAX, 1, P(cpu.ldAbsorb));
		xStore(e, 1, ABSORB(reg), RAX);
		xStoreI(e, 1, P(cpu.readFudge), reg);
		if (reg & 0x1F) {
			xAluI(e, X_OR, 1, P(cpu.absorbWhich), reg & 0x1F);
		}
	}
	xStoreI(e, 4, P(cpu.ldReg), NO_LOAD);
	xStoreI(e, 4, GPR(0), 0);
}

static void uses(struct Emitter* e, int reg) {
	if (reg) {
		xStoreI(e, 1, ABSORB(reg), 0);
	}
}

static void loadGpr(struct Emitter* e, int host, int reg) {
	if (reg) {
		xLoad(e, 4, host, GPR(reg));
	} else {
		xMovI(e, host, 0);
	}
}

struct Compile {
	struct Emitter e;
	uint8_t* leave;
	uint8_t* stubSites[BLOCK_MAX];
	struct { uint8_t* site; uint32_t pc, instr; } slows[BLOCK_MAX * 2];
	int slowCount;
};

// The instruction through cpuJitExecute; the block leaves if it has to (always after a delay slot,
// which leaves pc at the branch's target).
static void helper(struct Compile* k, uint32_t pc, uint32_t instr, bool delay) {
	struct Emitter* e = &k->e;
	xStore(e, 8, P(cycles), R13);
	xMovRR(e, 8, RDI, RBX);
	movI(e, RSI, pc);
	movI(e, RDX, instr);
	movI(e, RCX, delay);
	callC(e, (const void*) cpuJitExecute);
	xLoad(e, 8, R13, P(cycles));
	xTest(e, 4, R(RAX), RAX);
	uint8_t* stay = xJcc(e, CC_E, NULL);
	xJmp(e, k->leave);
	patchHere(e, stay);
	loadLimit(e);
}

static uint8_t* slowJump(struct Compile* k, int cc, uint32_t pc, uint32_t instr) {
	uint8_t* site = xJcc(&k->e, cc, NULL);
	k->slows[k->slowCount].site = site;
	k->slows[k->slowCount].pc = pc;
	k->slows[k->slowCount].instr = instr;
	k->slowCount++;
	return site;
}

// edx = the RAM offset of the address in ecx, or a jump to the slow path when it isn't RAM (r8 and
// eax are scratch). With scratch set, the scratchpad is tried first and taken at *scratchSite.
static void ramOffset(struct Compile* k, uint32_t pc, uint32_t instr, uint8_t** scratchSite) {
	struct Emitter* e = &k->e;
	// The scratchpad: (a & 0x7FFFFC00) == 0x1F800000 below 0xA0000000
	xMovRR(e, 4, RAX, RCX);
	xAluI(e, X_AND, 4, R(RAX), 0x7FFFFC00);
	xAluI(e, X_CMP, 4, R(RAX), 0x1F800000);
	uint8_t* notScratch = xJcc(e, CC_NE, NULL);
	xAluI(e, X_CMP, 4, R(RCX), (int32_t) 0xA0000000);
	*scratchSite = xJcc(e, CC_B, NULL);
	patchHere(e, notScratch);
	// RAM: KUSEG, KSEG0 or KSEG1 below 8 MB, mirrored every 2 MB
	xMovRR(e, 4, RDX, RCX);
	xAluI(e, X_AND, 4, R(RDX), 0x1FFFFFFF);
	xAluI(e, X_CMP, 4, R(RDX), 0x800000);
	slowJump(k, CC_AE, pc, instr);
	xMovRR(e, 4, RAX, RCX);
	xShiftI(e, X_SHR, 4, R(RAX), 29);
	xMovI(e, R8, 0x31);
	emitOp2(e, 4, 0x0F, 0xA3, RAX, R(R8));  // bt r8d, eax
	slowJump(k, CC_AE, pc, instr);
	xAluI(e, X_AND, 4, R(RDX), RAM_SIZE - 1);
}

// Loads: the address, the fast paths, then the load in flight as the interpreter leaves it.
static void emitLoad(struct Compile* k, uint32_t pc, uint32_t instr, int pending) {
	struct Emitter* e = &k->e;
	int op = instr >> 26, rs = instr >> 21 & 31, rt = instr >> 16 & 31;
	int32_t imm = (int16_t) instr;
	int width = op == 0x23 ? 4 : (op == 0x21 || op == 0x25) ? 2 : 1;
	loadGpr(e, RCX, rs);
	if (imm) xAluI(e, X_ADD, 4, R(RCX), imm);
	if (width > 1) {
		xTestI(e, 1, R(RCX), width - 1);
		slowJump(k, CC_NE, pc, instr);
	}
	uint8_t* scratchSite;
	ramOffset(k, pc, instr, &scratchSite);
	uint8_t* done = NULL;
	for (int pass = 0; pass < 2; pass++) {
		// pass 0: RAM (edx = offset); pass 1: the scratchpad
		if (pass == 1) {
			patchHere(e, scratchSite);
			xMovRR(e, 4, RDX, RCX);
			xAluI(e, X_AND, 4, R(RDX), 0x3FF);
			xAluI(e, X_ADD, 8, R(RDX), (int32_t) offsetof(struct Psx, scratch));
		}
		struct Opnd src = pass == 0 ? MI(R15, RDX, 1, 0) : MI(RBX, RDX, 1, 0);
		if (width == 4) xLoad(e, 4, RSI, src);
		else if (op == 0x20 || op == 0x21) xMovsx(e, RSI, width, src);
		else xMovzx(e, RSI, width, src);
		uses(e, rs);
		int reg = pending;
		if (pending == DYNAMIC) {
			xAluI(e, X_CMP, 4, P(cpu.ldReg), rt);
			uint8_t* other = xJcc(e, CC_NE, NULL);
			xStoreI(e, 4, P(cpu.ldReg), 0);
			patchHere(e, other);
		} else if (pending == rt) {
			reg = 0;
		}
		commit(e, reg);
		// beginRead: the absorb in use ends
		xMovzx(e, RAX, 1, P(cpu.absorbWhich));
		xStoreI(e, 1, ABSORB_AT(RAX), 0);
		xStoreI(e, 1, P(cpu.absorbWhich), 0);
		if (pass == 0) {
			// 2 more when no load came right before, the bus's 3 and 2 more; 5 to hide
			if (reg == DYNAMIC) {
				xMovzx(e, RAX, 1, P(cpu.readFudge));
				xShiftI(e, X_SHR, 4, R(RAX), 4);
				xAluI(e, X_AND, 4, R(RAX), 2);
				xAluRR(e, X_ADD, 8, R(R13), RAX);
			} else if ((reg >> 4) & 2) {
				xAluI(e, X_ADD, 8, R(R13), 2);
			}
			xAluI(e, X_ADD, 8, R(R13), 5);
			xStoreI(e, 1, P(cpu.ldAbsorb), 5);
		} else {
			xStoreI(e, 1, P(cpu.ldAbsorb), 0);
		}
		xStore(e, 4, P(cpu.ldValue), RSI);
		xStoreI(e, 4, P(cpu.ldReg), rt);
		if (pass == 0) done = xJmp(e, NULL);
	}
	patchHere(e, done);
}

static void emitStore(struct Compile* k, uint32_t pc, uint32_t instr, int pending) {
	struct Emitter* e = &k->e;
	int op = instr >> 26, rs = instr >> 21 & 31, rt = instr >> 16 & 31;
	int32_t imm = (int16_t) instr;
	int width = op == 0x2B ? 4 : op == 0x29 ? 2 : 1;
	loadGpr(e, RCX, rs);
	if (imm) xAluI(e, X_ADD, 4, R(RCX), imm);
	if (width > 1) {
		xTestI(e, 1, R(RCX), width - 1);
		slowJump(k, CC_NE, pc, instr);
	}
	xTestI(e, 4, P(cpu.sr), 0x10000);
	slowJump(k, CC_NE, pc, instr);
	uint8_t* scratchSite;
	ramOffset(k, pc, instr, &scratchSite);
	xAluRR(e, X_ADD, 8, R(RDX), R15);
	uint8_t* write = xJmp(e, NULL);
	patchHere(e, scratchSite);
	xMovRR(e, 4, RDX, RCX);
	xAluI(e, X_AND, 4, R(RDX), 0x3FF);
	xAluRR(e, X_ADD, 8, R(RDX), RBX);
	xAluI(e, X_ADD, 8, R(RDX), (int32_t) offsetof(struct Psx, scratch));
	patchHere(e, write);
	loadGpr(e, RSI, rt);
	uses(e, rs);
	uses(e, rt);
	commit(e, pending);
	xStore(e, width, M(RDX, 0), RSI);
}

// The ALU: operands first (before the load in flight lands), then the commit and the result.
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
			loadGpr(e, RSI, rt);
			if (sa) xShiftI(e, fn == 0 ? X_SHL : fn == 2 ? X_SHR : X_SAR, 4, R(RSI), sa);
			uses(e, rt); uses(e, rd);
			break;
		case 0x04: case 0x06: case 0x07:
			loadGpr(e, RSI, rt);
			loadGpr(e, RCX, rs);
			xShiftCl(e, fn == 4 ? X_SHL : fn == 6 ? X_SHR : X_SAR, 4, R(RSI));
			uses(e, rs); uses(e, rt); uses(e, rd);
			break;
		case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: {
			loadGpr(e, RSI, rs);
			loadGpr(e, RCX, rt);
			static const int alu[8] = { X_ADD, X_ADD, X_SUB, X_SUB, X_AND, X_OR, X_XOR, X_OR };
			xAluRR(e, alu[fn & 7], 4, R(RSI), RCX);
			if (fn == 0x20 || fn == 0x22) slowJump(k, CC_O, pc, instr);
			if (fn == 0x27) xNot(e, 4, R(RSI));
			uses(e, rs); uses(e, rt); uses(e, rd);
			break;
		}
		case 0x2A: case 0x2B:
			loadGpr(e, RAX, rs);
			loadGpr(e, RCX, rt);
			xMovI(e, RSI, 0);
			xAluRR(e, X_CMP, 4, R(RAX), RCX);
			xSetcc(e, fn == 0x2A ? CC_L : CC_B, R(RSI));
			uses(e, rs); uses(e, rt); uses(e, rd);
			break;
		case 0x11: case 0x13:
			// MTHI/MTLO
			loadGpr(e, RSI, rs);
			uses(e, rs);
			commit(e, pending);
			xStore(e, 4, GPR(fn == 0x11 ? 32 : 33), RSI);
			return true;
		default:
			return false;
		}
	} else {
		dst = rt;
		switch (op) {
		case 0x08: case 0x09:
			loadGpr(e, RSI, rs);
			xAluI(e, X_ADD, 4, R(RSI), (int32_t) imm);
			if (op == 0x08) slowJump(k, CC_O, pc, instr);
			break;
		case 0x0A: case 0x0B:
			loadGpr(e, RAX, rs);
			xMovI(e, RSI, 0);
			xAluI(e, X_CMP, 4, R(RAX), (int32_t) imm);
			xSetcc(e, op == 0x0A ? CC_L : CC_B, R(RSI));
			break;
		case 0x0C: case 0x0D: case 0x0E:
			loadGpr(e, RSI, rs);
			if (immu) {
				movI(e, RCX, immu);
				xAluRR(e, op == 0x0C ? X_AND : op == 0x0D ? X_OR : X_XOR, 4, R(RSI), RCX);
			} else if (op == 0x0C) {
				xMovI(e, RSI, 0);
			}
			break;
		case 0x0F:
			movI(e, RSI, immu << 16);
			uses(e, rt);
			goto result;
		default:
			return false;
		}
		uses(e, rs); uses(e, rt);
	}
result:
	commit(e, pending);
	if (dst) xStore(e, 4, GPR(dst), RSI);
	return true;
}

// Branches and jumps: the interpreter's order (operands, the commit, the link), then cpu.next = where
// the delay slot goes on to.
static void emitBranch(struct Compile* k, uint32_t pc, uint32_t instr, int pending) {
	struct Emitter* e = &k->e;
	int op = instr >> 26, rs = instr >> 21 & 31, rt = instr >> 16 & 31, rd = instr >> 11 & 31;
	uint32_t after = pc + 8, target = pc + 4 + ((uint32_t) (int32_t) (int16_t) instr << 2);
	uint32_t jump = ((pc + 4) & 0xF0000000) | (instr & 0x3FFFFFF) << 2;
	if (op == 0x02 || op == 0x03) {
		if (op == 0x03) uses(e, 31);
		commit(e, pending);
		if (op == 0x03) xStoreI(e, 4, GPR(31), after);
		xStoreI(e, 4, P(cpu.next), jump);
		return;
	}
	if (op == 0x00) {
		// JR/JALR
		loadGpr(e, RSI, rs);
		uses(e, rs);
		uses(e, rd);
		commit(e, pending);
		if ((instr & 63) == 0x09 && rd) xStoreI(e, 4, GPR(rd), after);
		xStore(e, 4, P(cpu.next), RSI);
		return;
	}
	loadGpr(e, RSI, rs);
	if (op == 0x04 || op == 0x05) loadGpr(e, RDI, rt);
	uses(e, rs);
	bool link = op == 0x01 && (rt & 0x1E) == 0x10;
	if (op == 0x04 || op == 0x05) uses(e, rt);
	if (link) uses(e, 31);
	commit(e, pending);
	if (link) xStoreI(e, 4, GPR(31), after);
	int cc;
	if (op == 0x04 || op == 0x05) {
		xAluRR(e, X_CMP, 4, R(RSI), RDI);
		cc = op == 0x04 ? CC_E : CC_NE;
	} else {
		xTest(e, 4, R(RSI), RSI);
		cc = op == 0x06 ? CC_LE : op == 0x07 ? CC_G : (rt & 1) ? CC_NS : CC_S;
	}
	movI(e, RAX, after);
	movI(e, RCX, target);
	xCmov(e, cc, 4, RAX, R(RCX));
	xStore(e, 4, P(cpu.next), RAX);
}

// COP2: the GTE's commands and moves, when the GTE is usable (otherwise the interpreter raises the
// exception). Calls into gte.c keep rbx, rbp and r12-r15.
static bool emitGte(struct Compile* k, uint32_t pc, uint32_t instr, int pending) {
	struct Emitter* e = &k->e;
	int rs = instr >> 21 & 31, rt = instr >> 16 & 31, rd = instr >> 11 & 31;
	if (!(instr & 0x02000000) && rs != 0 && rs != 2 && rs != 4 && rs != 6) return false;
	xTestI(e, 4, P(cpu.sr), 0x40000000);
	slowJump(k, CC_E, pc, instr);
	if (instr & 0x02000000) {
		commit(e, pending);
		xLoad(e, 8, RAX, P(cpu.gteDone));
		xAluRR(e, X_CMP, 8, R(R13), RAX);
		xCmov(e, CC_L, 8, R13, R(RAX));
		movI(e, RDI, instr);
		callC(e, (const void*) GTE_Instruction);
		xMovsxd(e, RAX, R(RAX));
		xAluRR(e, X_ADD, 8, R(RAX), R13);
		xStore(e, 8, P(cpu.gteDone), RAX);
		return true;
	}
	if (rs == 0 || rs == 2) {
		int reg = pending;
		if (pending == DYNAMIC) {
			xAluI(e, X_CMP, 4, P(cpu.ldReg), rt);
			uint8_t* other = xJcc(e, CC_NE, NULL);
			xStoreI(e, 4, P(cpu.ldReg), 0);
			patchHere(e, other);
		} else if (pending == rt) {
			reg = 0;
		}
		commit(e, reg);
		// The GTE's wait, which the next instructions may hide
		movI(e, RCX, 0);
		xLoad(e, 8, RAX, P(cpu.gteDone));
		xAluRR(e, X_SUB, 8, R(RAX), R13);
		xCmov(e, CC_L, 8, RAX, R(RCX));
		xStore(e, 1, P(cpu.ldAbsorb), RAX);
		xAluRR(e, X_ADD, 8, R(R13), RAX);
		movI(e, RDI, rd);
		callC(e, (const void*) (rs == 0 ? GTE_ReadDR : GTE_ReadCR));
		xStore(e, 4, P(cpu.ldValue), RAX);
		xStoreI(e, 4, P(cpu.ldReg), rt);
		return true;
	}
	loadGpr(e, RSI, rt);
	commit(e, pending);
	xLoad(e, 8, RAX, P(cpu.gteDone));
	xAluRR(e, X_CMP, 8, R(R13), RAX);
	xCmov(e, CC_L, 8, R13, R(RAX));
	movI(e, RDI, rd);
	callC(e, (const void*) (rs == 4 ? GTE_WriteDR : GTE_WriteCR));
	return true;
}

// A block's code, from start; NULL when it doesn't fit before end.
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
		xAluRR(e, X_CMP, 8, R(R13), R14);
		k.stubSites[i] = xJcc(e, CC_GE, NULL);
		xMovzx(e, RAX, 1, P(cpu.absorbWhich));
		xMovzx(e, RCX, 1, ABSORB_AT(RAX));
		xAluI(e, X_CMP, 4, R(RCX), 1);
		xAluI(e, X_ADC, 8, R(R13), 0);
		xAluI(e, X_CMP, 4, R(RCX), 1);
		xAluI(e, X_ADC, 4, R(RCX), -1);
		xStore(e, 1, ABSORB_AT(RAX), RCX);
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
				uint8_t* over = xJmp(e, NULL);
				for (int s = 0; s < k.slowCount; s++) patchHere(e, k.slows[s].site);
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
		xLoad(e, 4, RAX, P(cpu.next));
		xStore(e, 4, P(cpu.pc), RAX);
		xAluI(e, X_ADD, 4, R(RAX), 4);
		xStore(e, 4, P(cpu.next), RAX);
	} else {
		uint32_t end = pc + 4 * b->count;
		movI(e, RAX, end);
		xStore(e, 4, P(cpu.pc), RAX);
		movI(e, RAX, end + 4);
		xStore(e, 4, P(cpu.next), RAX);
	}
	xStoreI(e, 1, P(cpu.branched), 0);
	xJmp(e, j->dispatch);
	// Leaving before an instruction when the limit is reached
	for (int i = 0; i < b->count; i++) {
		patchHere(e, k.stubSites[i]);
		movI(e, RAX, pc + 4 * i);
		xStore(e, 4, P(cpu.pc), RAX);
		if (b->delay && i == b->count - 1) {
			// before the delay slot: cpu.next is the branch's, and the branch has just run
			xStoreI(e, 1, P(cpu.branched), 1);
		} else {
			movI(e, RAX, pc + 4 * i + 4);
			xStore(e, 4, P(cpu.next), RAX);
			xStoreI(e, 1, P(cpu.branched), 0);
		}
		xJmp(e, k.leave);
	}
	return e->p > e->end ? NULL : e->p;
}

static void flushCode(uint8_t* start, uint8_t* end) {
}

// The code every block shares: the way in (registers set, then the block's body), the way out, and
// the dispatcher a block ends with, which goes straight on to the next block while the limit allows
// and the next block's lines are as it last found them.
static void emitShared(struct Psx* p, struct Jit* j) {
	struct Emitter e = { j->arena, j->arena, j->arena + ARENA_SIZE };
	j->enter = (void (*)(struct Psx*, int64_t, uint8_t*)) (void*) e.p;
	xPush(&e, RBX); xPush(&e, RBP); xPush(&e, R12); xPush(&e, R13); xPush(&e, R14); xPush(&e, R15);
	xAluI(&e, X_SUB, 8, R(RSP), 8);
	xMovRR(&e, 8, RBX, RDI);
	xMovRR(&e, 8, RBP, RSI);
	xLoad(&e, 8, R13, P(cycles));
	xLea(&e, 8, R15, P(ram));
	xMovI64(&e, R12, (uint64_t) (uintptr_t) j->table);
	loadLimit(&e);
	xJmpR(&e, RDX);
	j->leave = e.p;
	xStore(&e, 8, P(cycles), R13);
	xAluI(&e, X_ADD, 8, R(RSP), 8);
	xPop(&e, R15); xPop(&e, R14); xPop(&e, R13); xPop(&e, R12); xPop(&e, RBP); xPop(&e, RBX);
	xRet(&e);
	j->dispatch = e.p;
	xAluRR(&e, X_CMP, 8, R(R13), R14);
	xJcc(&e, CC_GE, j->leave);
	xLoad(&e, 4, RAX, P(cpu.pc));
	xMovRR(&e, 4, RCX, RAX);
	xShiftI(&e, X_SHR, 4, R(RCX), 2);
	xAluI(&e, X_AND, 4, R(RCX), TABLE_SIZE - 1);
	xLoad(&e, 8, RDX, MI(R12, RCX, 8, 0));
	xTest(&e, 8, R(RDX), RDX);
	xJcc(&e, CC_E, j->leave);
	xAluRM(&e, X_CMP, 4, RAX, M(RDX, (int32_t) offsetof(struct Block, pc)));
	xJcc(&e, CC_NE, j->leave);
	xLoad(&e, 4, RAX, P(cpu.icacheChanges));
	xAluRM(&e, X_CMP, 4, RAX, M(RDX, (int32_t) offsetof(struct Block, checked)));
	uint8_t* go = xJcc(&e, CC_E, NULL);
	// The cache changed since: the block's own lines decide
	xLoad(&e, 4, RCX, M(RDX, (int32_t) offsetof(struct Block, pc)));
	xShiftI(&e, X_SHR, 4, R(RCX), 4);
	xLoad(&e, 4, RSI, M(RDX, (int32_t) offsetof(struct Block, lines)));
	xLea(&e, 8, RDI, M(RDX, (int32_t) offsetof(struct Block, gen)));
	uint8_t* loop = e.p;
	xMovzx(&e, R8, 1, R(RCX));
	xLoad(&e, 4, R9, MI(RBX, R8, 4, (int32_t) offsetof(struct Psx, cpu.icacheGen)));
	xAluRM(&e, X_CMP, 4, R9, M(RDI, 0));
	xJcc(&e, CC_NE, j->leave);
	xAluI(&e, X_ADD, 8, R(RDI), 4);
	xAluI(&e, X_ADD, 4, R(RCX), 1);
	xAluI(&e, X_SUB, 4, R(RSI), 1);
	xJcc(&e, CC_NE, loop);
	xStore(&e, 4, M(RDX, (int32_t) offsetof(struct Block, checked)), RAX);
	patchHere(&e, go);
	xJmpM(&e, M(RDX, (int32_t) offsetof(struct Block, body)));
	j->used = (size_t) (e.p - j->arena + 15) & ~(size_t) 15;
}

