/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The exact CPU, table-driven: each instruction goes straight from its opcode bits to a handler that
// decodes and runs that kind of instruction, with its bus accesses in the same order as step.c's,
// so the machine ends up exactly where step.c leaves it. Only Wii U Virtual Console calls, tracing
// and a cheat cartridge's trigger go through step.c.

#include "bus-inline.h"
#include "decode.h"
#include "gba.h"

typedef void (*LeanHandler)(struct GBA*, uint32_t opcode, uint32_t address);

// ARM opcodes by bits 27-20 and 7-4; Thumb by bits 15-6.
static LeanHandler armTable[4096];
static LeanHandler thumbTable[1024];

// Whether the flags satisfy an instruction's condition field, as step.c's passes().
static inline __attribute__((always_inline)) bool passes(uint32_t cpsr, int cond) {
	bool n = cpsr & PSR_N;
	bool z = cpsr & PSR_Z;
	bool c = cpsr & PSR_C;
	bool v = cpsr & PSR_V;
	switch (cond) {
	case 0x0:
		return z;
	case 0x1:
		return !z;
	case 0x2:
		return c;
	case 0x3:
		return !c;
	case 0x4:
		return n;
	case 0x5:
		return !n;
	case 0x6:
		return v;
	case 0x7:
		return !v;
	case 0x8:
		return c && !z;
	case 0x9:
		return !c || z;
	case 0xA:
		return n == v;
	case 0xB:
		return n != v;
	case 0xC:
		return !z && n == v;
	case 0xD:
		return z || n != v;
	case 0xE:
		return true;
	default:
		return false;
	}
}

// Any ARM instruction, by step.c's decoder and executor. Its condition has passed.
static void armGeneric(struct GBA* gba, uint32_t opcode, uint32_t address) {
	struct Inst in;
	decodeArm(&in, address, opcode);
	stepExecute(gba, &in);
}

// Any Thumb instruction, likewise; only conditional branches have a condition.
static void thumbGeneric(struct GBA* gba, uint32_t opcode, uint32_t address) {
	struct Inst in;
	decodeThumb(&in, address, (uint16_t) opcode);
	if (in.cond == COND_AL || passes(gba->cpu.cpsr, in.cond)) {
		stepExecute(gba, &in);
	}
}

// Sets N and Z from a result.
static inline void setNz(struct CPU* cpu, uint32_t d) {
	cpu->cpsr = (cpu->cpsr & ~(PSR_N | PSR_Z)) | (d & PSR_N) | (d ? 0 : PSR_Z);
}

// N, Z, C and V from an addition's or subtraction's result.
static inline void setNzcv(struct CPU* cpu, uint32_t d, bool c, bool v) {
	cpu->cpsr = (cpu->cpsr & ~(PSR_N | PSR_Z | PSR_C | PSR_V)) | (d & PSR_N) | (d ? 0 : PSR_Z) | (c ? PSR_C : 0) | (v ? PSR_V : 0);
}

static inline uint32_t add(struct CPU* cpu, uint32_t n, uint32_t m) {
	uint32_t d = n + m;
	setNzcv(cpu, d, d < n, (~(n ^ m) & (n ^ d)) >> 31);
	return d;
}

static inline uint32_t sub(struct CPU* cpu, uint32_t n, uint32_t m) {
	uint32_t d = n - m;
	setNzcv(cpu, d, n >= m, ((n ^ m) & (n ^ d)) >> 31);
	return d;
}

// Thumb LSL, LSR and ASR by a constant: MOVS of a register shifted by it.
static void thumbShiftImm(struct GBA* gba, uint32_t opcode, uint32_t address) {
	(void) address;
	struct CPU* cpu = &gba->cpu;
	uint32_t v = cpu->r[(opcode >> 3) & 7];
	unsigned a = (opcode >> 6) & 0x1F;
	bool carry;
	uint32_t d;
	switch (opcode >> 11) {
	case 0:
		carry = a ? (v >> (32 - a)) & 1 : !!(cpu->cpsr & PSR_C);
		d = a ? v << a : v;
		break;
	case 1:
		carry = a ? (v >> (a - 1)) & 1 : v >> 31;
		d = a ? v >> a : 0;
		break;
	default:
		carry = a ? (v >> (a - 1)) & 1 : v >> 31;
		d = a ? (uint32_t) ((int32_t) v >> a) : (uint32_t) ((int32_t) v >> 31);
		break;
	}
	cpu->r[opcode & 7] = d;
	setNz(cpu, d);
	cpu->cpsr = (cpu->cpsr & ~PSR_C) | (carry ? PSR_C : 0);
}

// Thumb ADD and SUB of a register or a 3-bit constant.
static void thumbAddSub3(struct GBA* gba, uint32_t opcode, uint32_t address) {
	(void) address;
	struct CPU* cpu = &gba->cpu;
	uint32_t n = cpu->r[(opcode >> 3) & 7];
	uint32_t m = (opcode & 0x0400) ? (opcode >> 6) & 7 : cpu->r[(opcode >> 6) & 7];
	cpu->r[opcode & 7] = (opcode & 0x0200) ? sub(cpu, n, m) : add(cpu, n, m);
}

// Thumb MOV, CMP, ADD and SUB with an 8-bit constant.
static void thumbImm8(struct GBA* gba, uint32_t opcode, uint32_t address) {
	(void) address;
	struct CPU* cpu = &gba->cpu;
	int rd = (opcode >> 8) & 7;
	uint32_t imm = opcode & 0xFF;
	switch ((opcode >> 11) & 3) {
	case 0:
		cpu->r[rd] = imm;
		setNz(cpu, imm);
		break;
	case 1:
		sub(cpu, cpu->r[rd], imm);
		break;
	case 2:
		cpu->r[rd] = add(cpu, cpu->r[rd], imm);
		break;
	default:
		cpu->r[rd] = sub(cpu, cpu->r[rd], imm);
		break;
	}
}

// Thumb's conditional branch (conditions 0-13).
static void thumbBranchCond(struct GBA* gba, uint32_t opcode, uint32_t address) {
	struct CPU* cpu = &gba->cpu;
	if (passes(cpu->cpsr, (opcode >> 8) & 0xF)) {
		cpu->r[15] = address + 4 + (int32_t) (int8_t) opcode * 2;
		stepReload(gba);
	}
}

// A Thumb load into a low register or store from one, as step.c's mem() makes it: the next fetch is
// nonsequential, and a load takes an internal cycle after its read. A signed halfword load from an
// odd address reads the byte there, sign-extended.
static inline void thumbMem(struct GBA* gba, int rd, uint32_t address, int size, bool load, bool signExtend) {
	struct CPU* cpu = &gba->cpu;
	cpu->fetchSeq = false;
	if (load) {
		uint32_t value = busRead(gba, address, size, false);
		if (signExtend) {
			value = (size == 1 || (address & 1)) ? (uint32_t) (int8_t) value : (uint32_t) (int16_t) value;
		}
		busIdle(gba);
		cpu->r[rd] = value;
	} else {
		busWrite(gba, address, cpu->r[rd], size, false);
	}
}

// LDR from the literal pool: the word-aligned PC plus a constant.
static void thumbLdrPc(struct GBA* gba, uint32_t opcode, uint32_t address) {
	(void) address;
	thumbMem(gba, (opcode >> 8) & 7, (gba->cpu.r[15] & ~3u) + ((opcode & 0xFF) << 2), 4, true, false);
}

// Loads and stores with a register offset, including the signed loads.
static void thumbMemReg(struct GBA* gba, uint32_t opcode, uint32_t address) {
	(void) address;
	static const struct {
		bool load;
		uint8_t size;
		bool signExtend;
	} forms[8] = {
		{ false, 4, false }, { false, 2, false }, { false, 1, false }, { true, 1, true },
		{ true, 4, false }, { true, 2, false }, { true, 1, false }, { true, 2, true },
	};
	int form = (opcode >> 9) & 7;
	struct CPU* cpu = &gba->cpu;
	thumbMem(gba, opcode & 7, cpu->r[(opcode >> 3) & 7] + cpu->r[(opcode >> 6) & 7], forms[form].size, forms[form].load, forms[form].signExtend);
}

// Loads and stores of words, bytes and halfwords with a 5-bit offset.
static void thumbMemImm(struct GBA* gba, uint32_t opcode, uint32_t address) {
	(void) address;
	static const uint8_t sizes[6] = { 4, 4, 1, 1, 2, 2 };
	int form = (opcode >> 11) - 0x0C;
	int size = sizes[form];
	thumbMem(gba, opcode & 7, gba->cpu.r[(opcode >> 3) & 7] + ((opcode >> 6) & 0x1F) * size, size, form & 1, false);
}

// Loads and stores relative to the stack pointer.
static void thumbMemSp(struct GBA* gba, uint32_t opcode, uint32_t address) {
	(void) address;
	thumbMem(gba, (opcode >> 8) & 7, gba->cpu.r[13] + ((opcode & 0xFF) << 2), 4, opcode & 0x0800, false);
}

// ADD of a constant to the word-aligned PC or to the stack pointer, into a low register; and the stack
// pointer moved by a constant. None of them sets flags.
static void thumbAddPc(struct GBA* gba, uint32_t opcode, uint32_t address) {
	gba->cpu.r[(opcode >> 8) & 7] = ((address + 4) & ~3u) + ((opcode & 0xFF) << 2);
}

static void thumbAddSp(struct GBA* gba, uint32_t opcode, uint32_t address) {
	(void) address;
	gba->cpu.r[(opcode >> 8) & 7] = gba->cpu.r[13] + ((opcode & 0xFF) << 2);
}

static void thumbMoveSp(struct GBA* gba, uint32_t opcode, uint32_t address) {
	(void) address;
	uint32_t offset = (opcode & 0x7F) << 2;
	gba->cpu.r[13] = (opcode & 0x80) ? gba->cpu.r[13] - offset : gba->cpu.r[13] + offset;
}

// The register-to-register operations but MUL. Shifts by a register take an internal cycle, after
// which the fetch is nonsequential.
static void thumbAlu(struct GBA* gba, uint32_t opcode, uint32_t address) {
	(void) address;
	struct CPU* cpu = &gba->cpu;
	int rd = opcode & 7;
	int rs = (opcode >> 3) & 7;
	int op = (opcode >> 6) & 0xF;
	bool c = cpu->cpsr & PSR_C;
	if (op == 2 || op == 3 || op == 4 || op == 7) {
		busIdle(gba);
		cpu->fetchSeq = false;
		uint32_t v = cpu->r[rd];
		unsigned s = cpu->r[rs] & 0xFF;
		bool carry = c;
		uint32_t d = v;
		if (s) {
			switch (op) {
			case 2:
				carry = s < 32 ? (v >> (32 - s)) & 1 : s == 32 ? v & 1 : 0;
				d = s < 32 ? v << s : 0;
				break;
			case 3:
				carry = s < 32 ? (v >> (s - 1)) & 1 : s == 32 ? v >> 31 : 0;
				d = s < 32 ? v >> s : 0;
				break;
			case 4:
				carry = s < 32 ? (v >> (s - 1)) & 1 : v >> 31;
				d = s < 32 ? (uint32_t) ((int32_t) v >> s) : (uint32_t) ((int32_t) v >> 31);
				break;
			default:
				if (s & 31) {
					carry = (v >> ((s & 31) - 1)) & 1;
					d = (v >> (s & 31)) | (v << (32 - (s & 31)));
				} else {
					carry = v >> 31;
				}
				break;
			}
		}
		cpu->r[rd] = d;
		setNz(cpu, d);
		cpu->cpsr = (cpu->cpsr & ~PSR_C) | (carry ? PSR_C : 0);
		return;
	}
	uint32_t n = cpu->r[rd];
	uint32_t m = cpu->r[rs];
	uint32_t d;
	switch (op) {
	case 0x0:
		d = cpu->r[rd] = n & m;
		break;
	case 0x1:
		d = cpu->r[rd] = n ^ m;
		break;
	case 0x5:
		d = cpu->r[rd] = n + m + c;
		setNzcv(cpu, d, (uint64_t) n + m + c > 0xFFFFFFFFu, (~(n ^ m) & (n ^ d)) >> 31);
		return;
	case 0x6:
		d = cpu->r[rd] = n - m - !c;
		setNzcv(cpu, d, (uint64_t) n >= (uint64_t) m + !c, ((n ^ m) & (n ^ d)) >> 31);
		return;
	case 0x8:
		d = n & m;
		break;
	case 0x9:
		// NEG: RSB from 0.
		cpu->r[rd] = sub(cpu, 0, m);
		return;
	case 0xA:
		sub(cpu, n, m);
		return;
	case 0xB:
		add(cpu, n, m);
		return;
	case 0xC:
		d = cpu->r[rd] = n | m;
		break;
	case 0xE:
		d = cpu->r[rd] = n & ~m;
		break;
	default:
		d = cpu->r[rd] = ~m;
		break;
	}
	setNz(cpu, d);
}

// ADD, CMP and MOV reaching r8-r15: ADD and MOV set no flags, and a PC they write refills the pipeline.
static void thumbHiReg(struct GBA* gba, uint32_t opcode, uint32_t address) {
	struct CPU* cpu = &gba->cpu;
	(void) address;
	int hd = (opcode & 7) | ((opcode >> 4) & 8);
	uint32_t m = cpu->r[(opcode >> 3) & 0xF];
	switch ((opcode >> 8) & 3) {
	case 0:
		cpu->r[hd] += m;
		break;
	case 1:
		sub(cpu, cpu->r[hd], m);
		return;
	default:
		cpu->r[hd] = m;
		break;
	}
	if (hd == 15) {
		stepReload(gba);
	}
}

// The unconditional branch, and BL's two halves.
static void thumbBranch(struct GBA* gba, uint32_t opcode, uint32_t address) {
	gba->cpu.r[15] = address + 4 + (((int32_t) ((uint32_t) opcode << 21)) >> 20);
	stepReload(gba);
}

static void thumbBl1(struct GBA* gba, uint32_t opcode, uint32_t address) {
	gba->cpu.r[14] = address + 4 + (((int32_t) ((uint32_t) opcode << 21)) >> 9);
}

static void thumbBl2(struct GBA* gba, uint32_t opcode, uint32_t address) {
	struct CPU* cpu = &gba->cpu;
	uint32_t target = cpu->r[14] + ((opcode & 0x7FF) << 1);
	cpu->r[14] = (address + 2) | 1;
	cpu->r[15] = target;
	stepReload(gba);
}

// The operation of ARM data processing, on its operands; carry is what the shifter left for the
// logical operations' flag.
static inline __attribute__((always_inline)) void dataProcessing(struct CPU* cpu, uint32_t opcode, int rd, uint32_t n, uint32_t m, bool c,
                                                                  bool carry) {
	bool const setFlags = opcode & 0x00100000;
	uint32_t d;
	switch ((opcode >> 21) & 0xF) {
	case ALU_AND:
		d = cpu->r[rd] = n & m;
		break;
	case ALU_EOR:
		d = cpu->r[rd] = n ^ m;
		break;
	case ALU_SUB:
		if (setFlags) {
			cpu->r[rd] = sub(cpu, n, m);
		} else {
			cpu->r[rd] = n - m;
		}
		return;
	case ALU_RSB:
		if (setFlags) {
			cpu->r[rd] = sub(cpu, m, n);
		} else {
			cpu->r[rd] = m - n;
		}
		return;
	case ALU_ADD:
		if (setFlags) {
			cpu->r[rd] = add(cpu, n, m);
		} else {
			cpu->r[rd] = n + m;
		}
		return;
	case ALU_ADC:
		d = cpu->r[rd] = n + m + c;
		if (setFlags) {
			setNzcv(cpu, d, (uint64_t) n + m + c > 0xFFFFFFFFu, (~(n ^ m) & (n ^ d)) >> 31);
		}
		return;
	case ALU_SBC:
		d = cpu->r[rd] = n - m - !c;
		if (setFlags) {
			setNzcv(cpu, d, (uint64_t) n >= (uint64_t) m + !c, ((n ^ m) & (n ^ d)) >> 31);
		}
		return;
	case ALU_RSC:
		d = cpu->r[rd] = m - n - !c;
		if (setFlags) {
			setNzcv(cpu, d, (uint64_t) m >= (uint64_t) n + !c, ((m ^ n) & (m ^ d)) >> 31);
		}
		return;
	case ALU_TST:
		d = n & m;
		break;
	case ALU_TEQ:
		d = n ^ m;
		break;
	case ALU_CMP:
		sub(cpu, n, m);
		return;
	case ALU_CMN:
		add(cpu, n, m);
		return;
	case ALU_ORR:
		d = cpu->r[rd] = n | m;
		break;
	case ALU_MOV:
		d = cpu->r[rd] = m;
		break;
	case ALU_BIC:
		d = cpu->r[rd] = n & ~m;
		break;
	default:
		d = cpu->r[rd] = ~m;
		break;
	}
	if (setFlags) {
		setNz(cpu, d);
		cpu->cpsr = (cpu->cpsr & ~PSR_C) | (carry ? PSR_C : 0);
	}
}

// Data processing with the PC as the destination, as step.c's alu() runs it: with S, the SPSR comes
// back into the CPSR (an exception return) in a mode that has one. TST, TEQ, CMP and CMN (always
// with S here) write no PC: the next instruction runs in whatever state the SPSR gave, refetched
// without a refill.
static __attribute__((noinline, cold)) void armDataProcessingPc(struct GBA* gba, uint32_t fixed, uint32_t address, uint32_t n, uint32_t m, bool c,
                                                                 bool carry) {
	struct CPU* cpu = &gba->cpu;
	unsigned const op = (fixed >> 21) & 0xF;
	bool const spsrBack = (fixed & 0x00100000) && cpuModeHasSpsr(cpu->cpsr & 0x1F);
	dataProcessing(cpu, fixed, 15, n, m, c, carry);
	if (spsrBack) {
		cpuWriteCpsr(gba, cpu->spsr);
	}
	if (op < ALU_TST || op > ALU_CMN) {
		stepReload(gba);
		return;
	}
	if (cpu->cpsr & PSR_T) {
		cpu->r[15] = address + 6;
		cpu->prefetch[0] = codeFetch16(gba, cpu->r[15] - 2);
		cpu->prefetch[1] = codeFetch16(gba, cpu->r[15]);
	} else {
		cpu->r[15] = address + 8;
		cpu->prefetch[0] = codeFetch32(gba, cpu->r[15] - 4);
		cpu->prefetch[1] = codeFetch32(gba, cpu->r[15]);
	}
}

// ARM data processing with a constant or a register shifted by a constant.
static inline __attribute__((always_inline)) void armDataProcessingAs(struct GBA* gba, uint32_t opcode, uint32_t address, uint32_t fixed) {
	struct CPU* cpu = &gba->cpu;
	int rd = (opcode >> 12) & 0xF;
	bool const c = cpu->cpsr & PSR_C;
	bool carry;
	uint32_t m;
	if (fixed & 0x02000000) {
		unsigned rotate = (opcode >> 7) & 0x1E;
		m = rotate ? ((opcode & 0xFF) >> rotate) | ((opcode & 0xFF) << (32 - rotate)) : opcode & 0xFF;
		carry = rotate ? m >> 31 : c;
	} else {
		uint32_t v = cpu->r[opcode & 0xF];
		unsigned a = (opcode >> 7) & 0x1F;
		switch ((opcode >> 5) & 3) {
		case SHIFT_LSL:
			carry = a ? (v >> (32 - a)) & 1 : c;
			m = v << a;
			break;
		case SHIFT_LSR:
			carry = a ? (v >> (a - 1)) & 1 : v >> 31;
			m = a ? v >> a : 0;
			break;
		case SHIFT_ASR:
			carry = a ? (v >> (a - 1)) & 1 : v >> 31;
			m = (uint32_t) ((int32_t) v >> (a ? a : 31));
			break;
		default:
			carry = a ? (v >> (a - 1)) & 1 : v & 1;
			m = a ? (v >> a) | (v << (32 - a)) : ((uint32_t) c << 31) | (v >> 1);
			break;
		}
	}
	if (rd == 15) {
		armDataProcessingPc(gba, fixed, address, cpu->r[(opcode >> 16) & 0xF], m, c, carry);
		return;
	}
	dataProcessing(cpu, fixed, rd, cpu->r[(opcode >> 16) & 0xF], m, c, carry);
}

// ARM LDR, STR, LDRB and STRB, as step.c's mem() makes them.
static inline __attribute__((always_inline)) void armMemAs(struct GBA* gba, uint32_t opcode, uint32_t address, uint32_t fixed) {
	struct CPU* cpu = &gba->cpu;
	int rd = (opcode >> 12) & 0xF;
	int rn = (opcode >> 16) & 0xF;
	bool const pre = fixed & 0x01000000;
	bool const writeback = !pre || (fixed & 0x00200000);
	bool const load = fixed & 0x00100000;
	uint32_t offset;
	if (fixed & 0x02000000) {
		uint32_t v = cpu->r[opcode & 0xF];
		unsigned a = (opcode >> 7) & 0x1F;
		switch ((opcode >> 5) & 3) {
		case SHIFT_LSL:
			offset = v << a;
			break;
		case SHIFT_LSR:
			offset = a ? v >> a : 0;
			break;
		case SHIFT_ASR:
			offset = (uint32_t) ((int32_t) v >> (a ? a : 31));
			break;
		default:
			offset = a ? (v >> a) | (v << (32 - a)) : ((uint32_t) !!(cpu->cpsr & PSR_C) << 31) | (v >> 1);
			break;
		}
	} else {
		offset = opcode & 0xFFF;
	}
	uint32_t const base = cpu->r[rn];
	uint32_t const moved = (fixed & 0x00800000) ? base + offset : base - offset;
	uint32_t const at = pre ? moved : base;
	int const size = (fixed & 0x00400000) ? 1 : 4;
	cpu->fetchSeq = false;
	if (load) {
		uint32_t value = busRead(gba, at, size, false);
		if (writeback) {
			cpu->r[rn] = moved;
		}
		busIdle(gba);
		cpu->r[rd] = value;
		if (rd == 15) {
			stepReload(gba);
		}
		return;
	}
	// A stored PC is 12 ahead of the instruction.
	busWrite(gba, at, cpu->r[rd] + (rd == 15 ? 4 : 0), size, false);
	if (writeback) {
		cpu->r[rn] = moved;
	}
}


// One copy of each for every combination of the opcode bits 25-20 the table already picks by (the
// operand form, the operation or transfer kind, S, P, U, B, W, L), so none of them is tested as it
// runs.
#define LEAN_ALL_64(X) X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15) X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23) X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31) X(32) X(33) X(34) X(35) X(36) X(37) X(38) X(39) X(40) X(41) X(42) X(43) X(44) X(45) X(46) X(47) X(48) X(49) X(50) X(51) X(52) X(53) X(54) X(55) X(56) X(57) X(58) X(59) X(60) X(61) X(62) X(63)
#define LEAN_DP(n) \
	static void armDataProcessing##n(struct GBA* gba, uint32_t opcode, uint32_t address) { \
		armDataProcessingAs(gba, opcode, address, (uint32_t) (n) << 20); \
	}
#define LEAN_MEM(n) \
	static void armMem##n(struct GBA* gba, uint32_t opcode, uint32_t address) { \
		armMemAs(gba, opcode, address, (uint32_t) (n) << 20); \
	}
#define LEAN_ENTRY_DP(n) armDataProcessing##n,
#define LEAN_ENTRY_MEM(n) armMem##n,
LEAN_ALL_64(LEAN_DP)
LEAN_ALL_64(LEAN_MEM)
static const LeanHandler armDataProcessingBy[64] = { LEAN_ALL_64(LEAN_ENTRY_DP) };
static const LeanHandler armMemBy[64] = { LEAN_ALL_64(LEAN_ENTRY_MEM) };

// The multiplier stops early once the rest of the operand is all zeros or all ones.
static inline int multiplyStages(uint32_t value) {
	value ^= (uint32_t) ((int32_t) value >> 31);
	return (31 - __builtin_clz(value | 1)) / 8 + 1;
}

// MUL and MLA, as step.c's mul() runs them: an internal cycle for each byte of the multiplier the
// early-out doesn't skip, and one more to accumulate.
static inline void multiply(struct GBA* gba, int rd, uint32_t multiplier, uint32_t multiplicand, uint32_t accum, bool accumulate,
                            bool setFlags) {
	struct CPU* cpu = &gba->cpu;
	int stages = multiplyStages(multiplier);
	uint32_t d = multiplicand * multiplier + accum;
	for (int i = 0; i < stages + accumulate; ++i) {
		busIdle(gba);
	}
	cpu->r[rd] = d;
	if (setFlags) {
		setNz(cpu, d);
		bool carry = stages == 4 ? cpuMultiplyCarryFull(multiplier) : cpuMultiplyCarryLow(multiplicand, multiplier, accum);
		cpu->cpsr = (cpu->cpsr & ~PSR_C) | (carry ? PSR_C : 0);
	}
}

static void armMultiply(struct GBA* gba, uint32_t opcode, uint32_t address) {
	struct CPU* cpu = &gba->cpu;
	int rd = (opcode >> 16) & 0xF;
	int rn = (opcode >> 12) & 0xF;
	bool accumulate = opcode & 0x00200000;
	(void) address;
	cpu->fetchSeq = false;
	// As in step.c, a multiply into the PC, or accumulating it, does nothing.
	if (rd == 15 || (accumulate && rn == 15)) {
		return;
	}
	multiply(gba, rd, cpu->r[(opcode >> 8) & 0xF], cpu->r[opcode & 0xF], accumulate ? cpu->r[rn] : 0, accumulate, opcode & 0x00100000);
}

// UMULL, UMLAL, SMULL and SMLAL, as step.c's mulLong() runs them.
static void armMultiplyLong(struct GBA* gba, uint32_t opcode, uint32_t address) {
	(void) address;
	struct CPU* cpu = &gba->cpu;
	int const rd = (opcode >> 12) & 0xF;
	int const rdHi = (opcode >> 16) & 0xF;
	bool const signedMul = opcode & 0x00400000;
	bool const accumulate = opcode & 0x00200000;
	cpu->fetchSeq = false;
	if (rd == 15 || rdHi == 15) {
		return;
	}
	uint32_t const multiplicand = cpu->r[opcode & 0xF];
	uint32_t const multiplier = cpu->r[(opcode >> 8) & 0xF];
	uint32_t const accumLo = accumulate ? cpu->r[rd] : 0;
	uint32_t const accumHi = accumulate ? cpu->r[rdHi] : 0;
	int const stages = signedMul ? multiplyStages(multiplier) : (31 - __builtin_clz(multiplier | 1)) / 8 + 1;
	uint64_t d = signedMul ? (uint64_t) ((int64_t) (int32_t) multiplicand * (int32_t) multiplier) : (uint64_t) multiplicand * multiplier;
	d += ((uint64_t) accumHi << 32) | accumLo;
	for (int i = 0; i < stages + 1 + accumulate; ++i) {
		busIdle(gba);
	}
	cpu->r[rd] = d;
	cpu->r[rdHi] = d >> 32;
	if (opcode & 0x00100000) {
		bool carry = stages == 4 ? cpuMultiplyCarryHigh(multiplicand, multiplier, accumHi, signedMul) : cpuMultiplyCarryLow(multiplicand, multiplier, accumLo);
		cpu->cpsr = (cpu->cpsr & ~(PSR_N | PSR_Z | PSR_C)) | ((d >> 32) & PSR_N) | (d ? 0 : PSR_Z) | (carry ? PSR_C : 0);
	}
}

// Undefined instructions (the coprocessor ones too: a GBA has none) and SWI.
static void armUndefined(struct GBA* gba, uint32_t opcode, uint32_t address) {
	(void) opcode;
	stepException(gba, MODE_UNDEFINED, 0x04, address + 4);
}

static void armSwi(struct GBA* gba, uint32_t opcode, uint32_t address) {
	(void) opcode;
	stepException(gba, MODE_SUPERVISOR, 0x08, address + 4);
}

static void thumbUndefined(struct GBA* gba, uint32_t opcode, uint32_t address) {
	(void) opcode;
	stepException(gba, MODE_UNDEFINED, 0x04, address + 2);
}

static void thumbSwi(struct GBA* gba, uint32_t opcode, uint32_t address) {
	(void) opcode;
	stepException(gba, MODE_SUPERVISOR, 0x08, address + 2);
}

// SWP and SWPB, as step.c's swap() runs them: a read and a write of the same address, then an
// internal cycle. With any of bits 11-8 set, the encoding is undefined.
static void armSwap(struct GBA* gba, uint32_t opcode, uint32_t address) {
	if (opcode & 0xF00) {
		armUndefined(gba, opcode, address);
		return;
	}
	struct CPU* cpu = &gba->cpu;
	uint32_t const at = cpu->r[(opcode >> 16) & 0xF];
	int const size = (opcode & 0x00400000) ? 1 : 4;
	cpu->fetchSeq = false;
	uint32_t value = busRead(gba, at, size, false);
	busWrite(gba, at, cpu->r[opcode & 0xF], size, false);
	busIdle(gba);
	cpu->r[(opcode >> 12) & 0xF] = value;
}

// CDP, MRC and MCR are undefined, except CDP p0 with nothing in bits 11-8 and 4: a Wii U Virtual
// Console call, which step.c runs.
static void armCoprocessor(struct GBA* gba, uint32_t opcode, uint32_t address) {
	if (opcode & 0xF10) {
		armUndefined(gba, opcode, address);
	} else {
		armGeneric(gba, opcode, address);
	}
}

// Thumb's MUL multiplies by the destination's old value.
static void thumbMultiply(struct GBA* gba, uint32_t opcode, uint32_t address) {
	(void) address;
	struct CPU* cpu = &gba->cpu;
	int rd = opcode & 7;
	cpu->fetchSeq = false;
	multiply(gba, rd, cpu->r[rd], cpu->r[(opcode >> 3) & 7], 0, false, true);
}

// ARM's halfword and signed transfers, as step.c's mem() makes them.
static void armMemHalf(struct GBA* gba, uint32_t opcode, uint32_t address) {
	(void) address;
	struct CPU* cpu = &gba->cpu;
	int rd = (opcode >> 12) & 0xF;
	int rn = (opcode >> 16) & 0xF;
	bool const pre = opcode & 0x01000000;
	bool const writeback = !pre || (opcode & 0x00200000);
	uint32_t offset = (opcode & 0x00400000) ? ((opcode >> 4) & 0xF0) | (opcode & 0xF) : cpu->r[opcode & 0xF];
	uint32_t const base = cpu->r[rn];
	uint32_t const moved = (opcode & 0x00800000) ? base + offset : base - offset;
	uint32_t const at = pre ? moved : base;
	int const sh = (opcode >> 5) & 3;
	int const size = sh == 2 ? 1 : 2;
	cpu->fetchSeq = false;
	if (opcode & 0x00100000) {
		uint32_t value = busRead(gba, at, size, false);
		if (sh != 1) {
			value = (size == 1 || (at & 1)) ? (uint32_t) (int8_t) value : (uint32_t) (int16_t) value;
		}
		if (writeback) {
			cpu->r[rn] = moved;
		}
		busIdle(gba);
		cpu->r[rd] = value;
		if (rd == 15) {
			stepReload(gba);
		}
		return;
	}
	busWrite(gba, at, cpu->r[rd] + (rd == 15 ? 4 : 0), size, false);
	if (writeback) {
		cpu->r[rn] = moved;
	}
}

// ARM data processing with a register shifted by another register's low byte: the shift takes an
// internal cycle, after which the fetch is nonsequential, and a PC operand is read 12 ahead instead
// of 8.
static void armDataProcessingByReg(struct GBA* gba, uint32_t opcode, uint32_t address) {
	struct CPU* cpu = &gba->cpu;
	int rd = (opcode >> 12) & 0xF;
	busIdle(gba);
	cpu->fetchSeq = false;
	bool const c = cpu->cpsr & PSR_C;
	int rm = opcode & 0xF;
	uint32_t v = cpu->r[rm] + (rm == 15 ? 4 : 0);
	unsigned s = cpu->r[(opcode >> 8) & 0xF] & 0xFF;
	bool carry = c;
	uint32_t m = v;
	if (s) {
		switch ((opcode >> 5) & 3) {
		case SHIFT_LSL:
			carry = s < 32 ? (v >> (32 - s)) & 1 : s == 32 ? v & 1 : 0;
			m = s < 32 ? v << s : 0;
			break;
		case SHIFT_LSR:
			carry = s < 32 ? (v >> (s - 1)) & 1 : s == 32 ? v >> 31 : 0;
			m = s < 32 ? v >> s : 0;
			break;
		case SHIFT_ASR:
			carry = s < 32 ? (v >> (s - 1)) & 1 : v >> 31;
			m = s < 32 ? (uint32_t) ((int32_t) v >> s) : (uint32_t) ((int32_t) v >> 31);
			break;
		default:
			if (s & 31) {
				carry = (v >> ((s & 31) - 1)) & 1;
				m = (v >> (s & 31)) | (v << (32 - (s & 31)));
			} else {
				carry = v >> 31;
			}
			break;
		}
	}
	int rn = (opcode >> 16) & 0xF;
	uint32_t const n = cpu->r[rn] + (rn == 15 ? 4 : 0);
	if (rd == 15) {
		armDataProcessingPc(gba, opcode, address, n, m, c, carry);
		return;
	}
	dataProcessing(cpu, opcode, rd, n, m, c, carry);
}

// LDM and STM without the S bit, and Thumb's PUSH, POP, LDMIA and STMIA, as step.c's block() makes
// them: an empty list moves the base as sixteen registers would and transfers r15; the base is
// written back after the first access; a load takes an internal cycle after its reads.
static inline void blockTransfer(struct GBA* gba, int rn, uint16_t list, bool load, bool up, bool pre, bool writeback, bool thumb) {
	struct CPU* cpu = &gba->cpu;
	int count = list ? __builtin_popcount(list) : 16;
	if (!list) {
		list = 0x8000;
	}
	uint32_t base = cpu->r[rn];
	uint32_t moved = up ? base + 4 * count : base - 4 * count;
	uint32_t address = up ? base : moved;
	if (!up) {
		pre = !pre;
	}
	cpu->fetchSeq = false;
	bool seq = false;
	for (int i = 0; i < 16; ++i) {
		if (!(list & (1 << i))) {
			continue;
		}
		if (pre) {
			address += 4;
		}
		uint32_t at = (address >> 24) < REGION_SRAM ? address & ~3u : address;
		if (load) {
			uint32_t value = busRead(gba, at, 4, seq);
			if (writeback && !seq) {
				cpu->r[rn] = moved;
			}
			cpu->r[i] = value;
		} else {
			uint32_t value = cpu->r[i];
			if (i == 15) {
				value += thumb ? 2 : 4;
			}
			busWrite(gba, at, value, 4, seq);
			if (writeback && !seq) {
				cpu->r[rn] = moved;
			}
		}
		if (!pre) {
			address += 4;
		}
		seq = true;
	}
	if (load) {
		busIdle(gba);
		if (list & 0x8000) {
			stepReload(gba);
		}
	}
}

// LDM and STM with the S bit, as step.c's block() makes them: the user mode registers are
// transferred, unless an LDM loads the PC, which brings the SPSR back into the CPSR.
static __attribute__((noinline, cold)) void armBlockUser(struct GBA* gba, uint32_t opcode) {
	struct CPU* cpu = &gba->cpu;
	int const rn = (opcode >> 16) & 0xF;
	bool const load = opcode & 0x00100000;
	bool const up = opcode & 0x00800000;
	bool const writeback = opcode & 0x00200000;
	uint16_t list = opcode & 0xFFFF;
	int count = list ? __builtin_popcount(list) : 16;
	if (!list) {
		list = 0x8000;
	}
	uint32_t base = cpu->r[rn];
	uint32_t moved = up ? base + 4 * count : base - 4 * count;
	uint32_t address = up ? base : moved;
	bool pre = up ? (opcode & 0x01000000) : !(opcode & 0x01000000);
	unsigned mode = cpu->mode;
	bool userBank = !(load && (list & 0x8000)) && cpuModeHasSpsr(mode);
	if (userBank) {
		cpuSetPrivilegeMode(gba, MODE_SYSTEM);
	}
	cpu->fetchSeq = false;
	bool seq = false;
	for (int i = 0; i < 16; ++i) {
		if (!(list & (1 << i))) {
			continue;
		}
		if (pre) {
			address += 4;
		}
		uint32_t at = (address >> 24) < REGION_SRAM ? address & ~3u : address;
		if (load) {
			uint32_t value = busRead(gba, at, 4, seq);
			if (writeback && !seq) {
				cpu->r[rn] = moved;
			}
			cpu->r[i] = value;
		} else {
			busWrite(gba, at, cpu->r[i] + (i == 15 ? 4 : 0), 4, seq);
			if (writeback && !seq) {
				cpu->r[rn] = moved;
			}
		}
		if (!pre) {
			address += 4;
		}
		seq = true;
	}
	if (load) {
		busIdle(gba);
	}
	if (userBank) {
		cpuSetPrivilegeMode(gba, mode);
	}
	if (load && (list & 0x8000)) {
		if (cpuModeHasSpsr(cpu->cpsr & 0x1F)) {
			cpuWriteCpsr(gba, cpu->spsr);
		}
		stepReload(gba);
	}
}

static void armBlock(struct GBA* gba, uint32_t opcode, uint32_t address) {
	(void) address;
	if (opcode & 0x00400000) {
		armBlockUser(gba, opcode);
		return;
	}
	blockTransfer(gba, (opcode >> 16) & 0xF, opcode & 0xFFFF, opcode & 0x00100000, opcode & 0x00800000, opcode & 0x01000000,
	              opcode & 0x00200000, false);
}

// BX: bit 0 of the target picks ARM or Thumb.
static inline void branchExchange(struct GBA* gba, uint32_t target) {
	struct CPU* cpu = &gba->cpu;
	if (target & 1) {
		cpu->cpsr |= PSR_T;
	} else {
		cpu->cpsr &= ~PSR_T;
	}
	cpu->r[15] = target & ~1u;
	stepReload(gba);
}

// MRS, MSR and BX, which share the encoding of TST, TEQ, CMP and CMN without S, as step.c's decoder
// tells them apart; anything else there goes through step.c. MSR to the CPSR refills the pipeline
// as mGBA does, and after a switch to Thumb a NOP fills the gap.
static void armStatus(struct GBA* gba, uint32_t opcode, uint32_t address) {
	struct CPU* cpu = &gba->cpu;
	bool const imm = opcode & 0x02000000;
	if (!imm) {
		if ((opcode & 0x0FFFFFF0) == 0x012FFF10) {
			branchExchange(gba, cpu->r[opcode & 0xF]);
			return;
		}
		if (opcode & 0x10) {
			armUndefined(gba, opcode, address);
			return;
		}
	}
	if (opcode & 0x00200000) {
		uint32_t value;
		if (imm) {
			unsigned rotate = (opcode >> 7) & 0x1E;
			value = rotate ? ((opcode & 0xFF) >> rotate) | ((opcode & 0xFF) << (32 - rotate)) : opcode & 0xFF;
		} else {
			value = cpu->r[opcode & 0xF];
		}
		bool const spsr = opcode & 0x00400000;
		uint32_t const fieldMask = ((opcode & 0x00010000) ? 0xFF : 0) | ((opcode & 0x00080000) ? 0xFF000000 : 0);
		jitHelperMsr(gba, value, fieldMask | (spsr ? 0x100 : 0));
		if (spsr) {
			return;
		}
		if (cpu->cpsr & PSR_T) {
			cpu->prefetch[0] = 0x46C0;
			cpu->prefetch[1] &= 0xFFFF;
			cpu->r[15] += 2;
		} else {
			cpu->prefetch[0] = codeFetch32(gba, cpu->r[15] - 4);
			cpu->prefetch[1] = codeFetch32(gba, cpu->r[15]);
		}
		return;
	}
	// MRS has no constant form. As in step.c, an MRS into the PC writes it without a refill.
	if (imm) {
		armUndefined(gba, opcode, address);
		return;
	}
	cpu->r[(opcode >> 12) & 0xF] = (opcode & 0x00400000) ? cpu->spsr : cpu->cpsr;
}

// B and BL: a signed 24-bit word offset from the PC, which is 8 ahead.
static void armBranch(struct GBA* gba, uint32_t opcode, uint32_t address) {
	struct CPU* cpu = &gba->cpu;
	if (opcode & 0x01000000) {
		cpu->r[14] = address + 4;
	}
	cpu->r[15] = address + 8 + ((int32_t) (opcode << 8) >> 6);
	stepReload(gba);
}

// The handler for an ARM opcode's bits 27-20 and 7-4, i.
static LeanHandler armHandler(int i) {
	int const bits27to20 = i >> 4;
	bool const bit4 = i & 1;
	bool const bit7 = i & 8;
	// TST, TEQ, CMP and CMN without S: MRS, MSR and BX.
	bool const statusOp = (bits27to20 & 0x19) == 0x10;
	switch (bits27to20 >> 5) {
	case 0:
		if (bit4 && bit7) {
			// Multiplies, SWP, and the halfword and signed transfers (bits 6-5 not both clear).
			if (!(i & 6)) {
				// MUL, the long multiplies, then SWP (bits 21-20 clear) or undefined.
				switch (bits27to20 & 0x18) {
				case 0x00:
					return armMultiply;
				case 0x08:
					return armMultiplyLong;
				case 0x10:
					return (bits27to20 & 3) ? armUndefined : armSwap;
				default:
					return armUndefined;
				}
			}
			// Stores with sign extension are undefined.
			bool const load = bits27to20 & 1;
			return load || ((i >> 1) & 3) == 1 ? armMemHalf : armUndefined;
		}
		if (statusOp) {
			return armStatus;
		}
		return bit4 ? armDataProcessingByReg : armDataProcessingBy[bits27to20 & 0x3F];
	case 1:
		return statusOp ? armStatus : armDataProcessingBy[bits27to20 & 0x3F];
	case 2:
		return armMemBy[bits27to20 & 0x3F];
	case 3:
		return bit4 ? armUndefined : armMemBy[bits27to20 & 0x3F];
	case 4:
		return armBlock;
	case 5:
		return armBranch;
	case 6:
		return armUndefined;
	default:
		(void) bit7;
		return (bits27to20 & 0x10) ? armSwi : armCoprocessor;
	}
}

static void thumbPush(struct GBA* gba, uint32_t opcode, uint32_t address) {
	(void) address;
	blockTransfer(gba, 13, (opcode & 0xFF) | ((opcode & 0x100) ? 0x4000 : 0), false, false, true, true, true);
}

static void thumbPop(struct GBA* gba, uint32_t opcode, uint32_t address) {
	(void) address;
	blockTransfer(gba, 13, (opcode & 0xFF) | ((opcode & 0x100) ? 0x8000 : 0), true, true, false, true, true);
}

static void thumbBlock(struct GBA* gba, uint32_t opcode, uint32_t address) {
	(void) address;
	blockTransfer(gba, (opcode >> 8) & 7, opcode & 0xFF, opcode & 0x0800, true, false, true, true);
}

static void thumbBx(struct GBA* gba, uint32_t opcode, uint32_t address) {
	(void) address;
	branchExchange(gba, gba->cpu.r[(opcode >> 3) & 0xF]);
}

// The handler for a Thumb opcode's bits 15-6, i.
static LeanHandler thumbHandler(int i) {
	int const format = i >> 5;
	switch (format) {
	case 0x00:
	case 0x01:
	case 0x02:
		return thumbShiftImm;
	case 0x03:
		return thumbAddSub3;
	case 0x04:
	case 0x05:
	case 0x06:
	case 0x07:
		return thumbImm8;
	case 0x08:
		if (!(i & 0x10)) {
			return (i & 0xF) == 0xD ? thumbMultiply : thumbAlu;
		}
		// Bits 9-8 pick ADD, CMP, MOV or BX.
		return ((i >> 2) & 3) != 3 ? thumbHiReg : thumbBx;
	case 0x09:
		return thumbLdrPc;
	case 0x0A:
	case 0x0B:
		return thumbMemReg;
	case 0x0C:
	case 0x0D:
	case 0x0E:
	case 0x0F:
	case 0x10:
	case 0x11:
		return thumbMemImm;
	case 0x12:
	case 0x13:
		return thumbMemSp;
	case 0x14:
		return thumbAddPc;
	case 0x15:
		return thumbAddSp;
	case 0x16:
		return ((i >> 2) & 0xF) == 0 ? thumbMoveSp : ((i >> 2) & 0xE) == 4 ? thumbPush : thumbUndefined;
	case 0x17:
		return ((i >> 2) & 0xE) == 0xC ? thumbPop : thumbUndefined;
	case 0x18:
	case 0x19:
		return thumbBlock;
	case 0x1A:
	case 0x1B:
		return ((i >> 2) & 0xF) < 0xE ? thumbBranchCond : ((i >> 2) & 0xF) == 0xE ? thumbUndefined : thumbSwi;
	case 0x1C:
		return thumbBranch;
	case 0x1E:
		return thumbBl1;
	case 0x1F:
		return thumbBl2;
	default:
		// 0x1D: undefined, except 0xE800 with bits 10-6 clear, a Wii U Virtual Console call.
		return i == 0x3A0 ? thumbGeneric : thumbUndefined;
	}
}

static void fillTables(void) {
	for (int i = 0; i < 4096; ++i) {
		armTable[i] = armHandler(i);
	}
	for (int i = 0; i < 1024; ++i) {
		thumbTable[i] = thumbHandler(i);
	}
}

// One instruction, as step.c's step() runs it: tracing or a cheat cartridge's trigger go through
// step() itself.
static inline void leanOne(struct GBA* gba) {
	struct CPU* cpu = &gba->cpu;
	if (gba->traceFile || gba->cartTrapped) {
		stepInstruction(gba);
		return;
	}
	if (gba->irq.line && !cpu->irqMasked) {
		stepTakeIrq(gba);
	}
	uint32_t const opcode = cpu->prefetch[0];
	// The I flag as it is before the instruction decides whether an IRQ is taken after it.
	cpu->irqMasked = cpu->cpsr & PSR_I;
	cpu->prefetch[0] = cpu->prefetch[1];
	if (cpu->cpsr & PSR_T) {
		cpu->r[15] += 2;
		cpu->prefetch[1] = busFetchInline(gba, cpu->r[15], true);
		thumbTable[(opcode >> 6) & 0x3FF](gba, opcode, cpu->r[15] - 4);
	} else {
		cpu->r[15] += 4;
		cpu->prefetch[1] = busFetchInline(gba, cpu->r[15], false);
		if ((opcode >> 28) == COND_AL || passes(cpu->cpsr, opcode >> 28)) {
			armTable[((opcode >> 16) & 0xFF0) | ((opcode >> 4) & 0xF)](gba, opcode, cpu->r[15] - 8);
		}
	}
}

// An instruction compiled code leaves to the CPU, with its fetch counted and its condition passed:
// an ARM one at address, or a Thumb one at address - 1. As step.c's stepExecute() runs it.
void leanExecute(struct GBA* gba, uint32_t opcode, uint32_t address) {
	if (!armTable[0]) {
		fillTables();
	}
	if (address & 1) {
		thumbTable[(opcode >> 6) & 0x3FF](gba, opcode, address & ~1u);
	} else {
		armTable[((opcode >> 16) & 0xFF0) | ((opcode >> 4) & 0xF)](gba, opcode, address);
	}
}

void leanStep(struct GBA* gba) {
	if (!armTable[0]) {
		fillTables();
	}
	leanOne(gba);
}

// Instructions until the frame ends, something asks for control back or the CPU halts.
void leanRun(struct GBA* gba) {
	if (!armTable[0]) {
		fillTables();
	}
	struct CPU* cpu = &gba->cpu;
	uint32_t const frame = gba->video.frameCounter;
	while (gba->video.frameCounter == frame && !gba->returnRequested && !cpu->halted) {
		leanOne(gba);
	}
}

// Runs until the frame ends or something asks for control back: the table-driven CPU, and a halted
// CPU's waits for events.
void leanRunLoop(struct GBA* gba) {
	struct CPU* cpu = &gba->cpu;
	uint32_t frame = gba->video.frameCounter;
	while (gba->video.frameCounter == frame && !gba->returnRequested) {
		if (cpu->halted) {
			if (cpu->cycles < cpu->nextEvent) {
				cpu->cycles = cpu->nextEvent;
			}
			processEvents(gba);
			continue;
		}
		leanRun(gba);
	}
	gba->returnRequested = false;
}
