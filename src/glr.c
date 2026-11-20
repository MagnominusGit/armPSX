/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "glr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gles.h"

typedef unsigned GLenum;
typedef unsigned GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned GLbitfield;
typedef unsigned char GLboolean;
typedef intptr_t GLsizeiptr;

enum {
	GL_TEXTURE_2D = 0x0DE1, GL_TEXTURE0 = 0x84C0, GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_MAG_FILTER = 0x2800,
	GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803, GL_NEAREST = 0x2600, GL_CLAMP_TO_EDGE = 0x812F,
	GL_RGBA = 0x1908, GL_RGBA8 = 0x8058, GL_UNSIGNED_BYTE = 0x1401,
	GL_UNSIGNED_SHORT = 0x1403, GL_SHORT = 0x1402, GL_UNSIGNED_INT = 0x1405, GL_FRAMEBUFFER = 0x8D40, GL_READ_FRAMEBUFFER = 0x8CA8,
	GL_DRAW_FRAMEBUFFER = 0x8CA9, GL_COLOR_ATTACHMENT0 = 0x8CE0, GL_FRAMEBUFFER_COMPLETE = 0x8CD5,
	GL_COLOR_BUFFER_BIT = 0x4000, GL_SCISSOR_TEST = 0x0C11, GL_BLEND = 0x0BE2, GL_FUNC_ADD = 0x8006,
	GL_FUNC_REVERSE_SUBTRACT = 0x800B, GL_ZERO = 0, GL_ONE = 1, GL_DST_ALPHA = 0x0304, GL_ONE_MINUS_DST_ALPHA = 0x0305,
	GL_CONSTANT_COLOR = 0x8001, GL_ARRAY_BUFFER = 0x8892, GL_STREAM_DRAW = 0x88E0, GL_TRIANGLES = 0x0004,
	GL_VERTEX_SHADER = 0x8B31, GL_FRAGMENT_SHADER = 0x8B30, GL_COMPILE_STATUS = 0x8B81, GL_LINK_STATUS = 0x8B82,
	GL_PACK_ALIGNMENT = 0x0D05, GL_UNPACK_ALIGNMENT = 0x0CF5,
};

struct Gl {
	void (*GenTextures)(GLsizei, GLuint*);
	void (*DeleteTextures)(GLsizei, const GLuint*);
	void (*BindTexture)(GLenum, GLuint);
	void (*ActiveTexture)(GLenum);
	void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
	void (*TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*);
	void (*TexParameteri)(GLenum, GLenum, GLint);
	void (*GenFramebuffers)(GLsizei, GLuint*);
	void (*DeleteFramebuffers)(GLsizei, const GLuint*);
	void (*BindFramebuffer)(GLenum, GLuint);
	void (*FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
	GLenum (*CheckFramebufferStatus)(GLenum);
	void (*BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
	void (*Viewport)(GLint, GLint, GLsizei, GLsizei);
	void (*Scissor)(GLint, GLint, GLsizei, GLsizei);
	void (*Enable)(GLenum);
	void (*Disable)(GLenum);
	void (*ClearColor)(float, float, float, float);
	void (*Clear)(GLbitfield);
	void (*BlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum);
	void (*BlendEquationSeparate)(GLenum, GLenum);
	void (*BlendColor)(float, float, float, float);
	GLuint (*CreateShader)(GLenum);
	void (*ShaderSource)(GLuint, GLsizei, const char* const*, const GLint*);
	void (*CompileShader)(GLuint);
	void (*GetShaderiv)(GLuint, GLenum, GLint*);
	void (*GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, char*);
	void (*DeleteShader)(GLuint);
	GLuint (*CreateProgram)(void);
	void (*AttachShader)(GLuint, GLuint);
	void (*LinkProgram)(GLuint);
	void (*GetProgramiv)(GLuint, GLenum, GLint*);
	void (*GetProgramInfoLog)(GLuint, GLsizei, GLsizei*, char*);
	void (*UseProgram)(GLuint);
	void (*DeleteProgram)(GLuint);
	GLint (*GetUniformLocation)(GLuint, const char*);
	void (*Uniform1i)(GLint, GLint);
	void (*Uniform4iv)(GLint, GLsizei, const GLint*);
	void (*Uniform1f)(GLint, float);
	void (*GenVertexArrays)(GLsizei, GLuint*);
	void (*DeleteVertexArrays)(GLsizei, const GLuint*);
	void (*BindVertexArray)(GLuint);
	void (*GenBuffers)(GLsizei, GLuint*);
	void (*DeleteBuffers)(GLsizei, const GLuint*);
	void (*BindBuffer)(GLenum, GLuint);
	void (*BufferData)(GLenum, GLsizeiptr, const void*, GLenum);
	void (*VertexAttribIPointer)(GLuint, GLint, GLenum, GLsizei, const void*);
	void (*EnableVertexAttribArray)(GLuint);
	void (*DrawArrays)(GLenum, GLint, GLsizei);
	void (*PixelStorei)(GLenum, GLint);
	void (*ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
};

// A vertex: position in 1/16 VRAM pixels; the texture's page, CLUT and window (u and v: mask |
// offset << 5); flags; and the primitive's planes (u, v, red, green, blue: bases, then x steps, then
// y steps), the same at its every vertex.
struct Vertex {
	int16_t x, y;
	uint16_t page, clut, winU, winV, flags;
	uint32_t plane[16];
};

enum { F_TEXTURED = 1, F_RAW = 2, F_DITHER = 4, F_MASK = 8, F_OPAQUE_TEXELS = 16, F_SEMI_TEXELS = 32, F_SCALED = 64, F_SKIP = 128, F_SKIP_ODD = 256, F_QUARTER = 512 };
// Blending: none, the four semi-transparent modes, keeping masked pixels (the mask test without
// semi-transparency), and the modes with the mask test (BLEND_MASKED + mode; not mode 0).
enum { BLEND_NONE = -1, BLEND_KEEP_MASKED = 4, BLEND_MASKED = 5 };

struct Run {
	int first, count, blend;
	int32_t clip[4];
};

enum { VERTICES = 65536, RUNS = 4096, TILE_W = 64, TILE_H = 16 };

// VRAM's 5-bit channels are kept as value * 8 in 8 bits, as the picture shows them: blending such
// values (halves, sums, differences, quarters) gives the GPU's results. Colors go out a quarter step
// over, so a conversion to 8 bits that truncates lands on the same value, and two steps over where
// they are halved by the blend constant, which drivers keep in 8 bits (127 or 128 / 255).
struct Glr {
	struct Gles* gles;
	struct Gl gl;
	int scale;
	GLuint target, targetFb, sample, sampleFb, words, picture, pictureFb;
	int pictureW, pictureH;
	GLuint draw, blit, vao, buffer;
	GLint drawScale, drawZoom, drawVram, blitWords, blitVram, blitScale, blitKind, blitMask, blitRect;
	struct Vertex* vertices;
	int vertexCount;
	uint32_t* staging;  // CPU words on their way up, in VRAM's colors
	struct Run runs[RUNS];
	int runCount;
	// VRAM tiles drawn since the sample copy was made
	uint32_t dirty[512 / TILE_H];
};

static const char* drawVertex =
	"#version 300 es\n"
	"layout(location = 0) in ivec2 aPos;\n"
	"layout(location = 1) in uvec4 aInfo;\n"
	"layout(location = 2) in uint aFlags;\n"
	"layout(location = 3) in uvec4 aPlane0;\n"
	"layout(location = 4) in uvec4 aPlane1;\n"
	"layout(location = 5) in uvec4 aPlane2;\n"
	"layout(location = 6) in uvec4 aPlane3;\n"
	"uniform float uZoom;\n"
	"flat out uvec4 vInfo;\n"
	"flat out uvec4 vPlane0;\n"
	"flat out uvec4 vPlane1;\n"
	"flat out uvec4 vPlane2;\n"
	"flat out uvec4 vPlane3;\n"
	"flat out uint vFlags;\n"
	"void main() {\n"
	// Pixels are sampled at their top-left corners, as the GPU does
	"	vec2 p = (vec2(aPos) / 16.0 * uZoom + 0.5) / (vec2(1024.0, 512.0) * uZoom) * 2.0 - 1.0;\n"
	"	gl_Position = vec4(p, 0.0, 1.0);\n"
	"	vInfo = aInfo;\n"
	"	vFlags = aFlags;\n"
	"	vPlane0 = aPlane0;\n"
	"	vPlane1 = aPlane1;\n"
	"	vPlane2 = aPlane2;\n"
	"	vPlane3 = aPlane3;\n"
	"}\n";

static const char* drawFragment =
	"#version 300 es\n"
	"precision highp float;\n"
	"precision highp int;\n"
	"uniform highp sampler2D uVram;\n"
	"uniform int uScale;\n"
	"flat in uvec4 vInfo;\n"
	"flat in uvec4 vPlane0;\n"
	"flat in uvec4 vPlane1;\n"
	"flat in uvec4 vPlane2;\n"
	"flat in uvec4 vPlane3;\n"
	"flat in uint vFlags;\n"
	"out vec4 oColor;\n"
	"const int dither[16] = int[16](-4, 0, -3, 1, 2, -2, 3, -1, -3, 1, -4, 0, 3, -1, 2, -2);\n"
	"int word(int x, int y) {\n"
	"	vec4 c = texelFetch(uVram, ivec2((x & 1023) * uScale + uScale / 2, (y & 511) * uScale + uScale / 2), 0);\n"
	"	ivec4 q = ivec4(round(c * 255.0)) >> 3;\n"
	"	return q.r | q.g << 5 | q.b << 10 | (q.a >> 4) << 15;\n"
	"}\n"
	// A plane's value at VRAM pixel p (and, scaled up, the fraction sub / uScale past it)
	"int at(uint base, uint dx, uint dy, ivec2 p, ivec2 sub) {\n"
	"	uint v = base + dx * uint(p.x) + dy * uint(p.y) + uint(int(dx) / uScale * sub.x + int(dy) / uScale * sub.y);\n"
	"	return int(v >> 24);\n"
	"}\n"
	"void main() {\n"
	"	ivec2 f = ivec2(gl_FragCoord.xy), p = f / uScale, sub = f - p * uScale;\n"
	"	int flags = int(vFlags);\n"
	"	if ((flags & 128) != 0 && (p.y & 1) == ((flags >> 8) & 1)) discard;\n"
	"	int d = (flags & 4) != 0 ? dither[(p.y & 3) * 4 + (p.x & 3)] : 0;\n"
	"	ivec3 c = ivec3(at(vPlane0.z, vPlane1.w, vPlane3.x, p, sub), at(vPlane0.w, vPlane2.x, vPlane3.y, p, sub), at(vPlane1.x, vPlane2.y, vPlane3.z, p, sub));\n"
	"	int mask = (flags >> 3) & 1;\n"
	"	ivec3 o;\n"
	"	if ((flags & 1) != 0) {\n"
	"		ivec2 uv = ivec2(at(vPlane0.x, vPlane1.y, vPlane2.z, p, sub), at(vPlane0.y, vPlane1.z, vPlane2.w, p, sub));\n"
	"		int wu = int(vInfo.z), wv = int(vInfo.w);\n"
	"		uv.x = (uv.x & ~((wu & 31) << 3)) | (((wu >> 5) & wu & 31) << 3);\n"
	"		uv.y = (uv.y & ~((wv & 31) << 3)) | (((wv >> 5) & wv & 31) << 3);\n"
	"		int page = int(vInfo.x), clut = int(vInfo.y);\n"
	"		int px = (page & 15) * 64, py = ((page >> 4) & 1) * 256, mode = (page >> 5) & 3;\n"
	"		int cx = (clut & 63) * 16, cy = (clut >> 6) & 511;\n"
	"		int t;\n"
	"		if (mode == 0) {\n"
	"			int w = word(px + (uv.x >> 2), py + uv.y);\n"
	"			t = word(cx + ((w >> ((uv.x & 3) * 4)) & 15), cy);\n"
	"		} else if (mode == 1) {\n"
	"			int w = word(px + (uv.x >> 1), py + uv.y);\n"
	"			t = word(cx + ((w >> ((uv.x & 1) * 8)) & 255), cy);\n"
	"		} else {\n"
	"			t = word(px + uv.x, py + uv.y);\n"
	"		}\n"
	"		bool semi = (t & 0x8000) != 0;\n"
	"		if (t == 0 || ((flags & 16) != 0 && semi) || ((flags & 32) != 0 && !semi)) discard;\n"
	"		ivec3 tc = ivec3(t, t >> 5, t >> 10) & 31;\n"
	"		o = (flags & 2) != 0 ? tc : clamp((((tc * c) >> 4) + d) >> 3, 0, 31);\n"
	"		mask |= t >> 15;\n"
	"	} else {\n"
	"		o = clamp((c + d) >> 3, 0, 31);\n"
	"	}\n"
	"	vec3 value = (flags & 512) != 0 ? vec3(o) * 2.0 + 0.25 : vec3(o) * 8.0 + ((flags & 64) != 0 ? 2.0 : 0.25);\n"
	"	oColor = vec4(value / 255.0, float(mask));\n"
	"}\n";

static const char* blitVertex =
	"#version 300 es\n"
	"void main() {\n"
	"	vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
	"	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
	"}\n";

// Kinds: 0 the CPU's words (uWords: VRAM-sized, in VRAM's colors) into VRAM, 1 a copy within VRAM, 2
// the 15-bit picture, 3 the 24-bit one.
static const char* blitFragment =
	"#version 300 es\n"
	"precision highp float;\n"
	"precision highp int;\n"
	"uniform highp sampler2D uWords;\n"
	"uniform highp sampler2D uVram;\n"
	"uniform int uScale, uKind, uMask;\n"
	"uniform ivec4 uRect;\n"
	"out vec4 oColor;\n"
	"int word(int x, int y) {\n"
	"	vec4 c = texelFetch(uVram, ivec2((x & 1023) * uScale + uScale / 2, (y & 511) * uScale + uScale / 2), 0);\n"
	"	ivec4 q = ivec4(round(c * 255.0)) >> 3;\n"
	"	return q.r | q.g << 5 | q.b << 10 | (q.a >> 4) << 15;\n"
	"}\n"
	"void main() {\n"
	"	ivec2 d = ivec2(gl_FragCoord.xy), uOrigin = uRect.xy, uSrc = uRect.zw;\n"
	"	ivec2 size = ivec2(1024, 512) * uScale;\n"
	"	if (uKind == 0) {\n"
	"		vec4 w = texelFetch(uWords, d / uScale, 0);\n"
	"		oColor = vec4(w.rgb, max(w.a, float(uMask)));\n"
	"	} else if (uKind == 1) {\n"
	"		ivec2 s = (uSrc * uScale + d - uOrigin) % size;\n"
	"		vec4 c = texelFetch(uVram, s, 0);\n"
	"		oColor = vec4(c.rgb, max(c.a, float(uMask)));\n"
	"	} else if (uKind == 2) {\n"
	"		oColor = vec4(texelFetch(uVram, (uSrc * uScale + d - uOrigin) % size, 0).rgb, 1.0);\n"
	"	} else {\n"
	"		ivec2 r = (d - uOrigin) / uScale;\n"
	"		int byte = uSrc.x * 2 + r.x * 3, y = uSrc.y + r.y;\n"
	"		int v = (word(byte >> 1, y) | word((byte >> 1) + 1, y) << 16) >> ((byte & 1) * 8);\n"
	"		oColor = vec4(vec3(ivec3(v, v >> 8, v >> 16) & 255) / 255.0, 1.0);\n"
	"	}\n"
	"}\n";

static GLuint shader(struct Gl* gl, GLenum type, const char* source, char* error, size_t errorSize) {
	GLuint s = gl->CreateShader(type);
	gl->ShaderSource(s, 1, &source, NULL);
	gl->CompileShader(s);
	GLint ok = 0;
	gl->GetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512] = "";
		gl->GetShaderInfoLog(s, sizeof log, NULL, log);
		snprintf(error, errorSize, "shader: %s", log);
		gl->DeleteShader(s);
		return 0;
	}
	return s;
}

static GLuint program(struct Gl* gl, const char* vertex, const char* fragment, char* error, size_t errorSize) {
	GLuint vs = shader(gl, GL_VERTEX_SHADER, vertex, error, errorSize);
	GLuint fs = vs ? shader(gl, GL_FRAGMENT_SHADER, fragment, error, errorSize) : 0;
	if (!fs) {
		if (vs) gl->DeleteShader(vs);
		return 0;
	}
	GLuint p = gl->CreateProgram();
	gl->AttachShader(p, vs);
	gl->AttachShader(p, fs);
	gl->LinkProgram(p);
	gl->DeleteShader(vs);
	gl->DeleteShader(fs);
	GLint ok = 0;
	gl->GetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[512] = "";
		gl->GetProgramInfoLog(p, sizeof log, NULL, log);
		snprintf(error, errorSize, "program: %s", log);
		gl->DeleteProgram(p);
		return 0;
	}
	return p;
}

#define LOAD(name) (*(void**) &r->gl.name = glesProc(r->gles, "gl" #name), r->gl.name != NULL)

static bool load(struct Glr* r) {
	return LOAD(GenTextures) && LOAD(DeleteTextures) && LOAD(BindTexture) && LOAD(ActiveTexture) && LOAD(TexImage2D) &&
	       LOAD(TexSubImage2D) && LOAD(TexParameteri) && LOAD(GenFramebuffers) && LOAD(DeleteFramebuffers) &&
	       LOAD(BindFramebuffer) && LOAD(FramebufferTexture2D) && LOAD(CheckFramebufferStatus) && LOAD(BlitFramebuffer) &&
	       LOAD(Viewport) && LOAD(Scissor) && LOAD(Enable) && LOAD(Disable) && LOAD(ClearColor) && LOAD(Clear) &&
	       LOAD(BlendFuncSeparate) && LOAD(BlendEquationSeparate) && LOAD(BlendColor) && LOAD(CreateShader) &&
	       LOAD(ShaderSource) && LOAD(CompileShader) && LOAD(GetShaderiv) && LOAD(GetShaderInfoLog) && LOAD(DeleteShader) &&
	       LOAD(CreateProgram) && LOAD(AttachShader) && LOAD(LinkProgram) && LOAD(GetProgramiv) && LOAD(GetProgramInfoLog) &&
	       LOAD(UseProgram) && LOAD(DeleteProgram) && LOAD(GetUniformLocation) && LOAD(Uniform1i) && LOAD(Uniform4iv) &&
	       LOAD(Uniform1f) && LOAD(GenVertexArrays) && LOAD(DeleteVertexArrays) && LOAD(BindVertexArray) &&
	       LOAD(GenBuffers) && LOAD(DeleteBuffers) && LOAD(BindBuffer) && LOAD(BufferData) && LOAD(VertexAttribIPointer) &&
	       LOAD(EnableVertexAttribArray) && LOAD(DrawArrays) && LOAD(PixelStorei) && LOAD(ReadPixels);
}

static GLuint texture(struct Gl* gl, GLint internal, int w, int h, GLenum format, GLenum type) {
	GLuint t;
	gl->GenTextures(1, &t);
	gl->BindTexture(GL_TEXTURE_2D, t);
	gl->TexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, format, type, NULL);
	gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	return t;
}

static GLuint framebuffer(struct Gl* gl, GLuint t) {
	GLuint f;
	gl->GenFramebuffers(1, &f);
	gl->BindFramebuffer(GL_FRAMEBUFFER, f);
	gl->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
	return f;
}

struct Glr* glrCreate(struct Gles* gles, int scale, char* error, size_t errorSize) {
	struct Glr* r = calloc(1, sizeof *r);
	if (!r) return NULL;
	r->gles = gles;
	r->scale = scale < 1 ? 1 : scale > 16 ? 16 : scale;
	r->vertices = malloc(VERTICES * sizeof *r->vertices);
	r->staging = malloc(1024 * 512 * 4);
	if (!r->vertices || !r->staging || !load(r)) {
		snprintf(error, errorSize, "incomplete GLES 3.0");
		glrDestroy(r);
		return NULL;
	}
	struct Gl* gl = &r->gl;
	r->draw = program(gl, drawVertex, drawFragment, error, errorSize);
	r->blit = r->draw ? program(gl, blitVertex, blitFragment, error, errorSize) : 0;
	if (!r->blit) {
		glrDestroy(r);
		return NULL;
	}
	int w = 1024 * r->scale, h = 512 * r->scale;
	r->target = texture(gl, GL_RGBA8, w, h, GL_RGBA, GL_UNSIGNED_BYTE);
	r->targetFb = framebuffer(gl, r->target);
	r->sample = texture(gl, GL_RGBA8, w, h, GL_RGBA, GL_UNSIGNED_BYTE);
	r->sampleFb = framebuffer(gl, r->sample);
	if (gl->CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		snprintf(error, errorSize, "no %dx%d framebuffer", w, h);
		glrDestroy(r);
		return NULL;
	}
	r->words = texture(gl, GL_RGBA8, 1024, 512, GL_RGBA, GL_UNSIGNED_BYTE);
	gl->ClearColor(0, 0, 0, 0);
	gl->BindFramebuffer(GL_FRAMEBUFFER, r->targetFb);
	gl->Clear(GL_COLOR_BUFFER_BIT);
	gl->BindFramebuffer(GL_FRAMEBUFFER, r->sampleFb);
	gl->Clear(GL_COLOR_BUFFER_BIT);

	r->drawScale = gl->GetUniformLocation(r->draw, "uScale");
	r->drawZoom = gl->GetUniformLocation(r->draw, "uZoom");
	r->drawVram = gl->GetUniformLocation(r->draw, "uVram");
	r->blitWords = gl->GetUniformLocation(r->blit, "uWords");
	r->blitVram = gl->GetUniformLocation(r->blit, "uVram");
	r->blitScale = gl->GetUniformLocation(r->blit, "uScale");
	r->blitKind = gl->GetUniformLocation(r->blit, "uKind");
	r->blitMask = gl->GetUniformLocation(r->blit, "uMask");
	r->blitRect = gl->GetUniformLocation(r->blit, "uRect");

	gl->GenVertexArrays(1, &r->vao);
	gl->BindVertexArray(r->vao);
	gl->GenBuffers(1, &r->buffer);
	gl->BindBuffer(GL_ARRAY_BUFFER, r->buffer);
	GLsizei stride = sizeof(struct Vertex);
	gl->VertexAttribIPointer(0, 2, GL_SHORT, stride, (void*) offsetof(struct Vertex, x));
	gl->VertexAttribIPointer(1, 4, GL_UNSIGNED_SHORT, stride, (void*) offsetof(struct Vertex, page));
	gl->VertexAttribIPointer(2, 1, GL_UNSIGNED_SHORT, stride, (void*) offsetof(struct Vertex, flags));
	for (int i = 0; i < 4; i++) gl->VertexAttribIPointer(3 + i, 4, GL_UNSIGNED_INT, stride, (void*) (offsetof(struct Vertex, plane) + i * 16));
	for (int i = 0; i < 7; i++) gl->EnableVertexAttribArray(i);
	return r;
}

void glrDestroy(struct Glr* r) {
	if (!r) return;
	struct Gl* gl = &r->gl;
	if (gl->DeleteProgram) {
		GLuint textures[] = { r->target, r->sample, r->words, r->picture };
		GLuint framebuffers[] = { r->targetFb, r->sampleFb, r->pictureFb };
		gl->DeleteTextures(4, textures);
		gl->DeleteFramebuffers(3, framebuffers);
		if (r->buffer) gl->DeleteBuffers(1, &r->buffer);
		if (r->vao) gl->DeleteVertexArrays(1, &r->vao);
		if (r->draw) gl->DeleteProgram(r->draw);
		if (r->blit) gl->DeleteProgram(r->blit);
	}
	free(r->vertices);
	free(r->staging);
	free(r);
}

// Dirty tiles: VRAM drawn since the sample copy (what textures are read from) was made.

static void markDirty(struct Glr* r, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > 1024) x1 = 1024;
	if (y1 > 512) y1 = 512;
	if (x1 <= x0 || y1 <= y0) return;
	uint32_t bits = (uint32_t) ((1ull << ((x1 + TILE_W - 1) / TILE_W)) - (1ull << (x0 / TILE_W)));
	for (int32_t ty = y0 / TILE_H; ty < (y1 + TILE_H - 1) / TILE_H; ty++) r->dirty[ty] |= bits;
}

// Whether a rectangle (which may wrap around VRAM's edges) touches a dirty tile.
static bool isDirty(struct Glr* r, int32_t x, int32_t y, int32_t w, int32_t h) {
	for (int32_t ty = 0; ty < (h + TILE_H - 1) / TILE_H + 1; ty++) {
		uint32_t row = r->dirty[(((y & 511) / TILE_H) + ty) & (512 / TILE_H - 1)];
		if (!row) continue;
		for (int32_t tx = 0; tx < (w + TILE_W - 1) / TILE_W + 1; tx++) {
			if (row >> ((((x & 1023) / TILE_W) + tx) & 15) & 1) return true;
		}
	}
	return false;
}

static void flush(struct Glr* r);

// The sample copy brought up to date with what was drawn.
static void syncSample(struct Glr* r) {
	struct Gl* gl = &r->gl;
	int32_t x0 = 16, x1 = -1, y0 = 32, y1 = -1;
	for (int ty = 0; ty < 512 / TILE_H; ty++) {
		if (!r->dirty[ty]) continue;
		if (ty < y0) y0 = ty;
		y1 = ty;
		for (int tx = 0; tx < 16; tx++) {
			if (r->dirty[ty] >> tx & 1) {
				if (tx < x0) x0 = tx;
				if (tx > x1) x1 = tx;
			}
		}
	}
	if (y1 < 0) return;
	memset(r->dirty, 0, sizeof r->dirty);
	int s = r->scale;
	int bx0 = x0 * TILE_W * s, by0 = y0 * TILE_H * s, bx1 = (x1 + 1) * TILE_W * s, by1 = (y1 + 1) * TILE_H * s;
	gl->Disable(GL_SCISSOR_TEST);
	gl->BindFramebuffer(GL_READ_FRAMEBUFFER, r->targetFb);
	gl->BindFramebuffer(GL_DRAW_FRAMEBUFFER, r->sampleFb);
	gl->BlitFramebuffer(bx0, by0, bx1, by1, bx0, by0, bx1, by1, GL_COLOR_BUFFER_BIT, GL_NEAREST);
}

static void needSample(struct Glr* r, int32_t x, int32_t y, int32_t w, int32_t h) {
	if (isDirty(r, x, y, w, h)) {
		flush(r);
		syncSample(r);
	}
}

static void setBlend(struct Gl* gl, int blend) {
	if (blend == BLEND_NONE) {
		gl->Disable(GL_BLEND);
		return;
	}
	gl->Enable(GL_BLEND);
	// The mask bit (alpha) is written as drawn, except over a kept pixel
	switch (blend) {
	case 0:
		gl->BlendColor(0.5f, 0.5f, 0.5f, 0.5f);
		gl->BlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
		gl->BlendFuncSeparate(GL_CONSTANT_COLOR, GL_CONSTANT_COLOR, GL_ONE, GL_ZERO);
		break;
	case 1:
		gl->BlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
		gl->BlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ZERO);
		break;
	case 2:
		gl->BlendEquationSeparate(GL_FUNC_REVERSE_SUBTRACT, GL_FUNC_ADD);
		gl->BlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ZERO);
		break;
	case 3:  // the shader gives a quarter of the color
		gl->BlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
		gl->BlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ZERO);
		break;
	// With the mask test: what is drawn is added (subtracted) where the mask bit is clear
	case BLEND_MASKED + 1:
	case BLEND_MASKED + 3:
		gl->BlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
		gl->BlendFuncSeparate(GL_ONE_MINUS_DST_ALPHA, GL_ONE, GL_ONE_MINUS_DST_ALPHA, GL_ONE);
		break;
	case BLEND_MASKED + 2:
		gl->BlendEquationSeparate(GL_FUNC_REVERSE_SUBTRACT, GL_FUNC_ADD);
		gl->BlendFuncSeparate(GL_ONE_MINUS_DST_ALPHA, GL_ONE, GL_ONE_MINUS_DST_ALPHA, GL_ONE);
		break;
	default:
		gl->BlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
		gl->BlendFuncSeparate(GL_ONE_MINUS_DST_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_DST_ALPHA);
		break;
	}
}

static void flush(struct Glr* r) {
	if (!r->vertexCount) return;
	struct Gl* gl = &r->gl;
	int s = r->scale;
	gl->BindFramebuffer(GL_FRAMEBUFFER, r->targetFb);
	gl->Viewport(0, 0, 1024 * s, 512 * s);
	gl->UseProgram(r->draw);
	gl->Uniform1f(r->drawZoom, (float) s);
	gl->Uniform1i(r->drawScale, s);
	gl->Uniform1i(r->drawVram, 0);
	gl->ActiveTexture(GL_TEXTURE0);
	gl->BindTexture(GL_TEXTURE_2D, r->sample);
	gl->BindVertexArray(r->vao);
	gl->BindBuffer(GL_ARRAY_BUFFER, r->buffer);
	gl->BufferData(GL_ARRAY_BUFFER, (GLsizeiptr) (r->vertexCount * sizeof(struct Vertex)), r->vertices, GL_STREAM_DRAW);
	gl->Enable(GL_SCISSOR_TEST);
	int blend = -2;
	for (int i = 0; i < r->runCount; i++) {
		const struct Run* run = &r->runs[i];
		if (run->blend != blend) setBlend(gl, blend = run->blend);
		gl->Scissor(run->clip[0] * s, run->clip[1] * s, (run->clip[2] - run->clip[0]) * s, (run->clip[3] - run->clip[1]) * s);
		gl->DrawArrays(GL_TRIANGLES, run->first, run->count);
	}
	gl->Disable(GL_BLEND);
	gl->Disable(GL_SCISSOR_TEST);
	r->vertexCount = 0;
	r->runCount = 0;
}

// Room for n vertices drawn with blend inside the clip rectangle (x0, y0, x1, y1 exclusive).
static struct Vertex* add(struct Glr* r, int n, int blend, const int32_t clip[4]) {
	if (r->vertexCount + n > VERTICES || r->runCount == RUNS) flush(r);
	struct Run* last = r->runCount ? &r->runs[r->runCount - 1] : NULL;
	if (!last || last->blend != blend || memcmp(last->clip, clip, sizeof last->clip)) {
		last = &r->runs[r->runCount++];
		last->first = r->vertexCount;
		last->count = 0;
		last->blend = blend;
		memcpy(last->clip, clip, sizeof last->clip);
	}
	last->count += n;
	struct Vertex* v = &r->vertices[r->vertexCount];
	r->vertexCount += n;
	return v;
}

FILE* glrLogFile;

static void clipOf(const struct GlrState* s, int32_t clip[4]) {
	clip[0] = s->clipX0;
	clip[1] = s->clipY0;
	clip[2] = s->clipX1 + 1;
	clip[3] = s->clipY1 + 1;
}

static void texInfo(const struct GlrState* s, struct Vertex* v) {
	v->page = (uint16_t) ((s->texPageX / 64) | (s->texPageY / 256) << 4 | s->texMode << 5);
	v->clut = (uint16_t) s->clut;
	v->winU = (uint16_t) (s->tww | s->twx << 5);
	v->winV = (uint16_t) (s->twh | s->twy << 5);
}

// The texture's page and CLUT are read from the sample copy: brought up to date first if drawn on.
static void needTexture(struct Glr* r, const struct GlrState* s) {
	needSample(r, (int32_t) s->texPageX, (int32_t) s->texPageY, 64 << s->texMode, 256);
	if (s->texMode < 2) needSample(r, (int32_t) (s->clut & 63) * 16, (int32_t) (s->clut >> 6) & 511, s->texMode ? 256 : 16, 1);
}

// Semi-transparent textured primitives go twice: opaque texels unblended, then the semi-transparent
// ones blended. Without semi-transparency, the mask test keeps masked pixels.
static void emit(struct Glr* r, const struct GlrState* s, const struct Vertex* v, int n, bool textured, int32_t bx0, int32_t by0,
                 int32_t bx1, int32_t by1) {
	int32_t clip[4];
	clipOf(s, clip);
	if (glrLogFile) {
		fprintf(glrLogFile, "prim n=%d bbox %d,%d-%d,%d flags %x blend %d maskEval %d page %d,%d mode %u clut %x win %u,%u,%u,%u clip %d,%d-%d,%d\n", n, bx0, by0, bx1, by1,
		        v[0].flags, s->blend, s->maskEval, s->texPageX, s->texPageY, s->texMode, s->clut, s->tww, s->twh, s->twx, s->twy,
		        clip[0], clip[1], clip[2], clip[3]);
		for (int i = 0; i < n; i++) {
			fprintf(glrLogFile, "  v %d,%d planes", v[i].x / 16, v[i].y / 16);
			for (int k = 0; k < 15; k++) fprintf(glrLogFile, " %08x", v[i].plane[k]);
			fprintf(glrLogFile, "\n");
		}
	}
	int opaque = s->maskEval ? BLEND_KEEP_MASKED : BLEND_NONE;
	struct Vertex skipped[6];
	if (s->skip >= 0) {
		memcpy(skipped, v, (size_t) n * sizeof *v);
		for (int i = 0; i < n; i++) skipped[i].flags |= F_SKIP | (s->skip ? F_SKIP_ODD : 0);
		v = skipped;
	}
	if (s->blend < 0) {
		memcpy(add(r, n, opaque, clip), v, (size_t) n * sizeof *v);
	} else {
		// Mode 0 with the mask test isn't done: it blends without
		int blend = s->maskEval && s->blend ? BLEND_MASKED + s->blend : s->blend;
		uint16_t semi = (uint16_t) ((textured ? F_SEMI_TEXELS : 0) | (s->blend == 0 ? F_SCALED : 0) | (s->blend == 3 ? F_QUARTER : 0));
		if (textured) {
			struct Vertex* o = add(r, n, opaque, clip);
			memcpy(o, v, (size_t) n * sizeof *v);
			for (int i = 0; i < n; i++) o[i].flags |= F_OPAQUE_TEXELS;
		}
		struct Vertex* t = add(r, n, blend, clip);
		memcpy(t, v, (size_t) n * sizeof *v);
		for (int i = 0; i < n; i++) t[i].flags |= semi;
	}
	if (bx0 < clip[0]) bx0 = clip[0];
	if (by0 < clip[1]) by0 = clip[1];
	if (bx1 > clip[2]) bx1 = clip[2];
	if (by1 > clip[3]) by1 = clip[3];
	markDirty(r, bx0, by0, bx1, by1);
}

static void setPlanes(struct Vertex* v, const struct GlrPlane planes[5]) {
	for (int i = 0; i < 5; i++) {
		v->plane[i] = planes[i].base;
		v->plane[5 + i] = planes[i].dx;
		v->plane[10 + i] = planes[i].dy;
	}
	v->plane[15] = 0;
}

void glrTriangle(struct Glr* r, const struct GlrState* s, const int32_t xy[3][2], const struct GlrPlane planes[5], bool textured, bool raw,
                 bool dither) {
	if (textured) needTexture(r, s);
	struct Vertex v[3];
	uint16_t flags = (uint16_t) ((textured ? F_TEXTURED : 0) | (raw ? F_RAW : 0) | (dither ? F_DITHER : 0) | (s->maskSet ? F_MASK : 0));
	int32_t x0 = xy[0][0], x1 = xy[0][0], y0 = xy[0][1], y1 = xy[0][1];
	for (int i = 0; i < 3; i++) {
		v[i].x = (int16_t) (xy[i][0] * 16);
		v[i].y = (int16_t) (xy[i][1] * 16);
		v[i].flags = flags;
		texInfo(s, &v[i]);
		setPlanes(&v[i], planes);
		if (xy[i][0] < x0) x0 = xy[i][0];
		if (xy[i][0] > x1) x1 = xy[i][0];
		if (xy[i][1] < y0) y0 = xy[i][1];
		if (xy[i][1] > y1) y1 = xy[i][1];
	}
	emit(r, s, v, 3, textured, x0, y0, x1 + 1, y1 + 1);
}

// A constant's plane.
static struct GlrPlane flat(uint32_t value) {
	return (struct GlrPlane) { value << 24 | 1u << 23, 0, 0 };
}

void glrSprite(struct Glr* r, const struct GlrState* s, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t u, uint32_t v, uint32_t color,
               bool flipX, bool flipY, bool textured, bool raw) {
	if (w <= 0 || h <= 0) return;
	if (textured) needTexture(r, s);
	// u steps by one a pixel across (down with a flip, from u | 1), v by one a line
	uint32_t u0 = flipX ? (u | 1) : u, du = flipX ? ~0u : 1, dv = flipY ? ~0u : 1;
	struct GlrPlane planes[5] = { { ((u0 - du * (uint32_t) x) << 24) | 1u << 23, du << 24, 0 },
	                              { ((v - dv * (uint32_t) y) << 24) | 1u << 23, 0, dv << 24 },
	                              flat(color & 0xFF), flat(color >> 8 & 0xFF), flat(color >> 16 & 0xFF) };
	struct Vertex q;
	q.flags = (uint16_t) ((textured ? F_TEXTURED : 0) | (raw ? F_RAW : 0) | (s->maskSet ? F_MASK : 0));
	texInfo(s, &q);
	setPlanes(&q, planes);
	struct Vertex c[4];
	for (int i = 0; i < 4; i++) {
		c[i] = q;
		c[i].x = (int16_t) ((i & 1 ? x + w : x) * 16);
		c[i].y = (int16_t) ((i & 2 ? y + h : y) * 16);
	}
	struct Vertex t[6] = { c[0], c[1], c[2], c[1], c[3], c[2] };
	emit(r, s, t, 6, textured, x, y, x + w, y + h);
}

void glrLine(struct Glr* r, const struct GlrState* s, const struct GlrVertex in[2], bool dither) {
	struct GlrVertex p[2] = { in[0], in[1] };
	int32_t adx = abs(p[1].x - p[0].x), ady = abs(p[1].y - p[0].y), k = adx > ady ? adx : ady;
	if (adx >= 1024 || ady >= 512) return;
	if (p[0].x >= p[1].x && k) {
		struct GlrVertex t = p[0];
		p[0] = p[1];
		p[1] = t;
	}
	// The GPU's color steps (12 fraction bits) along the line, as planes over the major axis
	bool xMajor = adx >= ady && adx;
	int32_t dir = xMajor ? 1 : (p[1].y >= p[0].y ? 1 : -1), origin = xMajor ? p[0].x : p[0].y;
	uint32_t c0[3] = { p[0].r, p[0].g, p[0].b }, c1[3] = { p[1].r, p[1].g, p[1].b };
	struct GlrPlane planes[5] = { flat(0), flat(0) };
	for (int i = 0; i < 3; i++) {
		uint32_t step = k ? (uint32_t) ((int32_t) ((c1[i] - c0[i]) << 12) / k) : 0, start = c0[i] << 12 | 1u << 11;
		uint32_t d = (uint32_t) dir * (step << 12);
		planes[2 + i] = (struct GlrPlane) { (start << 12) - d * (uint32_t) origin, xMajor ? d : 0, xMajor ? 0 : d };
	}
	struct Vertex q;
	q.flags = (uint16_t) ((dither ? F_DITHER : 0) | (s->maskSet ? F_MASK : 0));
	texInfo(s, &q);
	setPlanes(&q, planes);
	// A quad a pixel thick around the line, across its minor axis, ending half a pixel past its ends
	int32_t dx = p[1].x - p[0].x, dy = p[1].y - p[0].y;
	bool wide = adx >= ady;
	int32_t ex = k ? dx * 8 / k : 8, ey = k ? dy * 8 / k : 0;
	struct Vertex c[4];
	for (int i = 0; i < 4; i++) {
		const struct GlrVertex* e = &p[i >> 1];
		int32_t along = i >> 1 ? 1 : -1, side = (i & 1) ? 8 : -8;
		c[i] = q;
		c[i].x = (int16_t) (e->x * 16 + along * ex + (wide ? 0 : side));
		c[i].y = (int16_t) (e->y * 16 + along * ey + (wide ? side : 0));
	}
	struct Vertex t[6] = { c[0], c[1], c[2], c[1], c[3], c[2] };
	int32_t x0 = p[0].x < p[1].x ? p[0].x : p[1].x, y0 = p[0].y < p[1].y ? p[0].y : p[1].y;
	emit(r, s, t, 6, false, x0, y0, x0 + adx + 1, y0 + ady + 1);
}

// A rectangle split where it wraps around VRAM's edges: up to 4 pieces (x, y, w, h).
static int pieces(int32_t x, int32_t y, int32_t w, int32_t h, int32_t out[4][4]) {
	int n = 0;
	x &= 1023;
	y &= 511;
	for (int py = 0; py < 2; py++) {
		int32_t ys = py ? 0 : y, hs = py ? y + h - 512 : (y + h > 512 ? 512 - y : h);
		if (hs <= 0) continue;
		for (int px = 0; px < 2; px++) {
			int32_t xs = px ? 0 : x, ws = px ? x + w - 1024 : (x + w > 1024 ? 1024 - x : w);
			if (ws <= 0) continue;
			out[n][0] = xs;
			out[n][1] = ys;
			out[n][2] = ws;
			out[n][3] = hs;
			n++;
		}
	}
	return n;
}

void glrFill(struct Glr* r, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color, int skip) {
	if (glrLogFile) fprintf(glrLogFile, "fill %d,%d %dx%d %06x skip %d\n", x, y, w, h, color, skip);
	if (skip >= 0) {
		// Every other line: drawn as rectangles, without the clip area or the mask
		struct GlrState s = { 0, 0, 1023, 511, 0, 0, 0, 0, 0, 0, 0, false, false, false, -1, 0, skip };
		int32_t p[4][4];
		for (int i = pieces(x, y, w, h, p); i-- > 0;) glrSprite(r, &s, p[i][0], p[i][1], p[i][2], p[i][3], 0, 0, color & 0xF8F8F8, false, false, false, true);
		return;
	}
	flush(r);
	struct Gl* gl = &r->gl;
	int s = r->scale;
	gl->BindFramebuffer(GL_FRAMEBUFFER, r->targetFb);
	gl->ClearColor(((float) (color & 0xF8) + 0.25f) / 255.0f, ((float) (color >> 8 & 0xF8) + 0.25f) / 255.0f, ((float) (color >> 16 & 0xF8) + 0.25f) / 255.0f, 0);
	gl->Enable(GL_SCISSOR_TEST);
	int32_t p[4][4];
	for (int i = pieces(x, y, w, h, p); i-- > 0;) {
		gl->Scissor(p[i][0] * s, p[i][1] * s, p[i][2] * s, p[i][3] * s);
		gl->Clear(GL_COLOR_BUFFER_BIT);
		markDirty(r, p[i][0], p[i][1], p[i][0] + p[i][2], p[i][1] + p[i][3]);
	}
	gl->Disable(GL_SCISSOR_TEST);
}

// A blit program pass over the target pixels of rectangle (x, y, w, h) in VRAM pixels.
static void blitPass(struct Glr* r, int kind, int32_t x, int32_t y, int32_t w, int32_t h, int32_t sx, int32_t sy, bool mask) {
	struct Gl* gl = &r->gl;
	int s = r->scale;
	gl->Viewport(x * s, y * s, w * s, h * s);
	gl->Uniform1i(r->blitKind, kind);
	gl->Uniform1i(r->blitMask, mask);
	gl->Uniform4iv(r->blitRect, 1, (const GLint[4]) { x * s, y * s, sx, sy });
	gl->DrawArrays(GL_TRIANGLES, 0, 3);
}

static void useBlit(struct Glr* r, GLuint fb) {
	struct Gl* gl = &r->gl;
	gl->BindFramebuffer(GL_FRAMEBUFFER, fb);
	gl->UseProgram(r->blit);
	gl->Uniform1i(r->blitScale, r->scale);
	gl->Uniform1i(r->blitWords, 1);
	gl->Uniform1i(r->blitVram, 0);
	gl->ActiveTexture(GL_TEXTURE0 + 1);
	gl->BindTexture(GL_TEXTURE_2D, r->words);
	gl->ActiveTexture(GL_TEXTURE0);
	gl->BindTexture(GL_TEXTURE_2D, r->sample);
	gl->BindVertexArray(r->vao);
}

void glrCopy(struct Glr* r, int32_t sx, int32_t sy, int32_t dx, int32_t dy, int32_t w, int32_t h, bool maskSet, bool maskEval) {
	if (glrLogFile) fprintf(glrLogFile, "copy %d,%d -> %d,%d %dx%d\n", sx, sy, dx, dy, w, h);
	needSample(r, sx, sy, w, h);
	flush(r);
	useBlit(r, r->targetFb);
	setBlend(&r->gl, maskEval ? BLEND_KEEP_MASKED : BLEND_NONE);
	int32_t p[4][4];
	for (int i = pieces(dx, dy, w, h, p); i-- > 0;) {
		blitPass(r, 1, p[i][0], p[i][1], p[i][2], p[i][3], (sx + p[i][0] - (dx & 1023)) & 1023, (sy + p[i][1] - (dy & 511)) & 511, maskSet);
		markDirty(r, p[i][0], p[i][1], p[i][0] + p[i][2], p[i][1] + p[i][3]);
	}
	r->gl.Disable(GL_BLEND);
}

void glrUpload(struct Glr* r, const uint16_t* vram, int32_t x, int32_t y, int32_t w, int32_t h) {
	if (glrLogFile) fprintf(glrLogFile, "upload %d,%d %dx%d\n", x, y, w, h);
	flush(r);
	struct Gl* gl = &r->gl;
	int32_t p[4][4];
	int n = pieces(x, y, w, h, p);
	gl->ActiveTexture(GL_TEXTURE0 + 1);
	gl->BindTexture(GL_TEXTURE_2D, r->words);
	gl->PixelStorei(GL_UNPACK_ALIGNMENT, 4);
	for (int i = 0; i < n; i++) {
		uint32_t* row = r->staging;
		for (int32_t y = 0; y < p[i][3]; y++) {
			const uint16_t* src = &vram[(p[i][1] + y) * 1024 + p[i][0]];
			for (int32_t x = 0; x < p[i][2]; x++) {
				uint32_t w = src[x];
				row[y * p[i][2] + x] = (w & 31) << 3 | ((w >> 5) & 31) << 11 | ((w >> 10) & 31) << 19 | (w >> 15) * 0xFF000000u;
			}
		}
		gl->TexSubImage2D(GL_TEXTURE_2D, 0, p[i][0], p[i][1], p[i][2], p[i][3], GL_RGBA, GL_UNSIGNED_BYTE, row);
	}
	useBlit(r, r->targetFb);
	for (int i = 0; i < n; i++) {
		blitPass(r, 0, p[i][0], p[i][1], p[i][2], p[i][3], 0, 0, false);
		markDirty(r, p[i][0], p[i][1], p[i][0] + p[i][2], p[i][1] + p[i][3]);
	}
}

void glrPicture(struct Glr* r, int32_t x, int32_t y, int32_t width, int32_t height, bool mode24, bool off) {
	flush(r);
	struct Gl* gl = &r->gl;
	int s = r->scale;
	if (width < 1) width = 1;
	if (height < 1) height = 1;
	if (width * s != r->pictureW || height * s != r->pictureH) {
		if (r->picture) {
			gl->DeleteTextures(1, &r->picture);
			gl->DeleteFramebuffers(1, &r->pictureFb);
		}
		r->pictureW = width * s;
		r->pictureH = height * s;
		r->picture = texture(gl, GL_RGBA8, r->pictureW, r->pictureH, GL_RGBA, GL_UNSIGNED_BYTE);
		r->pictureFb = framebuffer(gl, r->picture);
	}
	if (off) {
		gl->BindFramebuffer(GL_FRAMEBUFFER, r->pictureFb);
		gl->ClearColor(0, 0, 0, 1);
		gl->Clear(GL_COLOR_BUFFER_BIT);
		return;
	}
	// The picture reads the sample copy (24-bit words have to be whole)
	syncSample(r);
	useBlit(r, r->pictureFb);
	gl->Viewport(0, 0, r->pictureW, r->pictureH);
	gl->Uniform1i(r->blitKind, mode24 ? 3 : 2);
	gl->Uniform1i(r->blitMask, 0);
	gl->Uniform4iv(r->blitRect, 1, (const GLint[4]) { 0, 0, x & 1023, y & 511 });
	gl->DrawArrays(GL_TRIANGLES, 0, 3);
}

unsigned glrPictureFramebuffer(struct Glr* r, int* width, int* height) {
	*width = r->pictureW;
	*height = r->pictureH;
	return r->pictureFb;
}

void glrShow(struct Glr* r, unsigned framebuffer, bool flip) {
	struct Gl* gl = &r->gl;
	gl->BindFramebuffer(GL_READ_FRAMEBUFFER, r->pictureFb);
	gl->BindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
	gl->BlitFramebuffer(0, 0, r->pictureW, r->pictureH, 0, flip ? r->pictureH : 0, r->pictureW, flip ? 0 : r->pictureH,
	                    GL_COLOR_BUFFER_BIT, GL_NEAREST);
	gl->BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
}

void glrReadPicture(struct Glr* r, uint32_t* pixels) {
	struct Gl* gl = &r->gl;
	gl->BindFramebuffer(GL_FRAMEBUFFER, r->pictureFb);
	gl->PixelStorei(GL_PACK_ALIGNMENT, 4);
	gl->ReadPixels(0, 0, r->pictureW, r->pictureH, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	for (int i = 0; i < r->pictureW * r->pictureH; i++) {
		uint32_t c = pixels[i];
		pixels[i] = (c & 0xFF) << 16 | (c & 0xFF00) | (c >> 16 & 0xFF);
	}
}

void glrReadVram(struct Glr* r, uint16_t* vram) {
	flush(r);
	struct Gl* gl = &r->gl;
	int s = r->scale;
	uint8_t* row = malloc((size_t) 1024 * s * 4);
	if (!row) return;
	gl->BindFramebuffer(GL_FRAMEBUFFER, r->targetFb);
	gl->PixelStorei(GL_PACK_ALIGNMENT, 4);
	for (int y = 0; y < 512; y++) {
		gl->ReadPixels(0, y * s + s / 2, 1024 * s, 1, GL_RGBA, GL_UNSIGNED_BYTE, row);
		for (int x = 0; x < 1024; x++) {
			const uint8_t* c = &row[(x * s + s / 2) * 4];
			vram[y * 1024 + x] = (uint16_t) (c[0] >> 3 | (c[1] >> 3) << 5 | (c[2] >> 3) << 10 | (c[3] >> 7) << 15);
		}
	}
	free(row);
}
