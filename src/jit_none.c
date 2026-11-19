/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "psx.h"

// Hosts without a recompiler: the interpreter runs everything.

bool jitCreate(struct Psx* p) {
	return false;
}

void jitFree(struct Psx* p) {
}

void jitFlush(struct Psx* p) {
}

bool jitRun(struct Psx* p, int64_t until) {
	return false;
}

uint32_t jitDescribe(struct Psx* p, const void* address, uint32_t* instr, int* version) {
	(void) version;
	return 0;
}

void jitStats(struct Psx* p, uint64_t* out) {
	for (int i = 0; i < 6; i++) out[i] = 0;
}

void jitDump(struct Psx* p, const char* path) {
	(void) p;
	(void) path;
}

void jitInvalidate(struct Psx* p, uint32_t offset) {
	(void) p;
	(void) offset;
}
