/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PSX_H
#define PSX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Clocks: the CPU runs at 33.8688 MHz (44100 * 768); everything is timed in CPU cycles.
#define CPU_HZ 33868800
#define RAM_SIZE (2 * 1024 * 1024)
#define BIOS_SIZE (512 * 1024)

enum {
	IRQ_VBLANK, IRQ_GPU, IRQ_CDROM, IRQ_DMA, IRQ_TIMER0, IRQ_TIMER1, IRQ_TIMER2, IRQ_SIO0, IRQ_SIO1, IRQ_SPU, IRQ_LIGHTPEN
};


struct Cpu {
	uint32_t r[35];  // r[32] = hi, r[33] = lo, r[34] = where "no load in flight" commits
	uint32_t pc, next;
	uint32_t ldReg, ldValue;  // the load in flight; ldReg 34 = none
	uint8_t absorb[35], absorbWhich, readFudge, ldAbsorb;  // load latency the next instructions hide (Mednafen's ReadAbsorb)
	bool branched, inDelay;   // the instruction just run was a jump or branch / this one is in its delay slot
	uint32_t sr, cause, epc, badVaddr, tar;
	uint32_t cop0[32];
	uint32_t biu;
	int64_t muldivDone;
	int64_t gteDone;
	struct { uint32_t tag, data; } icache[1024];
};

struct Dma {
	uint32_t madr[7], bcr[7], chcr[7];
	uint32_t dpcr, dicr;
	int64_t doneAt[7];  // the cycle a running block or list transfer finishes (0: none)
	uint32_t remaining[7], cursor[7];  // MDEC transfers waiting on the MDEC
};

struct Timer {
	uint32_t counter, mode, target;
	bool irqDone;
	int counting;  // 1 counting, 0 paused, -1 waiting for the first vblank (sync mode 3)
	uint32_t div8;
};

struct GpuVertex {
	int32_t x, y;
	uint32_t u, v, r, g, b;
};

struct Gpu {
	uint16_t vram[512 * 1024];
	uint16_t clut[256];
	uint32_t clutTag, clutCount;
	struct { uint32_t xAnd, xAdd, yAnd, yAdd; } window;
	struct { uint16_t data[4]; uint32_t tag; } texCache[256];
	uint32_t dmaControl;
	int32_t clipX0, clipY0, clipX1, clipY1, offX, offY;
	uint16_t maskSet, maskEval;
	bool dither, drawDisplayed, texDisable, texDisableAllowChange;
	uint32_t tww, twh, twx, twy;
	uint32_t texPageX, texPageY, spriteFlip, abr, texMode;
	uint32_t fifo[32];
	int fifoRead, fifoCount;
	uint32_t readBuffer, readBufferEx;
	bool irq;
	int inCommand;  // 0 none, 1 VRAM write, 2 VRAM read, 4 quad's second triangle, 8 polyline
	uint32_t inCommandCode;
	struct GpuVertex quad[3];
	struct GpuVertex lineLast;
	uint32_t rwX, rwY, rwW, rwH, rwCurX, rwCurY;
	uint32_t displayX, displayY, hStart, hEnd, vStart, vEnd, displayMode;
	bool displayOff, phaseChange, inVblank, field, fieldReadout, skip;
	uint32_t curYOffset, curLineY;
	uint64_t clockCounter;
	uint32_t clockRatio;
	uint32_t linesPerField, scanline, dotCounter, lineCounter, linePhase;
	int32_t drawTime;
	int64_t last;
	bool frameDone;
	uint8_t ditherLut[4][4][512];
	// The picture, as Mednafen makes it: 768 wide, up to 576 lines, with the line widths
	uint32_t frame[576 * 768];
	uint32_t lineWidth[576];
	int frameX, frameY, frameW, frameH;
};

struct Pad {
	uint16_t buttons;  // pressed bits in the pad's order (bit 0 Select ... bit 15 Square)
	int phase, txPos, txCount;
	uint8_t command, tx[3];
	bool dtr;
};

struct Memcard {
	uint8_t data[128 * 1024];
	bool present, fresh, dirty, dtr;
	int phase, txCount;
	uint8_t command, tx, xor, writeXor, rw[128];
	uint32_t address;
};

struct Sio {
	uint32_t mode, control, baud;
	uint8_t txBuffer, rxBuffer;
	bool txPending, txBusy, rxAvail, irq;
	int64_t doneAt, dsrAt, dsrUntil;
	struct Pad pad[2];
	struct Memcard card[2];
};

struct Disc;


struct Psx {
	struct Cpu cpu;
	int64_t cycles;     // CPU cycles since power-on
	int64_t nextEvent;  // the earliest cycle anything besides the CPU has to run at
	uint8_t ram[RAM_SIZE];
	uint8_t bios[BIOS_SIZE];
	uint8_t scratch[1024];
	uint32_t memControl[9], ramSize, cacheControl;
	uint16_t iStat, iMask;
	struct Dma dma;
	struct Timer timer[3];
	int64_t timersAt;  // the cycle the timers have counted to
	bool vblank, hretrace;
	struct Gpu gpu;
	struct Sio sio;
	struct Disc* disc;
	uint32_t frames;
	bool pal;
	uint32_t exeEntry;
	uint8_t* exe;
	size_t exeSize;
	char tty[256];
	int ttyLength;
};

// psx.c
struct Psx* psxCreate(void);
bool psxLoadBios(struct Psx* p, const char* path);
void psxReset(struct Psx* p);
void psxRunFrame(struct Psx* p);
void psxEvents(struct Psx* p);
void psxSchedule(struct Psx* p, int64_t at);
void psxIrq(struct Psx* p, int irq);
bool psxLoadExe(struct Psx* p, const uint8_t* data, size_t size);

// cpu.c
void cpuReset(struct Psx* p);
void cpuRun(struct Psx* p, int64_t until);

// gte.c (Mednafen's)
void GTE_Init(void);
void GTE_Power(void);
int32_t GTE_Instruction(uint32_t instr);
void GTE_WriteCR(unsigned int which, uint32_t value);
uint32_t GTE_ReadCR(unsigned int which);
void GTE_WriteDR(unsigned int which, uint32_t value);
uint32_t GTE_ReadDR(unsigned int which);

// bus.c
uint32_t busRead32(struct Psx* p, uint32_t address);
uint16_t busRead16(struct Psx* p, uint32_t address);
uint8_t busRead8(struct Psx* p, uint32_t address);
void busWrite32(struct Psx* p, uint32_t address, uint32_t value);
void busWrite16(struct Psx* p, uint32_t address, uint16_t value);
void busWrite8(struct Psx* p, uint32_t address, uint8_t value);

// dma.c
uint32_t dmaRead(struct Psx* p, uint32_t address);
void dmaWrite(struct Psx* p, uint32_t address, uint32_t value);
void dmaRun(struct Psx* p);

// timers.c
uint32_t timerRead(struct Psx* p, uint32_t address);
void timerWrite(struct Psx* p, uint32_t address, uint32_t value);
void timersRun(struct Psx* p);
void timersHblank(struct Psx* p);
void timersVblank(struct Psx* p, bool in);
void timersDots(struct Psx* p, uint32_t dots);

// gpu.c
void gpuReset(struct Psx* p);
uint32_t gpuRead(struct Psx* p, uint32_t address);
void gpuWrite(struct Psx* p, uint32_t address, uint32_t value);
void gpuGp0(struct Psx* p, uint32_t word);
void gpuRun(struct Psx* p);
int gpuDisplay(struct Psx* p, uint32_t* out, int* width, int* height);
int64_t gpuNextEvent(struct Psx* p);
uint32_t gpuDotClock(struct Psx* p);

// sio.c
uint32_t sioRead(struct Psx* p, uint32_t address, int size);
void sioWrite(struct Psx* p, uint32_t address, uint32_t value, int size);
void sioRun(struct Psx* p);

// spu.c
void spuReset(struct Psx* p);
uint16_t spuRead(struct Psx* p, uint32_t address);
void spuWrite(struct Psx* p, uint32_t address, uint16_t value);
void spuDmaWrite(struct Psx* p, uint32_t value);
uint32_t spuDmaRead(struct Psx* p);
void spuRun(struct Psx* p);
void SPU_Power(void);
int32_t SPU_UpdateFromCDC(int32_t clocks);
int spuSamples(int16_t* out);

// cdrom.c
void cdromReset(struct Psx* p);
uint8_t cdromRead(struct Psx* p, uint32_t address);
void cdromWrite(struct Psx* p, uint32_t address, uint8_t value);
uint32_t cdromDmaRead(struct Psx* p);
bool cdromDmaCanRead(struct Psx* p);
void CDC_GetCDAudio(int32_t samples[2]);
void cdromRun(struct Psx* p);

// mdec.c
uint32_t mdecRead(struct Psx* p, uint32_t address);
void mdecWrite(struct Psx* p, uint32_t address, uint32_t value);
uint32_t mdecDmaRead(struct Psx* p, uint32_t* offset);
void mdecReset(struct Psx* p);
bool mdecCanWrite(struct Psx* p);
bool mdecCanRead(struct Psx* p);
void mdecDmaWrite(struct Psx* p, uint32_t value);

#endif
