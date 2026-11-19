/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DISC_H
#define DISC_H

#include "cdutil.h"

struct Disc;

// A .cue/.bin, .chd or .m3u (its first disc); NULL when it can't be read.
struct Disc* discOpen(const char* path);
void discClose(struct Disc* d);
int discCount(const char* path);
struct Disc* discOpenIndex(const char* path, int index);
void discToc(struct Disc* d, struct Toc* toc);
// 2352 bytes of sector and 96 of interleaved subchannel P-W.
void discReadRaw(struct Disc* d, int32_t lba, uint8_t* buf);
void discReadPw(struct Disc* d, int32_t lba, uint8_t* pw);

#endif
