/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The instruction fetch and what it needs from bus.c, inline, for the table-driven CPU (lean.c) to
// fetch without a call; bus.c's busFetch, busCycles and busTick are these.

#ifndef ARMGBA_BUS_INLINE_H
#define ARMGBA_BUS_INLINE_H

#include "gba.h"
#include "cpu/jit-internal.h"

#include <string.h>

// The six regions the cartridge ROM is mirrored in.
static inline __attribute__((always_inline)) bool inRom(uint32_t address) {
	return (address >> 24) >= REGION_ROM0 && (address >> 24) <= REGION_ROM2_EX;
}

// 1 plus the wait states.
static inline __attribute__((always_inline)) int32_t busCyclesInline(struct GBA* gba, uint32_t address, int size, bool seq) {
	unsigned region = address >> 24;
	switch (region) {
	case REGION_SRAM:
	case REGION_SRAM_MIRROR:
		// An 8-bit bus that takes the same time whatever the size.
		return 1 + gba->waitN16[region];
	}
	if (size == 4) {
		return 1 + (seq ? gba->waitS32[region] : gba->waitN32[region]);
	}
	return 1 + (seq ? gba->waitS16[region] : gba->waitN16[region]);
}

// Moves the clock on and runs the events that have come due.
static inline __attribute__((always_inline)) void busTickInline(struct GBA* gba, int32_t cycles) {
	struct CPU* cpu = &gba->cpu;
	cpu->cycles += cycles;
	if (cpu->cycles >= cpu->nextEvent) {
		processEvents(gba);
	}
}

// A sequential cartridge access reads on from the cartridge's address counter, whatever address it
// asks for; a nonsequential one sets the counter. The counter covers 128 KiB, so the GBA starts a
// new burst at a boundary: for the CPU, at an access there; for DMA, at the access after one that
// ended there, wherever the counter has really got to.
static inline __attribute__((always_inline)) uint32_t cartridge(struct GBA* gba, uint32_t address, int size, bool* seq, bool dma) {
	if (!((dma ? gba->romNext : address) & 0x1FFFF)) {
		*seq = false;
	}
	gba->romNext = (address + size) & (SIZE_ROM - 1);
	if (!*seq) {
		gba->romLatch = address & (SIZE_ROM - size);
	}
	uint32_t latched = (address & ~(SIZE_ROM - 1)) | gba->romLatch;
	gba->romLatch = (gba->romLatch + size) & (SIZE_ROM - 1);
	return latched;
}

// Data on the cartridge bus stops the prefetcher, which costs a cycle if a read was just finishing.
// A code fetch from the cartridge already has the PC there.
static inline int32_t prefetcherStop(struct GBA* gba, bool codeFetch) {
	struct Prefetcher* p = &gba->prefetcher;
	if (!gba->cpu.prefetching) {
		return 0;
	}
	gba->cpu.prefetching = false;
	int32_t lead = timingCpuTime(gba) - p->base;
	if (lead >= p->full) {
		return 0;
	}
	if (!codeFetch) {
		// Compiled code leaves the PC to be worked out when something asks.
		jitSyncPc(gba);
		if (!inRom(gba->cpu.r[15])) {
			return 0;
		}
	}
	// Less than full: eight reads at most.
	int32_t into = lead;
	while (into >= p->duty) {
		into -= p->duty;
	}
	return into == p->duty - 1 || (p->width == 4 && into == p->duty - p->duty / 2 - 1);
}

// A code fetch from the cartridge: from the buffer, or once the read in flight finishes, or else as
// an ordinary access, after which the prefetcher reads on from there.
static inline __attribute__((always_inline)) int32_t prefetcherFetch(struct GBA* gba, uint32_t address, int size, bool seq) {
	struct Prefetcher* p = &gba->prefetcher;
	uint32_t now = timingCpuTime(gba);
	if (gba->cpu.prefetching && address == p->head) {
		uint32_t base = p->base;
		if ((int32_t) (now - base) > p->full) {
			base = now - p->full;
		}
		int32_t cycles = base + p->duty - now;
		p->base = base + p->duty;
		p->head += p->width;
		return cycles < 1 ? 1 : cycles;
	}
	int32_t cycles = prefetcherStop(gba, true);
	if (gba->prefetchDisabled) {
		gba->prefetchDisabled = false;
		seq = false;
	}
	cycles += busCyclesInline(gba, address, size, seq);
	if (gba->prefetchEnabled) {
		gba->cpu.prefetching = true;
		p->width = size;
		p->duty = busCyclesInline(gba, address, size, true);
		p->full = (size == 2 ? 8 : 4) * p->duty;
		p->head = address + size;
		p->base = now + cycles;
	}
	return cycles;
}

// Before any CPU access, DMA that is ready goes first, and the access after DMA is nonsequential.
static inline __attribute__((always_inline)) bool cpuAccess(struct GBA* gba, bool seq) {
	if (gba->dmaRunnable) {
		dmaRun(gba);
	}
	gba->dmaIdleCredit = 0;
	return seq && !gba->lastAccessDma;
}

// An instruction fetch. From the cartridge, it goes through the prefetch buffer, which may already
// have the word and so make the fetch cost one cycle.
static inline __attribute__((always_inline)) uint32_t busFetchInline(struct GBA* gba, uint32_t address, bool thumb) {
	struct CPU* cpu = &gba->cpu;
	// Work RAM, with no DMA waiting: what follows without the cartridge's counter and prefetch
	// buffer, or the search for where the code is.
	unsigned const region = address >> 24;
	if ((region == REGION_IWRAM || region == REGION_EWRAM) && !gba->dmaRunnable) {
		bool const seq = cpu->fetchSeq && !gba->lastAccessDma;
		gba->dmaIdleCredit = 0;
		cpu->fetchSeq = true;
		busTickInline(gba, 1 + (thumb ? (seq ? gba->waitS16[region] : gba->waitN16[region]) : (seq ? gba->waitS32[region] : gba->waitN32[region])));
		gba->lastAccessDma = false;
		const uint8_t* const memory = region == REGION_IWRAM ? &gba->iwram[address & (SIZE_IWRAM - 1) & -(thumb ? 2u : 4u)] :
		                                                       &gba->ewram[address & (SIZE_EWRAM - 1) & -(thumb ? 2u : 4u)];
		if (thumb) {
			uint16_t value;
			memcpy(&value, memory, 2);
			return value;
		}
		uint32_t value;
		memcpy(&value, memory, 4);
		return value;
	}
	int size = thumb ? 2 : 4;
	bool seq = cpuAccess(gba, cpu->fetchSeq);
	cpu->fetchSeq = true;
	address &= -size;
	if (inRom(address)) {
		cartridge(gba, address, size, &seq, false);
		busTickInline(gba, prefetcherFetch(gba, address, size, seq));
		gba->lastAccessDma = false;
		// The game's own ROM, read straight from it; anything else there (an empty slot, a cheat
		// cartridge's own code) by what it shows.
		uint32_t const offset = address & (SIZE_ROM - 1);
		if (offset < gba->romSize) {
			if (thumb) {
				uint16_t value;
				memcpy(&value, &gba->rom[offset], 2);
				return value;
			}
			uint32_t value;
			memcpy(&value, &gba->rom[offset], 4);
			return value;
		}
	} else {
		busTickInline(gba, busCyclesInline(gba, address, size, seq));
		gba->lastAccessDma = false;
	}
	return thumb ? codeFetch16(gba, address) : codeFetch32(gba, address);
}

#endif
