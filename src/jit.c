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
// writes can't touch it. Each instruction does what cpuStep does after a hit: the limit check, the
// cycle it takes or hides (Mednafen's ReadAbsorb), then its work; the CPU's state lives in memory as
// the interpreter keeps it. The ALU and RAM and scratchpad loads and stores run inline; everything
// else, and every load or store that leaves the fast path, goes through cpuJitExecute.
//

enum { ARENA_SIZE = 64 << 20, BLOCK_MAX = 32, BLOCKS_MAX = 1 << 18, TABLE_SIZE = 1 << 19, NO_LOAD = 34, DYNAMIC = -1 };

struct Block {
	uint32_t pc;
	int count, lines;
	bool delay;  // it ends with a branch and its delay slot
	uint32_t gen[BLOCK_MAX / 4 + 2];
	uint32_t words[BLOCK_MAX];
	uint32_t checked;  // cpu.icacheChanges when its lines were last found unchanged
	uint8_t* body;
	uint8_t* end;
	uint16_t at[BLOCK_MAX];  // each instruction's code, from body
};

struct Jit {
	uint8_t* arena;
	size_t used;
	void (*enter)(struct Psx*, int64_t, uint8_t*);
	uint8_t* leave;     // stores r13 and returns
	uint8_t* dispatch;  // on to the block at cpu.pc, or out
	struct Block** table;
	struct Block* blocks;
	int blockCount;
};

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

#if defined(__x86_64__)
#include "jit_x64.h"
#elif defined(__aarch64__)
#include "jit_a64.h"
#endif

static struct Block* compile(struct Psx* p, struct Jit* j, uint32_t pc) {
	struct Cpu* c = &p->cpu;
	if (j->blockCount == BLOCKS_MAX || j->used + 64 * 1024 > ARENA_SIZE) {
		jitFlush(p);
	}
	struct Block* b = &j->blocks[j->blockCount];
	b->pc = pc;
	b->count = 0;
	b->delay = false;
	for (uint32_t a = pc; b->count < BLOCK_MAX - 1; a += 4) {
		if (!fetchable(a) || !cached(c, a) || (a != pc && special(p, a))) break;
		uint32_t instr = cachedWord(c, a);
		b->words[b->count++] = instr;
		if (kind(instr) == 2) {
			// The delay slot comes along when the cache holds it and it isn't a branch too.
			uint32_t d = a + 4;
			if (fetchable(d) && cached(c, d) && !special(p, d) && kind(cachedWord(c, d)) != 2) {
				b->words[b->count++] = cachedWord(c, d);
				b->delay = true;
			}
			break;
		}
	}
	if (!b->count) return NULL;
	b->lines = ((pc + 4 * b->count - 4) >> 4) - (pc >> 4) + 1;
	for (int i = 0; i < b->lines; i++) {
		b->gen[i] = c->icacheGen[((pc >> 4) + i) & 255];
	}

	uint8_t* start = j->arena + j->used;
	uint8_t* end = emitBlock(p, j, b, start, j->arena + ARENA_SIZE);
	if (!end) {
		jitFlush(p);
		return NULL;
	}
	flushCode(start, end);
	j->used = (size_t) (end - j->arena + 15) & ~(size_t) 15;
	b->body = start;
	b->end = end;
	b->checked = c->icacheChanges;
	j->blockCount++;
	return b;
}

// Whether the block's lines still hold what it was compiled from.
static bool current(struct Cpu* c, struct Block* b) {
	for (int i = 0; i < b->lines; i++) {
		if (c->icacheGen[((b->pc >> 4) + i) & 255] != b->gen[i]) {
			for (int w = 0; w < b->count; w++) {
				uint32_t a = b->pc + 4 * w;
				if (!cached(c, a) || cachedWord(c, a) != b->words[w]) return false;
			}
			for (int l = 0; l < b->lines; l++) {
				b->gen[l] = c->icacheGen[((b->pc >> 4) + l) & 255];
			}
			return true;
		}
	}
	return true;
}

bool jitCreate(struct Psx* p) {
	struct Jit* j = calloc(1, sizeof *j);
	if (!j) return false;
	j->arena = mmap(NULL, ARENA_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	j->table = calloc(TABLE_SIZE, sizeof *j->table);
	j->blocks = calloc(BLOCKS_MAX, sizeof *j->blocks);
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
	struct Block** slot = &j->table[(pc >> 2) & (TABLE_SIZE - 1)];
	struct Block* b = *slot;
	if (!b || b->pc != pc) {
		if (!cached(c, pc)) return false;
		b = compile(p, j, pc);
		if (!b) return false;
		*slot = b;
	} else if (b->checked != c->icacheChanges) {
		if (!current(c, b)) return false;
		b->checked = c->icacheChanges;
	}
	j->enter(p, until, b->body);
	return true;
}

// For profiling: the guest instruction whose code holds address (0 when it isn't a block's).
uint32_t jitDescribe(struct Psx* p, const void* address, uint32_t* instr) {
	struct Jit* j = p->jit;
	const uint8_t* a = address;
	if (!j || a < j->arena || a >= j->arena + ARENA_SIZE) return 0;
	for (int i = 0; i < j->blockCount; i++) {
		struct Block* b = &j->blocks[i];
		if (a >= b->body && a < b->end) {
			int w = 0;
			while (w + 1 < b->count && a >= b->body + b->at[w + 1]) w++;
			*instr = b->words[w];
			return b->pc + 4 * w;
		}
	}
	*instr = 0;
	return 1;
}
