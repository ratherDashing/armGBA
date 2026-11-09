/* Copyright (c) 2026 armGBA 3DS port contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Executable memory for the recompiler, as DaedalusX64-3DS (which runs its MIPS recompiler on
// hardware) gets it: every kernel call opened to the process through svcBackdoor (PicoDrive's
// routine), then a page-aligned buffer made read-write-execute with svcControlProcessMemory
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
#include <stdlib.h>
#include <string.h>

#include "cpu/jit-internal.h"
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

static uint8_t codeBuffer[CODE_SIZE] __attribute__((aligned(0x1000)));
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

// The code memory: on a New 3DS, as much of the heap left after the game as can be spared, up to
// 16 MiB; the static 6 MiB otherwise. Advance Wars' battles alone need about 9 MiB of code: with
// only that, the recompiler threw everything away every few seconds and compiled it all again,
// stuttering each time (2026-11-09).
enum {
	HEAP_CODE_MAX = 16 << 20,
	HEAP_CODE_MIN = 8 << 20,
	// Left for the frontend and the core after the game has loaded.
	HEAP_RESERVE = 6 << 20,
};

void* jitHostAllocCode(size_t* size) {
	if (codeTaken) {
		logLine("jit: code memory already in use");
		return NULL;
	}
	if (isNew3ds()) {
		extern u32 __ctru_heap_size;
		struct mallinfo m = mallinfo();
		size_t const used = m.uordblks;
		size_t spare = __ctru_heap_size > used + HEAP_RESERVE ? __ctru_heap_size - used - HEAP_RESERVE : 0;
		spare &= ~(size_t) 0xFFFFF;
		if (spare > HEAP_CODE_MAX) {
			spare = HEAP_CODE_MAX;
		}
		while (spare >= HEAP_CODE_MIN && spare > sizeof(codeBuffer)) {
			uint8_t* code = memalign(0x1000, spare);
			if (code && makeExecutable(code, spare)) {
				heapCode = code;
				codeTaken = true;
				*size = spare;
				logLine("jit: %u KiB of code memory at %p (heap)", (unsigned) (spare >> 10), (void*) code);
				return code;
			}
			free(code);
			if (code) {
				// Not executable: the heap's memory can't be, so the static buffer it is.
				break;
			}
			spare -= 2 << 20;
		}
	}
	if (*size > sizeof(codeBuffer)) {
		*size = sizeof(codeBuffer);
	}
	if (!makeExecutable(codeBuffer, *size)) {
		logLine("jit: no executable memory: the recompiler can't run");
		return NULL;
	}
	codeTaken = true;
	logLine("jit: %u KiB of code memory at %p", (unsigned) (*size >> 10), (void*) codeBuffer);
	return codeBuffer;
}

// Puts the code memory back to read-write, as it was.
void jitHostRestoreMemory(void) {
	if (codeProtected) {
		Result rc = svcControlProcessMemory(self, (u32) (heapCode ? heapCode : codeBuffer), 0, codeProtected, MEMOP_PROT, MEMPERM_READ | MEMPERM_WRITE);
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
	}
}

uint64_t renderThreadHostTicks(void) {
	return svcGetSystemTick();
}
