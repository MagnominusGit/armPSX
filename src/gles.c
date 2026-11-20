/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "gles.h"
#ifdef ARMPSX_VULKAN
#include "vkgl.h"
#endif

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The few EGL names used, so building needs no EGL or GLES headers.
typedef void* EGLDisplay;
typedef void* EGLConfig;
typedef void* EGLContext;
typedef void* EGLSurface;
typedef int32_t EGLint;
typedef unsigned EGLBoolean;
typedef unsigned EGLenum;

enum {
	EGL_NONE = 0x3038, EGL_RENDERABLE_TYPE = 0x3040, EGL_SURFACE_TYPE = 0x3033, EGL_PBUFFER_BIT = 0x0001,
	EGL_OPENGL_ES3_BIT = 0x0040, EGL_OPENGL_ES_API = 0x30A0, EGL_CONTEXT_MAJOR_VERSION = 0x3098,
	EGL_CONTEXT_MINOR_VERSION = 0x30FB, EGL_EXTENSIONS = 0x3055, EGL_WIDTH = 0x3057, EGL_HEIGHT = 0x3056,
	EGL_PLATFORM_SURFACELESS_MESA = 0x31DD, GL_RENDERER = 0x1F01,
};

struct Gles {
	bool vulkan, ownsVulkan;  // GL done with Vulkan, and whether this context started it
	void* egl;
	void* gles;
	EGLDisplay display;
	EGLContext context;
	EGLSurface surface;
	bool external;  // the frontend's context: nothing to make or switch
	char renderer[128];

	EGLDisplay (*eglGetDisplay)(void*);
	EGLDisplay (*eglGetPlatformDisplayEXT)(EGLenum, void*, const EGLint*);
	EGLBoolean (*eglInitialize)(EGLDisplay, EGLint*, EGLint*);
	EGLBoolean (*eglTerminate)(EGLDisplay);
	EGLBoolean (*eglBindAPI)(EGLenum);
	EGLBoolean (*eglChooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
	EGLContext (*eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*);
	EGLSurface (*eglCreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint*);
	EGLBoolean (*eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
	EGLBoolean (*eglDestroyContext)(EGLDisplay, EGLContext);
	EGLBoolean (*eglDestroySurface)(EGLDisplay, EGLSurface);
	const char* (*eglQueryString)(EGLDisplay, EGLint);
	void* (*eglGetProcAddress)(const char*);
	const unsigned char* (*glGetString)(unsigned);
};

static void* (*frontendProc)(const char*);
static bool vulkanContexts;
static char cacheDir[4096];

void glesUseVulkan(bool on) {
	vulkanContexts = on;
}

void glesCacheDir(const char* dir) {
	snprintf(cacheDir, sizeof cacheDir, "%s", dir ? dir : "");
}

const char* glesCacheDirectory(void) {
	return cacheDir;
}

void glesFrontend(void* (*getProc)(const char* name)) {
	frontendProc = getProc;
}

static void* lookup(struct Gles* g, const char* name) {
	if (g->external) return frontendProc(name);
	void* f = g->gles ? dlsym(g->gles, name) : NULL;
	if (!f) f = dlsym(g->egl, name);
	if (!f && g->eglGetProcAddress) f = g->eglGetProcAddress(name);
	return f;
}

#define LOAD(name) (*(void**) &g->name = lookup(g, #name), g->name != NULL)

static bool loadEgl(struct Gles* g) {
	(void) LOAD(eglGetPlatformDisplayEXT);
	return LOAD(eglGetDisplay) && LOAD(eglInitialize) && LOAD(eglTerminate) && LOAD(eglBindAPI) && LOAD(eglChooseConfig) &&
	       LOAD(eglCreateContext) && LOAD(eglCreatePbufferSurface) && LOAD(eglMakeCurrent) && LOAD(eglDestroyContext) &&
	       LOAD(eglDestroySurface) && LOAD(eglQueryString);
}

static bool fail(char* error, size_t size, const char* what) {
	snprintf(error, size, "%s", what);
	return false;
}

// A GLES 3.0 context on the display, current without a window (or with a 1x1 pbuffer).
static bool makeContext(struct Gles* g, EGLDisplay display) {
	g->eglBindAPI(EGL_OPENGL_ES_API);
	const EGLint attributes[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE };
	const EGLint pbufferAttributes[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE };
	const EGLint version[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE };
	EGLConfig config = NULL;
	EGLint count = 0;
	if (!g->eglChooseConfig(display, attributes, &config, 1, &count) || !count) config = NULL;
	EGLContext context = g->eglCreateContext(display, config, NULL, version);
	if (!context) return false;
	if (g->eglMakeCurrent(display, NULL, NULL, context)) {
		g->context = context;
		return true;
	}
	EGLConfig pbufferConfig = NULL;
	const EGLint size[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
	EGLSurface surface = NULL;
	if (g->eglChooseConfig(display, pbufferAttributes, &pbufferConfig, 1, &count) && count &&
	    (surface = g->eglCreatePbufferSurface(display, pbufferConfig, size)) && g->eglMakeCurrent(display, surface, surface, context)) {
		g->context = context;
		g->surface = surface;
		return true;
	}
	if (surface) g->eglDestroySurface(display, surface);
	g->eglDestroyContext(display, context);
	return false;
}

static bool context(struct Gles* g, char* error, size_t errorSize) {
	if (frontendProc) {
		g->external = true;
	} else {
		g->egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
		if (!g->egl) g->egl = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
		if (!g->egl) return fail(error, errorSize, "no libEGL");
		g->gles = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_LOCAL);
		*(void**) &g->eglGetProcAddress = dlsym(g->egl, "eglGetProcAddress");
		if (!loadEgl(g)) return fail(error, errorSize, "incomplete libEGL");
		const char* clientExtensions = g->eglQueryString(NULL, EGL_EXTENSIONS);
		bool surfaceless = g->eglGetPlatformDisplayEXT && clientExtensions && strstr(clientExtensions, "EGL_MESA_platform_surfaceless");
		// The surfaceless platform first (no window system needed), then the default display.
		for (int attempt = surfaceless ? 0 : 1; attempt < 2 && !g->context; attempt++) {
			EGLDisplay display = attempt == 0 ? g->eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL) : g->eglGetDisplay(NULL);
			if (!display || !g->eglInitialize(display, NULL, NULL)) continue;
			if (makeContext(g, display)) g->display = display;
			else g->eglTerminate(display);
		}
		if (!g->context) return fail(error, errorSize, "no GLES 3.0 context");
	}
	if (!LOAD(glGetString)) return fail(error, errorSize, "incomplete GLES");
	const char* renderer = (const char*) g->glGetString(GL_RENDERER);
	snprintf(g->renderer, sizeof g->renderer, "%s", renderer ? renderer : "?");
	return true;
}

struct Gles* glesCreateContext(char* error, size_t errorSize) {
	struct Gles* g = calloc(1, sizeof *g);
	if (!g) return NULL;
#ifdef ARMPSX_VULKAN
	if (vulkanContexts) {
		g->vulkan = true;
		g->ownsVulkan = !vkglActive();
		if (!vkglCreate(NULL, error, errorSize)) {
			free(g);
			return NULL;
		}
		snprintf(g->renderer, sizeof g->renderer, "%s", vkglRenderer());
		return g;
	}
#endif
	if (!context(g, error, errorSize)) {
		glesDestroy(g);
		return NULL;
	}
	return g;
}

void* glesProc(struct Gles* g, const char* name) {
#ifdef ARMPSX_VULKAN
	if (g->vulkan) return vkglProc(name);
#endif
	return lookup(g, name);
}

void glesUse(struct Gles* g) {
	if (!g->external && !g->vulkan) g->eglMakeCurrent(g->display, g->surface, g->surface, g->context);
}

void glesDestroy(struct Gles* g) {
	if (!g) return;
#ifdef ARMPSX_VULKAN
	if (g->vulkan) {
		if (g->ownsVulkan) vkglDestroy();
		free(g);
		return;
	}
#endif
	if (g->context) {
		g->eglMakeCurrent(g->display, NULL, NULL, NULL);
		g->eglDestroyContext(g->display, g->context);
	}
	if (g->surface) g->eglDestroySurface(g->display, g->surface);
	if (g->display) g->eglTerminate(g->display);
	if (g->gles) dlclose(g->gles);
	if (g->egl) dlclose(g->egl);
	free(g);
}

const char* glesRenderer(struct Gles* g) {
	return g->renderer;
}
