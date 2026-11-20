/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
// The x86-64 backend of jit.c.
//
// Registers: rbx = p, rbp = the frame's limit, r13 = p->cycles, r14 = the limit (the next event or
// the frame's limit, whichever is first), r12 = the block table, r15 = RAM. rax, rcx, rdx, rsi, rdi
// and r8 are scratch.

#include "x64/emit.h"

#define P(f) M(RBX, (int32_t) offsetof(struct Psx, f))
#define GPR(i) M(RBX, (int32_t) (offsetof(struct Psx, cpu.r) + 4 * (i)))
#define ABSORB(i) M(RBX, (int32_t) (offsetof(struct Psx, cpu.absorb) + (i)))
#define ABSORB_AT(reg) MI(RBX, reg, 1, (int32_t) offsetof(struct Psx, cpu.absorb))

enum { FAST_JIT = 1 };  // this backend has the Fast mode's code

struct Compile {
	struct Emitter e;
	struct Jit* j;
	struct State st;
	bool fast;  // the Fast or Balanced mode: no ReadAbsorb or instruction cache timing
	bool balanced;  // the Balanced mode: a cycle per instruction plus the bus's time
	bool immediate;  // the Fast mode: this load writes its register at once (the next instruction doesn't read it)
	bool codePages;  // stores check RAM's code pages (the Fast and Balanced modes' blocks, and the BIOS ROM's)
	uint8_t* stubSites[BLOCK_MAX + 1];  // the check before instruction i that leaves near the limit
	uint8_t* slows[BLOCK_MAX * 2];
	int slowCount;
	struct Cold cold;
	// Each new line's check, for its way out: where, what follows it, and the state there
	struct { uint8_t* site; uint8_t* cont; int i, k; struct State st; } lines[BLOCK_MAX / 4 + 2];
	int lineCount;
};

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

static void loadGpr(struct Emitter* e, int host, int reg) {
	if (reg) {
		xLoad(e, 4, host, GPR(reg));
	} else {
		xMovI(e, host, 0);
	}
}

// The primitives jit.c's state uses.

static void bAddCycles(struct Compile* k, int n) {
	xAluI(&k->e, X_ADD, 8, R(R13), n);
}

static void bStoreAbsorb(struct Compile* k, int reg, int v) {
	xStoreI(&k->e, 1, ABSORB(reg), v);
}

// The fetch's cycle unless rax's absorb entry still hides some (then one less is hidden).
static void absorbStepRax(struct Emitter* e) {
	xMovzx(e, RCX, 1, ABSORB_AT(RAX));
	xAluI(e, X_CMP, 4, R(RCX), 1);
	xAluI(e, X_ADC, 8, R(R13), 0);
	xAluI(e, X_CMP, 4, R(RCX), 1);
	xAluI(e, X_ADC, 4, R(RCX), -1);
	xStore(e, 1, ABSORB_AT(RAX), RCX);
}

static void bAbsorbStepAt(struct Compile* k, int reg) {
	movI(&k->e, RAX, reg);
	absorbStepRax(&k->e);
}

// rax = memory's absorbWhich with or ORed in
static void awIndirect(struct Emitter* e, int or) {
	xMovzx(e, RAX, 1, P(cpu.absorbWhich));
	if (or) xAluI(e, X_OR, 4, R(RAX), or);
}

static void bAbsorbStepIndirect(struct Compile* k, int or) {
	awIndirect(&k->e, or);
	absorbStepRax(&k->e);
}

static void bZeroAbsorbIndirect(struct Compile* k, int or) {
	awIndirect(&k->e, or);
	xStoreI(&k->e, 1, ABSORB_AT(RAX), 0);
}

static void bStoreAw(struct Compile* k, int v) {
	xStoreI(&k->e, 1, P(cpu.absorbWhich), v);
}

static void bOrAw(struct Compile* k, int or) {
	xAluI(&k->e, X_OR, 1, P(cpu.absorbWhich), or);
}

static void bStoreFudge(struct Compile* k, int v) {
	xStoreI(&k->e, 1, P(cpu.readFudge), v);
}

// The memory part of commitLoad for a known register.
static void bCommitLoad(struct Compile* k, int reg) {
	struct Emitter* e = &k->e;
	xLoad(e, 4, RAX, P(cpu.ldValue));
	xStore(e, 4, GPR(reg), RAX);
	xMovzx(e, RAX, 1, P(cpu.ldAbsorb));
	xStore(e, 1, ABSORB(reg), RAX);
	xStoreI(e, 4, P(cpu.ldReg), NO_LOAD);
	if (!reg) xStoreI(e, 4, GPR(0), 0);
}

// The Fast mode's commits: the value only.
static void bCommitLoadFast(struct Compile* k, int reg) {
	struct Emitter* e = &k->e;
	if (reg) {
		xLoad(e, 4, RAX, P(cpu.ldValue));
		xStore(e, 4, GPR(reg), RAX);
	}
	xStoreI(e, 4, P(cpu.ldReg), NO_LOAD);
}

static void bCommitDynamicFast(struct Compile* k) {
	struct Emitter* e = &k->e;
	xLoad(e, 4, RAX, P(cpu.ldReg));
	xAluI(e, X_CMP, 4, R(RAX), NO_LOAD);
	uint8_t* none = xJcc(e, CC_E, NULL);
	xLoad(e, 4, R8, P(cpu.ldValue));
	xStore(e, 4, MI(RBX, RAX, 4, (int32_t) offsetof(struct Psx, cpu.r)), R8);
	xStoreI(e, 4, P(cpu.ldReg), NO_LOAD);
	xStoreI(e, 4, GPR(0), 0);
	patchHere(e, none);
}

// commitLoad as the interpreter does it, for whatever load is in flight.
static void bCommitDynamic(struct Compile* k) {
	struct Emitter* e = &k->e;
	xLoad(e, 4, RAX, P(cpu.ldReg));
	xLoad(e, 4, R8, P(cpu.ldValue));
	xStore(e, 4, MI(RBX, RAX, 4, (int32_t) offsetof(struct Psx, cpu.r)), R8);
	xMovzx(e, R8, 1, P(cpu.ldAbsorb));
	xStore(e, 1, ABSORB_AT(RAX), R8);
	xStore(e, 1, P(cpu.readFudge), RAX);
	xAluI(e, X_AND, 4, R(RAX), 0x1F);
	xAluRR(e, X_OR, 1, P(cpu.absorbWhich), RAX);
	xStoreI(e, 4, P(cpu.ldReg), NO_LOAD);
	xStoreI(e, 4, GPR(0), 0);
}

static void bSurchargeIndirect(struct Compile* k) {
	struct Emitter* e = &k->e;
	xMovzx(e, RAX, 1, P(cpu.readFudge));
	xShiftI(e, X_SHR, 4, R(RAX), 4);
	xAluI(e, X_AND, 4, R(RAX), 2);
	xAluRR(e, X_ADD, 8, R(R13), RAX);
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
	xJmp(&k->e, target);
}

static void slowJump(struct Compile* k, int cc) {
	k->slows[k->slowCount++] = xJcc(&k->e, cc, NULL);
}

static void bPatchSlows(struct Compile* k) {
	for (int i = 0; i < k->slowCount; i++) patchHere(&k->e, k->slows[i]);
}

// rax = the cycle count (or when the GTE is done, if that's later, with gte) plus bound, compared
// with the limit.
static void compareBound(struct Emitter* e, int bound, bool gte) {
	if (gte) {
		xLoad(e, 8, RAX, P(cpu.gteDone));
		xAluRR(e, X_CMP, 8, R(RAX), R13);
		xCmov(e, CC_L, 8, RAX, R(R13));
		xLea(e, 8, RAX, M(RAX, bound));
	} else {
		xLea(e, 8, RAX, M(R13, bound));
	}
	xAluRR(e, X_CMP, 8, R(RAX), R14);
}

// On the way in: the block's careful version takes over when its instructions up to the next through
// the interpreter could reach the limit.
static void bDecline(struct Compile* k, int bound, bool gte, uint8_t* careful) {
	compareBound(&k->e, bound, gte);
	xJcc(&k->e, CC_GE, careful);
}

// Before instruction next: the block leaves when the ones up to the next through the interpreter
// could reach the limit.
static void bCheck(struct Compile* k, int bound, bool gte, int next) {
	compareBound(&k->e, bound, gte);
	k->stubSites[next] = xJcc(&k->e, CC_GE, NULL);
}

// Before instruction i, the first of the block's line k: on while the line's generation is the one
// it was last found as compiled at.
static void bLineCheck(struct Compile* k, struct Block* b, int line, int i) {
	struct Emitter* e = &k->e;
	uint32_t pc = b->pc + 4 * i;
	xLoad(e, 4, RAX, P(cpu.icacheGen[(pc >> 4) & 255]));
	xMovI64(e, RCX, (uint64_t) (uintptr_t) &b->gen[line]);
	xAluRM(e, X_CMP, 4, RAX, M(RCX, 0));
	k->lines[k->lineCount].site = xJcc(e, CC_NE, NULL);
	k->lines[k->lineCount].cont = e->p;
	k->lines[k->lineCount].i = i;
	k->lines[k->lineCount].k = line;
	k->lines[k->lineCount].st = k->st;
	k->lineCount++;
}

// The instruction through cpuJitExecute (memory holds the state); the block leaves if it has to.
static void bHelper(struct Compile* k, uint32_t pc, uint32_t instr, bool delay) {
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
	xJmp(e, k->j->leave);
	patchHere(e, stay);
	loadLimit(e);
}

// rdx = a block (memory holds the state): into its clean version when nothing hides any load time and
// no load is in flight, otherwise its generic one.
static void enterVersion(struct Emitter* e, int version) {
	if (version == DYNAMIC) {
		xAluI(e, X_CMP, 4, P(cpu.ldReg), NO_LOAD);
		uint8_t* generic = xJcc(e, CC_NE, NULL);
		xLoad(e, 8, RAX, ABSORB(0));
		xAluRM(e, X_OR, 8, RAX, ABSORB(8));
		xAluRM(e, X_OR, 8, RAX, ABSORB(16));
		xAluRM(e, X_OR, 8, RAX, ABSORB(24));
		uint8_t* generic2 = xJcc(e, CC_NE, NULL);
		xJmpM(e, M(RDX, (int32_t) offsetof(struct Block, body[CLEAN])));
		patchHere(e, generic);
		patchHere(e, generic2);
		version = GENERIC;
	}
	xJmpM(e, M(RDX, (int32_t) offsetof(struct Block, body[version])));
}

// On to the block at pc through the block's link s, while the limit allows and the linked block's
// first line is as it last found it; jitLink otherwise. The state is memory's and next's version.
static void linkTo(struct Compile* k, struct Block* b, int s, uint32_t pc, int version) {
	struct Emitter* e = &k->e;
	xAluRR(e, X_CMP, 8, R(R13), R14);
	xJcc(e, CC_GE, k->j->leave);
	xMovI64(e, RDX, (uint64_t) (uintptr_t) &b->link[s]);
	xLoad(e, 8, RDX, M(RDX, 0));
	xTest(e, 8, R(RDX), RDX);
	uint8_t* none = xJcc(e, CC_E, NULL);
	xLoad(e, 4, RSI, P(cpu.icacheChanges));
	xAluRM(e, X_CMP, 4, RSI, M(RDX, (int32_t) offsetof(struct Block, checked)));
	uint8_t* changed = xJcc(e, CC_NE, NULL);
	uint8_t* go = e->p;
	enterVersion(e, version);
	patchHere(e, changed);
	xLoad(e, 4, RCX, P(cpu.icacheGen[(pc >> 4) & 255]));
	xAluRM(e, X_CMP, 4, RCX, M(RDX, (int32_t) offsetof(struct Block, gen)));
	uint8_t* miss = xJcc(e, CC_NE, NULL);
	xStore(e, 4, M(RDX, (int32_t) offsetof(struct Block, checked)), RSI);
	xJmp(e, go);
	patchHere(e, none);
	patchHere(e, miss);
	xStore(e, 8, P(cycles), R13);
	xMovRR(e, 8, RDI, RBX);
	xMovI64(e, RSI, (uint64_t) (uintptr_t) &b->link[s]);
	movI(e, RDX, pc);
	callC(e, (const void*) jitLink);
	xLoad(e, 8, R13, P(cycles));
	loadLimit(e);
	xJmpR(e, RAX);
}

// The end of the block: on to the next instruction, or where the branch sent the delay slot, in
// version when that's known.
static void bEnd(struct Compile* k, struct Block* b, int version) {
	struct Emitter* e = &k->e;
	if (!b->delay) {
		uint32_t end = b->pc + 4 * b->count;
		xStoreI(e, 4, P(cpu.pc), end);
		xStoreI(e, 4, P(cpu.next), end + 4);
		xStoreI(e, 1, P(cpu.branched), 0);
		linkTo(k, b, 0, end, version);
		return;
	}
	uint32_t pc = b->pc + 4 * (b->count - 2), instr = b->words[b->count - 2];
	int op = instr >> 26;
	xLoad(e, 4, RAX, P(cpu.next));
	xStore(e, 4, P(cpu.pc), RAX);
	xLea(e, 4, RCX, M(RAX, 4));
	xStore(e, 4, P(cpu.next), RCX);
	xStoreI(e, 1, P(cpu.branched), 0);
	if (op == 0x00) {
		xJmp(e, k->j->dispatch);
		return;
	}
	uint32_t after = pc + 8, target = pc + 4 + ((uint32_t) (int32_t) (int16_t) instr << 2);
	if (op == 0x02 || op == 0x03) {
		linkTo(k, b, 0, ((pc + 4) & 0xF0000000) | (instr & 0x3FFFFFF) << 2, version);
	} else if (target == after) {
		linkTo(k, b, 0, target, version);
	} else {
		xAluI(e, X_CMP, 4, R(RAX), (int32_t) target);
		uint8_t* other = xJcc(e, CC_NE, NULL);
		linkTo(k, b, 0, target, version);
		patchHere(e, other);
		linkTo(k, b, 1, after, version);
	}
}

// Leaving before instruction i near the limit (memory holds the state there), and each new line's
// way out when its check fails: memory gets the state, then jitLineMiss says where to go on.
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
		xStoreI(e, 4, P(cpu.pc), pc);
		if (b->delay && i == b->count - 1) {
			xStoreI(e, 1, P(cpu.branched), 1);
		} else {
			xStoreI(e, 4, P(cpu.next), pc + 4);
			xStoreI(e, 1, P(cpu.branched), 0);
		}
		xStore(e, 8, P(cycles), R13);
		xMovRR(e, 8, RDI, RBX);
		xMovI64(e, RSI, (uint64_t) (uintptr_t) b);
		movI(e, RDX, k->lines[l].k);
		movI(e, RCX, i);
		callC(e, (const void*) jitLineMiss);
		xLoad(e, 8, R13, P(cycles));
		loadLimit(e);
		xAluI(e, X_CMP, 8, R(RAX), 1);
		xJcc(e, CC_E, k->lines[l].cont);
		xJmpR(e, RAX);
	}
	for (int i = 1; i < b->count; i++) {
		if (!k->stubSites[i]) continue;
		patchHere(e, k->stubSites[i]);
		xStoreI(e, 4, P(cpu.pc), b->pc + 4 * i);
		if (b->delay && i == b->count - 1) {
			// before the delay slot: cpu.next is the branch's, and the branch has just run
			xStoreI(e, 1, P(cpu.branched), 1);
		} else {
			xStoreI(e, 4, P(cpu.next), b->pc + 4 * i + 4);
			xStoreI(e, 1, P(cpu.branched), 0);
			// to go on from here
			xMovI64(e, RAX, (uint64_t) (uintptr_t) &k->j->resumeBlock);
			xMovI64(e, RCX, (uint64_t) (uintptr_t) b);
			xStore(e, 8, M(RAX, 0), RCX);
			xStoreI(e, 4, M(RAX, (int32_t) (offsetof(struct Jit, resumeIndex) - offsetof(struct Jit, resumeBlock))), i);
			xLoad(e, 4, RCX, P(cpu.icacheChanges));
			xStore(e, 4, M(RAX, (int32_t) (offsetof(struct Jit, resumeChanges) - offsetof(struct Jit, resumeBlock))), RCX);
		}
		xJmp(e, k->j->leave);
	}
	return e->p > e->end ? NULL : e->p;
}

static void flushCode(uint8_t* start, uint8_t* end) {
}

// The instructions.

// rdx = the RAM offset of the address in ecx, from its 1 MB page's entry in the table before the
// block table (rax is scratch); taken at *otherSite when it isn't RAM.
static void ramOffset(struct Compile* k, uint8_t** otherSite) {
	struct Emitter* e = &k->e;
	xMovRR(e, 4, RAX, RCX);
	xShiftI(e, X_SHR, 4, R(RAX), 20);
	xLoad(e, 8, RDX, MI(R12, RAX, 8, -PAGES * 8));
	xAluRR(e, X_ADD, 8, R(RDX), RCX);
	*otherSite = xJcc(e, CC_S, NULL);
}

// The Fast mode: a store to a page recompiled code came from goes the slow way, which throws the code
// out (rdx = the RAM offset).
static void codePageCheck(struct Compile* k) {
	struct Emitter* e = &k->e;
	if (!k->codePages) return;
	xMovRR(e, 4, RAX, RDX);
	xShiftI(e, X_SHR, 4, R(RAX), 12);
	xAluI(e, X_CMP, 1, MI(RBX, RAX, 1, (int32_t) offsetof(struct Psx, codePage)), 0);
	slowJump(k, CC_NE);
}

// Not RAM: edx = the scratchpad offset of the address in ecx, or the slow path. The scratchpad is
// (a & 0x7FFFFC00) == 0x1F800000 below 0xA0000000.
static void scratchOffset(struct Compile* k) {
	struct Emitter* e = &k->e;
	xMovRR(e, 4, RAX, RCX);
	xAluI(e, X_AND, 4, R(RAX), 0x7FFFFC00);
	xAluI(e, X_CMP, 4, R(RAX), 0x1F800000);
	slowJump(k, CC_NE);
	xAluI(e, X_CMP, 4, R(RCX), (int32_t) 0xA0000000);
	slowJump(k, CC_AE);
	xMovRR(e, 4, RDX, RCX);
	xAluI(e, X_AND, 4, R(RDX), 0x3FF);
}

static void address(struct Compile* k, uint32_t instr, int width) {
	struct Emitter* e = &k->e;
	int rs = instr >> 21 & 31;
	int32_t imm = (int16_t) instr;
	loadGpr(e, RCX, rs);
	if (imm) xAluI(e, X_ADD, 4, R(RCX), imm);
	if (width > 1) {
		xTestI(e, 1, R(RCX), width - 1);
		slowJump(k, CC_NE);
	}
}

// The load in flight's register after "if (c->ldReg == rt) c->ldReg = 0".
static int cancelSame(struct Compile* k, int pending, int rt) {
	struct Emitter* e = &k->e;
	if (pending == DYNAMIC) {
		xAluI(e, X_CMP, 4, P(cpu.ldReg), rt);
		uint8_t* other = xJcc(e, CC_NE, NULL);
		xStoreI(e, 4, P(cpu.ldReg), 0);
		patchHere(e, other);
		return DYNAMIC;
	}
	return pending == rt ? 0 : pending;
}

// The scratchpad's way for a memory instruction, from *site with the state as it is now, back to here.
static void coldHere(struct Compile* k, int kind, uint8_t* site, uint32_t instr, int pending, struct State start) {
	k->cold = (struct Cold) { site, k->e.p, kind, pending, instr, start, k->immediate };
}

// A load from [r15 + rdx] (RAM) or [rbx + rdx] (the scratchpad, pass 1), then the load in flight
// as the interpreter leaves it.
static void loadPass(struct Compile* k, uint32_t instr, int pending, int pass) {
	struct Emitter* e = &k->e;
	int op = instr >> 26, rs = instr >> 21 & 31, rt = instr >> 16 & 31;
	int width = op == 0x23 ? 4 : (op == 0x21 || op == 0x25) ? 2 : 1;
	struct Opnd src = pass == 0 ? MI(R15, RDX, 1, 0) : MI(RBX, RDX, 1, 0);
	if (width == 4) xLoad(e, 4, RSI, src);
	else if (op == 0x20 || op == 0x21) xMovsx(e, RSI, width, src);
	else xMovzx(e, RSI, width, src);
	stUses(k, rs);
	stCommit(k, cancelSame(k, pending, rt));
	if (k->immediate) {
		if (k->balanced && pass == 0) xAluI(e, X_ADD, 8, R(R13), 5);
		if (rt) xStore(e, 4, GPR(rt), RSI);
		return;
	}
	int surcharge = stBeginRead(k);
	if (k->fast) {
		if (k->balanced && pass == 0) xAluI(e, X_ADD, 8, R(R13), 5);
	} else if (pass == 0) {
		if (surcharge < 0) bSurchargeIndirect(k);
		xAluI(e, X_ADD, 8, R(R13), 5 + (surcharge > 0 ? surcharge : 0));
		xStoreI(e, 1, P(cpu.ldAbsorb), 5);
	} else {
		xStoreI(e, 1, P(cpu.ldAbsorb), 0);
	}
	xStore(e, 4, P(cpu.ldValue), RSI);
	xStoreI(e, 4, P(cpu.ldReg), rt);
}

// LWC2's read from RAM or the scratchpad, as loadPass.
static void lwc2Pass(struct Compile* k, uint32_t instr, int pending, int pass) {
	struct Emitter* e = &k->e;
	int rs = instr >> 21 & 31;
	xLoad(e, 4, RSI, pass == 0 ? MI(R15, RDX, 1, 0) : MI(RBX, RDX, 1, 0));
	stUses(k, rs);
	stCommit(k, pending);
	int surcharge = stBeginRead(k);
	if (k->fast) {
		if (k->balanced && pass == 0) xAluI(e, X_ADD, 8, R(R13), 4);
	} else if (pass == 0) {
		if (surcharge < 0) bSurchargeIndirect(k);
		xAluI(e, X_ADD, 8, R(R13), 4 + (surcharge > 0 ? surcharge : 0));
		xStoreI(e, 1, P(cpu.ldAbsorb), 4);
	} else {
		xStoreI(e, 1, P(cpu.ldAbsorb), 0);
	}
}

// Loads: the address, RAM's fast path here and the scratchpad's after the block.
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
	xTestI(e, 4, P(cpu.sr), 0x10000);
	slowJump(k, CC_NE);
	uint8_t* otherSite;
	ramOffset(k, &otherSite);
	codePageCheck(k);
	xAluRR(e, X_ADD, 8, R(RDX), R15);
	coldHere(k, COLD_STORE, otherSite, instr, pending, k->st);
	loadGpr(e, RSI, rt);
	stUses(k, rs);
	stUses(k, rt);
	stCommit(k, pending);
	xStore(e, width, M(RDX, 0), RSI);
}

// The scratchpad's way: the address in rdx (or the slow path), then back.
static void bCold(struct Compile* k, struct Cold* c) {
	struct Emitter* e = &k->e;
	patchHere(e, c->site);
	k->st = c->start;
	k->immediate = c->immediate;
	scratchOffset(k);
	if (c->kind == COLD_STORE) xAluRR(e, X_ADD, 8, R(RDX), RBX);
	xAluI(e, X_ADD, 8, R(RDX), (int32_t) offsetof(struct Psx, scratch));
	if (c->kind == COLD_LOAD) loadPass(k, c->instr, c->pending, 1);
	if (c->kind == COLD_LWC2) lwc2Pass(k, c->instr, c->pending, 1);
	k->immediate = false;
	xJmp(e, c->back);
}

// The ALU: operands first (before the load in flight lands), then the commit and the result.
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
			loadGpr(e, RSI, rt);
			if (sa) xShiftI(e, fn == 0 ? X_SHL : fn == 2 ? X_SHR : X_SAR, 4, R(RSI), sa);
			stUses(k, rt); stUses(k, rd);
			break;
		case 0x04: case 0x06: case 0x07:
			loadGpr(e, RSI, rt);
			loadGpr(e, RCX, rs);
			xShiftCl(e, fn == 4 ? X_SHL : fn == 6 ? X_SHR : X_SAR, 4, R(RSI));
			stUses(k, rs); stUses(k, rt); stUses(k, rd);
			break;
		case 0x11: case 0x13:
			// MTHI/MTLO
			loadGpr(e, RSI, rs);
			stUses(k, rs);
			stCommit(k, pending);
			xStore(e, 4, GPR(fn == 0x11 ? 32 : 33), RSI);
			return;
		case 0x10: case 0x12:
			// MFHI/MFLO: the commit, then the wait for a multiply or divide (memory has the state)
			stUses(k, rd);
			stCommit(k, pending);
			if (!k->fast || k->balanced) {
				stFlush(k);
				xStore(e, 8, P(cycles), R13);
				xMovRR(e, 8, RDI, RBX);
				callC(e, (const void*) cpuJitWaitMuldiv);
				xLoad(e, 8, R13, P(cycles));
				stAfterWait(k);
			}
			if (rd) {
				xLoad(e, 4, RAX, GPR(fn == 0x10 ? 32 : 33));
				xStore(e, 4, GPR(rd), RAX);
			}
			return;
		case 0x18: case 0x19:
			// MULT/MULTU: done 7, 10 or 14 cycles on, by the operand's leading bits
			loadGpr(e, RSI, rs);
			loadGpr(e, RCX, rt);
			xMovRR(e, 4, RDI, RSI);
			if (fn == 0x18) {
				xMovRR(e, 4, R8, RSI);
				xShiftI(e, X_SAR, 4, R(R8), 31);
				xAluRR(e, X_XOR, 4, R(RDI), R8);
			}
			xAluI(e, X_OR, 4, R(RDI), 0x400);
			xBsr(e, 4, RDI, R(RDI));
			movI(e, R8, 7);
			xAluI(e, X_CMP, 4, R(RDI), 10);
			{
				uint8_t* skip = xJcc(e, CC_BE, NULL);
				xAluI(e, X_ADD, 4, R(R8), 3);
				xAluI(e, X_CMP, 4, R(RDI), 19);
				uint8_t* skip2 = xJcc(e, CC_BE, NULL);
				xAluI(e, X_ADD, 4, R(R8), 4);
				patchHere(e, skip);
				patchHere(e, skip2);
			}
			xLea(e, 8, RAX, MI(R13, R8, 1, 0));
			xStore(e, 8, P(cpu.muldivDone), RAX);
			xMovRR(e, 4, RAX, RSI);
			xMulWide(e, 4, R(RCX), fn == 0x18);
			xStore(e, 4, GPR(32), RDX);
			xStore(e, 4, GPR(33), RAX);
			stUses(k, rs); stUses(k, rt);
			stCommit(k, pending);
			return;
		case 0x1A: case 0x1B:
			// DIV/DIVU: done 37 cycles on
			loadGpr(e, RSI, rs);
			loadGpr(e, RDX, rt);
			xMovRR(e, 8, RDI, RBX);
			movI(e, RCX, fn == 0x1A);
			callC(e, (const void*) cpuJitDivide);
			xLea(e, 8, RAX, M(R13, 37));
			xStore(e, 8, P(cpu.muldivDone), RAX);
			stUses(k, rs); stUses(k, rt);
			stCommit(k, pending);
			return;
		case 0x2A: case 0x2B:
			loadGpr(e, RAX, rs);
			loadGpr(e, RCX, rt);
			xMovI(e, RSI, 0);
			xAluRR(e, X_CMP, 4, R(RAX), RCX);
			xSetcc(e, fn == 0x2A ? CC_L : CC_B, R(RSI));
			stUses(k, rs); stUses(k, rt); stUses(k, rd);
			break;
		default: {
			// 0x20-0x27
			loadGpr(e, RSI, rs);
			loadGpr(e, RCX, rt);
			static const int alu[8] = { X_ADD, X_ADD, X_SUB, X_SUB, X_AND, X_OR, X_XOR, X_OR };
			xAluRR(e, alu[fn & 7], 4, R(RSI), RCX);
			if (fn == 0x20 || fn == 0x22) slowJump(k, CC_O);
			if (fn == 0x27) xNot(e, 4, R(RSI));
			stUses(k, rs); stUses(k, rt); stUses(k, rd);
			break;
		}
		}
	} else {
		dst = rt;
		switch (op) {
		case 0x08: case 0x09:
			loadGpr(e, RSI, rs);
			xAluI(e, X_ADD, 4, R(RSI), (int32_t) imm);
			if (op == 0x08) slowJump(k, CC_O);
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
		default:
			// LUI
			movI(e, RSI, immu << 16);
			stUses(k, rt);
			goto result;
		}
		stUses(k, rs); stUses(k, rt);
	}
result:
	stCommit(k, pending);
	if (dst) xStore(e, 4, GPR(dst), RSI);
}

// Branches and jumps: the interpreter's order (operands, the commit, the link), then cpu.next = where
// the delay slot goes on to.
static void emitBranch(struct Compile* k, uint32_t pc, uint32_t instr, int pending) {
	struct Emitter* e = &k->e;
	int op = instr >> 26, rs = instr >> 21 & 31, rt = instr >> 16 & 31, rd = instr >> 11 & 31;
	uint32_t after = pc + 8, target = pc + 4 + ((uint32_t) (int32_t) (int16_t) instr << 2);
	uint32_t jump = ((pc + 4) & 0xF0000000) | (instr & 0x3FFFFFF) << 2;
	if (op == 0x02 || op == 0x03) {
		if (op == 0x03) stUses(k, 31);
		stCommit(k, pending);
		if (op == 0x03) xStoreI(e, 4, GPR(31), after);
		xStoreI(e, 4, P(cpu.next), jump);
		return;
	}
	if (op == 0x00) {
		// JR/JALR
		loadGpr(e, RSI, rs);
		stUses(k, rs);
		stUses(k, rd);
		stCommit(k, pending);
		if ((instr & 63) == 0x09 && rd) xStoreI(e, 4, GPR(rd), after);
		xStore(e, 4, P(cpu.next), RSI);
		return;
	}
	loadGpr(e, RSI, rs);
	if (op == 0x04 || op == 0x05) loadGpr(e, RDI, rt);
	stUses(k, rs);
	bool link = op == 0x01 && (rt & 0x1E) == 0x10;
	if (op == 0x04 || op == 0x05) stUses(k, rt);
	if (link) stUses(k, 31);
	stCommit(k, pending);
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

static void waitGte(struct Emitter* e) {
	xLoad(e, 8, RAX, P(cpu.gteDone));
	xAluRR(e, X_CMP, 8, R(R13), RAX);
	xCmov(e, CC_L, 8, R13, R(RAX));
}

// LWC2/SWC2: a word between RAM or the scratchpad and a GTE register, when the GTE is usable. LWC2's
// read is a load's but a cycle shorter (and hides one less) from RAM; both wait for the GTE.
static void emitGteMemory(struct Compile* k, uint32_t instr, int pending) {
	struct Emitter* e = &k->e;
	int op = instr >> 26, rs = instr >> 21 & 31, rt = instr >> 16 & 31;
	xTestI(e, 4, P(cpu.sr), 0x40000000);
	slowJump(k, CC_E);
	address(k, instr, 4);
	if (op == 0x3A) {
		xTestI(e, 4, P(cpu.sr), 0x10000);
		slowJump(k, CC_NE);
	}
	uint8_t* otherSite;
	ramOffset(k, &otherSite);
	if (op == 0x3A) {
		codePageCheck(k);
		xAluRR(e, X_ADD, 8, R(RDX), R15);
		coldHere(k, COLD_STORE, otherSite, instr, pending, k->st);
		stUses(k, rs);
		stCommit(k, pending);
		waitGte(e);
		// the address across the call
		xAluI(e, X_SUB, 8, R(RSP), 16);
		xStore(e, 8, M(RSP, 0), RDX);
		movI(e, RDI, rt);
		callC(e, (const void*) GTE_ReadDR);
		xLoad(e, 8, RDX, M(RSP, 0));
		xAluI(e, X_ADD, 8, R(RSP), 16);
		xStore(e, 4, M(RDX, 0), RAX);
		return;
	}
	struct State start = k->st;
	lwc2Pass(k, instr, pending, 0);
	coldHere(k, COLD_LWC2, otherSite, instr, pending, start);
	waitGte(e);
	movI(e, RDI, rt);
	callC(e, (const void*) GTE_WriteDR);
}

// COP2: the GTE's commands and moves, when the GTE is usable (otherwise the interpreter raises the
// exception). Calls into gte.c keep rbx, rbp and r12-r15.
static void emitGte(struct Compile* k, uint32_t instr, int pending) {
	struct Emitter* e = &k->e;
	int rs = instr >> 21 & 31, rt = instr >> 16 & 31, rd = instr >> 11 & 31;
	xTestI(e, 4, P(cpu.sr), 0x40000000);
	slowJump(k, CC_E);
	if (instr & 0x02000000) {
		stCommit(k, pending);
		xLoad(e, 8, RAX, P(cpu.gteDone));
		xAluRR(e, X_CMP, 8, R(R13), RAX);
		xCmov(e, CC_L, 8, R13, R(RAX));
		movI(e, RDI, instr);
		callC(e, (const void*) GTE_Instruction);
		xMovsxd(e, RAX, R(RAX));
		xAluRR(e, X_ADD, 8, R(RAX), R13);
		xStore(e, 8, P(cpu.gteDone), RAX);
		return;
	}
	if (rs == 0 || rs == 2) {
		stCommit(k, cancelSame(k, pending, rt));
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
		return;
	}
	loadGpr(e, RSI, rt);
	stCommit(k, pending);
	xLoad(e, 8, RAX, P(cpu.gteDone));
	xAluRR(e, X_CMP, 8, R(R13), RAX);
	xCmov(e, CC_L, 8, R13, R(RAX));
	movI(e, RDI, rd);
	callC(e, (const void*) (rs == 4 ? GTE_WriteDR : GTE_WriteCR));
}

// The code every block shares: the way in (registers set, then the block's body), the way out, and
// the dispatcher a block ends with, which goes straight on to the next block while the limit allows
// and the next block's lines are as it last found them, into its clean version when nothing hides
// any load time and no load is in flight.
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
	xLoad(&e, 4, RSI, P(cpu.icacheChanges));
	xAluRM(&e, X_CMP, 4, RSI, M(RDX, (int32_t) offsetof(struct Block, checked)));
	uint8_t* go = xJcc(&e, CC_E, NULL);
	// The cache changed since: the block's first line decides
	xMovRR(&e, 4, RCX, RAX);
	xShiftI(&e, X_SHR, 4, R(RCX), 4);
	xMovzx(&e, RCX, 1, R(RCX));
	xLoad(&e, 4, R8, MI(RBX, RCX, 4, (int32_t) offsetof(struct Psx, cpu.icacheGen)));
	xAluRM(&e, X_CMP, 4, R8, M(RDX, (int32_t) offsetof(struct Block, gen)));
	uint8_t* miss = xJcc(&e, CC_NE, NULL);
	xStore(&e, 4, M(RDX, (int32_t) offsetof(struct Block, checked)), RSI);
	patchHere(&e, go);
	uint8_t* clean = xJmp(&e, NULL);
	// Not as compiled: jitEnterMiss fills it or finishes the instruction
	patchHere(&e, miss);
	xStore(&e, 8, P(cycles), R13);
	xMovRR(&e, 8, RDI, RBX);
	xMovRR(&e, 4, RSI, RAX);
	callC(&e, (const void*) jitEnterMiss);
	xLoad(&e, 8, R13, P(cycles));
	loadLimit(&e);
	xJmpR(&e, RAX);
	patchHere(&e, clean);
	// Clean: no load in flight and every absorb entry 0
	xAluI(&e, X_CMP, 4, P(cpu.ldReg), NO_LOAD);
	uint8_t* generic = xJcc(&e, CC_NE, NULL);
	xLoad(&e, 8, RAX, ABSORB(0));
	xAluRM(&e, X_OR, 8, RAX, ABSORB(8));
	xAluRM(&e, X_OR, 8, RAX, ABSORB(16));
	xAluRM(&e, X_OR, 8, RAX, ABSORB(24));
	uint8_t* generic2 = xJcc(&e, CC_NE, NULL);
	xJmpM(&e, M(RDX, (int32_t) offsetof(struct Block, body[CLEAN])));
	patchHere(&e, generic);
	patchHere(&e, generic2);
	xJmpM(&e, M(RDX, (int32_t) offsetof(struct Block, body[GENERIC])));
	j->used = (size_t) (e.p - j->arena + 15) & ~(size_t) 15;
}
