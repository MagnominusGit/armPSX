/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The GL renderer: the GPU's drawing done with OpenGL ES 3.0 into a VRAM image scale times VRAM's
 * size, for the picture. VRAM for the emulation (reads, the CPU's view) stays the software
 * renderer's, so the emulation runs the same with either. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct Gles;
struct Glr;

// On gl's context, current while the renderer is used. NULL, with why in error, on failure.
struct Glr* glrCreate(struct Gles* gl, int scale, char* error, size_t errorSize);
void glrDestroy(struct Glr*);

// What the primitives are drawn with: the GPU's settings when each is drawn.
struct GlrState {
	int32_t clipX0, clipY0, clipX1, clipY1;
	uint32_t texPageX, texPageY, texMode;  // texMode 0-2 (4-bit, 8-bit, 15-bit)
	uint32_t tww, twh, twx, twy;
	bool maskSet, maskEval, dither;
	int blend;  // -1 opaque, 0-3 semi-transparent
	uint32_t clut;  // the command's CLUT field
	int skip;  // lines not drawn (480i, the field shown): -1 none, 0 even, 1 odd
};

// An interpolated value (texture u, v; red, green, blue) as the GPU steps it: at VRAM pixel (x, y) it
// is base + dx * x + dy * y (modulo 2^32), the value in the top 8 bits.
struct GlrPlane {
	uint32_t base, dx, dy;
};

struct GlrVertex {
	int32_t x, y;
	uint32_t r, g, b;
};

// planes: u, v, red, green, blue.
void glrTriangle(struct Glr*, const struct GlrState*, const int32_t xy[3][2], const struct GlrPlane planes[5], bool textured, bool raw,
                 bool dither);
void glrSprite(struct Glr*, const struct GlrState*, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t u, uint32_t v,
               uint32_t color, bool flipX, bool flipY, bool textured, bool raw);
void glrLine(struct Glr*, const struct GlrState*, const struct GlrVertex v[2], bool dither);
void glrFill(struct Glr*, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color, int skip);
void glrCopy(struct Glr*, int32_t sx, int32_t sy, int32_t dx, int32_t dy, int32_t w, int32_t h, bool maskSet, bool maskEval);
// A rectangle of VRAM words (vram is 1024 wide) the CPU wrote; it may wrap around VRAM's edges.
void glrUpload(struct Glr*, const uint16_t* vram, int32_t x, int32_t y, int32_t w, int32_t h);

// Draws everything and makes the picture: VRAM's displayed area (width by height VRAM pixels from
// x, y; for 24-bit, width is in picture pixels), or black with the display off.
void glrPicture(struct Glr*, int32_t x, int32_t y, int32_t width, int32_t height, bool mode24, bool off);
// The picture: its GL framebuffer and size, or its pixels (0xRRGGBB, top row first).
unsigned glrPictureFramebuffer(struct Glr*, int* width, int* height);
void glrReadPicture(struct Glr*, uint32_t* pixels);
// The picture copied to framebuffer (a frontend's), bottom row first.
void glrShow(struct Glr*, unsigned framebuffer);
// Where primitives are listed as they come (for tests), or NULL.
extern FILE* glrLogFile;
// VRAM as drawn, at 1x (for tests).
void glrReadVram(struct Glr*, uint16_t* vram);
