/* Copyright (c) 2026 armGBA 3DS port contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Executable memory for the recompiler, as DaedalusX64-3DS (which runs its MIPS recompiler on
// hardware) gets it: every kernel call opened to the process through svcBackdoor (PicoDrive's
// routine), then the code memory (taken from the heap) made read-write-execute with svcControlProcessMemory
// (MEMOP_PROT). Luma3DS's svcControlProcess(PROCESSOP_SET_MMU_TO_RWX) is only a fallback, and what
// Azahar, which implements neither of the others, gets.
//
// After code is written, on hardware, DaedalusX64-3DS's cache routine runs in kernel mode: the whole
// data cache cleaned, the whole instruction cache and branch predictor invalidated. An earlier build
// used Luma3DS's by-address instruction cache call (0x93), which faulted on hardware inside the
// kernel on a page of the code buffer (2026-11-09).

#include "frontend.h"

#include <malloc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu/jit-internal.h"
#include "cpu/jit.h"
#include "gba.h"

Result svcLumaControlProcess(Handle process, u32 op, u32 arg2, u32 arg3);
// svc.s: DaedalusX64-3DS's kernel routines (from PicoDrive's 3DS port), proven on hardware.
s32 ctrEnableAllSvcsKernel(void);
void ctrFlushCachesKernel(void);
void ctrInvalidateICacheKernel(void);
void ctrFlushRangeKernel(void);
// The range ctrFlushRangeKernel works on.
u32 jitFlushFrom, jitFlushTo;
Result svcLumaInvalidateInstructionCacheRange(u32 address, u32 size);

enum {
	PROCESSOP_SET_MMU_TO_RWX = 1,
};

static bool codeTaken;
// Made executable with svcControlProcessMemory, so to be put back before the app exits: a process
// that ends with memory still read-write-execute hangs the system on "Closing software".
static size_t codeProtected;
// The code memory when it came from the heap.
static uint8_t* heapCode;
uint32_t jitFlushes;
uint64_t jitFlushTicks;
// Whole: the earlier way (the whole instruction cache), kept for if the range one ever faults.
bool jitFlushWhole;
int renderThreadCore = -1;
// Whether a second worker draws every other line on core 3.
bool renderThreadSecond;
static Handle self;

// Makes size bytes at code read-write-execute. The CIA's permissions allow svcControlProcessMemory;
// only if it's refused (a launcher with fewer) is DaedalusX64-3DS's way used: every kernel call
// opened to this process through svcBackdoor, then the call again. Azahar answers neither the way
// hardware does (svcControlProcessMemory isn't implemented there and returns whatever was in r0), so
// only the Luma3DS call is made there.
static bool makeExecutable(uint8_t* code, size_t size) {
	if (!self && R_FAILED(svcDuplicateHandle(&self, CUR_PROCESS_HANDLE))) {
		self = 0;
	}
	Result rc = -1;
	if (!isEmulator()) {
		rc = svcControlProcessMemory(self, (u32) code, 0, size, MEMOP_PROT, MEMPERM_READ | MEMPERM_WRITE | MEMPERM_EXECUTE);
		if (R_FAILED(rc)) {
			logLine("jit: svcControlProcessMemory refused (%08lX): opening kernel calls", (unsigned long) rc);
			svcBackdoor(ctrEnableAllSvcsKernel);
			svcBackdoor(ctrEnableAllSvcsKernel);
			rc = svcControlProcessMemory(self, (u32) code, 0, size, MEMOP_PROT, MEMPERM_READ | MEMPERM_WRITE | MEMPERM_EXECUTE);
		}
	}
	Result rc2 = 0;
	if (R_FAILED(rc)) {
		rc2 = svcLumaControlProcess(self, PROCESSOP_SET_MMU_TO_RWX, 0, 0);
	}
	logLine("jit: svcControlProcessMemory %08lX%s, svcControlProcess(RWX) %08lX%s", (unsigned long) rc, isEmulator() ? " (skipped)" : "",
	        (unsigned long) rc2, R_FAILED(rc) ? "" : " (not needed)");
	if (R_FAILED(rc) && R_FAILED(rc2)) {
		return false;
	}
	codeProtected = R_SUCCEEDED(rc) ? size : 0;
	return true;
}

// The code memory: two thirds of the heap left after the game, up to 24 MiB (A32's branches reach 32);
// the rest is for what the recompiler keeps about each block, about 0.4 bytes for each byte of
// code (jitHostMemoryLow). Advance Wars' battles need about 9 MiB of code and Mario Kart's
// races more: when either runs out, the recompiler throws everything away and compiles it all again,
// a stutter each time (2026-11-09).
enum {
	HEAP_CODE_MAX = 24 << 20,
	HEAP_CODE_MIN = 4 << 20,
	// Kept free while compiling: what the frontend and the core allocate as they go, saved states
	// among it.
	HEAP_LOW = 2 << 20,
	// Allocated after the code memory: the render thread's log.
	HEAP_LATER = 2 << 20,
};

bool jitHostMemoryLow(void) {
	extern struct GBA* gba;
	extern u32 __ctru_heap_size;
	// mallinfo walks the heap's free lists, so only every so often.
	static int calls;
	static bool low;
	// The heap free just after the last flush: a flush helps only if compiling has taken some since.
	static uint64_t seenFlushes = ~0ull;
	static size_t freeAfterFlush;
	struct Jit* const jit = gba ? gba->jit : NULL;
	bool const flushed = jit && jit->flushes != seenFlushes;
	if (flushed || low || (calls++ & 15) == 0) {
		struct mallinfo m = mallinfo();
		size_t const free = __ctru_heap_size - (size_t) m.uordblks;
		if (flushed) {
			seenFlushes = jit->flushes;
			freeAfterFlush = free;
		}
		low = free < HEAP_LOW && freeAfterFlush > free + (1 << 20);
	}
	return low;
}

void* jitHostAllocCode(size_t* size) {
	if (codeTaken) {
		logLine("jit: code memory already in use");
		return NULL;
	}
	extern u32 __ctru_heap_size;
	struct mallinfo m = mallinfo();
	size_t const used = m.uordblks;
	size_t const reserve = HEAP_LOW + HEAP_LATER;
	size_t spare = __ctru_heap_size > used + reserve ? (__ctru_heap_size - used - reserve) / 3 * 2 : 0;
	spare &= ~(size_t) 0xFFFFF;
	if (spare > *size) {
		spare = *size;
	}
	if (spare > HEAP_CODE_MAX) {
		spare = HEAP_CODE_MAX;
	}
	for (; spare >= HEAP_CODE_MIN; spare -= 1 << 20) {
		uint8_t* code = memalign(0x1000, spare);
		if (!code) {
			continue;
		}
		if (!makeExecutable(code, spare)) {
			free(code);
			break;
		}
		heapCode = code;
		codeTaken = true;
		*size = spare;
		logLine("jit: %u KiB of code memory at %p", (unsigned) (spare >> 10), (void*) code);
		return code;
	}
	logLine("jit: no executable memory: the recompiler can't run");
	return NULL;
}

// Puts the code memory back to read-write, as it was.
void jitHostRestoreMemory(void) {
	if (codeProtected) {
		Result rc = svcControlProcessMemory(self, (u32) heapCode, 0, codeProtected, MEMOP_PROT, MEMPERM_READ | MEMPERM_WRITE);
		logLine("jit: code memory back to read-write: %08lX", (unsigned long) rc);
		codeProtected = 0;
	}
}

void jitHostFreeCode(void* code, size_t size) {
	(void) code;
	(void) size;
	jitHostRestoreMemory();
	free(heapCode);
	heapCode = NULL;
	codeTaken = false;
}

// The render thread on a core of its own: the New 3DS's third (core 2), else the system's core
// (core 1) with most of its time given to the app. At the game thread's priority. Not on an Old 3DS,
// whose system core is already busy with the system.
bool renderThreadHostCreate(pthread_t* thread, void* (*function)(void*), void* argument) {
	if (!isNew3ds()) {
		logLine("render thread: not on an Old 3DS");
		return false;
	}
	s32 priority = 0x30;
	svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
	Thread t = threadCreate((ThreadFunc) (void*) function, argument, 256 * 1024, priority, 2, false);
	renderThreadCore = 2;
	if (!t) {
		logLine("render thread: core 2 refused");
		Result rc = APT_SetAppCpuTimeLimit(80);
		t = threadCreate((ThreadFunc) (void*) function, argument, 256 * 1024, priority, 1, false);
		renderThreadCore = 1;
		logLine("render thread: core 1 with 80%% of its time (%08lX)", (unsigned long) rc);
	}
	logLine("render thread on core %d: %s", renderThreadCore, t ? "started" : "couldn't start");
	if (!t) {
		renderThreadCore = -1;
		return false;
	}
	// libctru's pthread_t is its Thread, so pthread_join waits for this one.
	*thread = (pthread_t) t;
	return true;
}

// A second worker on the New 3DS's fourth core, each drawing every other line. Without it the one
// worker draws them all.
bool renderThreadHostCreateSecond(pthread_t* thread, void* (*function)(void*), void* argument) {
	renderThreadSecond = false;
	if (renderThreadCore != 2) {
		return false;
	}
	s32 priority = 0x30;
	svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
	Thread t = threadCreate((ThreadFunc) (void*) function, argument, 256 * 1024, priority, 3, false);
	logLine("second render thread on core 3: %s", t ? "started" : "refused");
	if (!t) {
		return false;
	}
	renderThreadSecond = true;
	*thread = (pthread_t) t;
	return true;
}

void jitHostFlushCode(void* start, void* end) {
	u32 const from = (u32) start & ~31u;
	u32 const to = ((u32) end + 31) & ~31u;
	// On hardware, one kernel routine cleans the data cache and invalidates the instruction cache for
	// just the lines written (block links and patches are a word or two, thousands a second). In
	// Azahar (no svcCustomBackdoor, and a whole invalidate drops all its own recompiled code) the
	// range through Luma3DS's call.
	uint64_t const before = svcGetSystemTick();
	++jitFlushes;
	if (isEmulator()) {
		svcFlushProcessDataCache(CUR_PROCESS_HANDLE, from, to - from);
		svcLumaInvalidateInstructionCacheRange(from, to - from);
	} else if (jitFlushWhole || to - from > 64 * 1024) {
		svcFlushProcessDataCache(CUR_PROCESS_HANDLE, from, to - from);
		ctrInvalidateICacheKernel();
	} else {
		jitFlushFrom = from;
		jitFlushTo = to;
		ctrFlushRangeKernel();
	}
	jitFlushTicks += svcGetSystemTick() - before;
}

extern struct GBA* gba;

void jitCounts(struct JitCounts* c) {
	memset(c, 0, sizeof(*c));
	if (gba && gba->jit) {
		struct Jit* jit = gba->jit;
		c->compiled = jit->compiled;
		c->invalidations = jit->invalidations;
		c->patches = jit->patchWrites;
		c->revived = jit->revived;
		c->stepped = jit->stepped;
		c->codeWrites = jit->codeWrites;
		c->fullFlushes[0] = jit->flushReasons[0];
		c->fullFlushes[1] = jit->flushReasons[1];
		for (int i = 0; i < 5; ++i) {
			c->exits[i] = jit->exits[i];
		}
		c->codeUsed = jit->codeNext && jit->codeStart ? (uint32_t) (jit->codeNext - jit->codeStart) : 0;
		c->codeSize = (uint32_t) jit->codeSize;
		c->hotBytes = jit->hotBytes;
		c->coldBytes = jit->coldBytes;
		c->wastedBytes = jit->wastedBytes;
		c->carefulCompiles = jit->carefulCompiles;
		c->carefulBytes = jit->carefulBytes;
		c->carefulPutOff = jit->carefulPutOff;
		c->deferred = jit->deferred;
	}
}

// The warm list (cpu/jit.h) kept on the SD card for each game: which of the cartridge's code the
// recompiler compiled. The next time the game is played it is compiled in the time frames have to
// spare, before the game gets to it. Without it, the second a Mario Kart race started ran 4,900
// pieces of code for the first time and dropped to 56 fps (2026-11-09).
enum { WARM_MAGIC = 0x31574741 };

static uint32_t warmRomId(void) {
	uint32_t h = 2166136261u ^ (uint32_t) gba->romSize;
	for (size_t i = 0; i < 4096 && i < gba->romSize; ++i) {
		h = (h ^ gba->rom[i]) * 16777619u;
	}
	return h;
}

void jitWarmLoad(const char* path) {
	if (!gba || !gba->jit || !gba->rom) {
		return;
	}
	size_t size = 0;
	uint32_t* data = (uint32_t*) readFile(path, &size);
	if (data && size >= 12 && data[0] == WARM_MAGIC && data[1] == warmRomId() && data[2] <= (size - 12) / 8) {
		jitWarmGive(gba, data + 3, data[2]);
		logLine("warm list: %lu blocks from %s", (unsigned long) data[2], path);
	}
	free(data);
}

void jitWarmSave(const char* path) {
	if (!gba || !gba->jit || !gba->rom) {
		return;
	}
	enum { MOST = 1 << 16 };
	uint32_t* data = malloc(12 + MOST * 8);
	if (!data) {
		return;
	}
	size_t const count = jitWarmTake(gba, data + 3, MOST);
	data[0] = WARM_MAGIC;
	data[1] = warmRomId();
	data[2] = (uint32_t) count;
	if (count) {
		char temp[800];
		snprintf(temp, sizeof(temp), "%s.tmp", path);
		FILE* f = fopen(temp, "wb");
		if (f) {
			bool const ok = fwrite(data, 1, 12 + count * 8, f) == 12 + count * 8;
			fclose(f);
			if (ok) {
				remove(path);
				rename(temp, path);
			} else {
				remove(temp);
			}
		}
		logLine("warm list: %lu blocks to %s", (unsigned long) count, path);
	}
	free(data);
}

bool jitWarmOne(void) {
	return gba && gba->jit && jitWarmStep(gba);
}

uint64_t renderThreadHostTicks(void) {
	return svcGetSystemTick();
}
