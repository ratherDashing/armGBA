/* Copyright (c) 2026 armGBA 3DS port contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Executable memory for the recompiler. The 3DS kernel only maps code from the program itself, so
// the code buffer is ordinary heap made executable by a kernel call: Luma3DS's svcControlProcess
// (PROCESSOP_SET_MMU_TO_RWX, which Azahar answers too), else svcControlProcessMemory, which
// Luma3DS lets homebrew use. After code is written, the data cache is written back and the
// instruction cache dropped for that range.

#include "frontend.h"

#include <malloc.h>
#include <stdlib.h>

Result svcLumaControlProcess(Handle process, u32 op, u32 arg2, u32 arg3);
Result svcLumaInvalidateInstructionCacheRange(u32 address, u32 size);

enum {
	PROCESSOP_SET_MMU_TO_RWX = 1,
};

static Handle self;

void* jitHostAllocCode(size_t size) {
	size = (size + 0xFFF) & ~0xFFFu;
	void* code = memalign(0x1000, size);
	if (!code) {
		logLine("jit: no memory for %u KiB of code", (unsigned) (size >> 10));
		return NULL;
	}
	if (!self && R_FAILED(svcDuplicateHandle(&self, CUR_PROCESS_HANDLE))) {
		self = 0;
	}
	Result rc = svcControlProcessMemory(self, (u32) code, 0, size, MEMOP_PROT, MEMPERM_READ | MEMPERM_WRITE | MEMPERM_EXECUTE);
	if (R_FAILED(rc) || isEmulator()) {
		// Azahar has no svcControlProcessMemory, and maps everything executable with this.
		Result rc2 = svcLumaControlProcess(self, PROCESSOP_SET_MMU_TO_RWX, 0, 0);
		logLine("jit: svcControlProcessMemory %08lX, svcControlProcess(RWX) %08lX", rc, rc2);
		if (R_FAILED(rc2) && R_FAILED(rc)) {
			free(code);
			return NULL;
		}
	}
	logLine("jit: %u KiB of code memory at %p", (unsigned) (size >> 10), code);
	return code;
}

void jitHostFreeCode(void* code, size_t size) {
	(void) size;
	free(code);
}

void jitHostFlushCode(void* start, void* end) {
	u32 const from = (u32) start & ~31u;
	u32 const size = (((u32) end + 31) & ~31u) - from;
	svcFlushProcessDataCache(CUR_PROCESS_HANDLE, from, size);
	svcLumaInvalidateInstructionCacheRange(from, size);
}
