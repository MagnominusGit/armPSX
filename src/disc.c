/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <libchdr/chd.h>
#include "disc.h"

// Disc images: .cue sheets with their .bin files, .chd, and .m3u lists of either. The track layout
// (pregaps in and out of the image, postgaps) and the subchannel Q made for each sector follow
// Mednafen's CDAccess_Image and Beetle PSX's CDAccess_CHD (GPL-2.0-or-later).

enum { FORMAT_AUDIO, FORMAT_MODE1, FORMAT_MODE1_RAW, FORMAT_MODE2_RAW };

struct Track {
	int format;
	uint8_t control;
	int32_t lba, pregap, pregapDv, sectors, postgap;
	int32_t index[100];
	FILE* file;            // .bin; NULL for a .chd
	int64_t fileOffset;    // bytes (.bin) or frames (.chd) of the first INDEX 01 sector
	int sectorSize;
	bool audioMsbFirst;
};

struct Disc {
	struct Track tracks[100];
	int first, last;
	int32_t total;
	struct Toc toc;
	FILE* files[100];
	int fileCount;
	chd_file* chd;
	uint8_t* hunk;
	uint32_t hunkBytes;
	int64_t hunkNumber;
};

static void makeToc(struct Disc* d) {
	struct Toc* t = &d->toc;
	memset(t, 0, sizeof *t);
	t->first_track = d->first;
	t->last_track = d->last;
	t->disc_type = DISC_TYPE_CDDA_OR_M1;
	for (int i = d->first; i <= d->last; i++) {
		t->tracks[i].adr = 1;
		t->tracks[i].control = d->tracks[i].control;
		t->tracks[i].lba = d->tracks[i].lba;
		t->tracks[i].valid = true;
		if (d->tracks[i].format == FORMAT_MODE2_RAW) {
			t->disc_type = DISC_TYPE_CD_XA;
		}
	}
	t->tracks[100].adr = 1;
	t->tracks[100].control = d->tracks[d->last].control & 4;
	t->tracks[100].lba = d->total;
	t->tracks[100].valid = true;
}

static void finishIndexes(struct Disc* d) {
	for (int x = d->first; x <= d->last; x++) {
		struct Track* t = &d->tracks[x];
		int32_t base = t->index[1];
		for (int i = 0; i < 100; i++) {
			t->index[i] = i == 0 || t->index[i] == -1 ? INT32_MAX : t->lba + (t->index[i] - base);
		}
	}
}

// .chd

static struct Disc* openChd(const char* path) {
	chd_file* chd;
	if (chd_open(path, CHD_OPEN_READ, NULL, &chd) != CHDERR_NONE) {
		return NULL;
	}
	struct Disc* d = calloc(1, sizeof *d);
	d->chd = chd;
	d->hunkBytes = chd_get_header(chd)->hunkbytes;
	d->hunk = malloc(d->hunkBytes);
	d->hunkNumber = -1;
	int32_t plba = -150;
	int64_t offset = 0;
	d->first = 1;
	for (int i = 0; i < 99; i++) {
		char meta[256];
		uint32_t length;
		int track = 0, frames = 0, pregap = 0, postgap = 0;
		char type[64] = "", subtype[64] = "", pgtype[64] = "", pgsub[64] = "";
		if (chd_get_metadata(chd, CDROM_TRACK_METADATA2_TAG, i, meta, sizeof meta - 1, &length, NULL, NULL) == CHDERR_NONE) {
			meta[length < sizeof meta ? length : sizeof meta - 1] = 0;
			sscanf(meta, "TRACK:%d TYPE:%63s SUBTYPE:%63s FRAMES:%d PREGAP:%d PGTYPE:%63s PGSUB:%63s POSTGAP:%d", &track, type, subtype, &frames, &pregap, pgtype, pgsub, &postgap);
		} else if (chd_get_metadata(chd, CDROM_TRACK_METADATA_TAG, i, meta, sizeof meta - 1, &length, NULL, NULL) == CHDERR_NONE) {
			meta[length < sizeof meta ? length : sizeof meta - 1] = 0;
			sscanf(meta, "TRACK:%d TYPE:%63s SUBTYPE:%63s FRAMES:%d", &track, type, subtype, &frames);
		} else {
			break;
		}
		if (track < 1 || track > 99) break;
		struct Track* t = &d->tracks[track];
		if (!strncmp(type, "AUDIO", 5)) {
			t->format = FORMAT_AUDIO;
			t->control = 0;
		} else if (!strncmp(type, "MODE1_RAW", 9) || !strncmp(type, "MODE1", 5)) {
			t->format = FORMAT_MODE1_RAW;
			t->control = SUBQ_CTRLF_DATA;
		} else {
			t->format = FORMAT_MODE2_RAW;
			t->control = SUBQ_CTRLF_DATA;
		}
		t->audioMsbFirst = true;
		t->pregap = track == 1 ? 150 : pgtype[0] == 'V' ? 0 : pregap;
		t->pregapDv = pgtype[0] == 'V' ? pregap : 0;
		plba += t->pregap + t->pregapDv;
		t->lba = plba;
		t->postgap = postgap;
		t->sectors = frames - t->pregapDv;
		offset += t->pregapDv;
		t->fileOffset = offset;
		offset += frames - t->pregapDv;
		offset += t->postgap;
		offset += ((frames + 3) & ~3) - frames;
		plba += frames - t->pregapDv + t->postgap;
		for (int k = 0; k < 100; k++) t->index[k] = -1;
		t->index[1] = 0;
		d->last = track;
	}
	if (!d->last) {
		chd_close(chd);
		free(d->hunk);
		free(d);
		return NULL;
	}
	d->total = plba;
	finishIndexes(d);
	makeToc(d);
	return d;
}

// .cue

static int32_t msf(const char* s) {
	int m = 0, sec = 0, f = 0;
	sscanf(s, "%d:%d:%d", &m, &sec, &f);
	return (m * 60 + sec) * 75 + f;
}

static char* nextToken(char** p) {
	char* s = *p;
	while (*s && isspace((unsigned char) *s)) s++;
	if (!*s) return NULL;
	char* start;
	if (*s == '"') {
		start = ++s;
		while (*s && *s != '"') s++;
	} else {
		start = s;
		while (*s && !isspace((unsigned char) *s)) s++;
	}
	if (*s) *s++ = 0;
	*p = s;
	return start;
}

static void pathNextTo(char* out, size_t size, const char* base, const char* name) {
	const char* slash = strrchr(base, '/');
	if (name[0] == '/' || !slash) {
		snprintf(out, size, "%s", name);
	} else {
		snprintf(out, size, "%.*s/%s", (int) (slash - base), base, name);
	}
}

static struct Disc* openCue(const char* path) {
	FILE* f = fopen(path, "r");
	if (!f) return NULL;
	struct Disc* d = calloc(1, sizeof *d);
	struct { FILE* file; int64_t size; int32_t index0, index1; int pregap, postgap; } info[100];
	memset(info, 0, sizeof info);
	FILE* current = NULL;
	int64_t currentSize = 0;
	int track = 0;
	char line[1024];
	while (fgets(line, sizeof line, f)) {
		char* p = line;
		char* key = nextToken(&p);
		if (!key) continue;
		for (char* k = key; *k; k++) *k = toupper((unsigned char) *k);
		if (!strcmp(key, "FILE")) {
			char* name = nextToken(&p);
			if (!name) continue;
			char full[1024];
			pathNextTo(full, sizeof full, path, name);
			current = fopen(full, "rb");
			if (!current) {
				fclose(f);
				discClose(d);
				return NULL;
			}
			d->files[d->fileCount++] = current;
			fseek(current, 0, SEEK_END);
			currentSize = ftell(current);
		} else if (!strcmp(key, "TRACK")) {
			char* number = nextToken(&p);
			char* type = nextToken(&p);
			if (!number || !type) continue;
			track = atoi(number);
			if (track < 1 || track > 99) continue;
			struct Track* t = &d->tracks[track];
			for (int k = 0; k < 100; k++) t->index[k] = -1;
			if (!d->first) d->first = track;
			d->last = track;
			info[track].file = current;
			info[track].size = currentSize;
			info[track].index0 = -1;
			info[track].index1 = -1;
			if (!strcmp(type, "AUDIO")) {
				t->format = FORMAT_AUDIO;
				t->sectorSize = 2352;
				t->control = 0;
			} else if (!strcmp(type, "MODE1/2048")) {
				t->format = FORMAT_MODE1;
				t->sectorSize = 2048;
				t->control = SUBQ_CTRLF_DATA;
			} else if (!strncmp(type, "MODE1", 5)) {
				t->format = FORMAT_MODE1_RAW;
				t->sectorSize = 2352;
				t->control = SUBQ_CTRLF_DATA;
			} else {
				t->format = FORMAT_MODE2_RAW;
				t->sectorSize = 2352;
				t->control = SUBQ_CTRLF_DATA;
			}
		} else if (!strcmp(key, "INDEX") && track) {
			char* number = nextToken(&p);
			char* at = nextToken(&p);
			if (!number || !at) continue;
			int n = atoi(number);
			if (n == 0) info[track].index0 = msf(at);
			if (n == 1) info[track].index1 = msf(at);
			if (n >= 0 && n < 100) d->tracks[track].index[n] = msf(at);
		} else if (!strcmp(key, "PREGAP") && track) {
			char* at = nextToken(&p);
			if (at) info[track].pregap = msf(at);
		} else if (!strcmp(key, "POSTGAP") && track) {
			char* at = nextToken(&p);
			if (at) info[track].postgap = msf(at);
		} else if (!strcmp(key, "FLAGS") && track) {
			char* flag;
			while ((flag = nextToken(&p))) {
				if (!strcmp(flag, "DCP")) d->tracks[track].control |= SUBQ_CTRLF_DCP;
				if (!strcmp(flag, "PRE")) d->tracks[track].control |= SUBQ_CTRLF_PRE;
				if (!strcmp(flag, "4CH")) d->tracks[track].control |= SUBQ_CTRLF_4CH;
			}
		}
	}
	fclose(f);
	if (!d->last) {
		discClose(d);
		return NULL;
	}
	int32_t lba = 0;
	for (int i = d->first; i <= d->last; i++) {
		struct Track* t = &d->tracks[i];
		t->file = info[i].file;
		if (info[i].index1 < 0) info[i].index1 = info[i].index0 < 0 ? 0 : info[i].index0;
		t->pregap = info[i].pregap + (i == d->first ? 150 : 0);
		t->pregapDv = info[i].index0 >= 0 ? info[i].index1 - info[i].index0 : 0;
		t->postgap = info[i].postgap;
		t->fileOffset = (int64_t) info[i].index1 * t->sectorSize;
		int32_t end;
		if (i < d->last && info[i + 1].file == info[i].file) {
			end = info[i + 1].index0 >= 0 ? info[i + 1].index0 : info[i + 1].index1;
		} else {
			end = (int32_t) (info[i].size / t->sectorSize);
		}
		t->sectors = end - info[i].index1;
		if (i > d->first) {
			const struct Track* prev = &d->tracks[i - 1];
			lba += prev->sectors + prev->postgap + t->pregap + t->pregapDv;
		}
		t->lba = lba;
		t->index[1] = info[i].index1;
		if (t->index[0] >= 0) t->index[0] = -1;
	}
	d->total = d->tracks[d->last].lba + d->tracks[d->last].sectors + d->tracks[d->last].postgap;
	finishIndexes(d);
	makeToc(d);
	return d;
}

// .m3u

static bool m3uEntry(const char* path, int index, char* out, size_t size) {
	FILE* f = fopen(path, "r");
	if (!f) return false;
	char line[1024];
	int n = 0;
	bool found = false;
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (!line[0] || line[0] == '#') continue;
		if (n++ == index) {
			pathNextTo(out, size, path, line);
			found = true;
			break;
		}
	}
	fclose(f);
	return found;
}

static bool hasExtension(const char* path, const char* ext) {
	size_t n = strlen(path), e = strlen(ext);
	return n > e && !strcasecmp(path + n - e, ext);
}

// A folder named like a list file holds that list file and its discs.
static const char* folderList(const char* path, char* out, size_t size) {
	struct stat st;
	if (stat(path, &st) || !S_ISDIR(st.st_mode)) return path;
	const char* slash = strrchr(path, '/');
	snprintf(out, size, "%s/%s", path, slash ? slash + 1 : path);
	return out;
}

int discCount(const char* path) {
	char inner[1024];
	path = folderList(path, inner, sizeof inner);
	if (!hasExtension(path, ".m3u")) return 1;
	int n = 0;
	char entry[1024];
	while (m3uEntry(path, n, entry, sizeof entry)) n++;
	return n;
}

struct Disc* discOpenIndex(const char* path, int index) {
	char inner[1024];
	path = folderList(path, inner, sizeof inner);
	if (hasExtension(path, ".m3u")) {
		char entry[1024];
		if (!m3uEntry(path, index, entry, sizeof entry)) return NULL;
		return discOpenIndex(entry, 0);
	}
	if (hasExtension(path, ".chd")) return openChd(path);
	if (hasExtension(path, ".cue")) return openCue(path);
	return NULL;
}

struct Disc* discOpen(const char* path) {
	return discOpenIndex(path, 0);
}

void discClose(struct Disc* d) {
	if (!d) return;
	for (int i = 0; i < d->fileCount; i++) fclose(d->files[i]);
	if (d->chd) chd_close(d->chd);
	free(d->hunk);
	free(d);
}

void discToc(struct Disc* d, struct Toc* toc) {
	*toc = d->toc;
}

// Subchannel Q for the user data area (MakeSubPQ); the lead-in and lead-out are made separately.
static int makeSubPq(struct Disc* d, int32_t lba, uint8_t* pw) {
	uint8_t q[12];
	int track;
	for (track = d->first; track <= d->last; track++) {
		const struct Track* t = &d->tracks[track];
		if (lba >= t->lba - t->pregapDv - t->pregap && lba < t->lba + t->sectors + t->postgap) break;
	}
	if (track > d->last) track = d->last;
	const struct Track* t = &d->tracks[track];
	uint32_t relative = lba < t->lba ? (uint32_t) (t->lba - 1 - lba) : (uint32_t) (lba - t->lba);
	uint8_t control = t->control, pause = 0;
	if (lba < t->lba || lba >= t->lba + t->sectors) pause = 0x80;
	if (lba - t->lba < -150 && (t->control & SUBQ_CTRLF_DATA) && d->first < track && !(d->tracks[track - 1].control & SUBQ_CTRLF_DATA)) {
		control = d->tracks[track - 1].control;
	}
	memset(q, 0, sizeof q);
	q[0] = 1 | control << 4;
	q[1] = U8_to_BCD(track);
	int index = 0;
	for (int i = 0; i < 100; i++) {
		if (lba >= t->index[i]) index = i;
	}
	q[2] = U8_to_BCD(index);
	q[3] = U8_to_BCD(relative / 75 / 60);
	q[4] = U8_to_BCD((relative / 75) % 60);
	q[5] = U8_to_BCD(relative % 75);
	uint32_t aba = (uint32_t) (lba + 150);
	q[7] = U8_to_BCD(aba / 75 / 60);
	q[8] = U8_to_BCD((aba / 75) % 60);
	q[9] = U8_to_BCD(aba % 75);
	subq_generate_checksum(q);
	for (int i = 0; i < 96; i++) {
		pw[i] |= ((q[i >> 3] >> (7 - (i & 7))) & 1 ? 0x40 : 0) | pause;
	}
	return track;
}

static void makeLeadoutPq(struct Disc* d, int32_t lba, uint8_t* pw) {
	uint8_t q[12];
	uint32_t relative = (uint32_t) lba - d->toc.tracks[100].lba, aba = (uint32_t) (lba + 150);
	uint8_t control = d->toc.tracks[100].control | (d->tracks[d->last].control & 4);
	memset(q, 0, sizeof q);
	q[0] = 1 | control << 4;
	q[1] = 0xAA;
	q[2] = 0x01;
	q[3] = U8_to_BCD(relative / 75 / 60);
	q[4] = U8_to_BCD((relative / 75) % 60);
	q[5] = U8_to_BCD(relative % 75);
	q[7] = U8_to_BCD(aba / 75 / 60);
	q[8] = U8_to_BCD((aba / 75) % 60);
	q[9] = U8_to_BCD(aba % 75);
	subq_generate_checksum(q);
	for (int i = 0; i < 96; i++) {
		pw[i] = ((q[i >> 3] >> (7 - (i & 7))) & 1 ? 0x40 : 0) | 0x80;
	}
}

// Sync and header for a synthesized data sector (pregap, lead-out); ECC and EDC are left at zero.
static void synthHeader(uint8_t* buf, int32_t lba, int format) {
	memset(buf, 0, 2352);
	if (format == FORMAT_AUDIO) return;
	memset(buf + 1, 0xFF, 10);
	uint32_t aba = (uint32_t) (lba + 150);
	buf[12] = U8_to_BCD(aba / 75 / 60);
	buf[13] = U8_to_BCD((aba / 75) % 60);
	buf[14] = U8_to_BCD(aba % 75);
	if (format == FORMAT_MODE2_RAW) {
		buf[15] = 2;
		buf[16 + 2] = buf[16 + 6] = 0x20;
	} else {
		buf[15] = 1;
	}
}

static void readData(struct Disc* d, const struct Track* t, int32_t lba, uint8_t* buf) {
	int64_t sector = lba - t->lba;
	if (d->chd) {
		int64_t frame = t->fileOffset + sector;
		uint32_t perHunk = d->hunkBytes / 2448;
		int64_t hunk = frame / perHunk;
		if (hunk != d->hunkNumber) {
			if (chd_read(d->chd, (uint32_t) hunk, d->hunk) != CHDERR_NONE) {
				memset(buf, 0, 2352);
				d->hunkNumber = -1;
				return;
			}
			d->hunkNumber = hunk;
		}
		const uint8_t* s = d->hunk + (frame % perHunk) * 2448;
		if (t->format == FORMAT_AUDIO) {
			for (int i = 0; i < 2352; i += 2) {
				buf[i] = s[i + 1];
				buf[i + 1] = s[i];
			}
		} else {
			memcpy(buf, s, 2352);
		}
		return;
	}
	if (t->format == FORMAT_MODE1) {
		synthHeader(buf, lba, FORMAT_MODE1_RAW);
		fseek(t->file, t->fileOffset + sector * 2048, SEEK_SET);
		if (fread(buf + 16, 1, 2048, t->file) != 2048) memset(buf + 16, 0, 2048);
		return;
	}
	fseek(t->file, t->fileOffset + sector * 2352, SEEK_SET);
	if (fread(buf, 1, 2352, t->file) != 2352) memset(buf, 0, 2352);
}

void discReadRaw(struct Disc* d, int32_t lba, uint8_t* buf) {
	if (lba >= d->total) {
		synthHeader(buf, lba, d->tracks[d->last].format);
		makeLeadoutPq(d, lba, buf + 2352);
		return;
	}
	memset(buf + 2352, 0, 96);
	int track = makeSubPq(d, lba, buf + 2352);
	const struct Track* t = &d->tracks[track];
	if (lba < t->lba - t->pregapDv || lba >= t->lba + t->sectors) {
		const struct Track* e = t;
		if (lba - t->lba < -150 && (t->control & SUBQ_CTRLF_DATA) && d->first < track && !(d->tracks[track - 1].control & SUBQ_CTRLF_DATA)) {
			e = &d->tracks[track - 1];
		}
		synthHeader(buf, lba, e->format);
		return;
	}
	readData(d, t, lba, buf);
}

void discReadPw(struct Disc* d, int32_t lba, uint8_t* pw) {
	if (lba >= d->total) {
		makeLeadoutPq(d, lba, pw);
		return;
	}
	memset(pw, 0, 96);
	makeSubPq(d, lba, pw);
}
