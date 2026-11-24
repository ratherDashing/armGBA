/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gba.h"
#include "cpu/jit-internal.h"
#include "bus-inline.h"

#include <string.h>

// Timed bus accesses. Each moves the clock on by its own cycles and lets events due by then happen
// before it reads or writes, so it sees what the hardware would. A DMA waiting for the bus goes
// before the CPU's next access.

// 1 plus the wait states.
int32_t busCycles(struct GBA* gba, uint32_t address, int size, bool seq) {
	return busCyclesInline(gba, address, size, seq);
}

// Moves the clock on and runs the events that have come due.
void busTick(struct GBA* gba, int32_t cycles) {
	busTickInline(gba, cycles);
}

// An instruction fetch. From the cartridge, it goes through the prefetch buffer, which may already
// have the word and so make the fetch cost one cycle.
uint32_t busFetch(struct GBA* gba, uint32_t address, bool thumb) {
	return busFetchInline(gba, address, thumb);
}

// Reads or writes size bytes, without timing.
static uint32_t load(struct GBA* gba, uint32_t address, int size) {
	switch (size) {
	case 4:
		return memoryLoad32(gba, address, NULL);
	case 2:
		return memoryLoad16(gba, address, NULL);
	default:
		return memoryLoad8(gba, address, NULL);
	}
}

static void store(struct GBA* gba, uint32_t address, uint32_t value, int size) {
	switch (size) {
	case 4:
		memoryStore32(gba, address, value, NULL);
		break;
	case 2:
		memoryStore16(gba, address, value, NULL);
		break;
	default:
		memoryStore8(gba, address, value, NULL);
		break;
	}
}

// Returns what prefetcherStop would cost now for a code fetch, without stopping anything.
static int32_t stopCost(struct GBA* gba) {
	struct Prefetcher* p = &gba->prefetcher;
	if (!gba->cpu.prefetching) {
		return 0;
	}
	int32_t lead = timingCpuTime(gba) - p->base;
	if (lead >= p->full) {
		return 0;
	}
	int32_t into = lead;
	while (into >= p->duty) {
		into -= p->duty;
	}
	return into == p->duty - 1 || (p->width == 4 && into == p->duty - p->duty / 2 - 1);
}

// Makes the two fetches that refill the pipeline at pc in the cartridge, as busFetch makes them,
// when nothing can come between them: no DMA waiting, no event due, the buffer not already reading
// there, and no new 128 KiB burst for the second. Otherwise returns false, having done nothing.
bool busRefillCartridge(struct GBA* gba, uint32_t pc, bool thumb) {
	struct CPU* cpu = &gba->cpu;
	struct Prefetcher* p = &gba->prefetcher;
	int size = thumb ? 2 : 4;
	if (gba->dmaRunnable || gba->prefetchDisabled || (gba->cpu.prefetching && p->head == pc) || !((pc + size) & 0x1FFFF)) {
		return false;
	}
	unsigned region = pc >> 24;
	int32_t n = 1 + (thumb ? gba->waitN16[region] : gba->waitN32[region]);
	int32_t s = 1 + (thumb ? gba->waitS16[region] : gba->waitS32[region]);
	int32_t stop = stopCost(gba);
	if (cpu->cycles + stop + n + s >= cpu->nextEvent) {
		return false;
	}
	uint32_t now = timingCpuTime(gba);
	gba->dmaIdleCredit = 0;
	gba->romNext = (pc + 2 * size) & (SIZE_ROM - 1);
	gba->romLatch = gba->romNext;
	gba->cpu.prefetching = false;
	if (gba->prefetchEnabled) {
		// Started by the first fetch, which the second waits on.
		gba->cpu.prefetching = true;
		p->width = size;
		p->duty = s;
		p->full = (thumb ? 8 : 4) * s;
		p->head = pc + 2 * size;
		p->base = now + stop + n + s;
	}
	cpu->cycles += stop + n + s;
	cpu->fetchSeq = true;
	gba->lastAccessDma = false;
	cpu->prefetch[0] = thumb ? codeFetch16(gba, pc) : codeFetch32(gba, pc);
	cpu->prefetch[1] = thumb ? codeFetch16(gba, pc + size) : codeFetch32(gba, pc + size);
	return true;
}

// A data access's cycles; one on the cartridge bus stops the prefetcher first.
static int32_t dataCycles(struct GBA* gba, uint32_t address, int size, bool seq) {
	int32_t cycles = busCycles(gba, address, size, seq);
	unsigned region = address >> 24;
	if (region >= REGION_ROM0 && region <= REGION_SRAM_MIRROR) {
		cycles += prefetcherStop(gba, false);
	} else if (region == REGION_VRAM && gba->vramStall) {
		// The display has VRAM until H-blank.
		cycles += videoVramStall(gba, cycles);
	}
	return cycles;
}

// A data read by the CPU: events up to the end of the access happen first, then the value is read.
uint32_t busRead(struct GBA* gba, uint32_t address, int size, bool seq) {
	seq = cpuAccess(gba, seq);
	uint32_t at = address;
	if (inRom(address)) {
		at = cartridge(gba, address & -size, size, &seq, false) | (address & (size - 1));
	}
	busTick(gba, dataCycles(gba, address & -size, size, seq));
	uint32_t value = load(gba, at, size);
	gba->lastAccessDma = false;
	if (gba->idleProbing) {
		jitIdleRead(gba, address, size, value);
	}
	return value;
}

// A data write by the CPU, likewise.
void busWrite(struct GBA* gba, uint32_t address, uint32_t value, int size, bool seq) {
	if (gba->idleProbing) {
		jitIdleWrite(gba, address, value, size);
	}
	seq = cpuAccess(gba, seq);
	if (inRom(address)) {
		cartridge(gba, address & -size, size, &seq, false);
	}
	busTick(gba, dataCycles(gba, address & -size, size, seq));
	store(gba, address, value, size);
	gba->lastAccessDma = false;
}

// An internal cycle: a waiting DMA runs alongside, and covers the CPU's internal cycles while it
// lasts.
void busIdle(struct GBA* gba) {
	if (gba->dmaRunnable) {
		gba->dmaIdleCredit = dmaRun(gba);
	}
	if (gba->dmaIdleCredit) {
		--gba->dmaIdleCredit;
		return;
	}
	busTick(gba, 1);
}

// DMA's reads and writes: the same bus, without the CPU's prefetch.
uint32_t busDmaRead(struct GBA* gba, uint32_t address, int size, bool seq) {
	uint32_t at = address;
	if (inRom(address)) {
		at = cartridge(gba, address & -size, size, &seq, true);
	}
	busTick(gba, dataCycles(gba, address & -size, size, seq));
	uint32_t value = load(gba, at, size);
	gba->lastAccessDma = true;
	return value;
}

void busDmaWrite(struct GBA* gba, uint32_t address, uint32_t value, int size, bool seq) {
	if (inRom(address)) {
		cartridge(gba, address & -size, size, &seq, true);
	}
	busTick(gba, dataCycles(gba, address & -size, size, seq));
	store(gba, address, value, size);
	gba->lastAccessDma = true;
}
