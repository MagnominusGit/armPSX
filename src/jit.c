/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "psx.h"

// Native code for blocks of instructions the instruction cache holds. A block is compiled from the
// cache's words and runs only while its lines are unchanged, so every fetch in it is a hit and RAM
// writes can't touch it. It does exactly what cpuStep would: the CPU's state is the interpreter's,
// in memory, but what the compiler can tell about it (each register's ReadAbsorb, absorbWhich,
// readFudge, the cycles the fetches take) it keeps to itself and writes back only where something
// reads it. The ALU, branches, the GTE and RAM and scratchpad loads and stores run inline; the rest,
// and every load or store that leaves the fast path, goes through cpuJitExecute.
//
// Every block comes three times: one for when no register hides any load time and no load is in
// flight (most of the time; then all of it is known), one for anything else, and a careful one
// that checks the cycle limit before every instruction. The other two check it once on the way in,
// against the most the block can take before its next instruction through the interpreter, and
// again after each such instruction; near the limit they hand over to the careful one.

enum { PAGES = 4096, ARENA_SIZE = 64 << 20, BLOCK_MAX = 32, BLOCKS_MAX = 1 << 18, TABLE_SIZE = 1 << 19, NO_LOAD = 34, DYNAMIC = -1 };
enum { GENERIC, CLEAN, CAREFUL };
enum { FAST_LOAD_CYCLES = 5 };  // the Fast mode's cycles for a load besides its own: the bus's from RAM

struct Block {
	uint32_t pc;
	int count, lines;
	bool delay;  // it ends with a branch and its delay slot
	bool uncached;  // in the BIOS ROM through KSEG1: no lines, every fetch from the bus
	bool fast;      // the Fast or Balanced mode's: one version, from memory, gone when its RAM is written
	bool balanced;  // (the Balanced mode's)
	bool dead;      // its RAM was written (the Fast mode)
	uint32_t gen[BLOCK_MAX / 4 + 2];
	uint32_t words[BLOCK_MAX];
	uint32_t checked;  // cpu.icacheChanges when its lines were last found unchanged
	uint8_t* body[3];  // GENERIC, CLEAN, CAREFUL
	uint8_t* end[3];
	uint16_t at[3][BLOCK_MAX];  // each instruction's code, from body
	struct Block* link[2];  // the blocks it last went on to: the branch's target (or its end), and its end
};

struct Jit {
	uint8_t* arena;
	size_t used;
	void (*enter)(struct Psx*, int64_t, uint8_t*);
	uint8_t* leave;     // stores the cycle count and returns
	uint8_t* dispatch;  // on to the block at cpu.pc, or out
	int64_t* pages;  // for each 1 MB page, what an address in it adds to make its RAM offset (negative when it isn't RAM); then the block table
	struct Block** table;
	struct Block* blocks;
	int blockCount;
	uint64_t stats[6];  // entries, declined, compiled, stale, uncached, flushes
	int8_t gteCost[64];  // each GTE command's cycles
	// Where a block last left near the limit: it goes on from there in its careful version, so the
	// places it stops at don't each get a block of their own
	struct Block* resumeBlock;
	int32_t resumeIndex;
	uint32_t resumeChanges;
};

// What the compiler knows of the interpreter's state at a point in a block.
struct State {
	int8_t absorb[32];  // each register's ReadAbsorb, or -1 when only memory has it
	int8_t aw;          // absorbWhich, or -1: memory's, with awOr still to be ORed in
	uint8_t awOr;
	bool awDirty;       // aw is known and memory doesn't have it yet
	int8_t fudge;       // readFudge, or -1: memory's
	bool fudgeDirty;
	int cycles;         // cycles the count doesn't have yet
};

struct Compile;

// A memory instruction's way to the scratchpad when its address isn't RAM, compiled after the
// block's end: from site (where the state was start) back to where its RAM path goes on.
enum { COLD_STORE = 1, COLD_LOAD, COLD_LWC2 };
struct Cold {
	uint8_t* site;
	uint8_t* back;
	int kind, pending;
	uint32_t instr;
	struct State start;
	bool immediate;
};

// An instruction's way to the interpreter when its fast path can't do it, compiled after the block's end.
struct Slow {
	uint8_t* sites[BLOCK_MAX * 2];
	int count, i;
	bool after;
	bool commit;  // the interpreter leaves the load in flight that the fast path wrote at once
	struct State before;
	struct Cold cold;
	uint8_t* back;
};
static void stAbsorbStep(struct Compile* k);
static void stUses(struct Compile* k, int reg);
static void stCommit(struct Compile* k, int reg);
static int stBeginRead(struct Compile* k);
static void stFlushCycles(struct Compile* k);
static void stFlush(struct Compile* k);
static void stAfterWait(struct Compile* k);

static bool special(struct Psx* p, uint32_t pc) {
	uint32_t phys = pc & 0x1FFFFFFF;
	return (p->ttyOut && (phys == 0xA0 || phys == 0xB0)) || (p->exe && pc == 0x80030000);
}

static bool fetchable(uint32_t pc) {
	uint32_t phys = pc & 0x1FFFFFFF;
	return !(pc & 3) && pc < 0xA0000000 && (phys < 0x800000 || (phys >= 0x1FC00000 && phys < 0x1FC80000));
}

static bool romUncached(uint32_t pc) {
	return !(pc & 3) && pc >= 0xBFC00000 && pc < 0xBFC80000;
}

static bool cached(struct Cpu* c, uint32_t pc) {
	return c->icache[(pc & 0xFFC) >> 2].tag == pc;
}

static uint32_t cachedWord(struct Cpu* c, uint32_t pc) {
	return c->icache[(pc & 0xFFC) >> 2].data;
}

static int kind(uint32_t instr) {
	int op = instr >> 26, fn = instr & 63;
	if (op == 0 && (fn == 0x08 || fn == 0x09)) return 2;  // JR/JALR
	if (op >= 0x01 && op <= 0x07) return 2;  // branches and jumps
	if (op >= 0x20 && op <= 0x25 && op != 0x22) return 1;  // loads
	return 0;
}

// The register a load leaves in flight after this instruction, or NO_LOAD.
static int leavesLoad(uint32_t instr) {
	int op = instr >> 26, rs = instr >> 21 & 31, rt = instr >> 16 & 31;
	if ((op >= 0x20 && op <= 0x26)) return rt;
	if ((op == 0x10 || op == 0x12) && !(instr & 0x02000000) && (rs == 0 || rs == 2)) return rt;
	return NO_LOAD;
}

// The Fast mode: whether the block's instruction i, a load, can write its register at once, the
// next instruction (in the block) not naming it in either register field.
static bool immediateLoad(struct Block* b, int i) {
	uint32_t instr = b->words[i];
	int op = instr >> 26, rt = instr >> 16 & 31;
	if (!b->fast || op < 0x20 || op > 0x25 || op == 0x22 || i + 1 >= b->count) return false;
	uint32_t next = b->words[i + 1];
	return rt != (int) (next >> 21 & 31) && rt != (int) (next >> 16 & 31);
}

static bool aluInlinable(uint32_t instr) {
	int op = instr >> 26, fn = instr & 63;
	if (op == 0) {
		return fn == 0x00 || fn == 0x02 || fn == 0x03 || fn == 0x04 || fn == 0x06 || fn == 0x07 ||
		       (fn >= 0x10 && fn <= 0x13) || (fn >= 0x18 && fn <= 0x1B) || (fn >= 0x20 && fn <= 0x27) ||
		       fn == 0x2A || fn == 0x2B;
	}
	return op >= 0x08 && op <= 0x0F;
}

static bool gteInlinable(uint32_t instr) {
	int rs = instr >> 21 & 31;
	return (instr & 0x02000000) || rs == 0 || rs == 2 || rs == 4 || rs == 6;
}

// Whether the block's instruction i is compiled (otherwise it goes through the interpreter).
static bool inlinable(struct Block* b, int i) {
	uint32_t instr = b->words[i];
	int op = instr >> 26;
	if (op >= 0x20 && op <= 0x25 && op != 0x22) return true;
	if (op == 0x28 || op == 0x29 || op == 0x2B) return true;
	if (op == 0x12) return gteInlinable(instr);
	if (op == 0x32 || op == 0x3A) return true;
	if (kind(instr) == 2) return b->delay && i == b->count - 2;
	return aluInlinable(instr);
}

// Whether a compiled instruction may leave its fast path for the interpreter.
static bool fallible(uint32_t instr) {
	int op = instr >> 26, fn = instr & 63;
	return (op >= 0x20 && op <= 0x2B) || op == 0x12 || op == 0x32 || op == 0x3A || op == 0x08 ||
	       (op == 0 && (fn == 0x20 || fn == 0x22 || (fn >= 0x10 && fn <= 0x1B)));
}

// The most cycles from instruction s's start to the start of the next instruction through the
// interpreter (whose own check comes after it), or of the last one: each fetch (5 from the ROM
// through KSEG1, where the bus takes 4), each load's (2 more when no load came just before, the
// bus's 3 and 2 more), and each GTE wait, at most the command before it in the block takes. The first GTE wait is the GTE's own (*gte): the check then starts
// from when the GTE is done.
static int boundFrom(struct Jit* j, struct Block* b, const bool* inl, int s, bool* gte) {
	int sum = 0, last = -1;
	*gte = false;
	for (int t = s; t < b->count - 1 && inl[t]; t++) {
		uint32_t instr = b->words[t];
		int op = instr >> 26, fn = instr & 63;
		if (op == 0 && (fn == 0x10 || fn == 0x12)) sum += 37;  // the longest multiply or divide
		sum += (b->uncached ? 5 : 1) + (op >= 0x20 && op <= 0x25 ? 7 : op == 0x32 ? 6 : 0);
		if (op == 0x12 || op == 0x32 || op == 0x3A) {
			if (last >= 0) sum += last;
			else *gte = true;
			if (instr & 0x02000000) last = j->gteCost[instr & 0x3F];
		}
	}
	return sum;
}

static uint8_t* jitEnterMiss(struct Psx* p, uint32_t pc);
static uint8_t* jitLineMiss(struct Psx* p, struct Block* b, int k, int i);
static uint8_t* jitLink(struct Psx* p, struct Block** slot, uint32_t pc);

#if defined(__x86_64__)
#include "jit_x64.h"
#elif defined(__aarch64__)
#include "jit_a64.h"
#endif

// The Fast and Balanced modes' blocks: one version, from memory, no instruction cache or ReadAbsorb
// timing. Both charge a cycle per instruction and 4 more per BIOS fetch; Balanced the bus's time for
// each load and the multiplier's waits as they run, Fast FAST_LOAD_CYCLES per load as compiled.
static bool fastMode(struct Psx* p) {
	return FAST_JIT && p->mode != MODE_EXACT;
}

static void stUnknown(struct State* s) {
	memset(s->absorb, -1, sizeof s->absorb);
	s->aw = -1;
	s->awOr = 0;
	s->awDirty = false;
	s->fudge = -1;
	s->fudgeDirty = false;
	s->cycles = 0;
}

static bool allZero(const struct State* s) {
	for (int i = 0; i < 32; i++) {
		if (s->absorb[i]) return false;
	}
	return true;
}

// The fetch's cycle, unless the register absorbWhich names still hides some of a load's wait.
static void stAbsorbStep(struct Compile* k) {
	struct State* s = &k->st;
	if (k->fast) {
		s->cycles++;
		return;
	}
	if (s->aw >= 0) {
		int v = s->absorb[s->aw];
		if (v == 0) {
			s->cycles++;
		} else if (v > 0) {
			s->absorb[s->aw] = v - 1;
			bStoreAbsorb(k, s->aw, v - 1);
		} else {
			bAbsorbStepAt(k, s->aw);
		}
	} else if (allZero(s)) {
		s->cycles++;
	} else {
		bAbsorbStepIndirect(k, s->awOr);
	}
}

static void stUses(struct Compile* k, int reg) {
	struct State* s = &k->st;
	if (k->fast) return;
	if (reg && s->absorb[reg]) {
		bStoreAbsorb(k, reg, 0);
		s->absorb[reg] = 0;
	}
}

// commitLoad for the load the instruction before left in flight: reg is its register, NO_LOAD for
// none, DYNAMIC when it isn't known (the generic block's first instruction).
static void stCommit(struct Compile* k, int reg) {
	struct State* s = &k->st;
	if (k->fast) {
		if (reg == DYNAMIC) bCommitDynamicFast(k);
		else if (reg != NO_LOAD) bCommitLoadFast(k, reg);
		return;
	}
	if (reg == DYNAMIC) {
		stFlush(k);
		bCommitDynamic(k);
		int cycles = s->cycles;
		stUnknown(s);
		s->cycles = cycles;
		return;
	}
	if (reg != NO_LOAD) {
		bCommitLoad(k, reg);
		s->absorb[reg] = -1;
	}
	if (s->fudge != reg) {
		s->fudge = reg;
		s->fudgeDirty = true;
	}
	int bits = reg & 0x1F;
	if (s->aw >= 0) {
		if ((s->aw | bits) != s->aw) {
			s->aw |= bits;
			s->awDirty = true;
		}
	} else {
		s->awOr |= bits;
	}
}

// A read's start: the register absorbWhich names hides nothing more, and absorbWhich is 0. Returns
// the read's extra 2 cycles when no load came just before (0 or 2), or -1 when that's memory's.
static int stBeginRead(struct Compile* k) {
	struct State* s = &k->st;
	if (k->fast) return 0;
	if (s->aw >= 0) {
		if (s->absorb[s->aw]) {
			bStoreAbsorb(k, s->aw, 0);
			s->absorb[s->aw] = 0;
		}
	} else if (!allZero(s)) {
		bZeroAbsorbIndirect(k, s->awOr);
	}
	s->aw = 0;
	s->awOr = 0;
	s->awDirty = true;
	return s->fudge >= 0 ? (s->fudge >> 4) & 2 : -1;
}

// An uncached fetch: the register absorbWhich names hides nothing more, absorbWhich is 0, and the
// bus takes 4 cycles.
static void stUncachedFetch(struct Compile* k) {
	struct State* s = &k->st;
	if (k->fast) {
		s->cycles += 4;
		return;
	}
	if (s->aw >= 0) {
		if (s->absorb[s->aw]) {
			bStoreAbsorb(k, s->aw, 0);
			s->absorb[s->aw] = 0;
		}
	} else if (!allZero(s)) {
		bZeroAbsorbIndirect(k, s->awOr);
	}
	if (s->aw) {
		s->aw = 0;
		s->awOr = 0;
		s->awDirty = true;
	}
	s->cycles += 4;
}

// After MFHI/MFLO's wait, which takes from the absorb absorbWhich names while it waits.
static void stAfterWait(struct Compile* k) {
	struct State* s = &k->st;
	for (int i = 0; i < 32; i++) {
		if (s->absorb[i] > 0) s->absorb[i] = -1;
	}
}

static void stFlushCycles(struct Compile* k) {
	if (k->st.cycles) {
		bAddCycles(k, k->st.cycles);
		k->st.cycles = 0;
	}
}

// Memory gets everything the compiler kept to itself.
static void stFlush(struct Compile* k) {
	struct State* s = &k->st;
	stFlushCycles(k);
	if (k->fast) return;
	if (s->aw >= 0 && s->awDirty) {
		bStoreAw(k, s->aw);
		s->awDirty = false;
	} else if (s->aw < 0 && s->awOr) {
		bOrAw(k, s->awOr);
		s->awOr = 0;
	}
	if (s->fudgeDirty) {
		bStoreFudge(k, s->fudge);
		s->fudgeDirty = false;
	}
}

// One version of a block's code, from start; NULL when it doesn't fit before end. The careful one
// is compiled first: the others hand over to it near the limit.
static uint8_t* emitBlock(struct Jit* j, struct Block* b, int version, uint8_t* start, uint8_t* end) {
	struct Compile k;
	bBegin(&k, j, start, end);
	k.fast = b->fast;
	k.balanced = b->balanced;
	k.codePages = b->fast || b->uncached;
	bool inl[BLOCK_MAX];
	for (int i = 0; i < b->count; i++) {
		inl[i] = inlinable(b, i);
	}
	stUnknown(&k.st);
	if (version == CLEAN) {
		memset(k.st.absorb, 0, sizeof k.st.absorb);
	}
	if (version != CAREFUL && !b->fast) {
		bool gte;
		int bound = boundFrom(j, b, inl, 0, &gte);
		bDecline(&k, bound, gte, b->body[CAREFUL]);
	}
	int pending = version == CLEAN ? NO_LOAD : DYNAMIC;
	struct Slow slow[BLOCK_MAX];
	int slowCount = 0;
	for (int i = 0; i < b->count; i++) {
		uint32_t a = b->pc + 4 * i, instr = b->words[i];
		bool delay = b->delay && i == b->count - 1;
		if (version == CAREFUL && i && !b->fast) {
			// Memory has everything before each instruction, and the code assumes nothing: a line
			// that missed goes on here, after its fill
			stFlush(&k);
			stUnknown(&k.st);
			bCheck(&k, 0, false, i);
		}
		if (i && !(a & 0xF) && !b->uncached && !b->fast) {
			// A new cache line: on while it's as last checked
			stFlushCycles(&k);
			bLineCheck(&k, b, (a >> 4) - (b->pc >> 4), i);
		}
		b->at[version][i] = (uint16_t) (bHere(&k) - start);
		if (b->uncached) stUncachedFetch(&k);
		stAbsorbStep(&k);
		if (k.fast && !k.balanced && (((instr >> 26) >= 0x20 && (instr >> 26) <= 0x26) || (instr >> 26) == 0x32)) {
			k.st.cycles += FAST_LOAD_CYCLES;
		}
		// (The Fast mode's blocks run to their end; the limit is the dispatcher's.)
		bool after = version != CAREFUL && i + 1 < b->count && !delay && !b->fast;
		if (!inl[i]) {
			stFlush(&k);
			bHelper(&k, a, instr, delay);
			stUnknown(&k.st);
			if (after) {
				bool gte;
				int bound = boundFrom(j, b, inl, i + 1, &gte);
				bCheck(&k, bound, gte, i + 1);
			}
		} else {
			if (fallible(instr)) stFlushCycles(&k);
			struct State before = k.st;
			k.slowCount = 0;
			k.cold.site = NULL;
			int op = instr >> 26;
			k.immediate = immediateLoad(b, i);
			if (op >= 0x20 && op <= 0x25) emitLoad(&k, instr, pending);
			else if (op == 0x32 || op == 0x3A) emitGteMemory(&k, instr, pending);
			else if (op >= 0x28) emitStore(&k, instr, pending);
			else if (op == 0x12) emitGte(&k, instr, pending);
			else if (kind(instr) == 2) emitBranch(&k, a, instr, pending);
			else emitAlu(&k, instr, pending);
			if (k.slowCount || k.cold.site) {
				// The scratchpad and slow paths, after the block's end
				struct Slow* d = &slow[slowCount++];
				d->cold = k.cold;
				memcpy(d->sites, k.slows, k.slowCount * sizeof *k.slows);
				d->count = k.slowCount;
				d->before = before;
				d->i = i;
				d->after = after;
				d->back = bHere(&k);
				d->commit = k.immediate;
			}
		}
		pending = k.immediate ? NO_LOAD : leavesLoad(instr);
		k.immediate = false;
	}
	stFlush(&k);
	struct State atEnd = k.st;
	// The next block's version, when the state here decides it
	int next = b->fast || pending != NO_LOAD ? GENERIC : allZero(&k.st) ? CLEAN : DYNAMIC;
	for (int r = 0; r < 32 && next == DYNAMIC; r++) {
		if (k.st.absorb[r] > 0) next = GENERIC;
	}
	bEnd(&k, b, next);
	for (int s = 0; s < slowCount; s++) {
		// Memory as it stood before the instruction, then the interpreter, which leaves memory as
		// the fast path's state has it; then back after the fast path
		struct Slow* d = &slow[s];
		int i = d->i;
		memcpy(k.slows, d->sites, d->count * sizeof *k.slows);
		k.slowCount = d->count;
		if (d->cold.site) bCold(&k, &d->cold);
		bPatchSlows(&k);
		k.st = d->before;
		stFlush(&k);
		bHelper(&k, b->pc + 4 * i, b->words[i], b->delay && i == b->count - 1);
		if (d->commit) bCommitDynamicFast(&k);
		if (d->after) {
			bool gte;
			int bound = boundFrom(j, b, inl, i + 1, &gte);
			bCheck(&k, bound, gte, i + 1);
		}
		bJumpTo(&k, d->back);
	}
	k.st = atEnd;
	return bFinish(&k, b);
}

// What a fetch at a runs: the cache's word when it holds it, otherwise memory's, which a fill brings in.
static uint32_t fetchWord(struct Psx* p, uint32_t a) {
	if (!romUncached(a) && cached(&p->cpu, a) && !fastMode(p)) return cachedWord(&p->cpu, a);
	uint32_t phys = a & 0x1FFFFFFF, w;
	memcpy(&w, phys < 0x800000 ? p->ram + (phys & (RAM_SIZE - 1)) : p->bios + (phys & (BIOS_SIZE - 1)), 4);
	return w;
}

// Whether the cache holds the block's words in its line k.
static bool lineHeld(struct Cpu* c, struct Block* b, int k) {
	if (b->dead) return false;
	if (b->uncached || b->fast) return true;
	int first = k ? 4 * k - (int) ((b->pc >> 2) & 3) : 0;
	int last = 4 * (k + 1) - (int) ((b->pc >> 2) & 3);
	if (last > b->count) last = b->count;
	for (int w = first; w < last; w++) {
		uint32_t a = b->pc + 4 * w;
		if (!cached(c, a) || cachedWord(c, a) != b->words[w]) return false;
	}
	return true;
}

// Whether the block's line k is as compiled (its generation brought up to date when it is).
static bool lineCurrent(struct Cpu* c, struct Block* b, int k) {
	uint32_t gen = c->icacheGen[((b->pc >> 4) + k) & 255];
	if (gen == b->gen[k]) return true;
	if (!lineHeld(c, b, k)) return false;
	b->gen[k] = gen;
	return true;
}

// A block compiled into the arena; NULL when it doesn't fit and flushing isn't allowed (the
// recompiled code is running).
static struct Block* compileBlock(struct Psx* p, struct Jit* j, uint32_t pc, bool mayFlush) {
	struct Cpu* c = &p->cpu;
	if (j->blockCount == BLOCKS_MAX || j->used + 64 * 1024 > ARENA_SIZE) {
		if (!mayFlush) return NULL;
		jitFlush(p);
	}
	struct Block* b = &j->blocks[j->blockCount];
	b->pc = pc;
	b->count = 0;
	b->delay = false;
	b->link[0] = b->link[1] = NULL;
	b->uncached = romUncached(pc);
	// (the Fast mode runs the BIOS ROM's code on Exact's timing: its CD loader needs it)
	b->fast = fastMode(p) && !(b->uncached && p->mode == MODE_FAST);
	b->balanced = b->fast && p->mode == MODE_BALANCED;
	b->dead = false;
	for (uint32_t a = pc; b->count < BLOCK_MAX - 1; a += 4) {
		if (!(b->uncached ? romUncached(a) : fetchable(a)) || (a != pc && special(p, a))) break;
		uint32_t instr = fetchWord(p, a);
		b->words[b->count++] = instr;
		if (kind(instr) == 2) {
			// The delay slot comes along unless it's a branch too.
			uint32_t d = a + 4;
			if ((b->uncached ? romUncached(d) : fetchable(d)) && !special(p, d) && kind(fetchWord(p, d)) != 2) {
				b->words[b->count++] = fetchWord(p, d);
				b->delay = true;
			}
			break;
		}
	}
	if (!b->count) return NULL;
	b->lines = ((pc + 4 * b->count - 4) >> 4) - (pc >> 4) + 1;
	for (int i = 0; i < b->lines; i++) {
		// A line the cache doesn't hold as the block has it yet gets a generation that can't match
		b->gen[i] = c->icacheGen[((pc >> 4) + i) & 255] - !lineHeld(c, b, i);
	}

	static const int order[3] = { CAREFUL, GENERIC, CLEAN };
	for (int o = b->fast ? 1 : 0; o < (b->fast ? 2 : 3); o++) {
		int version = order[o];
		uint8_t* start = j->arena + j->used;
		uint8_t* end = emitBlock(j, b, version, start, j->arena + ARENA_SIZE);
		if (!end) {
			if (mayFlush) jitFlush(p);
			return NULL;
		}
		flushCode(start, end);
		j->used = (size_t) (end - j->arena + 15) & ~(size_t) 15;
		b->body[version] = start;
		b->end[version] = end;
	}
	if (b->fast) {
		// One version for all three; its RAM pages say when it's written
		for (int v = 0; v < 3; v++) {
			b->body[v] = b->body[GENERIC];
			b->end[v] = b->end[GENERIC];
			memcpy(b->at[v], b->at[GENERIC], sizeof b->at[v]);
		}
		uint32_t phys = pc & 0x1FFFFFFF;
		if (phys < 0x800000) {
			p->codePage[(phys & (RAM_SIZE - 1)) >> 12] = 1;
			p->codePage[((phys + 4 * b->count - 4) & (RAM_SIZE - 1)) >> 12] = 1;
		}
	}
	// Checked only when the cache holds its first line as compiled
	b->checked = c->icacheChanges - !lineHeld(c, b, 0);
	j->blockCount++;
	return b;
}

static uint8_t* enterAt(struct Psx* p, struct Jit* j, uint32_t pc);

// The dispatcher's way into a block whose first line changed: see enterAt.
static uint8_t* jitEnterMiss(struct Psx* p, uint32_t pc) {
	return enterAt(p, p->jit, pc);
}

// A block's line k, from its instruction i on, isn't as its last check found it (memory holds the
// state, the limit check passed): on (1) when the cache still holds what the block was compiled
// from; otherwise instruction i's fetch fills the line as the interpreter's would, and the block goes
// on from there in its careful version, or the interpreter finishes the instruction when the cache
// now holds other code.
static uint8_t* jitLineMiss(struct Psx* p, struct Block* b, int k, int i) {
	struct Cpu* c = &p->cpu;
	struct Jit* j = p->jit;
	if (lineCurrent(c, b, k)) return (uint8_t*) 1;
	uint32_t pc = b->pc + 4 * i;
	if (!cached(c, pc)) cpuFetch(p, pc);
	if (lineCurrent(c, b, k)) return b->body[CAREFUL] + b->at[CAREFUL][i];
	cpuStep(p);
	return cpuIrqPending(p) ? j->leave : j->dispatch;
}

// A block's end found its link to the block at pc missing or out of date (memory holds the state,
// the limit check passed): as the dispatcher would go on, and the link is the table's block.
static uint8_t* jitLink(struct Psx* p, struct Block** slot, uint32_t pc) {
	struct Jit* j = p->jit;
	struct Block* t = j->table[(pc >> 2) & (TABLE_SIZE - 1)];
	if ((!t || t->pc != pc) && (!(fetchable(pc) || romUncached(pc)) || special(p, pc))) return j->leave;
	uint8_t* to = enterAt(p, j, pc);
	t = j->table[(pc >> 2) & (TABLE_SIZE - 1)];
	if (t && t->pc == pc) *slot = t;
	return to;
}

static bool cleanState(struct Cpu* c) {
	uint64_t absorb[4];
	memcpy(absorb, c->absorb, sizeof absorb);
	return c->ldReg == NO_LOAD && !(absorb[0] | absorb[1] | absorb[2] | absorb[3]);
}

// Where the recompiled code goes on at the start of the instruction at pc (memory holds the state,
// the limit check passed): the instruction's fetch fills its line if it misses, as the interpreter's
// would; then the block from pc runs if the cache holds its first line as compiled, or else the
// interpreter finishes the instruction (its fetch now hits) and the dispatcher goes on. Nothing
// else runs between the fetch and the instruction. A delay slot is the interpreter's.
static uint8_t* enterAt(struct Psx* p, struct Jit* j, uint32_t pc) {
	struct Cpu* c = &p->cpu;
	if (!c->branched) {
		if (!romUncached(pc) && !cached(c, pc) && !fastMode(p)) cpuFetch(p, pc);
		struct Block** slot = &j->table[(pc >> 2) & (TABLE_SIZE - 1)];
		struct Block* b = *slot;
		if (!b || b->pc != pc || (b->checked != c->icacheChanges && !lineCurrent(c, b, 0))) {
			struct Block* n = compileBlock(p, j, pc, false);
			if (n) {
				*slot = b = n;
			} else if (fastMode(p) && (j->blockCount == BLOCKS_MAX || j->used + 64 * 1024 > ARENA_SIZE)) {
				// The arena is full: on through the dispatcher, which empties it, rather than the
				// interpreter, whose timing isn't the Fast and Balanced modes'
				return j->leave;
			}
		}
		if (b && b->pc == pc && (b->checked == c->icacheChanges || lineCurrent(c, b, 0))) {
			b->checked = c->icacheChanges;
			return b->body[cleanState(c)];
		}
	}
	cpuStep(p);
	return cpuIrqPending(p) ? j->leave : j->dispatch;
}

bool jitCreate(struct Psx* p) {
	struct Jit* j = calloc(1, sizeof *j);
	if (!j) return false;
	j->arena = mmap(NULL, ARENA_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	j->pages = calloc(PAGES + TABLE_SIZE, sizeof *j->pages);
	j->table = j->pages ? (struct Block**) (j->pages + PAGES) : NULL;
	// RAM: KUSEG, KSEG0 or KSEG1 below 8 MB, mirrored every 2 MB
	for (int i = 0; j->pages && i < PAGES; i++) {
		int seg = i >> 9;
		bool ram = (seg == 0 || seg == 4 || seg == 5) && (i & 0x1FF) < 8;
		j->pages[i] = ram ? ((int64_t) (i & 1) << 20) - ((int64_t) i << 20) : -((int64_t) 1 << 40);
	}
	j->blocks = calloc(BLOCKS_MAX, sizeof *j->blocks);
	// The GTE's commands take a fixed time each; finding it out here leaves the GTE as psxReset
	// starts it anyway.
	for (int i = 0; i < 64; i++) {
		j->gteCost[i] = (int8_t) GTE_Instruction(0x4A000000 | i);
	}
	GTE_Power();
	if (j->arena != MAP_FAILED && j->table && j->blocks) {
		emitShared(p, j);
		flushCode(j->arena, j->arena + j->used);
	}
	if (j->arena == MAP_FAILED || !j->table || !j->blocks) {
		if (j->arena != MAP_FAILED) munmap(j->arena, ARENA_SIZE);
		free(j->pages);
		free(j->blocks);
		free(j);
		return false;
	}
	p->jit = j;
	return true;
}

void jitFree(struct Psx* p) {
	struct Jit* j = p->jit;
	if (!j) return;
	munmap(j->arena, ARENA_SIZE);
	free(j->pages);
	free(j->blocks);
	free(j);
	p->jit = NULL;
}

void jitFlush(struct Psx* p) {
	struct Jit* j = p->jit;
	if (!j) return;
	memset(j->table, 0, TABLE_SIZE * sizeof *j->table);
	memset(p->codePage, 0, sizeof p->codePage);
	j->blockCount = 0;
	j->resumeBlock = NULL;
	j->stats[5]++;
	emitShared(p, j);
	flushCode(j->arena, j->arena + j->used);
}

bool jitRun(struct Psx* p, int64_t until) {
	struct Jit* j = p->jit;
	struct Cpu* c = &p->cpu;
	uint32_t pc = c->pc;
	if (p->watch || p->pcRing || !(c->biu & 0x800) || !(fetchable(pc) || romUncached(pc)) || special(p, pc)) {
		return false;
	}
	struct Block* r = j->resumeBlock;
	j->resumeBlock = NULL;
	// (its line's check comes after the limit's, so the line still has to be as compiled)
	if (r && pc == r->pc + 4 * (uint32_t) j->resumeIndex && c->icacheChanges == j->resumeChanges &&
	    lineCurrent(c, r, (pc >> 4) - (r->pc >> 4))) {
		j->stats[0]++;
		j->enter(p, until, r->body[CAREFUL] + r->at[CAREFUL][j->resumeIndex]);
		return true;
	}
	struct Block** slot = &j->table[(pc >> 2) & (TABLE_SIZE - 1)];
	struct Block* b = *slot;
	if (!b || b->pc != pc) {
		b = compileBlock(p, j, pc, true);
		if (!b) return false;
		*slot = b;
		j->stats[2]++;
	}
	j->stats[0]++;
	if (b->checked == c->icacheChanges || lineCurrent(c, b, 0)) {
		b->checked = c->icacheChanges;
		j->enter(p, until, b->body[cleanState(c)]);
	} else {
		// The first line isn't as compiled: a fill, or new code
		j->stats[3]++;
		uint8_t* to = enterAt(p, j, pc);
		if (to != j->dispatch && to != j->leave) j->enter(p, until, to);
	}
	return true;
}

// For profiling: the guest instruction whose code holds address and its block's version (GENERIC,
// CLEAN or CAREFUL); 0 when it isn't recompiled code, 1 for the shared code.
uint32_t jitDescribe(struct Psx* p, const void* address, uint32_t* instr, int* version) {
	struct Jit* j = p->jit;
	const uint8_t* a = address;
	if (!j || a < j->arena || a >= j->arena + ARENA_SIZE) return 0;
	for (int i = 0; i < j->blockCount; i++) {
		struct Block* b = &j->blocks[i];
		for (int v = 0; v < 3; v++) {
			if (a >= b->body[v] && a < b->end[v]) {
				int w = 0;
				while (w + 1 < b->count && a >= b->body[v] + b->at[v][w + 1]) w++;
				*instr = b->words[w];
				*version = v;
				return b->pc + 4 * w;
			}
		}
	}
	*instr = 0;
	return 1;
}

// The Fast mode: RAM at offset was written, and its 4 KB page holds recompiled code. The blocks in it
// (and those running into it from the page before) are gone: their table slots are cleared, and
// their links and the dispatcher find them out of date.
void jitInvalidate(struct Psx* p, uint32_t offset) {
	struct Jit* j = p->jit;
	uint32_t page = (offset & (RAM_SIZE - 1)) >> 12;
	p->codePage[page] = 0;
	if (!j) return;
	uint32_t first = page << 10, from = first >= BLOCK_MAX ? first - BLOCK_MAX : 0;
	for (uint32_t w = from; w < first + 1024; w++) {
		struct Block** slot = &j->table[w & (TABLE_SIZE - 1)];
		struct Block* b = *slot;
		if (!b || !b->fast || b->uncached || (b->pc & 0x1FFFFFFF) >= 0x800000) continue;
		uint32_t start = (b->pc & (RAM_SIZE - 1)) >> 2;
		if (start + (uint32_t) b->count <= first || start >= first + 1024) continue;
		b->dead = true;
		b->checked = p->cpu.icacheChanges - 1;
		b->gen[0] ^= 0x80000000;
		*slot = NULL;
	}
	if (j->resumeBlock && j->resumeBlock->dead) j->resumeBlock = NULL;
}

// For psxrun: the recompiled code (path.bin) and where each block's versions are in it (path.txt).
void jitDump(struct Psx* p, const char* path) {
	struct Jit* j = p->jit;
	if (!j) return;
	char name[512];
	snprintf(name, sizeof name, "%s.bin", path);
	FILE* f = fopen(name, "wb");
	if (!f) return;
	fwrite(j->arena, 1, j->used, f);
	fclose(f);
	snprintf(name, sizeof name, "%s.txt", path);
	f = fopen(name, "w");
	if (!f) return;
	for (int i = 0; i < j->blockCount; i++) {
		struct Block* b = &j->blocks[i];
		for (int v = 0; v < 3; v++) {
			fprintf(f, "%08x %d %d %lx %lx\n", b->pc, b->count, v, (unsigned long) (b->body[v] - j->arena),
			        (unsigned long) (b->end[v] - j->arena));
		}
	}
	fclose(f);
}

// For psxrun: what the recompiler did.
void jitStats(struct Psx* p, uint64_t* out) {
	struct Jit* j = p->jit;
	for (int i = 0; i < 6; i++) out[i] = j ? j->stats[i] : 0;
}
