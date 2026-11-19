/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "psx.h"

// The MDEC; decoding comes with the Mednafen port.

uint32_t mdecRead(struct Psx* p, uint32_t address) {
	return address & 4 ? 0x80040000 : 0;
}

void mdecWrite(struct Psx* p, uint32_t address, uint32_t value) {
}

uint32_t mdecDmaRead(struct Psx* p) {
	return 0;
}

void mdecDmaWrite(struct Psx* p, uint32_t value) {
}
