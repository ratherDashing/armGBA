/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gba.h"
#include "cpu/jit.h"

#include <stdlib.h>
#include <string.h>

// The cheat cartridges Future Console Design made for the Game Boy Advance: Blaze's Xploder Advance
// and Pelican's CodeBreaker. A game plugs in on top. Nothing about their hardware is published; this
// is what their programs do with it.
//
// - The cartridge's flash chip (the Xploder's 128 KiB, the CodeBreaker's 64 KiB) shows in one 4 MiB
//   page of the cartridge space, the window; everything else is the game on top. At power on the
//   window is the first page, and the program runs there.
// - Two 16-bit registers, written only, sit in the window: at 0x200000 and 0x200002 on the Xploder,
//   0x10000 and 0x10002 on the CodeBreaker. The second's bits 13-15 move the window to their page.
//   The first, and the second's low byte, give a halfword of the game: the trigger.
// - While the hook is on, the game's 32 bytes around the trigger read from the flash chip's last 512
//   bytes, where the program wrote them with its hook spliced in. The hook turns on and off at each
//   fetch of the trigger: the game's arrival there runs the hook, which calls the code engine (the
//   program's own code, run from the window) and branches back to the trigger, now showing the
//   game's own code again. The program writes the registers, then reads the trigger, so the game
//   starts with the hook off. The hooks the programs build work the same when it turns on and off
//   as the CPU runs the trigger, which is what armGBA does.
// - The flash chip takes the commands of SST's 16-bit chips at 0xAAAA and 0x5554 of the window: ID
//   (90), word program (A0) and erase (80, then 30 for 4 KiB or 10 for all of it).
// - The program starts the game with the BIOS's SoftReset, with the window moved off the game, at
//   the last page when no code runs.
// - From power on until the program first writes a register, the header's logo (0x04-0x9F) is the
//   game's, for the BIOS's check: the program's own holds its makers' names.

enum {
	PAGE_SIZE = 0x400000,
	HOOK_BLOCK = 0x20,
	HOOK_AREA = 0x200,
	LOGO_FROM = 0x04,
	LOGO_TO = 0xA0,
	STATE_MAGIC = 0x46434443,
};

#define NO_PATCH 0xFFFFFFFFu

static const char maker[] = " (C) Future Console Design";

struct FcdState {
	uint32_t magic;
	uint32_t trigger;
	// Where the hook's block is on the game, or NO_PATCH
	uint32_t patchedAt;
	uint16_t registers[2];
	uint8_t page;
	bool hookOn;
	bool boot;
	uint8_t flashStep;
	bool flashId;
	bool flashProgram;
	uint8_t hookWas[HOOK_BLOCK];
	uint16_t idWas[2];
};

static struct {
	struct GBA* gba;
	uint8_t* flash;
	uint32_t flashSize;
	uint8_t* game;
	uint32_t gameSize;
	// Where the registers are in the window, and how much of it the cartridge answers
	uint32_t registers;
	uint32_t claimed;
	// The chip as the cartridge's image has it (see fcdAttach)
	uint8_t* factory;
	// The flash chip as kept: without the ID or the game's logo showing over it
	uint8_t* kept;
	uint8_t logoWas[LOGO_TO - LOGO_FROM];
	bool flashChanged;
	struct FcdState s;
} fcd;

static uint16_t get16(const uint8_t* p) {
	return (uint16_t) (p[0] | p[1] << 8);
}

static void set16(uint8_t* p, uint16_t value) {
	p[0] = (uint8_t) value;
	p[1] = (uint8_t) (value >> 8);
}

bool fcdDetect(const void* data, size_t size) {
	return data && (size == 0x10000 || size == 0x20000) && !memcmp((const uint8_t*) data + LOGO_FROM, maker, sizeof(maker) - 1);
}

bool fcdActive(const struct GBA* gba) {
	return gba && fcd.gba == gba;
}

// All six cartridge regions: the cartridge doesn't see the wait states.
static bool ours(const struct GBA* gba, uint32_t address) {
	return fcdActive(gba) && (address >> 24) >= REGION_ROM0 && (address >> 24) <= REGION_ROM2_EX;
}

static uint32_t windowStart(void) {
	return fcd.s.page * (uint32_t) PAGE_SIZE;
}

static bool inWindow(uint32_t offset) {
	return offset - windowStart() < fcd.claimed;
}

static uint32_t pow2(uint32_t value) {
	uint32_t p = 1;
	while (p < value) {
		p <<= 1;
	}
	return p;
}

// The image the console reads directly: the flash chip with the window at the start, else the game up
// to the window.
static void map(void) {
	struct GBA* gba = fcd.gba;
	uint8_t* rom = fcd.flash;
	uint32_t size = fcd.flashSize;
	if (fcd.s.page) {
		rom = fcd.game ? fcd.game : fcd.flash;
		size = fcd.game ? (fcd.gameSize < windowStart() ? fcd.gameSize : windowStart()) : 0;
	}
	if (gba->rom == rom && gba->romSize == size) {
		return;
	}
	gba->rom = rom;
	gba->romSize = size;
	gba->romMask = pow2(size ? size : 1) - 1;
	jitRomChanged(gba);
}

// Whether the trigger hooks the game: it is outside the window, on the game the console reads.
static bool armed(void) {
	uint32_t const block = fcd.s.trigger & ~(uint32_t) (HOOK_BLOCK - 1);
	return fcd.game && fcd.s.page && !inWindow(fcd.s.trigger) && block + HOOK_BLOCK <= fcd.gba->romSize;
}

static void unpatch(void) {
	if (fcd.s.patchedAt != NO_PATCH) {
		memcpy(&fcd.game[fcd.s.patchedAt], fcd.s.hookWas, HOOK_BLOCK);
		// Compiled code never holds the block while the trigger is watched (see jit.c).
		if (!fcd.gba->cartTrapped) {
			jitInvalidate(fcd.gba, 0x08000000 + fcd.s.patchedAt, HOOK_BLOCK);
		}
		fcd.s.patchedAt = NO_PATCH;
		// What a state holds is the same however it got there
		memset(fcd.s.hookWas, 0, HOOK_BLOCK);
	}
}

// Puts the hook's block over the game while it is on, and has the CPU watch for it running the
// trigger while the trigger hooks the game. Compiled code is laid out around the block then, so it's
// thrown away when that changes.
static void patch(void) {
	struct GBA* gba = fcd.gba;
	bool const hooks = armed();
	// Not watched, no address: what a state holds is the same however it got there
	uint32_t const at = hooks ? fcd.s.trigger : 0;
	if (hooks != gba->cartTrapped || at != gba->cartTrapAt) {
		gba->cartTrapped = hooks;
		gba->cartTrapAt = at;
		jitRomChanged(gba);
	}
	uint32_t const block = fcd.s.trigger & ~(uint32_t) (HOOK_BLOCK - 1);
	uint32_t const want = hooks && fcd.s.hookOn ? block : NO_PATCH;
	if (want == fcd.s.patchedAt) {
		return;
	}
	unpatch();
	if (want == NO_PATCH) {
		return;
	}
	memcpy(fcd.s.hookWas, &fcd.game[block], HOOK_BLOCK);
	memcpy(&fcd.game[block], &fcd.flash[fcd.flashSize - HOOK_AREA + (block & (HOOK_AREA - 1))], HOOK_BLOCK);
	fcd.s.patchedAt = block;
}

// The CPU is about to run the trigger. On the cartridge, its fetch of the trigger turns the hook on
// or off, which comes to the same for the hooks the programs build: the game's arrival there runs
// the hook, and the hook's branch back runs the game's own code.
void fcdTrap(struct GBA* gba) {
	if (!fcdActive(gba)) {
		return;
	}
	fcd.s.hookOn = !fcd.s.hookOn;
	patch();
}

// The game's logo over the program's, for the BIOS, until the program first writes a register.
static void showLogo(bool on) {
	if (on == fcd.s.boot) {
		return;
	}
	if (on && !fcd.game) {
		return;
	}
	if (on) {
		memcpy(fcd.logoWas, &fcd.flash[LOGO_FROM], sizeof(fcd.logoWas));
		memcpy(&fcd.flash[LOGO_FROM], &fcd.game[LOGO_FROM], sizeof(fcd.logoWas));
	} else {
		memcpy(&fcd.flash[LOGO_FROM], fcd.logoWas, sizeof(fcd.logoWas));
	}
	fcd.s.boot = on;
	jitInvalidate(fcd.gba, 0x08000000 + LOGO_FROM, sizeof(fcd.logoWas));
}

// The ID shows over the chip's first two halfwords.
static void showId(bool on) {
	if (on == fcd.s.flashId) {
		return;
	}
	if (on) {
		fcd.s.idWas[0] = get16(&fcd.flash[0]);
		fcd.s.idWas[1] = get16(&fcd.flash[2]);
		set16(&fcd.flash[0], 0x00BF);
		set16(&fcd.flash[2], 0x2789);
	} else {
		set16(&fcd.flash[0], fcd.s.idWas[0]);
		set16(&fcd.flash[2], fcd.s.idWas[1]);
		fcd.s.idWas[0] = fcd.s.idWas[1] = 0;
	}
	fcd.s.flashId = on;
	jitInvalidate(fcd.gba, 0x08000000, 4);
}

static void erase(uint32_t at, uint32_t size) {
	showId(false);
	memset(&fcd.flash[at], 0xFF, size);
	jitRomChanged(fcd.gba);
	fcd.flashChanged = true;
}

static void writeFlash(uint32_t at, uint16_t value) {
	if (fcd.s.flashProgram) {
		fcd.s.flashProgram = false;
		at &= fcd.flashSize - 2;
		bool const id = fcd.s.flashId;
		showId(false);
		uint16_t const now = get16(&fcd.flash[at]) & value;
		if (now != get16(&fcd.flash[at])) {
			set16(&fcd.flash[at], now);
			jitInvalidate(fcd.gba, 0x08000000 + at, 2);
			fcd.flashChanged = true;
		}
		showId(id);
		return;
	}
	if ((value & 0xFF) == 0xF0) {
		fcd.s.flashStep = 0;
		showId(false);
		return;
	}
	uint32_t const command = at & 0xFFFFF;
	switch (fcd.s.flashStep) {
	case 0:
	case 3:
		fcd.s.flashStep = command == 0xAAAA && (value & 0xFF) == 0xAA ? fcd.s.flashStep + 1 : 0;
		break;
	case 1:
	case 4:
		fcd.s.flashStep = command == 0x5554 && (value & 0xFF) == 0x55 ? fcd.s.flashStep + 1 : 0;
		break;
	case 2:
		fcd.s.flashStep = 0;
		if (command != 0xAAAA) {
			break;
		}
		switch (value & 0xFF) {
		case 0x90:
			showId(true);
			break;
		case 0xA0:
			fcd.s.flashProgram = true;
			break;
		case 0x80:
			fcd.s.flashStep = 3;
			break;
		}
		break;
	case 5:
		fcd.s.flashStep = 0;
		if ((value & 0xFF) == 0x30) {
			erase(at & (fcd.flashSize - 1) & ~0xFFFu, 0x1000);
		} else if ((value & 0xFF) == 0x10 && command == 0xAAAA) {
			erase(0, fcd.flashSize);
		}
		break;
	}
}

// The window's page and the trigger, from the registers.
static void decodeRegisters(void) {
	fcd.s.page = fcd.s.registers[1] >> 13;
	fcd.s.trigger = ((uint32_t) (fcd.s.registers[1] & 0xFF) << 16 | fcd.s.registers[0]) << 1;
}

static void writeRegister(int which, uint16_t value) {
	showLogo(false);
	fcd.s.registers[which] = value;
	unpatch();
	decodeRegisters();
	// The program reads the trigger right after, which turns the hook off.
	fcd.s.hookOn = false;
	map();
	patch();
}

// Reads the console doesn't make from its image: the window when it isn't at the start, the flash
// chip's mirrors, and the game past the window.
bool fcdRead(struct GBA* gba, uint32_t address, int size, uint32_t* value) {
	if (!ours(gba, address)) {
		return false;
	}
	uint32_t const offset = address & (SIZE_ROM - 1);
	const uint8_t* p = NULL;
	if (inWindow(offset)) {
		p = &fcd.flash[(offset - windowStart()) & (fcd.flashSize - 1) & ~3u];
	} else if (fcd.game && (offset & ~3u) + 4 <= fcd.gameSize) {
		p = &fcd.game[offset & ~3u];
	} else {
		return false;
	}
	uint32_t const word = p[0] | p[1] << 8 | p[2] << 16 | (uint32_t) p[3] << 24;
	switch (size) {
	case 4:
		*value = word;
		break;
	case 2:
		*value = (word >> ((offset & 2) * 8)) & 0xFFFF;
		break;
	default:
		*value = (word >> ((offset & 3) * 8)) & 0xFF;
		break;
	}
	return true;
}

// Code the console runs from outside its image: the engine, in the window.
const uint8_t* fcdCode(struct GBA* gba, uint32_t address) {
	if (!ours(gba, address)) {
		return NULL;
	}
	uint32_t const offset = address & (SIZE_ROM - 1);
	if (inWindow(offset)) {
		return &fcd.flash[(offset - windowStart()) & (fcd.flashSize - 1)];
	}
	return fcd.game && offset < fcd.gameSize ? &fcd.game[offset] : NULL;
}

bool fcdWrite16(struct GBA* gba, uint32_t address, uint16_t value) {
	if (!ours(gba, address)) {
		return false;
	}
	uint32_t const offset = address & (SIZE_ROM - 2);
	if (!inWindow(offset)) {
		// The cartridge on top's, which a game ignores
		return false;
	}
	uint32_t const at = offset - windowStart();
	if (at == fcd.registers || at == fcd.registers + 2) {
		writeRegister(at == fcd.registers ? 0 : 1, value);
	} else if (at < fcd.registers) {
		writeFlash(at, value);
	}
	return true;
}

static void resetState(void) {
	memset(&fcd.s, 0, sizeof(fcd.s));
	fcd.s.magic = STATE_MAGIC;
	fcd.s.patchedAt = NO_PATCH;
}

// Power on: the program shows, with the game's logo for the BIOS.
void fcdReset(struct GBA* gba) {
	if (!fcdActive(gba)) {
		return;
	}
	unpatch();
	showId(false);
	showLogo(false);
	resetState();
	gba->cartTrapped = false;
	gba->cartTrapAt = 0;
	if (fcd.game) {
		gba->rom = fcd.game;
		gba->romSize = fcd.gameSize;
		gpioSetDevices(gba, overridesApply(gba));
	}
	map();
	showLogo(true);
	jitRomChanged(gba);
}

// The Xploder Advance (Europe) dump lacks a byte, X-Men's number of codes, where its code list ends
// (a dump of the chip as the program left it, a byte short): the program hangs reading the list.
// The byte goes back in, and the chip's last byte, which the program rewrites, goes.
static void repairDump(uint8_t* flash, uint32_t size) {
	static const char name[] = "X-Men: ROA";
	enum { AT = 0xF9C2, CODES = 6 };
	if (size == 0x20000 && !memcmp(&flash[AT], name, sizeof(name))) {
		memmove(&flash[AT + 1], &flash[AT], size - AT - 1);
		flash[AT] = CODES;
	}
}

bool fcdAttach(struct GBA* gba, const void* game, size_t size) {
	fcdDetach(fcd.gba);
	if (!gba || !fcdDetect(gba->rom, gba->romSize)) {
		return false;
	}
	memset(&fcd, 0, sizeof(fcd));
	if (game && size >= 0xC0) {
		fcd.game = calloc(1, SIZE_ROM);
		if (!fcd.game) {
			return false;
		}
		fcd.gameSize = (uint32_t) (size < SIZE_ROM ? size : SIZE_ROM);
		memcpy(fcd.game, game, fcd.gameSize);
	}
	fcd.gba = gba;
	fcd.flash = gba->rom;
	fcd.flashSize = gba->romSize;
	repairDump(fcd.flash, fcd.flashSize);
	// The chip as the cartridge's image has it, before its save: a state holds what differs from
	// it, so that every player has their own copy of the cartridge and none is sent in netplay
	fcd.factory = malloc(fcd.flashSize);
	if (!fcd.factory) {
		free(fcd.game);
		memset(&fcd, 0, sizeof(fcd));
		return false;
	}
	memcpy(fcd.factory, fcd.flash, fcd.flashSize);
	bool const xploder = fcd.flashSize == 0x20000;
	fcd.registers = xploder ? 0x200000 : 0x10000;
	fcd.claimed = xploder ? 0x200004 : 0x20000;
	resetState();
	return true;
}

bool fcdSetGame(struct GBA* gba, const void* game, size_t size) {
	if (!fcdActive(gba)) {
		return false;
	}
	uint8_t* fresh = NULL;
	uint32_t freshSize = 0;
	if (game && size >= 0xC0) {
		fresh = calloc(1, SIZE_ROM);
		if (!fresh) {
			return false;
		}
		freshSize = (uint32_t) (size < SIZE_ROM ? size : SIZE_ROM);
		memcpy(fresh, game, freshSize);
	}
	unpatch();
	showId(false);
	showLogo(false);
	gba->cartTrapped = false;
	gba->cartTrapAt = 0;
	gba->rom = fcd.flash;
	gba->romSize = fcd.flashSize;
	gba->romMask = pow2(fcd.flashSize) - 1;
	free(fcd.game);
	fcd.game = fresh;
	fcd.gameSize = freshSize;
	jitRomChanged(gba);
	return true;
}

void fcdDetach(struct GBA* gba) {
	if (!fcdActive(gba)) {
		return;
	}
	unpatch();
	showId(false);
	showLogo(false);
	gba->cartTrapped = false;
	gba->cartTrapAt = 0;
	gba->rom = fcd.flash;
	gba->romSize = fcd.flashSize;
	gba->romMask = pow2(fcd.flashSize) - 1;
	free(fcd.game);
	free(fcd.kept);
	free(fcd.factory);
	memset(&fcd, 0, sizeof(fcd));
}

uint8_t* fcdFlash(size_t* size) {
	*size = fcd.gba ? fcd.flashSize : 0;
	return fcd.gba ? fcd.flash : NULL;
}

// The chip as kept: without the ID or the game's logo over it.
const uint8_t* fcdKept(size_t* size) {
	*size = fcd.gba ? fcd.flashSize : 0;
	if (!fcd.gba || (!fcd.s.flashId && !fcd.s.boot)) {
		return fcd.gba ? fcd.flash : NULL;
	}
	if (!fcd.kept && !(fcd.kept = malloc(fcd.flashSize))) {
		*size = 0;
		return NULL;
	}
	memcpy(fcd.kept, fcd.flash, fcd.flashSize);
	if (fcd.s.flashId) {
		set16(&fcd.kept[0], fcd.s.idWas[0]);
		set16(&fcd.kept[2], fcd.s.idWas[1]);
	}
	if (fcd.s.boot) {
		memcpy(&fcd.kept[LOGO_FROM], fcd.logoWas, sizeof(fcd.logoWas));
	}
	return fcd.kept;
}

bool fcdFlashChanged(void) {
	bool const was = fcd.flashChanged;
	fcd.flashChanged = false;
	return was;
}

size_t fcdStateSize(void) {
	return sizeof(struct FcdState) + fcd.flashSize;
}

void fcdSaveState(struct GBA* gba, void* state) {
	if (!fcdActive(gba)) {
		return;
	}
	size_t size;
	const uint8_t* const kept = fcdKept(&size);
	// What the game had under the hook and the chip under its ID come from this device's own copies
	// as a state loads, so a state holds none of the game or the cartridge
	struct FcdState s = fcd.s;
	memset(s.hookWas, 0, sizeof(s.hookWas));
	memset(s.idWas, 0, sizeof(s.idWas));
	memcpy(state, &s, sizeof(s));
	// The chip as it differs from the cartridge's own image
	uint8_t* const out = (uint8_t*) state + sizeof(s);
	for (size_t i = 0; kept && i < size; ++i) {
		out[i] = kept[i] ^ fcd.factory[i];
	}
}

// Before the console's state loads: the image the state ran with shows, so its size matches.
bool fcdPrepareState(struct GBA* gba, const void* state) {
	struct FcdState s;
	memcpy(&s, state, sizeof(s));
	if (!fcdActive(gba) || s.magic != STATE_MAGIC) {
		return false;
	}
	unpatch();
	showId(false);
	showLogo(false);
	fcd.s.page = s.registers[1] >> 13;
	map();
	return true;
}

void fcdLoadState(struct GBA* gba, const void* state) {
	if (!fcdActive(gba)) {
		return;
	}
	struct FcdState s;
	memcpy(&s, state, sizeof(s));
	// The state's chip, as it differs from the cartridge's image
	const uint8_t* const flash = (const uint8_t*) state + sizeof(s);
	bool changes = false;
	for (uint32_t i = 0; i < fcd.flashSize; ++i) {
		uint8_t const value = flash[i] ^ fcd.factory[i];
		changes |= fcd.flash[i] != value;
		fcd.flash[i] = value;
	}
	if (changes) {
		jitRomChanged(gba);
	}
	resetState();
	memcpy(fcd.s.registers, s.registers, sizeof(s.registers));
	// From the registers, as they set them: a state's own could point the hook anywhere
	decodeRegisters();
	fcd.s.hookOn = s.hookOn;
	fcd.s.flashStep = s.flashStep;
	fcd.s.flashProgram = s.flashProgram;
	map();
	showId(s.flashId);
	showLogo(s.boot);
	// The console's state keeps what the CPU watched, which compiled code was laid out for (see
	// gbaLoadState), so patch() throws it away when the state's trigger differs.
	patch();
}
