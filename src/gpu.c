/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The GPU, ported to C from Mednafen's PS1 module (gpu.cpp, gpu_common.inc, gpu_polygon.cpp,
 * gpu_sprite.cpp, gpu_line.cpp; Copyright (C) 2011-2017 Mednafen Team, GPL-2.0-or-later). */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "psx.h"
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

#define ALWAYS static inline __attribute__((always_inline))

enum { CMD_NONE = 0, CMD_FBWRITE = 1, CMD_FBREAD = 2, CMD_QUAD = 4, CMD_PLINE = 8 };

static const uint32_t dotRatios[5] = { 10, 8, 5, 4, 7 };
enum { HVIS_MAX = 2800, HVIS = 2640, HVIS_OFFS = (HVIS_MAX - HVIS) / 2, DRXBO = 32, FB_WIDTH = 768 };

static inline int32_t sx11(uint32_t v) {
	return (int32_t) (v << 21) >> 21;
}

#define VRAM(g, x, y) (g)->vram[((y) & 511) * 1024 + ((x) & 1023)]

ALWAYS void plot(struct Gpu* g, int blend, bool maskEval, bool textured, uint32_t x, uint32_t y, uint16_t fore) {
	y &= 511;
	uint16_t* dest = &g->vram[y * 1024 + x];
	if (blend >= 0 && (fore & 0x8000)) {
		uint32_t bg = *dest, pix = 0;
		switch (blend) {
		case 0:
			bg |= 0x8000;
			pix = ((fore + bg) - ((fore ^ bg) & 0x0421)) >> 1;
			break;
		case 1: {
			bg &= ~0x8000u;
			uint32_t sum = fore + bg, carry = (sum - ((fore ^ bg) & 0x8421)) & 0x8420;
			pix = (sum - carry) | (carry - (carry >> 5));
			break;
		}
		case 2: {
			bg |= 0x8000;
			uint32_t f = fore & ~0x8000u;
			uint32_t diff = bg - f + 0x108420, borrow = (diff - ((bg ^ f) & 0x108420)) & 0x108420;
			pix = (diff - borrow) & (borrow - (borrow >> 5));
			break;
		}
		case 3: {
			bg &= ~0x8000u;
			uint32_t f = ((fore >> 2) & 0x1CE7) | 0x8000;
			uint32_t sum = f + bg, carry = (sum - ((f ^ bg) & 0x8421)) & 0x8420;
			pix = (sum - carry) | (carry - (carry >> 5));
			break;
		}
		}
		if (!maskEval || !(*dest & 0x8000)) {
			*dest = (textured ? pix : (pix & 0x7FFF)) | g->maskSet;
		}
	} else if (!maskEval || !(*dest & 0x8000)) {
		*dest = (textured ? fore : (fore & 0x7FFF)) | g->maskSet;
	}
}

static const int8_t ditherTable[4][4] = { { -4, 0, -3, 1 }, { 2, -2, 3, -1 }, { -3, 1, -4, 0 }, { 3, -1, 2, -2 } };

ALWAYS uint16_t modTexel(struct Gpu* g, uint16_t texel, int32_t r, int32_t gr, int32_t b, int dx, int dy) {
	uint16_t out = texel & 0x8000;
	out |= g->ditherLut[dy][dx][((texel & 0x1F) * r) >> 4];
	out |= g->ditherLut[dy][dx][((texel & 0x3E0) * gr) >> 9] << 5;
	out |= g->ditherLut[dy][dx][((texel & 0x7C00) * b) >> 14] << 10;
	return out;
}

// The CLUT cache reloads when the palette moves or more entries are needed than it holds: 8-bit to
// 4-bit keeps it (ps1-tests clut-cache on hardware).
ALWAYS void updateClut(struct Gpu* g, bool draw, int texMode, uint16_t raw) {
	if (texMode < 2) {
		uint32_t tag = raw & 0x7FFF, count = texMode ? 256 : 16;
		if (g->clutTag != tag || g->clutCount < count) {
			const uint16_t* line = &g->vram[((raw >> 6) & 0x1FF) * 1024];
			uint32_t cx = (raw & 0x3F) << 4;
			g->drawTime -= count;
			for (uint32_t i = 0; draw && i < count; i++) {
				g->clut[i] = line[(cx + i) & 0x3FF];
			}
			g->clutTag = tag;
			g->clutCount = count;
		}
	}
}

static void recalcWindow(struct Gpu* g) {
	g->window.xAnd = ~(g->tww << 3);
	g->window.xAdd = ((g->twx & g->tww) << 3) + (g->texPageX << (2 - (g->texMode < 2 ? g->texMode : 2)));
	g->window.yAnd = ~(g->twh << 3);
	g->window.yAdd = ((g->twy & g->twh) << 3) + g->texPageY;
}

// (Without draw, only the cache's tags and the time.)
ALWAYS uint16_t texel(struct Gpu* g, bool draw, int texMode, uint32_t u, uint32_t v) {
	uint32_t uExt = (u & g->window.xAnd) + g->window.xAdd;
	uint32_t fx = (uExt >> (2 - texMode)) & 1023;
	uint32_t fy = (v & g->window.yAnd) + g->window.yAdd;
	uint32_t gro = fy * 1024 + fx;
	if (draw && g->roughTime) {
		// The Fast mode draws from VRAM itself, without the texture cache
		uint16_t w = g->vram[gro & (512 * 1024 - 1)];
		if (texMode != 2) {
			w = texMode == 0 ? (w >> ((uExt & 3) * 4)) & 0xF : (w >> ((uExt & 1) * 8)) & 0xFF;
			w = g->clut[w];
		}
		return w;
	}
	uint32_t index = texMode == 0 ? (((gro >> 2) & 3) | ((gro >> 8) & 0xFC)) : (((gro >> 2) & 7) | ((gro >> 7) & 0xF8));
	struct { uint16_t data[4]; uint32_t tag; }* c = (void*) &g->texCache[index];
	if (c->tag != (gro & ~3u)) {
		g->drawTime -= 4;
		if (draw) memcpy(c->data, &g->vram[(gro & ~3u) & (512 * 1024 - 1)], 8);
		c->tag = gro & ~3u;
	}
	if (!draw) return 0;
	uint16_t w = c->data[gro & 3];
	if (texMode != 2) {
		w = texMode == 0 ? (w >> ((uExt & 3) * 4)) & 0xF : (w >> ((uExt & 1) * 8)) & 0xFF;
		w = g->clut[w];
	}
	return w;
}

ALWAYS bool lineSkip(struct Gpu* g, unsigned y) {
	return (g->displayMode & 0x24) == 0x24 && !g->drawDisplayed && (y & 1) == ((g->displayY + g->fieldReadout) & 1);
}

static void invalidateTexCache(struct Gpu* g) {
	for (int i = 0; i < 256; i++) {
		g->texCache[i].tag = ~0u;
	}
}

static void invalidateCache(struct Gpu* g) {
	g->clutTag = ~0u;
	invalidateTexCache(g);
}

// Triangles

enum { COORD_FBS = 12, COORD_POST_PADDING = 12 };

struct Interp { uint32_t u, v, r, g, b; };
struct Deltas { uint32_t dudx, dvdx, drdx, dgdx, dbdx, dudy, dvdy, drdy, dgdy, dbdy; };

static inline int64_t polyXfp(uint32_t x) {
	return (int64_t) (((uint64_t) x << 32) + ((1ull << 32) - (1 << 11)));
}

static inline int64_t polyXfpStep(int32_t dx, int32_t dy) {
	int64_t ex = (int64_t) ((uint64_t) (int64_t) dx << 32);
	if (ex < 0) ex -= dy - 1;
	if (ex > 0) ex += dy - 1;
	return ex / dy;
}

#define CALCIS(x, y) (((B->x - A->x) * (C->y - B->y)) - ((C->x - B->x) * (B->y - A->y)))
ALWAYS bool deltas(struct Deltas* d, const struct GpuVertex* A, const struct GpuVertex* B, const struct GpuVertex* C, bool gouraud, bool textured) {
	int32_t denom = CALCIS(x, y);
	if (!denom) return false;
	if (gouraud) {
		d->drdx = (uint32_t) ((int32_t) (CALCIS(r, y)) * (1 << COORD_FBS) / denom) << COORD_POST_PADDING;
		d->drdy = (uint32_t) ((int32_t) (CALCIS(x, r)) * (1 << COORD_FBS) / denom) << COORD_POST_PADDING;
		d->dgdx = (uint32_t) ((int32_t) (CALCIS(g, y)) * (1 << COORD_FBS) / denom) << COORD_POST_PADDING;
		d->dgdy = (uint32_t) ((int32_t) (CALCIS(x, g)) * (1 << COORD_FBS) / denom) << COORD_POST_PADDING;
		d->dbdx = (uint32_t) ((int32_t) (CALCIS(b, y)) * (1 << COORD_FBS) / denom) << COORD_POST_PADDING;
		d->dbdy = (uint32_t) ((int32_t) (CALCIS(x, b)) * (1 << COORD_FBS) / denom) << COORD_POST_PADDING;
	}
	if (textured) {
		d->dudx = (uint32_t) ((int32_t) (CALCIS(u, y)) * (1 << COORD_FBS) / denom) << COORD_POST_PADDING;
		d->dudy = (uint32_t) ((int32_t) (CALCIS(x, u)) * (1 << COORD_FBS) / denom) << COORD_POST_PADDING;
		d->dvdx = (uint32_t) ((int32_t) (CALCIS(v, y)) * (1 << COORD_FBS) / denom) << COORD_POST_PADDING;
		d->dvdy = (uint32_t) ((int32_t) (CALCIS(x, v)) * (1 << COORD_FBS) / denom) << COORD_POST_PADDING;
	}
	return true;
}
#undef CALCIS

ALWAYS void addDx(struct Interp* i, const struct Deltas* d, uint32_t n, bool gouraud, bool textured) {
	if (textured) { i->u += d->dudx * n; i->v += d->dvdx * n; }
	if (gouraud) { i->r += d->drdx * n; i->g += d->dgdx * n; i->b += d->dbdx * n; }
}

ALWAYS void addDy(struct Interp* i, const struct Deltas* d, uint32_t n, bool gouraud, bool textured) {
	if (textured) { i->u += d->dudy * n; i->v += d->dvdy * n; }
	if (gouraud) { i->r += d->drdy * n; i->g += d->dgdy * n; i->b += d->dbdy * n; }
}

#if defined(__SSE2__)
// Opaque, shaded texture spans 8 pixels at a time: the texels one by one, then modTexel and plot's
// write for all 8 in vector registers (the same results). A channel's ((t & mask) * c) >> n is
// ((t >> shift) & 31) * c >> 4, which fits 16 bits.
ALWAYS __m128i shadeChannel(__m128i t, __m128i c, __m128i off) {
	__m128i v = _mm_srli_epi16(_mm_mullo_epi16(_mm_and_si128(t, _mm_set1_epi16(31)), c), 4);
	v = _mm_srai_epi16(_mm_add_epi16(v, off), 3);
	return _mm_min_epi16(_mm_max_epi16(v, _mm_setzero_si128()), _mm_set1_epi16(31));
}

ALWAYS void spanVector(struct Gpu* g, int y, int32_t x, int32_t w, struct Interp ig, const struct Deltas* d, bool gouraud,
                       int texMode, bool maskEval) {
	uint16_t* dest = &g->vram[(y & 511) * 1024 + x];
	__m128i offsets[4];
	for (int phase = 0; phase < 4; phase++) {
		int16_t o[8];
		for (int k = 0; k < 8; k++) o[k] = g->dither ? ditherTable[y & 3][(phase + k) & 3] : 0;
		offsets[phase] = _mm_loadu_si128((const __m128i*) o);
	}
	const __m128i top = _mm_set1_epi16((short) 0x8000), set = _mm_set1_epi16((short) g->maskSet);
	int32_t i = 0;
	for (; i + 8 <= w; i += 8) {
		uint16_t t[8], cr[8], cg[8], cb[8];
		for (int k = 0; k < 8; k++) {
			t[k] = texel(g, true, texMode, ig.u >> (COORD_FBS + COORD_POST_PADDING), ig.v >> (COORD_FBS + COORD_POST_PADDING));
			cr[k] = (uint16_t) (ig.r >> (COORD_FBS + COORD_POST_PADDING));
			cg[k] = (uint16_t) (ig.g >> (COORD_FBS + COORD_POST_PADDING));
			cb[k] = (uint16_t) (ig.b >> (COORD_FBS + COORD_POST_PADDING));
			addDx(&ig, d, 1, gouraud, true);
		}
		__m128i tex = _mm_loadu_si128((const __m128i*) t);
		__m128i off = offsets[(x + i) & 3];
		__m128i c0 = shadeChannel(tex, _mm_loadu_si128((const __m128i*) cr), off);
		__m128i c1 = shadeChannel(_mm_srli_epi16(tex, 5), _mm_loadu_si128((const __m128i*) cg), off);
		__m128i c2 = shadeChannel(_mm_srli_epi16(tex, 10), _mm_loadu_si128((const __m128i*) cb), off);
		__m128i out = _mm_or_si128(_mm_and_si128(tex, top), _mm_or_si128(c0, _mm_or_si128(_mm_slli_epi16(c1, 5), _mm_slli_epi16(c2, 10))));
		out = _mm_or_si128(out, set);
		__m128i bg = _mm_loadu_si128((const __m128i*) (dest + i));
		__m128i skip = _mm_cmpeq_epi16(tex, _mm_setzero_si128());
		if (maskEval) skip = _mm_or_si128(skip, _mm_srai_epi16(bg, 15));
		_mm_storeu_si128((__m128i*) (dest + i), _mm_or_si128(_mm_andnot_si128(skip, out), _mm_and_si128(skip, bg)));
	}
	for (; i < w; i++) {
		uint16_t t = texel(g, true, texMode, ig.u >> (COORD_FBS + COORD_POST_PADDING), ig.v >> (COORD_FBS + COORD_POST_PADDING));
		if (t) {
			int dx = (x + i) & 3, dy = y & 3;
			if (!g->dither) { dx = 3; dy = 2; }
			t = modTexel(g, t, ig.r >> (COORD_FBS + COORD_POST_PADDING), ig.g >> (COORD_FBS + COORD_POST_PADDING),
			             ig.b >> (COORD_FBS + COORD_POST_PADDING), dx, dy);
			plot(g, -1, maskEval, true, (uint32_t) (x + i), (uint32_t) y, t);
		}
		addDx(&ig, d, 1, gouraud, true);
	}
}
#endif

ALWAYS void drawSpan(struct Gpu* g, bool draw, int y, int32_t xStart, int32_t xBound, struct Interp ig, const struct Deltas* d,
                     bool gouraud, bool textured, int blend, bool texMult, int texMode, bool maskEval) {
	if (lineSkip(g, y)) return;
	if (draw && g->drawParity >= 0 && (y & 1) != g->drawParity) return;
	int32_t xAdjust = xStart, w = xBound - xStart, x = sx11(xStart);
	if (x < g->clipX0) {
		int32_t delta = g->clipX0 - x;
		xAdjust += delta;
		x += delta;
		w -= delta;
	}
	if (x + w > g->clipX1 + 1) w = g->clipX1 + 1 - x;
	if (w <= 0) return;
	addDx(&ig, d, xAdjust, gouraud, textured);
	addDy(&ig, d, y, gouraud, textured);
	if (gouraud || textured) g->drawTime -= w * 2;
	else if (blend >= 0 || maskEval) g->drawTime -= w + ((w + 1) >> 1);
	else g->drawTime -= w;
	if (!draw && !textured) return;
#if defined(__SSE2__)
	if (draw && textured && texMult && blend < 0 && w >= 8) {
		spanVector(g, y, x, w, ig, d, gouraud, texMode, maskEval);
		return;
	}
#endif
	do {
		uint32_t r = ig.r >> (COORD_FBS + COORD_POST_PADDING);
		uint32_t gg = ig.g >> (COORD_FBS + COORD_POST_PADDING);
		uint32_t b = ig.b >> (COORD_FBS + COORD_POST_PADDING);
		if (textured) {
			uint16_t t = texel(g, draw, texMode, ig.u >> (COORD_FBS + COORD_POST_PADDING), ig.v >> (COORD_FBS + COORD_POST_PADDING));
			if (draw && t) {
				if (texMult) {
					int dx = x & 3, dy = y & 3;
					if (!g->dither) { dx = 3; dy = 2; }
					t = modTexel(g, t, r, gg, b, dx, dy);
				}
				plot(g, blend, maskEval, true, x, y, t);
			}
		} else {
			uint16_t pix = 0x8000;
			if (gouraud && g->dither) {
				pix |= g->ditherLut[y & 3][x & 3][r] | g->ditherLut[y & 3][x & 3][gg] << 5 | g->ditherLut[y & 3][x & 3][b] << 10;
			} else {
				pix |= (r >> 3) | (gg >> 3) << 5 | (b >> 3) << 10;
			}
			plot(g, blend, maskEval, false, x, y, pix);
		}
		x++;
		addDx(&ig, d, 1, gouraud, textured);
	} while (--w > 0);
}

static inline void swapVertex(struct GpuVertex* a, struct GpuVertex* b) {
	struct GpuVertex t = *a;
	*a = *b;
	*b = t;
}

// The Fast mode's drawing time for a triangle (sorted by y): its area within the drawing area at the
// rate drawSpan charges.
static void roughTriangle(struct Gpu* g, const struct GpuVertex* v, bool gouraud, bool textured, int blend, bool maskEval) {
	int64_t area2 = (int64_t) (v[1].x - v[0].x) * (v[2].y - v[0].y) - (int64_t) (v[2].x - v[0].x) * (v[1].y - v[0].y);
	if (area2 < 0) area2 = -area2;
	int32_t x0 = v[0].x, x1 = v[0].x;
	for (int i = 1; i < 3; i++) {
		if (v[i].x < x0) x0 = v[i].x;
		if (v[i].x > x1) x1 = v[i].x;
	}
	int32_t y0 = v[0].y > g->clipY0 ? v[0].y : g->clipY0, y1 = v[2].y < g->clipY1 + 1 ? v[2].y : g->clipY1 + 1;
	if (x0 < g->clipX0) x0 = g->clipX0;
	if (x1 > g->clipX1 + 1) x1 = g->clipX1 + 1;
	if (x1 <= x0 || y1 <= y0) return;
	int64_t pixels = area2 / 2, box = (int64_t) (x1 - x0) * (y1 - y0);
	if (pixels > box) pixels = box;
	if (gouraud || textured) g->drawTime -= (int32_t) (pixels * 2);
	else if (blend >= 0 || maskEval) g->drawTime -= (int32_t) (pixels + ((pixels + 1) >> 1));
	else g->drawTime -= (int32_t) pixels;
}

ALWAYS void drawTriangle(struct Gpu* g, bool draw, struct GpuVertex* v, bool gouraud, bool textured, int blend, bool texMult, int texMode, bool maskEval) {
	struct Deltas d;
	unsigned core;
	{
		unsigned cv = 0;
		if (v[1].x <= v[0].x) cv = v[2].x <= v[1].x ? 4 : 2;
		else if (v[2].x < v[0].x) cv = 4;
		else cv = 1;
		if (v[2].y < v[1].y) {
			swapVertex(&v[2], &v[1]);
			cv = ((cv >> 1) & 2) | ((cv << 1) & 4) | (cv & 1);
		}
		if (v[1].y < v[0].y) {
			swapVertex(&v[1], &v[0]);
			cv = ((cv >> 1) & 1) | ((cv << 1) & 2) | (cv & 4);
		}
		if (v[2].y < v[1].y) {
			swapVertex(&v[2], &v[1]);
			cv = ((cv >> 1) & 2) | ((cv << 1) & 4) | (cv & 1);
		}
		core = cv >> 1;
	}
	if (v[0].y == v[2].y || v[2].y - v[0].y >= 512) return;
	if (abs(v[2].x - v[0].x) >= 1024 || abs(v[2].x - v[1].x) >= 1024 || abs(v[1].x - v[0].x) >= 1024) return;
	if (!draw && g->roughTime) {
		roughTriangle(g, v, gouraud, textured, blend, maskEval);
		return;
	}
	if (!deltas(&d, &v[0], &v[1], &v[2], gouraud, textured)) return;

	struct Interp ig;
	if (textured) {
		ig.u = ((v[core].u << COORD_FBS) + (1 << (COORD_FBS - 1))) << COORD_POST_PADDING;
		ig.v = ((v[core].v << COORD_FBS) + (1 << (COORD_FBS - 1))) << COORD_POST_PADDING;
	}
	ig.r = ((v[core].r << COORD_FBS) + (1 << (COORD_FBS - 1))) << COORD_POST_PADDING;
	ig.g = ((v[core].g << COORD_FBS) + (1 << (COORD_FBS - 1))) << COORD_POST_PADDING;
	ig.b = ((v[core].b << COORD_FBS) + (1 << (COORD_FBS - 1))) << COORD_POST_PADDING;
	addDx(&ig, &d, -v[core].x, gouraud, textured);
	addDy(&ig, &d, -v[core].y, gouraud, textured);

	int64_t baseCoord = polyXfp(v[0].x);
	int64_t baseStep = polyXfpStep(v[2].x - v[0].x, v[2].y - v[0].y);
	int64_t boundUs, boundLs;
	bool right;
	if (v[1].y == v[0].y) {
		boundUs = 0;
		right = v[1].x > v[0].x;
	} else {
		boundUs = polyXfpStep(v[1].x - v[0].x, v[1].y - v[0].y);
		right = boundUs > baseStep;
	}
	boundLs = v[2].y == v[1].y ? 0 : polyXfpStep(v[2].x - v[1].x, v[2].y - v[1].y);

	struct { uint64_t xCoord[2], xStep[2]; int32_t yCoord, yBound; bool dec; } part[2];
	unsigned vo = core ? 1 : 0, vp = core == 2 ? 3 : 0;
	{
		typeof(part[0])* tp = &part[vo];
		tp->yCoord = v[0 ^ vo].y;
		tp->yBound = v[1 ^ vo].y;
		tp->xCoord[right] = polyXfp(v[0 ^ vo].x);
		tp->xStep[right] = boundUs;
		tp->xCoord[!right] = baseCoord + ((v[vo].y - v[0].y) * baseStep);
		tp->xStep[!right] = baseStep;
		tp->dec = vo;
	}
	{
		typeof(part[0])* tp = &part[vo ^ 1];
		tp->yCoord = v[1 ^ vp].y;
		tp->yBound = v[2 ^ vp].y;
		tp->xCoord[right] = polyXfp(v[1 ^ vp].x);
		tp->xStep[right] = boundLs;
		tp->xCoord[!right] = baseCoord + ((v[1 ^ vp].y - v[0].y) * baseStep);
		tp->xStep[!right] = baseStep;
		tp->dec = vp;
	}
	for (int i = 0; i < 2; i++) {
		int32_t yi = part[i].yCoord, yb = part[i].yBound;
		uint64_t lc = part[i].xCoord[0], ls = part[i].xStep[0], rc = part[i].xCoord[1], rs = part[i].xStep[1];
		if (part[i].dec) {
			while (yi > yb) {
				yi--;
				lc -= ls;
				rc -= rs;
				int32_t y = sx11(yi);
				if (y < g->clipY0) break;
				if (y > g->clipY1) {
					g->drawTime -= 2;
					continue;
				}
				drawSpan(g, draw, yi, (int32_t) (lc >> 32), (int32_t) (rc >> 32), ig, &d, gouraud, textured, blend, texMult, texMode, maskEval);
			}
		} else {
			while (yi < yb) {
				int32_t y = sx11(yi);
				if (y > g->clipY1) break;
				if (y < g->clipY0) {
					g->drawTime -= 2;
				} else {
					drawSpan(g, draw, yi, (int32_t) (lc >> 32), (int32_t) (rc >> 32), ig, &d, gouraud, textured, blend, texMult, texMode, maskEval);
				}
				yi++;
				lc += ls;
				rc += rs;
			}
		}
	}
}

#define DISPATCH_MASK(F, ...) do { if (maskEval) F(__VA_ARGS__, true); else F(__VA_ARGS__, false); } while (0)
#define DISPATCH_TEXMODE(F, ...) do { \
	switch (texMode) { \
	case 0: DISPATCH_MASK(F, __VA_ARGS__, 0); break; \
	case 1: DISPATCH_MASK(F, __VA_ARGS__, 1); break; \
	default: DISPATCH_MASK(F, __VA_ARGS__, 2); break; \
	} } while (0)
#define DISPATCH_TEXMULT(F, ...) do { if (texMult) DISPATCH_TEXMODE(F, __VA_ARGS__, true); else DISPATCH_TEXMODE(F, __VA_ARGS__, false); } while (0)
#define DISPATCH_BLEND(D, F, ...) do { \
	switch (blend) { \
	case 0: D(F, __VA_ARGS__, 0); break; \
	case 1: D(F, __VA_ARGS__, 1); break; \
	case 2: D(F, __VA_ARGS__, 2); break; \
	case 3: D(F, __VA_ARGS__, 3); break; \
	default: D(F, __VA_ARGS__, -1); break; \
	} } while (0)
#define UNTEXTURED(F, ...) DISPATCH_MASK(F, __VA_ARGS__, false, 0)

// Drawing, or (on the emulation's thread while another draws) only its time and cache tags.
static void triangleTextured(struct Gpu* g, struct GpuVertex* v, bool gouraud, int blend, bool texMult, int texMode, bool maskEval) {
	if (!g->drawPixels) {
		if (gouraud) DISPATCH_BLEND(DISPATCH_TEXMULT, drawTriangle, g, false, v, true, true);
		else DISPATCH_BLEND(DISPATCH_TEXMULT, drawTriangle, g, false, v, false, true);
	} else if (gouraud) {
		DISPATCH_BLEND(DISPATCH_TEXMULT, drawTriangle, g, true, v, true, true);
	} else {
		DISPATCH_BLEND(DISPATCH_TEXMULT, drawTriangle, g, true, v, false, true);
	}
}

static void triangleUntextured(struct Gpu* g, struct GpuVertex* v, bool gouraud, int blend, bool maskEval) {
	if (!g->drawPixels) {
		if (gouraud) DISPATCH_BLEND(UNTEXTURED, drawTriangle, g, false, v, true, false);
		else DISPATCH_BLEND(UNTEXTURED, drawTriangle, g, false, v, false, false);
	} else if (gouraud) {
		DISPATCH_BLEND(UNTEXTURED, drawTriangle, g, true, v, true, false);
	} else {
		DISPATCH_BLEND(UNTEXTURED, drawTriangle, g, true, v, false, false);
	}
}

static void setTpage(struct Gpu* g, uint32_t w) {
	uint32_t x = (w & 0xF) * 64, y = (w & 0x10) * 16, mode = (w >> 7) & 3;
	g->abr = (w >> 5) & 3;
	if (!mode != !g->texMode || x != g->texPageX || y != g->texPageY) {
		invalidateTexCache(g);
	}
	// Without "allow texture disable" a write clears the bit (ps1-tests gp0-e1 on hardware).
	bool disable = g->texDisableAllowChange && ((w >> 11) & 1);
	if (disable != g->texDisable) invalidateTexCache(g);
	g->texDisable = disable;
	g->texPageX = x;
	g->texPageY = y;
	g->texMode = mode;
	recalcWindow(g);
}

static void commandPolygon(struct Gpu* g, const uint32_t* cb, uint32_t cc) {
	bool quad = cc & 8, gouraud = cc & 0x10, textured = cc & 4;
	int blend = cc & 2 ? (int) g->abr : -1;
	bool texMult = textured && !(cc & 1);
	int texMode = g->texMode < 2 ? g->texMode : 2;
	bool maskEval = g->maskEval;
	struct GpuVertex v[3];
	unsigned sv = 0;
	uint32_t cb0 = cb[0];
	g->drawTime -= quad && g->inCommand == CMD_QUAD ? 28 + 18 : 64 + 18;
	if (gouraud && textured) g->drawTime -= 150 * 3;
	else if (gouraud) g->drawTime -= 96 * 3;
	else if (textured) g->drawTime -= 60 * 3;
	if (quad && g->inCommand == CMD_QUAD) {
		v[0] = g->quad[1];
		v[1] = g->quad[2];
		sv = 2;
	}
	for (unsigned i = sv; i < 3; i++) {
		if (i == 0 || gouraud) {
			v[i].r = *cb & 0xFF;
			v[i].g = (*cb >> 8) & 0xFF;
			v[i].b = (*cb >> 16) & 0xFF;
			cb++;
		} else {
			v[i].r = v[0].r;
			v[i].g = v[0].g;
			v[i].b = v[0].b;
		}
		v[i].x = sx11((uint32_t) (int32_t) (int16_t) (*cb & 0xFFFF)) + g->offX;
		v[i].y = sx11((uint32_t) (int32_t) (int16_t) (*cb >> 16)) + g->offY;
		cb++;
		if (textured) {
			v[i].u = *cb & 0xFF;
			v[i].v = (*cb >> 8) & 0xFF;
			if (i == 0) updateClut(g, g->drawPixels, texMode, (*cb >> 16) & 0xFFFF);
			cb++;
		}
	}
	if (quad) {
		if (g->inCommand == CMD_QUAD) {
			g->inCommand = CMD_NONE;
		} else {
			g->inCommand = CMD_QUAD;
			g->inCommandCode = cb0 >> 24;
			memcpy(g->quad, v, sizeof g->quad);
		}
	}
	if (textured) triangleTextured(g, v, gouraud, blend, texMult, texMode, maskEval);
	else triangleUntextured(g, v, gouraud, blend, maskEval);
}

// Sprites

ALWAYS void drawSprite(struct Gpu* g, bool draw, int32_t x0, int32_t y0, int32_t w, int32_t h, uint8_t u0, uint8_t v0, uint32_t color,
                       bool flipX, bool flipY, bool textured, int blend, bool texMult, int texMode, bool maskEval) {
	int32_t r = color & 0xFF, gr = (color >> 8) & 0xFF, b = (color >> 16) & 0xFF;
	uint16_t fill = 0x8000 | (r >> 3) | (gr >> 3) << 5 | (b >> 3) << 10;
	int32_t xs = x0, xb = x0 + w, ys = y0, yb = y0 + h;
	uint8_t u = 0, v = 0;
	int uInc = 1, vInc = 1;
	if (textured) {
		u = u0;
		v = v0;
		if (flipX) { uInc = -1; u |= 1; }
		if (flipY) vInc = -1;
	}
	if (xs < g->clipX0) {
		if (textured) u += (g->clipX0 - xs) * uInc;
		xs = g->clipX0;
	}
	if (ys < g->clipY0) {
		if (textured) v += (g->clipY0 - ys) * vInc;
		ys = g->clipY0;
	}
	if (xb > g->clipX1 + 1) xb = g->clipX1 + 1;
	if (yb > g->clipY1 + 1) yb = g->clipY1 + 1;
	for (int32_t y = ys; y < yb; y++) {
		uint8_t ur = u;
		if (!lineSkip(g, y) && !(draw && g->drawParity >= 0 && (y & 1) != g->drawParity)) {
			if (xb > xs) {
				int32_t t = xb - xs;
				if (blend >= 0 || maskEval) t += (((xb + 1) & ~1) - (xs & ~1)) >> 1;
				g->drawTime -= t;
			}
			for (int32_t x = xs; x < xb && (draw || (textured && !g->roughTime)); x++) {
				if (textured) {
					uint16_t t = texel(g, draw, texMode, ur, v);
					if (draw && t) {
						if (texMult) t = modTexel(g, t, r, gr, b, 3, 2);
						plot(g, blend, maskEval, true, x, y, t);
					}
					ur += uInc;
				} else {
					plot(g, blend, maskEval, false, x, y, fill);
				}
			}
		}
		if (textured) v += vInc;
	}
}

#define SPRITE_ARGS(draw) g, draw, x, y, w, h, u, v, color
#define SPRITE_FLIPS(draw) do { \
	switch ((flipX ? 1 : 0) | (flipY ? 2 : 0)) { \
	case 0: DISPATCH_BLEND(DISPATCH_TEXMULT, drawSprite, SPRITE_ARGS(draw), false, false, true); break; \
	case 1: DISPATCH_BLEND(DISPATCH_TEXMULT, drawSprite, SPRITE_ARGS(draw), true, false, true); break; \
	case 2: DISPATCH_BLEND(DISPATCH_TEXMULT, drawSprite, SPRITE_ARGS(draw), false, true, true); break; \
	default: DISPATCH_BLEND(DISPATCH_TEXMULT, drawSprite, SPRITE_ARGS(draw), true, true, true); break; \
	} } while (0)
static void spriteTextured(struct Gpu* g, int32_t x, int32_t y, int32_t w, int32_t h, uint8_t u, uint8_t v, uint32_t color, bool flipX, bool flipY,
                           int blend, bool texMult, int texMode, bool maskEval) {
	if (g->drawPixels) SPRITE_FLIPS(true);
	else SPRITE_FLIPS(false);
}

static void spriteUntextured(struct Gpu* g, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color, int blend, bool maskEval) {
	uint8_t u = 0, v = 0;
	if (g->drawPixels) DISPATCH_BLEND(UNTEXTURED, drawSprite, SPRITE_ARGS(true), false, false, false);
	else DISPATCH_BLEND(UNTEXTURED, drawSprite, SPRITE_ARGS(false), false, false, false);
}

static void commandSprite(struct Gpu* g, const uint32_t* cb, uint32_t cc) {
	int size = (cc >> 3) & 3;
	bool textured = cc & 4;
	int blend = cc & 2 ? (int) g->abr : -1;
	bool texMult = textured && !(cc & 1);
	int texMode = g->texMode < 2 ? g->texMode : 2;
	int32_t x, y, w, h;
	uint8_t u = 0, v = 0;
	g->drawTime -= 16;
	uint32_t color = *cb & 0xFFFFFF;
	cb++;
	x = sx11(*cb & 0xFFFF);
	y = sx11(*cb >> 16);
	cb++;
	if (textured) {
		u = *cb & 0xFF;
		v = (*cb >> 8) & 0xFF;
		updateClut(g, g->drawPixels, texMode, (*cb >> 16) & 0xFFFF);
		cb++;
	}
	switch (size) {
	case 0: w = *cb & 0x3FF; h = (*cb >> 16) & 0x1FF; break;
	case 1: w = h = 1; break;
	case 2: w = h = 8; break;
	default: w = h = 16; break;
	}
	x = sx11(x + g->offX);
	y = sx11(y + g->offY);
	if (textured) {
		if (color == 0x808080) texMult = false;
		spriteTextured(g, x, y, w, h, u, v, color, g->spriteFlip & 0x1000, g->spriteFlip & 0x2000, blend, texMult, texMode, g->maskEval);
	} else {
		spriteUntextured(g, x, y, w, h, color, blend, g->maskEval);
	}
}

// Lines

struct LinePoint { int32_t x, y; uint32_t r, g, b; };

static void drawLine(struct Gpu* g, struct LinePoint* pts, bool gouraud, int blend, bool maskEval) {
	int32_t idx = abs(pts[1].x - pts[0].x), idy = abs(pts[1].y - pts[0].y);
	int32_t k = idx > idy ? idx : idy;
	if (idx >= 1024 || idy >= 512) return;
	if (pts[0].x >= pts[1].x && k) {
		struct LinePoint t = pts[1];
		pts[1] = pts[0];
		pts[0] = t;
	}
	g->drawTime -= k * 2;
	if (!g->drawPixels) return;
	int64_t dxdk = 0, dydk = 0;
	int32_t drdk = 0, dgdk = 0, dbdk = 0;
	if (k) {
		int64_t dx = (int64_t) ((uint64_t) (int64_t) (pts[1].x - pts[0].x) << 32), dy = (int64_t) ((uint64_t) (int64_t) (pts[1].y - pts[0].y) << 32);
		if (dx < 0) dx -= k - 1;
		if (dx > 0) dx += k - 1;
		if (dy < 0) dy -= k - 1;
		if (dy > 0) dy += k - 1;
		dxdk = dx / k;
		dydk = dy / k;
		if (gouraud) {
			drdk = (int32_t) ((uint32_t) (pts[1].r - pts[0].r) << 12) / k;
			dgdk = (int32_t) ((uint32_t) (pts[1].g - pts[0].g) << 12) / k;
			dbdk = (int32_t) ((uint32_t) (pts[1].b - pts[0].b) << 12) / k;
		}
	}
	uint64_t cx = ((uint64_t) (uint32_t) pts[0].x << 32) | (1ull << 31);
	uint64_t cy = ((uint64_t) (uint32_t) pts[0].y << 32) | (1ull << 31);
	cx -= 1024;
	if (dydk < 0) cy -= 1024;
	uint32_t cr = 0, cg = 0, cbl = 0;
	if (gouraud) {
		cr = (pts[0].r << 12) | (1 << 11);
		cg = (pts[0].g << 12) | (1 << 11);
		cbl = (pts[0].b << 12) | (1 << 11);
	}
	for (int32_t i = 0; i <= k; i++) {
		int32_t x = (cx >> 32) & 2047, y = (cy >> 32) & 2047;
		if (!lineSkip(g, y)) {
			uint8_t r, gr, b;
			if (gouraud) {
				r = cr >> 12;
				gr = cg >> 12;
				b = cbl >> 12;
			} else {
				r = pts[0].r;
				gr = pts[0].g;
				b = pts[0].b;
			}
			uint16_t pix = 0x8000;
			if (g->dither) {
				pix |= g->ditherLut[y & 3][x & 3][r] | g->ditherLut[y & 3][x & 3][gr] << 5 | g->ditherLut[y & 3][x & 3][b] << 10;
			} else {
				pix |= (r >> 3) | (gr >> 3) << 5 | (b >> 3) << 10;
			}
			if (x >= g->clipX0 && x <= g->clipX1 && y >= g->clipY0 && y <= g->clipY1 && (g->drawParity < 0 || (y & 1) == g->drawParity)) {
				switch (blend) {
				case 0: maskEval ? plot(g, 0, true, false, x, y, pix) : plot(g, 0, false, false, x, y, pix); break;
				case 1: maskEval ? plot(g, 1, true, false, x, y, pix) : plot(g, 1, false, false, x, y, pix); break;
				case 2: maskEval ? plot(g, 2, true, false, x, y, pix) : plot(g, 2, false, false, x, y, pix); break;
				case 3: maskEval ? plot(g, 3, true, false, x, y, pix) : plot(g, 3, false, false, x, y, pix); break;
				default: maskEval ? plot(g, -1, true, false, x, y, pix) : plot(g, -1, false, false, x, y, pix); break;
				}
			}
		}
		cx += dxdk;
		cy += dydk;
		if (gouraud) {
			cr += drdk;
			cg += dgdk;
			cbl += dbdk;
		}
	}
}

static void commandLine(struct Gpu* g, const uint32_t* cb, uint32_t cc) {
	bool polyline = cc & 8, gouraud = cc & 0x10;
	int blend = cc & 2 ? (int) g->abr : -1;
	struct LinePoint pts[2];
	g->drawTime -= 16;
	if (polyline && g->inCommand == CMD_PLINE) {
		pts[0].x = g->lineLast.x;
		pts[0].y = g->lineLast.y;
		pts[0].r = g->lineLast.r;
		pts[0].g = g->lineLast.g;
		pts[0].b = g->lineLast.b;
	} else {
		pts[0].r = *cb & 0xFF;
		pts[0].g = (*cb >> 8) & 0xFF;
		pts[0].b = (*cb >> 16) & 0xFF;
		cb++;
		pts[0].x = sx11(*cb & 0xFFFF) + g->offX;
		pts[0].y = sx11(*cb >> 16) + g->offY;
		cb++;
	}
	if (gouraud) {
		pts[1].r = *cb & 0xFF;
		pts[1].g = (*cb >> 8) & 0xFF;
		pts[1].b = (*cb >> 16) & 0xFF;
		cb++;
	} else {
		pts[1].r = pts[0].r;
		pts[1].g = pts[0].g;
		pts[1].b = pts[0].b;
	}
	pts[1].x = sx11(*cb & 0xFFFF) + g->offX;
	pts[1].y = sx11(*cb >> 16) + g->offY;
	if (polyline) {
		g->lineLast.x = pts[1].x;
		g->lineLast.y = pts[1].y;
		g->lineLast.r = pts[1].r;
		g->lineLast.g = pts[1].g;
		g->lineLast.b = pts[1].b;
		if (g->inCommand != CMD_PLINE) {
			g->inCommand = CMD_PLINE;
			g->inCommandCode = cc;
		}
	}
	drawLine(g, pts, gouraud, blend, g->maskEval);
}

// Other commands

static void commandFill(struct Gpu* g, const uint32_t* cb) {
	int32_t r = cb[0] & 0xFF, gr = (cb[0] >> 8) & 0xFF, b = (cb[0] >> 16) & 0xFF;
	uint16_t value = (r >> 3) | (gr >> 3) << 5 | (b >> 3) << 10;
	int32_t dx = cb[1] & 0x3F0, dy = (cb[1] >> 16) & 0x3FF;
	int32_t w = ((cb[2] & 0x3FF) + 0xF) & ~0xF, h = (cb[2] >> 16) & 0x1FF;
	g->drawTime -= 46;
	for (int32_t y = 0; y < h; y++) {
		int32_t yy = (y + dy) & 511;
		if (lineSkip(g, yy)) continue;
		g->drawTime -= (w >> 3) + 9;
		if (g->drawParity >= 0 && (yy & 1) != g->drawParity) continue;
		if (!g->drawPixels) continue;
		uint16_t* row = &g->vram[yy * 1024];
		if (dx + w <= 1024) {
			for (int32_t x = 0; x < w; x++) row[dx + x] = value;
		} else {
			for (int32_t x = 0; x < w; x++) row[(x + dx) & 1023] = value;
		}
	}
}

static void commandCopy(struct Gpu* g, const uint32_t* cb) {
	int32_t sx = cb[1] & 0x3FF, sy = (cb[1] >> 16) & 0x3FF, dx = cb[2] & 0x3FF, dy = (cb[2] >> 16) & 0x3FF;
	int32_t w = cb[3] & 0x3FF, h = (cb[3] >> 16) & 0x1FF;
	if (!w) w = 0x400;
	if (!h) h = 0x200;
	invalidateTexCache(g);
	g->drawTime -= w * h * 2;
	for (int32_t y = 0; y < h && g->drawPixels && g->drawParity != 1; y++) {
		for (int32_t x = 0; x < w; x += 128) {
			int32_t n = w - x < 128 ? w - x : 128;
			uint16_t tmp[128];
			for (int32_t i = 0; i < n; i++) tmp[i] = VRAM(g, x + i + sx, y + sy);
			for (int32_t i = 0; i < n; i++) {
				uint16_t* d = &VRAM(g, x + i + dx, y + dy);
				if (!(*d & g->maskEval)) *d = tmp[i] | g->maskSet;
			}
		}
	}
}

static void commandTransfer(struct Gpu* g, const uint32_t* cb, bool write) {
	g->rwX = cb[1] & 0x3FF;
	g->rwY = (cb[1] >> 16) & 0x3FF;
	g->rwW = cb[2] & 0x3FF;
	g->rwH = (cb[2] >> 16) & (write ? 0x1FF : 0x3FF);
	if (!g->rwW) g->rwW = 0x400;
	if (write) {
		if (!g->rwH) g->rwH = 0x200;
	} else if (g->rwH > 0x200) {
		g->rwH &= 0x1FF;
	}
	g->rwCurX = g->rwX;
	g->rwCurY = g->rwY;
	invalidateTexCache(g);
	if (g->rwW && g->rwH) g->inCommand = write ? CMD_FBWRITE : CMD_FBREAD;
}

// Command lengths in words, the FIFO room each needs, and whether it runs while the GPU is busy.
static void commandInfo(uint32_t cc, int* len, int* fifoLen, bool* ss) {
	*ss = false;
	*fifoLen = 1;
	if (cc >= 0x20 && cc < 0x40) {
		*len = 1 + 3 * (1 + ((cc & 4) >> 2) + ((cc & 0x10) >> 4)) - ((cc & 0x10) >> 4);
	} else if (cc >= 0x40 && cc < 0x60) {
		*len = 3 + ((cc & 0x10) >> 4);
	} else if (cc >= 0x60 && cc < 0x80) {
		*len = 2 + ((cc & 4) >> 2) + ((cc & 0x18) ? 0 : 1);
		*fifoLen = 2 | ((cc & 4) >> 2) | ((cc & 0x18) ? 0 : 1);
	} else if (cc >= 0x80 && cc < 0xA0) {
		*len = 4;
		*fifoLen = 2;
	} else if (cc >= 0xA0 && cc < 0xE0) {
		*len = 3;
		*fifoLen = 2;
	} else if (cc == 0x01 || cc == 0xE1 || cc == 0xE2 || cc == 0xE6) {
		*len = 1;
		*fifoLen = 2;
	} else if (cc == 0x02) {
		*len = 3;
		*fifoLen = 3;
	} else if (cc == 0x1F) {
		*len = 1;
	} else if (cc >= 0xE3 && cc <= 0xE5) {
		*len = 1;
		*ss = true;
	} else {
		*len = 1;
		*ss = true;
	}
}

static inline uint32_t fifoPeek(struct Gpu* g) {
	return g->fifo[g->fifoRead];
}

static inline uint32_t fifoPop(struct Gpu* g) {
	uint32_t v = g->fifo[g->fifoRead];
	g->fifoRead = (g->fifoRead + 1) & 31;
	g->fifoCount--;
	return v;
}

// The GPU's drawing thread: it draws what the emulation's thread runs through only for its time, in
// the same order, with a GPU of its own whose VRAM is the real one meanwhile.
enum { JOB_COMMAND, JOB_QUAD, JOB_POLYLINE, JOB_POLYLINE_END, JOB_VRAM_WRITE, JOB_LINE, JOB_GP1, JOBS = 4096 };

struct GpuJob {
	uint8_t type, len;
	bool fieldReadout;
	uint32_t cc, displayMode, displayY;  // and the display as it was then, for interlaced line skipping
	uint32_t w[16];
};

// One writer (the emulation) and one reader (the drawing thread): the indexes are atomic, and the lock
// and conditions are only for a side that has to sleep.
// In the Fast mode a second drawing thread (PSX_GPU_WORKERS=2) can take every other line: both run
// each job, the second starting a job only when the first has done the one before, and the other way
// round. It helps where drawing holds the emulation up and costs a little elsewhere.
struct GpuThread {
	pthread_t thread, thread1;
	pthread_mutex_t lock;
	pthread_cond_t work, done;
	_Atomic unsigned head, head1, tail;  // jobs done by each drawing thread (by the batch), jobs queued
	atomic_bool sleeping, sleeping1, syncing, quit;
	bool two;
	// Each drawing thread's jobs done, job by job for the other, on lines of their own
	_Alignas(64) _Atomic unsigned progress0;
	_Alignas(64) _Atomic unsigned progress1;
	_Alignas(64) char pad;
	uint32_t* frame;
	uint32_t writes[16];  // VRAM write words not queued yet
	int writeCount;
	struct GpuJob jobs[JOBS];
	struct Gpu g, g1;
};

// A spin loop's breath, so a thread sharing the core keeps its pace.
static inline void relax(void) {
#if defined(__x86_64__) || defined(__i386__)
	__builtin_ia32_pause();
#elif defined(__aarch64__)
	__asm__ volatile("yield");
#endif
}

// Jobs both drawing threads have done.
static unsigned consumed(struct GpuThread* t) {
	unsigned a = atomic_load(&t->head);
	if (!t->two) return a;
	unsigned b = atomic_load(&t->head1);
	return (int) (a - b) < 0 ? a : b;
}

static void wake(struct GpuThread* t) {
	if (atomic_load(&t->sleeping) || atomic_load(&t->sleeping1)) {
		pthread_mutex_lock(&t->lock);
		pthread_cond_broadcast(&t->work);
		pthread_mutex_unlock(&t->lock);
	}
}

static void publish(struct GpuThread* t, const struct GpuJob* j) {
	unsigned tail = atomic_load(&t->tail);
	while (tail - consumed(t) == JOBS) sched_yield();
	t->jobs[tail % JOBS] = *j;
	atomic_store(&t->tail, tail + 1);
	// A sleeping drawing thread is woken for a batch, not each job
	if (tail + 1 - consumed(t) >= 64) wake(t);
}

static void flushWrites(struct Psx* p) {
	struct GpuThread* t = p->gpuThread;
	if (!t->writeCount) return;
	struct GpuJob j = { JOB_VRAM_WRITE, (uint8_t) t->writeCount, false, 0, 0, 0, { 0 } };
	memcpy(j.w, t->writes, (size_t) t->writeCount * 4);
	t->writeCount = 0;
	publish(t, &j);
}

static void queueJob(struct Psx* p, int type, const uint32_t* w, int len, uint32_t cc) {
	struct GpuThread* t = p->gpuThread;
	if (!t) return;
	if (type == JOB_VRAM_WRITE) {
		t->writes[t->writeCount++] = w[0];
		if (t->writeCount == 16) flushWrites(p);
		return;
	}
	flushWrites(p);
	struct GpuJob j = { (uint8_t) type, (uint8_t) len, p->gpu.fieldReadout, cc, p->gpu.displayMode, p->gpu.displayY, { 0 } };
	memcpy(j.w, w, (size_t) len * 4);
	publish(t, &j);
}

// Everything queued is drawn.
void gpuSync(struct Psx* p) {
	struct GpuThread* t = p->gpuThread;
	if (!t) return;
	flushWrites(p);
	unsigned tail = atomic_load(&t->tail);
	if (consumed(t) == tail) return;
	wake(t);
	for (int spin = 0; spin < 1000 && consumed(t) != tail; spin++) {}
	if (consumed(t) == tail) return;
	atomic_store(&t->syncing, true);
	pthread_mutex_lock(&t->lock);
	while (consumed(t) != tail) pthread_cond_wait(&t->done, &t->lock);
	pthread_mutex_unlock(&t->lock);
	atomic_store(&t->syncing, false);
}

const uint16_t* gpuVram(struct Psx* p) {
	gpuSync(p);
	return p->gpuThread ? p->gpuThread->g.vram : p->gpu.vram;
}

static void executeDraw(struct Gpu* g, const uint32_t* cb, uint32_t cc) {
	if (cc >= 0x20 && cc < 0x40) {
		if (cc & 4) setTpage(g, cb[4 + ((cc >> 4) & 1)] >> 16);
		commandPolygon(g, cb, cc);
	} else if (cc >= 0x40 && cc < 0x60) {
		commandLine(g, cb, cc);
	} else if (cc >= 0x60 && cc < 0x80) {
		commandSprite(g, cb, cc);
	} else if (cc >= 0x80 && cc < 0xA0) {
		commandCopy(g, cb);
	} else if (cc >= 0xA0 && cc < 0xC0) {
		commandTransfer(g, cb, true);
	} else if (cc >= 0xC0 && cc < 0xE0) {
		commandTransfer(g, cb, false);
	} else {
		switch (cc) {
		case 0x01: invalidateCache(g); break;
		case 0x02: commandFill(g, cb); break;
		case 0xE1:
			setTpage(g, cb[0]);
			g->spriteFlip = cb[0] & 0x3000;
			g->dither = (cb[0] >> 9) & 1;
			g->drawDisplayed = (cb[0] >> 10) & 1;
			break;
		case 0xE2:
			g->tww = cb[0] & 0x1F;
			g->twh = (cb[0] >> 5) & 0x1F;
			g->twx = (cb[0] >> 10) & 0x1F;
			g->twy = (cb[0] >> 15) & 0x1F;
			recalcWindow(g);
			break;
		case 0xE3:
			g->clipX0 = cb[0] & 1023;
			g->clipY0 = (cb[0] >> 10) & 1023;
			break;
		case 0xE4:
			g->clipX1 = cb[0] & 1023;
			g->clipY1 = (cb[0] >> 10) & 1023;
			break;
		case 0xE5:
			g->offX = sx11(cb[0] & 2047);
			g->offY = sx11((cb[0] >> 11) & 2047);
			break;
		case 0xE6:
			g->maskSet = cb[0] & 1 ? 0x8000 : 0;
			g->maskEval = cb[0] & 2 ? 0x8000 : 0;
			break;
		}
	}
}

static void execute(struct Psx* p, const uint32_t* cb, int len, uint32_t cc) {
	struct Gpu* g = &p->gpu;
	if (cc >= 0xA0 && cc < 0xC0 && p->logGpu) {
		fprintf(stderr, "gpu vram write %08x %08x %08x frame %u at %lld\n", cb[0], cb[1], cb[2], p->frames, (long long) (p->cycles - p->frameStart));
	}
	if (cc == 0x1F) {
		g->irq = true;
		psxIrq(p, IRQ_GPU);
		return;
	}
	executeDraw(g, cb, cc);
	queueJob(p, JOB_COMMAND, cb, len, cc);
}

// A word of a CPU-to-VRAM transfer.
static void vramWrite(struct Gpu* g, uint32_t data, bool write) {
	for (int i = 0; i < 2; i++) {
		uint16_t* d = &VRAM(g, g->rwCurX, g->rwCurY);
		if (write && g->drawPixels && !(*d & g->maskEval)) *d = (uint16_t) data | g->maskSet;
		g->rwCurX++;
		if (g->rwCurX == g->rwX + g->rwW) {
			g->rwCurX = g->rwX;
			g->rwCurY++;
			if (g->rwCurY == g->rwY + g->rwH) {
				g->inCommand = CMD_NONE;
				break;
			}
		}
		data >>= 16;
	}
}

static void processFifo(struct Psx* p) {
	struct Gpu* g = &p->gpu;
	while (g->fifoCount) {
		switch (g->inCommand) {
		case CMD_FBREAD:
			return;
		case CMD_FBWRITE: {
			uint32_t data = fifoPop(g);
			vramWrite(g, data, true);
			queueJob(p, JOB_VRAM_WRITE, &data, 1, 0);
			continue;
		}
		case CMD_QUAD: {
			if (g->drawTime < 0) return;
			uint32_t cc = g->inCommandCode;
			int vl = 1 + ((cc & 4) ? 1 : 0) + ((cc & 0x10) ? 1 : 0);
			if (g->fifoCount < vl) return;
			uint32_t cb[3];
			for (int i = 0; i < vl; i++) cb[i] = fifoPop(g);
			commandPolygon(g, cb, cc);
			queueJob(p, JOB_QUAD, cb, vl, cc);
			continue;
		}
		case CMD_PLINE: {
			if (g->drawTime < 0) return;
			uint32_t cc = g->inCommandCode;
			int vl = 1 + ((cc & 0x10) ? 1 : 0);
			if ((fifoPeek(g) & 0xF000F000) == 0x50005000) {
				fifoPop(g);
				g->inCommand = CMD_NONE;
				queueJob(p, JOB_POLYLINE_END, NULL, 0, 0);
				continue;
			}
			if (g->fifoCount < vl) return;
			uint32_t cb[2];
			for (int i = 0; i < vl; i++) cb[i] = fifoPop(g);
			commandLine(g, cb, cc);
			queueJob(p, JOB_POLYLINE, cb, vl, cc);
			continue;
		}
		}
		uint32_t cc = fifoPeek(g) >> 24;
		int len, fifoLen;
		bool ss;
		commandInfo(cc, &len, &fifoLen, &ss);
		if (g->drawTime < 0 && !ss) return;
		if (g->fifoCount < len) return;
		uint32_t cb[16];
		for (int i = 0; i < len; i++) cb[i] = fifoPop(g);
		if (!ss) g->drawTime -= 2;
		execute(p, cb, len, cc);
	}
}

// GP0 words go through the FIFO; a word that finds it full is lost (Mednafen's WriteCB).
void gpuGp0(struct Psx* p, uint32_t word) {
	struct Gpu* g = &p->gpu;
	if (g->fifoCount >= 16) {
		int len, fifoLen;
		bool ss;
		commandInfo(fifoPeek(g) >> 24, &len, &fifoLen, &ss);
		if (g->inCommand != CMD_NONE || g->fifoCount - 16 >= fifoLen) {
			return;
		}
	}
	g->fifo[(g->fifoRead + g->fifoCount) & 31] = word;
	g->fifoCount++;
	processFifo(p);
}

static bool fifoReady(struct Gpu* g) {
	if (g->inCommand & (CMD_PLINE | CMD_QUAD)) return false;
	if (g->fifoCount == 0) return true;
	if (g->inCommand & (CMD_FBREAD | CMD_FBWRITE)) return false;
	int len, fifoLen;
	bool ss;
	commandInfo(fifoPeek(g) >> 24, &len, &fifoLen, &ss);
	return g->fifoCount < fifoLen;
}

bool gpuDmaCanWrite(struct Psx* p) {
	return fifoReady(&p->gpu);
}

static void softResetGpu(struct Gpu* g) {
	g->irq = false;
	invalidateCache(g);
	g->dmaControl = 0;
	if (g->drawTime < 0) g->drawTime = 0;
	g->fifoCount = g->fifoRead = 0;
	g->readBufferEx = 0;
	g->inCommand = CMD_NONE;
	g->displayOff = true;
	g->displayX = g->displayY = 0;
	g->displayMode = 0;
	g->hStart = 0x200;
	g->hEnd = 0xC00;
	g->vStart = 0x10;
	g->vEnd = 0x100;
	g->texPageX = g->texPageY = 0;
	g->spriteFlip = 0;
	g->abr = g->texMode = 0;
	g->dither = g->drawDisplayed = false;
	g->tww = g->twh = g->twx = g->twy = 0;
	recalcWindow(g);
	g->clipX0 = g->clipY0 = g->clipX1 = g->clipY1 = 0;
	g->offX = g->offY = 0;
	g->maskSet = g->maskEval = 0;
	g->texDisable = g->texDisableAllowChange = false;
}

static void softReset(struct Psx* p) {
	softResetGpu(&p->gpu);
	uint32_t v = 0;
	queueJob(p, JOB_GP1, &v, 1, 0x00);
}

static void runJob(struct GpuThread* t, int id, const struct GpuJob* j);

static void drawLoop(struct GpuThread* t, int id) {
	_Atomic unsigned* mine = id ? &t->head1 : &t->head;
	_Atomic unsigned* progress = id ? &t->progress1 : &t->progress0;
	_Atomic unsigned* other = id ? &t->progress0 : &t->progress1;
	atomic_bool* sleeping = id ? &t->sleeping1 : &t->sleeping;
	atomic_bool* otherSleeping = id ? &t->sleeping : &t->sleeping1;
	for (;;) {
		unsigned head = atomic_load(mine), tail = atomic_load(&t->tail);
		for (int spin = 0; spin < 2000 && head == tail; spin++) tail = atomic_load(&t->tail);
		if (head == tail) {
			pthread_mutex_lock(&t->lock);
			atomic_store(sleeping, true);
			while (atomic_load(&t->tail) == head && !atomic_load(&t->quit)) pthread_cond_wait(&t->work, &t->lock);
			atomic_store(sleeping, false);
			pthread_mutex_unlock(&t->lock);
			if (atomic_load(&t->tail) == head) break;
			continue;
		}
		for (unsigned i = head; i != tail; i++) {
			if (t->two) {
				while ((int) (atomic_load(other) - i) < 0) {
					if (atomic_load(otherSleeping)) wake(t);
					relax();
				}
			}
			runJob(t, id, &t->jobs[i % JOBS]);
			if (t->two) atomic_store(progress, i + 1);
		}
		atomic_store(mine, tail);
		if (atomic_load(&t->syncing)) {
			pthread_mutex_lock(&t->lock);
			pthread_cond_broadcast(&t->done);
			pthread_mutex_unlock(&t->lock);
		}
	}
}

static void* drawThread(void* arg) {
	drawLoop(arg, 0);
	return NULL;
}

static void* drawThread1(void* arg) {
	drawLoop(arg, 1);
	return NULL;
}

void gpuFree(struct Psx* p) {
	struct GpuThread* t = p->gpuThread;
	if (!t) return;
	gpuSync(p);
	pthread_mutex_lock(&t->lock);
	atomic_store(&t->quit, true);
	pthread_cond_broadcast(&t->work);
	pthread_mutex_unlock(&t->lock);
	pthread_join(t->thread, NULL);
	if (t->two) pthread_join(t->thread1, NULL);
	memcpy(p->gpu.vramStore, t->g.vram, sizeof p->gpu.vramStore);
	pthread_mutex_destroy(&t->lock);
	pthread_cond_destroy(&t->work);
	pthread_cond_destroy(&t->done);
	free(t);
	p->gpuThread = NULL;
	p->gpu.drawPixels = true;
}

// The drawing thread, unless PSX_GPU_THREAD=0, starting from the GPU as it is.
static void startDrawThread(struct Psx* p) {
	const char* env = getenv("PSX_GPU_THREAD");
	if (!p->gpuThread && (!env || strcmp(env, "0"))) {
		struct GpuThread* t = calloc(1, sizeof *t);
		if (t) {
			pthread_mutex_init(&t->lock, NULL);
			pthread_cond_init(&t->work, NULL);
			pthread_cond_init(&t->done, NULL);
			if (pthread_create(&t->thread, NULL, drawThread, t) == 0) {
				p->gpuThread = t;
				const char* workers = getenv("PSX_GPU_WORKERS");
				if (p->gpu.roughTime && workers && !strcmp(workers, "2")) {
					t->two = pthread_create(&t->thread1, NULL, drawThread1, t) == 0;
				}
			} else {
				pthread_mutex_destroy(&t->lock);
				pthread_cond_destroy(&t->work);
				pthread_cond_destroy(&t->done);
				free(t);
			}
		}
	}
	struct GpuThread* t = p->gpuThread;
	p->gpu.drawPixels = !t;
	if (t) {
		gpuSync(p);
		t->g = p->gpu;
		t->g.vram = t->g.vramStore;
		t->g.drawPixels = true;
		if (t->two) {
			t->g.drawParity = 0;
			t->g1 = t->g;
			t->g1.drawParity = 1;
		}
		t->frame = p->gpu.frame;
	}
}

void gpuReset(struct Psx* p) {
	struct Gpu* g = &p->gpu;
	for (int y = 0; y < 4; y++) {
		for (int x = 0; x < 4; x++) {
			for (int v = 0; v < 512; v++) {
				int value = (v + ditherTable[y][x]) >> 3;
				g->ditherLut[y][x][v] = value < 0 ? 0 : value > 0x1F ? 0x1F : value;
			}
		}
	}
	g->clockRatio = p->pal ? 102948 : 103896;
	g->vram = g->vramStore;
	g->drawParity = -1;
	memset(g->vram, 0, sizeof g->vramStore);
	memset(g->clut, 0, sizeof g->clut);
	g->clutTag = ~0u;
	memset(g->texCache, 0xFF, sizeof g->texCache);
	g->readBuffer = g->readBufferEx = 0;
	g->rwX = g->rwY = g->rwW = g->rwH = g->rwCurX = g->rwCurY = 0;
	g->curYOffset = g->curLineY = 0;
	g->inVblank = true;
	g->linesPerField = 263;
	g->scanline = 0;
	g->field = g->fieldReadout = false;
	g->phaseChange = false;
	g->dotCounter = 0;
	g->clockCounter = 0;
	g->lineCounter = 3412 - 200;
	g->linePhase = 0;
	g->drawTime = 0;
	g->last = p->cycles;
	g->skip = false;
	softReset(p);
	g->hStart = g->hEnd = g->vStart = g->vEnd = 0;
	timersVblank(p, true);
	g->frameW = 320;
	g->frameH = 240;
	g->roughTime = p->mode != MODE_EXACT;
	startDrawThread(p);
}

void gpuWrite(struct Psx* p, uint32_t address, uint32_t v) {
	struct Gpu* g = &p->gpu;
	gpuRun(p);
	if (!(address & 4)) {
		gpuGp0(p, v);
		return;
	}
	uint32_t command = v >> 24;
	v &= 0xFFFFFF;
	switch (command) {
	case 0x00: softReset(p); break;
	case 0x01:
		if (g->drawTime < 0) g->drawTime = 0;
		g->fifoCount = g->fifoRead = 0;
		g->inCommand = CMD_NONE;
		queueJob(p, JOB_GP1, &v, 1, command);
		break;
	case 0x02: g->irq = false; break;
	case 0x03: g->displayOff = v & 1; break;
	case 0x04: g->dmaControl = v & 3; break;
	case 0x05:
		g->displayX = v & 0x3FE;
		g->displayY = (v >> 10) & 0x1FF;
		break;
	case 0x06:
		g->hStart = v & 0xFFF;
		g->hEnd = (v >> 12) & 0xFFF;
		break;
	case 0x07:
		g->vStart = v & 0x3FF;
		g->vEnd = (v >> 10) & 0x3FF;
		break;
	case 0x08: g->displayMode = v & 0xFF; break;
	case 0x09:
		g->texDisableAllowChange = v & 1;
		queueJob(p, JOB_GP1, &v, 1, command);
		break;
	case 0x10:
		switch (v & 0xF) {
		case 2:
			g->readBufferEx = (g->readBufferEx & 0xFFF00000) | g->tww | g->twh << 5 | g->twx << 10 | g->twy << 15;
			g->readBuffer = g->readBufferEx;
			break;
		case 3:
			g->readBufferEx = (g->readBufferEx & 0xFFF00000) | (uint32_t) g->clipY0 << 10 | (uint32_t) g->clipX0;
			g->readBuffer = g->readBufferEx;
			break;
		case 4:
			g->readBufferEx = (g->readBufferEx & 0xFFF00000) | (uint32_t) g->clipY1 << 10 | (uint32_t) g->clipX1;
			g->readBuffer = g->readBufferEx;
			break;
		case 5:
			g->readBufferEx = (g->readBufferEx & 0xFFC00000) | ((uint32_t) g->offX & 2047) | ((uint32_t) g->offY & 2047) << 11;
			g->readBuffer = g->readBufferEx;
			break;
		case 7: g->readBuffer = g->readBufferEx = 2; break;
		case 8: g->readBuffer = g->readBufferEx = 0; break;
		}
		break;
	}
}

static uint32_t readData(struct Psx* p) {
	struct Gpu* g = &p->gpu;
	if (g->inCommand == CMD_FBREAD) {
		const uint16_t* vram = gpuVram(p);
		g->readBufferEx = 0;
		for (int i = 0; i < 2; i++) {
			g->readBufferEx |= (uint32_t) vram[(g->rwCurY & 511) * 1024 + (g->rwCurX & 1023)] << (i * 16);
			g->rwCurX++;
			if (g->rwCurX == g->rwX + g->rwW) {
				if (g->rwCurY + 1 == g->rwY + g->rwH) {
					g->inCommand = CMD_NONE;
				} else {
					g->rwCurY++;
					g->rwCurX = g->rwX;
				}
			}
		}
		return g->readBufferEx;
	}
	return g->readBuffer;
}

uint32_t gpuRead(struct Psx* p, uint32_t address) {
	struct Gpu* g = &p->gpu;
	gpuRun(p);
	if (!(address & 4)) {
		return readData(p);
	}
	uint32_t v = (((g->displayMode << 1) & 0x7F) | ((g->displayMode >> 6) & 1)) << 16;
	v |= (g->displayMode & 0x80) << 7;
	v |= g->dmaControl << 29;
	v |= (g->curLineY & 1) << 31;
	v |= (uint32_t) !g->field << 13;
	if (g->dmaControl & 2) v |= 1 << 25;
	v |= (uint32_t) g->irq << 24;
	v |= (uint32_t) g->displayOff << 23;
	if (g->inCommand == CMD_NONE && g->drawTime >= 0 && g->fifoCount == 0) v |= 1 << 26;
	if (g->inCommand == CMD_FBREAD) v |= 1 << 27;
	v |= (uint32_t) fifoReady(g) << 28;
	v |= g->texPageX >> 6;
	v |= g->texPageY >> 4;
	v |= g->abr << 5;
	v |= g->texMode << 7;
	v |= (uint32_t) g->dither << 9;
	v |= (uint32_t) g->drawDisplayed << 10;
	if (g->maskSet) v |= 1 << 11;
	if (g->maskEval) v |= 1 << 12;
	v |= (uint32_t) g->texDisable << 15;
	return v;
}

// 15-bit pixels to the frame's.
static void outputPixels(uint32_t* restrict dest, const uint16_t* restrict src, int32_t n) {
	for (int32_t x = 0; x < n; x++) {
		uint32_t v = src[x];
		dest[x] = (v & 0x1F) << 19 | (v & 0x3E0) << 6 | (v & 0x7C00) >> 7;
	}
}

// A line of the picture from VRAM's line src.
static void linePixels(uint32_t* dest, const uint16_t* src, int32_t xs, int32_t xe, int32_t dmw, int32_t fbx, bool mode24) {
	for (int32_t x = 0; x < xs; x++) dest[x] = 0;
	if (mode24) {
		for (int32_t x = xs; x < xe; x++) {
			uint32_t s = src[fbx >> 1] | (uint32_t) src[((fbx >> 1) + 1) & 0x7FF] << 16;
			s >>= (fbx & 1) * 8;
			dest[x] = (s & 0xFF) << 16 | (s & 0xFF00) | (s >> 16 & 0xFF);
			fbx = (fbx + 3) & 0x7FF;
		}
	} else if ((fbx >> 1) + (xe - xs) <= 1024) {
		outputPixels(dest + xs, src + (fbx >> 1), xe - xs);
	} else {
		for (int32_t x = xs; x < xe; x++) {
			uint32_t s = src[fbx >> 1];
			dest[x] = (s & 0x1F) << 19 | (s & 0x3E0) << 6 | (s & 0x7C00) >> 7;
			fbx = (fbx + 2) & 0x7FF;
		}
	}
	for (int32_t x = xe; x < dmw; x++) dest[x] = 0;
}

static void outputLine(struct Psx* p) {
	struct Gpu* g = &p->gpu;
	uint32_t dmc = (g->displayMode & 0x40) ? 4 : (g->displayMode & 3);
	int32_t dmw = HVIS_MAX / dotRatios[dmc], dmpa = HVIS_OFFS / dotRatios[dmc];
	unsigned first = p->pal ? 20 : 16, count = p->pal ? 288 : 240;
	if (g->scanline < first || g->scanline >= first + count || g->skip) return;
	bool interlace = g->displayMode & 0x20;
	int32_t line = ((g->scanline - first) << interlace) + (interlace && g->field);
	uint32_t* dest = g->frame + line * FB_WIDTH + (DRXBO - dmpa);
	int32_t fbx = g->displayX * 2, xs = g->hStart, xe = g->hEnd;
	if (xe < xs) xe = xs;
	uint32_t hmc = p->pal ? 560 : 520;
	xs = xs / dotRatios[dmc] - hmc / dotRatios[dmc] + 7;
	xe = xe / dotRatios[dmc] - hmc / dotRatios[dmc] + 7;
	if (xs < 0) {
		fbx -= xs * ((g->displayMode & 0x10) ? 3 : 2);
		fbx &= 0x7FF;
		xs = 0;
	}
	if (xe > dmw) xe = dmw;
	if (g->inVblank || g->displayOff) xs = xe = 0;
	g->lineWidth[line] = dmw - dmpa * 2;
	bool mode24 = g->displayMode & 0x10;
	if (p->gpuThread) {
		uint32_t w[7] = { (uint32_t) (dest - g->frame), g->curLineY, (uint32_t) xs, (uint32_t) xe, (uint32_t) dmw, (uint32_t) fbx, mode24 };
		queueJob(p, JOB_LINE, w, 7, 0);
	} else {
		linePixels(dest, &g->vram[g->curLineY * 1024], xs, xe, dmw, fbx, mode24);
	}
	g->frameX = DRXBO;
	g->frameW = dmw - dmpa * 2;
	g->frameH = count << interlace;
}

// Whether a timer's IRQ depends on each line's edges (timer 0 or 1 clocked or gated by them).
static bool linesWatched(struct Psx* p) {
	for (int i = 0; i < 2; i++) {
		uint32_t mode = p->timer[i].mode;
		if ((mode & 0x30) && (mode & 0x101)) return true;
	}
	return false;
}

void gpuRun(struct Psx* p) {
	struct Gpu* g = &p->gpu;
	int64_t sys = p->cycles - g->last;
	if (sys <= 0) {
		return;
	}
	g->last = p->cycles;
	g->drawTime += (int32_t) (sys > 1000000 ? 1000000 : sys) << 1;
	if (g->drawTime > 256) g->drawTime = 256;
	processFifo(p);
	g->clockCounter += (uint64_t) sys * g->clockRatio;
	int32_t clocks = (int32_t) (g->clockCounter >> 16);
	g->clockCounter -= (uint64_t) clocks << 16;
	while (clocks > 0) {
		int32_t chunk = clocks;
		if ((uint32_t) chunk > g->lineCounter) chunk = g->lineCounter;
		clocks -= chunk;
		g->lineCounter -= chunk;
		g->dotCounter += chunk;
		uint32_t dots = g->dotCounter / dotRatios[g->displayMode & 3];
		g->dotCounter -= dots * dotRatios[g->displayMode & 3];
		timersDots(p, dots);
		if (g->lineCounter) continue;
		timersRun(p);
		g->linePhase = (g->linePhase + 1) & 1;
		if (g->linePhase) {
			timersHretrace(p, true);
			g->lineCounter = 200;
			timersHblank(p);
			continue;
		}
		timersHretrace(p, false);
		g->lineCounter = g->displayMode & 8 ? 3405 - 200 : 3412 + g->phaseChange - 200;
		g->scanline = (g->scanline + 1) % g->linesPerField;
		g->phaseChange = !g->phaseChange;
		if (g->scanline == g->linesPerField - 1) {
			g->field = g->displayMode & 0x20 ? !g->field : false;
		}
		if (g->scanline == 0) {
			if (g->displayMode & 0x20) {
				g->skip = false;
				g->linesPerField = (g->displayMode & 8 ? 313 : 263) - g->field;
			} else {
				g->field = false;
				g->linesPerField = g->displayMode & 8 ? 314 : 263;
			}
		}
		if (g->scanline == g->vEnd && !g->inVblank) {
			g->inVblank = true;
			g->curYOffset = 0;
			g->fieldReadout = (g->displayMode & 0x24) == 0x24 ? !g->field : false;
			psxIrq(p, IRQ_VBLANK);
			g->frameDone = true;
		}
		if (g->scanline == g->vStart && g->inVblank) {
			g->inVblank = false;
		}
		timersVblank(p, g->inVblank);
		if ((g->displayMode & 0x24) == 0x24) {
			g->curLineY = (g->displayY + (g->curYOffset << 1) + (g->inVblank ? 0 : g->fieldReadout)) & 0x1FF;
		} else {
			g->curLineY = (g->displayY + g->curYOffset) & 0x1FF;
		}
		outputLine(p);
		if (!g->inVblank) {
			g->curYOffset = (g->curYOffset + 1) & 0x1FF;
		}
	}
	// Mednafen wakes the GPU every 128 cycles at most; that's only needed while commands wait for drawing
	// time or timer 0 counts dots. Otherwise the next line edge is the next thing that happens.
	int64_t next = ((((int64_t) g->lineCounter) << 16) - (int64_t) g->clockCounter + g->clockRatio - 1) / g->clockRatio;
	if (p->mode != MODE_EXACT && !linesWatched(p)) {
		// The Fast and Balanced modes wake for the next line where something happens (vblank, the field); the lines
		// before it are caught up then, or when the CPU looks
		int32_t lines = (int32_t) g->linesPerField;
		uint32_t targets[3] = { g->vEnd, g->vStart, 0 };
		for (int i = 0; i < 3; i++) {
			int32_t d = (int32_t) targets[i] - (int32_t) g->scanline;
			if (d <= 0) d += (int32_t) g->linesPerField;
			if (d > 0 && d < lines) lines = d;
		}
		int64_t lineCycles = ((int64_t) (g->displayMode & 8 ? 3405 : 3413) << 16) / g->clockRatio;
		next += (lines - 1) * lineCycles;
	}
	if (p->timer[0].mode & 0x100) {
		if (next > 128) next = 128;
	} else if (g->drawTime < 0 && (g->fifoCount || (g->inCommand & (CMD_QUAD | CMD_PLINE)))) {
		int64_t ready = (1 - (int64_t) g->drawTime) / 2;
		if (next > ready) next = ready;
	}
	if (next < 1) next = 1;
	psxSchedule(p, p->cycles + next);
}

int gpuDisplay(struct Psx* p, uint32_t* out, int* width, int* height) {
	struct Gpu* g = &p->gpu;
	gpuSync(p);
	*width = g->frameW;
	*height = g->frameH;
	for (int y = 0; y < g->frameH; y++) {
		memcpy(out + y * g->frameW, g->frame + y * FB_WIDTH + g->frameX, g->frameW * 4);
	}
	return 0;
}

static void runJob(struct GpuThread* t, int id, const struct GpuJob* j) {
	struct Gpu* g = id ? &t->g1 : &t->g;
	g->displayMode = j->displayMode;
	g->displayY = j->displayY;
	g->fieldReadout = j->fieldReadout;
	const uint32_t* w = j->w;
	switch (j->type) {
	case JOB_COMMAND: executeDraw(g, w, j->cc); break;
	case JOB_QUAD: commandPolygon(g, w, j->cc); break;
	case JOB_POLYLINE: commandLine(g, w, j->cc); break;
	case JOB_POLYLINE_END: g->inCommand = CMD_NONE; break;
	case JOB_VRAM_WRITE:
		for (int i = 0; i < j->len; i++) vramWrite(g, w[i], id == 0);
		break;
	case JOB_LINE:
		if (id) break;
		linePixels(t->frame + w[0], &g->vram[w[1] * 1024], (int32_t) w[2], (int32_t) w[3], (int32_t) w[4], (int32_t) w[5], w[6]); break;
	case JOB_GP1:
		if (j->cc == 0x00) softResetGpu(g);
		else if (j->cc == 0x01) g->inCommand = CMD_NONE;
		else g->texDisableAllowChange = w[0] & 1;
		break;
	}
}
