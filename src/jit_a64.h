/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
// The AArch64 backend of jit.c, instruction for instruction what jit_x64.h emits.
//
// Registers: x19 = p, x20 = the frame's limit, x21 = p->cycles, x22 = the limit (the next event or
// the frame's limit, whichever is first), x23 = RAM, x24 = the block table, x25 = the scratchpad,
// x26 = cpu.absorb. w0-w13 are scratch (the state's primitives use w0-w2 only); calls into C keep
// x19-x28.

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

static void loadGpr(struct Emitter* e, int host, int reg) {
	if (reg) {
		aLdr(e, host, X19, GPR_OFF(reg));
	} else {
		aMov(e, host, ZR);
	}
}

enum { FAST_JIT = 0 };  // the Fast mode's code isn't here yet: its blocks are the Exact mode's

struct Compile {
	struct Emitter e;
	struct Jit* j;
	struct State st;
	bool fast;
	bool immediate;
	uint8_t* stubSites[BLOCK_MAX + 1];
	uint8_t* slows[BLOCK_MAX * 2];
	int slowCount;
	struct Cold cold;
	struct { uint8_t* site; uint8_t* cont; int i, k; struct State st; } lines[BLOCK_MAX / 4 + 2];
	int lineCount;
};

// The primitives jit.c's state uses (w0-w2 only).

static void bAddCycles(struct Compile* k, int n) {
	aDpImm(&k->e, A_ADD_IMM | W64, X21, X21, n);
}

static void bStoreAbsorb(struct Compile* k, int reg, int v) {
	if (v) {
		movI(&k->e, X0, v);
		aStrb(&k->e, X0, X26, reg);
	} else {
		aStrb(&k->e, ZR, X26, reg);
	}
}

// The fetch's cycle unless w0's absorb entry still hides some (then one less is hidden).
static void absorbStepW0(struct Emitter* e) {
	aLdst(e, L_LDRB, X1, X26, X0, L_UXTW);
	aCmpImm(e, X1, 0);
	aCset(e, X2, C_EQ);
	aDp(e, A_ADD | W64, X21, X21, X2, S_LSL, 0);
	aDpImm(e, A_SUB_IMM, X1, X1, 1);
	aDp(e, A_ADD, X1, X1, X2, S_LSL, 0);
	aLdst(e, L_STRB, X1, X26, X0, L_UXTW);
}

static void bAbsorbStepAt(struct Compile* k, int reg) {
	movI(&k->e, X0, reg);
	absorbStepW0(&k->e);
}

// w0 = memory's absorbWhich with or ORed in
static void awIndirect(struct Emitter* e, int or) {
	aLdrb(e, X0, X19, OFF(cpu.absorbWhich));
	if (or) logicImm(e, A_ORR_IMM, A_ORR, X0, X0, or, X1);
}

static void bAbsorbStepIndirect(struct Compile* k, int or) {
	awIndirect(&k->e, or);
	absorbStepW0(&k->e);
}

static void bZeroAbsorbIndirect(struct Compile* k, int or) {
	awIndirect(&k->e, or);
	aLdst(&k->e, L_STRB, ZR, X26, X0, L_UXTW);
}

static void bStoreAw(struct Compile* k, int v) {
	storeByteI(&k->e, OFF(cpu.absorbWhich), v);
}

static void bOrAw(struct Compile* k, int or) {
	awIndirect(&k->e, or);
	aStrb(&k->e, X0, X19, OFF(cpu.absorbWhich));
}

static void bStoreFudge(struct Compile* k, int v) {
	storeByteI(&k->e, OFF(cpu.readFudge), v);
}

static void bCommitLoad(struct Compile* k, int reg) {
	struct Emitter* e = &k->e;
	aLdr(e, X0, X19, OFF(cpu.ldValue));
	aStr(e, X0, X19, GPR_OFF(reg));
	aLdrb(e, X0, X19, OFF(cpu.ldAbsorb));
	aStrb(e, X0, X26, reg);
	storeWordI(e, OFF(cpu.ldReg), NO_LOAD);
	if (!reg) aStr(e, ZR, X19, GPR_OFF(0));
}

static void bCommitDynamic(struct Compile* k);

static void bCommitLoadFast(struct Compile* k, int reg) {
	(void) reg;
	bCommitDynamic(k);
}

static void bCommitDynamicFast(struct Compile* k) {
	bCommitDynamic(k);
}

static void bCommitDynamic(struct Compile* k) {
	struct Emitter* e = &k->e;
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
	storeWordI(e, OFF(cpu.ldReg), NO_LOAD);
	aStr(e, ZR, X19, GPR_OFF(0));
}

static void bSurchargeIndirect(struct Compile* k) {
	struct Emitter* e = &k->e;
	aLdrb(e, X0, X19, OFF(cpu.readFudge));
	aLsrImm(e, X0, X0, 4);
	aLogicImm(e, A_AND_IMM, X0, X0, 2);
	aDp(e, A_ADD | W64, X21, X21, X0, S_LSL, 0);
}

// The block's frame.

static void bBegin(struct Compile* k, struct Jit* j, uint8_t* start, uint8_t* end) {
	memset(k, 0, sizeof *k);
	k->e.start = k->e.p = start;
	k->e.end = end;
	k->j = j;
}

static uint8_t* bHere(struct Compile* k) {
	return k->e.p;
}

static void bJumpTo(struct Compile* k, uint8_t* target) {
	aB(&k->e, target);
}

static void slowAt(struct Compile* k, uint8_t* site) {
	k->slows[k->slowCount++] = site;
}

static void bPatchSlows(struct Compile* k) {
	for (int i = 0; i < k->slowCount; i++) patchHere(&k->e, k->slows[i]);
}

// x0 = the cycle count (or when the GTE is done, if that's later, with gte) plus bound, compared
// with the limit.
static void compareBound(struct Emitter* e, int bound, bool gte) {
	if (gte) {
		aLdrX(e, X0, X19, OFF(cpu.gteDone));
		aCmpX(e, X0, X21);
		aCselX(e, X0, X21, X0, C_LT);
		aAddImmX(e, X0, X0, (uint32_t) bound);
	} else {
		aAddImmX(e, X0, X21, (uint32_t) bound);
	}
	aCmpX(e, X0, X22);
}

static void bDecline(struct Compile* k, int bound, bool gte, uint8_t* careful) {
	compareBound(&k->e, bound, gte);
	aBcond(&k->e, C_GE, careful);
}

static void bCheck(struct Compile* k, int bound, bool gte, int next) {
	compareBound(&k->e, bound, gte);
	k->stubSites[next] = aBcond(&k->e, C_GE, NULL);
}

// Before instruction i, the first of the block's line k: on while the line's generation is the one
// it was last found as compiled at (see jit_x64.h).
static void bLineCheck(struct Compile* k, struct Block* b, int line, int i) {
	struct Emitter* e = &k->e;
	uint32_t pc = b->pc + 4 * i;
	aLdr(e, X0, X19, OFF(cpu.icacheGen) + 4 * ((pc >> 4) & 255));
	aMovImm64(e, X1, (uint64_t) (uintptr_t) &b->gen[line]);
	aLdr(e, X1, X1, 0);
	aCmp(e, X0, X1);
	k->lines[k->lineCount].site = aBcond(e, C_NE, NULL);
	k->lines[k->lineCount].cont = e->p;
	k->lines[k->lineCount].i = i;
	k->lines[k->lineCount].k = line;
	k->lines[k->lineCount].st = k->st;
	k->lineCount++;
}

static void bHelper(struct Compile* k, uint32_t pc, uint32_t instr, bool delay) {
	struct Emitter* e = &k->e;
	aStrX(e, X21, X19, OFF(cycles));
	aMovX(e, X0, X19);
	movI(e, X1, pc);
	movI(e, X2, instr);
	movI(e, X3, delay);
	callC(e, (const void*) cpuJitExecute);
	aLdrX(e, X21, X19, OFF(cycles));
	uint8_t* stay = aCbz(e, X0, NULL);
	aB(e, k->j->leave);
	patchHere(e, stay);
	loadLimit(e);
}

// x2 = a block (memory holds the state): into its clean version when nothing hides any load time and
// no load is in flight, otherwise its generic one.
static void enterVersion(struct Emitter* e, int version) {
	if (version == DYNAMIC) {
		aLdr(e, X0, X19, OFF(cpu.ldReg));
		aCmpImm(e, X0, NO_LOAD);
		uint8_t* generic = aBcond(e, C_NE, NULL);
		aLdrX(e, X0, X26, 0);
		aLdrX(e, X1, X26, 8);
		aDp(e, A_ORR | W64, X0, X0, X1, S_LSL, 0);
		aLdrX(e, X1, X26, 16);
		aDp(e, A_ORR | W64, X0, X0, X1, S_LSL, 0);
		aLdrX(e, X1, X26, 24);
		aDp(e, A_ORR | W64, X0, X0, X1, S_LSL, 0);
		uint8_t* generic2 = aCbnzX(e, X0, NULL);
		aLdrX(e, X3, X2, (unsigned) offsetof(struct Block, body[CLEAN]));
		aBr(e, X3);
		patchHere(e, generic);
		patchHere(e, generic2);
		version = GENERIC;
	}
	aLdrX(e, X3, X2, (unsigned) offsetof(struct Block, body[version]));
	aBr(e, X3);
}

static void linkTo(struct Compile* k, struct Block* b, int s, uint32_t pc, int version) {
	struct Emitter* e = &k->e;
	aCmpX(e, X21, X22);
	uint8_t* in = aBcond(e, C_LT, NULL);
	aB(e, k->j->leave);
	patchHere(e, in);
	aMovImm64(e, X2, (uint64_t) (uintptr_t) &b->link[s]);
	aLdrX(e, X2, X2, 0);
	uint8_t* none = aCbzX(e, X2, NULL);
	aLdr(e, X4, X19, OFF(cpu.icacheChanges));
	aLdr(e, X5, X2, (unsigned) offsetof(struct Block, checked));
	aCmp(e, X4, X5);
	uint8_t* changed = aBcond(e, C_NE, NULL);
	uint8_t* go = e->p;
	enterVersion(e, version);
	patchHere(e, changed);
	aAddImmX(e, X11, X19, OFF(cpu.icacheGen));
	aLdr(e, X12, X11, 4 * ((pc >> 4) & 255));
	aLdr(e, X13, X2, (unsigned) offsetof(struct Block, gen));
	aCmp(e, X12, X13);
	uint8_t* miss = aBcond(e, C_NE, NULL);
	aStr(e, X4, X2, (unsigned) offsetof(struct Block, checked));
	aB(e, go);
	patchHere(e, none);
	patchHere(e, miss);
	aStrX(e, X21, X19, OFF(cycles));
	aMovX(e, X0, X19);
	aMovImm64(e, X1, (uint64_t) (uintptr_t) &b->link[s]);
	movI(e, X2, pc);
	callC(e, (const void*) jitLink);
	aLdrX(e, X21, X19, OFF(cycles));
	loadLimit(e);
	aBr(e, X0);
}

static void bEnd(struct Compile* k, struct Block* b, int version) {
	struct Emitter* e = &k->e;
	if (!b->delay) {
		uint32_t endPc = b->pc + 4 * b->count;
		storeWordI(e, OFF(cpu.pc), endPc);
		storeWordI(e, OFF(cpu.next), endPc + 4);
		aStrb(e, ZR, X19, OFF(cpu.branched));
		linkTo(k, b, 0, endPc, version);
		return;
	}
	uint32_t pc = b->pc + 4 * (b->count - 2), instr = b->words[b->count - 2];
	int op = instr >> 26;
	aLdr(e, X0, X19, OFF(cpu.next));
	aStr(e, X0, X19, OFF(cpu.pc));
	aDpImm(e, A_ADD_IMM, X1, X0, 4);
	aStr(e, X1, X19, OFF(cpu.next));
	aStrb(e, ZR, X19, OFF(cpu.branched));
	if (op == 0x00) {
		aB(e, k->j->dispatch);
		return;
	}
	uint32_t after = pc + 8, target = pc + 4 + ((uint32_t) (int32_t) (int16_t) instr << 2);
	if (op == 0x02 || op == 0x03) {
		linkTo(k, b, 0, ((pc + 4) & 0xF0000000) | (instr & 0x3FFFFFF) << 2, version);
	} else if (target == after) {
		linkTo(k, b, 0, target, version);
	} else {
		movI(e, X1, target);
		aCmp(e, X0, X1);
		uint8_t* other = aBcond(e, C_NE, NULL);
		linkTo(k, b, 0, target, version);
		patchHere(e, other);
		linkTo(k, b, 1, after, version);
	}
}

static uint8_t* bFinish(struct Compile* k, struct Block* b) {
	struct Emitter* e = &k->e;
	for (int l = 0; l < k->lineCount; l++) {
		int i = k->lines[l].i;
		uint32_t pc = b->pc + 4 * i;
		patchHere(e, k->lines[l].site);
		struct State keep = k->st;
		k->st = k->lines[l].st;
		stFlush(k);
		k->st = keep;
		storeWordI(e, OFF(cpu.pc), pc);
		if (b->delay && i == b->count - 1) {
			storeByteI(e, OFF(cpu.branched), 1);
		} else {
			storeWordI(e, OFF(cpu.next), pc + 4);
			aStrb(e, ZR, X19, OFF(cpu.branched));
		}
		aStrX(e, X21, X19, OFF(cycles));
		aMovX(e, X0, X19);
		aMovImm64(e, X1, (uint64_t) (uintptr_t) b);
		movI(e, X2, k->lines[l].k);
		movI(e, X3, i);
		callC(e, (const void*) jitLineMiss);
		aLdrX(e, X21, X19, OFF(cycles));
		loadLimit(e);
		aDpImm(e, A_SUBS_IMM | W64, ZR, X0, 1);
		aBcond(e, C_EQ, k->lines[l].cont);
		aBr(e, X0);
	}
	for (int i = 1; i < b->count; i++) {
		if (!k->stubSites[i]) continue;
		patchHere(e, k->stubSites[i]);
		storeWordI(e, OFF(cpu.pc), b->pc + 4 * i);
		if (b->delay && i == b->count - 1) {
			storeByteI(e, OFF(cpu.branched), 1);
		} else {
			storeWordI(e, OFF(cpu.next), b->pc + 4 * i + 4);
			aStrb(e, ZR, X19, OFF(cpu.branched));
			// to go on from here
			aMovImm64(e, X1, (uint64_t) (uintptr_t) &k->j->resumeBlock);
			aMovImm64(e, X2, (uint64_t) (uintptr_t) b);
			aStrX(e, X2, X1, 0);
			movI(e, X2, i);
			aStr(e, X2, X1, (unsigned) (offsetof(struct Jit, resumeIndex) - offsetof(struct Jit, resumeBlock)));
			aLdr(e, X2, X19, OFF(cpu.icacheChanges));
			aStr(e, X2, X1, (unsigned) (offsetof(struct Jit, resumeChanges) - offsetof(struct Jit, resumeBlock)));
		}
		aB(e, k->j->leave);
	}
	return e->p > e->end ? NULL : e->p;
}

static void flushCode(uint8_t* start, uint8_t* end) {
	__builtin___clear_cache((char*) start, (char*) end);
}

// The instructions.

// x9 = the RAM offset of the address in w10, from its 1 MB page's entry in the table before the
// block table (x3 and x4 are scratch); taken at *otherSite when it isn't RAM.
static void ramOffset(struct Compile* k, uint8_t** otherSite) {
	struct Emitter* e = &k->e;
	aLsrImm(e, X3, X10, 20);
	aDpImm(e, A_SUB_IMM | W64, X4, X24, PAGES * 8);
	aLdst(e, L_LDRX, X9, X4, X3, L_UXTW | SCALED);
	aDp(e, A_ADDS | W64, X9, X9, X10, S_LSL, 0);
	*otherSite = aBcond(e, C_MI, NULL);
}

// Not RAM: w9 = the scratchpad offset of the address in w10, or the slow path. The scratchpad is
// (a & 0x7FFFFC00) == 0x1F800000 below 0xA0000000 (w2 and w3 are scratch).
static void scratchOffset(struct Compile* k) {
	struct Emitter* e = &k->e;
	aLogicImm(e, A_AND_IMM, X2, X10, 0x7FFFFC00);
	movI(e, X3, 0x1F800000);
	aCmp(e, X2, X3);
	slowAt(k, aBcond(e, C_NE, NULL));
	movI(e, X3, 0xA0000000);
	aCmp(e, X10, X3);
	slowAt(k, aBcond(e, C_HS, NULL));
	aLogicImm(e, A_AND_IMM, X9, X10, 0x3FF);
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

static int cancelSame(struct Compile* k, int pending, int rt) {
	struct Emitter* e = &k->e;
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

// The scratchpad's way for a memory instruction, from *site with the state as it is now, back to here.
static void coldHere(struct Compile* k, int kind, uint8_t* site, uint32_t instr, int pending, struct State start) {
	k->cold = (struct Cold) { site, k->e.p, kind, pending, instr, start, k->immediate };
}

// A load from [x23 + x9] (RAM) or [x25 + x9] (the scratchpad, pass 1), then the load in flight as
// the interpreter leaves it.
static void loadPass(struct Compile* k, uint32_t instr, int pending, int pass) {
	struct Emitter* e = &k->e;
	int op = instr >> 26, rs = instr >> 21 & 31, rt = instr >> 16 & 31;
	static const uint32_t loads[6] = { L_LDRSB, L_LDRSH, 0, L_LDR, L_LDRB, L_LDRH };
	aLdst(e, loads[op - 0x20], X5, pass == 0 ? X23 : X25, X9, L_UXTW);
	stUses(k, rs);
	stCommit(k, cancelSame(k, pending, rt));
	int surcharge = stBeginRead(k);
	if (pass == 0) {
		if (surcharge < 0) bSurchargeIndirect(k);
		aDpImm(e, A_ADD_IMM | W64, X21, X21, 5 + (surcharge > 0 ? surcharge : 0));
		storeByteI(e, OFF(cpu.ldAbsorb), 5);
	} else {
		aStrb(e, ZR, X19, OFF(cpu.ldAbsorb));
	}
	aStr(e, X5, X19, OFF(cpu.ldValue));
	storeWordI(e, OFF(cpu.ldReg), rt);
}

// LWC2's read from RAM or the scratchpad, as loadPass.
static void lwc2Pass(struct Compile* k, uint32_t instr, int pending, int pass) {
	struct Emitter* e = &k->e;
	int rs = instr >> 21 & 31;
	aLdst(e, L_LDR, X5, pass == 0 ? X23 : X25, X9, L_UXTW);
	stUses(k, rs);
	stCommit(k, pending);
	int surcharge = stBeginRead(k);
	if (pass == 0) {
		if (surcharge < 0) bSurchargeIndirect(k);
		aDpImm(e, A_ADD_IMM | W64, X21, X21, 4 + (surcharge > 0 ? surcharge : 0));
		storeByteI(e, OFF(cpu.ldAbsorb), 4);
	} else {
		aStrb(e, ZR, X19, OFF(cpu.ldAbsorb));
	}
}

static void emitLoad(struct Compile* k, uint32_t instr, int pending) {
	int op = instr >> 26;
	int width = op == 0x23 ? 4 : (op == 0x21 || op == 0x25) ? 2 : 1;
	address(k, instr, width);
	uint8_t* otherSite;
	ramOffset(k, &otherSite);
	struct State start = k->st;
	loadPass(k, instr, pending, 0);
	coldHere(k, COLD_LOAD, otherSite, instr, pending, start);
}

static void emitStore(struct Compile* k, uint32_t instr, int pending) {
	struct Emitter* e = &k->e;
	int op = instr >> 26, rs = instr >> 21 & 31, rt = instr >> 16 & 31;
	int width = op == 0x2B ? 4 : op == 0x29 ? 2 : 1;
	address(k, instr, width);
	aLdr(e, X0, X19, OFF(cpu.sr));
	slowAt(k, aTbnz(e, X0, 16, NULL));
	uint8_t* otherSite;
	ramOffset(k, &otherSite);
	aDp(e, A_ADD | W64, X11, X23, X9, S_LSL, 0);
	coldHere(k, COLD_STORE, otherSite, instr, pending, k->st);
	loadGpr(e, X5, rt);
	stUses(k, rs);
	stUses(k, rt);
	stCommit(k, pending);
	if (width == 4) aStr(e, X5, X11, 0);
	else if (width == 2) aStrh(e, X5, X11, 0);
	else aStrb(e, X5, X11, 0);
}

// The scratchpad's way: the address in x9 and x11 (or the slow path), then back.
static void bCold(struct Compile* k, struct Cold* c) {
	struct Emitter* e = &k->e;
	patchHere(e, c->site);
	k->st = c->start;
	scratchOffset(k);
	if (c->kind == COLD_STORE) aDp(e, A_ADD | W64, X11, X25, X9, S_LSL, 0);
	if (c->kind == COLD_LOAD) loadPass(k, c->instr, c->pending, 1);
	if (c->kind == COLD_LWC2) lwc2Pass(k, c->instr, c->pending, 1);
	aB(e, c->back);
}

static void emitAlu(struct Compile* k, uint32_t instr, int pending) {
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
			stUses(k, rt); stUses(k, rd);
			break;
		case 0x04: case 0x06: case 0x07:
			loadGpr(e, X7, rt);
			loadGpr(e, X6, rs);
			aDp(e, fn == 4 ? A_LSLV : fn == 6 ? A_LSRV : A_ASRV, X5, X7, X6, S_LSL, 0);
			stUses(k, rs); stUses(k, rt); stUses(k, rd);
			break;
		case 0x11: case 0x13:
			loadGpr(e, X5, rs);
			stUses(k, rs);
			stCommit(k, pending);
			aStr(e, X5, X19, GPR_OFF(fn == 0x11 ? 32 : 33));
			return;
		case 0x10: case 0x12:
			stUses(k, rd);
			stCommit(k, pending);
			stFlush(k);
			aStrX(e, X21, X19, OFF(cycles));
			aMovX(e, X0, X19);
			callC(e, (const void*) cpuJitWaitMuldiv);
			aLdrX(e, X21, X19, OFF(cycles));
			stAfterWait(k);
			if (rd) {
				aLdr(e, X0, X19, GPR_OFF(fn == 0x10 ? 32 : 33));
				aStr(e, X0, X19, GPR_OFF(rd));
			}
			return;
		case 0x18: case 0x19:
			loadGpr(e, X6, rs);
			loadGpr(e, X7, rt);
			if (fn == 0x18) {
				aAsrImm(e, X8, X6, 31);
				aDp(e, A_EOR, X8, X6, X8, S_LSL, 0);
			} else {
				aMov(e, X8, X6);
			}
			aLogicImm(e, A_ORR_IMM, X8, X8, 0x400);
			aClz(e, X8, X8);
			// 7, plus 3 below 21 leading zeros, plus 4 more below 12
			movI(e, X9, 7);
			aCmpImm(e, X8, 21);
			aCset(e, X10, C_LO);
			aDp(e, A_ADD, X9, X9, X10, S_LSL, 0);
			aDp(e, A_ADD, X9, X9, X10, S_LSL, 1);
			aCmpImm(e, X8, 12);
			aCset(e, X10, C_LO);
			aDp(e, A_ADD, X9, X9, X10, S_LSL, 2);
			aDp(e, A_ADD | W64, X9, X21, X9, S_LSL, 0);
			aStrX(e, X9, X19, OFF(cpu.muldivDone));
			if (fn == 0x18) aSmull(e, X5, X6, X7);
			else aUmull(e, X5, X6, X7);
			aStr(e, X5, X19, GPR_OFF(33));
			aLsrImmX(e, X5, X5, 32);
			aStr(e, X5, X19, GPR_OFF(32));
			stUses(k, rs); stUses(k, rt);
			stCommit(k, pending);
			return;
		case 0x1A: case 0x1B:
			loadGpr(e, X1, rs);
			loadGpr(e, X2, rt);
			aMovX(e, X0, X19);
			movI(e, X3, fn == 0x1A);
			callC(e, (const void*) cpuJitDivide);
			aDpImm(e, A_ADD_IMM | W64, X0, X21, 37);
			aStrX(e, X0, X19, OFF(cpu.muldivDone));
			stUses(k, rs); stUses(k, rt);
			stCommit(k, pending);
			return;
		case 0x2A: case 0x2B:
			loadGpr(e, X6, rs);
			loadGpr(e, X7, rt);
			aCmp(e, X6, X7);
			aCset(e, X5, fn == 0x2A ? C_LT : C_LO);
			stUses(k, rs); stUses(k, rt); stUses(k, rd);
			break;
		default: {
			loadGpr(e, X6, rs);
			loadGpr(e, X7, rt);
			static const uint32_t alu[8] = { A_ADDS, A_ADD, A_SUBS, A_SUB, A_AND, A_ORR, A_EOR, A_ORR };
			aDp(e, alu[fn & 7], X5, X6, X7, S_LSL, 0);
			if (fn == 0x20 || fn == 0x22) slowAt(k, aBcond(e, C_VS, NULL));
			if (fn == 0x27) aMvn(e, X5, X5);
			stUses(k, rs); stUses(k, rt); stUses(k, rd);
			break;
		}
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
		default:
			movI(e, X5, immu << 16);
			stUses(k, rt);
			goto result;
		}
		stUses(k, rs); stUses(k, rt);
	}
result:
	stCommit(k, pending);
	if (dst) aStr(e, X5, X19, GPR_OFF(dst));
}

static void emitBranch(struct Compile* k, uint32_t pc, uint32_t instr, int pending) {
	struct Emitter* e = &k->e;
	int op = instr >> 26, rs = instr >> 21 & 31, rt = instr >> 16 & 31, rd = instr >> 11 & 31;
	uint32_t after = pc + 8, target = pc + 4 + ((uint32_t) (int32_t) (int16_t) instr << 2);
	uint32_t jump = ((pc + 4) & 0xF0000000) | (instr & 0x3FFFFFF) << 2;
	if (op == 0x02 || op == 0x03) {
		if (op == 0x03) stUses(k, 31);
		stCommit(k, pending);
		if (op == 0x03) storeWordI(e, GPR_OFF(31), after);
		storeWordI(e, OFF(cpu.next), jump);
		return;
	}
	if (op == 0x00) {
		loadGpr(e, X5, rs);
		stUses(k, rs);
		stUses(k, rd);
		stCommit(k, pending);
		if ((instr & 63) == 0x09 && rd) storeWordI(e, GPR_OFF(rd), after);
		aStr(e, X5, X19, OFF(cpu.next));
		return;
	}
	loadGpr(e, X6, rs);
	if (op == 0x04 || op == 0x05) loadGpr(e, X7, rt);
	stUses(k, rs);
	bool link = op == 0x01 && (rt & 0x1E) == 0x10;
	if (op == 0x04 || op == 0x05) stUses(k, rt);
	if (link) stUses(k, 31);
	stCommit(k, pending);
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

static void waitGte(struct Emitter* e) {
	aLdrX(e, X0, X19, OFF(cpu.gteDone));
	aCmpX(e, X21, X0);
	aCselX(e, X21, X0, X21, C_LT);
}

// LWC2/SWC2 (see jit_x64.h). Calls keep x19-x28 only, so SWC2 keeps its address on the stack across
// the GTE call.
static void emitGteMemory(struct Compile* k, uint32_t instr, int pending) {
	struct Emitter* e = &k->e;
	int op = instr >> 26, rs = instr >> 21 & 31, rt = instr >> 16 & 31;
	aLdr(e, X0, X19, OFF(cpu.sr));
	slowAt(k, aTbz(e, X0, 30, NULL));
	address(k, instr, 4);
	if (op == 0x3A) {
		aLdr(e, X0, X19, OFF(cpu.sr));
		slowAt(k, aTbnz(e, X0, 16, NULL));
	}
	uint8_t* otherSite;
	ramOffset(k, &otherSite);
	if (op == 0x3A) {
		aDp(e, A_ADD | W64, X11, X23, X9, S_LSL, 0);
		coldHere(k, COLD_STORE, otherSite, instr, pending, k->st);
		stUses(k, rs);
		stCommit(k, pending);
		waitGte(e);
		aPush(e, X11, X30, 16);
		movI(e, X0, rt);
		callC(e, (const void*) GTE_ReadDR);
		aPop(e, X11, X30, 16);
		aStr(e, X0, X11, 0);
		return;
	}
	struct State start = k->st;
	lwc2Pass(k, instr, pending, 0);
	coldHere(k, COLD_LWC2, otherSite, instr, pending, start);
	waitGte(e);
	movI(e, X0, rt);
	aMov(e, X1, X5);
	callC(e, (const void*) GTE_WriteDR);
}

static void emitGte(struct Compile* k, uint32_t instr, int pending) {
	struct Emitter* e = &k->e;
	int rs = instr >> 21 & 31, rt = instr >> 16 & 31, rd = instr >> 11 & 31;
	aLdr(e, X0, X19, OFF(cpu.sr));
	slowAt(k, aTbz(e, X0, 30, NULL));
	if (instr & 0x02000000) {
		stCommit(k, pending);
		aLdrX(e, X0, X19, OFF(cpu.gteDone));
		aCmpX(e, X21, X0);
		aCselX(e, X21, X0, X21, C_LT);
		movI(e, X0, instr);
		callC(e, (const void*) GTE_Instruction);
		aSxtw(e, X0, X0);
		aDp(e, A_ADD | W64, X0, X0, X21, S_LSL, 0);
		aStrX(e, X0, X19, OFF(cpu.gteDone));
		return;
	}
	if (rs == 0 || rs == 2) {
		stCommit(k, cancelSame(k, pending, rt));
		aLdrX(e, X0, X19, OFF(cpu.gteDone));
		aDp(e, A_SUBS | W64, X0, X0, X21, S_LSL, 0);
		aCselX(e, X0, X0, ZR, C_GT);
		aStrb(e, X0, X19, OFF(cpu.ldAbsorb));
		aDp(e, A_ADD | W64, X21, X21, X0, S_LSL, 0);
		movI(e, X0, rd);
		callC(e, (const void*) (rs == 0 ? GTE_ReadDR : GTE_ReadCR));
		aStr(e, X0, X19, OFF(cpu.ldValue));
		storeWordI(e, OFF(cpu.ldReg), rt);
		return;
	}
	loadGpr(e, X5, rt);
	stCommit(k, pending);
	aLdrX(e, X0, X19, OFF(cpu.gteDone));
	aCmpX(e, X21, X0);
	aCselX(e, X21, X0, X21, C_LT);
	movI(e, X0, rd);
	aMov(e, X1, X5);
	callC(e, (const void*) (rs == 4 ? GTE_WriteDR : GTE_WriteCR));
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
	// The cache changed since: the block's first line decides
	aLsrImm(&e, X6, X0, 4);
	aLogicImm(&e, A_AND_IMM, X6, X6, 255);
	aAddImmX(&e, X11, X19, OFF(cpu.icacheGen));
	aLdst(&e, L_LDR, X12, X11, X6, L_UXTW | SCALED);
	aLdr(&e, X13, X2, (unsigned) offsetof(struct Block, gen));
	aCmp(&e, X12, X13);
	uint8_t* miss = aBcond(&e, C_NE, NULL);
	aStr(&e, X4, X2, (unsigned) offsetof(struct Block, checked));
	patchHere(&e, go);
	uint8_t* clean = aB(&e, NULL);
	// Not as compiled: jitEnterMiss fills it or finishes the instruction
	patchHere(&e, miss);
	aStrX(&e, X21, X19, OFF(cycles));
	aMov(&e, X1, X0);
	aMovX(&e, X0, X19);
	callC(&e, (const void*) jitEnterMiss);
	aLdrX(&e, X21, X19, OFF(cycles));
	loadLimit(&e);
	aBr(&e, X0);
	patchHere(&e, clean);
	// Clean: no load in flight and every absorb entry 0
	aLdr(&e, X0, X19, OFF(cpu.ldReg));
	aCmpImm(&e, X0, NO_LOAD);
	uint8_t* generic = aBcond(&e, C_NE, NULL);
	aLdrX(&e, X0, X26, 0);
	aLdrX(&e, X1, X26, 8);
	aDp(&e, A_ORR | W64, X0, X0, X1, S_LSL, 0);
	aLdrX(&e, X1, X26, 16);
	aDp(&e, A_ORR | W64, X0, X0, X1, S_LSL, 0);
	aLdrX(&e, X1, X26, 24);
	aDp(&e, A_ORR | W64, X0, X0, X1, S_LSL, 0);
	uint8_t* generic2 = aCbnzX(&e, X0, NULL);
	aLdrX(&e, X3, X2, (unsigned) offsetof(struct Block, body[CLEAN]));
	aBr(&e, X3);
	patchHere(&e, generic);
	patchHere(&e, generic2);
	aLdrX(&e, X3, X2, (unsigned) offsetof(struct Block, body[GENERIC]));
	aBr(&e, X3);
	j->used = (size_t) (e.p - j->arena + 15) & ~(size_t) 15;
}
