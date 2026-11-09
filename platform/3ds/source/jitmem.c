/* Copyright (c) 2026 armGBA 3DS port contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Executable memory for the recompiler, the way the 3DS dynarecs that run on hardware (pcsx_rearmed,
// gpSP) get it: a page-aligned buffer in the program's own .bss, made read-write-execute with
// svcControlProcessMemory(MEMOP_PROT), which Luma3DS lets homebrew use. Only if that fails (Azahar
// doesn't implement it) is Luma3DS's svcControlProcess(PROCESSOP_SET_MMU_TO_RWX) used, which
// rewrites the whole process's page tables.
//
// After code is written, the data cache is written back for the range (svcFlushProcessDataCache) and
// the whole instruction cache dropped (Luma3DS's svcInvalidateEntireInstructionCache, 0x94). The
// by-address one (0x93) faulted on hardware, inside the kernel, on a page of the code buffer
// (an "instruction cache maintenance" data abort, 2026-11-09); the whole-cache operation takes no
// address, so it can't. (Azahar keeps the by-address one: see jitHostFlushCode.)

#include "frontend.h"

#include <stdlib.h>

#include "cpu/jit-internal.h"

Result svcLumaControlProcess(Handle process, u32 op, u32 arg2, u32 arg3);
Result svcLumaInvalidateEntireInstructionCache(void);
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
	svcFlushProcessDataCache(CUR_PROCESS_HANDLE, from, size);
	// In Azahar, dropping the whole instruction cache throws away all of its own recompiled code,
	// which after every block makes it crawl; there the by-address call is safe.
	if (isEmulator()) {
		svcLumaInvalidateInstructionCacheRange(from, size);
	} else {
		svcLumaInvalidateEntireInstructionCache();
	}
}
