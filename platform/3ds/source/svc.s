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
