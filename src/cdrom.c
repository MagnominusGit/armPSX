/* armPSX: Mednafen's cdc.cpp converted to C (file-level state, no save states yet). */
/******************************************************************************/
/* Mednafen Sony PS1 Emulation Module                                         */
/******************************************************************************/
/* cdc.cpp:
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

// TODO: Test seekl to CD-DA, and seekl to beyond end of disc.

// TODO: Rewrite command processing to use switch() state machine madness, to handle some commands(like 0x0A) whose timing
//       characteristics cannot be handled well with the current way.  Be sure to test interruptability(with a new command) of the commands, too.

#if defined(__GNUC__) && !defined(__clang__)
 #pragma GCC optimize ("unroll-loops")
#endif

/*
  Games to test after changing code affecting CD reading and buffering:
	Bedlam (massive graphics corruption)
	Rise 2 (massive graphics corruption)

	Captain Commando (boss graphics corruption on first stage)
	Gran Turismo (missing music; GetLocL must be valid and sector data must be ready simultaneous with clearing of status seek bit and setting of reading bit)
	Harukanaru Toki no Naka de - Banjou Yuugi (needs GetLocL to reflect dancing around target seek position after seek completes; otherwise hangs on voice acting)
	Incredible Crisis (needs GetLocL to be valid after a SeekL; otherwise hangs without music near start of "Etsuko and the Golden Pig")
	Tomb Raider(needs GetLocP to reflect dancing around just before target seek position after seek completes; otherwise, CD-DA tracks at M:S:F=x:x:0 fail to play and game hangs)
	Vib Ribbon, with extra audio CD
	Mortal Kombat Trilogy, music resumption after pause.

  Nightmare Creatures hangs during startup for some CD timing alignments/windows, and it spams command 0x0A without properly waiting for it to complete.
*/

// TODO: async command counter and async command phase?
/*

 TODO:
	Implement missing commands.

	SPU CD-DA and CD-XA streaming semantics.
*/

/*
 After eject(doesn't appear to occur when drive is in STOP state):
	* Does not appear to occur in STOP state.
	* Does not appear to occur in PAUSE state.
	* DOES appear to occur in STANDBY state. (TODO: retest)

% Result 0: 16
% Result 1: 08
% IRQ Result: e5
% 19 e0

 Command abortion tests(NOP tested):
	Does not appear to occur when in STOP or PAUSE states(STOP or PAUSE command just executed).

	DOES occur after a ReadTOC completes, if ReadTOC is not followed by a STOP or PAUSE.  Odd.
*/

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "psx.h"
#include "disc.h"

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

struct CD_Audio_Buffer {
 int16_t Samples[2][0x1000];
 uint32_t Size;
 uint32_t Freq;
 uint32_t ReadPos;
};

enum { SectorPipe_Count = 2 };
enum
 {
  CDCIRQ_NONE = 0,
  CDCIRQ_DATA_READY = 1,
  CDCIRQ_COMPLETE = 2,
  CDCIRQ_ACKNOWLEDGE = 3,
  CDCIRQ_DATA_END = 4,
  CDCIRQ_DISC_ERROR = 5
 };
enum
 {
  ERRCODE_BAD_ARGVAL  = 0x10,
  ERRCODE_BAD_NUMARGS = 0x20,
  ERRCODE_BAD_COMMAND = 0x40,
  ERRCODE_NOT_READY = 0x80,	// 0x80 (happens with getlocl when drive isn't reading, pause when tray is open, and MAYBE when trying to run an async
				//	 command while another async command is currently in its asynch phase being executed[pause when in readtoc, todo test more])
 };
enum { MODE_SPEED = 0x80 };
enum { MODE_STRSND = 0x40 };
enum { MODE_SIZE = 0x20 };
enum { MODE_SIZE2 = 0x10 };
enum { MODE_SF = 0x08 };
enum { MODE_REPORT = 0x04 };
enum { MODE_AUTOPAUSE = 0x02 };
enum { MODE_CDDA = 0x01 };
enum
 {
  DS_STANDBY = -2,
  DS_PAUSED = -1,
  DS_STOPPED = 0,
  DS_SEEKING,
  DS_SEEKING_LOGICAL,
  DS_SEEKING_LOGICAL2,
  DS_PLAYING,
  DS_READING,
  //DS_RESETTING
 };
static struct Disc* Cur_CDIF;
static bool DiscChanged;
static int32_t DiscStartupDelay;
static struct CD_Audio_Buffer AudioBuffer;
static uint8_t Pending_DecodeVolume[2][2], DecodeVolume[2][2];
static int16_t ADPCM_ResampBuf[2][32 * 2];
static uint8_t ADPCM_ResampCurPos;
static uint8_t ADPCM_ResampCurPhase;
static uint8_t RegSelector;
static uint8_t ArgsBuf[16];
static uint8_t ArgsWP;
static uint8_t ArgsRP;
static uint8_t ArgsReceiveLatch;
static uint8_t ArgsReceiveBuf[32];
static uint8_t ArgsReceiveIn;
static uint8_t ResultsBuffer[16];
static uint8_t ResultsIn;
static uint8_t ResultsWP;
static uint8_t ResultsRP;
static struct { uint8_t data[4096]; uint32_t read_pos, write_pos, in_count; } DMABuffer;
static uint8_t SB[2340];
static uint32_t SB_In;
static uint8_t SectorPipe[SectorPipe_Count][2352];
static uint8_t SectorPipe_Pos;
static uint8_t SectorPipe_In;
static uint8_t SubQBuf[0xC];
static uint8_t SubQBuf_Safe[0xC];
static bool SubQChecksumOK;
static bool HeaderBufValid;
static uint8_t HeaderBuf[12];
static uint8_t IRQBuffer;
static uint8_t IRQOutTestMask;
static int32_t CDCReadyReceiveCounter;
static uint8_t FilterFile;
static uint8_t FilterChan;
static uint8_t PendingCommand;
static int PendingCommandPhase;
static int32_t PendingCommandCounter;
static int32_t SPUCounter;
static uint8_t Mode;
static int DriveStatus;
static int StatusAfterSeek;
static bool Forward;
static bool Backward;
static bool Muted;
static int32_t PlayTrackMatch;
static int32_t PSRCounter;
static bool HoldLogicalPos;
static int32_t CurSector;
static uint32_t SectorsRead;
static unsigned AsyncIRQPending;
static uint8_t AsyncResultsPending[16];
static uint8_t AsyncResultsPendingCount;
static int32_t SeekTarget;
static uint32_t SeekRetryCounter;
static int SeekFinished;
static int64_t lastts;
static struct Toc toc;
static bool IsPSXDisc;
static uint8_t DiscID[4];
static int32_t CommandLoc;
static bool CommandLoc_Dirty;
static int16_t xa_previous[2][2];
static uint8_t ReportLastF;
static int32_t ReportStartupDelay;


typedef struct {
 uint8_t args_min, args_max;
 const char *name;
 int32_t (*func)(const int arg_count, const uint8_t *args);
 int32_t (*func2)(void);
} CDC_CTEntry;

static const CDC_CTEntry Commands[0x20];
static void fifoFlush(void) { DMABuffer.read_pos = DMABuffer.write_pos = DMABuffer.in_count = 0; }
static uint8_t fifoReadByte(void) { uint8_t v = DMABuffer.data[DMABuffer.read_pos]; DMABuffer.read_pos = (DMABuffer.read_pos + 1) & 4095; DMABuffer.in_count--; return v; }
static void fifoWriteByte(uint8_t v) { DMABuffer.data[DMABuffer.write_pos] = v; DMABuffer.write_pos = (DMABuffer.write_pos + 1) & 4095; DMABuffer.in_count++; }
static void fifoWrite(const uint8_t* p, uint32_t n) { for (uint32_t i = 0; i < n; i++) fifoWriteByte(p[i]); }
static inline void clamp(int32_t* v, int32_t lo, int32_t hi) { if (*v < lo) *v = lo; if (*v > hi) *v = hi; }
static inline int16_t de16lsb(const uint8_t* p) { return (int16_t) (p[0] | p[1] << 8); }
static uint32_t PSX_GetRandU32(uint32_t mina, uint32_t maxa);

static struct Psx* P;
static bool irqLevel;
static void cdcIrq(bool level) {
 if (level && !irqLevel) psxIrq(P, IRQ_CDROM);
 irqLevel = level;
}
static void cdcSchedule(int64_t ts) { psxSchedule(P, ts); }

// Mednafen's PRNG (psx.cpp), reset at power so runs repeat.
static uint32_t rx, ry, rz, rc;
static uint64_t rlcgo;
static void PSX_ResetRand(void) { rx = 123456789; ry = 987654321; rz = 43219876; rc = 6543217; rlcgo = 0xDEADBEEFCAFEBABEULL; }
static uint32_t PSX_RandU32(void) {
 uint64_t t;
 rx = 314527869 * rx + 1234567;
 ry ^= ry << 5; ry ^= ry >> 7; ry ^= ry << 22;
 t = 4294584393ULL * rz + rc; rc = t >> 32; rz = (uint32_t) t;
 rlcgo = (19073486328125ULL * rlcgo) + 1;
 return (rx + ry + rz) ^ (uint32_t) (rlcgo >> 16);
}
static uint32_t PSX_GetRandU32(uint32_t mina, uint32_t maxa) {
 const uint32_t range_m1 = maxa - mina;
 uint32_t mask = range_m1, tmp;
 mask |= mask >> 1; mask |= mask >> 2; mask |= mask >> 4; mask |= mask >> 8; mask |= mask >> 16;
 do { tmp = PSX_RandU32() & mask; } while (tmp > range_m1);
 return mina + tmp;
}
static bool CommandCheckDiscPresent(void);
static bool DecodeSubQ(uint8_t *subpw);
static bool XA_Test(const uint8_t *sdata);
static int32_t CalcSeekTime(int32_t initial, int32_t target, bool motor_on, bool paused);
static int32_t Command_0x1d(const int arg_count, const uint8_t *args);
static int32_t Command_Backward(const int arg_count, const uint8_t *args);
static int32_t Command_Demute(const int arg_count, const uint8_t *args);
static int32_t Command_Forward(const int arg_count, const uint8_t *args);
static int32_t Command_GetTD(const int arg_count, const uint8_t *args);
static int32_t Command_GetTN(const int arg_count, const uint8_t *args);
static int32_t Command_GetlocL(const int arg_count, const uint8_t *args);
static int32_t Command_GetlocP(const int arg_count, const uint8_t *args);
static int32_t Command_Getparam(const int arg_count, const uint8_t *args);
static int32_t Command_ID(const int arg_count, const uint8_t *args);
static int32_t Command_ID_Part2(void);
static int32_t Command_Init(const int arg_count, const uint8_t *args);
static int32_t Command_Mute(const int arg_count, const uint8_t *args);
static int32_t Command_Nop(const int arg_count, const uint8_t *args);
static int32_t Command_Pause(const int arg_count, const uint8_t *args);
static int32_t Command_Pause_Part2(void);
static int32_t Command_Play(const int arg_count, const uint8_t *args);
static int32_t Command_ReadN(const int arg_count, const uint8_t *args);
static int32_t Command_ReadS(const int arg_count, const uint8_t *args);
static int32_t Command_ReadT(const int arg_count, const uint8_t *args);
static int32_t Command_ReadTOC(const int arg_count, const uint8_t *args);
static int32_t Command_ReadTOC_Part2(void);
static int32_t Command_ReadT_Part2(void);
static int32_t Command_Reset(const int arg_count, const uint8_t *args);
static int32_t Command_SeekL(const int arg_count, const uint8_t *args);
static int32_t Command_SeekP(const int arg_count, const uint8_t *args);
static int32_t Command_Seek_PartN(void);
static int32_t Command_Setfilter(const int arg_count, const uint8_t *args);
static int32_t Command_Setloc(const int arg_count, const uint8_t *args);
static int32_t Command_Setmode(const int arg_count, const uint8_t *args);
static int32_t Command_Standby(const int arg_count, const uint8_t *args);
static int32_t Command_Standby_Part2(void);
static int32_t Command_Stop(const int arg_count, const uint8_t *args);
static int32_t Command_Stop_Part2(void);
static int32_t Command_Test(const int arg_count, const uint8_t *args);
static uint8_t MakeStatus(bool cmd_error);
static uint8_t ReadResult(void);
static void ApplyVolume(int32_t samples[2]);
static void BeginResults(void);
static void CheckAIP(void);
static void ClearAIP(void);
static void ClearAudioBuffers(void);
static void EnbufferizeCDDASector(const uint8_t *buf);
static void HandlePlayRead(void);
static void PreSeekHack(int32_t target);
static void ReadAudioBuffer(int32_t samples[2]);
static void ReadBase(void);
static void RecalcIRQ(void);
static void SetAIP1(unsigned irq, uint8_t result0);
static void SetAIP2(unsigned irq, uint8_t result0, uint8_t result1);
static void SetAIP_Buf(unsigned irq, unsigned result_count, uint8_t *r);
static void WriteIRQ(uint8_t V);
static void WriteResult(uint8_t V);
static void XA_ProcessSector(const uint8_t *sdata, struct CD_Audio_Buffer *ab);


void CDC_Init(void)
{
 IsPSXDisc = false;
 Cur_CDIF = NULL;

 DriveStatus = DS_STOPPED;
 PendingCommandPhase = 0;
}


void CDC_SetDisc(bool tray_open, struct Disc* cdif, const char *disc_id)
{
 if(tray_open)
  cdif = NULL;

 Cur_CDIF = cdif;
 IsPSXDisc = false;
 memset(DiscID, 0, sizeof(DiscID));

 if(Cur_CDIF)
 {
  HeaderBufValid = false;
  DiscStartupDelay = (int64_t)1000 * 33868800 / 1000;
  DiscChanged = true;

  discToc(Cur_CDIF, &toc);

  if(disc_id)
  {
   memcpy(DiscID, disc_id, MIN(4, strlen(disc_id)));
   IsPSXDisc = true;
  }
 }
}

int32_t CDC_CalcNextEvent(void)
{
 int32_t next_event = SPUCounter;

 if(PSRCounter > 0 && next_event > PSRCounter)
  next_event = PSRCounter;

 if(PendingCommandCounter > 0 && next_event > PendingCommandCounter)
  next_event = PendingCommandCounter;

 if(!(IRQBuffer & 0xF))
 {
  if(CDCReadyReceiveCounter > 0 && next_event > CDCReadyReceiveCounter)
   next_event = CDCReadyReceiveCounter;
 }

 if(DiscStartupDelay > 0 && next_event > DiscStartupDelay)
  next_event = DiscStartupDelay;

 //fprintf(stderr, "%d %d %d %d --- %d\n", PSRCounter, PendingCommandCounter, CDCReadyReceiveCounter, DiscStartupDelay, next_event);

 return(next_event);
}

void CDC_SoftReset(void)
{
 ClearAudioBuffers();

 ReportLastF = 0;
 ReportStartupDelay = 0;

 // Not sure about initial volume state
 Pending_DecodeVolume[0][0] = 0x80;
 Pending_DecodeVolume[0][1] = 0x00;
 Pending_DecodeVolume[1][0] = 0x00;
 Pending_DecodeVolume[1][1] = 0x80;
 memcpy(DecodeVolume, Pending_DecodeVolume, sizeof(DecodeVolume));

 RegSelector = 0;
 memset(ArgsBuf, 0, sizeof(ArgsBuf));
 ArgsWP = ArgsRP = 0;

 ArgsReceiveLatch = 0;
 memset(ArgsReceiveBuf, 0, sizeof(ArgsReceiveBuf));
 ArgsReceiveIn = 0;

 memset(ResultsBuffer, 0, sizeof(ResultsBuffer));
 ResultsWP = 0;
 ResultsRP = 0;
 ResultsIn = 0;

 CDCReadyReceiveCounter = 0;

 IRQBuffer = 0;
 IRQOutTestMask = 0;
 RecalcIRQ();

 fifoFlush();
 SB_In = 0;
 SectorPipe_Pos = SectorPipe_In = 0;
 SectorsRead = 0;

 memset(SubQBuf, 0, sizeof(SubQBuf));
 memset(SubQBuf_Safe, 0, sizeof(SubQBuf_Safe));
 SubQChecksumOK = false;

 memset(HeaderBuf, 0, sizeof(HeaderBuf));


 FilterFile = 0;
 FilterChan = 0;

 PendingCommand = 0;
 PendingCommandPhase = 0;
 PendingCommandCounter = 0;

 Mode = 0x20;

 HeaderBufValid = false;
 SeekFinished = -1;
 DriveStatus = DS_STOPPED;
 ClearAIP();
 StatusAfterSeek = DS_STOPPED;
 SeekRetryCounter = 0;

 Forward = false;
 Backward = false;
 Muted = false;

 PlayTrackMatch = 0;

 PSRCounter = 0;

 CurSector = 0;

 ClearAIP();

 SeekTarget = 0;

 CommandLoc = 0;
 CommandLoc_Dirty = true;

 DiscChanged = true;
}

void CDC_Power(void)
{
 SPU_Power();

 memset(SectorPipe, 0, sizeof(SectorPipe));
 memset(SB, 0, sizeof(SB));
 memset(AsyncResultsPending, 0, sizeof(AsyncResultsPending));
 memset(DMABuffer.data, 0, sizeof DMABuffer.data);
 CDC_SoftReset();

 HoldLogicalPos = false;

 DiscStartupDelay = 0;

 SPUCounter = SPU_UpdateFromCDC(0);
 lastts = 0;
}


void CDC_ResetTS(void)
{
 lastts = 0;
}

static void RecalcIRQ(void)
{
 cdcIrq((bool)(IRQBuffer & (IRQOutTestMask & 0x1F)));
}

//static int32_t doom_ts;
static void WriteIRQ(uint8_t V)
{
 assert(CDCReadyReceiveCounter <= 0);
 assert(!(IRQBuffer & 0xF));

 //

 CDCReadyReceiveCounter = 2000; //1024;

 IRQBuffer = (IRQBuffer & 0x10) | V;
 RecalcIRQ();
}

static void BeginResults(void)
{
 //if(ResultsIn)
 // {
 // printf("Cleared %d results. IRQBuffer=0x%02x\n", ResultsIn, IRQBuffer);
 //}

 ResultsIn = 0;
 ResultsWP = 0;
 ResultsRP = 0;

 memset(ResultsBuffer, 0x00, sizeof(ResultsBuffer));
}

static void WriteResult(uint8_t V)
{
 ResultsBuffer[ResultsWP] = V;
 ResultsWP = (ResultsWP + 1) & 0xF;
 ResultsIn = (ResultsIn + 1) & 0x1F;

  
}

static uint8_t ReadResult(void)
{
 uint8_t ret = ResultsBuffer[ResultsRP];


 ResultsRP = (ResultsRP + 1) & 0xF;
 ResultsIn = (ResultsIn - 1) & 0x1F;

 return ret;
}

static uint8_t MakeStatus(bool cmd_error)
{
 uint8_t ret = 0;

 // Are these bit positions right?

 if(DriveStatus == DS_PLAYING)
  ret |= 0x80;

 if(DriveStatus == DS_READING)
  ret |= 0x20;

 if(DriveStatus == DS_SEEKING || DriveStatus == DS_SEEKING_LOGICAL || DriveStatus == DS_SEEKING_LOGICAL2)
  ret |= 0x40;

 if(!Cur_CDIF || DiscChanged)
  ret |= 0x10;

 if(DriveStatus != DS_STOPPED)
  ret |= 0x02;

 if(cmd_error)
  ret |= 0x01;

 DiscChanged = false;	// FIXME: Only do it on NOP command execution?

 return(ret);
}

static bool DecodeSubQ(uint8_t *subpw)
{
 uint8_t tmp_q[0xC];

 memset(tmp_q, 0, 0xC);

 for(int i = 0; i < 96; i++)
  tmp_q[i >> 3] |= ((subpw[i] & 0x40) >> 6) << (7 - (i & 7));

 if((tmp_q[0] & 0xF) == 1)
 {
  memcpy(SubQBuf, tmp_q, 0xC);
  SubQChecksumOK = subq_check_checksum(tmp_q);

  if(SubQChecksumOK)
  {
   memcpy(SubQBuf_Safe, tmp_q, 0xC);
   return(true);
  }
 }

 return(false);
}

static const int16_t CDADPCMImpulse[7][25] =
{
 {     0,    -5,    17,   -35,    70,   -23,   -68,   347,  -839,  2062, -4681, 15367, 21472, -5882,  2810, -1352,   635,  -235,    26,    43,   -35,    16,    -8,     2,     0,  }, /* 0 */
 {     0,    -2,    10,   -34,    65,   -84,    52,     9,  -266,  1024, -2680,  9036, 26516, -6016,  3021, -1571,   848,  -365,   107,    10,   -16,    17,    -8,     3,    -1,  }, /* 1 */
 {    -2,     0,     3,   -19,    60,   -75,   162,  -227,   306,   -67,  -615,  3229, 29883, -4532,  2488, -1471,   882,  -424,   166,   -27,     5,     6,    -8,     3,    -1,  }, /* 2 */
 {    -1,     3,    -2,    -5,    31,   -74,   179,  -402,   689,  -926,  1272, -1446, 31033, -1446,  1272,  -926,   689,  -402,   179,   -74,    31,    -5,    -2,     3,    -1,  }, /* 3 */
 {    -1,     3,    -8,     6,     5,   -27,   166,  -424,   882, -1471,  2488, -4532, 29883,  3229,  -615,   -67,   306,  -227,   162,   -75,    60,   -19,     3,     0,    -2,  }, /* 4 */
 {    -1,     3,    -8,    17,   -16,    10,   107,  -365,   848, -1571,  3021, -6016, 26516,  9036, -2680,  1024,  -266,     9,    52,   -84,    65,   -34,    10,    -2,     0,  }, /* 5 */
 {     0,     2,    -8,    16,   -35,    43,    26,  -235,   635, -1352,  2810, -5882, 21472, 15367, -4681,  2062,  -839,   347,   -68,   -23,    70,   -35,    17,    -5,     0,  }, /* 6 */
};

static void ReadAudioBuffer(int32_t samples[2])
{
 samples[0] = AudioBuffer.Samples[0][AudioBuffer.ReadPos];
 samples[1] = AudioBuffer.Samples[1][AudioBuffer.ReadPos];

 AudioBuffer.ReadPos++;
}

static inline void ApplyVolume(int32_t samples[2])
{
 //
 // Take care not to alter samples[] before we're done calculating the new output samples!
 //
 int32_t left_out = ((samples[0] * DecodeVolume[0][0]) >> 7) + ((samples[1] * DecodeVolume[1][0]) >> 7);
 int32_t right_out = ((samples[0] * DecodeVolume[0][1]) >> 7) + ((samples[1] * DecodeVolume[1][1]) >> 7);

 clamp(&left_out, -32768, 32767);
 clamp(&right_out, -32768, 32767);

 if(Muted)
 {
  left_out = 0;
  right_out = 0;
 }

 samples[0] = left_out;
 samples[1] = right_out;
}

//
// This function must always set samples[0] and samples[1], even if just to 0; range of samples[n] shall be restricted to -32768 through 32767.
//
void CDC_GetCDAudio(int32_t samples[2])
{
 const unsigned freq = (AudioBuffer.ReadPos < AudioBuffer.Size) ? AudioBuffer.Freq : 0;

 samples[0] = 0;
 samples[1] = 0;

 if(!freq)
  return;

 if(freq == 7 || freq == 14)
 {
  ReadAudioBuffer(samples);
  if(freq == 14)
   ReadAudioBuffer(samples);
 }
 else
 {
  int32_t out_tmp[2] = { 0, 0 };

  for(unsigned i = 0; i < 2; i++)
  {
   const int16_t* imp = CDADPCMImpulse[ADPCM_ResampCurPhase];
   int16_t* wf = &ADPCM_ResampBuf[i][(ADPCM_ResampCurPos + 32 - 25) & 0x1F];

   for(unsigned s = 0; s < 25; s++)
   {
    out_tmp[i] += imp[s] * wf[s];
   }

   out_tmp[i] >>= 15;
   clamp(&out_tmp[i], -32768, 32767);
   samples[i] = out_tmp[i];
  }

  ADPCM_ResampCurPhase += freq;

  if(ADPCM_ResampCurPhase >= 7)
  {
   int32_t raw[2] = { 0, 0 };

   ADPCM_ResampCurPhase -= 7;
   ReadAudioBuffer(raw);

   for(unsigned i = 0; i < 2; i++)
   {
    ADPCM_ResampBuf[i][ADPCM_ResampCurPos +  0] = 
    ADPCM_ResampBuf[i][ADPCM_ResampCurPos + 32] = raw[i];
   }
   ADPCM_ResampCurPos = (ADPCM_ResampCurPos + 1) & 0x1F;
  }
 }

 //
 // Algorithmically, volume is applied after resampling for CD-XA ADPCM playback, per PS1 tests(though when "mute" is applied wasn't tested).
 //
 ApplyVolume(samples);
}


typedef struct XA_Subheader
{
 uint8_t file;
 uint8_t channel;
 uint8_t submode;
 uint8_t coding;

 uint8_t file_dup;
 uint8_t channel_dup;
 uint8_t submode_dup;
 uint8_t coding_dup;
} XA_Subheader;
_Static_assert(sizeof(XA_Subheader) == 8, "XA_Subheader wrong size!");

typedef struct XA_SoundGroup
{
 uint8_t params[16];
 uint8_t samples[112];
} XA_SoundGroup;
_Static_assert(sizeof(XA_SoundGroup) == 128, "XA_SoundGroup wrong size!");

#define XA_SUBMODE_EOF		0x80
#define XA_SUBMODE_REALTIME	0x40
#define XA_SUBMODE_FORM		0x20
#define XA_SUBMODE_TRIGGER	0x10
#define XA_SUBMODE_DATA		0x08
#define XA_SUBMODE_AUDIO	0x04
#define XA_SUBMODE_VIDEO	0x02
#define XA_SUBMODE_EOR		0x01

#define XA_CODING_EMPHASIS	0x40

//#define XA_CODING_BPS_MASK	0x30
//#define XA_CODING_BPS_4BIT	0x00
//#define XA_CODING_BPS_8BIT	0x10
//#define XA_CODING_SR_MASK	0x0C
//#define XA_CODING_SR_378	0x00
//#define XA_CODING_SR_

#define XA_CODING_8BIT		0x10
#define XA_CODING_189		0x04
#define XA_CODING_STEREO	0x01

// Special regression prevention test cases:
//	Um Jammer Lammy (start doing poorly)
//	Yarudora Series Vol.1 - Double Cast (non-FMV speech)

static bool XA_Test(const uint8_t *sdata)
{
 const XA_Subheader *sh = (const XA_Subheader *)&sdata[12 + 4];

 if(!(Mode & MODE_STRSND))
  return false;

 if(!(sh->submode & XA_SUBMODE_AUDIO))
  return false;

 //printf("Test File: 0x%02x 0x%02x - Channel: 0x%02x 0x%02x - Submode: 0x%02x 0x%02x - Coding: 0x%02x 0x%02x - \n", sh->file, sh->file_dup, sh->channel, sh->channel_dup, sh->submode, sh->submode_dup, sh->coding, sh->coding_dup);

 if((Mode & MODE_SF) && (sh->file != FilterFile || sh->channel != FilterChan))
  return false;

 return true;
}

static void ClearAudioBuffers(void)
{
 memset(&AudioBuffer, 0, sizeof(AudioBuffer));
 memset(xa_previous, 0, sizeof(xa_previous));

 memset(ADPCM_ResampBuf, 0, sizeof(ADPCM_ResampBuf));
 ADPCM_ResampCurPhase = 0;
 ADPCM_ResampCurPos = 0;
}

//
// output should be readable at -2 and -1
static void DecodeXAADPCM(const uint8_t *input, int16_t *output, const unsigned shift, const unsigned weight)
{
 // Weights copied over from SPU channel ADPCM playback code, may not be entirely the same for CD-XA ADPCM, we need to run tests.
 static const int32_t Weights[16][2] =
 {
  // s-1    s-2
  {   0,    0 },
  {  60,    0 },
  { 115,  -52 },
  {  98,  -55 },
  { 122,  -60 },
 };

 for(int i = 0; i < 28; i++)
 {
  int32_t sample;

  sample = (int16_t)(input[i] << 8);
  sample >>= shift;

  sample += ((output[i - 1] * Weights[weight][0]) >> 6) + ((output[i - 2] * Weights[weight][1]) >> 6);

  if(sample < -32768)
   sample = -32768;

  if(sample > 32767)
   sample = 32767;

  output[i] = sample;
 }
}

static void XA_ProcessSector(const uint8_t *sdata, struct CD_Audio_Buffer *ab)
{
 const XA_Subheader *sh = (const XA_Subheader *)&sdata[12 + 4];
 const unsigned unit_index_shift = (sh->coding & XA_CODING_8BIT) ? 0 : 1;

 //printf("File: 0x%02x 0x%02x - Channel: 0x%02x 0x%02x - Submode: 0x%02x 0x%02x - Coding: 0x%02x 0x%02x - \n", sh->file, sh->file_dup, sh->channel, sh->channel_dup, sh->submode, sh->submode_dup, sh->coding, sh->coding_dup);
 ab->ReadPos = 0;
 ab->Size = 18 * (4 << unit_index_shift) * 28;

 if(sh->coding & XA_CODING_STEREO)
  ab->Size >>= 1;

 ab->Freq = (sh->coding & XA_CODING_189) ? 3 : 6;

 //fprintf(stderr, "Coding: %02x %02x\n", sh->coding, sh->coding_dup);

 for(unsigned group = 0; group < 18; group++)
 {
  const XA_SoundGroup *sg = (const XA_SoundGroup *)&sdata[12 + 4 + 8 + group * 128];

  for(unsigned unit = 0; unit < (4U << unit_index_shift); unit++)
  {
   const uint8_t param = sg->params[(unit & 3) | ((unit & 4) << 1)];
   const uint8_t param_copy = sg->params[4 | (unit & 3) | ((unit & 4) << 1)];
   uint8_t ibuffer[28];
   int16_t obuffer[2 + 28];

   if(param != param_copy)
   {
    
   }

   for(unsigned i = 0; i < 28; i++)
   {
    uint8_t tmp = sg->samples[i * 4 + (unit >> unit_index_shift)];

    if(unit_index_shift)
    {
     tmp <<= (unit & 1) ? 0 : 4;
     tmp &= 0xf0;
    }

    ibuffer[i] = tmp;
   }

   const bool ocn = (bool)(unit & 1) && (sh->coding & XA_CODING_STEREO);

   obuffer[0] = xa_previous[ocn][0];
   obuffer[1] = xa_previous[ocn][1];

   DecodeXAADPCM(ibuffer, &obuffer[2], param & 0x0F, param >> 4);

   xa_previous[ocn][0] = obuffer[28];
   xa_previous[ocn][1] = obuffer[29];

   if(param != param_copy)
    memset(obuffer, 0, sizeof(obuffer));

   if(sh->coding & XA_CODING_STEREO)
   {
    for(unsigned s = 0; s < 28; s++)
    {
     ab->Samples[ocn][group * (2 << unit_index_shift) * 28 + (unit >> 1) * 28 + s] = obuffer[2 + s];
    }
   }
   else
   {
    for(unsigned s = 0; s < 28; s++)
    {
     ab->Samples[0][group * (4 << unit_index_shift) * 28 + unit * 28 + s] = obuffer[2 + s];
     ab->Samples[1][group * (4 << unit_index_shift) * 28 + unit * 28 + s] = obuffer[2 + s];
    }
   }
  }
 }

#if 0
 // Test
 for(unsigned i = 0; i < ab->Size; i++)
 {
  static unsigned counter = 0;

  ab->Samples[0][i] = (counter & 2) ? -0x6000 : 0x6000;
  ab->Samples[1][i] = rand();
  counter++;
 }
#endif
}

static void ClearAIP(void)
{
 AsyncResultsPendingCount = 0;
 AsyncIRQPending = 0;
}

static void CheckAIP(void)
{
 if(AsyncIRQPending && CDCReadyReceiveCounter <= 0)
 {
  BeginResults();

  for(unsigned i = 0; i < AsyncResultsPendingCount; i++)
   WriteResult(AsyncResultsPending[i]);

  WriteIRQ(AsyncIRQPending);

  ClearAIP();
 }
}

static void SetAIP_Buf(unsigned irq, unsigned result_count, uint8_t *r)
{
 if(AsyncIRQPending)
 {
  
 }
 ClearAIP();

 AsyncResultsPendingCount = result_count;

 for(unsigned i = 0; i < result_count; i++)
  AsyncResultsPending[i] = r[i];

 AsyncIRQPending = irq;

 CheckAIP();
}

static void SetAIP1(unsigned irq, uint8_t result0)
{
 uint8_t tr[1] = { result0 };
 SetAIP_Buf(irq, 1, tr);
}

static void SetAIP2(unsigned irq, uint8_t result0, uint8_t result1)
{
 uint8_t tr[2] = { result0, result1 };
 SetAIP_Buf(irq, 2, tr);
}


static void EnbufferizeCDDASector(const uint8_t *buf)
{
	struct CD_Audio_Buffer *ab = &AudioBuffer;

        ab->Freq = 7 * ((Mode & MODE_SPEED) ? 2 : 1);
        ab->Size = 588;

	if(SubQBuf_Safe[0] & 0x40)
	{
	 for(int i = 0; i < 588; i++)
	 {
	  ab->Samples[0][i] = 0;
	  ab->Samples[1][i] = 0;
	 }
	}
	else
	{
	 for(int i = 0; i < 588; i++)
	 {
	  ab->Samples[0][i] = (int16_t)de16lsb(&buf[i * sizeof(int16_t) * 2 + 0]);
	  ab->Samples[1][i] = (int16_t)de16lsb(&buf[i * sizeof(int16_t) * 2 + 2]);
	 }
	}

	ab->ReadPos = 0;
}

static void HandlePlayRead(void)
{
 uint8_t read_buf[2352 + 96];

 //

 if(CurSector >= ((int32_t)toc.tracks[100].lba + 300) && CurSector >= (75 * 60 * 75 - 150))
 {
  
  SeekFinished = -1;
  DriveStatus = DS_STOPPED;
  SectorPipe_Pos = SectorPipe_In = 0;
  SectorsRead = 0;
  return;
 }

 if(CurSector >= (int32_t)toc.tracks[100].lba)
 {
  
 }

 discReadRaw(Cur_CDIF, CurSector, read_buf);	// FIXME: error out on error.
 DecodeSubQ(read_buf + 2352);

 if(SubQBuf_Safe[1] == 0xAA && (DriveStatus == DS_PLAYING || (DriveStatus == DS_READING && !(SubQBuf_Safe[0] & 0x40) && (Mode & MODE_CDDA))))
 {
  HeaderBufValid = false;

  

  // Status in this end-of-disc context here should be generated after we're in the pause state.
  SeekTarget = CurSector;
  DriveStatus = DS_PAUSED;
  SectorPipe_Pos = SectorPipe_In = 0;
  SectorsRead = 0;
  SetAIP1(CDCIRQ_DATA_END, MakeStatus(false));

  return;
 }

 if(DriveStatus == DS_PLAYING)
 {
  if(Mode & MODE_AUTOPAUSE)
  {
   // Note: Some game(s) start playing in the pregap of a track(so don't replace this with a simple subq index == 0 check for autopause).
   // Pitball enables autopause a while after playing starts.
   //
   if(PlayTrackMatch == -1 && SubQChecksumOK)
    PlayTrackMatch = SubQBuf_Safe[0x1];

   if(PlayTrackMatch != -1 && SubQBuf_Safe[0x1] != PlayTrackMatch)
   {
    // Status needs to be taken before we're paused(IE it should still report playing).
    SetAIP1(CDCIRQ_DATA_END, MakeStatus(false));

    SeekTarget = CurSector;
    DriveStatus = DS_PAUSED;
    SectorPipe_Pos = SectorPipe_In = 0;
    SectorsRead = 0;
    PSRCounter = 0;
    return;
   }
  }

  if((Mode & MODE_REPORT) && ReportStartupDelay <= 0 && (((SubQBuf_Safe[0x9] >> 4) != ReportLastF) || Forward || Backward) && SubQChecksumOK)
  {
   uint8_t tr[8];
   uint16_t abs_lev_max = 0;
   bool abs_lev_chselect = SubQBuf_Safe[0x8] & 0x01;

   for(int i = 0; i < 588; i++)
    abs_lev_max = MAX(abs_lev_max, MIN(abs((int16_t)de16lsb(&read_buf[i * 4 + (abs_lev_chselect * 2)])), 32767));
   abs_lev_max |= abs_lev_chselect << 15;

   tr[0] = MakeStatus(false);
   tr[1] = SubQBuf_Safe[0x1];	// Track
   tr[2] = SubQBuf_Safe[0x2];	// Index

   if(SubQBuf_Safe[0x9] & 0x10)
   {
    tr[3] = SubQBuf_Safe[0x3];		// R M
    tr[4] = SubQBuf_Safe[0x4] | 0x80;	// R S
    tr[5] = SubQBuf_Safe[0x5];		// R F
   }
   else	
   {
    tr[3] = SubQBuf_Safe[0x7];	// A M
    tr[4] = SubQBuf_Safe[0x8];	// A S
    tr[5] = SubQBuf_Safe[0x9];	// A F
   }

   tr[6] = abs_lev_max >> 0;
   tr[7] = abs_lev_max >> 8;

   SetAIP_Buf(CDCIRQ_DATA_READY, 8, tr);
  }
  ReportLastF = SubQBuf_Safe[0x9] >> 4;
 }

 if(SectorPipe_In >= SectorPipe_Count)
 {
  uint8_t* buf = SectorPipe[SectorPipe_Pos];
  SectorPipe_In--;

  if(SubQBuf_Safe[0] & 0x40) // Data sector
  {
   if(DriveStatus == DS_SEEKING_LOGICAL2 || DriveStatus == DS_READING || (HoldLogicalPos && (DriveStatus == DS_PAUSED || DriveStatus == DS_STANDBY)))
   {
    memcpy(HeaderBuf, buf + 12, 12);
    HeaderBufValid = true;

    if(DriveStatus == DS_SEEKING_LOGICAL2)
    {
     if(AMSF_to_LBA(BCD_to_U8(HeaderBuf[0]), BCD_to_U8(HeaderBuf[1]), BCD_to_U8(HeaderBuf[2])) == SeekTarget)
     {
      //puts("Logical2 seek finished");
      DriveStatus = StatusAfterSeek;
      SeekFinished = true;
      ReportStartupDelay = 24000000;
     }
     else
     {
      if(!SeekRetryCounter)
      {
       HoldLogicalPos = false;
       DriveStatus = DS_STANDBY;
       SeekFinished = -1;
      }
      else
       SeekRetryCounter--;
     }
    }
   }
   //
   //
   //
   //printf("%d, %02x:%02x:%02x --- %d %d\n", DriveStatus, HeaderBuf[0], HeaderBuf[1], HeaderBuf[2], SeekTarget, AMSF_to_LBA(BCD_to_U8(HeaderBuf[0]), BCD_to_U8(HeaderBuf[1]), BCD_to_U8(HeaderBuf[2])));

   if(DriveStatus == DS_READING)
   {
    if((Mode & MODE_STRSND) && (buf[12 + 3] == 0x2) && ((buf[12 + 6] & 0x64) == 0x64))
    {
     if(XA_Test(buf))
     {
      if(AudioBuffer.ReadPos < AudioBuffer.Size)
      {
       
      }
      else
      {
       XA_ProcessSector(buf, &AudioBuffer);
      }
     }
    }
    else
    {
     // maybe if(!(Mode & 0x30)) too?


     int32_t offs = (Mode & 0x20) ? 0 : 12;
     int32_t size = (Mode & 0x20) ? 2340 : 2048;

     if(Mode & 0x10)
     {
      offs = 12;
      size = 2328;
     }

     memcpy(SB, buf + 12 + offs, size);
     SB_In = size;
     SetAIP1(CDCIRQ_DATA_READY, MakeStatus(false));
    }
   }
  }
  else // CD-DA sector
  {
   if(DriveStatus == DS_SEEKING_LOGICAL2)
   {
    if(Mode & MODE_CDDA)
    {
     DriveStatus = StatusAfterSeek;
     SeekFinished = true;
     ReportStartupDelay = 24000000;
    }
    else
    {
     if(!SeekRetryCounter)
     {
      HoldLogicalPos = false;
      DriveStatus = DS_STANDBY;
      SeekFinished = -1;
     }
     else
      SeekRetryCounter--;
    }
   }
   //
   //
   //
   if((DriveStatus == DS_READING && (Mode & MODE_CDDA)) || DriveStatus == DS_PLAYING)
   {
    if(AudioBuffer.ReadPos < AudioBuffer.Size)
    {
     
    }
    else
    {
     EnbufferizeCDDASector(buf);
    }
   }
  }
 }

 memcpy(SectorPipe[SectorPipe_Pos], read_buf, 2352);
 SectorPipe_Pos = (SectorPipe_Pos + 1) % SectorPipe_Count;
 SectorPipe_In++;

 PSRCounter += 33868800 / (75 * ((Mode & MODE_SPEED) ? 2 : 1));

 if(DriveStatus == DS_PLAYING)
 {
  // FIXME: What's the real fast-forward and backward speed?
  if(Forward)
   CurSector += 12;
  else if(Backward)
  {
   CurSector -= 12;

   if(CurSector < 0)	// FIXME: How does a real PS handle this condition?
    CurSector = 0;
  }
  else
   CurSector++;
 }
 else
  CurSector++;

 if(DriveStatus == DS_PAUSED || DriveStatus == DS_STANDBY) // || DriveStatus == DS_SEEKING_LOGICAL2)
 {
  if(CurSector >= (SeekTarget + (HoldLogicalPos ? 2 : 0)))
   CurSector = MAX(-150, CurSector - 9);
 }
 else
  SectorsRead++;
}

int64_t CDC_Update(const int64_t timestamp)
{
 int32_t clocks = timestamp - lastts;

 if(!Cur_CDIF)
 {
  if(DriveStatus != DS_STOPPED || (PendingCommandCounter > 0 && PendingCommandPhase >= 2))
  {
   SetAIP2(CDCIRQ_DISC_ERROR, (MakeStatus(false) & ~0xE0) | 0x04, 0x08);

   SectorPipe_Pos = SectorPipe_In = 0;
   SectorsRead = 0;
   HeaderBufValid = false;
   SeekFinished = -1;

   DriveStatus = DS_STOPPED;
   PSRCounter = 0;

   if(PendingCommandPhase >= 2)
   {
    PendingCommand = 0x00;
    PendingCommandCounter = 0;
    PendingCommandPhase = 0;
   }
  }

  DiscStartupDelay = 0;
 }
 //
 //
 //
 while(clocks > 0)
 {
  int32_t chunk_clocks = clocks;

  if(PSRCounter > 0 && chunk_clocks > PSRCounter)
   chunk_clocks = PSRCounter;

  if(PendingCommandCounter > 0 && chunk_clocks > PendingCommandCounter)
   chunk_clocks = PendingCommandCounter;

  if(chunk_clocks > SPUCounter)
   chunk_clocks = SPUCounter;

  if(DiscStartupDelay > 0)
  {
   if(chunk_clocks > DiscStartupDelay)
    chunk_clocks = DiscStartupDelay;

   DiscStartupDelay -= chunk_clocks;

   if(DiscStartupDelay <= 0)
   {
    SeekTarget = CurSector;
    HoldLogicalPos = false;
    DriveStatus = DS_PAUSED;	// or is it supposed to be DS_STANDBY?
   }
  }

  //

  if(!(IRQBuffer & 0xF))
  {
   if(CDCReadyReceiveCounter > 0 && chunk_clocks > CDCReadyReceiveCounter)
    chunk_clocks = CDCReadyReceiveCounter;

   if(CDCReadyReceiveCounter > 0)
    CDCReadyReceiveCounter -= chunk_clocks;
  }

  CheckAIP();

  if(PSRCounter > 0)
  {
   PSRCounter -= chunk_clocks;

   if(ReportStartupDelay > 0)
    ReportStartupDelay -= chunk_clocks;

   if(PSRCounter <= 0) 
   {
    if(DriveStatus == DS_SEEKING)
    {
     CurSector = SeekTarget;

     HoldLogicalPos = false;
     DriveStatus = StatusAfterSeek;
     SeekFinished = true;
     ReportStartupDelay = 24000000;

     if(DriveStatus == DS_PAUSED || DriveStatus == DS_STANDBY)
      CurSector = MAX(-150, CurSector - 9);

     PSRCounter = 33868800 / (75 * ((Mode & MODE_SPEED) ? 2 : 1));
    }
    else if(DriveStatus == DS_SEEKING_LOGICAL)
    {
     CurSector = SeekTarget;

     HoldLogicalPos = true;
     DriveStatus = DS_SEEKING_LOGICAL2;
    }
    //
    //
    //
    if(DriveStatus == DS_PAUSED || DriveStatus == DS_STANDBY || DriveStatus == DS_READING || DriveStatus == DS_PLAYING || DriveStatus == DS_SEEKING_LOGICAL2)
    {
     HandlePlayRead();
    }
   }
  }

  if(PendingCommandCounter > 0)
  {
   PendingCommandCounter -= chunk_clocks;

   if(PendingCommandCounter <= 0 && CDCReadyReceiveCounter > 0)
   {
    PendingCommandCounter = CDCReadyReceiveCounter; //256;
   }
   //else if(PendingCommandCounter <= 0 && PSRCounter > 0 && PSRCounter < 2000)
   //{
   // PendingCommandCounter = PSRCounter + 1;
   //}
   else if(PendingCommandCounter <= 0)
   {
    int32_t next_time = 0;

    if(PendingCommandPhase == -1)
    {
     if(ArgsRP != ArgsWP)
     {
      ArgsReceiveLatch = ArgsBuf[ArgsRP & 0x0F];
      ArgsRP = (ArgsRP + 1) & 0x1F;
      PendingCommandPhase += 1;
      next_time = 1815;
     }
     else
     {
      PendingCommandPhase += 2;
      next_time = 8500;
     }
    }
    else if(PendingCommandPhase == 0)	// Command phase 0
    {
     if(ArgsReceiveIn < 32)
      ArgsReceiveBuf[ArgsReceiveIn++] = ArgsReceiveLatch;

     if(ArgsRP != ArgsWP)
     {
      ArgsReceiveLatch = ArgsBuf[ArgsRP & 0x0F];
      ArgsRP = (ArgsRP + 1) & 0x1F;
      next_time = 1815;
     }
     else
     {
      PendingCommandPhase++;
      next_time = 8500;
     }
    }
    else if(PendingCommandPhase >= 2)	// Command phase 2+
    {
     BeginResults();

     const CDC_CTEntry *command = &Commands[PendingCommand];

     next_time = command->func2();
    }
    else	// Command phase 1
    {
     if(PendingCommand >= 0x20 || !Commands[PendingCommand].func)
     {
      BeginResults();

      

      WriteResult(MakeStatus(true));
      WriteResult(ERRCODE_BAD_COMMAND);
      WriteIRQ(CDCIRQ_DISC_ERROR);
     }
     else if(ArgsReceiveIn < Commands[PendingCommand].args_min || ArgsReceiveIn > Commands[PendingCommand].args_max)
     {
      BeginResults();

      
      for(unsigned int i = 0; i < ArgsReceiveIn; i++);
      WriteResult(MakeStatus(true));
      WriteResult(ERRCODE_BAD_NUMARGS);
      WriteIRQ(CDCIRQ_DISC_ERROR);
     }
     else
     {
      BeginResults();

      const CDC_CTEntry *command = &Commands[PendingCommand];

      
      for(unsigned int i = 0; i < ArgsReceiveIn; i++);
      next_time = command->func(ArgsReceiveIn, ArgsReceiveBuf);
      PendingCommandPhase = 2;
     }
     ArgsReceiveIn = 0;
    } // end command phase 1

    if(!next_time)
     PendingCommandCounter = 0;
    else
     PendingCommandCounter += next_time;
   }
  }

  SPUCounter = SPU_UpdateFromCDC(chunk_clocks);

  clocks -= chunk_clocks;
 } // end while(clocks > 0)

 lastts = timestamp;

 return(timestamp + CDC_CalcNextEvent());
}

void CDC_Write(const int64_t timestamp, uint32_t A, uint8_t V)
{
 A &= 0x3;

 //printf("Write: %08x %02x\n", A, V);

 if(A == 0x00)
 {
  RegSelector = V & 0x3;
 }
 else
 {
  const unsigned reg_index = ((RegSelector & 0x3) * 3) + (A - 1);

  CDC_Update(timestamp);
  //

  switch(reg_index)
  {
	default:
		
		break;

	 case 0x00:
		if(PendingCommandCounter > 0)
		{
		 
		}

		if(IRQBuffer & 0xF)
		{
		 
		}

		if(ResultsIn > 0)
		{
		 
		}

         	PendingCommandCounter = 10500 + PSX_GetRandU32(0, 3000) + 1815;
	 	PendingCommand = V;
         	PendingCommandPhase = -1;
		ArgsReceiveIn = 0;
		break;

	 case 0x01:
		ArgsBuf[ArgsWP & 0xF] = V;
		ArgsWP = (ArgsWP + 1) & 0x1F;
		
		if(!((ArgsWP - ArgsRP) & 0x0F))
		{
		 
		}
		break;

	 case 0x02:
 	 	if(V & 0x80)
	 	{
	  	 if(!DMABuffer.in_count)
	  	 {
		  if(!SB_In)
		  {
		   

		   fifoWrite(SB, 2340);

		   while((sizeof DMABuffer.data - DMABuffer.in_count))
		    fifoWriteByte(0x00);
		  }
		  else
		  {
	   	   fifoWrite(SB, SB_In);
		   SB_In = 0;
		  }
	  	 }
		 else
		 {
		  //
		 }
	 	}
	 	else if(V & 0x40)	// Something CD-DA related(along with & 0x20 ???)?
	 	{
		 for(unsigned i = 0; i < 4 && DMABuffer.in_count; i++)
		  fifoReadByte();
		}
		else
		{
		 fifoFlush();
		}

		if(V & 0x20)
		{
		 
		 IRQBuffer |= 0x10;
		 RecalcIRQ();
		}
		break;

	 case 0x04:
	 	IRQOutTestMask = V;
		RecalcIRQ();
		break;

	 case 0x05:
		if((IRQBuffer &~ V) != IRQBuffer && ResultsIn)
		{
		 // To debug icky race-condition related problems in "Psychic Detective", and to see if any games suffer from the same potential issue
		 // (to know what to test when we emulate CPU more accurately in regards to pipeline stalls and timing, which could throw off our kludge
		 //  for this issue)
		 
		}

	 	IRQBuffer &= ~V;
	 	RecalcIRQ();

		if(V & 0x80)	// Forced CD hardware reset of some kind(interface, controller, and drive?)  Seems to take a while(relatively speaking) to complete.
		{
		 
		 CDC_SoftReset();
		}

		if(V & 0x40)	// Does it clear more than arguments buffer?  Doesn't appear to clear results buffer.
		{
		 ArgsWP = ArgsRP = 0;
		}
		break;

	 case 0x07:
		Pending_DecodeVolume[0][0] = V;
		break;

	 case 0x08:
		Pending_DecodeVolume[0][1] = V;
		break;

	 case 0x09:
		Pending_DecodeVolume[1][1] = V;
		break;

	 case 0x0A:
		Pending_DecodeVolume[1][0] = V;
		break;

	 case 0x0B:
		if(V & 0x20)
		{
		 memcpy(DecodeVolume, Pending_DecodeVolume, sizeof(DecodeVolume));

		 for(int i = 0; i < 2; i++)
		 {
		  for(int o = 0; o < 2; o++)
		  {
		   //fprintf(stderr, "Input Channel %d, Output Channel %d -- Volume=%d\n", i, o, DecodeVolume[i][o]);
		  }
		 }
		}
		break;
  }
  cdcSchedule(timestamp + CDC_CalcNextEvent());
 }
}

uint8_t CDC_Read(const int64_t timestamp, uint32_t A)
{
 uint8_t ret = 0;

 A &= 0x03;

 //printf("Read %08x\n", A);

 if(A == 0x00)
 {
	ret = RegSelector & 0x3;

	if(ArgsWP == ArgsRP)
	 ret |= 0x08;	// Args FIFO empty.

	if(!((ArgsWP - ArgsRP) & 0x10))
	 ret |= 0x10;	// Args FIFO has room.

	if(ResultsIn)
	 ret |= 0x20;

	if(DMABuffer.in_count)
	 ret |= 0x40;

        if(PendingCommandCounter > 0 && PendingCommandPhase <= 1)
	 ret |= 0x80;
 }
 else
 {
  switch(A & 0x3)
  {
   case 0x01:
	ret = ReadResult();
	break;

   case 0x02:
	//
	if(DMABuffer.in_count)
	 ret = fifoReadByte();
	else
	{
	 
	}
	break;

   case 0x03:
	if(RegSelector & 0x1)
	{
	 ret = 0xE0 | IRQBuffer;
	}
	else
	{
	 ret = 0xFF;
	}
	break;
  }
 }

 return(ret);
}


bool CDC_DMACanRead(void)
{
 return(DMABuffer.in_count);
}

uint32_t CDC_DMARead(void)
{
 uint32_t data = 0;

 for(int i = 0; i < 4; i++)
 {
  if(DMABuffer.in_count)
   data |= fifoReadByte() << (i * 8);
  else
  {
   
  }
 }

 return(data);
}

static bool CommandCheckDiscPresent(void)
{
 if(!Cur_CDIF || DiscStartupDelay > 0)
 {
  WriteResult(MakeStatus(true));
  WriteResult(ERRCODE_NOT_READY);

  WriteIRQ(CDCIRQ_DISC_ERROR);

  return(false);
 }

 return(true);
}

static int32_t Command_Nop(const int arg_count, const uint8_t *args)
{
 WriteResult(MakeStatus(false));
 WriteIRQ(CDCIRQ_ACKNOWLEDGE);

 return(0);
}

static int32_t Command_Setloc(const int arg_count, const uint8_t *args)
{
 uint8_t m, s, f;

 if((args[0] & 0x0F) > 0x09 || args[0] > 0x99 ||
    (args[1] & 0x0F) > 0x09 || args[1] > 0x59 ||
    (args[2] & 0x0F) > 0x09 || args[2] > 0x74)
 {
  WriteResult(MakeStatus(true));
  WriteResult(ERRCODE_BAD_ARGVAL);
  WriteIRQ(CDCIRQ_DISC_ERROR);
  return(0);
 }

 m = BCD_to_U8(args[0]);
 s = BCD_to_U8(args[1]);
 f = BCD_to_U8(args[2]);

 CommandLoc = f + 75 * s + 75 * 60 * m - 150;
 CommandLoc_Dirty = true;

 WriteResult(MakeStatus(false));
 WriteIRQ(CDCIRQ_ACKNOWLEDGE);

 return(0);
}

static int32_t CalcSeekTime(int32_t initial, int32_t target, bool motor_on, bool paused)
{
 int32_t ret = 0;

 if(!motor_on)
 {
  initial = 0;
  ret += 33868800;
 }

 const int32_t abs_diff = abs(initial - target);

 ret += MAX((int64_t)abs_diff * 33868800 * 1000 / (72 * 60 * 75) / 1000, 20000);

 if(abs_diff >= 2250)
  ret += (int64_t)33868800 * 300 / 1000;
 else if(paused)
 {
  // The delay to restart from a Pause state is...very....WEIRD.  The time it takes is related to the amount of time that has passed since the pause, and
  // where on the disc the laser head is, with generally more time passed = longer to resume, except that there's a window of time where it takes a
  // ridiculous amount of time when not much time has passed.
  // 
  // What we have here will be EXTREMELY simplified.

  //
  //

  //if(time_passed >= 67737)
  //{
  //}
  //else
  {
   // Take twice as long for 1x mode.
   ret += 1237952 * ((Mode & MODE_SPEED) ? 1 : 2);
  }
 }
 else if(abs_diff >= 3 && abs_diff < 12)
  ret += 33868800 / (75 * ((Mode & MODE_SPEED) ? 2 : 1)) * 4;

 //else if(target < initial)
 // ret += 1000000;

 ret += PSX_GetRandU32(0, 25000);

 

 return(ret);
}

#if 0
static void BeginSeek(uint32_t target, int after_seek)
{
 SeekTarget = target;
 StatusAfterSeek = after_seek;

 PSRCounter = CalcSeekTime(CurSector, SeekTarget, DriveStatus != DS_STOPPED, DriveStatus == DS_PAUSED);
}
#endif

// Remove this function when we have better seek emulation; it's here because the Rockman complete works games(at least 2 and 4) apparently have finicky fubared CD
// access code.
static void PreSeekHack(int32_t target)
{
 uint8_t pwbuf[96];
 int max_try = 32;

 CurSector = target;	// If removing/changing this, take into account how it will affect ReadN/ReadS/Play/etc command calls that interrupt a seek.
 SeekRetryCounter = 128;

 // If removing this SubQ reading bit, think about how it will interact with a Read command of data(or audio :b) sectors when Mode bit0 is 1.
 do
 {
  discReadPw(Cur_CDIF, target++, pwbuf);
 } while(!DecodeSubQ(pwbuf) && --max_try > 0);
}

/*
 Play command with a track argument that's not a valid BCD quantity causes interesting half-buggy behavior on an actual PS1(unlike some of the other commands,
 an error doesn't seem to be generated for a bad BCD argument).
*/
static int32_t Command_Play(const int arg_count, const uint8_t *args)
{
 if(!CommandCheckDiscPresent())
  return(0);

 ClearAIP();

 WriteResult(MakeStatus(false));
 WriteIRQ(CDCIRQ_ACKNOWLEDGE);

 Forward = Backward = false;

 if(arg_count && args[0])
 {
  int track = BCD_to_U8(args[0]);

  if(track < toc.first_track)
  {
   
   track = toc.first_track;
  }
  else if(track > toc.last_track)
  {
   
   track = toc.last_track;
  }

  ClearAudioBuffers();
  SectorPipe_Pos = SectorPipe_In = 0;
  SectorsRead = 0;

  PlayTrackMatch = track;

  

  SeekTarget = toc.tracks[track].lba;
  PSRCounter = CalcSeekTime(CurSector, SeekTarget, DriveStatus != DS_STOPPED, DriveStatus == DS_PAUSED);
  HeaderBufValid = false;
  PreSeekHack(SeekTarget);

  SeekFinished = false;
  DriveStatus = DS_SEEKING;
  StatusAfterSeek = DS_PLAYING;
 }
 else if(CommandLoc_Dirty || DriveStatus != DS_PLAYING)
 {
  ClearAudioBuffers();
  SectorPipe_Pos = SectorPipe_In = 0;
  SectorsRead = 0;

  if(CommandLoc_Dirty)
   SeekTarget = CommandLoc;
  else
   SeekTarget = CurSector;

  PlayTrackMatch = -1;

  PSRCounter = CalcSeekTime(CurSector, SeekTarget, DriveStatus != DS_STOPPED, DriveStatus == DS_PAUSED);
  HeaderBufValid = false;
  PreSeekHack(SeekTarget);

  SeekFinished = false;
  DriveStatus = DS_SEEKING;
  StatusAfterSeek = DS_PLAYING;
 }

 CommandLoc_Dirty = false;
 return(0);
}

static int32_t Command_Forward(const int arg_count, const uint8_t *args)
{
 if(!CommandCheckDiscPresent())
  return(0);

 WriteResult(MakeStatus(false));
 WriteIRQ(CDCIRQ_ACKNOWLEDGE);

 Backward = false;
 Forward = true;

 return(0);
}

static int32_t Command_Backward(const int arg_count, const uint8_t *args)
{
 if(!CommandCheckDiscPresent())
  return(0);

 WriteResult(MakeStatus(false));
 WriteIRQ(CDCIRQ_ACKNOWLEDGE);

 Backward = true;
 Forward = false;

 return(0);
}


static void ReadBase(void)
{
 if(!CommandCheckDiscPresent())
  return;

 if(!IsPSXDisc)
 {
  WriteResult(MakeStatus(true));
  WriteResult(ERRCODE_BAD_COMMAND);

  WriteIRQ(CDCIRQ_DISC_ERROR);
  return;
 }

 WriteResult(MakeStatus(false));
 WriteIRQ(CDCIRQ_ACKNOWLEDGE);

 if((DriveStatus == DS_SEEKING_LOGICAL || DriveStatus == DS_SEEKING_LOGICAL2) && SeekTarget == CommandLoc && StatusAfterSeek == DS_READING)
 {
  CommandLoc_Dirty = false;
  return;
 }

 if(CommandLoc_Dirty || DriveStatus != DS_READING)
 {
  // Don't flush the DMABuffer here; see CTR course selection screen.
  ClearAIP();
  ClearAudioBuffers();
  SB_In = 0;
  SectorPipe_Pos = SectorPipe_In = 0;
  SectorsRead = 0;

  // TODO: separate motor start from seek phase?

  if(CommandLoc_Dirty)
   SeekTarget = CommandLoc;
  else if(DriveStatus != DS_PAUSED && DriveStatus != DS_STANDBY)
   SeekTarget = CurSector;

  PSRCounter = 33868800 / (75 * ((Mode & MODE_SPEED) ? 2 : 1)) + CalcSeekTime(CurSector, SeekTarget, DriveStatus != DS_STOPPED, DriveStatus == DS_PAUSED);
  HeaderBufValid = false;
  PreSeekHack(SeekTarget);

  SeekFinished = false;
  DriveStatus = DS_SEEKING_LOGICAL;
  StatusAfterSeek = DS_READING;
 }

 CommandLoc_Dirty = false;
}

static int32_t Command_ReadN(const int arg_count, const uint8_t *args)
{
 ReadBase();
 return 0;
}

static int32_t Command_ReadS(const int arg_count, const uint8_t *args)
{
 ReadBase();
 return 0;
}

static int32_t Command_Stop(const int arg_count, const uint8_t *args)
{
 if(!CommandCheckDiscPresent())
  return(0);

 WriteResult(MakeStatus(false));
 WriteIRQ(CDCIRQ_ACKNOWLEDGE);

 if(DriveStatus == DS_STOPPED)
 {
  return(5000);
 }
 else
 {
  ClearAudioBuffers();
  ClearAIP();
  SectorPipe_Pos = SectorPipe_In = 0;
  SectorsRead = 0;

  DriveStatus = DS_STOPPED;
  HeaderBufValid = false;

  return(33868);	// FIXME, should be much higher.
 }
}

static int32_t Command_Stop_Part2(void)
{
 PSRCounter = 0;

 WriteResult(MakeStatus(false));
 WriteIRQ(CDCIRQ_COMPLETE);

 return(0);
}

static int32_t Command_Standby(const int arg_count, const uint8_t *args)
{
 if(!CommandCheckDiscPresent())
  return(0);

 if(DriveStatus != DS_STOPPED)
 {
  WriteResult(MakeStatus(true));
  WriteResult(0x20);
  WriteIRQ(CDCIRQ_DISC_ERROR);
  return(0);
 }

 WriteResult(MakeStatus(false));
 WriteIRQ(CDCIRQ_ACKNOWLEDGE);

 ClearAudioBuffers();
 ClearAIP();
 SectorPipe_Pos = SectorPipe_In = 0;
 SectorsRead = 0;

 SeekTarget = CurSector;	// FIXME?  CurSector = 0?
 HoldLogicalPos = false;
 DriveStatus = DS_STANDBY;

 return((int64_t)33868800 * 100 / 1000);	// No idea, FIXME.
}

static int32_t Command_Standby_Part2(void)
{
 PSRCounter = 0;

 WriteResult(MakeStatus(false));
 WriteIRQ(CDCIRQ_COMPLETE);

 return(0);
}

static int32_t Command_Pause(const int arg_count, const uint8_t *args)
{
 if(!CommandCheckDiscPresent())
  return 0;

 WriteResult(MakeStatus(false));
 WriteIRQ(CDCIRQ_ACKNOWLEDGE);

 if(DriveStatus == DS_PAUSED || DriveStatus == DS_STOPPED)
 {
  return 5000;
 }
 else
 {
  CurSector -= MIN(4, SectorsRead);	// See: Bedlam, Rise 2
  SectorsRead = 0;

  // "Viewpoint" flips out and crashes if reading isn't stopped (almost?) immediately.
  //ClearAudioBuffers();
  SectorPipe_Pos = SectorPipe_In = 0;
  ClearAIP();
  SeekTarget = CurSector;
  DriveStatus = DS_PAUSED;
  PSRCounter = 33868800 / (75 * ((Mode & MODE_SPEED) ? 2 : 1));

  // An approximation.
  return (1124584 + ((int64_t)CurSector * 42596 / (75 * 60))) * ((Mode & MODE_SPEED) ? 1 : 2) + PSX_GetRandU32(0, 100000);
 }
}

static int32_t Command_Pause_Part2(void)
{
 WriteResult(MakeStatus(false));
 WriteIRQ(CDCIRQ_COMPLETE);

 return 0;
}

static int32_t Command_Reset(const int arg_count, const uint8_t *args)
{
 WriteResult(MakeStatus(false));
 WriteIRQ(CDCIRQ_ACKNOWLEDGE);

 Muted = false; // Does it get reset here?
 Mode = 0x20;	// Confirmed(and see "This Is Football 2").
 CommandLoc = 0;
 //
 //
 //
// printf("DriveStatus: %d %d %d\n", DriveStatus, StatusAfterSeek, SeekTarget);
 if(Cur_CDIF && DiscStartupDelay <= 0)
 {
  if((DriveStatus == DS_SEEKING_LOGICAL || DriveStatus == DS_SEEKING) && StatusAfterSeek == DS_PAUSED && SeekTarget == 0)
  {
   //puts("HRMPH");
   return 256;
  }
  else
  {
   ClearAudioBuffers();

   SB_In = 0;
   SectorPipe_Pos = SectorPipe_In = 0;
   SectorsRead = 0;

   SeekTarget = 0;
   PSRCounter = MAX(PSX_GetRandU32(0, 3250000), CalcSeekTime(CurSector, SeekTarget, DriveStatus != DS_STOPPED, DriveStatus == DS_PAUSED));
   PreSeekHack(SeekTarget);
   SeekFinished = false;
   DriveStatus = HoldLogicalPos ? DS_SEEKING_LOGICAL : DS_SEEKING;
   StatusAfterSeek = DS_PAUSED;
   ClearAIP();

   return 4100000;
  }
 }
 else
 {
  SeekFinished = true;
  return 70000;
 }
}

static int32_t Command_Mute(const int arg_count, const uint8_t *args)
{
 Muted = true;

 WriteResult(MakeStatus(false));
 WriteIRQ(CDCIRQ_ACKNOWLEDGE);

 return(0);
}

static int32_t Command_Demute(const int arg_count, const uint8_t *args)
{
 Muted = false;

 WriteResult(MakeStatus(false));
 WriteIRQ(CDCIRQ_ACKNOWLEDGE);

 return(0);
}

static int32_t Command_Setfilter(const int arg_count, const uint8_t *args)
{
 FilterFile = args[0];
 FilterChan = args[1];

 WriteResult(MakeStatus(false));
 WriteIRQ(CDCIRQ_ACKNOWLEDGE);

 return(0);
}

static int32_t Command_Setmode(const int arg_count, const uint8_t *args)
{
 Mode = args[0];

 WriteResult(MakeStatus(false));
 WriteIRQ(CDCIRQ_ACKNOWLEDGE);

 return(0);
}

static int32_t Command_Getparam(const int arg_count, const uint8_t *args)
{
 WriteResult(MakeStatus(false));
 WriteResult(Mode);
 WriteResult(0x00);
 WriteResult(FilterFile);
 WriteResult(FilterChan);

 WriteIRQ(CDCIRQ_ACKNOWLEDGE);


 return(0);
}

static int32_t Command_GetlocL(const int arg_count, const uint8_t *args)
{
 if(!CommandCheckDiscPresent())
  return(0);

 if(!HeaderBufValid)
 {
  WriteResult(MakeStatus(true));
  WriteResult(0x80);
  WriteIRQ(CDCIRQ_DISC_ERROR);
  return(0);
 }

 for(unsigned i = 0; i < 8; i++)
 {
  //printf("%d %d: %02x\n", DriveStatus, i, HeaderBuf[i]);
  WriteResult(HeaderBuf[i]);
 }

 WriteIRQ(CDCIRQ_ACKNOWLEDGE);

 return(0);
}

static int32_t Command_GetlocP(const int arg_count, const uint8_t *args)
{
 if(!CommandCheckDiscPresent())
  return(0);

 //printf("%2x:%2x %2x:%2x:%2x %2x:%2x:%2x\n", SubQBuf_Safe[0x1], SubQBuf_Safe[0x2], SubQBuf_Safe[0x3], SubQBuf_Safe[0x4], SubQBuf_Safe[0x5], SubQBuf_Safe[0x7], SubQBuf_Safe[0x8], SubQBuf_Safe[0x9]);

 WriteResult(SubQBuf_Safe[0x1]);	// Track
 WriteResult(SubQBuf_Safe[0x2]);	// Index
 WriteResult(SubQBuf_Safe[0x3]);	// R M
 WriteResult(SubQBuf_Safe[0x4]);	// R S
 WriteResult(SubQBuf_Safe[0x5]);	// R F
 WriteResult(SubQBuf_Safe[0x7]);	// A M
 WriteResult(SubQBuf_Safe[0x8]);	// A S
 WriteResult(SubQBuf_Safe[0x9]);	// A F

 WriteIRQ(CDCIRQ_ACKNOWLEDGE);

 return(0);
}

static int32_t Command_ReadT(const int arg_count, const uint8_t *args)
{
 WriteResult(MakeStatus(false));
 WriteIRQ(CDCIRQ_ACKNOWLEDGE);

 return(44100 * 768 / 1000);
}

static int32_t Command_ReadT_Part2(void)
{
 WriteResult(MakeStatus(false));
 WriteIRQ(CDCIRQ_COMPLETE);

 return(0);
}

static int32_t Command_GetTN(const int arg_count, const uint8_t *args)
{
 if(!CommandCheckDiscPresent())
  return(0);

 WriteResult(MakeStatus(false));
 WriteResult(U8_to_BCD(toc.first_track));
 WriteResult(U8_to_BCD(toc.last_track));

 WriteIRQ(CDCIRQ_ACKNOWLEDGE);

 return(0);
}

static int32_t Command_GetTD(const int arg_count, const uint8_t *args)
{
 if(!CommandCheckDiscPresent())
  return(0);

 int track;
 uint8_t m, s, f;

 if(!args[0])
  track = 100;
 else
 {
  track = BCD_to_U8(args[0]);

  if(!BCD_is_valid(args[0]) || track < toc.first_track || track > toc.last_track)	// Error
  {
   WriteResult(MakeStatus(true));
   WriteResult(ERRCODE_BAD_ARGVAL);
   WriteIRQ(CDCIRQ_DISC_ERROR);
   return(0);
  }
 }

 LBA_to_AMSF(toc.tracks[track].lba, &m, &s, &f);

 WriteResult(MakeStatus(false));
 WriteResult(U8_to_BCD(m));
 WriteResult(U8_to_BCD(s));
 //WriteResult(U8_to_BCD(f));

 WriteIRQ(CDCIRQ_ACKNOWLEDGE);

 return(0);
}

static int32_t Command_SeekL(const int arg_count, const uint8_t *args)
{
 if(!CommandCheckDiscPresent())
  return(0);

 WriteResult(MakeStatus(false));
 WriteIRQ(CDCIRQ_ACKNOWLEDGE);

 SeekTarget = CommandLoc;

#if 1
 SectorsRead = 0;
 SectorPipe_Pos = SectorPipe_In = 0;
#endif
 PSRCounter = CalcSeekTime(CurSector, SeekTarget, DriveStatus != DS_STOPPED, DriveStatus == DS_PAUSED);
 //HeaderBufValid = false;
 PreSeekHack(SeekTarget);
 SeekFinished = false;
 DriveStatus = DS_SEEKING_LOGICAL;
 StatusAfterSeek = DS_STANDBY;
 ClearAIP();

 return(PSRCounter);
}

static int32_t Command_SeekP(const int arg_count, const uint8_t *args)
{
 if(!CommandCheckDiscPresent())
  return(0);

 WriteResult(MakeStatus(false));
 WriteIRQ(CDCIRQ_ACKNOWLEDGE);

 SeekTarget = CommandLoc;

 PSRCounter = CalcSeekTime(CurSector, SeekTarget, DriveStatus != DS_STOPPED, DriveStatus == DS_PAUSED);
 HeaderBufValid = false;
 PreSeekHack(SeekTarget);
 SeekFinished = false;
 DriveStatus = DS_SEEKING;
 StatusAfterSeek = DS_STANDBY;
 ClearAIP();

 return(PSRCounter);
}

// Used with Command_Reset(), too.
static int32_t Command_Seek_PartN(void)
{
 if(DriveStatus != DS_SEEKING && DriveStatus != DS_SEEKING_LOGICAL && DriveStatus != DS_SEEKING_LOGICAL2)
 {
  if(SeekFinished)
  {
   BeginResults();

   if(SeekFinished < 0)
   {
    WriteResult(MakeStatus(false) | 0x04);
    WriteResult(0x04);
    WriteIRQ(CDCIRQ_DISC_ERROR);
   }
   else
   {
    WriteResult(MakeStatus(false));
    WriteIRQ(CDCIRQ_COMPLETE);
   }
  }
  else
  {
   
  }

  return 0;
 }
 else
 {
  return MAX(PSRCounter, 256);
 }
}

static int32_t Command_Test(const int arg_count, const uint8_t *args)
{
 switch(args[0])
 {
  default:
	
	WriteResult(MakeStatus(true));
	WriteResult(0x10);
	WriteIRQ(CDCIRQ_DISC_ERROR);
	break;

  case 0x00:
  case 0x01:
  case 0x02:
  case 0x03:
  case 0x10:
  case 0x11:
  case 0x12:
  case 0x13:
  case 0x14:
  case 0x15:
  case 0x16:
  case 0x17:
  case 0x18:
  case 0x19:
  case 0x1A:
	
	WriteResult(MakeStatus(false));
	WriteIRQ(CDCIRQ_ACKNOWLEDGE);
  	break;

#if 0
  case 0x50:	// *Need to retest this test command, it takes additional arguments??? Or in any case, it generates a different error code(0x20) than most other Test
		// sub-commands that generate an error code(0x10).
	break;

 // Same with 0x60, 0x71-0x76

#endif

  case 0x51:	// *Need to retest this test command
	
	WriteResult(0x01);
	WriteResult(0x00);
	WriteResult(0x00);
	break;

  case 0x75:	// *Need to retest this test command
	
	WriteResult(0x00);
	WriteResult(0xC0);
	WriteResult(0x00);
	WriteResult(0x00);
	break;

  // 
  // SCEx counters not reset by command 0x0A.
  //

  case 0x04:	// Reset SCEx counters
	WriteResult(MakeStatus(false));
	WriteIRQ(CDCIRQ_ACKNOWLEDGE);
  	break;

  case 0x05:	// Read SCEx counters
	WriteResult(0x00);	// Number of TOC/leadin reads? (apparently increases by 1 or 2 per ReadTOC, even on non-PSX music CD)
	WriteResult(0x00);	// Number of SCEx strings received? (Stays at zero on music CD)
	WriteIRQ(CDCIRQ_ACKNOWLEDGE);
  	break;

  case 0x20:
	{
	 WriteResult(0x97);
	 WriteResult(0x01);
	 WriteResult(0x10);
	 WriteResult(0xC2);

	 WriteIRQ(CDCIRQ_ACKNOWLEDGE);
	}
	break;

  case 0x21:	// *Need to retest this test command.
	{
	 WriteResult(0x01);
	 WriteIRQ(CDCIRQ_ACKNOWLEDGE);
	}
	break;

  case 0x22:
	{
	 static const uint8_t td[7] = { 0x66, 0x6f, 0x72, 0x20, 0x55, 0x2f, 0x43 };

	 for(unsigned i = 0; i < 7; i++)
	  WriteResult(td[i]);

	 WriteIRQ(CDCIRQ_ACKNOWLEDGE);
	}
	break;

  case 0x23:
  case 0x24:
	{
	 static const uint8_t td[8] = { 0x43, 0x58, 0x44, 0x32, 0x35, 0x34, 0x35, 0x51 };

	 for(unsigned i = 0; i < 8; i++)
	  WriteResult(td[i]);

	 WriteIRQ(CDCIRQ_ACKNOWLEDGE);
	}
	break;

  case 0x25:
	{
	 static const uint8_t td[8] = { 0x43, 0x58, 0x44, 0x31, 0x38, 0x31, 0x35, 0x51 };

	 for(unsigned i = 0; i < 8; i++)
	  WriteResult(td[i]);

	 WriteIRQ(CDCIRQ_ACKNOWLEDGE);
	}
	break;
 }
 return(0);
}

static int32_t Command_ID(const int arg_count, const uint8_t *args)
{
 if(!CommandCheckDiscPresent())
  return(0);

 WriteResult(MakeStatus(false));
 WriteIRQ(CDCIRQ_ACKNOWLEDGE);

 return(33868);
}

static int32_t Command_ID_Part2(void)
{
 if(IsPSXDisc)
 {
  WriteResult(MakeStatus(false));
  WriteResult(0x00);
  WriteResult(0x20);
  WriteResult(0x00);
 }
 else
 {
  WriteResult(MakeStatus(false) | 0x08);
  WriteResult(0x90);
  WriteResult(toc.disc_type);
  WriteResult(0x00);
 }

 if(IsPSXDisc)
 {
  WriteResult(DiscID[0]);
  WriteResult(DiscID[1]);
  WriteResult(DiscID[2]);
  WriteResult(DiscID[3]);
 }
 else
 {
  WriteResult(0xff);
  WriteResult(0);
  WriteResult(0);
  WriteResult(0);
 }

 if(IsPSXDisc)
  WriteIRQ(CDCIRQ_COMPLETE);
 else
  WriteIRQ(CDCIRQ_DISC_ERROR);

 return(0);
}

static int32_t Command_Init(const int arg_count, const uint8_t *args)
{
 return(0);
}

static int32_t Command_ReadTOC(const int arg_count, const uint8_t *args)
{
 int32_t ret_time;

 HeaderBufValid = false;
 WriteResult(MakeStatus(false));
 WriteIRQ(CDCIRQ_ACKNOWLEDGE);

 // ReadTOC doesn't error out if the tray is open, and it completes rather quickly in that case.
 //
 if(!CommandCheckDiscPresent())
  return(26000);
 


 // A gross approximation.  
 // The penalty for the drive being stopped seems to be rather high(higher than what CalcSeekTime() currently introduces), although
 // that should be investigated further.
 //
 // ...and not to mention the time taken varies from disc to disc even!
 ret_time = 30000000 + CalcSeekTime(CurSector, 0, DriveStatus != DS_STOPPED, DriveStatus == DS_PAUSED);

 SeekTarget = 0;
 HoldLogicalPos = false;
 DriveStatus = DS_PAUSED;	// Ends up in a pause state when the command is finished.  Maybe we should add DS_READTOC or something...
 ClearAIP();

 return ret_time;
}

static int32_t Command_ReadTOC_Part2(void)
{
 //if(!CommandCheckDiscPresent())
 // DriveStatus = DS_PAUSED;

 WriteResult(MakeStatus(false));
 WriteIRQ(CDCIRQ_COMPLETE);

 return(0);
}

static int32_t Command_0x1d(const int arg_count, const uint8_t *args)
{
 WriteResult(MakeStatus(false));
 WriteIRQ(CDCIRQ_ACKNOWLEDGE);
 return(0);
}

static const CDC_CTEntry Commands[0x20] =
{
 { /* 0x00, */ 0, 0, NULL, NULL, NULL },
 { /* 0x01, */ 0, 0, "Nop", Command_Nop, NULL },
 { /* 0x02, */ 3, 3, "Setloc", Command_Setloc, NULL },
 { /* 0x03, */ 0, 1, "Play", Command_Play, NULL },
 { /* 0x04, */ 0, 0, "Forward", Command_Forward, NULL },
 { /* 0x05, */ 0, 0, "Backward", Command_Backward, NULL },
 { /* 0x06, */ 0, 0, "ReadN", Command_ReadN, NULL },
 { /* 0x07, */ 0, 0, "Standby", Command_Standby, Command_Standby_Part2 },
 { /* 0x08, */ 0, 0, "Stop", Command_Stop, Command_Stop_Part2 },
 { /* 0x09, */ 0, 0, "Pause", Command_Pause, Command_Pause_Part2 },
 { /* 0x0A, */ 0, 0, "Reset", Command_Reset, Command_Seek_PartN },
 { /* 0x0B, */ 0, 0, "Mute", Command_Mute, NULL },
 { /* 0x0C, */ 0, 0, "Demute", Command_Demute, NULL },
 { /* 0x0D, */ 2, 2, "Setfilter", Command_Setfilter, NULL },
 { /* 0x0E, */ 1, 1, "Setmode", Command_Setmode, NULL },
 { /* 0x0F, */ 0, 0, "Getparam", Command_Getparam, NULL },
 { /* 0x10, */ 0, 0, "GetlocL", Command_GetlocL, NULL },
 { /* 0x11, */ 0, 0, "GetlocP", Command_GetlocP, NULL },
 { /* 0x12, */ 1, 1, "ReadT", Command_ReadT, Command_ReadT_Part2 },
 { /* 0x13, */ 0, 0, "GetTN", Command_GetTN, NULL },
 { /* 0x14, */ 1, 1, "GetTD", Command_GetTD, NULL },
 { /* 0x15, */ 0, 0, "SeekL", Command_SeekL, Command_Seek_PartN },
 { /* 0x16, */ 0, 0, "SeekP", Command_SeekP, Command_Seek_PartN },

 { /* 0x17, */ 0, 0, NULL, NULL, NULL },
 { /* 0x18, */ 0, 0, NULL, NULL, NULL },

 { /* 0x19, */ 1, 1/* ??? */, "Test", Command_Test, NULL },
 { /* 0x1A, */ 0, 0, "ID", Command_ID, Command_ID_Part2 },
 { /* 0x1B, */ 0, 0, "ReadS", Command_ReadS, NULL },
 { /* 0x1C, */ 0, 0, "Init", Command_Init, NULL },
 { /* 0x1D, */ 2, 2, "Unknown 0x1D", Command_0x1d, NULL },
 { /* 0x1E, */ 0, 0, "ReadTOC", Command_ReadTOC, Command_ReadTOC_Part2 },
 { /* 0x1F, */ 0, 0, NULL, NULL, NULL },
};



// armPSX's side

void cdromReset(struct Psx* p) {
 P = p;
 PSX_ResetRand();
 CDC_Init();
 CDC_SetDisc(p->disc == NULL, p->disc, "SCEA");
 CDC_Power();
 lastts = p->cycles;
}

void cdromRun(struct Psx* p) {
 psxSchedule(p, CDC_Update(p->cycles));
}

uint8_t cdromRead(struct Psx* p, uint32_t address) {
 return CDC_Read(p->cycles, address & 3);
}

void cdromWrite(struct Psx* p, uint32_t address, uint8_t v) {
 CDC_Write(p->cycles, address & 3, v);
}

uint32_t cdromDmaRead(struct Psx* p) {
 return CDC_DMARead();
}

bool cdromDmaCanRead(struct Psx* p) {
 return CDC_DMACanRead();
}
