/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "psx.h"

// The CD-ROM controller without a disc, for bringing the BIOS up; Mednafen's controller replaces it.

static void push(struct Cdrom* c, int64_t at, int irq, const uint8_t* bytes, int n) {
	if (c->queued == 4) return;
	c->queue[c->queued].at = at;
	c->queue[c->queued].irq = irq;
	c->queue[c->queued].count = n;
	memcpy(c->queue[c->queued].bytes, bytes, n);
	c->queued++;
}

void cdromReset(struct Psx* p) {
	struct Cdrom* c = &p->cdrom;
	memset(c, 0, sizeof *c);
	c->status = 0x10;
	c->shellOpen = true;
}

static void command(struct Psx* p, uint8_t cmd) {
	struct Cdrom* c = &p->cdrom;
	int64_t at = p->cycles + 25000;
	uint8_t s = c->status;
	switch (cmd) {
	case 0x01: push(c, at, 3, &s, 1); break;
	case 0x19:
		if (c->paramCount && c->params[0] == 0x20) {
			static const uint8_t version[4] = { 0x94, 0x09, 0x19, 0xC0 };
			push(c, at, 3, version, 4);
		} else {
			push(c, at, 3, &s, 1);
		}
		break;
	case 0x1A: {
		uint8_t e[2] = { (uint8_t) (s | 1), 0x80 };
		push(c, at, 3, &s, 1);
		push(c, at + 30000, 5, e, 2);
		break;
	}
	case 0x0A:
		push(c, at, 3, &s, 1);
		push(c, at + 80000, 2, &s, 1);
		break;
	default: {
		uint8_t e[2] = { (uint8_t) (s | 1), 0x80 };
		push(c, at, 5, e, 2);
		break;
	}
	}
	c->paramCount = 0;
	psxSchedule(p, at);
}

void cdromRun(struct Psx* p) {
	struct Cdrom* c = &p->cdrom;
	if (c->queued && c->iflags == 0 && p->cycles >= c->queue[0].at) {
		c->iflags = c->queue[0].irq;
		memcpy(c->response, c->queue[0].bytes, c->queue[0].count);
		c->responseCount = c->queue[0].count;
		c->responseRead = 0;
		memmove(c->queue, c->queue + 1, sizeof c->queue[0] * (c->queued - 1));
		c->queued--;
		if (c->iflags & c->ie) psxIrq(p, IRQ_CDROM);
	}
	if (c->queued) psxSchedule(p, c->queue[0].at > p->cycles ? c->queue[0].at : p->cycles + 1000);
}

uint8_t cdromRead(struct Psx* p, uint32_t address) {
	struct Cdrom* c = &p->cdrom;
	switch (address & 3) {
	case 0:
		return c->index | (c->paramCount == 0) << 3 | (c->paramCount < 16) << 4 | (c->responseRead < c->responseCount) << 5;
	case 1:
		return c->responseRead < c->responseCount ? c->response[c->responseRead++] : 0;
	case 2:
		return 0;
	default:
		return c->index & 1 ? 0xE0 | c->iflags : 0xE0 | c->ie;
	}
}

void cdromWrite(struct Psx* p, uint32_t address, uint8_t v) {
	struct Cdrom* c = &p->cdrom;
	switch ((address & 3) | c->index << 2) {
	case 0: case 4: case 8: case 12: c->index = v & 3; break;
	case 1: command(p, v); break;
	case 2: if (c->paramCount < 16) c->params[c->paramCount++] = v; break;
	case 6: c->ie = v & 0x1F; break;
	case 7:
		c->iflags &= ~(v & 0x1F);
		if (v & 0x40) c->paramCount = 0;
		cdromRun(p);
		break;
	}
}

uint32_t cdromDmaRead(struct Psx* p) {
	return 0;
}
