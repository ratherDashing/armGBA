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
