/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "psx.h"

// SIO0: the controller and memory card port, a byte at a time. The pad's and the card's answers and
// acknowledge delays follow Mednafen's frontio.cpp, gamepad.cpp and memcard.cpp
// (Copyright (C) 2011-2017 Mednafen Team, GPL-2.0-or-later).

static const uint8_t scaleShift[4] = { 0, 0, 4, 6 };

static uint32_t bitClocks(struct Sio* s) {
	uint32_t d = (s->baud << scaleShift[s->mode & 3]) & ~1u;
	return d < 0x20 ? 0x20 : d;
}

// Each device puts out what it queued after the last byte, then takes this one; it returns the delay
// of its acknowledge pulse, 0 for none.
static int padByte(struct Pad* d, uint8_t in, uint8_t* out) {
	if (!d->dtr) return 0;
	if (d->txCount) {
		*out &= d->tx[d->txPos];
		d->txPos++;
		d->txCount--;
	}
	switch (d->phase) {
	case 0:
		if (in != 0x01) {
			d->phase = -1;
		} else {
			d->tx[0] = 0x41;
			d->txPos = 0;
			d->txCount = 1;
			d->phase++;
		}
		break;
	case 1:
		d->command = in;
		d->phase++;
		d->tx[0] = 0x5A;
		if (in == 0x42) {
			d->tx[1] = 0xFF ^ (uint8_t) d->buttons;
			d->tx[2] = 0xFF ^ (uint8_t) (d->buttons >> 8);
			d->txPos = 0;
			d->txCount = 3;
		} else {
			d->phase = -1;
			d->txPos = 0;
			d->txCount = 0;
		}
		break;
	}
	return d->txCount ? 0x40 : 0;
}

static int cardByte(struct Memcard* c, uint8_t in, uint8_t* out) {
	if (!c->dtr || !c->present) return 0;
	if (c->txCount) {
		*out &= c->tx;
		c->txCount--;
	}
	int phase = c->phase;
	if (phase == 0) {
		if (in != 0x81) {
			c->phase = -1;
		} else {
			c->tx = c->fresh ? 0x08 : 0x00;
			c->txCount = 1;
			c->phase++;
		}
	} else if (phase == 1) {
		c->command = in;
		if (in == 'R' || in == 'W') {
			c->phase++;
			c->tx = 0x5A;
			c->txCount = 1;
		} else {
			c->phase = -1;
			c->tx = 0;
			c->txCount = 0;
		}
	} else if (phase == 2) {
		c->tx = 0x5D;
		c->txCount = 1;
		c->phase++;
	} else if (phase == 3) {
		c->tx = 0;
		c->txCount = 1;
		c->phase = c->command == 'R' ? 1000 : 2000;
	} else if (phase == 1000) {
		c->address = (uint32_t) in << 8;
		c->tx = in;
		c->txCount = 1;
		c->phase++;
	} else if (phase == 1001) {
		c->address |= in;
		c->tx = '\\';
		c->txCount = 1;
		c->phase++;
	} else if (phase == 1002) {
		if (c->address >= sizeof c->data >> 7) c->address = 0xFFFF;
		c->xor = 0;
		c->tx = ']';
		c->txCount = 1;
		c->phase++;
	} else if (phase == 1003) {
		c->tx = c->address >> 8;
		c->xor ^= c->tx;
		c->txCount = 1;
		c->phase++;
	} else if (phase == 1004) {
		c->tx = c->address & 0xFF;
		c->xor ^= c->tx;
		c->txCount = 1;
		c->phase = c->address == 0xFFFF ? -1 : 1024;
	} else if (phase >= 1024 && phase < 1024 + 128) {
		c->tx = c->data[(c->address << 7) + (phase - 1024)];
		c->xor ^= c->tx;
		c->txCount = 1;
		c->phase++;
	} else if (phase == 1024 + 128) {
		c->tx = c->xor;
		c->txCount = 1;
		c->phase++;
	} else if (phase == 1024 + 129) {
		c->tx = 'G';
		c->txCount = 1;
		c->phase = -1;
	} else if (phase == 2000) {
		c->xor = in;
		c->address = (uint32_t) in << 8;
		c->tx = in;
		c->txCount = 1;
		c->phase++;
	} else if (phase == 2001) {
		c->xor ^= in;
		c->address |= in;
		c->tx = in;
		c->txCount = 1;
		c->phase = 2048;
	} else if (phase >= 2048 && phase < 2048 + 128) {
		c->xor ^= in;
		c->rw[phase - 2048] = in;
		c->tx = in;
		c->txCount = 1;
		c->phase++;
	} else if (phase == 2048 + 128) {
		c->writeXor = in;
		c->tx = '\\';
		c->txCount = 1;
		c->phase++;
	} else if (phase == 2048 + 129) {
		c->tx = ']';
		c->txCount = 1;
		c->phase++;
	} else if (phase == 2048 + 130) {
		if (c->xor != c->writeXor) {
			c->tx = 'N';
		} else if (c->address >= sizeof c->data >> 7) {
			c->tx = 0xFF;
		} else {
			c->tx = 'G';
			c->fresh = false;
			if (memcmp(&c->data[c->address << 7], c->rw, 128)) {
				memcpy(&c->data[c->address << 7], c->rw, 128);
				c->dirty = true;
			}
		}
		c->txCount = 1;
		c->phase = -1;
	}
	return c->txCount ? 0x100 : 0;
}

static void setDtr(struct Psx* p) {
	struct Sio* s = &p->sio;
	for (int i = 0; i < 2; i++) {
		bool dtr = (s->control & 2) && !!(s->control & 0x2000) == i;
		struct Pad* d = &s->pad[i];
		struct Memcard* c = &s->card[i];
		if (!d->dtr && dtr) {
			d->phase = 0;
			d->txPos = d->txCount = 0;
		}
		if (!c->dtr && dtr) {
			c->phase = 0;
			c->txCount = 0;
		}
		d->dtr = c->dtr = dtr;
	}
	if (!(s->control & 2)) {
		s->dsrAt = 0;
	}
}

static void startPending(struct Psx* p) {
	struct Sio* s = &p->sio;
	if (s->txPending && (s->control & 1)) {
		s->txPending = false;
		s->txBusy = true;
		s->rxAvail = false;
		s->doneAt = p->cycles + 8 * (int64_t) bitClocks(s);
		psxSchedule(p, s->doneAt);
	}
	if (!(s->control & 5)) {
		s->txBusy = false;
	}
}

void sioRun(struct Psx* p) {
	struct Sio* s = &p->sio;
	if (s->txBusy && p->cycles >= s->doneAt) {
		s->txBusy = false;
		uint8_t out = 0xFF;
		int delay = 0, d;
		for (int i = 0; i < 2; i++) {
			if ((d = padByte(&s->pad[i], s->txBuffer, &out))) delay = d;
			if ((d = cardByte(&s->card[i], s->txBuffer, &out))) delay = d;
		}
		s->rxBuffer = out;
		s->rxAvail = true;
		if (s->control & 0x400) {
			s->irq = true;
			psxIrq(p, IRQ_SIO0);
		}
		if (s->control & 0x800) {
			s->irq = true;
			psxIrq(p, IRQ_SIO0);
		}
		if (delay) {
			s->dsrAt = s->doneAt + delay;
		}
		startPending(p);
	}
	if (s->dsrAt && p->cycles >= s->dsrAt) {
		s->dsrUntil = s->dsrAt + 32;
		s->dsrAt = 0;
		if (s->control & 0x1000) {
			s->irq = true;
			psxIrq(p, IRQ_SIO0);
		}
	}
	if (s->txBusy) psxSchedule(p, s->doneAt);
	if (s->dsrAt) psxSchedule(p, s->dsrAt);
}

uint32_t sioRead(struct Psx* p, uint32_t address, int size) {
	struct Sio* s = &p->sio;
	if (address >= 0x1F801050) {
		return address == 0x1F801054 ? 5 : 0;
	}
	sioRun(p);
	uint32_t v = 0;
	switch (address & 0xE) {
	case 0x0: case 0x2:
		v = s->rxBuffer * 0x01010101u;
		s->rxAvail = false;
		v >>= (address & 2) * 8;
		break;
	case 0x4:
		if (!s->txPending && !s->txBusy) v |= 1;
		if (s->rxAvail) v |= 2;
		if (!s->txPending && !s->txBusy) v |= 4;
		if (p->cycles < s->dsrUntil) v |= 0x80;
		if (s->irq) v |= 0x200;
		break;
	case 0x8: v = s->mode; break;
	case 0xA: v = s->control; break;
	case 0xE: v = s->baud; break;
	}
	return v >> (address & 1) * 8;
}

void sioWrite(struct Psx* p, uint32_t address, uint32_t value, int size) {
	struct Sio* s = &p->sio;
	if (address >= 0x1F801050) {
		return;
	}
	sioRun(p);
	value <<= (address & 1) * 8;
	switch (address & 0xE) {
	case 0x0: case 0x2:
		s->txBuffer = value;
		s->txPending = true;
		s->txBusy = false;
		break;
	case 0x8: s->mode = value & 0x13F; break;
	case 0xA:
		s->control = value & 0x3F2F;
		if (value & 0x10) s->irq = false;
		if (value & 0x40) {
			s->irq = false;
			s->txPending = s->txBusy = s->rxAvail = false;
			s->txBuffer = s->rxBuffer = 0;
			s->mode = s->control = s->baud = 0;
		}
		setDtr(p);
		break;
	case 0xE: s->baud = value & 0xFFFF; break;
	}
	startPending(p);
}
