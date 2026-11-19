/* armPSX: Mednafen's spu.cpp (with spu_reverb.inc and spu_fir_table.inc) converted to C;
   output is the SPU's own 44.1 kHz, no resampler; no save states yet. */
/******************************************************************************/
/* Mednafen Sony PS1 Emulation Module                                         */
/******************************************************************************/
/* spu.cpp:
**  Copyright (C) 2011-2023 Mednafen Team
**
** This program is free software; you can redistribute it and/or
** modify it under the terms of the GNU General Public License
** as published by the Free Software Foundation; either version 2
** of the License, or (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software Foundation, Inc.,
** 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#if defined(__GNUC__) && !defined(__clang__)
 #pragma GCC optimize ("unroll-loops")
#endif

/* TODO:
	Note to self: Emulating the SPU at more timing accuracy than sample, and emulating the whole SPU RAM write port FIFO thing and hypothetical periodic FIFO commit to
	SPU RAM(maybe every 32 CPU cycles, with exceptions?) will likely necessitate a much more timing-accurate CPU core, and emulation of the SPU delay register(or at least the
	effects of the standard value written to it), to avoid game glitches.  Probably more trouble than it's worth....

	SPU IRQ emulation isn't totally correct, behavior is kind of complex; run more tests on PS1.

	Test reverb upsampler on the real thing.

	Alter reverb algorithm to process in the pattern of L,R,L,R,L,R on each input sample, instead of doing both L and R on every 2 input samples(make
	sure the real thing does it this way too, I think it at least runs the downsampler this way).

	Alter reverb algorithm to perform saturation more often, as occurs on the real thing.

	See if sample flag & 0x8 does anything weird, like suppressing the program-readable block end flag setting.

	Determine the actual purpose of global register 0x2C(is it REALLY an address multiplier?  And if so, does it affect the reverb offsets too?)

	For ADSR and volume sweep, should the divider be reset to 0 on &0x8000 == true, or should the upper bit be cleared?

	Should shift occur on all stages of ADPCM sample decoding, or only at the end?

	On the real thing, there's some kind of weirdness with ADSR when you voice on when attack_rate(raw) = 0x7F; the envelope level register is repeatedly
	reset to 0, which you can see by manual writes to the envelope level register.  Normally in the attack phase when attack_rate = 0x7F, enveloping is
	effectively stuck/paused such that the value you write is sticky and won't be replaced or reset.  Note that after you voice on, you can write a new
	attack_rate < 0x7F, and enveloping will work "normally" again shortly afterwards.  You can even write an attack_rate of 0x7F at that point to pause
	enveloping clocking.  I doubt any games rely on this, but it's something to keep in mind if we ever need greater insight as to how the SPU functions
	at a low-level in order to emulate it at cycle granularity rather than sample granularity, and it may not be a bad idea to investigate this oddity
	further and emulate it in the future regardless.

	Voice 1 and 3 waveform output writes to SPURAM might not be correct(noted due to problems reading this area of SPU RAM on the real thing
	based on my expectations of how this should work).
*/

/*
 Notes:
	All addresses(for 16-bit access, at least) within the SPU address space appear to be fully read/write as if they were RAM, though
	values at some addresses(like the envelope current value) will be "overwritten" by the sound processing at certain times.

	32-bit and 8-bit reads act as if it were RAM(not tested with all addresses, but a few, assuming the rest are the same), but 8-bit writes
	to odd addresses appear to be ignored, and 8-bit writes to even addresses are treated as 16-bit writes(most likely, but, need to code custom assembly to
	fully test the upper 8 bits).  NOTE: the preceding information doesn't necessarily cover accesses with side effects, they still need to be tested; and it
	of course covers reads/writes from the point of view of software running on the CPU.

	It doesn't appear to be possible to enable FM on the first channel/voice(channel/voice 0).

	Lower bit of channel start address appears to be masked out to 0(such that ADPCM block decoding is always 8 16-bit units, 16 bytes, aligned), as far as
	block-decoding and flag-set program-readable loop address go.
*/

/*
 Update() isn't called on Read and Writes for performance reasons, it's called with sufficient granularity from the event
 system, though this will obviously need to change if we ever emulate the SPU with better precision than per-sample(pair).
*/

#define SPUIRQ_DBG(format, ...) { printf("[SPUIRQDBG] " format " -- Voice 22 CA=0x%06x,LA=0x%06x\n", ## __VA_ARGS__, Voices[22].CurAddr, Voices[22].LoopAddr); }

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "psx.h"

#define MDFN_LIKELY(x) __builtin_expect(!!(x), 1)
#define MDFN_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define INLINE inline
#define MDFN_WARN_UNUSED_RESULT
static inline void clamp(int32_t* v, int32_t lo, int32_t hi) { if (*v < lo) *v = lo; if (*v > hi) *v = hi; }
enum
{
 ADSR_ATTACK = 0,
 ADSR_DECAY = 1,
 ADSR_SUSTAIN = 2,
 ADSR_RELEASE = 3
};

typedef struct SPU_ADSR
{
 uint16_t EnvLevel;	// We typecast it to (int16_t) in several places, but keep it here as (uint16_t) to prevent signed overflow/underflow, which compilers
			// may not treat consistently.
 uint32_t Divider;
 uint32_t Phase;

 bool AttackExp;
 bool SustainExp;
 bool SustainDec;
 bool ReleaseExp;

 int32_t AttackRate;	// Ar
 int32_t DecayRate;	// Dr * 4
 int32_t SustainRate;	// Sr
 int32_t ReleaseRate;	// Rr * 4

 int32_t SustainLevel;	// (Sl + 1) << 11
} SPU_ADSR;

typedef struct SPU_Sweep
{
 uint16_t Control;
 uint16_t Current;	// We typecast it to (int16_t) in several places, but keep it here as (uint16_t) to prevent signed overflow/underflow, which compilers
			// may not treat consistently.
 uint32_t Divider;
} SPU_Sweep;

typedef struct SPU_Voice
{
 int16_t DecodeBuffer[0x20];
 int16_t DecodeM2;
 int16_t DecodeM1;

 uint32_t DecodePlayDelay;
 uint32_t DecodeWritePos;
 uint32_t DecodeReadPos;
 uint32_t DecodeAvail;

 uint8_t DecodeShift;
 uint8_t DecodeWeight;
 uint8_t DecodeFlags;

 bool IgnoreSampLA;

 SPU_Sweep Sweep[2];

 uint16_t Pitch;
 uint32_t CurPhase;

 uint32_t StartAddr;

 uint32_t CurAddr;

 uint32_t ADSRControl;

 uint32_t LoopAddr;

 int32_t PreLRSample;	// After enveloping, but before L/R volume.  Range of -32768 to 32767

 SPU_ADSR ADSR;
} SPU_Voice;


static SPU_Voice Voices[24];
static uint32_t NoiseDivider;
static uint32_t NoiseCounter;
static uint16_t LFSR;
static uint32_t FM_Mode;
static uint32_t Noise_Mode;
static uint32_t Reverb_Mode;
static uint32_t ReverbWA;
static SPU_Sweep GlobalSweep[2];
static int32_t ReverbVol[2];
static int32_t CDVol[2];
static int32_t ExternVol[2];
static uint32_t IRQAddr;
static uint32_t RWAddr;
static uint16_t SPUControl;
static uint32_t VoiceOn;
static uint32_t VoiceOff;
static uint32_t BlockEnd;
static uint32_t CWA;
static uint16_t AuxRegs[0x10];
static int16_t RDSB[2][128];
static int16_t RUSB[2][64];
static int32_t RvbResPos;
static uint32_t ReverbCur;
static bool IRQAsserted;
static int32_t clock_divider;
static uint32_t IntermediateBufferPos;
static int16_t IntermediateBuffer[4096][2];
static uint16_t SPURAM[524288 / sizeof(uint16_t)];
static struct {
 union
 {
  uint16_t Regs[0x100];
  struct
  {
   uint16_t VoiceRegs[0xC0];
   union
   {
    uint16_t GlobalRegs[0x20];
    struct
    {
     uint16_t _Global0[0x17];
     uint16_t SPUStatus;
     uint16_t _Global1[0x08];
    };
   };
   union
   {
    uint16_t ReverbRegs[0x20];
    struct
    {
     uint16_t FB_SRC_A;
     uint16_t FB_SRC_B;
     int16_t IIR_ALPHA;
     int16_t ACC_COEF_A;
     int16_t ACC_COEF_B;
     int16_t ACC_COEF_C;
     int16_t ACC_COEF_D;
     int16_t IIR_COEF;
     int16_t FB_ALPHA;
     int16_t FB_X;
     uint16_t IIR_DEST_A[2];
     uint16_t ACC_SRC_A[2];
     uint16_t ACC_SRC_B[2];
     uint16_t IIR_SRC_A[2];
     uint16_t IIR_DEST_B[2];
     uint16_t ACC_SRC_C[2];
     uint16_t ACC_SRC_D[2];
     uint16_t IIR_SRC_B[2];
     uint16_t MIX_DEST_A[2];
     uint16_t MIX_DEST_B[2];
     int16_t IN_COEF[2];
    };
   };
  };
 };
} SR;
#define Regs SR.Regs
#define VoiceRegs SR.VoiceRegs
#define GlobalRegs SR.GlobalRegs
#define _Global0 SR._Global0
#define SPUStatus SR.SPUStatus
#define _Global1 SR._Global1
#define ReverbRegs SR.ReverbRegs
#define FB_SRC_A SR.FB_SRC_A
#define FB_SRC_B SR.FB_SRC_B
#define IIR_ALPHA SR.IIR_ALPHA
#define ACC_COEF_A SR.ACC_COEF_A
#define ACC_COEF_B SR.ACC_COEF_B
#define ACC_COEF_C SR.ACC_COEF_C
#define ACC_COEF_D SR.ACC_COEF_D
#define IIR_COEF SR.IIR_COEF
#define FB_ALPHA SR.FB_ALPHA
#define FB_X SR.FB_X
#define IIR_DEST_A SR.IIR_DEST_A
#define ACC_SRC_A SR.ACC_SRC_A
#define ACC_SRC_B SR.ACC_SRC_B
#define IIR_SRC_A SR.IIR_SRC_A
#define IIR_DEST_B SR.IIR_DEST_B
#define ACC_SRC_C SR.ACC_SRC_C
#define ACC_SRC_D SR.ACC_SRC_D
#define IIR_SRC_B SR.IIR_SRC_B
#define MIX_DEST_A SR.MIX_DEST_A
#define MIX_DEST_B SR.MIX_DEST_B
#define IN_COEF SR.IN_COEF

static void spuIrq(bool level);
static void Sweep_Power(SPU_Sweep* self);
static void Sweep_WriteControl(SPU_Sweep* self, uint16_t value);
static int16_t Sweep_ReadVolume(SPU_Sweep* self);
static void Sweep_Clock(SPU_Sweep* self);
static void Sweep_WriteVolume(SPU_Sweep* self, int16_t value);
static int16_t RD_RVB2(uint16_t raw_offs, int32_t extra_offs);
static void WR_RVB(uint16_t raw_offs, int16_t sample);
static void RunDecoder(SPU_Voice *voice);
static void CacheEnvelope(SPU_Voice *voice);
static void ResetEnvelope(SPU_Voice *voice);
static void ReleaseEnvelope(SPU_Voice *voice);
static void RunEnvelope(SPU_Voice *voice);
static void CheckIRQAddr(uint32_t addr);
static void WriteSPURAM(uint32_t addr, uint16_t value);
static uint16_t ReadSPURAM(uint32_t addr);
static uint32_t Get_Reverb_Offset(uint32_t in_offset);
static void RunReverb(const int32_t* in, int32_t* out);
static void RunNoise(void);
static const int16_t FIR_Table[256][4] =
{
  { (int16_t)0x12c7, (int16_t)0x59b3, (int16_t)0x1307, (int16_t)0xffff },
 { (int16_t)0x1288, (int16_t)0x59b2, (int16_t)0x1347, (int16_t)0xffff },
 { (int16_t)0x1249, (int16_t)0x59b0, (int16_t)0x1388, (int16_t)0xffff },
 { (int16_t)0x120b, (int16_t)0x59ad, (int16_t)0x13c9, (int16_t)0xffff },
 { (int16_t)0x11cd, (int16_t)0x59a9, (int16_t)0x140b, (int16_t)0xffff },
 { (int16_t)0x118f, (int16_t)0x59a4, (int16_t)0x144d, (int16_t)0xffff },
 { (int16_t)0x1153, (int16_t)0x599e, (int16_t)0x1490, (int16_t)0xffff },
 { (int16_t)0x1116, (int16_t)0x5997, (int16_t)0x14d4, (int16_t)0xffff },
 { (int16_t)0x10db, (int16_t)0x598f, (int16_t)0x1517, (int16_t)0xffff },
 { (int16_t)0x109f, (int16_t)0x5986, (int16_t)0x155c, (int16_t)0xffff },
 { (int16_t)0x1065, (int16_t)0x597c, (int16_t)0x15a0, (int16_t)0xffff },
 { (int16_t)0x102a, (int16_t)0x5971, (int16_t)0x15e6, (int16_t)0xffff },
 { (int16_t)0x0ff1, (int16_t)0x5965, (int16_t)0x162c, (int16_t)0xffff },
 { (int16_t)0x0fb7, (int16_t)0x5958, (int16_t)0x1672, (int16_t)0xffff },
 { (int16_t)0x0f7f, (int16_t)0x5949, (int16_t)0x16b9, (int16_t)0xffff },
 { (int16_t)0x0f46, (int16_t)0x593a, (int16_t)0x1700, (int16_t)0xffff },
 { (int16_t)0x0f0f, (int16_t)0x592a, (int16_t)0x1747, (int16_t)0x0000 },
 { (int16_t)0x0ed7, (int16_t)0x5919, (int16_t)0x1790, (int16_t)0x0000 },
 { (int16_t)0x0ea1, (int16_t)0x5907, (int16_t)0x17d8, (int16_t)0x0000 },
 { (int16_t)0x0e6b, (int16_t)0x58f4, (int16_t)0x1821, (int16_t)0x0000 },
 { (int16_t)0x0e35, (int16_t)0x58e0, (int16_t)0x186b, (int16_t)0x0000 },
 { (int16_t)0x0e00, (int16_t)0x58cb, (int16_t)0x18b5, (int16_t)0x0000 },
 { (int16_t)0x0dcb, (int16_t)0x58b5, (int16_t)0x1900, (int16_t)0x0000 },
 { (int16_t)0x0d97, (int16_t)0x589e, (int16_t)0x194b, (int16_t)0x0001 },
 { (int16_t)0x0d63, (int16_t)0x5886, (int16_t)0x1996, (int16_t)0x0001 },
 { (int16_t)0x0d30, (int16_t)0x586d, (int16_t)0x19e2, (int16_t)0x0001 },
 { (int16_t)0x0cfd, (int16_t)0x5853, (int16_t)0x1a2e, (int16_t)0x0001 },
 { (int16_t)0x0ccb, (int16_t)0x5838, (int16_t)0x1a7b, (int16_t)0x0002 },
 { (int16_t)0x0c99, (int16_t)0x581c, (int16_t)0x1ac8, (int16_t)0x0002 },
 { (int16_t)0x0c68, (int16_t)0x57ff, (int16_t)0x1b16, (int16_t)0x0002 },
 { (int16_t)0x0c38, (int16_t)0x57e2, (int16_t)0x1b64, (int16_t)0x0003 },
 { (int16_t)0x0c07, (int16_t)0x57c3, (int16_t)0x1bb3, (int16_t)0x0003 },
 { (int16_t)0x0bd8, (int16_t)0x57a3, (int16_t)0x1c02, (int16_t)0x0003 },
 { (int16_t)0x0ba9, (int16_t)0x5782, (int16_t)0x1c51, (int16_t)0x0004 },
 { (int16_t)0x0b7a, (int16_t)0x5761, (int16_t)0x1ca1, (int16_t)0x0004 },
 { (int16_t)0x0b4c, (int16_t)0x573e, (int16_t)0x1cf1, (int16_t)0x0005 },
 { (int16_t)0x0b1e, (int16_t)0x571b, (int16_t)0x1d42, (int16_t)0x0005 },
 { (int16_t)0x0af1, (int16_t)0x56f6, (int16_t)0x1d93, (int16_t)0x0006 },
 { (int16_t)0x0ac4, (int16_t)0x56d1, (int16_t)0x1de5, (int16_t)0x0007 },
 { (int16_t)0x0a98, (int16_t)0x56ab, (int16_t)0x1e37, (int16_t)0x0007 },
 { (int16_t)0x0a6c, (int16_t)0x5684, (int16_t)0x1e89, (int16_t)0x0008 },
 { (int16_t)0x0a40, (int16_t)0x565b, (int16_t)0x1edc, (int16_t)0x0009 },
 { (int16_t)0x0a16, (int16_t)0x5632, (int16_t)0x1f2f, (int16_t)0x0009 },
 { (int16_t)0x09eb, (int16_t)0x5609, (int16_t)0x1f82, (int16_t)0x000a },
 { (int16_t)0x09c1, (int16_t)0x55de, (int16_t)0x1fd6, (int16_t)0x000b },
 { (int16_t)0x0998, (int16_t)0x55b2, (int16_t)0x202a, (int16_t)0x000c },
 { (int16_t)0x096f, (int16_t)0x5585, (int16_t)0x207f, (int16_t)0x000d },
 { (int16_t)0x0946, (int16_t)0x5558, (int16_t)0x20d4, (int16_t)0x000e },
 { (int16_t)0x091e, (int16_t)0x5529, (int16_t)0x2129, (int16_t)0x000f },
 { (int16_t)0x08f7, (int16_t)0x54fa, (int16_t)0x217f, (int16_t)0x0010 },
 { (int16_t)0x08d0, (int16_t)0x54ca, (int16_t)0x21d5, (int16_t)0x0011 },
 { (int16_t)0x08a9, (int16_t)0x5499, (int16_t)0x222c, (int16_t)0x0012 },
 { (int16_t)0x0883, (int16_t)0x5467, (int16_t)0x2282, (int16_t)0x0013 },
 { (int16_t)0x085d, (int16_t)0x5434, (int16_t)0x22da, (int16_t)0x0015 },
 { (int16_t)0x0838, (int16_t)0x5401, (int16_t)0x2331, (int16_t)0x0016 },
 { (int16_t)0x0813, (int16_t)0x53cc, (int16_t)0x2389, (int16_t)0x0018 },
 { (int16_t)0x07ef, (int16_t)0x5397, (int16_t)0x23e1, (int16_t)0x0019 },
 { (int16_t)0x07cb, (int16_t)0x5361, (int16_t)0x2439, (int16_t)0x001b },
 { (int16_t)0x07a7, (int16_t)0x532a, (int16_t)0x2492, (int16_t)0x001c },
 { (int16_t)0x0784, (int16_t)0x52f3, (int16_t)0x24eb, (int16_t)0x001e },
 { (int16_t)0x0762, (int16_t)0x52ba, (int16_t)0x2545, (int16_t)0x0020 },
 { (int16_t)0x0740, (int16_t)0x5281, (int16_t)0x259e, (int16_t)0x0021 },
 { (int16_t)0x071e, (int16_t)0x5247, (int16_t)0x25f8, (int16_t)0x0023 },
 { (int16_t)0x06fd, (int16_t)0x520c, (int16_t)0x2653, (int16_t)0x0025 },
 { (int16_t)0x06dc, (int16_t)0x51d0, (int16_t)0x26ad, (int16_t)0x0027 },
 { (int16_t)0x06bb, (int16_t)0x5194, (int16_t)0x2708, (int16_t)0x0029 },
 { (int16_t)0x069b, (int16_t)0x5156, (int16_t)0x2763, (int16_t)0x002c },
 { (int16_t)0x067c, (int16_t)0x5118, (int16_t)0x27be, (int16_t)0x002e },
 { (int16_t)0x065c, (int16_t)0x50da, (int16_t)0x281a, (int16_t)0x0030 },
 { (int16_t)0x063e, (int16_t)0x509a, (int16_t)0x2876, (int16_t)0x0033 },
 { (int16_t)0x061f, (int16_t)0x505a, (int16_t)0x28d2, (int16_t)0x0035 },
 { (int16_t)0x0601, (int16_t)0x5019, (int16_t)0x292e, (int16_t)0x0038 },
 { (int16_t)0x05e4, (int16_t)0x4fd7, (int16_t)0x298b, (int16_t)0x003a },
 { (int16_t)0x05c7, (int16_t)0x4f95, (int16_t)0x29e7, (int16_t)0x003d },
 { (int16_t)0x05aa, (int16_t)0x4f52, (int16_t)0x2a44, (int16_t)0x0040 },
 { (int16_t)0x058e, (int16_t)0x4f0e, (int16_t)0x2aa1, (int16_t)0x0043 },
 { (int16_t)0x0572, (int16_t)0x4ec9, (int16_t)0x2aff, (int16_t)0x0046 },
 { (int16_t)0x0556, (int16_t)0x4e84, (int16_t)0x2b5c, (int16_t)0x0049 },
 { (int16_t)0x053b, (int16_t)0x4e3e, (int16_t)0x2bba, (int16_t)0x004d },
 { (int16_t)0x0520, (int16_t)0x4df7, (int16_t)0x2c18, (int16_t)0x0050 },
 { (int16_t)0x0506, (int16_t)0x4db0, (int16_t)0x2c76, (int16_t)0x0054 },
 { (int16_t)0x04ec, (int16_t)0x4d68, (int16_t)0x2cd4, (int16_t)0x0057 },
 { (int16_t)0x04d2, (int16_t)0x4d20, (int16_t)0x2d33, (int16_t)0x005b },
 { (int16_t)0x04b9, (int16_t)0x4cd7, (int16_t)0x2d91, (int16_t)0x005f },
 { (int16_t)0x04a0, (int16_t)0x4c8d, (int16_t)0x2df0, (int16_t)0x0063 },
 { (int16_t)0x0488, (int16_t)0x4c42, (int16_t)0x2e4f, (int16_t)0x0067 },
 { (int16_t)0x0470, (int16_t)0x4bf7, (int16_t)0x2eae, (int16_t)0x006b },
 { (int16_t)0x0458, (int16_t)0x4bac, (int16_t)0x2f0d, (int16_t)0x006f },
 { (int16_t)0x0441, (int16_t)0x4b5f, (int16_t)0x2f6c, (int16_t)0x0074 },
 { (int16_t)0x042a, (int16_t)0x4b13, (int16_t)0x2fcc, (int16_t)0x0078 },
 { (int16_t)0x0413, (int16_t)0x4ac5, (int16_t)0x302b, (int16_t)0x007d },
 { (int16_t)0x03fc, (int16_t)0x4a77, (int16_t)0x308b, (int16_t)0x0082 },
 { (int16_t)0x03e7, (int16_t)0x4a29, (int16_t)0x30ea, (int16_t)0x0087 },
 { (int16_t)0x03d1, (int16_t)0x49d9, (int16_t)0x314a, (int16_t)0x008c },
 { (int16_t)0x03bc, (int16_t)0x498a, (int16_t)0x31aa, (int16_t)0x0091 },
 { (int16_t)0x03a7, (int16_t)0x493a, (int16_t)0x3209, (int16_t)0x0096 },
 { (int16_t)0x0392, (int16_t)0x48e9, (int16_t)0x3269, (int16_t)0x009c },
 { (int16_t)0x037e, (int16_t)0x4898, (int16_t)0x32c9, (int16_t)0x00a1 },
 { (int16_t)0x036a, (int16_t)0x4846, (int16_t)0x3329, (int16_t)0x00a7 },
 { (int16_t)0x0356, (int16_t)0x47f4, (int16_t)0x3389, (int16_t)0x00ad },
 { (int16_t)0x0343, (int16_t)0x47a1, (int16_t)0x33e9, (int16_t)0x00b3 },
 { (int16_t)0x0330, (int16_t)0x474e, (int16_t)0x3449, (int16_t)0x00ba },
 { (int16_t)0x031d, (int16_t)0x46fa, (int16_t)0x34a9, (int16_t)0x00c0 },
 { (int16_t)0x030b, (int16_t)0x46a6, (int16_t)0x3509, (int16_t)0x00c7 },
 { (int16_t)0x02f9, (int16_t)0x4651, (int16_t)0x3569, (int16_t)0x00cd },
 { (int16_t)0x02e7, (int16_t)0x45fc, (int16_t)0x35c9, (int16_t)0x00d4 },
 { (int16_t)0x02d6, (int16_t)0x45a6, (int16_t)0x3629, (int16_t)0x00db },
 { (int16_t)0x02c4, (int16_t)0x4550, (int16_t)0x3689, (int16_t)0x00e3 },
 { (int16_t)0x02b4, (int16_t)0x44fa, (int16_t)0x36e8, (int16_t)0x00ea },
 { (int16_t)0x02a3, (int16_t)0x44a3, (int16_t)0x3748, (int16_t)0x00f2 },
 { (int16_t)0x0293, (int16_t)0x444c, (int16_t)0x37a8, (int16_t)0x00fa },
 { (int16_t)0x0283, (int16_t)0x43f4, (int16_t)0x3807, (int16_t)0x0101 },
 { (int16_t)0x0273, (int16_t)0x439c, (int16_t)0x3867, (int16_t)0x010a },
 { (int16_t)0x0264, (int16_t)0x4344, (int16_t)0x38c6, (int16_t)0x0112 },
 { (int16_t)0x0255, (int16_t)0x42eb, (int16_t)0x3926, (int16_t)0x011b },
 { (int16_t)0x0246, (int16_t)0x4292, (int16_t)0x3985, (int16_t)0x0123 },
 { (int16_t)0x0237, (int16_t)0x4239, (int16_t)0x39e4, (int16_t)0x012c },
 { (int16_t)0x0229, (int16_t)0x41df, (int16_t)0x3a43, (int16_t)0x0135 },
 { (int16_t)0x021b, (int16_t)0x4185, (int16_t)0x3aa2, (int16_t)0x013f },
 { (int16_t)0x020d, (int16_t)0x412a, (int16_t)0x3b00, (int16_t)0x0148 },
 { (int16_t)0x0200, (int16_t)0x40d0, (int16_t)0x3b5f, (int16_t)0x0152 },
 { (int16_t)0x01f2, (int16_t)0x4074, (int16_t)0x3bbd, (int16_t)0x015c },
 { (int16_t)0x01e5, (int16_t)0x4019, (int16_t)0x3c1b, (int16_t)0x0166 },
 { (int16_t)0x01d9, (int16_t)0x3fbd, (int16_t)0x3c79, (int16_t)0x0171 },
 { (int16_t)0x01cc, (int16_t)0x3f62, (int16_t)0x3cd7, (int16_t)0x017b },
 { (int16_t)0x01c0, (int16_t)0x3f05, (int16_t)0x3d35, (int16_t)0x0186 },
 { (int16_t)0x01b4, (int16_t)0x3ea9, (int16_t)0x3d92, (int16_t)0x0191 },
 { (int16_t)0x01a8, (int16_t)0x3e4c, (int16_t)0x3def, (int16_t)0x019c },
 { (int16_t)0x019c, (int16_t)0x3def, (int16_t)0x3e4c, (int16_t)0x01a8 },
 { (int16_t)0x0191, (int16_t)0x3d92, (int16_t)0x3ea9, (int16_t)0x01b4 },
 { (int16_t)0x0186, (int16_t)0x3d35, (int16_t)0x3f05, (int16_t)0x01c0 },
 { (int16_t)0x017b, (int16_t)0x3cd7, (int16_t)0x3f62, (int16_t)0x01cc },
 { (int16_t)0x0171, (int16_t)0x3c79, (int16_t)0x3fbd, (int16_t)0x01d9 },
 { (int16_t)0x0166, (int16_t)0x3c1b, (int16_t)0x4019, (int16_t)0x01e5 },
 { (int16_t)0x015c, (int16_t)0x3bbd, (int16_t)0x4074, (int16_t)0x01f2 },
 { (int16_t)0x0152, (int16_t)0x3b5f, (int16_t)0x40d0, (int16_t)0x0200 },
 { (int16_t)0x0148, (int16_t)0x3b00, (int16_t)0x412a, (int16_t)0x020d },
 { (int16_t)0x013f, (int16_t)0x3aa2, (int16_t)0x4185, (int16_t)0x021b },
 { (int16_t)0x0135, (int16_t)0x3a43, (int16_t)0x41df, (int16_t)0x0229 },
 { (int16_t)0x012c, (int16_t)0x39e4, (int16_t)0x4239, (int16_t)0x0237 },
 { (int16_t)0x0123, (int16_t)0x3985, (int16_t)0x4292, (int16_t)0x0246 },
 { (int16_t)0x011b, (int16_t)0x3926, (int16_t)0x42eb, (int16_t)0x0255 },
 { (int16_t)0x0112, (int16_t)0x38c6, (int16_t)0x4344, (int16_t)0x0264 },
 { (int16_t)0x010a, (int16_t)0x3867, (int16_t)0x439c, (int16_t)0x0273 },
 { (int16_t)0x0101, (int16_t)0x3807, (int16_t)0x43f4, (int16_t)0x0283 },
 { (int16_t)0x00fa, (int16_t)0x37a8, (int16_t)0x444c, (int16_t)0x0293 },
 { (int16_t)0x00f2, (int16_t)0x3748, (int16_t)0x44a3, (int16_t)0x02a3 },
 { (int16_t)0x00ea, (int16_t)0x36e8, (int16_t)0x44fa, (int16_t)0x02b4 },
 { (int16_t)0x00e3, (int16_t)0x3689, (int16_t)0x4550, (int16_t)0x02c4 },
 { (int16_t)0x00db, (int16_t)0x3629, (int16_t)0x45a6, (int16_t)0x02d6 },
 { (int16_t)0x00d4, (int16_t)0x35c9, (int16_t)0x45fc, (int16_t)0x02e7 },
 { (int16_t)0x00cd, (int16_t)0x3569, (int16_t)0x4651, (int16_t)0x02f9 },
 { (int16_t)0x00c7, (int16_t)0x3509, (int16_t)0x46a6, (int16_t)0x030b },
 { (int16_t)0x00c0, (int16_t)0x34a9, (int16_t)0x46fa, (int16_t)0x031d },
 { (int16_t)0x00ba, (int16_t)0x3449, (int16_t)0x474e, (int16_t)0x0330 },
 { (int16_t)0x00b3, (int16_t)0x33e9, (int16_t)0x47a1, (int16_t)0x0343 },
 { (int16_t)0x00ad, (int16_t)0x3389, (int16_t)0x47f4, (int16_t)0x0356 },
 { (int16_t)0x00a7, (int16_t)0x3329, (int16_t)0x4846, (int16_t)0x036a },
 { (int16_t)0x00a1, (int16_t)0x32c9, (int16_t)0x4898, (int16_t)0x037e },
 { (int16_t)0x009c, (int16_t)0x3269, (int16_t)0x48e9, (int16_t)0x0392 },
 { (int16_t)0x0096, (int16_t)0x3209, (int16_t)0x493a, (int16_t)0x03a7 },
 { (int16_t)0x0091, (int16_t)0x31aa, (int16_t)0x498a, (int16_t)0x03bc },
 { (int16_t)0x008c, (int16_t)0x314a, (int16_t)0x49d9, (int16_t)0x03d1 },
 { (int16_t)0x0087, (int16_t)0x30ea, (int16_t)0x4a29, (int16_t)0x03e7 },
 { (int16_t)0x0082, (int16_t)0x308b, (int16_t)0x4a77, (int16_t)0x03fc },
 { (int16_t)0x007d, (int16_t)0x302b, (int16_t)0x4ac5, (int16_t)0x0413 },
 { (int16_t)0x0078, (int16_t)0x2fcc, (int16_t)0x4b13, (int16_t)0x042a },
 { (int16_t)0x0074, (int16_t)0x2f6c, (int16_t)0x4b5f, (int16_t)0x0441 },
 { (int16_t)0x006f, (int16_t)0x2f0d, (int16_t)0x4bac, (int16_t)0x0458 },
 { (int16_t)0x006b, (int16_t)0x2eae, (int16_t)0x4bf7, (int16_t)0x0470 },
 { (int16_t)0x0067, (int16_t)0x2e4f, (int16_t)0x4c42, (int16_t)0x0488 },
 { (int16_t)0x0063, (int16_t)0x2df0, (int16_t)0x4c8d, (int16_t)0x04a0 },
 { (int16_t)0x005f, (int16_t)0x2d91, (int16_t)0x4cd7, (int16_t)0x04b9 },
 { (int16_t)0x005b, (int16_t)0x2d33, (int16_t)0x4d20, (int16_t)0x04d2 },
 { (int16_t)0x0057, (int16_t)0x2cd4, (int16_t)0x4d68, (int16_t)0x04ec },
 { (int16_t)0x0054, (int16_t)0x2c76, (int16_t)0x4db0, (int16_t)0x0506 },
 { (int16_t)0x0050, (int16_t)0x2c18, (int16_t)0x4df7, (int16_t)0x0520 },
 { (int16_t)0x004d, (int16_t)0x2bba, (int16_t)0x4e3e, (int16_t)0x053b },
 { (int16_t)0x0049, (int16_t)0x2b5c, (int16_t)0x4e84, (int16_t)0x0556 },
 { (int16_t)0x0046, (int16_t)0x2aff, (int16_t)0x4ec9, (int16_t)0x0572 },
 { (int16_t)0x0043, (int16_t)0x2aa1, (int16_t)0x4f0e, (int16_t)0x058e },
 { (int16_t)0x0040, (int16_t)0x2a44, (int16_t)0x4f52, (int16_t)0x05aa },
 { (int16_t)0x003d, (int16_t)0x29e7, (int16_t)0x4f95, (int16_t)0x05c7 },
 { (int16_t)0x003a, (int16_t)0x298b, (int16_t)0x4fd7, (int16_t)0x05e4 },
 { (int16_t)0x0038, (int16_t)0x292e, (int16_t)0x5019, (int16_t)0x0601 },
 { (int16_t)0x0035, (int16_t)0x28d2, (int16_t)0x505a, (int16_t)0x061f },
 { (int16_t)0x0033, (int16_t)0x2876, (int16_t)0x509a, (int16_t)0x063e },
 { (int16_t)0x0030, (int16_t)0x281a, (int16_t)0x50da, (int16_t)0x065c },
 { (int16_t)0x002e, (int16_t)0x27be, (int16_t)0x5118, (int16_t)0x067c },
 { (int16_t)0x002c, (int16_t)0x2763, (int16_t)0x5156, (int16_t)0x069b },
 { (int16_t)0x0029, (int16_t)0x2708, (int16_t)0x5194, (int16_t)0x06bb },
 { (int16_t)0x0027, (int16_t)0x26ad, (int16_t)0x51d0, (int16_t)0x06dc },
 { (int16_t)0x0025, (int16_t)0x2653, (int16_t)0x520c, (int16_t)0x06fd },
 { (int16_t)0x0023, (int16_t)0x25f8, (int16_t)0x5247, (int16_t)0x071e },
 { (int16_t)0x0021, (int16_t)0x259e, (int16_t)0x5281, (int16_t)0x0740 },
 { (int16_t)0x0020, (int16_t)0x2545, (int16_t)0x52ba, (int16_t)0x0762 },
 { (int16_t)0x001e, (int16_t)0x24eb, (int16_t)0x52f3, (int16_t)0x0784 },
 { (int16_t)0x001c, (int16_t)0x2492, (int16_t)0x532a, (int16_t)0x07a7 },
 { (int16_t)0x001b, (int16_t)0x2439, (int16_t)0x5361, (int16_t)0x07cb },
 { (int16_t)0x0019, (int16_t)0x23e1, (int16_t)0x5397, (int16_t)0x07ef },
 { (int16_t)0x0018, (int16_t)0x2389, (int16_t)0x53cc, (int16_t)0x0813 },
 { (int16_t)0x0016, (int16_t)0x2331, (int16_t)0x5401, (int16_t)0x0838 },
 { (int16_t)0x0015, (int16_t)0x22da, (int16_t)0x5434, (int16_t)0x085d },
 { (int16_t)0x0013, (int16_t)0x2282, (int16_t)0x5467, (int16_t)0x0883 },
 { (int16_t)0x0012, (int16_t)0x222c, (int16_t)0x5499, (int16_t)0x08a9 },
 { (int16_t)0x0011, (int16_t)0x21d5, (int16_t)0x54ca, (int16_t)0x08d0 },
 { (int16_t)0x0010, (int16_t)0x217f, (int16_t)0x54fa, (int16_t)0x08f7 },
 { (int16_t)0x000f, (int16_t)0x2129, (int16_t)0x5529, (int16_t)0x091e },
 { (int16_t)0x000e, (int16_t)0x20d4, (int16_t)0x5558, (int16_t)0x0946 },
 { (int16_t)0x000d, (int16_t)0x207f, (int16_t)0x5585, (int16_t)0x096f },
 { (int16_t)0x000c, (int16_t)0x202a, (int16_t)0x55b2, (int16_t)0x0998 },
 { (int16_t)0x000b, (int16_t)0x1fd6, (int16_t)0x55de, (int16_t)0x09c1 },
 { (int16_t)0x000a, (int16_t)0x1f82, (int16_t)0x5609, (int16_t)0x09eb },
 { (int16_t)0x0009, (int16_t)0x1f2f, (int16_t)0x5632, (int16_t)0x0a16 },
 { (int16_t)0x0009, (int16_t)0x1edc, (int16_t)0x565b, (int16_t)0x0a40 },
 { (int16_t)0x0008, (int16_t)0x1e89, (int16_t)0x5684, (int16_t)0x0a6c },
 { (int16_t)0x0007, (int16_t)0x1e37, (int16_t)0x56ab, (int16_t)0x0a98 },
 { (int16_t)0x0007, (int16_t)0x1de5, (int16_t)0x56d1, (int16_t)0x0ac4 },
 { (int16_t)0x0006, (int16_t)0x1d93, (int16_t)0x56f6, (int16_t)0x0af1 },
 { (int16_t)0x0005, (int16_t)0x1d42, (int16_t)0x571b, (int16_t)0x0b1e },
 { (int16_t)0x0005, (int16_t)0x1cf1, (int16_t)0x573e, (int16_t)0x0b4c },
 { (int16_t)0x0004, (int16_t)0x1ca1, (int16_t)0x5761, (int16_t)0x0b7a },
 { (int16_t)0x0004, (int16_t)0x1c51, (int16_t)0x5782, (int16_t)0x0ba9 },
 { (int16_t)0x0003, (int16_t)0x1c02, (int16_t)0x57a3, (int16_t)0x0bd8 },
 { (int16_t)0x0003, (int16_t)0x1bb3, (int16_t)0x57c3, (int16_t)0x0c07 },
 { (int16_t)0x0003, (int16_t)0x1b64, (int16_t)0x57e2, (int16_t)0x0c38 },
 { (int16_t)0x0002, (int16_t)0x1b16, (int16_t)0x57ff, (int16_t)0x0c68 },
 { (int16_t)0x0002, (int16_t)0x1ac8, (int16_t)0x581c, (int16_t)0x0c99 },
 { (int16_t)0x0002, (int16_t)0x1a7b, (int16_t)0x5838, (int16_t)0x0ccb },
 { (int16_t)0x0001, (int16_t)0x1a2e, (int16_t)0x5853, (int16_t)0x0cfd },
 { (int16_t)0x0001, (int16_t)0x19e2, (int16_t)0x586d, (int16_t)0x0d30 },
 { (int16_t)0x0001, (int16_t)0x1996, (int16_t)0x5886, (int16_t)0x0d63 },
 { (int16_t)0x0001, (int16_t)0x194b, (int16_t)0x589e, (int16_t)0x0d97 },
 { (int16_t)0x0000, (int16_t)0x1900, (int16_t)0x58b5, (int16_t)0x0dcb },
 { (int16_t)0x0000, (int16_t)0x18b5, (int16_t)0x58cb, (int16_t)0x0e00 },
 { (int16_t)0x0000, (int16_t)0x186b, (int16_t)0x58e0, (int16_t)0x0e35 },
 { (int16_t)0x0000, (int16_t)0x1821, (int16_t)0x58f4, (int16_t)0x0e6b },
 { (int16_t)0x0000, (int16_t)0x17d8, (int16_t)0x5907, (int16_t)0x0ea1 },
 { (int16_t)0x0000, (int16_t)0x1790, (int16_t)0x5919, (int16_t)0x0ed7 },
 { (int16_t)0x0000, (int16_t)0x1747, (int16_t)0x592a, (int16_t)0x0f0f },
 { (int16_t)0xffff, (int16_t)0x1700, (int16_t)0x593a, (int16_t)0x0f46 },
 { (int16_t)0xffff, (int16_t)0x16b9, (int16_t)0x5949, (int16_t)0x0f7f },
 { (int16_t)0xffff, (int16_t)0x1672, (int16_t)0x5958, (int16_t)0x0fb7 },
 { (int16_t)0xffff, (int16_t)0x162c, (int16_t)0x5965, (int16_t)0x0ff1 },
 { (int16_t)0xffff, (int16_t)0x15e6, (int16_t)0x5971, (int16_t)0x102a },
 { (int16_t)0xffff, (int16_t)0x15a0, (int16_t)0x597c, (int16_t)0x1065 },
 { (int16_t)0xffff, (int16_t)0x155c, (int16_t)0x5986, (int16_t)0x109f },
 { (int16_t)0xffff, (int16_t)0x1517, (int16_t)0x598f, (int16_t)0x10db },
 { (int16_t)0xffff, (int16_t)0x14d4, (int16_t)0x5997, (int16_t)0x1116 },
 { (int16_t)0xffff, (int16_t)0x1490, (int16_t)0x599e, (int16_t)0x1153 },
 { (int16_t)0xffff, (int16_t)0x144d, (int16_t)0x59a4, (int16_t)0x118f },
 { (int16_t)0xffff, (int16_t)0x140b, (int16_t)0x59a9, (int16_t)0x11cd },
 { (int16_t)0xffff, (int16_t)0x13c9, (int16_t)0x59ad, (int16_t)0x120b },
 { (int16_t)0xffff, (int16_t)0x1388, (int16_t)0x59b0, (int16_t)0x1249 },
 { (int16_t)0xffff, (int16_t)0x1347, (int16_t)0x59b2, (int16_t)0x1288 },
 { (int16_t)0xffff, (int16_t)0x1307, (int16_t)0x59b3, (int16_t)0x12c7 },

};



void SPU_Power(void)
{
 clock_divider = 768;

 memset(SPURAM, 0, sizeof(SPURAM));

 for(int i = 0; i < 24; i++)
 {
  memset(Voices[i].DecodeBuffer, 0, sizeof(Voices[i].DecodeBuffer));
  Voices[i].DecodeM2 = 0;
  Voices[i].DecodeM1 = 0;

  Voices[i].DecodePlayDelay = 0;
  Voices[i].DecodeWritePos = 0;
  Voices[i].DecodeReadPos = 0;
  Voices[i].DecodeAvail = 0;

  Voices[i].DecodeShift = 0;
  Voices[i].DecodeWeight = 0;
  Voices[i].DecodeFlags = 0;

  Voices[i].IgnoreSampLA = false;

  Sweep_Power(&Voices[i].Sweep[0]);
  Sweep_Power(&Voices[i].Sweep[1]);

  Voices[i].Pitch = 0;
  Voices[i].CurPhase = 0;

  Voices[i].StartAddr = 0;

  Voices[i].CurAddr = 0;

  Voices[i].ADSRControl = 0;

  Voices[i].LoopAddr = 0;

  Voices[i].PreLRSample = 0;

  memset(&Voices[i].ADSR, 0, sizeof(SPU_ADSR));
 }

 Sweep_Power(&GlobalSweep[0]);
 Sweep_Power(&GlobalSweep[1]);

 NoiseDivider = 0;
 NoiseCounter = 0;
 LFSR = 0;

 FM_Mode = 0;
 Noise_Mode = 0;
 Reverb_Mode = 0;
 ReverbWA = 0;

 ReverbVol[0] = ReverbVol[1] = 0;

 CDVol[0] = CDVol[1] = 0;

 ExternVol[0] = ExternVol[1] = 0;

 IRQAddr = 0;

 RWAddr = 0;

 SPUControl = 0;

 VoiceOn = 0;
 VoiceOff = 0;

 BlockEnd = 0;

 CWA = 0;

 memset(Regs, 0, sizeof(Regs));
 memset(AuxRegs, 0, sizeof(AuxRegs));
 memset(RDSB, 0, sizeof(RDSB));
 memset(RUSB, 0, sizeof(RUSB));
 RvbResPos = 0;

 ReverbCur = ReverbWA;

 IRQAsserted = false;
}

static INLINE void CalcVCDelta(const uint8_t zs, uint8_t speed, bool log_mode, bool dec_mode, bool inv_increment, int16_t Current, int *increment, int *divinco)
{
  (*increment) = (7 - (speed & 0x3));

  if(inv_increment)
   (*increment) = ~(*increment);

  (*divinco) = 32768;

  if(speed < 0x2C)
   (*increment) = (unsigned)(*increment) << ((0x2F - speed) >> 2);

  if(speed >= 0x30)
   (*divinco) >>= (speed - 0x2C) >> 2;

  if(log_mode)
  {
   if(dec_mode)	// Log decrement mode
    (*increment) = (Current * (*increment)) >> 15;
   else			// Log (*increment) mode
   {
    if((Current & 0x7FFF) >= 0x6000)
    {
     if(speed < 0x28)
      (*increment) >>= 2;
     else if(speed >= 0x2C)
      (*divinco) >>= 2;
     else	// 0x28 ... 0x2B
     {
      (*increment) >>= 1;
      (*divinco) >>= 1;
     }
    }
   }
  } // end if(log_mode)

  if((*divinco) == 0 && speed < zs) //0x7F)
   (*divinco) = 1;
}


static void Sweep_Power(SPU_Sweep* self)
{
 self->Control = 0;
 self->Current = 0;
 self->Divider = 0;
}

static void Sweep_WriteControl(SPU_Sweep* self, uint16_t value)
{
 self->Control = value;
}

static int16_t Sweep_ReadVolume(SPU_Sweep* self)
{
 return((int16_t)self->Current);
}

static void Sweep_Clock(SPU_Sweep* self)
{
 if(!(self->Control & 0x8000))
 {
  self->Current = (self->Control & 0x7FFF) << 1;
  return;
 }

 if(self->Control & 0x8000) 	// Sweep enabled
 {
  const bool log_mode = (bool)(self->Control & 0x4000);
  const bool dec_mode = (bool)(self->Control & 0x2000);
  const bool inv_mode = (bool)(self->Control & 0x1000);
  const bool inv_increment = (dec_mode ^ inv_mode) | (dec_mode & log_mode);
  const uint16_t vc_cv_xor = (inv_mode & !(dec_mode & log_mode)) ? 0xFFFF : 0x0000;
  const uint16_t TestInvert = inv_mode ? 0xFFFF : 0x0000;
  int increment;
  int divinco;

  CalcVCDelta(0x7F, self->Control & 0x7F, log_mode, dec_mode, inv_increment, (int16_t)(self->Current ^ vc_cv_xor), &increment, &divinco);
  //printf("%d %d\n", divinco, increment);

  if((dec_mode & !(inv_mode & log_mode)) && ((self->Current & 0x8000) == (inv_mode ? 0x0000 : 0x8000) || (self->Current == 0)))
  {
   //
   // Not sure if this condition should stop the self->Divider adding or force the increment value to 0.
   //
   self->Current = 0;
  }
  else
  {
   self->Divider += divinco;

   if(self->Divider & 0x8000)
   {
    self->Divider = 0;

    if(dec_mode || ((self->Current ^ TestInvert) != 0x7FFF))
    {
     uint16_t PrevCurrent = self->Current;
     self->Current = self->Current + increment;

     //printf("%04x %04x\n", PrevCurrent, self->Current);

     if(!dec_mode && ((self->Current ^ PrevCurrent) & 0x8000) && ((self->Current ^ TestInvert) & 0x8000))
      self->Current = 0x7FFF ^ TestInvert;
    }
   }
  }
 }
}

static void Sweep_WriteVolume(SPU_Sweep* self, int16_t value)
{
 self->Current = value;
}


//
// Take care not to trigger SPU IRQ for the next block before its decoding start.
//
static void RunDecoder(SPU_Voice *voice)
{
 // 5 through 0xF appear to be 0 on the real thing.
 static const int32_t Weights[16][2] =
 {
  // s-1    s-2
  {   0,    0 },
  {  60,    0 },
  { 115,  -52 },
  {  98,  -55 },
  { 122,  -60 },
 };

 if(voice->DecodeAvail >= 11)
 {
  if(SPUControl & 0x40)
  {
   unsigned test_addr = (voice->CurAddr - 1) & 0x3FFFF;
   if(IRQAddr == test_addr || IRQAddr == (test_addr & 0x3FFF8))
   {
    IRQAsserted = true;
    spuIrq(IRQAsserted);
   }
  }
  return;
 }

 if((voice->CurAddr & 0x7) == 0)
 {
  //
  // Handle delayed flags from the previously-decoded block.
  //
  // NOTE: The timing of setting the BlockEnd bit here, and forcing ADSR envelope volume to 0, is a bit late.  (And I'm not sure if it should be done once
  // per decoded block, or more than once, but that's probably not something games would rely on, but we should test it anyway).
  //
  // Correctish timing can be achieved by moving this block of code up above voice->DecodeAvail >= 11, and sticking it inside an: if(voice->DecodeAvail <= 12),
  // though more tests are needed on the ADPCM decoding process as a whole before we should actually make such a change.  Additionally, we'd probably
  // have to separate the CurAddr = LoopAddr logic, so we don't generate spurious early SPU IRQs.
  //
  if(voice->DecodeFlags & 0x1)
  {
   voice->CurAddr = voice->LoopAddr & ~0x7;

   BlockEnd |= 1 << (voice - Voices);

   if(!(voice->DecodeFlags & 0x2))	// Force enveloping to 0 if not "looping".  TODO: Should we reset the ADSR divider counter too?
   {
    if(!(Noise_Mode & (1 << (voice - Voices))))
    {
     voice->ADSR.Phase = ADSR_RELEASE;
     voice->ADSR.EnvLevel = 0;
    }
   }
  }
 }

 //for(int z = 0; z < 4; z++)
 {
  if(SPUControl & 0x40)
  {
   unsigned test_addr = voice->CurAddr & 0x3FFFF;
   if(IRQAddr == test_addr || IRQAddr == (test_addr & 0x3FFF8))
   {
    IRQAsserted = true;
    spuIrq(IRQAsserted);
   }
  }

  if((voice->CurAddr & 0x7) == 0)
  {
   const uint16_t CV = SPURAM[voice->CurAddr];

   voice->DecodeShift = CV & 0xF;
   voice->DecodeWeight = (CV >> 4) & 0xF;
   voice->DecodeFlags = (CV >> 8) & 0xFF;

   if(voice->DecodeFlags & 0x4)
   {
    if(!voice->IgnoreSampLA)
    {
     voice->LoopAddr = voice->CurAddr;
    }
    else
    {
     if((voice->LoopAddr ^ voice->CurAddr) & ~0x7)
     {
      ;
     }
    }
   }
   voice->CurAddr = (voice->CurAddr + 1) & 0x3FFFF;
  }

  //
  // Don't else this block; we need to ALWAYS decode 4 samples per call to RunDecoder() if DecodeAvail < 11, or else sample playback
  // at higher rates will fail horribly.
  //
  {
   const int32_t weight_m1 = Weights[voice->DecodeWeight][0];
   const int32_t weight_m2 = Weights[voice->DecodeWeight][1];
   uint16_t CV;
   unsigned shift;
   uint32_t coded;
   int16_t *tb = &voice->DecodeBuffer[voice->DecodeWritePos];

   CV = SPURAM[voice->CurAddr];
   shift = voice->DecodeShift;

   if(MDFN_UNLIKELY(shift > 12))
   {
    //;

    shift = 8;
    CV &= 0x8888;
   }

   coded = (uint32_t)CV << 12;

   for(int i = 0; i < 4; i++)
   {
    int32_t sample = (int16_t)(coded & 0xF000) >> shift;

    sample += ((voice->DecodeM2 * weight_m2) >> 6);
    sample += ((voice->DecodeM1 * weight_m1) >> 6);

    clamp(&sample, -32768, 32767);

    tb[i] = sample;
    voice->DecodeM2 = voice->DecodeM1;
    voice->DecodeM1 = sample;
    coded >>= 4;
   }
   voice->DecodeWritePos = (voice->DecodeWritePos + 4) & 0x1F;
   voice->DecodeAvail += 4;
   voice->CurAddr = (voice->CurAddr + 1) & 0x3FFFF;
  }
 }
}

static void CacheEnvelope(SPU_Voice *voice)
{
 uint32_t raw = voice->ADSRControl;
 SPU_ADSR *ADSR = &voice->ADSR;
 int32_t Sl, Dr, Ar, Rr, Sr;

 Sl = (raw >> 0) & 0x0F;
 Dr = (raw >> 4) & 0x0F;
 Ar = (raw >> 8) & 0x7F;

 Rr = (raw >> 16) & 0x1F;
 Sr = (raw >> 22) & 0x7F;


 ADSR->AttackExp = (bool)(raw & (1 << 15));
 ADSR->ReleaseExp = (bool)(raw & (1 << 21));
 ADSR->SustainExp = (bool)(raw & (1 << 31));
 ADSR->SustainDec = (bool)(raw & (1 << 30));

 ADSR->AttackRate = Ar;
 ADSR->DecayRate = Dr << 2;
 ADSR->SustainRate = Sr;
 ADSR->ReleaseRate = Rr << 2;

 ADSR->SustainLevel = (Sl + 1) << 11;
}

static INLINE void ResetEnvelope(SPU_Voice *voice)
{
 SPU_ADSR *ADSR = &voice->ADSR;

 ADSR->EnvLevel = 0;
 ADSR->Divider = 0;
 ADSR->Phase = ADSR_ATTACK;
}

static INLINE void ReleaseEnvelope(SPU_Voice *voice)
{
 SPU_ADSR *ADSR = &voice->ADSR;

 ADSR->Divider = 0;
 ADSR->Phase = ADSR_RELEASE;
}


static INLINE void RunEnvelope(SPU_Voice *voice)
{
 SPU_ADSR *ADSR = &voice->ADSR;
 int increment;
 int divinco;
 int16_t uoflow_reset;

 if(ADSR->Phase == ADSR_ATTACK && ADSR->EnvLevel == 0x7FFF)
  ADSR->Phase++;

 //static INLINE void CalcVCDelta(const uint8_t zs, uint8_t speed, bool log_mode, bool decrement, bool inv_increment, int16_t Current, int *increment, int *divinco)
 switch(ADSR->Phase)
 {
  default:	// Won't happen, but helps shut up gcc warnings.
  case ADSR_ATTACK:
	CalcVCDelta(0x7F, ADSR->AttackRate, ADSR->AttackExp, false, false, (int16_t)ADSR->EnvLevel, &increment, &divinco);
	uoflow_reset = 0x7FFF;
	break;

  case ADSR_DECAY:
	CalcVCDelta(0x1F << 2, ADSR->DecayRate, true, true, true, (int16_t)ADSR->EnvLevel, &increment, &divinco);
	uoflow_reset = 0;
	break;

  case ADSR_SUSTAIN:
	CalcVCDelta(0x7F, ADSR->SustainRate, ADSR->SustainExp, ADSR->SustainDec, ADSR->SustainDec, (int16_t)ADSR->EnvLevel, &increment, &divinco);
	uoflow_reset = ADSR->SustainDec ? 0 : 0x7FFF;
	break;

  case ADSR_RELEASE:
	CalcVCDelta(0x1F << 2, ADSR->ReleaseRate, ADSR->ReleaseExp, true, true, (int16_t)ADSR->EnvLevel, &increment, &divinco);
	uoflow_reset = 0;
	break;
 }

 ADSR->Divider += divinco;
 if(ADSR->Divider & 0x8000)
 {
  const uint16_t prev_level = ADSR->EnvLevel;

  ADSR->Divider = 0;
  ADSR->EnvLevel += increment;

  if(ADSR->Phase == ADSR_ATTACK)
  {
   // If previous the upper bit was 0, but now it's 1, handle overflow.
   if(((prev_level ^ ADSR->EnvLevel) & ADSR->EnvLevel) & 0x8000)
    ADSR->EnvLevel = uoflow_reset;
  }
  else
  {
   if(ADSR->EnvLevel & 0x8000)
    ADSR->EnvLevel = uoflow_reset;
  }
  if(ADSR->Phase == ADSR_DECAY && (uint16_t)ADSR->EnvLevel < ADSR->SustainLevel)
   ADSR->Phase++;
 }
}

static INLINE void CheckIRQAddr(uint32_t addr)
{
 if(SPUControl & 0x40)
 {
  if(IRQAddr == addr)
  {
   IRQAsserted = true;
   spuIrq(IRQAsserted);
  }
 }
}

static INLINE void WriteSPURAM(uint32_t addr, uint16_t value)
{
 CheckIRQAddr(addr);

 SPURAM[addr] = value;
}

static INLINE uint16_t ReadSPURAM(uint32_t addr)
{
 CheckIRQAddr(addr);
 return(SPURAM[addr]);
}

/******************************************************************************/
/* Mednafen Sony PS1 Emulation Module                                         */
/******************************************************************************/
/* spu_reverb.inc:
**  Copyright (C) 2011-2020 Mednafen Team
**
** This program is free software; you can redistribute it and/or
** modify it under the terms of the GNU General Public License
** as published by the Free Software Foundation; either version 2
** of the License, or (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software Foundation, Inc.,
** 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

static int16_t ReverbSat(int32_t samp) MDFN_WARN_UNUSED_RESULT;
static INLINE int16_t ReverbSat(int32_t samp)
{
 if(samp > 32767)
  samp = 32767;

 if(samp < -32768)
  samp = -32768;

 return(samp);
}

static INLINE int16_t ReverbNeg(int16_t samp)
{
 if(samp == -32768)
  return 0x7FFF;

 return -samp;
}

static INLINE uint32_t Get_Reverb_Offset(uint32_t in_offset)
{
 uint32_t offset = ReverbCur + (in_offset & 0x3FFFF);

 offset += ReverbWA & ((int32_t)(offset << 13) >> 31);
 offset &= 0x3FFFF;

 // For debugging in case there are any problems with games misprogramming the reverb registers in a race-conditiony manner that
 // causes important data in SPU RAM to be trashed:
 //if(offset < ReverbWA)
 // printf("BARF: offset=%05x reverbwa=%05x reverbcur=%05x in_offset=%05x\n", offset, ReverbWA, ReverbCur, in_offset & 0x3FFFF);

 return(offset);
}

static int16_t __attribute__((noinline)) RD_RVB2(uint16_t raw_offs, int32_t extra_offs)
{
 return ReadSPURAM(Get_Reverb_Offset((raw_offs << 2) + extra_offs));
}

static void __attribute__((noinline)) WR_RVB(uint16_t raw_offs, int16_t sample)
{
 if(SPUControl & 0x80)
  WriteSPURAM(Get_Reverb_Offset(raw_offs << 2), sample);
}

//
// Zeroes optimized out; middle removed too(it's 16384)
static const int16_t ResampTable[20] =
{
 -1, 2, -10, 35, -103, 266, -616, 1332, -2960, 10246, 10246, -2960, 1332, -616, 266, -103, 35, -10, 2, -1,
};

static INLINE int32_t Reverb4422(const int16_t *src)
{
 int32_t out = 0;	// 32-bits is adequate(it won't overflow)

 for(unsigned i = 0; i < 20; i++)
  out += ResampTable[i] * src[i * 2];

 // Middle non-zero
 out += 0x4000 * src[19];

 out >>= 15;

 clamp(&out, -32768, 32767);

 return(out);
}

static INLINE int32_t Reverb2244(const int16_t *src, bool phase)
{
 int32_t out;	// 32-bits is adequate(it won't overflow)

 if(phase)
 {
  // Middle non-zero
  out = src[9];
 }
 else
 {
  out = 0;

  for(unsigned i = 0; i < 20; i++)
   out += ResampTable[i] * src[i];

  out >>= 14;

  clamp(&out, -32768, 32767);
 }

 return(out);
}

static int32_t IIASM(const int16_t alpha, const int16_t insamp)
{
 if(MDFN_UNLIKELY(alpha == -32768))
 {
  //const int32_t iai_adj = sign_x_to_s32(17, (32768 - alpha));
  //tmp = iai_adj * in_samp

  if(insamp == -32768)
   return 0;
  else
   return insamp * -65536;
 }
 else
  return insamp * (32768 - alpha);
}

//
// Take care to thoroughly test the reverb resampling code when modifying anything that uses RvbResPos.
//
static INLINE void RunReverb(const int32_t* in, int32_t* out)
{
 int32_t upsampled[2] = { 0, 0 };

 for(unsigned lr = 0; lr < 2; lr++)
 {
  RDSB[lr][RvbResPos | 0x00] = in[lr];
  RDSB[lr][RvbResPos | 0x40] = in[lr];	// So we don't have to &/bounds check in our MAC loop
 }

 if(RvbResPos & 1)
 {
  int32_t downsampled[2];

  for(unsigned lr = 0; lr < 2; lr++)
   downsampled[lr] = Reverb4422(&RDSB[lr][(RvbResPos - 38) & 0x3F]);

  //
  // Run algorithm
  //
  for(unsigned lr = 0; lr < 2; lr++)
  {
   const int16_t IIR_INPUT_A = ReverbSat((((RD_RVB2(IIR_SRC_A[lr ^ 0], 0) * IIR_COEF) >> 14) + ((downsampled[lr] * IN_COEF[lr]) >> 14)) >> 1);
   const int16_t IIR_INPUT_B = ReverbSat((((RD_RVB2(IIR_SRC_B[lr ^ 1], 0) * IIR_COEF) >> 14) + ((downsampled[lr] * IN_COEF[lr]) >> 14)) >> 1);
   const int16_t IIR_A = ReverbSat((((IIR_INPUT_A * IIR_ALPHA) >> 14) + (IIASM(IIR_ALPHA, RD_RVB2(IIR_DEST_A[lr], -1)) >> 14)) >> 1);
   const int16_t IIR_B = ReverbSat((((IIR_INPUT_B * IIR_ALPHA) >> 14) + (IIASM(IIR_ALPHA, RD_RVB2(IIR_DEST_B[lr], -1)) >> 14)) >> 1);

   WR_RVB(IIR_DEST_A[lr], IIR_A);
   WR_RVB(IIR_DEST_B[lr], IIR_B);

   const int32_t ACC = ((RD_RVB2(ACC_SRC_A[lr], 0) * ACC_COEF_A) >> 14) +
		     ((RD_RVB2(ACC_SRC_B[lr], 0) * ACC_COEF_B) >> 14) +
		     ((RD_RVB2(ACC_SRC_C[lr], 0) * ACC_COEF_C) >> 14) +
		     ((RD_RVB2(ACC_SRC_D[lr], 0) * ACC_COEF_D) >> 14);

   const int16_t FB_A = RD_RVB2(MIX_DEST_A[lr] - FB_SRC_A, 0);
   const int16_t FB_B = RD_RVB2(MIX_DEST_B[lr] - FB_SRC_B, 0);
   const int16_t MDA = ReverbSat((ACC + ((FB_A * ReverbNeg(FB_ALPHA)) >> 14)) >> 1);
   const int16_t MDB = ReverbSat(FB_A + ((((MDA * FB_ALPHA) >> 14) + ((FB_B * ReverbNeg(FB_X)) >> 14)) >> 1));
   const int16_t IVB = ReverbSat(FB_B + ((MDB * FB_X) >> 15));

   WR_RVB(MIX_DEST_A[lr], MDA);
   WR_RVB(MIX_DEST_B[lr], MDB);
#if 0
   {
    static uint32_t sqcounter;
    RUSB[lr][(RvbResPos >> 1) | 0x20] = RUSB[lr][RvbResPos >> 1] = ((sqcounter & 0xFF) == 0) ? 0x8000 : 0x0000; //((sqcounter & 0x80) ? 0x7000 : 0x9000);
    sqcounter += lr;
   }
#else
   RUSB[lr][(RvbResPos >> 1) | 0x20] = RUSB[lr][RvbResPos >> 1] = IVB;	// Output sample
#endif
  }
  //
  //
  //
  ReverbCur = (ReverbCur + 1) & 0x3FFFF;
  if(!ReverbCur)
   ReverbCur = ReverbWA;

  for(unsigned lr = 0; lr < 2; lr++)
   upsampled[lr] = Reverb2244(&RUSB[lr][((RvbResPos >> 1) - 19) & 0x1F], false);
 }
 else
 {
  for(unsigned lr = 0; lr < 2; lr++)
   upsampled[lr] = Reverb2244(&RUSB[lr][((RvbResPos >> 1) - 19) & 0x1F], true);
 }

 RvbResPos = (RvbResPos + 1) & 0x3F;

 for(unsigned lr = 0; lr < 2; lr++)
 {
#if 0
  if(!lr)
   printf("%d\n", (-upsampled[lr]) >> 1);
#endif

  out[lr] = upsampled[lr];
 }
}



static INLINE void RunNoise(void)
{
 const unsigned rf = ((SPUControl >> 8) & 0x3F);
 uint32_t NoiseDividerInc = (2 << (rf >> 2));
 uint32_t NoiseCounterInc = 4 + (rf & 0x3);

 if(rf >= 0x3C)
 {
  NoiseDividerInc = 0x8000;
  NoiseCounterInc = 8;
 }

 NoiseDivider += NoiseDividerInc;
 if(NoiseDivider & 0x8000)
 {
  NoiseDivider = 0;

  NoiseCounter += NoiseCounterInc;
  if(NoiseCounter & 0x8)
  {
   NoiseCounter &= 0x7;
   LFSR = (LFSR << 1) | (((LFSR >> 15) ^ (LFSR >> 12) ^ (LFSR >> 11) ^ (LFSR >> 10) ^ 1) & 1);
  }
 }
}

int32_t SPU_UpdateFromCDC(int32_t clocks)
//int64_t Update(const int64_t timestamp)
{
 //int32_t clocks = timestamp - lastts;
 int32_t sample_clocks = 0;
 //lastts = timestamp;

 clock_divider -= clocks;

 while(clock_divider <= 0)
 {
  clock_divider += 768;
  sample_clocks++;
 }

 while(sample_clocks > 0)
 {
  // xxx[0] = left, xxx[1] = right

  // Accumulated sound output.
  int32_t accum[2] = { 0, 0 };

  // Accumulated sound output for reverb input
  int32_t accum_fv[2] = { 0, 0 };

  // Output of reverb processing.
  int32_t reverb[2] = { 0, 0 };

  // Final output.
  int32_t output[2] = { 0, 0 };

  const uint32_t PhaseModCache = FM_Mode & ~ 1;
/*
**
** 0x1F801DAE Notes and Conjecture:
**   -------------------------------------------------------------------------------------
**   |   15   14 | 13 | 12 | 11 | 10  | 9  | 8 |  7 |  6  | 5    4    3    2    1    0   |
**   |      ?    | *13| ?  | ba | *10 | wrr|rdr| df |  is |      c                       |
**   -------------------------------------------------------------------------------------
**
**	c - Appears to be delayed copy of lower 6 bits from 0x1F801DAA.
**
**     is - Interrupt asserted out status. (apparently not instantaneous status though...)
**
**     df - Related to (c & 0x30) == 0x20 or (c & 0x30) == 0x30, at least.
**          0 = DMA busy(FIFO not empty when in DMA write mode?)?
**	    1 = DMA ready?  Something to do with the FIFO?
**
**     rdr - Read(DMA read?) Ready?
**
**     wrr - Write(DMA write?) Ready?
**
**     *10 - Unknown.  Some sort of (FIFO?) busy status?(BIOS tests for this bit in places)
**
**     ba - Alternates between 0 and 1, even when SPUControl bit15 is 0; might be related to CD audio and voice 1 and 3 writing to SPU RAM.
**
**     *13 - Unknown, was set to 1 when testing with an SPU delay system reg value of 0x200921E1(test result might not be reliable, re-run).
*/
  SPUStatus = SPUControl & 0x3F;
  SPUStatus |= IRQAsserted ? 0x40 : 0x00;

  if(Regs[0xD6] == 0x4)	// TODO: Investigate more(case 0x2C in global regs r/w handler)
   SPUStatus |= (CWA & 0x100) ? 0x800 : 0x000;

  for(int voice_num = 0; voice_num < 24; voice_num++)
  {
   SPU_Voice *voice = &Voices[voice_num];
   int32_t voice_pvs;

   voice->PreLRSample = 0;

   //;

   if(voice->DecodePlayDelay)
   {
    voice->IgnoreSampLA = false;
   }

   //
   // Decode new samples if necessary.
   //
   RunDecoder(voice);


   //
   //
   //
   int l, r;

   if(Noise_Mode & (1 << voice_num))
    voice_pvs = (int16_t)LFSR;
   else
   {
    const int si = voice->DecodeReadPos;
    const int pi = ((voice->CurPhase & 0xFFF) >> 4);

    voice_pvs = ((voice->DecodeBuffer[(si + 0) & 0x1F] * FIR_Table[pi][0]) +
	         (voice->DecodeBuffer[(si + 1) & 0x1F] * FIR_Table[pi][1]) +
	         (voice->DecodeBuffer[(si + 2) & 0x1F] * FIR_Table[pi][2]) +   
 	         (voice->DecodeBuffer[(si + 3) & 0x1F] * FIR_Table[pi][3])) >> 15;
   }

   voice_pvs = (voice_pvs * (int16_t)voice->ADSR.EnvLevel) >> 15;
   voice->PreLRSample = voice_pvs;

   if(voice_num == 1 || voice_num == 3)
   {
    int index = voice_num >> 1;

    WriteSPURAM(0x400 | (index * 0x200) | CWA, voice_pvs);
   }


   l = (voice_pvs * Sweep_ReadVolume(&voice->Sweep[0])) >> 15;
   r = (voice_pvs * Sweep_ReadVolume(&voice->Sweep[1])) >> 15;

   accum[0] += l;
   accum[1] += r;

   if(Reverb_Mode & (1 << voice_num))
   {
    accum_fv[0] += l;
    accum_fv[1] += r;
   }

   // Run sweep
   for(int lr = 0; lr < 2; lr++)
    Sweep_Clock(&voice->Sweep[lr]);

   // Increment stuff
   if(!voice->DecodePlayDelay)
   {
    unsigned phase_inc;

    // Run enveloping
    RunEnvelope(voice);

    if(PhaseModCache & (1 << voice_num))
    {
     // This old formula: phase_inc = (voice->Pitch * ((voice - 1)->PreLRSample + 0x8000)) >> 15;
     // is incorrect, as it does not handle carrier pitches >= 0x8000 properly.
     phase_inc = voice->Pitch + (((int16_t)voice->Pitch * ((voice - 1)->PreLRSample)) >> 15);
    }
    else
     phase_inc = voice->Pitch;

    if(phase_inc > 0x3FFF)
     phase_inc = 0x3FFF;

    {
     const uint32_t tmp_phase = voice->CurPhase + phase_inc;
     const unsigned used = tmp_phase >> 12;

     voice->CurPhase = tmp_phase & 0xFFF;
     voice->DecodeAvail -= used;
     voice->DecodeReadPos = (voice->DecodeReadPos + used) & 0x1F;
    }
   }
   else
    voice->DecodePlayDelay--;

   if(VoiceOff & (1U << voice_num))
   {
    if(voice->ADSR.Phase != ADSR_RELEASE)
    {
     // TODO/FIXME:
     //  To fix all the missing notes in "Dragon Ball GT: Final Bout" music, !voice->DecodePlayDelay instead of
     //  voice->DecodePlayDelay < 3 is necessary, but that would cause the length of time for which the voice off is
     //  effectively ignored to be too long by about half a sample(rough test measurement).  That, combined with current
     //  CPU and DMA emulation timing inaccuracies(execution generally too fast), creates a significant risk of regressions
     //  in other games, so be very conservative for now.
     //
     //  Also, voice on should be ignored during the delay as well, but comprehensive tests are needed before implementing that
     //  due to some effects that appear to occur repeatedly during the delay on a PS1 but are currently only emulated as
     //  performed when the voice on is processed(e.g. curaddr = startaddr).
     //
     if(voice->DecodePlayDelay < 3)
     {
      if(voice->DecodePlayDelay)
       ;

      ReleaseEnvelope(voice);
     }
     else
      ;
    }
   }

   if(VoiceOn & (1U << voice_num))
   {
    //printf("Voice On: %u\n", voice_num);

    ResetEnvelope(voice);

    voice->DecodeFlags = 0;
    voice->DecodeWritePos = 0;
    voice->DecodeReadPos = 0;
    voice->DecodeAvail = 0;
    voice->DecodePlayDelay = 4;

    BlockEnd &= ~(1 << voice_num);

    //
    // Weight/filter previous value initialization:
    //
    voice->DecodeM2 = 0;
    voice->DecodeM1 = 0;

    voice->CurPhase = 0;
    voice->CurAddr = voice->StartAddr & ~0x7;
    voice->IgnoreSampLA = false;
   }

   if(!(SPUControl & 0x8000))
   {
    voice->ADSR.Phase = ADSR_RELEASE;
    voice->ADSR.EnvLevel = 0;
   }
  }

  VoiceOff = 0;
  VoiceOn = 0; 

  // "Mute" control doesn't seem to affect CD audio(though CD audio reverb wasn't tested...)
  // TODO: If we add sub-sample timing accuracy, see if it's checked for every channel at different times, or just once.
  if(!(SPUControl & 0x4000))
  {
   accum[0] = 0;
   accum[1] = 0;
   accum_fv[0] = 0;
   accum_fv[1] = 0;
  }

  // Get CD-DA
  {
   int32_t cda_raw[2];
   int32_t cdav[2];

   CDC_GetCDAudio(cda_raw);	// PS_CDC::GetCDAudio() guarantees the variables passed by reference will be set to 0,
				// and that their range shall be -32768 through 32767.

   WriteSPURAM(CWA | 0x000, cda_raw[0]);
   WriteSPURAM(CWA | 0x200, cda_raw[1]);

   for(unsigned i = 0; i < 2; i++)
    cdav[i] = (cda_raw[i] * CDVol[i]) >> 15;

   if(SPUControl & 0x0001)
   {
    accum[0] += cdav[0];
    accum[1] += cdav[1];

    if(SPUControl & 0x0004)	// TODO: Test this bit(and see if it is really dependent on bit0)
    {
     accum_fv[0] += cdav[0];
     accum_fv[1] += cdav[1];
    }
   }
  }

  CWA = (CWA + 1) & 0x1FF;

  RunNoise();

  for(unsigned lr = 0; lr < 2; lr++)
   clamp(&accum_fv[lr], -32768, 32767);
  
  RunReverb(accum_fv, reverb);

  for(unsigned lr = 0; lr < 2; lr++)
  {
   accum[lr] += ((reverb[lr] * ReverbVol[lr]) >> 15);
   clamp(&accum[lr], -32768, 32767);
   output[lr] = (accum[lr] * Sweep_ReadVolume(&GlobalSweep[lr])) >> 15;
   clamp(&output[lr], -32768, 32767);
  }

  if(IntermediateBufferPos < 4096)	// Overflow might occur in some debugger use cases.
  {
   // 75%, for some (resampling) headroom.
   for(unsigned lr = 0; lr < 2; lr++)
    IntermediateBuffer[IntermediateBufferPos][lr] = (output[lr] * 3 + 2) >> 2;

   IntermediateBufferPos++;
  }

  sample_clocks--;

  // Clock global sweep
  for(unsigned lr = 0; lr < 2; lr++)
   Sweep_Clock(&GlobalSweep[lr]);
 }

 //assert(clock_divider < 768);

 return clock_divider;
}

void SPU_WriteDMA(uint32_t V)
{
 //SPUIRQ_DBG("DMA Write, RWAddr after=0x%06x", RWAddr);
 WriteSPURAM(RWAddr, V);
 RWAddr = (RWAddr + 1) & 0x3FFFF;

 WriteSPURAM(RWAddr, V >> 16);
 RWAddr = (RWAddr + 1) & 0x3FFFF;


 CheckIRQAddr(RWAddr);
}

uint32_t SPU_ReadDMA(void)
{
 uint32_t ret;

 ret = (uint16_t)ReadSPURAM(RWAddr);
 RWAddr = (RWAddr + 1) & 0x3FFFF;

 ret |= (uint32_t)(uint16_t)ReadSPURAM(RWAddr) << 16;
 RWAddr = (RWAddr + 1) & 0x3FFFF;

 CheckIRQAddr(RWAddr);

 //SPUIRQ_DBG("DMA Read, RWAddr after=0x%06x", RWAddr);

 return(ret);
}

void SPU_Write(int64_t timestamp, uint32_t A, uint16_t V)
{
 //if((A & 0x3FF) < 0x180)
 // ;

 A &= 0x3FF;

 if(A >= 0x200)
 {
  //printf("Write: %08x %04x\n", A, V);
  if(A < 0x260)
  {
   SPU_Voice *voice = &Voices[(A - 0x200) >> 2];
   Sweep_WriteVolume(&voice->Sweep[(A & 2) >> 1], V);
  }
  else if(A < 0x280)
   AuxRegs[(A & 0x1F) >> 1] = V;

  return;
 }

 if(A < 0x180)
 {
  SPU_Voice *voice = &Voices[A >> 4];

  switch(A & 0xF)
  {
   case 0x00:
   case 0x02:
	     Sweep_WriteControl(&voice->Sweep[(A & 2) >> 1], V);
	     break;

   case 0x04: voice->Pitch = V;
	      break;

   case 0x06: voice->StartAddr = (V << 2) & 0x3FFFF;
	      break;

   case 0x08: voice->ADSRControl &= 0xFFFF0000;
	      voice->ADSRControl |= V;
	      CacheEnvelope(voice);
	      break;

   case 0x0A: voice->ADSRControl &= 0x0000FFFF;
	      voice->ADSRControl |= V << 16;
	      CacheEnvelope(voice);
	      break;

   case 0x0C: voice->ADSR.EnvLevel = V;
	      break;

   case 0x0E: voice->LoopAddr = (V << 2) & 0x3FFFF;
	      voice->IgnoreSampLA = true;

	      if(voice->DecodePlayDelay || (VoiceOn & (1U << (voice - Voices))))
	      {
	       ;
	      }
	      //if((voice - Voices) == 22)
	      //{
	      // SPUIRQ_DBG("Manual loop address setting for voice %d: %04x", (int)(voice - Voices), V);
	      //}
	      break;
  }
 }
 else
 {
  switch(A & 0x7F)
  {
   case 0x00:
   case 0x02: Sweep_WriteControl(&GlobalSweep[(A & 2) >> 1], V);
	      break;

   case 0x04: ReverbVol[0] = (int16_t)V;
	      break;

   case 0x06: ReverbVol[1] = (int16_t)V;
	      break;

   // Voice ON:
   case 0x08: VoiceOn &= 0xFFFF0000;
	      VoiceOn |= V << 0;
	      break;

   case 0x0a: VoiceOn &= 0x0000FFFF;
              VoiceOn |= (V & 0xFF) << 16;
	      break;

   // Voice OFF:
   case 0x0c: VoiceOff &= 0xFFFF0000;
              VoiceOff |= V << 0;
	      break;

   case 0x0e: VoiceOff &= 0x0000FFFF;
              VoiceOff |= (V & 0xFF) << 16;
	      break;

   case 0x10: FM_Mode &= 0xFFFF0000;
	      FM_Mode |= V << 0;
	      break;

   case 0x12: FM_Mode &= 0x0000FFFF;
	      FM_Mode |= (V & 0xFF) << 16;
	      break;

   case 0x14: Noise_Mode &= 0xFFFF0000;
	      Noise_Mode |= V << 0;
	      break;

   case 0x16: Noise_Mode &= 0x0000FFFF;
	      Noise_Mode |= (V & 0xFF) << 16;
	      break;

   case 0x18: Reverb_Mode &= 0xFFFF0000;
	      Reverb_Mode |= V << 0;
	      break;

   case 0x1A: Reverb_Mode &= 0x0000FFFF;
	      Reverb_Mode |= (V & 0xFF) << 16;
	      break;

   case 0x1C: BlockEnd &= 0xFFFF0000;
	      BlockEnd |= V << 0;
	      break;

   case 0x1E: BlockEnd &= 0x0000FFFF;
	      BlockEnd |= V << 16;
	      break;

   case 0x22: ReverbWA = (V << 2) & 0x3FFFF;
	      ReverbCur = ReverbWA;
	      //;
	      break;

   case 0x24: IRQAddr = (V << 2) & 0x3FFFF;
	      CheckIRQAddr(RWAddr);
	      //SPUIRQ_DBG("Set IRQAddr=0x%06x", IRQAddr);
	      break;

   case 0x26: RWAddr = (V << 2) & 0x3FFFF;	      
	      CheckIRQAddr(RWAddr);
	      //SPUIRQ_DBG("Set RWAddr=0x%06x", RWAddr);
	      break;

   case 0x28: WriteSPURAM(RWAddr, V);
	      RWAddr = (RWAddr + 1) & 0x3FFFF;
	      CheckIRQAddr(RWAddr);
	      break;

   case 0x2A: //if((SPUControl & 0x80) && !(V & 0x80))
	      // printf("\n\n\n\n ************** REVERB PROCESSING DISABLED\n\n\n\n");

	      SPUControl = V;
	      //SPUIRQ_DBG("Set SPUControl=0x%04x -- IRQA=%06x, RWA=%06x", V, IRQAddr, RWAddr);
	      //printf("SPU control write: %04x\n", V);
	      if(!(V & 0x40))
	      {
	       IRQAsserted = false;
	       spuIrq(IRQAsserted);
	      }
	      CheckIRQAddr(RWAddr);
	      break;

   case 0x2C: ;
	      break;

   case 0x30: CDVol[0] = (int16_t)V;
	      break;

   case 0x32: CDVol[1] = (int16_t)V;
	      break;

   case 0x34: ExternVol[0] = (int16_t)V;
	      break;

   case 0x36: ExternVol[1] = (int16_t)V;
	      break;

   case 0x38:
   case 0x3A: Sweep_WriteVolume(&GlobalSweep[(A & 2) >> 1], V);
	      break;
  }
 }

 Regs[(A & 0x1FF) >> 1] = V;
}

uint16_t SPU_Read(int64_t timestamp, uint32_t A)
{
 A &= 0x3FF;

 //PSX_DBGINFO("[SPU] Read: %08x", A);

 if(A >= 0x200)
 {
  if(A < 0x260)
  {
   SPU_Voice *voice = &Voices[(A - 0x200) >> 2];

   //printf("Read: %08x %04x\n", A, Sweep_ReadVolume(&voice->Sweep[(A & 2) >> 1]));

   return Sweep_ReadVolume(&voice->Sweep[(A & 2) >> 1]);
  }
  else if(A < 0x280)
   return(AuxRegs[(A & 0x1F) >> 1]);

  return(0xFFFF);
 }


 if(A < 0x180)
 {
  SPU_Voice *voice = &Voices[A >> 4];

  switch(A & 0xF)
  {
   case 0x0C: return(voice->ADSR.EnvLevel);
   case 0x0E: return(voice->LoopAddr >> 2);
  }
 }
 else
 {
  switch(A & 0x7F)
  {
   case 0x1C: return(BlockEnd);
   case 0x1E: return(BlockEnd >> 16);

   case 0x26: //;
	      break;

   case 0x28: ;

	      {
	       uint16_t ret = ReadSPURAM(RWAddr);

	       RWAddr = (RWAddr + 1) & 0x3FFFF;
	       CheckIRQAddr(RWAddr);

	       return(ret);
	      }

   case 0x2a:
	return(SPUControl);

/* FIXME: What is this used for? */
   case 0x3C:
	//;
	return(0);

   case 0x38:
   case 0x3A: return(Sweep_ReadVolume(&GlobalSweep[(A & 2) >> 1]));
  }
 }

 return(Regs[(A & 0x1FF) >> 1]);
}



int32_t SPU_EndFrame(int16_t *SoundBuf)
{
 int32_t ret = IntermediateBufferPos;
 memcpy(SoundBuf, IntermediateBuffer, IntermediateBufferPos * 2 * sizeof(int16_t));
 IntermediateBufferPos = 0;
 return ret;
}


// armPSX's side

static struct Psx* P;
static bool irqLevel;

static void spuIrq(bool level) {
 if (level && !irqLevel) psxIrq(P, IRQ_SPU);
 irqLevel = level;
}

void spuReset(struct Psx* p) {
 P = p;
 irqLevel = false;
 memset(SPURAM, 0, sizeof SPURAM);
 IntermediateBufferPos = 0;
 SPU_Power();
}

uint16_t spuRead(struct Psx* p, uint32_t address) {
 return SPU_Read(p->cycles, address);
}

void spuWrite(struct Psx* p, uint32_t address, uint16_t value) {
 SPU_Write(p->cycles, address, value);
}

void spuDmaWrite(struct Psx* p, uint32_t value) {
 SPU_WriteDMA(value);
}

uint32_t spuDmaRead(struct Psx* p) {
 return SPU_ReadDMA();
}

void spuRun(struct Psx* p) {
}

int spuSamples(int16_t* out) {
 return SPU_EndFrame(out);
}
