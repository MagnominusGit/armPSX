/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * CD addressing and the table of contents, after Mednafen's CDUtility (GPL-2.0-or-later). */
#ifndef CDUTIL_H
#define CDUTIL_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum { SUBQ_CTRLF_PRE = 1, SUBQ_CTRLF_DCP = 2, SUBQ_CTRLF_DATA = 4, SUBQ_CTRLF_4CH = 8 };
enum { DISC_TYPE_CDDA_OR_M1 = 0x00, DISC_TYPE_CD_I = 0x10, DISC_TYPE_CD_XA = 0x20 };

struct TocTrack {
	uint8_t adr, control;
	uint32_t lba;
	bool valid;
};

// tracks[100] is the lead-out.
struct Toc {
	uint8_t first_track, last_track, disc_type;
	struct TocTrack tracks[101];
};

static inline int tocFindTrackByLba(const struct Toc* toc, uint32_t lba) {
	for (int track = toc->first_track; track <= toc->last_track + 1; track++) {
		if (track == toc->last_track + 1) {
			if (lba < toc->tracks[100].lba) return track - 1;
		} else if (lba < toc->tracks[track].lba) {
			return track - 1;
		}
	}
	return 0;
}

static inline uint8_t U8_to_BCD(uint8_t n) {
	return ((n / 10) << 4) | (n % 10);
}

static inline uint8_t BCD_to_U8(uint8_t b) {
	return ((b >> 4) * 10) + (b & 0xF);
}

static inline bool BCD_is_valid(uint8_t b) {
	return (b & 0xF0) <= 0x90 && (b & 0x0F) <= 0x09;
}

static inline int32_t AMSF_to_LBA(uint8_t m, uint8_t s, uint8_t f) {
	return (int32_t) (f + 75 * s + 75 * 60 * m) - 150;
}

static inline void LBA_to_AMSF(int32_t lba, uint8_t* m, uint8_t* s, uint8_t* f) {
	uint32_t aba = (uint32_t) (lba + 150);
	*m = aba / 75 / 60;
	*s = (aba - *m * 75 * 60) / 75;
	*f = aba - (*m * 75 * 60) - (*s * 75);
}

static inline uint16_t subqCrc(const uint8_t* buf) {
	uint16_t crc = 0;
	for (int i = 0; i < 10; i++) {
		crc ^= (uint16_t) buf[i] << 8;
		for (int b = 0; b < 8; b++) {
			crc = crc & 0x8000 ? (uint16_t) (crc << 1) ^ 0x1021 : (uint16_t) (crc << 1);
		}
	}
	return ~crc;
}

static inline bool subq_check_checksum(const uint8_t* q) {
	return subqCrc(q) == (uint16_t) (q[10] << 8 | q[11]);
}

static inline void subq_generate_checksum(uint8_t* q) {
	uint16_t crc = subqCrc(q);
	q[10] = crc >> 8;
	q[11] = (uint8_t) crc;
}

#endif
