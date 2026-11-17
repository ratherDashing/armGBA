/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_JIT_LAYOUT_H
#define ARMGBA_JIT_LAYOUT_H

// For backends that keep blocks' bodies apart from their cold code: how the code memory is laid out,
// and the retry of a block that doesn't fit. Included after jit-compile.h; the backend defines
// compileInStretch.

// Blocks' bodies are kept apart from their cold code (slow paths, exits, and whole careful copies),
// so the code a game spends its time in is dense in the caches; on a Cortex-A7 or A55 with 32 KiB of
// instruction cache, that decides much of a busy game's speed. The code memory is laid out in
// stretches, each with a part for bodies and a part for cold code, so a block's cold code stays near
// its body.
enum {
	STRETCH = 1 << 20,
	// The first stretch's part for bodies; later ones are split as the game's blocks have been so
	// far (Mario Kart's take about 160 bytes of body to 900 of cold code, and a fixed split of 352 to
	// 640 KiB left 70% of the code memory unused, 2026-11-09).
	HOT_PART = 352 << 10,
	HOT_PART_MIN = 64 << 10,
	HOT_PART_MAX = STRETCH - (128 << 10),
	// Room a block may need in each part; one that needs more is compiled again in a new stretch.
	HOT_ROOM = 16 << 10,
	COLD_ROOM = 32 << 10,
};

static bool compileInStretch(struct Jit*, struct Block*, struct Inst* insts, int count, const uint32_t* nextOps);

enum {
	// When one part of a stretch is full and the other has at least this much room, that room is
	// laid out as a smaller stretch of its own rather than left: a fixed split wasted about 40% of
	// the code memory in Yoshi's Island, whose careful copies (cold code) come in bursts (2026-11-16).
	CARVE_MIN = 128 << 10,
};

// The part for bodies in size bytes: a quarter more than the share seen so far.
static size_t hotShare(const struct Jit* jit, size_t size, size_t least, size_t most) {
	size_t hot = size * HOT_PART / STRETCH;
	uint64_t const total = jit->hotBytes + jit->coldBytes;
	if (total > (256 << 10)) {
		hot = (size_t) (size * jit->hotBytes * 5 / 4 / total);
	}
	hot = hot < least ? least : hot > most ? most : hot;
	return hot & ~(size_t) 0x3F;
}

// Notes that from start on, code at or past hotEnd is cold, keeping the marks in address order.
static bool addMark(struct Jit* jit, uint8_t* start, uint8_t* hotEnd) {
	int const capacity = (int) (sizeof(jit->marks) / sizeof(*jit->marks));
	if (jit->markCount >= capacity) {
		return false;
	}
	int at = jit->markCount;
	while (at > 0 && jit->marks[at - 1].start >= start) {
		--at;
	}
	if (at < jit->markCount && jit->marks[at].start == start) {
		jit->marks[at].hotEnd = hotEnd;
		return true;
	}
	memmove(&jit->marks[at + 1], &jit->marks[at], (size_t) (jit->markCount - at) * sizeof(*jit->marks));
	jit->marks[at] = (struct StretchMark) { start, hotEnd };
	++jit->markCount;
	return true;
}

// Lays out [base, end) as a stretch: block bodies fill its first part and cold code its second.
static void layStretch(struct Jit* jit, uint8_t* base, uint8_t* end, size_t hot) {
	jit->codeNext = base;
	jit->hotEnd = base + hot;
	jit->coldNext = jit->hotEnd;
	jit->coldEnd = end;
}

// What the stretch being left had no use for.
static void leave(struct Jit* jit) {
	if (jit->hotEnd) {
		jit->wastedBytes += (uint64_t) (jit->hotEnd - jit->codeNext) + (uint64_t) (jit->coldEnd - jit->coldNext);
	}
}

// A new stretch from the code memory no stretch has taken. Fails if there is none.
static bool freshStretch(struct Jit* jit) {
	if (jit->codeEnd - jit->freshNext < STRETCH || jit->markCount + 1 > (int) (sizeof(jit->marks) / sizeof(*jit->marks))) {
		return false;
	}
	uint8_t* const base = jit->freshNext;
	size_t const hot = hotShare(jit, STRETCH, HOT_PART_MIN, HOT_PART_MAX);
	leave(jit);
	addMark(jit, base, base + hot);
	layStretch(jit, base, base + STRETCH, hot);
	jit->freshNext = base + STRETCH;
	return true;
}

// The room one part of the stretch being filled has left, laid out as a stretch of its own, when
// the other part is full. The bodies' room: the new stretch's code is past all bodies so far, and
// the marks then say its last part is the old stretch's cold code again. The cold room: past all
// code of the old stretch.
static bool carveStretch(struct Jit* jit, bool fromHot) {
	uint8_t* const base = fromHot ? jit->codeNext : jit->coldNext;
	uint8_t* const end = fromHot ? jit->hotEnd : jit->coldEnd;
	size_t const size = (size_t) (end - base);
	if (size < CARVE_MIN || jit->markCount + 2 > (int) (sizeof(jit->marks) / sizeof(*jit->marks))) {
		return false;
	}
	size_t const hot = hotShare(jit, size, HOT_ROOM * 2, size - COLD_ROOM * 2);
	if (fromHot) {
		jit->wastedBytes += (uint64_t) (jit->coldEnd - jit->coldNext);
		addMark(jit, end, end);
	} else {
		jit->wastedBytes += (uint64_t) (jit->hotEnd - jit->codeNext);
	}
	addMark(jit, base, base + hot);
	layStretch(jit, base, end, hot);
	return true;
}

// After a flush, compiling starts in a new stretch.
void backendReset(struct Jit* jit) {
	jit->markCount = 0;
	jit->freshNext = jit->codeNext;
	jit->hotEnd = NULL;
	freshStretch(jit);
}

// Whether an address is in a stretch's cold part.
bool backendColdSide(const struct Jit* jit, const uint8_t* p) {
	int lo = 0;
	int hi = jit->markCount;
	while (hi - lo > 1) {
		int const mid = (lo + hi) / 2;
		if (jit->marks[mid].start <= p) {
			lo = mid;
		} else {
			hi = mid;
		}
	}
	return jit->markCount && p >= jit->marks[lo].start && p >= jit->marks[lo].hotEnd;
}

// Forgets what a compile whose code didn't fit left behind.
static void abandon(struct Compiler* c) {
	c->block->siteCount = 0;
	free(c->block->out);
	c->block->out = NULL;
	c->block->outCount = 0;
}

// A block's code goes in the stretch being filled while there is room for a large block there,
// otherwise in a new one; a block too large for the room left is compiled again in a new stretch.
// Fails if the code memory is full; the caller then flushes.
static bool backendCompileHere(struct Jit*, struct Block*, struct Inst*, int, const uint32_t*);

// Counts the bytes each part took.
bool backendCompile(struct Jit* jit, struct Block* block, struct Inst* insts, int count, const uint32_t* nextOps) {
	if (!backendCompileHere(jit, block, insts, count, nextOps)) {
		return false;
	}
	jit->hotBytes += (uint64_t) (jit->codeNext - jit->hotMark);
	jit->coldBytes += (uint64_t) (jit->coldNext - jit->coldMark);
	if (block->careful) {
		++jit->carefulCompiles;
		jit->carefulBytes += (uint64_t) (jit->coldNext - jit->coldMark);
	}
	return true;
}

static bool backendCompileHere(struct Jit* jit, struct Block* block, struct Inst* insts, int count, const uint32_t* nextOps) {
	bool const hotFull = jit->hotEnd - jit->codeNext < HOT_ROOM;
	if (hotFull || jit->coldEnd - jit->coldNext < COLD_ROOM) {
		if (!carveStretch(jit, !hotFull) && !freshStretch(jit)) {
			return false;
		}
	}
	jit->hotMark = jit->codeNext;
	jit->coldMark = jit->coldNext;
	if (compileInStretch(jit, block, insts, count, nextOps)) {
		return true;
	}
	if (!freshStretch(jit)) {
		return false;
	}
	jit->hotMark = jit->codeNext;
	jit->coldMark = jit->coldNext;
	return compileInStretch(jit, block, insts, count, nextOps);
}

#endif
