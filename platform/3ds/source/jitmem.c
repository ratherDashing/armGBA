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

#include <pthread.h>
#include <stdlib.h>

#include "cpu/jit-internal.h"

Result svcLumaControlProcess(Handle process, u32 op, u32 arg2, u32 arg3);
// svc.s: DaedalusX64-3DS's kernel routines (from PicoDrive's 3DS port), proven on hardware.
s32 ctrEnableAllSvcsKernel(void);
void ctrFlushCachesKernel(void);
void ctrInvalidateICacheKernel(void);
Result svcLumaInvalidateInstructionCacheRange(u32 address, u32 size);

enum {
	PROCESSOP_SET_MMU_TO_RWX = 1,
};

static uint8_t codeBuffer[CODE_SIZE] __attribute__((aligned(0x1000)));
static bool codeTaken;
// Made executable with svcControlProcessMemory, so to be put back before the app exits: a process
// that ends with memory still read-write-execute hangs the system on "Closing software".
static size_t codeProtected;
uint32_t jitFlushes;
int renderThreadCore = -1;
static Handle self;

void* jitHostAllocCode(size_t size) {
	if (codeTaken || size > sizeof(codeBuffer)) {
		logLine("jit: code memory %s", codeTaken ? "already in use" : "too small");
		return NULL;
	}
	if (!self && R_FAILED(svcDuplicateHandle(&self, CUR_PROCESS_HANDLE))) {
		self = 0;
	}
	size = (size + 0xFFF) & ~0xFFFu;
	// The CIA's permissions allow svcControlProcessMemory. Only if it's refused (a launcher with fewer)
	// is DaedalusX64-3DS's way used: every kernel call opened to this process through svcBackdoor,
	// then the call again. Azahar answers neither the way hardware does (svcControlProcessMemory isn't
	// implemented there and returns whatever was in r0), so only the Luma3DS call is made there.
	Result rc = -1;
	if (!isEmulator()) {
		rc = svcControlProcessMemory(self, (u32) codeBuffer, 0, size, MEMOP_PROT, MEMPERM_READ | MEMPERM_WRITE | MEMPERM_EXECUTE);
		if (R_FAILED(rc)) {
			logLine("jit: svcControlProcessMemory refused (%08lX): opening kernel calls", (unsigned long) rc);
			svcBackdoor(ctrEnableAllSvcsKernel);
			svcBackdoor(ctrEnableAllSvcsKernel);
			rc = svcControlProcessMemory(self, (u32) codeBuffer, 0, size, MEMOP_PROT, MEMPERM_READ | MEMPERM_WRITE | MEMPERM_EXECUTE);
		}
	}
	Result rc2 = 0;
	if (R_FAILED(rc)) {
		rc2 = svcLumaControlProcess(self, PROCESSOP_SET_MMU_TO_RWX, 0, 0);
	}
	logLine("jit: svcControlProcessMemory %08lX%s, svcControlProcess(RWX) %08lX%s", (unsigned long) rc, isEmulator() ? " (skipped)" : "",
	        (unsigned long) rc2, R_FAILED(rc) ? "" : " (not needed)");
	if (R_FAILED(rc) && R_FAILED(rc2)) {
		logLine("jit: no executable memory: the recompiler can't run");
		return NULL;
	}
	codeTaken = true;
	codeProtected = R_SUCCEEDED(rc) ? size : 0;
	logLine("jit: %u KiB of code memory at %p", (unsigned) (size >> 10), (void*) codeBuffer);
	return codeBuffer;
}

// Puts the code buffer back to read-write, as it was.
void jitHostRestoreMemory(void) {
	if (codeProtected) {
		Result rc = svcControlProcessMemory(self, (u32) codeBuffer, 0, codeProtected, MEMOP_PROT, MEMPERM_READ | MEMPERM_WRITE);
		logLine("jit: code memory back to read-write: %08lX", (unsigned long) rc);
		codeProtected = 0;
	}
}

void jitHostFreeCode(void* code, size_t size) {
	(void) code;
	(void) size;
	jitHostRestoreMemory();
	codeTaken = false;
}

// The render thread on a core of its own: the New 3DS's third (the second is the system's), at the
// game thread's priority. Not on an Old 3DS, whose other core the system mostly keeps.
bool renderThreadHostCreate(pthread_t* thread, void* (*function)(void*), void* argument) {
	if (!isNew3ds()) {
		logLine("render thread: not on an Old 3DS");
		return false;
	}
	s32 priority = 0x30;
	svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
	Thread t = threadCreate((ThreadFunc) (void*) function, argument, 256 * 1024, priority, 2, false);
	logLine("render thread on core 2: %s", t ? "started" : "couldn't start");
	if (!t) {
		return false;
	}
	renderThreadCore = 2;
	// libctru's pthread_t is its Thread, so pthread_join waits for this one.
	*thread = (pthread_t) t;
	return true;
}

void jitHostFlushCode(void* start, void* end) {
	u32 const from = (u32) start & ~31u;
	u32 const size = (((u32) end + 31) & ~31u) - from;
	// On hardware, DaedalusX64-3DS's kernel routine: the whole data cache cleaned, the whole
	// instruction cache and branch predictor invalidated. In Azahar (no svcCustomBackdoor, and a whole
	// invalidate drops all its own recompiled code, which after every block makes it crawl) the range.
	++jitFlushes;
	svcFlushProcessDataCache(CUR_PROCESS_HANDLE, from, size);
	if (isEmulator()) {
		svcLumaInvalidateInstructionCacheRange(from, size);
	} else {
		ctrInvalidateICacheKernel();
	}
}
