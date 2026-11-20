/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A GLES 3.0 context, from armN64's gles.c: libEGL/libGLESv2 loaded at run time (dlopen), without a
 * window, or the frontend's context (libretro's hardware context). */
#pragma once

#include <stdbool.h>
#include <stddef.h>

struct Gles;

// Its own context, current on this thread after; NULL with why in error when there is none.
struct Gles* glesCreateContext(char* error, size_t errorSize);
// From now on, contexts are the frontend's, current already when they are used, with GL functions
// from getProc.
void glesFrontend(void* (*getProc)(const char* name));
void* glesProc(struct Gles*, const char* name);
void glesUse(struct Gles*);
void glesDestroy(struct Gles*);
const char* glesRenderer(struct Gles*);
