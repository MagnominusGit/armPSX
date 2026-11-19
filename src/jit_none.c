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

uint32_t jitDescribe(struct Psx* p, const void* address, uint32_t* instr) {
	return 0;
}
