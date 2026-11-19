/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The GPU, ported to C from Mednafen's PS1 module (gpu.cpp, gpu_common.inc, gpu_polygon.cpp,
 * gpu_sprite.cpp, gpu_line.cpp; Copyright (C) 2011-2017 Mednafen Team, GPL-2.0-or-later). */
#include <stdlib.h>
#include <string.h>
#include "psx.h"

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

ALWAYS uint16_t modTexel(struct Gpu* g, uint16_t texel, int32_t r, int32_t gr, int32_t b, int dx, int dy) {
	uint16_t out = texel & 0x8000;
	out |= g->ditherLut[dy][dx][((texel & 0x1F) * r) >> 4];
	out |= g->ditherLut[dy][dx][((texel & 0x3E0) * gr) >> 9] << 5;
	out |= g->ditherLut[dy][dx][((texel & 0x7C00) * b) >> 14] << 10;
	return out;
}

ALWAYS void updateClut(struct Gpu* g, int texMode, uint16_t raw) {
	if (texMode < 2) {
		uint32_t tag = (raw & 0x7FFF) | (uint32_t) texMode << 16;
		if (g->clutTag != tag) {
			const uint16_t* line = &g->vram[((raw >> 6) & 0x1FF) * 1024];
			uint32_t cx = (raw & 0x3F) << 4, count = texMode ? 256 : 16;
			g->drawTime -= count;
			for (uint32_t i = 0; i < count; i++) {
				g->clut[i] = line[(cx + i) & 0x3FF];
			}
			g->clutTag = tag;
		}
	}
}

static void recalcWindow(struct Gpu* g) {
	g->window.xAnd = ~(g->tww << 3);
	g->window.xAdd = ((g->twx & g->tww) << 3) + (g->texPageX << (2 - (g->texMode < 2 ? g->texMode : 2)));
	g->window.yAnd = ~(g->twh << 3);
	g->window.yAdd = ((g->twy & g->twh) << 3) + g->texPageY;
}

ALWAYS uint16_t texel(struct Gpu* g, int texMode, uint32_t u, uint32_t v) {
	uint32_t uExt = (u & g->window.xAnd) + g->window.xAdd;
	uint32_t fx = (uExt >> (2 - texMode)) & 1023;
	uint32_t fy = (v & g->window.yAnd) + g->window.yAdd;
	uint32_t gro = fy * 1024 + fx;
	uint32_t index = texMode == 0 ? (((gro >> 2) & 3) | ((gro >> 8) & 0xFC)) : (((gro >> 2) & 7) | ((gro >> 7) & 0xF8));
	struct { uint16_t data[4]; uint32_t tag; }* c = (void*) &g->texCache[index];
	if (c->tag != (gro & ~3u)) {
		g->drawTime -= 4;
		memcpy(c->data, &g->vram[(gro & ~3u) & (512 * 1024 - 1)], 8);
		c->tag = gro & ~3u;
	}
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

ALWAYS void drawSpan(struct Gpu* g, int y, int32_t xStart, int32_t xBound, struct Interp ig, const struct Deltas* d,
                     bool gouraud, bool textured, int blend, bool texMult, int texMode, bool maskEval) {
	if (lineSkip(g, y)) return;
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
	do {
		uint32_t r = ig.r >> (COORD_FBS + COORD_POST_PADDING);
		uint32_t gg = ig.g >> (COORD_FBS + COORD_POST_PADDING);
		uint32_t b = ig.b >> (COORD_FBS + COORD_POST_PADDING);
		if (textured) {
			uint16_t t = texel(g, texMode, ig.u >> (COORD_FBS + COORD_POST_PADDING), ig.v >> (COORD_FBS + COORD_POST_PADDING));
			if (t) {
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

ALWAYS void drawTriangle(struct Gpu* g, struct GpuVertex* v, bool gouraud, bool textured, int blend, bool texMult, int texMode, bool maskEval) {
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
				drawSpan(g, yi, (int32_t) (lc >> 32), (int32_t) (rc >> 32), ig, &d, gouraud, textured, blend, texMult, texMode, maskEval);
			}
		} else {
			while (yi < yb) {
				int32_t y = sx11(yi);
				if (y > g->clipY1) break;
				if (y < g->clipY0) {
					g->drawTime -= 2;
				} else {
					drawSpan(g, yi, (int32_t) (lc >> 32), (int32_t) (rc >> 32), ig, &d, gouraud, textured, blend, texMult, texMode, maskEval);
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

static void triangleTextured(struct Gpu* g, struct GpuVertex* v, bool gouraud, int blend, bool texMult, int texMode, bool maskEval) {
	if (gouraud) DISPATCH_BLEND(DISPATCH_TEXMULT, drawTriangle, g, v, true, true);
	else DISPATCH_BLEND(DISPATCH_TEXMULT, drawTriangle, g, v, false, true);
}

static void triangleUntextured(struct Gpu* g, struct GpuVertex* v, bool gouraud, int blend, bool maskEval) {
	if (gouraud) DISPATCH_BLEND(UNTEXTURED, drawTriangle, g, v, true, false);
	else DISPATCH_BLEND(UNTEXTURED, drawTriangle, g, v, false, false);
}

static void setTpage(struct Gpu* g, uint32_t w) {
	uint32_t x = (w & 0xF) * 64, y = (w & 0x10) * 16, mode = (w >> 7) & 3;
	g->abr = (w >> 5) & 3;
	if (!mode != !g->texMode || x != g->texPageX || y != g->texPageY) {
		invalidateTexCache(g);
	}
	if (g->texDisableAllowChange) {
		bool disable = (w >> 11) & 1;
		if (disable != g->texDisable) invalidateTexCache(g);
		g->texDisable = disable;
	}
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
			if (i == 0) updateClut(g, texMode, (*cb >> 16) & 0xFFFF);
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

ALWAYS void drawSprite(struct Gpu* g, int32_t x0, int32_t y0, int32_t w, int32_t h, uint8_t u0, uint8_t v0, uint32_t color,
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
		if (!lineSkip(g, y)) {
			if (xb > xs) {
				int32_t t = xb - xs;
				if (blend >= 0 || maskEval) t += (((xb + 1) & ~1) - (xs & ~1)) >> 1;
				g->drawTime -= t;
			}
			for (int32_t x = xs; x < xb; x++) {
				if (textured) {
					uint16_t t = texel(g, texMode, ur, v);
					if (t) {
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

#define SPRITE_ARGS g, x, y, w, h, u, v, color
static void spriteTextured(struct Gpu* g, int32_t x, int32_t y, int32_t w, int32_t h, uint8_t u, uint8_t v, uint32_t color, bool flipX, bool flipY,
                           int blend, bool texMult, int texMode, bool maskEval) {
	switch ((flipX ? 1 : 0) | (flipY ? 2 : 0)) {
	case 0: DISPATCH_BLEND(DISPATCH_TEXMULT, drawSprite, SPRITE_ARGS, false, false, true); break;
	case 1: DISPATCH_BLEND(DISPATCH_TEXMULT, drawSprite, SPRITE_ARGS, true, false, true); break;
	case 2: DISPATCH_BLEND(DISPATCH_TEXMULT, drawSprite, SPRITE_ARGS, false, true, true); break;
	default: DISPATCH_BLEND(DISPATCH_TEXMULT, drawSprite, SPRITE_ARGS, true, true, true); break;
	}
}

static void spriteUntextured(struct Gpu* g, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color, int blend, bool maskEval) {
	uint8_t u = 0, v = 0;
	DISPATCH_BLEND(UNTEXTURED, drawSprite, SPRITE_ARGS, false, false, false);
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
		updateClut(g, texMode, (*cb >> 16) & 0xFFFF);
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
			if (x >= g->clipX0 && x <= g->clipX1 && y >= g->clipY0 && y <= g->clipY1) {
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
		for (int32_t x = 0; x < w; x++) {
			g->vram[yy * 1024 + ((x + dx) & 1023)] = value;
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
	for (int32_t y = 0; y < h; y++) {
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

static void execute(struct Psx* p, const uint32_t* cb, uint32_t cc) {
	struct Gpu* g = &p->gpu;
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
		case 0x1F:
			g->irq = true;
			psxIrq(p, IRQ_GPU);
			break;
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

static void processFifo(struct Psx* p) {
	struct Gpu* g = &p->gpu;
	while (g->fifoCount) {
		switch (g->inCommand) {
		case CMD_FBREAD:
			return;
		case CMD_FBWRITE: {
			uint32_t data = fifoPop(g);
			for (int i = 0; i < 2; i++) {
				uint16_t* d = &VRAM(g, g->rwCurX, g->rwCurY);
				if (!(*d & g->maskEval)) *d = (uint16_t) data | g->maskSet;
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
			continue;
		}
		case CMD_PLINE: {
			if (g->drawTime < 0) return;
			uint32_t cc = g->inCommandCode;
			int vl = 1 + ((cc & 0x10) ? 1 : 0);
			if ((fifoPeek(g) & 0xF000F000) == 0x50005000) {
				fifoPop(g);
				g->inCommand = CMD_NONE;
				continue;
			}
			if (g->fifoCount < vl) return;
			uint32_t cb[2];
			for (int i = 0; i < vl; i++) cb[i] = fifoPop(g);
			commandLine(g, cb, cc);
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
		execute(p, cb, cc);
	}
}

// GP0 words go through the FIFO; a full FIFO makes the GPU catch up first, as the DMA would wait.
void gpuGp0(struct Psx* p, uint32_t word) {
	struct Gpu* g = &p->gpu;
	if (g->fifoCount >= 16) {
		int32_t saved = g->drawTime;
		if (g->drawTime < 0) {
			g->drawTime = 0;
		}
		processFifo(p);
		g->drawTime += saved < 0 ? saved : 0;
		if (g->fifoCount >= 32) {
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

static void softReset(struct Psx* p) {
	struct Gpu* g = &p->gpu;
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

void gpuReset(struct Psx* p) {
	struct Gpu* g = &p->gpu;
	static const int8_t ditherTable[4][4] = { { -4, 0, -3, 1 }, { 2, -2, 3, -1 }, { -3, 1, -4, 0 }, { 3, -1, 2, -2 } };
	for (int y = 0; y < 4; y++) {
		for (int x = 0; x < 4; x++) {
			for (int v = 0; v < 512; v++) {
				int value = (v + ditherTable[y][x]) >> 3;
				g->ditherLut[y][x][v] = value < 0 ? 0 : value > 0x1F ? 0x1F : value;
			}
		}
	}
	g->clockRatio = p->pal ? 102948 : 103896;
	memset(g->vram, 0, sizeof g->vram);
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
	case 0x09: g->texDisableAllowChange = v & 1; break;
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

static uint32_t readData(struct Gpu* g) {
	if (g->inCommand == CMD_FBREAD) {
		g->readBufferEx = 0;
		for (int i = 0; i < 2; i++) {
			g->readBufferEx |= (uint32_t) VRAM(g, g->rwCurX, g->rwCurY) << (i * 16);
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
		return readData(g);
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
	const uint16_t* src = &g->vram[g->curLineY * 1024];
	for (int32_t x = 0; x < xs; x++) dest[x] = 0;
	if (g->displayMode & 0x10) {
		for (int32_t x = xs; x < xe; x++) {
			uint32_t s = src[fbx >> 1] | (uint32_t) src[((fbx >> 1) + 1) & 0x7FF] << 16;
			s >>= (fbx & 1) * 8;
			dest[x] = (s & 0xFF) << 16 | (s & 0xFF00) | (s >> 16 & 0xFF);
			fbx = (fbx + 3) & 0x7FF;
		}
	} else {
		for (int32_t x = xs; x < xe; x++) {
			uint32_t s = src[fbx >> 1];
			dest[x] = (s & 0x1F) << 19 | (s & 0x3E0) << 6 | (s & 0x7C00) >> 7;
			fbx = (fbx + 2) & 0x7FF;
		}
	}
	for (int32_t x = xe; x < dmw; x++) dest[x] = 0;
	g->frameX = DRXBO;
	g->frameW = dmw - dmpa * 2;
	g->frameH = count << interlace;
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
			p->hretrace = true;
			g->lineCounter = 200;
			timersHblank(p);
			continue;
		}
		p->hretrace = false;
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
	int64_t next = ((((int64_t) g->lineCounter) << 16) - (int64_t) g->clockCounter + g->clockRatio - 1) / g->clockRatio;
	if (next < 1) next = 1;
	if (next > 128) next = 128;
	psxSchedule(p, p->cycles + next);
}

int gpuDisplay(struct Psx* p, uint32_t* out, int* width, int* height) {
	struct Gpu* g = &p->gpu;
	*width = g->frameW;
	*height = g->frameH;
	for (int y = 0; y < g->frameH; y++) {
		memcpy(out + y * g->frameW, g->frame + y * FB_WIDTH + g->frameX, g->frameW * 4);
	}
	return 0;
}
