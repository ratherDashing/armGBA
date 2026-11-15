@ Kernel calls Luma3DS adds (and Azahar answers), which libctru has no wrappers for.
	.arm
	.section .text.svcLuma, "ax", %progbits
	.align 2

	.global svcLumaControlProcess
	.type svcLumaControlProcess, %function
svcLumaControlProcess:
	svc 0xB3
	bx lr

	.global svcLumaInvalidateInstructionCacheRange
	.type svcLumaInvalidateInstructionCacheRange, %function
svcLumaInvalidateInstructionCacheRange:
	svc 0x93
	bx lr

	.global svcLumaInvalidateEntireInstructionCache
	.type svcLumaInvalidateEntireInstructionCache, %function
svcLumaInvalidateEntireInstructionCache:
	svc 0x94
	bx lr

@ The same cache maintenance DaedalusX64-3DS (and PicoDrive's libretro port before it) uses on
@ hardware, run in kernel mode through Luma3DS's svcCustomBackdoor (0x80): the whole data cache
@ cleaned, the whole instruction cache and the branch predictor invalidated, with barriers.
	.section .text.cacheKernel, "ax", %progbits
	.align 2
cacheKernel:
	cpsid aif
	mov r0, #0
	mcr p15, 0, r0, c7, c10, 0	@ clean the entire data cache
	mcr p15, 0, r0, c7, c10, 5	@ data memory barrier
	mcr p15, 0, r0, c7, c5, 0	@ invalidate the entire instruction cache, flush the BTB
	mcr p15, 0, r0, c7, c10, 4	@ data synchronization barrier
	bx lr

	.global ctrFlushCachesKernel
	.type ctrFlushCachesKernel, %function
ctrFlushCachesKernel:
	ldr r0, =cacheKernel
	svc 0x80
	bx lr
	.pool

@ PicoDrive's (via DaedalusX64-3DS) kernel routine, run through svcBackdoor: opens every kernel call
@ to this process, so svcControlProcessMemory is allowed whatever the launcher's permissions are.
	.section .text.allSvcs, "ax", %progbits
	.align 2
	.global ctrEnableAllSvcsKernel
	.type ctrEnableAllSvcsKernel, %function
ctrEnableAllSvcsKernel:
	cpsid aif
	ldr r0, =0xFFFF9000		@ the current KThread
	ldr r0, [r0]
	ldr r0, [r0, #0x88]		@ (0x22 words in): its kernel stack top's context
	sub r0, r0, #0x18		@ the svc access control mask, 6 words below
	mvn r1, #1			@ 0xFFFFFFFE
	str r1, [r0]
	mvn r1, #0			@ 0xFFFFFFFF
	str r1, [r0, #4]
	str r1, [r0, #8]
	mvn r1, #0xC0000000		@ 0x3FFFFFFF
	str r1, [r0, #12]
	bx lr
	.pool

@ The instruction cache and branch predictor invalidated, in kernel mode, after the data cache has
@ been cleaned for the range written (svcFlushProcessDataCache): DaedalusX64-3DS's routine without
@ its whole-data-cache clean, which the recompiler would otherwise pay on every block link.
	.section .text.icacheKernel, "ax", %progbits
	.align 2
icacheKernel:
	cpsid aif
	mov r0, #0
	mcr p15, 0, r0, c7, c10, 4	@ data synchronization barrier
	mcr p15, 0, r0, c7, c5, 0	@ invalidate the entire instruction cache
	mcr p15, 0, r0, c7, c5, 6	@ flush the entire branch target cache
	mcr p15, 0, r0, c7, c10, 4	@ data synchronization barrier
	mcr p15, 0, r0, c7, c5, 4	@ flush the prefetch buffer
	bx lr

	.global ctrInvalidateICacheKernel
	.type ctrInvalidateICacheKernel, %function
ctrInvalidateICacheKernel:
	ldr r0, =icacheKernel
	svc 0x80
	bx lr
	.pool

@ The range just written, line by line, in kernel mode (one call instead of svcFlushProcessDataCache
@ plus a whole-cache invalidate): each data cache line cleaned, each instruction cache line
@ invalidated by address, then the branch predictor flushed. jitFlushFrom and jitFlushTo (32-byte
@ aligned, in the code buffer, which is mapped executable) are read from the process's memory,
@ which kernel mode sees while this process runs.
	.section .text.rangeKernel, "ax", %progbits
	.align 2
rangeKernel:
	cpsid aif
	ldr r0, =jitFlushFrom
	ldr r0, [r0]
	ldr r1, =jitFlushTo
	ldr r1, [r1]
	mov r2, r0
1:	mcr p15, 0, r2, c7, c10, 1	@ clean the data cache line
	add r2, r2, #32
	cmp r2, r1
	blo 1b
	mov r3, #0
	mcr p15, 0, r3, c7, c10, 4	@ data synchronization barrier
	mov r2, r0
2:	mcr p15, 0, r2, c7, c5, 1	@ invalidate the instruction cache line
	add r2, r2, #32
	cmp r2, r1
	blo 2b
	mcr p15, 0, r3, c7, c5, 6	@ flush the entire branch target cache
	mcr p15, 0, r3, c7, c10, 4	@ data synchronization barrier
	mcr p15, 0, r3, c7, c5, 4	@ flush the prefetch buffer
	bx lr
	.pool

	.global ctrFlushRangeKernel
	.type ctrFlushRangeKernel, %function
ctrFlushRangeKernel:
	ldr r0, =rangeKernel
	svc 0x80
	bx lr
	.pool

@ The ARM11's cycle counter, read in kernel mode: started (without the divide-by-64) if it isn't,
@ its count stored in ctrCycles. With the system tick it gives the real CPU clock.
	.section .text.cyclesKernel, "ax", %progbits
	.align 2
cyclesKernel:
	cpsid aif
	mrc p15, 0, r0, c15, c12, 0	@ performance monitor control
	bic r0, r0, #8			@ count every cycle, not every 64th
	orr r0, r0, #1			@ enabled
	mcr p15, 0, r0, c15, c12, 0
	mrc p15, 0, r0, c15, c12, 1	@ the cycle count
	ldr r1, =ctrCycles
	str r0, [r1]
	bx lr
	.pool

@ The ARM11's two event counters and its cycle counter, read in kernel mode into pmuRead (events 0
@ and 1, then cycles); then the control register is written from pmuControl, which chooses the next
@ two events, clears all three counts and starts them. A development build's log uses it to say what
@ the emulation's time goes on (main.c).
	.section .text.pmuKernel, "ax", %progbits
	.align 2
pmuKernel:
	cpsid aif
	ldr r1, =pmuRead
	mrc p15, 0, r0, c15, c12, 2
	str r0, [r1]
	mrc p15, 0, r0, c15, c12, 3
	str r0, [r1, #4]
	mrc p15, 0, r0, c15, c12, 1
	str r0, [r1, #8]
	ldr r1, =pmuControl
	ldr r0, [r1]
	mcr p15, 0, r0, c15, c12, 0
	bx lr
	.pool

	.global ctrPmuKernel
	.type ctrPmuKernel, %function
ctrPmuKernel:
	ldr r0, =pmuKernel
	svc 0x80
	bx lr
	.pool

	.global ctrReadCycles
	.type ctrReadCycles, %function
ctrReadCycles:
	ldr r0, =cyclesKernel
	svc 0x80
	ldr r0, =ctrCycles
	ldr r0, [r0]
	bx lr
	.pool
