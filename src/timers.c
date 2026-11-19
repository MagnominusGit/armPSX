/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "psx.h"

// The three root counters. Counting, matching and the sync modes follow Mednafen's timer.c
// (Copyright (C) 2011-2016 Mednafen Team, GPL-2.0-or-later).

static bool match(struct Psx* p, int i) {
	struct Timer* t = &p->timer[i];
	bool exact = false;
	t->mode |= 0x800;
	if (t->mode & 8) {
		t->counter %= t->target > 1 ? t->target : 1;
	}
	if ((t->mode & 0x10) && !t->irqDone) {
		exact = t->counter == 0 || t->counter == t->target;
		t->irqDone = true;
		psxIrq(p, IRQ_TIMER0 + i);
	}
	return exact;
}

static bool overflow(struct Psx* p, int i) {
	struct Timer* t = &p->timer[i];
	bool exact = false;
	t->mode |= 0x1000;
	t->counter &= 0xFFFF;
	if ((t->mode & 0x20) && !t->irqDone) {
		exact = t->counter == 0;
		t->irqDone = true;
		psxIrq(p, IRQ_TIMER0 + i);
	}
	return exact;
}

static void clock(struct Psx* p, int i, uint32_t clocks) {
	struct Timer* t = &p->timer[i];
	if (t->counting <= 0) {
		clocks = 0;
	}
	if (i == 2) {
		t->div8 += clocks;
		uint32_t d8 = t->div8 >> 3;
		t->div8 &= 7;
		if (t->mode & 0x200) {
			clocks = d8;
		}
		if (t->mode & 1) {
			clocks = 0;
		}
	}
	if ((t->mode & 8) && t->target == 0 && t->counter == 0) {
		match(p, i);
	} else if (clocks) {
		uint32_t before = t->counter;
		bool exact = false;
		t->counter += clocks;
		if (t->mode & 0x40) {
			t->irqDone = false;
		}
		if ((before < t->target && t->counter >= t->target) || t->counter >= t->target + 0x10000) {
			exact |= match(p, i);
		}
		if (t->counter >= 0x10000) {
			exact |= overflow(p, i);
		}
		if ((t->mode & 0x40) && !exact) {
			t->irqDone = false;
		}
	}
}

static int64_t nextEvent(struct Psx* p) {
	int64_t next = 1 << 20;
	for (int i = 0; i < 3; i++) {
		struct Timer* t = &p->timer[i];
		if (!(t->mode & 0x30)) {
			continue;
		}
		if ((t->mode & 8) && t->counter == 0 && t->target == 0 && !t->irqDone) {
			next = 1;
			continue;
		}
		if ((i < 2 && (t->mode & 0x100)) || t->counting <= 0 || (i == 2 && (t->mode & 1))) {
			continue;
		}
		uint32_t target = (t->mode & 0x18) && t->counter < t->target ? t->target : 0x10000;
		int64_t clocks = target - t->counter;
		if (i == 2 && (t->mode & 0x200)) {
			clocks = clocks * 8 - t->div8;
		}
		if (clocks < next) {
			next = clocks;
		}
	}
	return next;
}

void timersRun(struct Psx* p) {
	uint32_t elapsed = (uint32_t) (p->cycles - p->timersAt);
	p->timersAt = p->cycles;
	for (int i = 0; i < 3; i++) {
		if (!(p->timer[i].mode & 0x100) || i == 2) {
			clock(p, i, elapsed);
		}
	}
	psxSchedule(p, p->cycles + nextEvent(p));
}

void timersDots(struct Psx* p, uint32_t dots) {
	if (p->timer[0].mode & 0x100) {
		clock(p, 0, dots);
	}
}

void timersHblank(struct Psx* p) {
	if (p->timer[1].mode & 0x100) {
		clock(p, 1, 1);
	}
}

// Timer 0's sync modes follow the hblank the way timer 1's follow the vblank (Mednafen does only the
// reset mode for timer 0; ps1-tests timers shows the other three on hardware).
static void syncTo(struct Psx* p, int i, bool was, bool in) {
	struct Timer* t = &p->timer[i];
	switch (t->mode & 7) {
	case 1:
		t->counting = !in;
		break;
	case 3:
		if (was && !in) {
			t->counter = 0;
			if (t->counter == t->target) match(p, i);
		}
		break;
	case 5:
		t->counting = in;
		if (was && !in) {
			t->counter = 0;
			if (t->counter == t->target) match(p, i);
		}
		break;
	case 7:
		if (t->counting == -1) {
			if (!was && in) t->counting = 0;
		} else if (t->counting == 0) {
			if (was && !in) t->counting = 1;
		}
		break;
	}
}

void timersHretrace(struct Psx* p, bool in) {
	timersRun(p);
	syncTo(p, 0, p->hretrace, in);
	p->hretrace = in;
}

void timersVblank(struct Psx* p, bool in) {
	syncTo(p, 1, p->vblank, in);
	p->vblank = in;
}

uint32_t timerRead(struct Psx* p, uint32_t address) {
	int i = address >> 4 & 3;
	if (i == 3) {
		return 0;
	}
	gpuRun(p);
	timersRun(p);
	struct Timer* t = &p->timer[i];
	switch (address & 0xC) {
	case 0: return t->counter;
	case 4: {
		uint32_t v = t->mode;
		t->mode &= ~0x1000u;
		if (t->counter != t->target) {
			t->mode &= ~0x800u;
		}
		return v;
	}
	case 8: return t->target;
	}
	return 0;
}

void timerWrite(struct Psx* p, uint32_t address, uint32_t value) {
	int i = address >> 4 & 3;
	if (i == 3) {
		return;
	}
	gpuRun(p);
	timersRun(p);
	struct Timer* t = &p->timer[i];
	switch (address & 0xC) {
	case 0:
		t->irqDone = false;
		t->counter = value & 0xFFFF;
		break;
	case 4:
		t->mode = (value & 0x3FF) | (t->mode & 0x1C00);
		t->irqDone = false;
		t->counter = 0;
		t->counting = 1;
		if (i < 2) {
			bool in = i == 0 ? p->hretrace : p->vblank;
			switch (t->mode & 7) {
			case 1: t->counting = !in; break;
			case 5: t->counting = in; break;
			case 7: t->counting = -1; break;
			}
		}
		break;
	case 8:
		t->target = value & 0xFFFF;
		break;
	}
	if (t->counter == t->target) {
		match(p, i);
	}
	psxSchedule(p, p->cycles + nextEvent(p));
}
