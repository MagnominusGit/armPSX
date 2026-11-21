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
static void dualShockByte(struct Pad* d, uint8_t in);

static int padByte(struct Pad* d, uint8_t in, uint8_t* out) {
	if (!d->dtr) return 0;
	if (d->txCount) {
		*out &= d->tx[d->txPos];
		d->txPos++;
		d->txCount--;
	}
	if (d->dualShock) {
		dualShockByte(d, in);
		return d->txCount ? 0x40 : 0;
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

// The DualShock (Mednafen's dualshock.cpp): the digital pad's reply in digital mode, the sticks too
// in analog mode, and a configuration mode (entered with command 0x43) with commands 0x40-0x4F for
// the mode, its lock, the motors' mapping and the pad's description.
static void queue(struct Pad* d, const uint8_t* bytes, int n) {
	memcpy(d->tx, bytes, (size_t) n);
	d->txPos = 0;
	d->txCount = n;
}

static void dualShockByte(struct Pad* d, uint8_t in) {
	uint8_t b0 = 0xFF ^ (uint8_t) d->buttons, b1 = 0xFF ^ (uint8_t) (d->buttons >> 8);
	int phase = d->phase;
	if (phase == 0) {
		if (in != 0x01) {
			d->phase = -1;
		} else if (d->config) {
			queue(d, (const uint8_t[]) { 0xF3 }, 1);
			d->phase = 101;
		} else {
			queue(d, (const uint8_t[]) { d->analogMode ? 0x73 : 0x41 }, 1);
			d->phase = 1;
		}
	} else if (phase == 1) {
		d->command = in;
		d->phase = 2;
		if (in == 0x42) {
			queue(d, (const uint8_t[]) { 0x5A }, 1);
			d->phase = 0x4200;
		} else if (in == 0x43) {
			const uint8_t a[7] = { 0x5A, b0, b1, d->axes[0][0], d->axes[0][1], d->axes[1][0], d->axes[1][1] };
			queue(d, a, d->analogMode ? 7 : 3);
		} else {
			d->phase = -1;
			d->txCount = 0;
		}
	} else if (phase == 2) {
		// Command 0x43 outside the configuration mode: its second data byte 1 enters it
		if (d->command == 0x43 && d->txPos == 2 && in == 0x01) {
			d->config = true;
			if (d->daRumble) {
				d->rumble[0] = d->rumble[1] = 0;
				d->daRumble = false;
			}
			d->phase = -1;
		}
	} else if (phase == 101) {
		d->command = in;
		if (in >= 0x40 && in <= 0x4F) {
			queue(d, (const uint8_t[]) { 0x5A }, 1);
			d->phase = in << 8;
		} else {
			d->txCount = 0;
			d->phase = -1;
		}
	} else if (phase == 0x4200) {
		if (d->analogMode || d->config) {
			const uint8_t a[6] = { b0, b1, d->axes[0][0], d->axes[0][1], d->axes[1][0], d->axes[1][1] };
			queue(d, a, 6);
		} else {
			const uint8_t a[4] = { b0, b1, 0, 0 };
			queue(d, a, (d->rumbleMap[2] & 0xFE) ? 2 : 4);
		}
		d->phase++;
	} else if (phase == 0x4201) {
		const uint8_t* m = d->rumbleMap;
		if (d->daRumble) d->rumble[0] = in;
		else if (m[0] == 0x00 && m[2] && m[3] && m[4] && m[5]) d->rumble[0] = in;
		d->phase++;
	} else if (phase == 0x4202) {
		const uint8_t* m = d->rumbleMap;
		if (d->daRumble) d->rumble[1] = in;
		else if (m[1] == 0x01) d->rumble[1] = in;
		else if (m[1] == 0x00 && m[2] && m[3] && m[4] && m[5]) d->rumble[0] = in;
		d->phase++;
	} else if (phase == 0x4203) {
		if (!d->daRumble && d->rumbleMap[1] == 0x00 && d->rumbleMap[2] == 0x01) d->rumble[1] = in;
		d->phase++;
	} else if ((phase & 0xFF) == 0 && phase >= 0x4000 && phase <= 0x4F00) {
		// The configuration commands' first data byte: 0 goes on
		if (in != 0x00) {
			d->phase = -1;
		} else {
			uint8_t first = phase == 0x4500 ? 0x01 : phase == 0x4D00 ? d->rumbleMap[0] : 0x00;
			queue(d, &first, 1);
			d->phase++;
		}
	} else if (phase >= 0x4D01 && phase <= 0x4D06) {
		unsigned index = (unsigned) (phase - 0x4D01);
		if (index < 5) {
			queue(d, &d->rumbleMap[1 + index], 1);
			d->phase++;
		} else {
			d->phase = -1;
		}
		d->rumbleMap[index] = in;
	} else if (phase == 0x4402) {
		if (in == 0x02) d->analogLocked = false;
		else if (in == 0x03) d->analogLocked = true;
		d->phase = -1;
	} else if ((phase & 0xFF) == 1 && phase >= 0x4001 && phase <= 0x4F01) {
		uint8_t a[5] = { 0 };
		int command = phase >> 8;
		d->phase = -1;
		if (command == 0x43) {
			if (in == 0x00) d->config = false;
		} else if (command == 0x44) {
			// The mode, whatever the lock; then its lock
			if (in == 0x00) d->analogMode = false;
			else if (in == 0x01) d->analogMode = true;
			d->phase = 0x4402;
		} else if (command == 0x45) {
			a[0] = 0x02, a[1] = d->analogMode ? 0x01 : 0x00, a[2] = 0x02, a[3] = 0x01;
		} else if (command == 0x46) {
			if (in == 0x00) a[1] = 0x01, a[2] = 0x02, a[4] = 0x0A;
			else if (in == 0x01) a[1] = 0x01, a[2] = 0x01, a[3] = 0x01, a[4] = 0x14;
		} else if (command == 0x47) {
			if (in == 0x00) a[1] = 0x02, a[3] = 0x01;
		} else if (command == 0x48) {
			if (in == 0x00) a[3] = 0x01, a[4] = d->rumble[0];
			else if (in == 0x01) a[3] = 0x01, a[4] = d->rumble[1];
		} else if (command == 0x4C) {
			if (in == 0x00) a[2] = 0x04;
			else if (in == 0x01) a[2] = 0x07;
		}
		queue(d, a, 5);
	}
}

// Its Analog button changes its mode while it isn't being talked to, unless the mode is locked.
static void analogButton(struct Pad* d) {
	if (d->dtr) return;
	if (d->analogButton && !d->analogButtonWas && !d->analogLocked) d->analogMode = !d->analogMode;
	d->analogButtonWas = d->analogButton;
}

void sioPadInput(struct Pad* d) {
	if (d->dualShock) analogButton(d);
}

void sioPadRumble(const struct Pad* d, uint8_t* large, uint8_t* small) {
	if (!d->dualShock) {
		*large = *small = 0;
	} else if (!d->daRumble) {
		*small = d->rumble[0] == 0x01 ? 0xFF : 0;
		*large = d->rumble[1];
	} else {
		*small = (d->rumble[0] & 0xC0) == 0x40 && (d->rumble[1] & 1) ? 0xFF : 0;
		*large = 0;
	}
}

void sioPowerPads(struct Psx* p) {
	for (int i = 0; i < 2; i++) {
		struct Pad* d = &p->sio.pad[i];
		struct Pad keep = *d;
		memset(d, 0, sizeof *d);
		d->buttons = keep.buttons;
		d->dualShock = keep.dualShock;
		memcpy(d->axes, keep.axes, sizeof d->axes);
		d->analogButton = d->analogButtonWas = keep.analogButton;
		d->analogMode = keep.analogMode;  // (as the frontend set it to start)
		memset(d->rumbleMap, 0xFF, sizeof d->rumbleMap);
		d->daRumble = true;
	}
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
		bool was = d->dtr;
		d->dtr = c->dtr = dtr;
		if (was && !dtr && d->dualShock) analogButton(d);
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
		// SIO1 (the serial port) has nothing on it: its registers just keep what's written.
		switch (address & 0xE) {
		case 0x4: return 5;
		case 0x8: return p->sio.sio1[0] >> (address & 1) * 8;
		case 0xA: return p->sio.sio1[1] >> (address & 1) * 8;
		case 0xE: return p->sio.sio1[2] >> (address & 1) * 8;
		}
		return 0;
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
		switch (address & 0xE) {
		case 0x8: p->sio.sio1[0] = value & 0xFF; break;
		case 0xA: p->sio.sio1[1] = value & 0xFFFF; break;
		case 0xE: p->sio.sio1[2] = value & 0xFFFF; break;
		}
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

// An empty, formatted card (Mednafen's memcard.cpp Format()).
void sioFormatCard(struct Memcard* c) {
	memset(c->data, 0, sizeof c->data);
	c->data[0] = 0x4D;
	c->data[1] = 0x43;
	c->data[0x7F] = 0x0E;
	for (unsigned a = 0x80; a < 0x800; a += 0x80) {
		c->data[a] = 0xA0;
		c->data[a + 8] = c->data[a + 9] = 0xFF;
		c->data[a + 0x7F] = 0xA0;
	}
	for (unsigned a = 0x800; a < 0x1200; a += 0x80) {
		c->data[a] = c->data[a + 1] = c->data[a + 2] = c->data[a + 3] = 0xFF;
		c->data[a + 8] = c->data[a + 9] = 0xFF;
	}
	c->present = true;
	c->fresh = true;
}
