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

#include <stdlib.h>

#include "cpu/jit-internal.h"

Result svcLumaControlProcess(Handle process, u32 op, u32 arg2, u32 arg3);
// svc.s: DaedalusX64-3DS's kernel routines (from PicoDrive's 3DS port), proven on hardware.
s32 ctrEnableAllSvcsKernel(void);
void ctrFlushCachesKernel(void);
Result svcLumaInvalidateInstructionCacheRange(u32 address, u32 size);

enum {
	PROCESSOP_SET_MMU_TO_RWX = 1,
};

static uint8_t codeBuffer[CODE_SIZE] __attribute__((aligned(0x1000)));
static bool codeTaken;
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
	// As DaedalusX64-3DS does first: every kernel call opened to this process, so that
	// svcControlProcessMemory is allowed however the app was launched. Azahar has no svcBackdoor.
	if (!isEmulator()) {
		svcBackdoor(ctrEnableAllSvcsKernel);
		svcBackdoor(ctrEnableAllSvcsKernel);
	}
	// Azahar answers neither call the way hardware does: there svcControlProcessMemory isn't
	// implemented (and returns whatever was in r0), so only the Luma3DS call is made.
	Result rc = isEmulator() ? -1 : svcControlProcessMemory(self, (u32) codeBuffer, 0, size, MEMOP_PROT,
	                                                       MEMPERM_READ | MEMPERM_WRITE | MEMPERM_EXECUTE);
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
	logLine("jit: %u KiB of code memory at %p", (unsigned) (size >> 10), (void*) codeBuffer);
	return codeBuffer;
}

void jitHostFreeCode(void* code, size_t size) {
	(void) code;
	(void) size;
	codeTaken = false;
}

void jitHostFlushCode(void* start, void* end) {
	u32 const from = (u32) start & ~31u;
	u32 const size = (((u32) end + 31) & ~31u) - from;
	// On hardware, DaedalusX64-3DS's kernel routine: the whole data cache cleaned, the whole
	// instruction cache and branch predictor invalidated. In Azahar (no svcCustomBackdoor, and a whole
	// invalidate drops all its own recompiled code, which after every block makes it crawl) the range.
	if (isEmulator()) {
		svcFlushProcessDataCache(CUR_PROCESS_HANDLE, from, size);
		svcLumaInvalidateInstructionCacheRange(from, size);
	} else {
		ctrFlushCachesKernel();
	}
}
