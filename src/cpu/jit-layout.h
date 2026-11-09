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

// Lays out a stretch at base: block bodies fill its first part and cold code its second.
static void startStretch(struct Jit* jit, uint8_t* base) {
	if (jit->hotEnd && base != jit->codeNext) {
		// What the stretch being left had no use for.
		jit->wastedBytes += (uint64_t) (jit->hotEnd - jit->codeNext) + (uint64_t) (jit->coldEnd - jit->coldNext);
	}
	size_t hot = HOT_PART;
	uint64_t const total = jit->hotBytes + jit->coldBytes;
	if (total > (256 << 10)) {
		// A quarter more than the share seen, for the games whose blocks change as they go on.
		hot = (size_t) (STRETCH * jit->hotBytes * 5 / 4 / total);
		hot = hot < HOT_PART_MIN ? HOT_PART_MIN : hot > HOT_PART_MAX ? HOT_PART_MAX : hot;
		hot &= ~(size_t) 0xFFF;
	}
	size_t const index = (size_t) (base - jit->codeStart) / STRETCH;
	if (base >= jit->codeStart && index < sizeof(jit->stretchHot) / sizeof(*jit->stretchHot)) {
		jit->stretchHot[index] = (uint32_t) hot;
	}
	jit->codeNext = base;
	jit->hotEnd = base + hot;
	jit->coldNext = jit->hotEnd;
	jit->coldEnd = base + STRETCH;
}

// After a flush, compiling starts in a new stretch.
void backendReset(struct Jit* jit) {
	startStretch(jit, jit->codeNext);
}

// Whether an address is in a stretch's cold part.
bool backendColdSide(const struct Jit* jit, const uint8_t* p) {
	if (p < jit->codeStart) {
		return false;
	}
	size_t const index = (size_t) (p - jit->codeStart) / STRETCH;
	size_t const hot = index < sizeof(jit->stretchHot) / sizeof(*jit->stretchHot) ? jit->stretchHot[index] : HOT_PART;
	return (size_t) (p - jit->codeStart) % STRETCH >= hot;
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
	if (jit->hotEnd - jit->codeNext < HOT_ROOM || jit->coldEnd - jit->coldNext < COLD_ROOM) {
		if (jit->codeEnd - jit->coldEnd < STRETCH) {
			return false;
		}
		startStretch(jit, jit->coldEnd);
	}
	jit->hotMark = jit->codeNext;
	jit->coldMark = jit->coldNext;
	if (compileInStretch(jit, block, insts, count, nextOps)) {
		return true;
	}
	if (jit->codeEnd - jit->coldEnd < STRETCH) {
		return false;
	}
	startStretch(jit, jit->coldEnd);
	jit->hotMark = jit->codeNext;
	jit->coldMark = jit->coldNext;
	return compileInStretch(jit, block, insts, count, nextOps);
}

#endif
