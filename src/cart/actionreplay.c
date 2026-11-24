/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gba.h"
#include "cpu/jit.h"

#include <stdlib.h>
#include <string.h>

// Datel's Action Replay for the Game Boy Advance (Action Replay v3, GBX and MAX, Pro Action Replay):
// a cartridge with a game plugged in on top, a switch, a button, and a flash chip holding its program,
// its code lists and the code engine. Nothing about its hardware is published; this is what its
// program does with it.
//
// - 0x0800009C, written, says what the cartridge space shows. Bit 0 shows the game on top instead of
//   the flash chip, bits 1 and 2 put the hook's patches and its code over the game, and bit 6 shows
//   the code engine at 0x09FE0400. The program runs with 0x120 and reads the game with 0x101; a game
//   runs with 1 (no codes) or 7, and the hook switches to 0x67 to call the engine and back to 7.
// - 0x09FE0000-0x09FE03FF are 32 registers, 0x20 apart. Six of them hold patches, each the address
//   of a halfword of the game (in halfwords, low then high register) and what it reads instead:
//   (0x000, 0x020, 0x040), (0x080, 0x0A0, 0x0C0), (0x100, 0x120, 0x060), (0x200, 0x220, 0x0E0),
//   (0x280, 0x2A0, 0x240) and (0x2C0, 0x2E0, 0x260). 0x180 and 0x1A0 keep what the engine writes
//   (the code it is on, and a delay); 0x1C0 is the status: the switch in bits 12-15 (all set for
//   right, codes on), the button in bit 10 (clear while pressed), and the PC link in bits 8 and 11.
//   0x160 is the hardware's version, which changes where the switch reads; 0 is the one above.
//   The first cartridges (the GameShark's) have only the first three patch slots, and show the
//   engine from 0x09FE0200; their program calls it there.
// - The flash chip takes the commands of SST's 16-bit chips at 0x0800AAAA and 0x08005554: ID (90),
//   word program (A0), and erase (80, then 30 for 4 KiB, 50 for 64 KiB, 10 for all of it).
// - To start a game, the program writes a 4 KiB block to its flash at 0x20000: at 0x20 the code a
//   hook jumps to (it calls the engine at 0x09FE0401, then runs the instructions the hook covers), at
//   0x400 the engine, which runs one code each call, and at 0x800 the codes. It patches the hook
//   (a BL to 0x08000020) in at the address the game's master code gives, and jumps to the game's
//   0x08000000. The block's start shows over the game's logo, which only the BIOS reads, and the rest
//   at 0x09FE0400 while the engine runs.
// - Some games are started with the switch to the left (codes off) and moved right once they run:
//   the program waits for it in work RAM. The switch reads left to code running there, so the game
//   starts as if it was moved left, then right.

enum {
	// Offsets in the cartridge space.
	AR_CONTROL = 0x9C,
	AR_WINDOW = 0x01FE0000,
	AR_WINDOW_SIZE = 0x1000,
	AR_ENGINE = 0x400,
	AR_ENGINE_FIRST = 0x200,
	AR_STATUS = 0x1C0,
	AR_VERSION = 0x160,
	// Where in the flash chip the program leaves the hook's code, the engine and the codes.
	AR_BLOCK = 0x20000,
	AR_OVERLAY_FROM = 0x20,
	AR_OVERLAY_TO = 0xA0,

	CONTROL_GAME = 0x01,
	CONTROL_PATCHES = 0x02,
	CONTROL_OVERLAY = 0x04,
	CONTROL_ENGINE = 0x40,

	SLOTS = 6,
	STATE_MAGIC = 0x52414743,
};

// A patch slot that holds nothing.
#define NO_PATCH 0xFFFFFFFFu

static const uint16_t slotRegisters[SLOTS][3] = {
	{ 0x000, 0x020, 0x040 }, { 0x080, 0x0A0, 0x0C0 }, { 0x100, 0x120, 0x060 },
	{ 0x200, 0x220, 0x0E0 }, { 0x280, 0x2A0, 0x240 }, { 0x2C0, 0x2E0, 0x260 },
};

// What a state holds of the cartridge besides its flash chip, which follows it: everything but the
// game's image.
struct ActionReplayState {
	uint32_t magic;
	uint16_t control;
	uint16_t registers[32];
	uint8_t flashStep;
	bool flashId;
	bool flashProgram;
	bool overlaid;
	uint32_t patchAt[SLOTS];
	uint16_t patchWas[SLOTS];
	uint16_t patchValue[SLOTS];
	uint16_t idWas[2];
	uint8_t overlayWas[AR_OVERLAY_TO - AR_OVERLAY_FROM];
};

static struct {
	struct GBA* gba;
	// The flash chip, which is the image the console loaded, and the game on top, if there is one,
	// in a buffer of the whole cartridge space.
	uint8_t* flash;
	uint32_t flashSize;
	uint8_t* game;
	uint32_t gameSize;
	// Where the engine shows from: the first cartridges show it from 0x200.
	uint32_t engine;
	// The flash chip as kept, while the ID shows over its start (see actionReplayKept)
	uint8_t* kept;
	// The flash chip as the cartridge's image has it, before its save: a state holds what differs
	// from it, so that every player has their own copy of the cartridge and none is sent in netplay
	uint8_t* factory;
	bool button;
	bool flashChanged;
	struct ActionReplayState s;
} ar;

// Whether the cartridge attached is Future Console Design's (fcd.c), whose functions these hand to.
static bool fcd(void) {
	return !ar.gba && fcdFlash(&(size_t) { 0 });
}

static uint16_t get16(const uint8_t* p) {
	return (uint16_t) (p[0] | p[1] << 8);
}

static void set16(uint8_t* p, uint16_t value) {
	p[0] = (uint8_t) value;
	p[1] = (uint8_t) (value >> 8);
}

static uint32_t count32(const uint8_t* data, size_t size, uint32_t value) {
	uint32_t n = 0;
	for (size_t i = 0; i + 4 <= size; i += 4) {
		n += data[i] == (uint8_t) value && data[i + 1] == (uint8_t) (value >> 8) && data[i + 2] == (uint8_t) (value >> 16) &&
		     data[i + 3] == (uint8_t) (value >> 24);
	}
	return n;
}

// An Action Replay's program uses its status register and its control register all through.
bool actionReplayDetect(const void* data, size_t size) {
	if (fcdDetect(data, size)) {
		return true;
	}
	return size >= 0x40000 && size <= 0x200000 && count32(data, size, 0x09FE01C0) >= 3 && count32(data, size, 0x0800009C) >= 3;
}

static bool arActive(const struct GBA* gba) {
	return gba && ar.gba == gba;
}

bool actionReplayActive(const struct GBA* gba) {
	return arActive(gba) || fcdActive(gba);
}

// The cartridge answers in the first 32 MiB of the space, not in its mirrors with other wait states.
static bool ours(const struct GBA* gba, uint32_t address) {
	return arActive(gba) && (address >> 25) == 4;
}

static bool gameShown(void) {
	return ar.game && (ar.s.control & CONTROL_GAME);
}

static uint32_t pow2(uint32_t value) {
	uint32_t p = 1;
	while (p < value) {
		p <<= 1;
	}
	return p;
}

// Puts the image the control register picks in the cartridge space. The game stops short of the
// registers, which a 32 MiB game's last 128 KiB can't be seen behind.
static void map(void) {
	struct GBA* gba = ar.gba;
	uint8_t* rom = ar.flash;
	uint32_t size = ar.flashSize;
	if (ar.s.control & CONTROL_GAME) {
		rom = ar.game ? ar.game : ar.flash;
		size = ar.game ? (ar.gameSize < AR_WINDOW ? ar.gameSize : AR_WINDOW) : 0;
	}
	if (gba->rom == rom && gba->romSize == size) {
		return;
	}
	gba->rom = rom;
	gba->romSize = size;
	gba->romMask = pow2(size) - 1;
	jitRomChanged(gba);
}

static void changed(uint32_t offset, uint32_t size) {
	jitInvalidate(ar.gba, 0x08000000 + offset, size);
}

// Takes the patches and the hook's code off the game, newest first.
static void unpatch(void) {
	for (int i = SLOTS; i-- > 0;) {
		if (ar.s.patchAt[i] != NO_PATCH) {
			set16(&ar.game[ar.s.patchAt[i]], ar.s.patchWas[i]);
			changed(ar.s.patchAt[i], 2);
			ar.s.patchAt[i] = NO_PATCH;
		}
	}
	if (ar.s.overlaid) {
		memcpy(&ar.game[AR_OVERLAY_FROM], ar.s.overlayWas, sizeof(ar.s.overlayWas));
		changed(AR_OVERLAY_FROM, sizeof(ar.s.overlayWas));
		ar.s.overlaid = false;
	}
}

static uint32_t slotAddress(int slot) {
	uint32_t const halfwords = (uint32_t) ar.s.registers[slotRegisters[slot][1] >> 5] << 16 | ar.s.registers[slotRegisters[slot][0] >> 5];
	uint32_t const offset = (halfwords << 1) & (SIZE_ROM - 1);
	return offset + 2 <= ar.gameSize && offset + 2 <= AR_WINDOW ? offset : NO_PATCH;
}

// Whether the patches and code on the game are those the registers ask for.
static bool patchesCurrent(void) {
	bool const on = gameShown() && (ar.s.control & CONTROL_PATCHES);
	for (int i = 0; i < SLOTS; ++i) {
		uint32_t const want = on ? slotAddress(i) : NO_PATCH;
		uint16_t const value = ar.s.registers[slotRegisters[i][2] >> 5];
		if (want != ar.s.patchAt[i] || (want != NO_PATCH && value != ar.s.patchValue[i])) {
			return false;
		}
	}
	return ar.s.overlaid == (gameShown() && (ar.s.control & CONTROL_OVERLAY) && ar.flashSize >= AR_BLOCK + AR_OVERLAY_TO);
}

// Puts on the game what the registers ask for, when it differs from what is there.
static void patch(void) {
	if (!ar.game || patchesCurrent()) {
		return;
	}
	unpatch();
	if (!gameShown()) {
		return;
	}
	if ((ar.s.control & CONTROL_OVERLAY) && ar.flashSize >= AR_BLOCK + AR_OVERLAY_TO) {
		memcpy(ar.s.overlayWas, &ar.game[AR_OVERLAY_FROM], sizeof(ar.s.overlayWas));
		memcpy(&ar.game[AR_OVERLAY_FROM], &ar.flash[AR_BLOCK + AR_OVERLAY_FROM], sizeof(ar.s.overlayWas));
		changed(AR_OVERLAY_FROM, sizeof(ar.s.overlayWas));
		ar.s.overlaid = true;
	}
	if (!(ar.s.control & CONTROL_PATCHES)) {
		return;
	}
	for (int i = 0; i < SLOTS; ++i) {
		uint32_t const at = slotAddress(i);
		if (at == NO_PATCH) {
			continue;
		}
		ar.s.patchAt[i] = at;
		ar.s.patchWas[i] = get16(&ar.game[at]);
		ar.s.patchValue[i] = ar.s.registers[slotRegisters[i][2] >> 5];
		set16(&ar.game[at], ar.s.patchValue[i]);
		changed(at, 2);
	}
}

// The hook switches the engine in and out on every call, which changes nothing on the game.
static void writeControl(uint16_t value) {
	bool const sameImage = !((ar.s.control ^ value) & CONTROL_GAME);
	ar.s.control = value;
	if (sameImage && patchesCurrent()) {
		return;
	}
	unpatch();
	map();
	patch();
}

// The flash chip's ID: SST's 39VF200A, 400A or 800A, by size. The program doesn't check it.
static uint16_t flashDevice(void) {
	return ar.flashSize > 0x80000 ? 0x2781 : ar.flashSize > 0x40000 ? 0x2780 : 0x2789;
}

// The ID shows in place of the chip's first two halfwords, which the program reads from work RAM.
static void showId(bool on) {
	if (on == ar.s.flashId) {
		return;
	}
	if (on) {
		ar.s.idWas[0] = get16(&ar.flash[0]);
		ar.s.idWas[1] = get16(&ar.flash[2]);
		set16(&ar.flash[0], 0x00BF);
		set16(&ar.flash[2], flashDevice());
	} else {
		set16(&ar.flash[0], ar.s.idWas[0]);
		set16(&ar.flash[2], ar.s.idWas[1]);
		ar.s.idWas[0] = ar.s.idWas[1] = 0;
	}
	ar.s.flashId = on;
	changed(0, 4);
}

static void erase(uint32_t at, uint32_t size) {
	at &= ~(size - 1);
	if (at >= ar.flashSize) {
		return;
	}
	if (size > ar.flashSize - at) {
		size = ar.flashSize - at;
	}
	memset(&ar.flash[at], 0xFF, size);
	ar.flashChanged = true;
	if (size > 0x1000) {
		jitRomChanged(ar.gba);
	} else {
		changed(at, size);
	}
}

// A write to the chip: a step of a command, or the word a program command writes. Every step
// finishes at once, so the program's toggle and data polling see it done.
static void writeFlash(uint32_t offset, uint16_t value) {
	if (ar.s.flashProgram) {
		ar.s.flashProgram = false;
		if (offset + 2 <= ar.flashSize) {
			uint16_t const was = get16(&ar.flash[offset & ~1u]);
			uint16_t const now = was & value;
			if (now != was) {
				set16(&ar.flash[offset & ~1u], now);
				ar.flashChanged = true;
				changed(offset & ~1u, 2);
			}
		}
		return;
	}
	uint8_t const command = (uint8_t) value;
	if (command == 0xF0) {
		showId(false);
		ar.s.flashStep = 0;
		return;
	}
	switch (ar.s.flashStep) {
	case 0:
	case 3:
		ar.s.flashStep = offset == 0xAAAA && command == 0xAA ? ar.s.flashStep + 1 : 0;
		break;
	case 1:
	case 4:
		ar.s.flashStep = offset == 0x5554 && command == 0x55 ? ar.s.flashStep + 1 : 0;
		break;
	case 2:
		ar.s.flashStep = 0;
		if (offset != 0xAAAA) {
			break;
		}
		if (command == 0x90) {
			showId(true);
		} else if (command == 0xA0) {
			ar.s.flashProgram = true;
		} else if (command == 0x80) {
			ar.s.flashStep = 3;
		}
		break;
	case 5:
		ar.s.flashStep = 0;
		if (command == 0x30) {
			erase(offset, 0x1000);
		} else if (command == 0x50) {
			erase(offset, 0x10000);
		} else if (command == 0x10 && offset == 0xAAAA) {
			erase(0, pow2(ar.flashSize));
		}
		break;
	}
}

static uint16_t status(void) {
	uint16_t value = 0;
	if (ar.gba->activeRegion != REGION_IWRAM) {
		value |= 0xF000;
	}
	if (!ar.button) {
		value |= 0x0400;
	}
	return value;
}

static uint16_t read16(uint32_t offset) {
	uint32_t const at = offset & (AR_WINDOW_SIZE - 2);
	if (at >= ar.engine) {
		uint32_t const block = AR_BLOCK + at;
		return (ar.s.control & CONTROL_ENGINE) && block + 2 <= ar.flashSize ? get16(&ar.flash[block]) : (uint16_t) ((0x08000000 + offset) >> 1);
	}
	switch (at) {
	case AR_STATUS:
		return status();
	case AR_VERSION:
		return 0;
	default:
		return ar.s.registers[at >> 5];
	}
}

// A read past the end of what the cartridge space shows: the registers, and the engine while the
// hook runs it.
bool actionReplayRead(struct GBA* gba, uint32_t address, int size, uint32_t* value) {
	if (fcdActive(gba)) {
		return fcdRead(gba, address, size, value);
	}
	uint32_t const offset = address & (SIZE_ROM - 1);
	if (!ours(gba, address) || (offset & ~(AR_WINDOW_SIZE - 1)) != AR_WINDOW) {
		return false;
	}
	uint32_t const low = read16(offset & ~1u);
	switch (size) {
	case 4:
		*value = read16(offset & ~3u) | (uint32_t) read16((offset & ~3u) + 2) << 16;
		break;
	case 2:
		*value = low;
		break;
	default:
		*value = (low >> ((offset & 1) * 8)) & 0xFF;
		break;
	}
	return true;
}

// The engine, for running: the block's code at 0x09FE0400 while the hook calls it.
const uint8_t* actionReplayCode(struct GBA* gba, uint32_t address) {
	if (fcdActive(gba)) {
		return fcdCode(gba, address);
	}
	uint32_t const offset = address & (SIZE_ROM - 1);
	if (!ours(gba, address) || (offset & ~(AR_WINDOW_SIZE - 1)) != AR_WINDOW || (offset & (AR_WINDOW_SIZE - 1)) < ar.engine ||
	    !(ar.s.control & CONTROL_ENGINE) || AR_BLOCK + AR_WINDOW_SIZE > ar.flashSize) {
		return NULL;
	}
	return &ar.flash[AR_BLOCK + (offset & (AR_WINDOW_SIZE - 1))];
}

// A halfword written in the cartridge space. Returns whether the cartridge took it.
bool actionReplayWrite16(struct GBA* gba, uint32_t address, uint16_t value) {
	if (fcdActive(gba)) {
		return fcdWrite16(gba, address, value);
	}
	if (!ours(gba, address)) {
		return false;
	}
	uint32_t const offset = address & (SIZE_ROM - 2);
	if (offset == AR_CONTROL) {
		writeControl(value);
		return true;
	}
	if ((offset & ~(AR_WINDOW_SIZE - 1)) == AR_WINDOW) {
		uint32_t const at = offset & (AR_WINDOW_SIZE - 2);
		if (at < ar.engine && !(at & 0x1F)) {
			ar.s.registers[at >> 5] = value;
			patch();
		}
		return true;
	}
	if (!gameShown() && offset < ar.flashSize) {
		writeFlash(offset, value);
		return true;
	}
	return false;
}

// The button on the cartridge, which some codes wait for.
void actionReplayButton(bool pressed) {
	ar.button = pressed;
}

static void resetState(void) {
	memset(&ar.s, 0, sizeof(ar.s));
	ar.s.magic = STATE_MAGIC;
	for (int i = 0; i < 32; ++i) {
		ar.s.registers[i] = 0xFFFF;
	}
	for (int i = 0; i < SLOTS; ++i) {
		ar.s.patchAt[i] = NO_PATCH;
	}
}

// Power on: the program shows, and the game's own hardware (its save memory and clock) is what the
// console has, as the game would have without the Action Replay.
void actionReplayReset(struct GBA* gba) {
	fcdReset(gba);
	if (!arActive(gba)) {
		return;
	}
	unpatch();
	showId(false);
	resetState();
	ar.s.control = 0x120;
	if (ar.game) {
		gba->rom = ar.game;
		gba->romSize = ar.gameSize < AR_WINDOW ? ar.gameSize : AR_WINDOW;
		gpioSetDevices(gba, overridesApply(gba));
	}
	map();
	jitRomChanged(gba);
}

// Puts the Action Replay in a console that has loaded its image, with a game on top (or none).
bool actionReplayAttach(struct GBA* gba, const void* game, size_t size) {
	actionReplayDetach(ar.gba);
	fcdDetach(gba);
	if (gba && fcdDetect(gba->rom, gba->romSize)) {
		return fcdAttach(gba, game, size);
	}
	if (!gba || !gba->rom || !gba->romSize) {
		return false;
	}
	memset(&ar, 0, sizeof(ar));
	if (game && size >= 0xC0) {
		ar.game = calloc(1, SIZE_ROM);
		if (!ar.game) {
			return false;
		}
		ar.gameSize = (uint32_t) (size < SIZE_ROM ? size : SIZE_ROM);
		memcpy(ar.game, game, ar.gameSize);
	}
	ar.gba = gba;
	ar.flash = gba->rom;
	ar.flashSize = gba->romSize;
	ar.factory = malloc(ar.flashSize);
	if (!ar.factory) {
		free(ar.game);
		memset(&ar, 0, sizeof(ar));
		return false;
	}
	memcpy(ar.factory, ar.flash, ar.flashSize);
	// A program for the first cartridges calls the engine at 0x09FE0201, a later one at 0x09FE0401.
	bool const first = count32(ar.flash, ar.flashSize, 0x08000000 + AR_WINDOW + AR_ENGINE_FIRST + 1) > 0 &&
	                   !count32(ar.flash, ar.flashSize, 0x08000000 + AR_WINDOW + AR_ENGINE + 1);
	ar.engine = first ? AR_ENGINE_FIRST : AR_ENGINE;
	resetState();
	ar.s.control = 0x120;
	return true;
}

// Another game on top (or none), the chip as it is: it shows from the console's next reset.
bool actionReplaySetGame(struct GBA* gba, const void* game, size_t size) {
	if (fcd()) {
		return fcdSetGame(gba, game, size);
	}
	if (!arActive(gba)) {
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
	gba->rom = ar.flash;
	gba->romSize = ar.flashSize;
	gba->romMask = pow2(ar.flashSize) - 1;
	free(ar.game);
	ar.game = fresh;
	ar.gameSize = freshSize;
	jitRomChanged(gba);
	return true;
}

// Takes the Action Replay out, leaving its image in the cartridge space for the console to free.
void actionReplayDetach(struct GBA* gba) {
	fcdDetach(gba);
	if (!arActive(gba)) {
		return;
	}
	unpatch();
	showId(false);
	gba->rom = ar.flash;
	gba->romSize = ar.flashSize;
	gba->romMask = pow2(ar.flashSize) - 1;
	free(ar.game);
	free(ar.kept);
	free(ar.factory);
	memset(&ar, 0, sizeof(ar));
}

// The flash chip, which holds the codes added and the settings: the cartridge's own save.
uint8_t* actionReplayFlash(size_t* size) {
	if (fcd()) {
		return fcdFlash(size);
	}
	*size = ar.gba ? ar.flashSize : 0;
	return ar.gba ? ar.flash : NULL;
}

// What the chip holds, to keep: the flash, or while the program reads the chip's ID (shown over its
// first two halfwords), a copy with what those hold.
const uint8_t* actionReplayKept(size_t* size) {
	if (fcd()) {
		return fcdKept(size);
	}
	*size = ar.gba ? ar.flashSize : 0;
	if (!ar.gba || !ar.s.flashId) {
		return ar.gba ? ar.flash : NULL;
	}
	if (!ar.kept && !(ar.kept = malloc(ar.flashSize))) {
		*size = 0;
		return NULL;
	}
	memcpy(ar.kept, ar.flash, ar.flashSize);
	set16(&ar.kept[0], ar.s.idWas[0]);
	set16(&ar.kept[2], ar.s.idWas[1]);
	return ar.kept;
}

bool actionReplayFlashChanged(void) {
	if (fcd()) {
		return fcdFlashChanged();
	}
	bool const was = ar.flashChanged;
	ar.flashChanged = false;
	return was;
}

// States: the registers and what is patched where, then the flash chip (as kept, without the ID
// showing), so every device in netplay has the same code lists, menus and codes running.
size_t actionReplayStateSize(void) {
	if (fcd()) {
		return fcdStateSize();
	}
	return sizeof(struct ActionReplayState) + ar.flashSize;
}

void actionReplaySaveState(struct GBA* gba, void* state) {
	if (fcdActive(gba)) {
		fcdSaveState(gba, state);
		return;
	}
	if (arActive(gba)) {
		size_t size;
		const uint8_t* const kept = actionReplayKept(&size);
		// What the game and the chip had under the patches, the overlay and the ID come from this
		// device's own copies as a state loads, so a state holds none of the game or the cartridge
		struct ActionReplayState s = ar.s;
		memset(s.patchWas, 0, sizeof(s.patchWas));
		memset(s.overlayWas, 0, sizeof(s.overlayWas));
		memset(s.idWas, 0, sizeof(s.idWas));
		memcpy(state, &s, sizeof(s));
		// The chip as it differs from the cartridge's own image
		uint8_t* const out = (uint8_t*) state + sizeof(s);
		for (size_t i = 0; kept && i < size; ++i) {
			out[i] = kept[i] ^ ar.factory[i];
		}
	}
}

// Before the console's state loads: the image the state ran with shows, so its size matches.
bool actionReplayPrepareState(struct GBA* gba, const void* state) {
	if (fcdActive(gba)) {
		return fcdPrepareState(gba, state);
	}
	struct ActionReplayState s;
	memcpy(&s, state, sizeof(s));
	if (!arActive(gba) || s.magic != STATE_MAGIC) {
		return false;
	}
	unpatch();
	showId(false);
	ar.s.control = s.control;
	map();
	return true;
}

// After it: the registers, and the patches and ID put back as the state had them.
void actionReplayLoadState(struct GBA* gba, const void* state) {
	if (fcdActive(gba)) {
		fcdLoadState(gba, state);
		return;
	}
	if (!arActive(gba)) {
		return;
	}
	struct ActionReplayState s;
	memcpy(&s, state, sizeof(s));
	resetState();
	ar.s.control = s.control;
	memcpy(ar.s.registers, s.registers, sizeof(s.registers));
	ar.s.flashStep = s.flashStep;
	ar.s.flashProgram = s.flashProgram;
	// The state's flash chip, as it differs from the cartridge's image (the ID isn't showing here,
	// after actionReplayPrepareState)
	const uint8_t* const flash = (const uint8_t*) state + sizeof(s);
	bool changes = false;
	for (uint32_t i = 0; i < ar.flashSize; ++i) {
		uint8_t const value = flash[i] ^ ar.factory[i];
		changes |= ar.flash[i] != value;
		ar.flash[i] = value;
	}
	if (changes) {
		jitRomChanged(gba);
	}
	map();
	showId(s.flashId);
	patch();
}
