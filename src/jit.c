/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stddef.h>
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

enum { ARENA_SIZE = 64 << 20, BLOCK_MAX = 32, BLOCKS_MAX = 1 << 18, TABLE_SIZE = 1 << 19, NO_LOAD = 34, DYNAMIC = -1 };
enum { GENERIC, CLEAN, CAREFUL };

struct Block {
	uint32_t pc;
	int count, lines;
	bool delay;  // it ends with a branch and its delay slot
	uint32_t gen[BLOCK_MAX / 4 + 2];
	uint32_t words[BLOCK_MAX];
	uint32_t checked;  // cpu.icacheChanges when its lines were last found unchanged
	uint8_t* body[3];  // GENERIC, CLEAN, CAREFUL
	uint8_t* end[3];
	uint16_t at[3][BLOCK_MAX];  // each instruction's code, from body
};

struct Jit {
	uint8_t* arena;
	size_t used;
	void (*enter)(struct Psx*, int64_t, uint8_t*);
	uint8_t* leave;     // stores the cycle count and returns
	uint8_t* dispatch;  // on to the block at cpu.pc, or out
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
static void stAbsorbStep(struct Compile* k);
static void stUses(struct Compile* k, int reg);
static void stCommit(struct Compile* k, int reg);
static int stBeginRead(struct Compile* k);
static void stFlushCycles(struct Compile* k);
static void stFlush(struct Compile* k);

static bool special(struct Psx* p, uint32_t pc) {
	uint32_t phys = pc & 0x1FFFFFFF;
	return phys == 0xA0 || phys == 0xB0 || (p->exe && pc == 0x80030000);
}

static bool fetchable(uint32_t pc) {
	uint32_t phys = pc & 0x1FFFFFFF;
	return !(pc & 3) && pc < 0xA0000000 && (phys < 0x800000 || (phys >= 0x1FC00000 && phys < 0x1FC80000));
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

static bool aluInlinable(uint32_t instr) {
	int op = instr >> 26, fn = instr & 63;
	if (op == 0) {
		return fn == 0x00 || fn == 0x02 || fn == 0x03 || fn == 0x04 || fn == 0x06 || fn == 0x07 || fn == 0x11 ||
		       fn == 0x13 || (fn >= 0x20 && fn <= 0x27) || fn == 0x2A || fn == 0x2B;
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
	if (kind(instr) == 2) return b->delay && i == b->count - 2;
	return aluInlinable(instr);
}

// Whether a compiled instruction may leave its fast path for the interpreter.
static bool fallible(uint32_t instr) {
	int op = instr >> 26, fn = instr & 63;
	return (op >= 0x20 && op <= 0x2B) || op == 0x12 || op == 0x08 || (op == 0 && (fn == 0x20 || fn == 0x22));
}

// The most cycles from instruction s's start to the start of the next instruction through the
// interpreter (whose own check comes after it), or of the last one: each fetch, each load's (2 more
// when no load came just before, the bus's 3 and 2 more), and each GTE wait, at most the command
// before it in the block takes. The first GTE wait is the GTE's own (*gte): the check then starts
// from when the GTE is done.
static int boundFrom(struct Jit* j, struct Block* b, const bool* inl, int s, bool* gte) {
	int sum = 0, last = -1;
	*gte = false;
	for (int t = s; t < b->count - 1 && inl[t]; t++) {
		uint32_t instr = b->words[t];
		int op = instr >> 26;
		sum += 1 + (op >= 0x20 && op <= 0x25 ? 7 : 0);
		if (op == 0x12) {
			if (last >= 0) sum += last;
			else *gte = true;
			if (instr & 0x02000000) last = j->gteCost[instr & 0x3F];
		}
	}
	return sum;
}

static uint8_t* jitEnterMiss(struct Psx* p, uint32_t pc);
static uint8_t* jitLineMiss(struct Psx* p, struct Block* b, int k, int i);

#if defined(__x86_64__)
#include "jit_x64.h"
#elif defined(__aarch64__)
#include "jit_a64.h"
#endif

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
	if (reg && s->absorb[reg]) {
		bStoreAbsorb(k, reg, 0);
		s->absorb[reg] = 0;
	}
}

// commitLoad for the load the instruction before left in flight: reg is its register, NO_LOAD for
// none, DYNAMIC when it isn't known (the generic block's first instruction).
static void stCommit(struct Compile* k, int reg) {
	struct State* s = &k->st;
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
	bool inl[BLOCK_MAX];
	for (int i = 0; i < b->count; i++) {
		inl[i] = inlinable(b, i);
	}
	stUnknown(&k.st);
	if (version == CLEAN) {
		memset(k.st.absorb, 0, sizeof k.st.absorb);
	}
	if (version != CAREFUL) {
		bool gte;
		int bound = boundFrom(j, b, inl, 0, &gte);
		bDecline(&k, bound, gte, b->body[CAREFUL]);
	}
	int pending = version == CLEAN ? NO_LOAD : DYNAMIC;
	for (int i = 0; i < b->count; i++) {
		uint32_t a = b->pc + 4 * i, instr = b->words[i];
		bool delay = b->delay && i == b->count - 1;
		if (version == CAREFUL && i) {
			// Memory has everything before each instruction, and the code assumes nothing: a line
			// that missed goes on here, after its fill
			stFlush(&k);
			stUnknown(&k.st);
			bCheck(&k, 0, false, i);
		}
		if (i && !(a & 0xF)) {
			// A new cache line: on while it's as last checked
			stFlushCycles(&k);
			bLineCheck(&k, b, (a >> 4) - (b->pc >> 4), i);
		}
		b->at[version][i] = (uint16_t) (bHere(&k) - start);
		stAbsorbStep(&k);
		bool after = version != CAREFUL && i + 1 < b->count && !delay;
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
			int op = instr >> 26;
			if (op >= 0x20 && op <= 0x25) emitLoad(&k, instr, pending);
			else if (op >= 0x28) emitStore(&k, instr, pending);
			else if (op == 0x12) emitGte(&k, instr, pending);
			else if (kind(instr) == 2) emitBranch(&k, a, instr, pending);
			else emitAlu(&k, instr, pending);
			if (k.slowCount) {
				// The slow path: memory as it stood before the instruction, then the interpreter,
				// which leaves memory as the fast path's state has it
				uint8_t* over = bJump(&k);
				bPatchSlows(&k);
				struct State fast = k.st;
				k.st = before;
				stFlush(&k);
				k.st = fast;
				bHelper(&k, a, instr, delay);
				if (after) {
					bool gte;
					int bound = boundFrom(j, b, inl, i + 1, &gte);
					bCheck(&k, bound, gte, i + 1);
				}
				bPatchHere(&k, over);
			}
		}
		pending = leavesLoad(instr);
	}
	stFlush(&k);
	bEnd(&k, b);
	return bFinish(&k, b);
}

// What a fetch at a runs: the cache's word when it holds it, otherwise memory's, which a fill brings in.
static uint32_t fetchWord(struct Psx* p, uint32_t a) {
	if (cached(&p->cpu, a)) return cachedWord(&p->cpu, a);
	uint32_t phys = a & 0x1FFFFFFF, w;
	memcpy(&w, phys < 0x800000 ? p->ram + (phys & (RAM_SIZE - 1)) : p->bios + (phys & (BIOS_SIZE - 1)), 4);
	return w;
}

// Whether the cache holds the block's words in its line k.
static bool lineHeld(struct Cpu* c, struct Block* b, int k) {
	for (int w = 0; w < b->count; w++) {
		uint32_t a = b->pc + 4 * w;
		if ((a >> 4) - (b->pc >> 4) != (uint32_t) k) continue;
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
	for (uint32_t a = pc; b->count < BLOCK_MAX - 1; a += 4) {
		if (!fetchable(a) || (a != pc && special(p, a))) break;
		uint32_t instr = fetchWord(p, a);
		b->words[b->count++] = instr;
		if (kind(instr) == 2) {
			// The delay slot comes along unless it's a branch too.
			uint32_t d = a + 4;
			if (fetchable(d) && !special(p, d) && kind(fetchWord(p, d)) != 2) {
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
	for (int o = 0; o < 3; o++) {
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
		if (!cached(c, pc)) cpuFetch(p, pc);
		struct Block** slot = &j->table[(pc >> 2) & (TABLE_SIZE - 1)];
		struct Block* b = *slot;
		if (!b || b->pc != pc || (b->checked != c->icacheChanges && !lineCurrent(c, b, 0))) {
			struct Block* n = compileBlock(p, j, pc, false);
			if (n) *slot = b = n;
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
	j->table = calloc(TABLE_SIZE, sizeof *j->table);
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
		free(j->table);
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
	free(j->table);
	free(j->blocks);
	free(j);
	p->jit = NULL;
}

void jitFlush(struct Psx* p) {
	struct Jit* j = p->jit;
	if (!j) return;
	memset(j->table, 0, TABLE_SIZE * sizeof *j->table);
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
	if (p->watch || p->pcRing || !(c->biu & 0x800) || !fetchable(pc) || special(p, pc)) {
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

// For profiling: the guest instruction whose code holds address (0 when it isn't a block's).
uint32_t jitDescribe(struct Psx* p, const void* address, uint32_t* instr) {
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
				return b->pc + 4 * w;
			}
		}
	}
	*instr = 0;
	return 1;
}

// For psxrun: what the recompiler did.
void jitStats(struct Psx* p, uint64_t* out) {
	struct Jit* j = p->jit;
	for (int i = 0; i < 6; i++) out[i] = j ? j->stats[i] : 0;
}
