/* armPSX: Mednafen's mdec.cpp converted to C (plain IDCT, no save states yet). */
/******************************************************************************/
/* Mednafen Sony PS1 Emulation Module                                         */
/******************************************************************************/
/* mdec.cpp:
**  Copyright (C) 2011-2016 Mednafen Team
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

/*
 MDEC_READ_FIFO(tfr) vs InCounter vs MDEC_DMACanRead() is a bit fragile right now.  Actually, the entire horrible state machine monstrosity is fragile.

 TODO: OutFIFOReady, so <16bpp works right.

 TODO CODE:

  bool InFIFOReady;

  if(InFIFO.CanWrite())
  {
   InFIFO.Write(V);

   if(InCommand)
   {
    if(InCounter != 0xFFFF)
    {
     InCounter--;

     // This condition when InFIFO.CanWrite() != 0 is a bit buggy on real hardware, decoding loop *seems* to be reading too
     // much and pulling garbage from the FIFO.
     if(InCounter == 0xFFFF)	
      InFIFOReady = true;
    }

    if(InFIFO.CanWrite() == 0)
     InFIFOReady = true;
   }
  }
*/

// Good test-case games:
//	Dragon Knight 4(bad disc?)
//	Final Fantasy 7 intro movie.
//	GameShark Version 4.0 intro movie; (clever) abuse of DMA channel 0.
//	SimCity 2000 startup.


#include <string.h>
#include "psx.h"

#define INLINE inline
#define NO_INLINE __attribute__((noinline))
#define MDFN_LIKELY(x) __builtin_expect(!!(x), 1)
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MDAP(p) (&(p)[0][0])

struct Fifo { uint32_t data[0x20]; unsigned read, count; };
static void fifoFlush(struct Fifo* f) { f->read = f->count = 0; }
static void fifoWrite(struct Fifo* f, uint32_t v) { f->data[(f->read + f->count) & 0x1F] = v; f->count++; }
static uint32_t fifoRead(struct Fifo* f) { uint32_t v = f->data[f->read]; f->read = (f->read + 1) & 0x1F; f->count--; return v; }
static inline int16_t sign_10_to_s16(uint16_t v) { return (int16_t) (v << 6) >> 6; }
static inline int32_t sign_x_to_s32(int bits, uint32_t v) { return (int32_t) (v << (32 - bits)) >> (32 - bits); }

static int32_t ClockCounter;
static unsigned MDRPhase;
static struct Fifo InFIFO;
static struct Fifo OutFIFO;

static int8_t block_y[8][8];
static int8_t block_cb[8][8];	// [y >> 1][x >> 1]
static int8_t block_cr[8][8];	// [y >> 1][x >> 1]

static uint32_t Control;
static uint32_t Command;
static bool InCommand;

static uint8_t QMatrix[2][64];
static uint32_t QMIndex;

static int16_t IDCTMatrix[64];
static uint32_t IDCTMIndex;

static uint8_t QScale;

static int16_t Coeff[64];
static uint32_t CoeffIndex;
static uint32_t DecodeWB;

static union
{
 uint32_t pix32[48];
 uint16_t pix16[96];
 uint8_t   pix8[192];
} PixelBuffer;
static uint32_t PixelBufferReadOffset;
static uint32_t PixelBufferCount32;

static uint16_t InCounter;

static uint8_t RAMOffsetY;
static uint8_t RAMOffsetCounter;
static uint8_t RAMOffsetWWS;

static const uint8_t ZigZag[64] =
{
 0x00, 0x08, 0x01, 0x02, 0x09, 0x10, 0x18, 0x11, 
 0x0a, 0x03, 0x04, 0x0b, 0x12, 0x19, 0x20, 0x28, 
 0x21, 0x1a, 0x13, 0x0c, 0x05, 0x06, 0x0d, 0x14, 
 0x1b, 0x22, 0x29, 0x30, 0x38, 0x31, 0x2a, 0x23, 
 0x1c, 0x15, 0x0e, 0x07, 0x0f, 0x16, 0x1d, 0x24, 
 0x2b, 0x32, 0x39, 0x3a, 0x33, 0x2c, 0x25, 0x1e, 
 0x17, 0x1f, 0x26, 0x2d, 0x34, 0x3b, 0x3c, 0x35, 
 0x2e, 0x27, 0x2f, 0x36, 0x3d, 0x3e, 0x37, 0x3f, 
};

void MDEC_Power(void)
{
 ClockCounter = 0;
 MDRPhase = 0;

 fifoFlush(&InFIFO);
 fifoFlush(&OutFIFO);

 memset(block_y, 0, sizeof(block_y));
 memset(block_cb, 0, sizeof(block_cb));
 memset(block_cr, 0, sizeof(block_cr));

 Control = 0;
 Command = 0;
 InCommand = false;

 memset(QMatrix, 0, sizeof(QMatrix));
 QMIndex = 0;

 memset(IDCTMatrix, 0, sizeof(IDCTMatrix));
 IDCTMIndex = 0;

 QScale = 0;

 memset(Coeff, 0, sizeof(Coeff));
 CoeffIndex = 0;
 DecodeWB = 0;

 memset(PixelBuffer.pix32, 0, sizeof(PixelBuffer.pix32));
 PixelBufferReadOffset = 0;
 PixelBufferCount32 = 0;

 InCounter = 0;

 RAMOffsetY = 0;
 RAMOffsetCounter = 0;
 RAMOffsetWWS = 0;
}


static INLINE int8_t Mask9ClampS8(int32_t v)
{
 v = sign_x_to_s32(9, v);

 if(v < -128)
  v = -128;

 if(v > 127)
  v = 127;

 return v;
}

////////////////////////
//
//

static INLINE void IDCT_1D_Multi16(int16_t *in_coeff, int16_t *out_coeff)
{
 for(unsigned col = 0; col < 8; col++)
 {
  for(unsigned x = 0; x < 8; x++)
  {
   int32_t sum = 0;
   for(unsigned u = 0; u < 8; u++)
    sum += (in_coeff[(col * 8) + u] * IDCTMatrix[(x * 8) + u]);
   out_coeff[(x * 8) + col] = (sum + 0x4000) >> 15;
  }
 }
}

static INLINE void IDCT_1D_Multi8(int16_t *in_coeff, int8_t *out_coeff)
{
 for(unsigned col = 0; col < 8; col++)
 {
  for(unsigned x = 0; x < 8; x++)
  {
   int32_t sum = 0;
   for(unsigned u = 0; u < 8; u++)
    sum += (in_coeff[(col * 8) + u] * IDCTMatrix[(x * 8) + u]);
   out_coeff[(col * 8) + x] = Mask9ClampS8((sum + 0x4000) >> 15);
  }
 }
}

static NO_INLINE void IDCT(int16_t *in_coeff, int8_t *out_coeff)
{
 int16_t tmpbuf[64];

 IDCT_1D_Multi16(in_coeff, tmpbuf);
 IDCT_1D_Multi8(tmpbuf, out_coeff);
}
//
//
///////////////////////

static INLINE void YCbCr_to_RGB(const int8_t y, const int8_t cb, const int8_t cr, int *r, int *g, int *b)
{
 //
 // The formula for green is still a bit off(precision/rounding issues when both cb and cr are non-zero).
 //

 *r = Mask9ClampS8(y + (((359 * cr) + 0x80) >> 8));
 //g = Mask9ClampS8(y + (((-88 * cb) + (-183 * cr) + 0x80) >> 8));
 *g = Mask9ClampS8(y + ((((-88 * cb) &~ 0x1F) + ((-183 * cr) &~ 0x07) + 0x80) >> 8));
 *b = Mask9ClampS8(y + (((454 * cb) + 0x80) >> 8));

 *r ^= 0x80;
 *g ^= 0x80;
 *b ^= 0x80;
}

static INLINE uint16_t RGB_to_RGB555(uint8_t r, uint8_t g, uint8_t b)
{
 r = (r + 4) >> 3;
 g = (g + 4) >> 3;
 b = (b + 4) >> 3;

 if(r > 0x1F)
  r = 0x1F;

 if(g > 0x1F)
  g = 0x1F;

 if(b > 0x1F)
  b = 0x1F;

 return((r << 0) | (g << 5) | (b << 10));
}

static void EncodeImage(const unsigned ybn)
{
 //printf("ENCODE, %d\n", (Command & 0x08000000) ? 256 : 384);

 PixelBufferCount32 = 0;

 switch((Command >> 27) & 0x3)
 {
  case 0:	// 4bpp
  {
   const uint8_t us_xor = (Command & (1U << 26)) ? 0x00 : 0x88;
   uint8_t* pix_out = PixelBuffer.pix8;

   for(int y = 0; y < 8; y++)
   {
    for(int x = 0; x < 8; x += 2)
    {
     uint8_t p0 = MIN(127, block_y[y][x + 0] + 8);
     uint8_t p1 = MIN(127, block_y[y][x + 1] + 8);

     *pix_out = ((p0 >> 4) | (p1 & 0xF0)) ^ us_xor;
     pix_out++;
    }
   }
   PixelBufferCount32 = 8;
  }
  break;


  case 1:	// 8bpp
  {
   const uint8_t us_xor = (Command & (1U << 26)) ? 0x00 : 0x80;
   uint8_t* pix_out = PixelBuffer.pix8;

   for(int y = 0; y < 8; y++)
   {
    for(int x = 0; x < 8; x++)
    {
     *pix_out = (uint8_t)block_y[y][x] ^ us_xor;
     pix_out++;
    }
   }
   PixelBufferCount32 = 16;
  }
  break;

  case 2:	// 24bpp
  {
   const uint8_t rgb_xor = (Command & (1U << 26)) ? 0x80 : 0x00;
   uint8_t* pix_out = PixelBuffer.pix8;

   for(int y = 0; y < 8; y++)
   {
    const int8_t* by = &block_y[y][0];
    const int8_t* cb = &block_cb[(y >> 1) | ((ybn & 2) << 1)][(ybn & 1) << 2];
    const int8_t* cr = &block_cr[(y >> 1) | ((ybn & 2) << 1)][(ybn & 1) << 2];
    
    for(int x = 0; x < 8; x++)
    {
     int r, g, b;

     YCbCr_to_RGB(by[x], cb[x >> 1], cr[x >> 1], &r, &g, &b);

     pix_out[0] = r ^ rgb_xor;
     pix_out[1] = g ^ rgb_xor;
     pix_out[2] = b ^ rgb_xor;
     pix_out += 3;
    }
   }
   PixelBufferCount32 = 48;
  }
  break;

  case 3:	// 16bpp
  {
   uint16_t pixel_xor = ((Command & 0x02000000) ? 0x8000 : 0x0000) | ((Command & (1U << 26)) ? 0x4210 : 0x0000);
   uint16_t* pix_out = PixelBuffer.pix16;

   for(int y = 0; y < 8; y++)
   {
    const int8_t* by = &block_y[y][0];
    const int8_t* cb = &block_cb[(y >> 1) | ((ybn & 2) << 1)][(ybn & 1) << 2];
    const int8_t* cr = &block_cr[(y >> 1) | ((ybn & 2) << 1)][(ybn & 1) << 2];

    for(int x = 0; x < 8; x++)
    {
     int r, g, b;

     YCbCr_to_RGB(by[x], cb[x >> 1], cr[x >> 1], &r, &g, &b);

     *pix_out = pixel_xor ^ RGB_to_RGB555(r, g, b);
     pix_out++;
    }
   }
   PixelBufferCount32 = 32;
  }
  break;

 }
}

static INLINE void WriteImageData(uint16_t V, int32_t* eat_cycles)
{
 const uint32_t qmw = (bool)(DecodeWB < 2);

  //printf("MDEC DMA SubWrite: %04x, %d\n", V, CoeffIndex);

  if(!CoeffIndex)
  {
   if(V == 0xFE00)
   {
    //printf("FE00 @ %u\n", DecodeWB);
    return;
   }

   QScale = V >> 10;

   {
    int q = QMatrix[qmw][0];	// No QScale here!
    int ci = sign_10_to_s16(V & 0x3FF);
    int tmp;

    if(q != 0)
     tmp = (int32_t)((uint32_t)(ci * q) << 4) + (ci ? ((ci < 0) ? 8 : -8) : 0);
    else
     tmp = (uint32_t)(ci * 2) << 4;

    // Not sure if it should be 0x3FFF or 0x3FF0 or maybe 0x3FF8?
    Coeff[ZigZag[0]] = MIN(0x3FFF, MAX(-0x4000, tmp));
    CoeffIndex++;
   }
  }
  else
  {
   if(V == 0xFE00)
   {
    while(CoeffIndex < 64)
     Coeff[ZigZag[CoeffIndex++]] = 0;
   }
   else
   {
    uint32_t rlcount = V >> 10;

    for(uint32_t i = 0; i < rlcount && CoeffIndex < 64; i++)
    {
     Coeff[ZigZag[CoeffIndex]] = 0;
     CoeffIndex++;
    }

    if(CoeffIndex < 64)
    {
     int q = QScale * QMatrix[qmw][CoeffIndex];
     int ci = sign_10_to_s16(V & 0x3FF);
     int tmp;

     if(q != 0)
      tmp = (int32_t)((uint32_t)((ci * q) >> 3) << 4) + (ci ? ((ci < 0) ? 8 : -8) : 0);
     else
      tmp = (uint32_t)(ci * 2) << 4;

     // Not sure if it should be 0x3FFF or 0x3FF0 or maybe 0x3FF8?
     Coeff[ZigZag[CoeffIndex]] = MIN(0x3FFF, MAX(-0x4000, tmp));
     CoeffIndex++;
    }
   }
  }

  if(CoeffIndex == 64)
  {
   CoeffIndex = 0;

   //printf("Block %d finished\n", DecodeWB);

   switch(DecodeWB)
   {
    case 0: IDCT(Coeff, MDAP(block_cr)); break;
    case 1: IDCT(Coeff, MDAP(block_cb)); break;
    case 2: IDCT(Coeff, MDAP(block_y)); break;
    case 3: IDCT(Coeff, MDAP(block_y)); break;
    case 4: IDCT(Coeff, MDAP(block_y)); break;
    case 5: IDCT(Coeff, MDAP(block_y)); break;
   }   

   // Timing in the PS1 MDEC is complex due to (apparent) pipelining, but the average when decoding a large number of blocks is
   // about 512.
   *eat_cycles += 512;

   if(DecodeWB >= 2)
   {
    EncodeImage((DecodeWB + 4) % 6);
   }

   DecodeWB++;
   if(DecodeWB == (((Command >> 27) & 2) ? 6 : 3))
   {
    DecodeWB = ((Command >> 27) & 2) ? 0 : 2;
   }
  }
}


//
//
//
#define MDEC_WAIT_COND(n)  { case __COUNTER__: if(!(n)) { MDRPhase = __COUNTER__ - MDRPhaseBias - 1; return; } }

#define MDEC_WRITE_FIFO(n) { MDEC_WAIT_COND((0x20 - OutFIFO.count)); fifoWrite(&OutFIFO, n);  }
#define MDEC_READ_FIFO(n)  { MDEC_WAIT_COND(InFIFO.count); n = fifoRead(&InFIFO); }
#define MDEC_EAT_CLOCKS(n) { ClockCounter -= (n); MDEC_WAIT_COND(ClockCounter > 0); }

void MDEC_Run(int32_t clocks)
{
 static const unsigned MDRPhaseBias = __COUNTER__ + 1;

 //MDFN_DispMessage("%u", OutFIFO.count);

 ClockCounter += clocks;

 if(ClockCounter > 128)
 {
  //if(MDRPhase != 0)
  // printf("SNORT: %d\n", ClockCounter);
  ClockCounter = 128;
 }

 switch(MDRPhase + MDRPhaseBias)
 {
  for(;;)
  {
   InCommand = false;
   MDEC_READ_FIFO(Command);	// This must be the first MDEC_* macro used!
   InCommand = true;
   MDEC_EAT_CLOCKS(1);

   //printf("****************** Command: %08x, %02x\n", Command, Command >> 29);

   //
   //
   //
   if(((Command >> 29) & 0x7) == 1)
   {
    InCounter = Command & 0xFFFF;
    fifoFlush(&OutFIFO);
    //OutBuffer.Flush();

    PixelBufferCount32 = 0;
    CoeffIndex = 0;

    if((Command >> 27) & 2)
     DecodeWB = 0;
    else
     DecodeWB = 2;

    switch((Command >> 27) & 0x3)
    {
     case 0:
     case 1: RAMOffsetWWS = 0; break;
     case 2: RAMOffsetWWS = 6; break;
     case 3: RAMOffsetWWS = 4; break;
    }
    RAMOffsetY = 0;
    RAMOffsetCounter = RAMOffsetWWS;

    InCounter--;
    do
    {
     uint32_t tfr;
     int32_t need_eat; // = 0;

     MDEC_READ_FIFO(tfr);
     InCounter--;

//     printf("KA: %04x %08x\n", InCounter, tfr);

     need_eat = 0;
     PixelBufferCount32 = 0;
     WriteImageData(tfr, &need_eat);
     WriteImageData(tfr >> 16, &need_eat);

     MDEC_EAT_CLOCKS(need_eat);

     PixelBufferReadOffset = 0;
     while(PixelBufferReadOffset < PixelBufferCount32)
     {
      MDEC_WRITE_FIFO((PixelBuffer.pix32[PixelBufferReadOffset++]));
     }
    } while(InCounter != 0xFFFF);
   }
   //
   //
   //
   else if(((Command >> 29) & 0x7) == 2)
   {
    QMIndex = 0;
    InCounter = 0x10 + ((Command & 0x1) ? 0x10 : 0x00);

    InCounter--;
    do
    {
	uint32_t tfr;
    
	MDEC_READ_FIFO(tfr);
	InCounter--;

	//printf("KA: %04x %08x\n", InCounter, tfr);

	for(int i = 0; i < 4; i++)
	{
         QMatrix[QMIndex >> 6][QMIndex & 0x3F] = (uint8_t)tfr;
	 QMIndex = (QMIndex + 1) & 0x7F;
	 tfr >>= 8;
	}
    } while(InCounter != 0xFFFF);
   }
   //
   //
   //
   else if(((Command >> 29) & 0x7) == 3)
   {
    IDCTMIndex = 0;
    InCounter = 0x20;

    InCounter--;
    do
    {
     uint32_t tfr;

     MDEC_READ_FIFO(tfr);
     InCounter--;

     for(unsigned i = 0; i < 2; i++)
     {
      IDCTMatrix[((IDCTMIndex & 0x7) << 3) | ((IDCTMIndex >> 3) & 0x7)] = (int16_t)(tfr & 0xFFFF) >> 3;
      IDCTMIndex = (IDCTMIndex + 1) & 0x3F;

      tfr >>= 16;
     }
    } while(InCounter != 0xFFFF);
   }
   else
   {
    InCounter = Command & 0xFFFF;
   }
  } // end for(;;)
 }
}

void MDEC_DMAWrite(uint32_t V)
{
 if((0x20 - InFIFO.count))
 {
  fifoWrite(&InFIFO, V);
  MDEC_Run(0);
 }
 else
 {
  ;
 }
}

uint32_t MDEC_DMARead(uint32_t* offs)
{
 uint32_t V = 0;

 *offs = 0;

 if(MDFN_LIKELY(OutFIFO.count))
 {
  V = fifoRead(&OutFIFO);

  *offs = (RAMOffsetY & 0x7) * RAMOffsetWWS;

  if(RAMOffsetY & 0x08)
  {
   *offs = (*offs - RAMOffsetWWS*7);
  }

  RAMOffsetCounter--;
  if(!RAMOffsetCounter)
  {
   RAMOffsetCounter = RAMOffsetWWS;
   RAMOffsetY++;
  }

  MDEC_Run(0);
 }
 else
 {
  ;
 }

 return(V);
}

bool MDEC_DMACanWrite(void)
{
 return(((0x20 - InFIFO.count) >= 0x20) && (Control & (1U << 30)) && InCommand && InCounter != 0xFFFF);
}

bool MDEC_DMACanRead(void)
{
 return((OutFIFO.count >= 0x20) && (Control & (1U << 29)));
}

void MDEC_Write(const int64_t timestamp, uint32_t A, uint32_t V)
{
 //PSX_WARNING("[MDEC] Write: 0x%08x 0x%08x, %d  --- %u %u", A, V, timestamp, InFIFO.count, OutFIFO.count);
 if(A & 4)
 {
  if(V & 0x80000000) // Reset?
  {
   MDRPhase = 0;
   InCounter = 0;
   Command = 0;
   InCommand = false;

   PixelBufferCount32 = 0;
   ClockCounter = 0;
   QMIndex = 0;
   IDCTMIndex = 0;

   QScale = 0;

   memset(Coeff, 0, sizeof(Coeff));
   CoeffIndex = 0;
   DecodeWB = 0;

   fifoFlush(&InFIFO);
   fifoFlush(&OutFIFO);
  }
  Control = V & 0x7FFFFFFF;
 }
 else
 {
  if((0x20 - InFIFO.count))
  {
   fifoWrite(&InFIFO, V);

   if(!InCommand)
   {
    if(ClockCounter < 1)
     ClockCounter = 1;
   }
   MDEC_Run(0);
  }
  else
  {
   ;
  }
 }
}

uint32_t MDEC_Read(const int64_t timestamp, uint32_t A)
{
 uint32_t ret = 0;

 if(A & 4)
 {
  ret = 0;

  ret |= (OutFIFO.count == 0) << 31;
  ret |= ((0x20 - InFIFO.count) == 0) << 30;
  ret |= InCommand << 29;

  ret |= MDEC_DMACanWrite() << 28;
  ret |= MDEC_DMACanRead() << 27;

  ret |= ((Command >> 25) & 0xF) << 23;

  // Needs refactoring elsewhere to work right: ret |= ((DecodeWB + 4) % 6) << 16;

  ret |= InCounter & 0xFFFF;
 }
 else
 {
  if(OutFIFO.count)
   ret = fifoRead(&OutFIFO);
 }

 //PSX_WARNING("[MDEC] Read: 0x%08x 0x%08x -- %d %d", A, ret, InputBuffer.CanRead(), InCounter);

 return(ret);
}


// armPSX's side: the DMA moves whole blocks at once, so the MDEC runs until it waits.

static void runUntilStuck(void) {
 for (int i = 0; i < 4096; i++) {
  unsigned phase = MDRPhase, in = InFIFO.count, outc = OutFIFO.count;
  MDEC_Run(128);
  if (MDRPhase == phase && InFIFO.count == in && OutFIFO.count == outc && ClockCounter > 0) break;
 }
}

void mdecReset(struct Psx* p) {
 MDEC_Power();
}

uint32_t mdecRead(struct Psx* p, uint32_t address) {
 runUntilStuck();
 return MDEC_Read(p->cycles, address);
}

void mdecWrite(struct Psx* p, uint32_t address, uint32_t value) {
 MDEC_Write(p->cycles, address, value);
 runUntilStuck();
}

void mdecDmaWrite(struct Psx* p, uint32_t value) {
 if (InFIFO.count == 0x20) runUntilStuck();
 MDEC_DMAWrite(value);
}

uint32_t mdecDmaRead(struct Psx* p, uint32_t* offset) {
 if (!OutFIFO.count) runUntilStuck();
 return MDEC_DMARead(offset);
}
