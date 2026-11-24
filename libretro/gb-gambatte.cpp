/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The libretro core's Game Boy and Game Boy Color side, running on Gambatte-Speedrun's core
// (gambatte/, GPL-2.0).

extern "C" {
#include "gb.h"
#include "libretro.h"
}
#include "dmg07.h"
#include "link.h"
#include "printer.h"
#include "megaduck/laptop.h"
#include "armgba-boot.h"

#include "gambatte.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace gambatte {
// MBC7's tilt sensor, as an offset from lying flat, and its flick (defined in mbc7.cpp).
extern int mbc7TiltX;
extern int mbc7TiltY;
extern bool mbc7Flick;
gb_ulong crc32(gb_ulong crc, void const* buf, std::size_t size);
}

namespace {

enum {
	// Gambatte produces sound at 2^21 Hz, 35112 samples per frame, and may overrun by up to 2064.
	NATIVE_RATE = 2097152,
	FRAME_SAMPLES = 35112,
	OVERRUN = 2064,
	// Extra room in the state size reported to the frontend: for the state to grow, and for the
	// extra work RAM a Game Boy Color has if the game restarts as one.
	STATE_SLACK = 4096 + 6 * 4096,
	MAX_PLAYERS = 4,
	// With exact link timing, the most samples run between looks at the network.
	EXACT_PART = 2048,
};

// Gambatte's linkStatus calls; a value below 256 shifts that byte in.
enum {
	LINK_CLOCKED = 256, // whether this Game Boy's own clock finished a transfer
	LINK_ACK = 257,     // clears that
	LINK_OUT = 258,     // the byte being sent (SB)
	LINK_ON = 264,
	LINK_OFF = 265,
	LINK_STOP = 266,    // ends the transfer under way, stopping the console's own clock
	LINK_CONTROL = 267, // SC
	LINK_LEFT = 268,    // cycles until the transfer under way ends, or -1
};

void linkClocked();
unsigned laptopControl();
void laptopIn(uint8_t byte);
bool laptopSlot();
// How many native samples make one frontend sample.
unsigned ratio;
unsigned cgbColors[32768];
// The boot ROM folder, kept so a game can be restarted as another model.
std::string systemPath;
bool hasSystemPath;
// The boot ROM choice from the option.
LibretroGbBoot bootWanted = LIBRETRO_GB_BOOT_NINTENDO;
// -1, or the kind of Mega Duck cartridge (see libretroGbSetMegaDuck)
int megaDuckKind = -1;
// The Mega Duck laptop's controller on the serial port (see libretroGbSetMegaDuckLaptop)
bool laptopWanted = false;
int laptopPrinter = 2;
DuckLaptopLanguage laptopLanguage = DUCK_LAPTOP_SPANISH;
// The cartridge in the laptop's slot, if any, and its kind
std::vector<unsigned char> laptopCartridge;
int laptopCartridgeKind = 0;
// Whether the cartridge in the laptop's slot has taken the system ROM's place, as states keep it
bool laptopSlotIn;
// The codes the Game Genie fills its empty slots with (libretroGbSetGameGenieCodes)
static unsigned char genieDigits[27] = { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
                                         0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10 };
static unsigned char genieCounts[3];
// Datel's Action Replay in front of the game (libretroGbSetActionReplay): its program, whether a game
// is on top, and what it keeps with the power off as it was last read out of the console
std::vector<unsigned char> replayProgram;
bool replayGame;
std::vector<unsigned char> replayKept;

// A cartridge's clock, kept by the frontend as the .rtc file. Other cores read their part of it:
// - 8 bytes: the Unix time the clock would have started from, gambatte-libretro's whole file;
// - "aGBm", and the Unix time it was saved;
// - an MBC3's registers at that time, 4 bytes each, seconds first, then the time again: what mGBA
//   reads of its own file;
// - the cycles into the second and the latched registers, which carry more detail;
// - the Clock Offset the clock was moved by, and how much of a move back it couldn't make, in
//   seconds.
// A HuC3 chip's file has "aGBh" and the chip's whole state after the saved time. Files from before
// the mGBA part ("aGBc") had the registers and the offset where mGBA's part is now.
enum {
	RTC_BASE = 0,
	RTC_MAGIC = 8,
	RTC_SAVED = 12,
	RTC_REGS = 20,
	MGBA_REGS = 20,
	MGBA_SAVED = 40,
	RTC_CYCLES = 48,
	RTC_LATCH = 52,
	RTC_MOVED = 64,
	RTC_SHORT = 72,
	RTC_SIZE = 80,
	OLD_OFFSET = RTC_REGS + 14,
	HUC3_STATE = 0x104,
	HUC3_SIZE = RTC_REGS + HUC3_STATE,
	// In the HuC3 state: the cycles into the current minute, then I/O with the minutes and days as
	// 4-bit fields.
	HUC3_MINUTES = 4 + 0x10,
	HUC3_DAYS = 4 + 0x13,
	// A host clock before 2024 was never set, so it says nothing about the time the game was off.
	SANE_TIME = 1704067200,
};
// The registers in the order Gambatte keeps them (mem/rtc.h), and the length of a second in rtc_c's
// cycles.
enum { RTC_DH, RTC_DL, RTC_H, RTC_M, RTC_S, RTC_C, RTC_LATCHED };
unsigned long const RTC_SECOND = 0x400000;

// How the output capacitor is wired: a Game Boy's, connected while a DAC is on; a Game Boy Advance's,
// after its amp, always connected, per side with the speaker; or its headphones', one charge their
// two sides share.
enum class Wiring { GAME_BOY, ADVANCE, SHARED_RETURN };

// One Game Boy. There are two to four when the consoles of a link run here; s is the one being worked
// on.
struct Console {
	gambatte::GB* gb;
	// The CRC of the boot ROM from the system folder it started with, 0 for armGBA's or none.
	uint32_t bootCrc;
	// Whether Gambatte's serial port goes to another Game Boy.
	bool linked;
	unsigned loadFlags;
	unsigned pressed;
	gambatte::uint_least32_t pixels[LIBRETRO_GB_WIDTH * LIBRETRO_GB_HEIGHT];
	// The last whole picture, when both consoles run here and a frame can end mid-picture.
	gambatte::uint_least32_t shown[LIBRETRO_GB_WIDTH * LIBRETRO_GB_HEIGHT];
	gambatte::uint_least32_t native[FRAME_SAMPLES + OVERRUN];
	// A frame's sound for the frontend: at most two frames' worth of native samples, two channels
	// each, and the running sums towards the next sample.
	int16_t samples[(FRAME_SAMPLES + OVERRUN) * 2 / 16 * 2 + 8];
	size_t sampleCount;
	long sumLeft;
	long sumRight;
	unsigned summed;
	// The output capacitor's charge per side, in samples x 256, and how much of it it keeps each
	// frontend sample, out of 0x10000 (0: none, the Game Boy Audio Filter off). Saved with states.
	// With headphones on a Game Boy Advance, the one charge both sides share is capacitor[0].
	int32_t capacitor[2];
	int32_t charge;
	Wiring wiring;
	size_t stateSize;
	// The game, kept so it can be restarted as another model.
	std::vector<unsigned char> game;
	LibretroGbModel chosen;
	// The model the Game Boy Model option asks for. Loading a state from another model may have
	// switched the machine away from it.
	LibretroGbModel wanted;
	// The boot ROM this game actually started with.
	LibretroGbBoot bootUsed;
	// The cheat codes in force, in Gambatte's format. Loading the game again clears them, so
	// they're reapplied.
	std::string gameGenie;
	std::string gameShark;
	unsigned char rtcFile[HUC3_SIZE];
	// The Clock Offset the MBC3 clock was moved by, and how much of a move back the clock couldn't
	// make, at day 0; a move forwards makes that up first.
	long long clockMoved;
	long long clockShort;
	// The frontend fills rtcFile after the game is loaded, so the file is read just before the
	// first frame.
	bool rtcPending;
	// Native samples run past the end of the last frame, when the consoles of a link run here.
	long ahead;
	// Channel Selection: the same sums and charges for what's heard, with channels left out, apart
	// from the console's from when the first is (heardApart; before, they are the console's). Not
	// saved: states keep the console's.
	long heardLeft;
	long heardRight;
	int32_t heardCapacitor[2];
	bool heardApart;
};
Console consoles[MAX_PLAYERS];

// Layer Selection's layers shown (1 background, 2 sprites, 4 window) and Channel Selection's channels
// left out (bits 0-3: channels 1-4), for every console: only what's drawn and heard changes.
static unsigned layersShown = 7;
static unsigned channelsMuted;

static void applyShownAndHeard(gambatte::GB* gb) {
	gb->setShownLayers(layersShown);
	gb->setMutedChannels(channelsMuted);
}
Console* s = &consoles[0];

// The Game Boy Audio Filter, and the output capacitor's charge factor a 4 MiHz cycle (Pan Docs, Audio
// Details): the original Game Boy's 0.999958 (about 28 Hz), the Pocket's and the Color's 0.998943
// (about 700 Hz). A Game Boy Advance has its own output's (GBA Audio Output), always connected, or
// with that off the original's. A Mega Duck is taken as an original Game Boy.
enum LibretroGbFilter audioFilter = LIBRETRO_GB_FILTER_ACCURATE;
enum LibretroAdvanceOutput advanceOutput = LIBRETRO_ADVANCE_SPEAKER;

void setCapacitor(Console& c) {
	c.wiring = Wiring::GAME_BOY;
	if (audioFilter == LIBRETRO_GB_FILTER_OFF) {
		c.charge = 0;
		return;
	}
	bool const advance = c.loadFlags & gambatte::GB::GBA_FLAG;
	if (audioFilter == LIBRETRO_GB_FILTER_ACCURATE && advance && advanceOutput != LIBRETRO_ADVANCE_OFF) {
		c.wiring = advanceOutput == LIBRETRO_ADVANCE_HEADPHONES ? Wiring::SHARED_RETURN : Wiring::ADVANCE;
		// Per frontend sample: 2097152 / ratio a second
		double const seconds = ratio / 2097152.0;
		c.charge = static_cast<int32_t>(std::lround(std::exp(-seconds / (ADVANCE_OUTPUT_OHMS(advanceOutput) * ADVANCE_OUTPUT_FARADS)) * 0x10000));
		return;
	}
	bool const pocketOrLater = !advance && c.loadFlags & (gambatte::GB::MGB_FLAG | gambatte::GB::CGB_MODE);
	double const perCycle = audioFilter == LIBRETRO_GB_FILTER_ACCURATE && pocketOrLater ? 0.998943 : 0.999958;
	// Per frontend sample: 4194304 / (2097152 / ratio) cycles
	c.charge = static_cast<int32_t>(std::lround(std::pow(perCycle, 2.0 * ratio) * 0x10000));
}
// The CPU Clock, in quarters of the console's own clock.
static unsigned cpuQuarters = 4;
// The Clock options: Game Time, the speed in percent and the offset in seconds.
static bool gameTime;
static unsigned clockPercent = 100;
static long long clockOffset;
// Real Time outside netplay: the clock runs on the device's time, so fast-forward and pauses don't
// move it, and loaded states leave it as it is.
static bool deviceClock;
// How many consoles of a link run here (1 without one), and whether the Four Player Adapter joins
// them; two without it share the cable.
int players = 1;
bool adapterOn;

Dmg07 adapter;
// The Game Boy Printer on player 1's port, and where its prints go.
Printer printer;
bool printerOn;
PrinterOutput printed;

// One transfer on the Four Player Adapter: each Game Boy waiting on the external clock takes the
// adapter's byte and gives its own.
void adapterTransfer() {
	uint8_t in[DMG07_PORTS];
	for (int i = 0; i < DMG07_PORTS; ++i) {
		in[i] = 0xFF;
		gambatte::GB* const gb = i < players ? consoles[i].gb : 0;
		if (gb && (gb->linkStatus(LINK_CONTROL) & 0x81) == 0x80) {
			in[i] = gb->linkStatus(LINK_OUT);
			gb->linkStatus(dmg07Out(&adapter, i));
		}
	}
	dmg07Transfer(&adapter, in);
}

void put64(unsigned char* to, unsigned long long value) {
	for (int i = 0; i < 8; ++i) {
		to[i] = value >> i * 8;
	}
}

unsigned long long get64(const unsigned char* from) {
	unsigned long long value = 0;
	for (int i = 7; i >= 0; --i) {
		value = value << 8 | from[i];
	}
	return value;
}

// Advances a clock by some seconds the way the chip counts: a field past its range counts up to its
// last value and wraps to 0 without a carry; past 511 days, the day counter wraps and sets its
// carry bit.
void runClock(gb_ulong r[11], unsigned long long seconds) {
	if (r[RTC_DH] & 0x40) {
		return;
	}
	static const unsigned limits[3] = { 60, 60, 24 };
	static const unsigned ends[3] = { 64, 64, 32 };
	static const int fields[3] = { RTC_S, RTC_M, RTC_H };
	unsigned long long carry = seconds;
	for (int i = 0; i < 3 && carry; ++i) {
		gb_ulong& value = r[fields[i]];
		if (value >= limits[i]) {
			if (carry < ends[i] - value) {
				value += carry;
				return;
			}
			carry -= ends[i] - value;
			value = 0;
		}
		unsigned long long const total = value + carry;
		value = total % limits[i];
		carry = total / limits[i];
	}
	unsigned long long days = ((r[RTC_DH] & 1) << 8 | r[RTC_DL]) + carry;
	if (days > 511) {
		r[RTC_DH] |= 0x80;
	}
	r[RTC_DL] = days & 0xFF;
	r[RTC_DH] = (r[RTC_DH] & 0xFE) | (days >> 8 & 1);
}

// Moves a running clock by some seconds, forwards or back, though not before day 0. Returns how much
// of a move back it couldn't make.
long long moveClock(gb_ulong r[11], long long seconds) {
	if (r[RTC_DH] & 0x40 || r[RTC_H] >= 24 || r[RTC_M] >= 60 || r[RTC_S] >= 60) {
		return 0;
	}
	long long const day = 24 * 60 * 60;
	long long total = (((r[RTC_DH] & 1) << 8 | r[RTC_DL]) * 24 + r[RTC_H]) * 3600 + r[RTC_M] * 60 + r[RTC_S] + seconds;
	long long const shortBy = total < 0 ? -total : 0;
	total += shortBy;
	if (total >= 512 * day) {
		total %= 512 * day;
		r[RTC_DH] |= 0x80;
	}
	r[RTC_S] = total % 60;
	r[RTC_M] = total / 60 % 60;
	r[RTC_H] = total / 3600 % 24;
	r[RTC_DL] = total / day & 0xFF;
	r[RTC_DH] = (r[RTC_DH] & 0xFE) | (total / day >> 8 & 1);
	return shortBy;
}

// Moves the selected console's clock from one Clock Offset to another, making up first what an
// earlier move back couldn't make.
void moveBy(Console& c, gb_ulong r[11], long long offset) {
	long long seconds = offset - c.clockMoved;
	if (seconds > 0) {
		long long const owed = seconds < c.clockShort ? seconds : c.clockShort;
		c.clockShort -= owed;
		seconds -= owed;
	}
	c.clockShort += moveClock(r, seconds);
	c.clockMoved = offset;
}

// How much of rtcFile the cartridge's clock uses, or 0 if it has none.
size_t rtcSize() {
	return !s->gb ? 0 : s->gb->hasMbc3Rtc() ? RTC_SIZE : s->gb->hasHuC3() ? HUC3_SIZE : 0;
}

// Reads a 12-bit count stored as three 4-bit fields, lowest first.
unsigned long getNibbles(const unsigned char* at) {
	return (at[0] & 0xF) | (at[1] & 0xF) << 4 | (at[2] & 0xF) << 8;
}

void putNibbles(unsigned char* at, unsigned long value) {
	for (int i = 0; i < 3; ++i) {
		at[i] = value >> i * 4 & 0xF;
	}
}

// Advances a HuC3 clock by some seconds the way the chip counts: minutes up to 1440, then days up
// to 4096.
void runHuC3(unsigned char* state, unsigned long long seconds) {
	unsigned long long const minute = RTC_SECOND * 60;
	unsigned long long cycles = (unsigned long long) state[0] << 24 | state[1] << 16 | state[2] << 8 | state[3];
	cycles = cycles % minute + seconds % 60 * RTC_SECOND;
	unsigned long long minutes = getNibbles(state + HUC3_MINUTES) + seconds / 60 + cycles / minute;
	cycles %= minute;
	unsigned long long const days = getNibbles(state + HUC3_DAYS) + minutes / 1440;
	putNibbles(state + HUC3_MINUTES, minutes % 1440);
	putNibbles(state + HUC3_DAYS, days & 0xFFF);
	for (int i = 0; i < 4; ++i) {
		state[i] = cycles >> (24 - i * 8);
	}
}

unsigned long get32(const unsigned char* from) {
	return from[0] | from[1] << 8 | from[2] << 16 | (unsigned long) from[3] << 24;
}

void put32(unsigned char* to, unsigned long value) {
	for (int i = 0; i < 4; ++i) {
		to[i] = value >> i * 8;
	}
}

// Whether a file holds mGBA's registers and the time they were saved, sane ones.
bool mgbaFile(const unsigned char* file, unsigned long long now) {
	static const unsigned ends[5] = { 64, 64, 32, 256, 256 };
	unsigned long long const saved = get64(file + MGBA_SAVED);
	for (int i = 0; i < 5; ++i) {
		if (get32(file + MGBA_REGS + i * 4) >= ends[i]) {
			return false;
		}
	}
	return saved >= SANE_TIME / 2 && saved <= now + 366 * 86400ull;
}

// Loads the clock from the .rtc file, armGBA's, mGBA's or gambatte-libretro's, and advances it by the
// time the game was off, when the host clock can tell, except in Game Time.
void readRtcFile() {
	s->rtcPending = false;
	unsigned long long const now = std::time(0);
	const unsigned char* const file = s->rtcFile;
	unsigned long long saved = get64(file + RTC_SAVED);
	if (s->gb->hasHuC3()) {
		unsigned char state[HUC3_STATE];
		if (!std::memcmp(file + RTC_MAGIC, "aGBh", 4)) {
			std::memcpy(state, file + RTC_REGS, sizeof(state));
		} else {
			// gambatte-libretro's: the minutes and days since the clock's start.
			unsigned long long const base = get64(file + RTC_BASE);
			if (!base || base < SANE_TIME / 2 || now < SANE_TIME || base > now) {
				return;
			}
			s->gb->getHuC3Regs(state);
			std::memset(state, 0, 4);
			putNibbles(state + HUC3_MINUTES, 0);
			putNibbles(state + HUC3_DAYS, 0);
			saved = base;
		}
		if (!gameTime && saved >= SANE_TIME / 2 && now >= SANE_TIME && now >= saved) {
			runHuC3(state, now - saved);
		}
		s->gb->setHuC3Regs(state);
		return;
	}
	gb_ulong r[11] = { 0 };
	bool const ours = !std::memcmp(file + RTC_MAGIC, "aGBm", 4), older = !std::memcmp(file + RTC_MAGIC, "aGBc", 4);
	s->clockMoved = s->clockShort = 0;
	if (older) {
		const unsigned char* const regs = file + RTC_REGS;
		for (int i = 0; i < 5; ++i) {
			r[i] = regs[i];
			r[i + RTC_LATCHED] = regs[9 + i];
		}
		r[RTC_C] = get32(regs + 5);
		s->clockMoved = (long long) get64(file + OLD_OFFSET);
	} else if (ours || mgbaFile(file, now)) {
		static const int fields[5] = { RTC_S, RTC_M, RTC_H, RTC_DL, RTC_DH };
		for (int i = 0; i < 5; ++i) {
			r[fields[i]] = get32(file + MGBA_REGS + i * 4);
			r[fields[i] + RTC_LATCHED] = ours ? file[RTC_LATCH + fields[i]] : r[fields[i]];
		}
		saved = get64(file + MGBA_SAVED);
		if (ours) {
			r[RTC_C] = get32(file + RTC_CYCLES);
			s->clockMoved = (long long) get64(file + RTC_MOVED);
			s->clockShort = (long long) get64(file + RTC_SHORT);
		}
	} else {
		// gambatte-libretro's: the clock shows the time since its start, unless that start is in the
		// future.
		unsigned long long const base = get64(file + RTC_BASE);
		if (base && base >= SANE_TIME / 2 && now >= SANE_TIME && base <= now) {
			runClock(r, now - base);
			for (int i = 0; i < 5; ++i) {
				r[i + RTC_LATCHED] = r[i];
			}
		}
		saved = 0;
	}
	r[RTC_DH] &= 0xC1;
	r[RTC_DH + RTC_LATCHED] &= 0xC1;
	for (int i = RTC_H; i <= RTC_S; ++i) {
		r[i] &= i == RTC_H ? 0x1F : 0x3F;
		r[i + RTC_LATCHED] &= i == RTC_H ? 0x1F : 0x3F;
	}
	r[RTC_C] %= RTC_SECOND;
	if (!gameTime && saved >= SANE_TIME && now >= saved) {
		runClock(r, now - saved);
	}
	moveBy(*s, r, clockOffset);
	s->gb->setRtcRegs(r);
}

void writeRtcFile() {
	unsigned char* const file = s->rtcFile;
	unsigned long long const now = std::time(0);
	put64(file + RTC_SAVED, now);
	if (s->gb->hasHuC3()) {
		std::memcpy(file + RTC_MAGIC, "aGBh", 4);
		unsigned char* const state = file + RTC_REGS;
		s->gb->getHuC3Regs(state);
		unsigned long long const cycles = (unsigned long long) state[0] << 24 | state[1] << 16 | state[2] << 8 | state[3];
		unsigned long long const shown = getNibbles(state + HUC3_DAYS) * 86400ull + getNibbles(state + HUC3_MINUTES) * 60 + cycles / RTC_SECOND;
		put64(file + RTC_BASE, now - shown);
		return;
	}
	gb_ulong r[11];
	s->gb->getRtcRegs(r);
	unsigned long long const shown = (((((r[RTC_DH] & 1) << 8 | r[RTC_DL]) * 24 + r[RTC_H]) * 60
	                                  + r[RTC_M]) * 60 + r[RTC_S]);
	put64(file + RTC_BASE, now - shown);
	std::memcpy(file + RTC_MAGIC, "aGBm", 4);
	static const int fields[5] = { RTC_S, RTC_M, RTC_H, RTC_DL, RTC_DH };
	for (int i = 0; i < 5; ++i) {
		put32(file + MGBA_REGS + i * 4, r[fields[i]]);
		file[RTC_LATCH + fields[i]] = r[fields[i] + RTC_LATCHED];
	}
	put64(file + MGBA_SAVED, now);
	put32(file + RTC_CYCLES, r[RTC_C]);
	put64(file + RTC_MOVED, s->clockMoved);
	put64(file + RTC_SHORT, s->clockShort);
}

unsigned input(void* console) {
	return static_cast<Console*>(console)->pressed;
}

// Reads the first 2304 bytes of a boot ROM file (the size of the largest boot ROM), or nothing if
// there is no file.
std::vector<unsigned char> readBoot(std::string const& path) {
	std::vector<unsigned char> bytes(2305);
	FILE* const f = std::fopen(path.c_str(), "rb");
	if (!f) {
		return std::vector<unsigned char>();
	}
	bytes.resize(std::fread(&bytes[0], 1, bytes.size(), f));
	std::fclose(f);
	return bytes;
}

// Whether a boot ROM is Nintendo's: each of Nintendo's holds the logo it checks cartridges against,
// while SameBoy's draw it from the cartridge.
bool nintendoBoot(std::vector<unsigned char> const& bytes) {
	static const unsigned char logo[48] = {
		0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B, 0x03, 0x73, 0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D,
		0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E, 0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99,
		0xBB, 0xBB, 0x67, 0x63, 0x6E, 0x0E, 0xEC, 0xCC, 0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E,
	};
	return std::search(bytes.begin(), bytes.end(), logo, logo + sizeof(logo)) != bytes.end();
}

// Finds the boot ROM for the model in the system directory, or returns none. It looks for
// Nintendo's boot ROM under every name that ROCKNIX's BIOS check, Gambatte, gbdev's archive,
// SameBoy and Gearboy use for it, then for a SameBoy boot ROM under the same names.
//
// For a Game Boy Advance, the Advance's own comes first; a Game Boy Color boot ROM is used only if
// it is Nintendo's: Gambatte can convert that one, but not SameBoy's, and the conversion's timing
// is not quite the Advance's.
std::string bootRom(const char* systemDirectory, LibretroGbModel model) {
	static const char* const names[4][5] = {
		{ "gb_bios.bin", "dmg_boot.bin" },
		{ "gbc_bios.bin", "cgb_boot.bin" },
		// The later Game Boy Advances', the first ones', the name SameBoy also uses, then the Game Boy
		// Color's.
		{ "cgb_agb_boot.bin", "cgb_agb0_boot.bin", "agb_boot.bin", "gbc_bios.bin", "cgb_boot.bin" },
		// The Game Boy's boot ROM would leave the wrong value in A for a Pocket, so without the
		// Pocket's there is none.
		{ "mgb_boot.bin" },
	};
	std::string other;
	for (int i = 0; systemDirectory && i < 5 && names[model - 1][i]; ++i) {
		std::string const path = std::string(systemDirectory) + "/" + names[model - 1][i];
		std::vector<unsigned char> const bytes = readBoot(path);
		if (bytes.empty()) {
			continue;
		}
		bool const converted = model == LIBRETRO_GB_AGB && (i == 3 || i == 4);
		if (converted ? bytes.size() == 2304 && gambatte::crc32(0, &bytes[0], bytes.size()) == 0x41884E46
		              : nintendoBoot(bytes)) {
			return path;
		}
		if (!converted && other.empty()) {
			other = path;
		}
	}
	return other;
}

// Returns armGBA's boot ROM for the model (gbboot/).
std::vector<unsigned char> armgbaBoot(LibretroGbModel model) {
	switch (model) {
	case LIBRETRO_GB_CGB:
		return std::vector<unsigned char>(armgba_cgb_boot, armgba_cgb_boot + sizeof(armgba_cgb_boot));
	case LIBRETRO_GB_AGB:
		return std::vector<unsigned char>(armgba_agb_boot, armgba_agb_boot + sizeof(armgba_agb_boot));
	case LIBRETRO_GB_MGB:
		return std::vector<unsigned char>(armgba_mgb_boot, armgba_mgb_boot + sizeof(armgba_mgb_boot));
	default:
		return std::vector<unsigned char>(armgba_dmg_boot, armgba_dmg_boot + sizeof(armgba_dmg_boot));
	}
}

// How long the Game Boy Advance takes before handing over to the Game Boy Color hardware inside it.
size_t stall() {
	return (s->loadFlags & gambatte::GB::GBA_FLAG) && !(s->loadFlags & gambatte::GB::NO_BIOS) ? 485808 : 0;
}

// The original Game Boy screen chosen: 0 plain grays, 1 the first model's, 2 the Pocket's, 3 the
// Light's with its backlight on.
int screen;
bool correction;
// On the Game Boy Advance model, the Color's panel shown instead of the Advance's.
bool colorPanel;
// The contrast wheel of the first Game Boy, the Pocket and the Light, -5 to 5.
int contrast;
// The four shades shown now, the contrast wheel's included.
unsigned long shown[4];
// What the color table was last built for: 0 uncorrected, 1 a Game Boy Color's screen, 2 a Game Boy
// Advance's.
int built = -1;

// Builds the Game Boy Color's colors as its screen shows them, the way SameBoy does (Lior Halphon,
// MIT license; its "modern balanced" correction). Each channel goes through a response curve, then
// some blue is mixed into green. The Game Boy Advance's screen has its own curves and mixes less.
void correctedColors(bool advance) {
	static const unsigned char cgbCurve[32] = { 0, 6, 12, 20, 28, 36, 45, 56, 66, 76, 88, 100, 113, 125, 137, 149,
	                                            161, 172, 182, 192, 202, 210, 218, 225, 232, 238, 243, 247, 250, 252, 254, 255 };
	static const unsigned char agbCurve[32] = { 0, 3, 8, 14, 20, 26, 33, 40, 47, 54, 62, 70, 78, 86, 94, 103,
	                                            112, 120, 129, 138, 147, 157, 166, 176, 185, 195, 205, 215, 225, 235, 245, 255 };
	const unsigned char* const curve = advance ? agbCurve : cgbCurve;
	double linear[32];
	for (int i = 0; i < 32; ++i) {
		linear[i] = std::pow(curve[i] / 255.0, 1.6);
	}
	double const greenShare = advance ? 5 / 6.0 : 3 / 4.0;
	for (unsigned i = 0; i < 32768; ++i) {
		unsigned const r = curve[i & 0x1F];
		unsigned const g = i >> 5 & 0x1F;
		unsigned const b = i >> 10 & 0x1F;
		unsigned const mixed = std::lround(std::pow(linear[g] * greenShare + linear[b] * (1 - greenShare), 1 / 1.6) * 255);
		cgbColors[i] = 0xFF000000 | r << 16 | mixed << 8 | curve[b];
	}
}

// Installs the colors: the chosen screen's four shades for original Game Boy games, and the table
// that turns Game Boy Color 15-bit colors into the frontend's, corrected or not.
// Each screen: its color while the game has it off, its four shades, and the color its pixels go
// toward driven harder than the darkest shade. The plain grays are gambatte-libretro's. The first
// model's and the Pocket's are BGB's (bgb.bircd.org/reality): real ones photographed under
// color-accurate light and corrected with a ColorChecker. A first Game Boy's pixels go toward dark
// blue at the most contrast, as in BGB's contrast wheel; the Pocket's only darker. The Light is the
// Pocket's screen lit from behind, in the backlight's cyan (hue 180; photographed Lights run from
// about 157 to 191). A Light photographed with its light off and on, the contrast wheel unchanged,
// shows each shade's share of the lightest when lit as about the unlit share to the power 0.85, so
// each color here is the Pocket's share of its lightest shade to that power. The lightest shade
// passes the square root of the Pocket's share of the screen switched off: light crosses the screen
// once rather than twice.
struct Screen {
	unsigned long off;
	unsigned long shades[4];
	unsigned long full;
};
const Screen screens[4] = {
	{ 0xFFFFFF, { 0xFFFFFF, 0xA5A5A5, 0x525252, 0x000000 }, 0x000000 },
	{ 0x948A04, { 0x759833, 0x588F51, 0x3B7560, 0x2E615A }, 0x00005E },
	{ 0xC2CFA8, { 0xADBF92, 0x96A67C, 0x727E64, 0x5A635C }, 0x2C322E },
	{ 0x60F1F1, { 0x59DFDF, 0x4EC6C6, 0x3C9E9E, 0x308383 }, 0x194B4B },
};

void setColors() {
	Screen const& now = screens[screen];
	s->gb->setDmgPaletteColor(0, 4, now.off);
	// The wheel sets the drive voltages of the whole STN screen together (DMG-LCD-06: its 30k pot sets
	// the IR3E02's bias levels), from about -11 V to -14 V end to end, so 12% either side of the
	// middle, each step a tenth of the turn. An STN pixel's darkness is an S-curve of its voltage, as
	// steep as BGB's contrast wheel shows: all the way down the screen is blank, and all the way up
	// even the lightest shade is nearly as dark as it goes, 10% to 90% dark over about 7% of the
	// voltage. A shade's darkness places it on its
	// screen's own path, from the screen switched off through the four shades to its darkest, so
	// its color changes along the way as a real screen's does: the lightest a little past the screen
	// switched off, the others by their brightness, the darkest nine tenths of the way.
	auto const linear = [](unsigned long color, int bits) {
		double const c = (color >> bits & 0xFF) / 255.0;
		return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
	};
	auto const luminance = [&](unsigned long color) {
		return 0.2126 * linear(color, 16) + 0.7152 * linear(color, 8) + 0.0722 * linear(color, 0);
	};
	// Where a shade sits on the wheel's S-curve at the middle of its turn (before), from its darkness
	// in linear light, and where the turn moves it (after): 0 the screen switched off, 1 its darkest.
	auto const wheel = [&](Screen const& on, int shade, double places[6]) {
		double const lightest = 0.03, darkest = 0.95, width = 0.12 / (std::log((1 - lightest) / lightest) * 2.12);
		double const top = luminance(on.shades[0]), bottom = luminance(on.shades[3]);
		places[0] = 0, places[1] = lightest, places[4] = darkest, places[5] = 1;
		for (int k = 2; k <= 3; ++k) {
			places[k] = lightest + (darkest - lightest) * (top - luminance(on.shades[k - 1])) / (top - bottom);
		}
		double const before = places[shade + 1];
		double const voltage = (1 + width * std::log(before / (1 - before))) * (1 + 0.12 * contrast / 5);
		return 1 / (1 + std::exp(-(voltage - 1) / width));
	};
	auto const contrasted = [&](Screen const& on, int shade) {
		double places[6];
		double const after = wheel(on, shade, places);
		unsigned long const path[6] = { on.off, on.shades[0], on.shades[1], on.shades[2], on.shades[3], on.full };
		int k = 0;
		while (k < 4 && after > places[k + 1]) {
			++k;
		}
		double const t = std::min(std::max((after - places[k]) / (places[k + 1] - places[k]), 0.0), 1.0);
		unsigned long result = 0;
		for (int bits = 0; bits < 24; bits += 8) {
			double const l = linear(path[k], bits) + (linear(path[k + 1], bits) - linear(path[k], bits)) * t;
			double const c = l <= 0.0031308 ? l * 12.92 : 1.055 * std::pow(l, 1 / 2.4) - 0.055;
			result |= (unsigned long) std::lround(c * 255) << bits;
		}
		return result;
	};
	// Grayscale is no real screen, and already white to black, so each shade moves as the Pocket's same
	// shade does, by the share of the way that one goes to the Pocket's darkest when turned up, or to
	// it switched off when turned down: to black, or to white.
	auto const followed = [&](int shade) {
		// The shares are of perceived brightness, so the faint picture a real screen keeps at either
		// end of the wheel stays too.
		auto const perceived = [&](unsigned long color) {
			double const l = luminance(color);
			return l <= 0.0031308 ? l * 12.92 : 1.055 * std::pow(l, 1 / 2.4) - 0.055;
		};
		Screen const& real = screens[2];
		double const from = perceived(real.shades[shade]), to = perceived(contrasted(real, shade));
		double const was = perceived(now.shades[shade]);
		double const c = to <= from ? was * (1 - (from - to) / (from - perceived(real.full)))
		                            : was + (1 - was) * (to - from) / (perceived(real.off) - from);
		unsigned long const gray = (unsigned long) std::lround(std::min(std::max(c, 0.0), 1.0) * 255);
		return gray << 16 | gray << 8 | gray;
	};
	// A Game Boy game started through a Game Boy Color's boot ROM keeps the colors the boot ROM
	// gave it (a state stores them too), so they aren't replaced here.
	if (!s->gb->isCgb() || (s->loadFlags & gambatte::GB::NO_BIOS)) {
		for (int i = 0; i < 4; ++i) {
			shown[i] = !contrast ? now.shades[i] : screen == 0 ? followed(i) : contrasted(now, i);
		}
		for (int i = 0; i < 12; ++i) {
			s->gb->setDmgPaletteColor(i / 4, i % 4, shown[i % 4]);
		}
	}
	// Game Boy screen shades are shown as they are; color correction is only for a Game Boy Color's
	// screen.
	int const table = correction && s->gb->isCgb() ? (s->loadFlags & gambatte::GB::GBA_FLAG && !colorPanel ? 2 : 1) : 0;
	if (table != built) {
		built = table;
		if (table) {
			correctedColors(table == 2);
		} else {
			for (unsigned i = 0; i < 32768; ++i) {
				unsigned const r = i & 0x1F;
				unsigned const g = i >> 5 & 0x1F;
				unsigned const b = i >> 10 & 0x1F;
				cgbColors[i] = 0xFF000000 | ((r * 255 + 15) / 31) << 16 | ((g * 255 + 15) / 31) << 8 | ((b * 255 + 15) / 31);
			}
		}
	}
	s->gb->setCgbPalette(cgbColors);
}

void restartAs(LibretroGbModel model);

// Reads what the Action Replay keeps out of the console, if one is in, so a new cartridge starts
// with it.
void keepReplayFlash() {
	std::size_t size = 0;
	const unsigned char* const kept = s->gb ? s->gb->actionReplayFlash(&size) : 0;
	if (kept) {
		replayKept.assign(kept, kept + size);
	}
}

// Sachen's multicarts show the boot ROM their own logo through a lock that isn't emulated, and keep
// their header where their scrambled address lines put it.
bool isSachen(const unsigned char* bytes, size_t size) {
	return size >= 0x200 && ((bytes[0x104] == 0xCE && bytes[0x144] == 0xED && bytes[0x114] == 0x66)
	                         || (bytes[0x184] == 0xCE && bytes[0x1C4] == 0xED && bytes[0x194] == 0x66));
}

// The model a game's header asks for.
LibretroGbModel headerModel(const unsigned char* header, size_t size, bool sachen) {
	if (sachen) {
		return (header[0x151] & 0xBF) == 0x80 ? LIBRETRO_GB_CGB : LIBRETRO_GB_DMG;
	}
	// The header's CGB flag: 0x80 for color games that also run on a Game Boy, 0xC0 for color-only
	// games. Any other value is part of an older game's title. So is 0x80 in an unlicensed game
	// whose title runs right up to it and whose header names no known cartridge type.
	unsigned const flag = size > 0x14B ? header[0x143] : 0;
	unsigned const type = size > 0x14B ? header[0x147] : 0;
	bool const knownType = type <= 0x22 || type >= 0xFC;
	bool const title = !knownType && header[0x14B] != 0x33 && header[0x142] > 0x20 && header[0x142] < 0x7F;
	return flag == 0xC0 || (flag == 0x80 && !title) ? LIBRETRO_GB_CGB : LIBRETRO_GB_DMG;
}

// Starts the game as the chosen model, or as its header asks for, reusing the existing machine if
// there is one.
bool start(LibretroGbModel model, char* error, size_t errorSize) {
	s->chosen = model;
	const unsigned char* const bytes = &s->game[0];
	size_t const size = s->game.size();
	const char* const systemDirectory = hasSystemPath ? systemPath.c_str() : 0;
	bool const megaDuck = megaDuckKind >= 0;
	bool const sachen = !megaDuck && isSachen(bytes, size);
	if (megaDuck) {
		model = LIBRETRO_GB_DMG;
	} else if (model == LIBRETRO_GB_AUTO) {
		model = headerModel(bytes, size, sachen);
	}
	// Starts through Nintendo's boot ROM when the system directory has it, else armGBA's; with Boot
	// ROM off, in the state a boot ROM would have left the machine. A Mega Duck has none.
	// An Action Replay with nothing on top shows the boot ROM no logo, so it starts without one
	bool const bootable = !sachen && !megaDuck && (replayProgram.empty() || replayGame);
	std::string const boot = bootable && bootWanted == LIBRETRO_GB_BOOT_NINTENDO ? bootRom(systemDirectory, model) : std::string();
	std::vector<unsigned char> const builtIn = bootable && bootWanted != LIBRETRO_GB_BOOT_OFF && boot.empty() ? armgbaBoot(model)
	                                                                                                       : std::vector<unsigned char>();
	s->bootUsed = bootWanted;
	std::vector<unsigned char> const bootBytes = boot.empty() ? std::vector<unsigned char>() : readBoot(boot);
	s->bootCrc = bootBytes.empty() ? 0 : gambatte::crc32(0, &bootBytes[0], bootBytes.size());
	s->loadFlags = gambatte::GB::READONLY_SAV;
	if (model == LIBRETRO_GB_MGB) {
		s->loadFlags |= gambatte::GB::MGB_FLAG;
	} else if (model != LIBRETRO_GB_DMG) {
		s->loadFlags |= gambatte::GB::CGB_MODE;
	}
	if (model == LIBRETRO_GB_AGB) {
		s->loadFlags |= gambatte::GB::GBA_FLAG;
	}
	bool const laptop = megaDuck && laptopWanted && players == 1;
	if (megaDuck) {
		s->loadFlags |= gambatte::GB::MEGA_DUCK | megaDuckKind * gambatte::GB::MEGA_DUCK_MAPPER;
		if (laptop) {
			s->loadFlags |= gambatte::GB::MEGA_DUCK_LAPTOP;
		}
	} else if (boot.empty() && builtIn.empty()) {
		s->loadFlags |= gambatte::GB::NO_BIOS;
	}
	// What the Action Replay keeps goes into the new cartridge as the last one left it
	keepReplayFlash();
	if (!s->gb) {
		s->gb = new gambatte::GB;
		s->gb->setCpuQuarters(cpuQuarters);
		s->gb->setRtcSpeed(clockPercent);
	}
	s->gb->setLinkCallback(players > 1 ? 0 : linkClocked);
	duckLaptopConnect(laptop, laptopPrinter, laptopLanguage, laptopControl, laptopIn, laptopSlot);
	s->linked = players == 1 && (printerOn || laptop);
	if (s->gb->load(reinterpret_cast<const char*>(bytes), size, s->loadFlags) != gambatte::LOADRES_OK) {
		std::snprintf(error, errorSize, "armGBA can't run this Game Boy game");
		return false;
	}
	applyShownAndHeard(s->gb);
	laptopSlotIn = false;
	if (!replayProgram.empty()) {
		if (!s->gb->attachCheatCart(reinterpret_cast<const char*>(&replayProgram[0]), replayProgram.size(), replayGame)) {
			std::snprintf(error, errorSize, "armGBA can't run this cheat cartridge");
			return false;
		}
		std::size_t size = 0;
		unsigned char* const kept = s->gb->actionReplayFlash(&size);
		if (kept && size == replayKept.size()) {
			std::memcpy(kept, &replayKept[0], size);
		}
		s->gb->setGameGenieCodes(genieDigits, genieCounts);
	}
	s->gb->linkStatus(players > 1 || s->linked ? LINK_ON : LINK_OFF);
	if (!boot.empty()) {
		if (s->gb->loadBios(boot) < 0) {
			std::snprintf(error, errorSize, "armGBA can't read %s", boot.c_str());
			return false;
		}
		s->gb->reset(stall());
	} else if (!builtIn.empty()) {
		s->gb->loadBios(reinterpret_cast<const char*>(&builtIn[0]), builtIn.size());
		s->gb->reset(stall());
	}
	s->gb->setInputGetter(input, s);
	s->gb->setGameGenie(s->gameGenie);
	s->gb->setGameShark(s->gameShark);
	setColors();
	s->sumLeft = s->sumRight = 0;
	s->summed = 0;
	s->capacitor[0] = s->capacitor[1] = 0;
	s->heardApart = false;
	setCapacitor(*s);
	s->pressed = 0;
	std::memset(s->pixels, 0, sizeof(s->pixels));
	size_t const needed = std::max(s->gb->saveState(0, 0, static_cast<char*>(0)), s->gb->exactStateSize()) + sizeof(gambatte::uint_least32_t);
	if (!s->stateSize) {
		s->stateSize = needed + STATE_SLACK;
	} else if (needed > s->stateSize) {
		s->stateSize = needed;
	}
	return true;
}

// The eight bits of a transfer this Game Boy clocked are out: the other one's byte, or the
// printer's, comes back.
void linkClocked() {
	s->gb->linkStatus(LINK_ACK);
	int const out = s->gb->linkStatus(LINK_OUT);
	if (duckLaptopConnected()) {
		s->gb->linkStatus(duckLaptopClockedByConsole(static_cast<uint8_t>(out)));
		return;
	}
	bool const toPrinter = printerOn && !linkGbConnected();
	s->gb->linkStatus(toPrinter ? printerTransfer(&printer, out, s->gb->timeNow(), printed) : linkGbTransfer(out));
}

void updateLink() {
	bool const linked = linkGbConnected() || printerOn || duckLaptopConnected();
	if (s->linked != linked) {
		s->linked = linked;
		s->gb->linkStatus(s->linked ? LINK_ON : LINK_OFF);
	}
}

}

extern "C" {

bool libretroGbLoad(const void* rom, size_t size, const char* systemDirectory, enum LibretroGbModel model, bool colorCorrection,
                    unsigned audioRate, char* error, size_t errorSize) {
	libretroGbUnload();
	s->game.assign(static_cast<const unsigned char*>(rom), static_cast<const unsigned char*>(rom) + size);
	systemPath = systemDirectory ? systemDirectory : "";
	hasSystemPath = systemDirectory != 0;
	correction = colorCorrection;
	ratio = NATIVE_RATE / audioRate;
	s->stateSize = 0;
	s->wanted = model;
	printerReset(&printer);
	if (!start(model, error, errorSize)) {
		libretroGbUnload();
		return false;
	}
	std::memset(s->rtcFile, 0, sizeof(s->rtcFile));
	s->rtcPending = rtcSize();
	return true;
}

void libretroGbUnload(void) {
	delete s->gb;
	s->gb = 0;
	s->gameGenie.clear();
	s->gameShark.clear();
}

void libretroGbReset(enum LibretroGbModel model) {
	s->wanted = model;
	if (model == s->chosen && s->bootUsed == bootWanted) {
		s->gb->reset(stall());
		return;
	}
	restartAs(model);
}

// Restarts the selected console as its power switch would, keeping it on the link.
void libretroGbRestart(void) {
	libretroGbReset(s->wanted);
	if (players > 1) {
		s->gb->setLinkCallback(0);
		s->gb->linkStatus(LINK_ON);
	}
	s->linked = false;
}

}

namespace {

// Restarts the game in the same machine as another model, or with the boot ROM switched on or off.
// Its cartridge RAM stays as it was and where it was, since the frontend may hold a pointer to it,
// and so does its clock.
void restartAs(LibretroGbModel model) {
	size_t size;
	const unsigned char* const ram = static_cast<const unsigned char*>(libretroGbSaveRam(&size));
	std::vector<unsigned char> const kept(ram, ram + size);
	gb_ulong clock[11];
	unsigned char huc3[HUC3_STATE];
	if (s->gb->hasMbc3Rtc()) {
		s->gb->getRtcRegs(clock);
	} else if (s->gb->hasHuC3()) {
		s->gb->getHuC3Regs(huc3);
	}
	LibretroGbModel const before = s->chosen;
	char error[64];
	if (!start(model, error, sizeof(error))) {
		start(before, error, sizeof(error));
	}
	void* const now = libretroGbSaveRam(&size);
	if (now && size == kept.size() && size) {
		std::memcpy(now, &kept[0], size);
	}
	if (s->gb->hasMbc3Rtc() && !s->rtcPending) {
		s->gb->setRtcRegs(clock);
	} else if (s->gb->hasHuC3() && !s->rtcPending) {
		s->gb->setHuC3Regs(huc3);
	}
}

}

extern "C" {

void libretroGbSetColorCorrection(bool on) {
	correction = on;
	setColors();
}

void libretroGbSetColorPanel(bool color) {
	colorPanel = color;
	if (s->gb) {
		setColors();
	}
}

void libretroGbSetBootRom(enum LibretroGbBoot boot) {
	bootWanted = boot;
}

void libretroGbSetAudioFilter(enum LibretroGbFilter filter) {
	audioFilter = filter;
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		setCapacitor(consoles[i]);
	}
}

void libretroGbSetAdvanceOutput(enum LibretroAdvanceOutput output) {
	advanceOutput = output;
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		setCapacitor(consoles[i]);
	}
}

void libretroGbSetMegaDuck(int kind) {
	megaDuckKind = kind;
}

void libretroGbSetMegaDuckLaptop(bool on, int printer, int language) {
	laptopWanted = on;
	laptopPrinter = printer;
	laptopLanguage = language ? DUCK_LAPTOP_GERMAN : DUCK_LAPTOP_SPANISH;
}

void libretroGbSetGameGenieCodes(const uint8_t digits[27], const uint8_t counts[3]) {
	std::memcpy(genieDigits, digits, sizeof(genieDigits));
	std::memcpy(genieCounts, counts, sizeof(genieCounts));
	if (s->gb) {
		s->gb->setGameGenieCodes(genieDigits, genieCounts);
	}
}

bool libretroGbSwapGame(const void* rom, size_t size, bool game, char* error, size_t errorSize) {
	if (!s->gb || replayProgram.empty()) {
		return false;
	}
	keepReplayFlash();
	std::vector<unsigned char> const before(s->game);
	bool const hadGame = replayGame;
	delete s->gb;
	s->gb = 0;
	s->game.assign(static_cast<const unsigned char*>(rom), static_cast<const unsigned char*>(rom) + size);
	replayGame = game;
	bool ok = start(s->wanted, error, errorSize);
	if (!ok) {
		// The game before goes back in
		delete s->gb;
		s->gb = 0;
		s->game = before;
		replayGame = hadGame;
		char ignored[256];
		start(s->wanted, ignored, sizeof(ignored));
	}
	std::memset(s->rtcFile, 0, sizeof(s->rtcFile));
	s->rtcPending = rtcSize();
	return ok;
}

void libretroGbSetActionReplay(const void* program, size_t size, bool game) {
	const unsigned char* const bytes = static_cast<const unsigned char*>(program);
	replayProgram.assign(bytes, bytes + (program ? size : 0));
	replayGame = game;
	replayKept.clear();
}

void libretroGbActionReplayControls(bool button, bool switchOn) {
	if (s->gb) {
		s->gb->setActionReplayControls(button, switchOn);
	}
}

uint8_t* libretroGbActionReplayFlash(size_t* size) {
	std::size_t kept = 0;
	unsigned char* const bytes = s->gb ? s->gb->actionReplayFlash(&kept) : 0;
	*size = kept;
	return bytes;
}

bool libretroGbActionReplayFlashChanged(void) {
	return s->gb && s->gb->actionReplayFlashChanged();
}

void libretroGbSetMegaDuckSlot(const void* rom, size_t size, int kind) {
	const unsigned char* const bytes = static_cast<const unsigned char*>(rom);
	laptopCartridge.assign(bytes, bytes + (rom ? size : 0));
	laptopCartridgeKind = kind;
}

void libretroGbKeyboard(bool down, unsigned keycode, uint32_t character, uint16_t modifiers) {
	duckLaptopKeyboard(down, keycode, character, modifiers);
}

const int16_t* libretroGbMixSpeech(const int16_t* samples, size_t frames) {
	return duckLaptopConnected() ? duckLaptopMixSpeech(samples, frames) : samples;
}

void libretroGbSetScreen(int which) {
	screen = which;
	if (s->gb) {
		setColors();
	}
}

void libretroGbSetLayersShown(unsigned mask) {
	layersShown = mask & 7;
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		if (consoles[i].gb) {
			applyShownAndHeard(consoles[i].gb);
		}
	}
}

void libretroGbSetMutedChannels(unsigned mask) {
	channelsMuted = mask & 0xF;
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		if (consoles[i].gb) {
			applyShownAndHeard(consoles[i].gb);
		}
	}
}

void libretroGbSetContrast(int steps) {
	contrast = steps;
	if (s->gb) {
		setColors();
	}
}

uint32_t libretroGbShade(int shade) {
	return (uint32_t) shown[shade];
}

uint32_t libretroGbDarkestColor(void) {
	return screens[screen].shades[3];
}

uint32_t libretroGbOffColor(void) {
	return screens[screen].off;
}

bool libretroGbOnAdvance(void) {
	return s->loadFlags & gambatte::GB::GBA_FLAG;
}

uint32_t libretroGbBootCrc(void) {
	uint32_t crc = 0;
	for (int i = 0; i < players; ++i) {
		crc = (crc << 1 | crc >> 31) ^ consoles[i].bootCrc;
	}
	return crc;
}

}

namespace {

// The console's sound leaves through a capacitor that keeps DC out of it, connected only while a
// channel's DAC is on (Pan Docs, Audio Details). Where the DACs go from all off to any on, or back,
// comes from Gambatte, sample by sample.
inline int16_t throughCapacitor(int32_t* capacitor, int side, long in, bool connected) {
	if (!s->charge) {
		return static_cast<int16_t>(in);
	}
	return connected ? outputCapacitor(&capacitor[side], static_cast<int32_t>(in), s->charge) : 0;
}

// A frontend sample out through the capacitor: the console's, or what's heard
inline void throughOutput(int32_t* capacitor, long left, long right, bool dacs, int16_t* out) {
	if (s->wiring == Wiring::SHARED_RETURN) {
		outputSharedReturn(&capacitor[0], static_cast<int32_t>(left), static_cast<int32_t>(right), s->charge, out);
	} else {
		// A Game Boy Advance's capacitor is after its amp: connected whatever the DACs do
		bool const connected = dacs || s->wiring == Wiring::ADVANCE;
		out[0] = throughCapacitor(capacitor, 0, left, connected);
		out[1] = throughCapacitor(capacitor, 1, right, connected);
	}
}

// mixDown with channels left out: what's heard is summed and sent beside the console's sound, which
// the sums and the charges states keep follow
void mixDownHeard(std::size_t count, gambatte::uint_least32_t const* const heard) {
	std::size_t const* edgeAt;
	bool const* edgeOn;
	std::size_t const edges = s->gb->soundDacEdges(&edgeAt, &edgeOn);
	bool dacs = s->gb->soundDacsAtStart();
	std::size_t edge = 0;
	if (!s->heardApart) {
		s->heardLeft = s->sumLeft;
		s->heardRight = s->sumRight;
		s->heardCapacitor[0] = s->capacitor[0];
		s->heardCapacitor[1] = s->capacitor[1];
		s->heardApart = true;
	}
	for (std::size_t i = 0; i < count;) {
		std::size_t const run = ratio - s->summed < count - i ? ratio - s->summed : count - i;
		int left = 0;
		int right = 0;
		for (std::size_t k = 0; k < run; ++k) {
			left += static_cast<int16_t>(s->native[i + k] & 0xFFFF);
			right += static_cast<int16_t>(s->native[i + k] >> 16);
		}
		s->sumLeft += left;
		s->sumRight += right;
		{
			int heardLeft = 0;
			int heardRight = 0;
			for (std::size_t k = 0; k < run; ++k) {
				heardLeft += static_cast<int16_t>(heard[i + k] & 0xFFFF);
				heardRight += static_cast<int16_t>(heard[i + k] >> 16);
			}
			s->heardLeft += heardLeft;
			s->heardRight += heardRight;
		}
		s->summed += run;
		i += run;
		// The DACs as the frontend sample ends
		while (edge < edges && edgeAt[edge] < i) {
			dacs = edgeOn[edge++];
		}
		if (s->summed == ratio) {
			if (s->sampleCount + 2 <= sizeof(s->samples) / sizeof(s->samples[0])) {
				int16_t* const out = &s->samples[s->sampleCount];
				throughOutput(s->capacitor, s->sumLeft / static_cast<long>(ratio), s->sumRight / static_cast<long>(ratio), dacs, out);
				throughOutput(s->heardCapacitor, s->heardLeft / static_cast<long>(ratio), s->heardRight / static_cast<long>(ratio), dacs, out);
				s->sampleCount += 2;
			}
			s->sumLeft = s->sumRight = 0;
			s->heardLeft = s->heardRight = 0;
			s->summed = 0;
		}
	}
}

// Averages a run's 2 MHz stereo samples down to the frontend's rate: each frontend sample is the
// mean of the native samples it covers, then goes through the capacitor.
void mixDown(std::size_t count) {
	// Channel Selection
	gambatte::uint_least32_t const* const heard = s->gb->heardSamples();
	if (heard) {
		mixDownHeard(count, heard);
		return;
	}
	s->heardApart = false;
	std::size_t const* edgeAt;
	bool const* edgeOn;
	std::size_t const edges = s->gb->soundDacEdges(&edgeAt, &edgeOn);
	bool dacs = s->gb->soundDacsAtStart();
	std::size_t edge = 0;
	for (std::size_t i = 0; i < count;) {
		std::size_t const run = ratio - s->summed < count - i ? ratio - s->summed : count - i;
		int left = 0;
		int right = 0;
		for (std::size_t k = 0; k < run; ++k) {
			left += static_cast<int16_t>(s->native[i + k] & 0xFFFF);
			right += static_cast<int16_t>(s->native[i + k] >> 16);
		}
		s->sumLeft += left;
		s->sumRight += right;
		s->summed += run;
		i += run;
		// The DACs as the frontend sample ends
		while (edge < edges && edgeAt[edge] < i) {
			dacs = edgeOn[edge++];
		}
		if (s->summed == ratio) {
			if (s->sampleCount + 2 <= sizeof(s->samples) / sizeof(s->samples[0])) {
				throughOutput(s->capacitor, s->sumLeft / static_cast<long>(ratio), s->sumRight / static_cast<long>(ratio), dacs, &s->samples[s->sampleCount]);
				s->sampleCount += 2;
			}
			s->sumLeft = s->sumRight = 0;
			s->summed = 0;
		}
	}
}

// Starts a frame on the selected console: its buttons, and its clock file once loaded.
void beginFrame(uint16_t keys) {
	s->pressed = keys & 0xFF;
	s->sampleCount = 0;
	if (s->rtcPending) {
		readRtcFile();
	}
}

void endFrame() {
	if (rtcSize()) {
		writeRtcFile();
	}
}

// The cable between the two consoles here. When one has clocked out its eight bits, it takes the
// other's byte. The other takes this one's only if its serial port is waiting for a clock or
// clocking too. Both then stop their own clocks, since the transfer is done.
void exchange(Console& clocked, Console& other) {
	clocked.gb->linkStatus(LINK_ACK);
	int const out = clocked.gb->linkStatus(LINK_OUT);
	clocked.gb->linkStatus(other.gb->linkStatus(LINK_OUT));
	clocked.gb->linkStatus(LINK_STOP);
	other.gb->linkStatus(out);
	other.gb->linkStatus(LINK_STOP);
}

// Sound samples until a Game Boy's transfer on its own clock ends: -1 for none, 0 when due.
long serialLeft(Console& c) {
	if (c.gb->linkStatus(LINK_CLOCKED)) {
		return 0;
	}
	int const cycles = c.gb->linkStatus(LINK_LEFT);
	return cycles < 0 ? -1 : cycles / 2;
}

// The fewest samples a transfer a Game Boy could start now takes: eight bits at 8192 Hz, or on a
// Game Boy Color with SC's fast bit, at 262144 Hz.
long shortestTransfer(Console& c) {
	return c.gb->isCgb() && c.gb->linkStatus(LINK_CONTROL) & 0x02 ? 64 : 2048;
}

// Where the consoles next meet: at the frame's end, at the next transfer's end on the cable or the
// adapter, and no later than the shortest transfer any console could start from now.
long nextMeeting(long const done[]) {
	long now = done[0];
	for (int i = 1; i < players; ++i) {
		now = std::min(now, done[i]);
	}
	long meet = FRAME_SAMPLES;
	if (adapterOn) {
		meet = std::min<long>(meet, std::max<long>(adapter.at, now));
	}
	for (int i = 0; i < players; ++i) {
		long const left = serialLeft(consoles[i]);
		if (left >= 0) {
			meet = std::min(meet, done[i] + left);
		}
		if (!adapterOn) {
			meet = std::min(meet, now + shortestTransfer(consoles[i]));
		}
	}
	return meet;
}

// With every console at the meeting point: the transfers ending there.
void meet(long now) {
	if (adapterOn) {
		// On the adapter, a Game Boy's own clock reaches nothing.
		for (int i = 0; i < players; ++i) {
			if (serialLeft(consoles[i]) == 0) {
				consoles[i].gb->linkStatus(LINK_ACK);
				consoles[i].gb->linkStatus(0xFF);
				consoles[i].gb->linkStatus(LINK_STOP);
			}
		}
		while (adapter.at <= now) {
			adapterTransfer();
		}
		return;
	}
	for (int i = 0; i < players; ++i) {
		if (serialLeft(consoles[i]) == 0) {
			exchange(consoles[i], consoles[1 - i]);
		}
	}
}

// With exact link timing: this Game Boy's transfer ends, and the other's byte comes back.
void clockedExactly() {
	s->gb->linkStatus(LINK_ACK);
	int const in = linkGbTransfer(s->gb->linkStatus(LINK_OUT));
	s->gb->linkStatus(in & 0xFF);
	s->gb->linkStatus(LINK_STOP);
}

// With exact link timing, a frame runs in parts. Each part ends at this Game Boy's own transfer
// end, or where the leading console has got to. Its own transfer is told to the other as soon as it
// starts. A long wait at a stop ends the frame early.
void runExactlyParts() {
	long done = 0;
	while (done < FRAME_SAMPLES) {
		long const own = serialLeft(*s);
		if (own > 0) {
			linkGbClocking(s->gb->linkStatus(LINK_OUT), own, shortestTransfer(*s));
		}
		linkGbPoll();
		if (own == 0) {
			clockedExactly();
			continue;
		}
		long const allowed = linkGbAllowance();
		if (allowed == 0) {
			if (!linkGbArrived()) {
				return;
			}
			continue;
		}
		long count = std::min<long>(FRAME_SAMPLES - done, EXACT_PART);
		count = std::min(count, allowed);
		count = own > 0 ? std::min(count, own) : count;
		std::size_t samples = count;
		if (s->gb->runFor(s->pixels, LIBRETRO_GB_WIDTH, s->native, samples) >= 0) {
			std::memcpy(s->shown, s->pixels, sizeof(s->shown));
		}
		mixDown(samples);
		done += samples;
		linkGbRan(samples);
	}
}

// The laptop's controller: the console's SC, and a byte it clocks in
unsigned laptopControl() {
	return static_cast<unsigned>(s->gb->linkStatus(LINK_CONTROL));
}

void laptopIn(uint8_t byte) {
	s->gb->linkStatus(byte);
}

bool laptopSlot() {
	bool const swapped = !laptopCartridge.empty()
	    && s->gb->swapMegaDuckCartridge(reinterpret_cast<const char*>(&laptopCartridge[0]), laptopCartridge.size(), laptopCartridgeKind);
	laptopSlotIn |= swapped;
	return swapped;
}

// Puts the slot's cartridge or the system ROM (the game loaded, padded to fit either) in place.
bool laptopSlotAs(bool in) {
	if (in == laptopSlotIn || laptopCartridge.empty()) {
		return in == laptopSlotIn;
	}
	if (in) {
		return laptopSlot();
	}
	bool const swapped = s->gb->swapMegaDuckCartridge(reinterpret_cast<const char*>(&s->game[0]), s->game.size(), megaDuckKind);
	laptopSlotIn = !swapped;
	return swapped;
}

// With a Mega Duck laptop, runs until a picture is drawn in parts that end where its controller does
// something next.
void runWithLaptop() {
	bool drawn = false;
	for (long done = 0; !drawn && done < 4L * FRAME_SAMPLES;) {
		std::size_t count = std::max<long>(1, std::min<long>((duckLaptopCyclesToNext() + 1) / 2, FRAME_SAMPLES));
		drawn = s->gb->runFor(s->pixels, LIBRETRO_GB_WIDTH, s->native, count) >= 0;
		mixDown(count);
		done += count;
		duckLaptopRan(2 * static_cast<long>(count));
	}
}

// Runs until a picture is drawn; linked, in sixteen parts with the cable checked between them.
void runUntilDrawn() {
	int const parts = s->linked ? 16 : 1;
	bool drawn = false;
	for (int runs = 0; !drawn && runs < 4 * parts; ++runs) {
		if (s->linked) {
			linkGbPoll();
		}
		std::size_t count = FRAME_SAMPLES / parts;
		drawn = s->gb->runFor(s->pixels, LIBRETRO_GB_WIDTH, s->native, count) >= 0;
		mixDown(count);
	}
}

void runExactly() {
	s->gb->setLinkCallback(0);
	runExactlyParts();
	s->gb->setLinkCallback(linkClocked);
}

}

extern "C" {

// Runs one frame. Gambatte runs by audio samples, so it is asked for a frame's worth at a time
// until it has drawn a frame.
//
// Turning the screen off can delay a frame by up to three frames' worth. A game that turns the
// screen off and on faster than a frame never finishes one, so after four frames' worth the
// frontend gets the last picture again.
const uint32_t* libretroGbRunFrame(uint16_t keys, const int16_t** out, size_t* frames) {
	beginFrame(keys);
	updateLink();
	bool const exactly = s->linked && linkExactOn() && !duckLaptopConnected();
	if (exactly) {
		runExactly();
	} else if (duckLaptopConnected()) {
		runWithLaptop();
	} else {
		runUntilDrawn();
	}
	endFrame();
	*out = s->samples;
	*frames = s->sampleCount / 2;
	return reinterpret_cast<const uint32_t*>(exactly ? s->shown : s->pixels);
}

void libretroGbRunPlayers(const uint16_t keys[], const uint32_t* pictures[], const int16_t* out[], size_t frames[]) {
	Console* const selected = s;
	long done[MAX_PLAYERS];
	for (int i = 0; i < players; ++i) {
		s = &consoles[i];
		beginFrame(keys[i]);
		done[i] = s->ahead;
	}
	// The consoles run in turns, the one furthest behind next, each up to where they next meet; there
	// they all stand on the same sample as a transfer ends.
	for (;;) {
		int i = 0;
		for (int j = 1; j < players; ++j) {
			i = done[j] < done[i] ? j : i;
		}
		if (done[i] >= FRAME_SAMPLES) {
			break;
		}
		long const meeting = nextMeeting(done);
		if (meeting <= done[i]) {
			meet(meeting);
			if (nextMeeting(done) <= done[i]) {
				// Nothing ended after all, as a game stopped its transfer: run on a little.
				s = &consoles[i];
				std::size_t count = 1;
				s->gb->runFor(s->pixels, LIBRETRO_GB_WIDTH, s->native, count);
				mixDown(count);
				done[i] += count;
			}
			continue;
		}
		s = &consoles[i];
		std::size_t count = meeting - done[i];
		if (s->gb->runFor(s->pixels, LIBRETRO_GB_WIDTH, s->native, count) >= 0) {
			std::memcpy(s->shown, s->pixels, sizeof(s->shown));
		}
		mixDown(count);
		done[i] += count;
	}
	dmg07EndFrame(&adapter, FRAME_SAMPLES);
	for (int i = 0; i < players; ++i) {
		s = &consoles[i];
		endFrame();
		s->ahead = done[i] - FRAME_SAMPLES;
		pictures[i] = reinterpret_cast<const uint32_t*>(s->shown);
		out[i] = s->samples;
		frames[i] = s->sampleCount / 2;
	}
	s = selected;
}

// The cartridge's battery RAM, which the frontend loads and saves as the save file.
void* libretroGbSaveRam(size_t* size) {
	unsigned char* data = 0;
	int length = 0;
	*size = 0;
	if (!s->gb || !s->gb->getMemoryArea(3, &data, &length)) {
		return 0;
	}
	*size = length;
	return data;
}

// The cartridge's clock, MBC3 or HuC3, which the frontend keeps as the .rtc file.
void* libretroGbRtc(size_t* size) {
	*size = rtcSize();
	return *size ? s->rtcFile : 0;
}

LibretroGbClock libretroGbClock(void) {
	return !s->gb ? LIBRETRO_GB_NO_CLOCK : s->gb->hasMbc3Rtc() ? LIBRETRO_GB_MBC3_CLOCK : s->gb->hasHuC3() ? LIBRETRO_GB_HUC3_CLOCK : LIBRETRO_GB_NO_CLOCK;
}

void libretroGbSetClock(bool timeOfGame, unsigned percent, long long offset) {
	gameTime = timeOfGame;
	clockPercent = gameTime ? percent : 100;
	clockOffset = offset;
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		Console& c = consoles[i];
		if (!c.gb) {
			continue;
		}
		c.gb->setRtcSpeed(clockPercent);
		if (c.gb->hasMbc3Rtc() && !c.rtcPending && c.clockMoved != offset) {
			gb_ulong r[11];
			c.gb->getRtcRegs(r);
			moveBy(c, r, offset);
			c.gb->setRtcRegs(r);
		}
	}
}

void libretroGbDeviceTime(bool on) {
	deviceClock = on;
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		if (consoles[i].gb && consoles[i].gb->deviceTime() != on) {
			consoles[i].gb->setDeviceTime(on);
		}
	}
}

bool libretroGbResetClock(void) {
	bool reset = false;
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		Console& c = consoles[i];
		if (c.gb && c.gb->hasMbc3Rtc()) {
			gb_ulong r[11] = { 0 };
			r[RTC_DH] = r[RTC_DH + RTC_LATCHED] = 0x40;
			c.gb->setRtcRegs(r);
			c.rtcPending = false;
			c.clockMoved = clockOffset;
			c.clockShort = 0;
			reset = true;
		}
	}
	return reset;
}

// Converts a stick position into the tilt sensor's reading.
void libretroGbSetTilt(int x, int y) {
	// The sensor reads lower to the right and towards the player; 0x70 is the cartridge tipped onto
	// its side.
	gambatte::mbc7TiltX = -x * 0x70 / 32768;
	gambatte::mbc7TiltY = -y * 0x70 / 32768;
}

// Flicks the console, for the one sensor reading the flick lasts.
void libretroGbFlick(void) {
	gambatte::mbc7Flick = true;
}

// Work RAM, for achievements and cheats.
void* libretroGbSystemRam(size_t* size) {
	unsigned char* data = 0;
	int length = 0;
	*size = 0;
	if (!s->gb || !s->gb->getMemoryArea(2, &data, &length)) {
		return 0;
	}
	*size = length;
	return data;
}

size_t libretroGbMemoryMap(struct retro_memory_descriptor* out) {
	unsigned char* vram;
	unsigned char* rom;
	unsigned char* wram;
	unsigned char* cartRam;
	unsigned char* oam;
	int vramLength, romLength, wramLength, cartRamLength, oamLength;
	if (!s->gb || !s->gb->getMemoryArea(0, &vram, &vramLength) || !s->gb->getMemoryArea(1, &rom, &romLength)
	    || !s->gb->getMemoryArea(2, &wram, &wramLength) || !s->gb->getMemoryArea(3, &cartRam, &cartRamLength)
	    || !s->gb->getMemoryArea(4, &oam, &oamLength)) {
		return 0;
	}
	size_t count = 0;
	// OAM, then the I/O registers and high RAM, are one block.
	struct retro_memory_descriptor const areas[] = {
		{ RETRO_MEMDESC_SYSTEM_RAM, wram, 0, 0xC000, 0, 0, 0x1000, 0 },
		{ RETRO_MEMDESC_SYSTEM_RAM, wram, 0x1000, 0xD000, 0, 0, 0x1000, 0 },
		{ RETRO_MEMDESC_SYSTEM_RAM, oam, 0x180, 0xFF80, 0, 0, 0x80, 0 },
		{ 0, vram, 0, 0x8000, 0, 0, 0x2000, 0 },
		{ 0, oam, 0, 0xFE00, 0xFFFFFFE0, 0, 0xA0, 0 },
		{ RETRO_MEMDESC_CONST, rom, 0, 0x0000, 0, 0, 0x4000, 0 },
		{ RETRO_MEMDESC_CONST, rom, 0x4000, 0x4000, 0, 0, 0x4000, 0 },
		{ 0, oam, 0x100, 0xFF00, 0, 0, 0x80, 0 },
	};
	for (std::size_t i = 0; i < sizeof(areas) / sizeof(areas[0]); ++i) {
		out[count++] = areas[i];
	}
	if (cartRamLength) {
		struct retro_memory_descriptor const area = { 0, cartRam, 0, 0xA000, ~(std::size_t) 0x1FFF, 0, (std::size_t) cartRamLength, 0 };
		out[count++] = area;
	}
	// A Game Boy Color's work RAM banks 2 to 7, at the addresses achievements expect.
	if (s->gb->isCgb()) {
		struct retro_memory_descriptor const area = { RETRO_MEMDESC_SYSTEM_RAM, wram, 0x2000, 0x10000, 0xFFFFA000, 0, 0x6000, 0 };
		out[count++] = area;
	}
	return count;
}

bool libretroGbIsColor(void) {
	return s->gb && s->gb->isCgb();
}

bool libretroGbBooting(void) {
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		if (consoles[i].gb && consoles[i].gb->inBootRom()) {
			return true;
		}
	}
	return false;
}

void libretroGbSetCpuClock(unsigned quarters, bool loaded) {
	cpuQuarters = quarters;
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		if (consoles[i].gb) {
			consoles[i].gb->setCpuQuarters(quarters, loaded);
		}
	}
}

bool libretroGbSetCheats(const char* const* codes, size_t count) {
	bool understood = true;
	s->gameGenie.clear();
	s->gameShark.clear();
	for (size_t i = 0; i < count; ++i) {
		std::string const all = codes[i] ? codes[i] : "";
		std::size_t pos = 0;
		while (pos < all.size()) {
			std::size_t const end = all.find_first_of("+ \t\r\n;", pos);
			std::string const code = all.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
			pos = end == std::string::npos ? all.size() : end + 1;
			if (code.empty()) {
				continue;
			}
			bool const genie = (code.size() == 7 && code[3] == '-') || (code.size() == 11 && code[3] == '-' && code[7] == '-');
			bool const shark = code.size() == 8 && code.find_first_not_of("0123456789ABCDEFabcdef") == std::string::npos;
			std::string& list = genie ? s->gameGenie : s->gameShark;
			if (!genie && !shark) {
				understood = false;
				continue;
			}
			if (!list.empty()) {
				list += ';';
			}
			list += code;
		}
	}
	if (s->gb) {
		s->gb->setGameGenie(s->gameGenie);
		s->gb->setGameShark(s->gameShark);
	}
	return understood;
}

}

namespace {

// Save states: the size reported to the frontend leaves room for a state to grow.
size_t stateSizeOne() {
	return s->gb ? s->stateSize : 0;
}

// Rollback and run-ahead take states of the whole machine exactly (exactStates), which mark their
// length's top bit; a state to keep is gambatte's own, which a Game Boy of another model can load.
// An exact state is saved with the shades blank, as each device shows the screen its own way.
bool exactStates;
gambatte::uint_least32_t const EXACT = 0x80000000u;
// Netplay's states hold no capacitor charge, and loading one keeps this device's.
bool netplayStates;

// Player 1's printer is kept at the end of its console's part, in the room left for the state to
// grow; zeros there, as in a state from before the printer, are a printer just switched on.
size_t printerAt() {
	return s->stateSize - sizeof(printer);
}

// The sums towards the next frontend sample, just before the printer's state, so sound carries on
// from a loaded state as it would have. Older states have zeros there.
size_t mixAt() {
	return printerAt() - 3 * sizeof(int32_t);
}

void saveMix(void* data, size_t length) {
	if (sizeof(gambatte::uint_least32_t) + length <= mixAt()) {
		int32_t const mix[3] = { static_cast<int32_t>(s->sumLeft), static_cast<int32_t>(s->sumRight), static_cast<int32_t>(s->summed) };
		std::memcpy(static_cast<char*>(data) + mixAt(), mix, sizeof(mix));
	}
}

void loadMix(const void* data, size_t size) {
	gambatte::uint_least32_t length;
	std::memcpy(&length, data, sizeof(length));
	length &= ~0x80000000u;
	int32_t mix[3] = { 0, 0, 0 };
	if (size >= s->stateSize && sizeof(length) + length <= mixAt()) {
		std::memcpy(mix, static_cast<const char*>(data) + mixAt(), sizeof(mix));
	}
	s->sumLeft = mix[0];
	s->sumRight = mix[1];
	s->summed = mix[2] >= 0 && static_cast<unsigned>(mix[2]) < ratio ? mix[2] : 0;
}

// The Mega Duck laptop's controller, just before the sums: where a transfer is, so that rollback,
// run-ahead and a loaded state carry on with the laptop as they would have. A state without it
// leaves the controller as it is.
// After it, whether the slot's cartridge had taken the system ROM's place.
size_t laptopAt() {
	return mixAt() - duckLaptopStateSize() - 1;
}

void saveLaptop(void* data, size_t length) {
	if (duckLaptopConnected() && sizeof(gambatte::uint_least32_t) + length <= laptopAt()) {
		duckLaptopSaveState(static_cast<uint8_t*>(data) + laptopAt());
		static_cast<uint8_t*>(data)[laptopAt() + duckLaptopStateSize()] = laptopSlotIn;
	}
}

// Whether a state has the laptop's part.
bool hasLaptop(const void* data, size_t size) {
	if (!duckLaptopConnected() || size < s->stateSize || size < sizeof(gambatte::uint_least32_t)) {
		return false;
	}
	gambatte::uint_least32_t length;
	std::memcpy(&length, data, sizeof(length));
	length &= ~0x80000000u;
	return sizeof(length) + length <= laptopAt();
}

void loadLaptop(const void* data, size_t size) {
	if (hasLaptop(data, size)) {
		duckLaptopLoadState(static_cast<const uint8_t*>(data) + laptopAt());
	}
}

// The output capacitor's charge, just before the laptop's controller, as 64-bit samples x 0x10000, as
// it was first kept. Older states have zeros there: an uncharged capacitor. Netplay's have zeros too,
// as the charge depends on each player's audio settings, and loading one keeps the charge as it is.
size_t capacitorAt() {
	return laptopAt() - 2 * sizeof(int64_t);
}

// Channel Selection: what's heard isn't in states, so where one is made, it's noted by the state's
// bytes, for a load of the same point (run-ahead loads the state it just made, netplay those of
// the last frames); another load picks up from the console's sound.
enum { HEARD_SAVED = 32 };
struct HeardSaved {
	const Console* console;
	uint64_t key;
	long left;
	long right;
	int32_t capacitor[2];
	gambatte::uint_least32_t sum;
};
HeardSaved heardSaved[HEARD_SAVED];
unsigned heardSavedNext;

uint64_t stateKey(const void* data) {
	gambatte::uint_least32_t length;
	std::memcpy(&length, data, sizeof(length));
	length &= ~0x80000000u;
	const unsigned char* bytes = static_cast<const unsigned char*>(data) + sizeof(length);
	uint64_t key = 1469598103934665603ull;
	for (gambatte::uint_least32_t i = 0; i < length; ++i) {
		key = (key ^ bytes[i]) * 1099511628211ull;
	}
	return key;
}

void saveHeard(const void* data) {
	if (!channelsMuted) {
		return;
	}
	HeardSaved& saved = heardSaved[heardSavedNext++ % HEARD_SAVED];
	saved.console = s;
	saved.key = stateKey(data);
	bool const apart = s->heardApart;
	saved.left = apart ? s->heardLeft : s->sumLeft;
	saved.right = apart ? s->heardRight : s->sumRight;
	saved.capacitor[0] = apart ? s->heardCapacitor[0] : s->capacitor[0];
	saved.capacitor[1] = apart ? s->heardCapacitor[1] : s->capacitor[1];
	saved.sum = s->gb->heardSum();
}

void loadHeard(const void* data) {
	s->heardLeft = s->sumLeft;
	s->heardRight = s->sumRight;
	s->heardCapacitor[0] = s->capacitor[0];
	s->heardCapacitor[1] = s->capacitor[1];
	s->heardApart = true;
	if (!channelsMuted) {
		return;
	}
	uint64_t const key = stateKey(data);
	for (int i = 0; i < HEARD_SAVED; ++i) {
		if (heardSaved[i].console == s && heardSaved[i].key == key) {
			s->heardLeft = heardSaved[i].left;
			s->heardRight = heardSaved[i].right;
			s->heardCapacitor[0] = heardSaved[i].capacitor[0];
			s->heardCapacitor[1] = heardSaved[i].capacitor[1];
			s->gb->setHeardSum(heardSaved[i].sum);
			return;
		}
	}
}

void saveCapacitor(void* data, size_t length) {
	if (sizeof(gambatte::uint_least32_t) + length <= capacitorAt()) {
		int64_t const saved[2] = { netplayStates ? 0 : static_cast<int64_t>(s->capacitor[0]) * 256, netplayStates ? 0 : static_cast<int64_t>(s->capacitor[1]) * 256 };
		std::memcpy(static_cast<char*>(data) + capacitorAt(), saved, sizeof(saved));
	}
}

void loadCapacitor(const void* data, size_t size) {
	if (netplayStates) {
		return;
	}
	gambatte::uint_least32_t length;
	std::memcpy(&length, data, sizeof(length));
	length &= ~0x80000000u;
	if (size >= s->stateSize && sizeof(length) + length <= capacitorAt()) {
		int64_t saved[2];
		std::memcpy(saved, static_cast<const char*>(data) + capacitorAt(), sizeof(saved));
		s->capacitor[0] = static_cast<int32_t>(saved[0] / 256);
		s->capacitor[1] = static_cast<int32_t>(saved[1] / 256);
	} else {
		s->capacitor[0] = s->capacitor[1] = 0;
	}
}

void savePrinter(void* data, size_t length) {
	if (s == &consoles[0] && sizeof(gambatte::uint_least32_t) + length <= printerAt()) {
		std::memcpy(static_cast<char*>(data) + printerAt(), &printer, sizeof(printer));
	}
}

void loadPrinter(const void* data, size_t size) {
	if (s != &consoles[0]) {
		return;
	}
	gambatte::uint_least32_t length;
	std::memcpy(&length, data, sizeof(length));
	length &= ~0x80000000u;
	if (size >= s->stateSize && sizeof(length) + length <= printerAt()) {
		std::memcpy(&printer, static_cast<const char*>(data) + printerAt(), sizeof(printer));
	} else {
		printerReset(&printer);
	}
}

bool saveExact(void* data) {
	bool const shades = !s->gb->isCgb() || (s->loadFlags & gambatte::GB::NO_BIOS);
	for (int i = 0; shades && i < 12; ++i) {
		s->gb->setDmgPaletteColor(i / 4, i % 4, 0);
	}
	gambatte::uint_least32_t const length = static_cast<gambatte::uint_least32_t>(s->gb->exactStateSize());
	bool const fits = length + sizeof(length) <= s->stateSize;
	if (fits) {
		gambatte::uint_least32_t const marked = length | EXACT;
		std::memcpy(data, &marked, sizeof(marked));
		s->gb->saveExactState(static_cast<char*>(data) + sizeof(length));
		std::memset(static_cast<char*>(data) + sizeof(length) + length, 0, s->stateSize - sizeof(length) - length);
		saveMix(data, length);
		saveCapacitor(data, length);
		saveLaptop(data, length);
		savePrinter(data, length);
		saveHeard(data);
	}
	setColors();
	return fits;
}

// Saves the state's length, then the state.
bool saveOne(void* data, size_t size) {
	if (!s->gb || size < s->stateSize) {
		return false;
	}
	if (exactStates) {
		return saveExact(data);
	}
	gambatte::uint_least32_t const length = s->gb->saveState(0, 0, static_cast<char*>(0));
	if (length + sizeof(length) > size) {
		return false;
	}
	std::memcpy(data, &length, sizeof(length));
	s->gb->saveState(0, 0, static_cast<char*>(data) + sizeof(length));
	// The room left for the state to grow is zero, so the same machine always makes the same state.
	std::memset(static_cast<char*>(data) + sizeof(length) + length, 0, s->stateSize - sizeof(length) - length);
	saveMix(data, length);
	saveCapacitor(data, length);
	saveLaptop(data, length);
	savePrinter(data, length);
	saveHeard(data);
	return true;
}

bool readState(const void* data, size_t size) {
	gambatte::uint_least32_t length;
	if (size < sizeof(length)) {
		return false;
	}
	std::memcpy(&length, data, sizeof(length));
	const char* const state = static_cast<const char*>(data) + sizeof(length);
	// An exact state is read straight into the machine, its pointers as offsets, so one of another
	// size, from another build, is refused before any of it is.
	if (length & EXACT) {
		length &= ~EXACT;
		if (length > size - sizeof(length) || length != s->gb->exactStateSize() || !s->gb->loadExactState(state, length)) {
			return false;
		}
		setColors();
		return true;
	}
	if (length > size - sizeof(length)) {
		return false;
	}
	// A state from another model switches the machine to that model until the next restart. The
	// state's third byte says whether it ran as a Game Boy Color, and in states that record it
	// (0x80), whether it ran as a Game Boy Advance. An older Color state counts as Game Boy Advance
	// when the option asks for one.
	//
	// A second instance that a frontend runs ahead with is given each state the first one makes, so
	// it follows along.
	bool const header = length >= 3 && static_cast<unsigned char>(state[0]) == 0xFF;
	unsigned const flags = header ? static_cast<unsigned char>(state[2]) : 0;
	bool const color = flags & gambatte::GB::CGB_MODE;
	bool const recorded = flags & 0x80;
	bool const advance = recorded ? (flags & gambatte::GB::GBA_FLAG) != 0 : s->wanted == LIBRETRO_GB_AGB;
	bool const machineAdvance = s->loadFlags & gambatte::GB::GBA_FLAG;
	if (header && (color != s->gb->isCgb() || (color && recorded && advance != machineAdvance))) {
		LibretroGbModel const was = s->chosen;
		restartAs(color ? (advance ? LIBRETRO_GB_AGB : LIBRETRO_GB_CGB) : LIBRETRO_GB_DMG);
		if (!s->gb->loadState(state, length)) {
			restartAs(was);
			return false;
		}
	} else if (!s->gb->loadState(state, length)) {
		return false;
	}
	// A state holds the shades it was saved with, but the screen should be the one chosen now. It
	// also holds the clock, so a state loaded before the first frame wins over the .rtc file.
	setColors();
	s->rtcPending = false;
	return true;
}

// On the device's time, a loaded state leaves the clock as it was, as the Advance's does, except
// before the first frame, where the state's clock wins over the .rtc file.
bool loadOne(const void* data, size_t size) {
	if (!s->gb) {
		return false;
	}
	gb_ulong clock[11];
	bool const keepClock = deviceClock && s->gb->hasMbc3Rtc() && !s->rtcPending;
	if (keepClock) {
		s->gb->getRtcRegs(clock);
	}
	// The ROM the state ran (the system's or the slot cartridge's) goes in first, for its mapper to
	// take the state's banks.
	bool const slotWas = laptopSlotIn;
	if (hasLaptop(data, size)) {
		laptopSlotAs(static_cast<const uint8_t*>(data)[laptopAt() + duckLaptopStateSize()] & 1);
	}
	if (!readState(data, size)) {
		laptopSlotAs(slotWas);
		return false;
	}
	loadMix(data, size);
	loadCapacitor(data, size);
	loadHeard(data);
	loadLaptop(data, size);
	loadPrinter(data, size);
	if (keepClock) {
		s->gb->setRtcRegs(clock);
	}
	s->gb->setDeviceTime(deviceClock);
	return true;
}

}

extern "C" {

void libretroGbExactStates(bool exact) {
	exactStates = exact;
}

void libretroGbNetplayStates(bool netplay) {
	netplayStates = netplay;
}

// Link cable: the byte this Game Boy shifts out, and one shifted in from the other's transfer.
int libretroGbLinkOut(void) {
	return s->gb ? s->gb->linkStatus(LINK_OUT) : 0xFF;
}

void libretroGbLinkIn(int byte) {
	if (s->gb) {
		s->gb->linkStatus(byte & 0xFF);
	}
}

int libretroGbLinkControl(void) {
	return s->gb ? s->gb->linkStatus(LINK_CONTROL) : 0;
}

void libretroGbSelect(int which) {
	s = &consoles[which];
}

// Real consoles are never in step, and games rely on it: when two try to lead the link at the same
// moment, Pokemon's handshake never settles. So each console runs a part of a frame behind the one
// before.
void libretroGbSetPrinter(bool on, PrinterOutput output) {
	if (on && !printerOn) {
		printerReset(&printer);
	}
	printerOn = on;
	printed = output;
}

void libretroGbSetPlayers(int count, bool withAdapter) {
	players = count;
	adapterOn = count > 1 && withAdapter;
	dmg07Reset(&adapter);
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		static long const behind[MAX_PLAYERS] = { 0, FRAME_SAMPLES / 2, FRAME_SAMPLES / 4, FRAME_SAMPLES * 3 / 4 };
		consoles[i].ahead = behind[i];
		if (consoles[i].gb) {
			consoles[i].gb->setLinkCallback(count > 1 ? 0 : linkClocked);
			consoles[i].gb->linkStatus(count > 1 ? LINK_ON : LINK_OFF);
			consoles[i].linked = false;
		}
	}
}

// With the consoles of a link here, a state holds, in order:
// - how far each console ran past the last frame;
// - the adapter;
// - each console's state, in its own fixed-size part;
// - their pictures, since a frame ends partway through drawing one and a state loaded for a
//   rollback has to carry on with it.
size_t libretroGbStateSize(void) {
	if (players < 2) {
		return stateSizeOne();
	}
	size_t size = players * sizeof(int32_t) + sizeof(adapter);
	for (int i = 0; i < players; ++i) {
		size += consoles[i].stateSize + sizeof(consoles[i].pixels) + sizeof(consoles[i].shown);
	}
	return size;
}

bool libretroGbSaveState(void* data, size_t size) {
	if (players < 2) {
		return saveOne(data, size);
	}
	if (size < libretroGbStateSize()) {
		return false;
	}
	char* at = static_cast<char*>(data);
	for (int i = 0; i < players; ++i) {
		if (!consoles[i].gb) {
			return false;
		}
		int32_t const ahead = consoles[i].ahead;
		std::memcpy(at, &ahead, sizeof(ahead));
		at += sizeof(ahead);
	}
	std::memcpy(at, &adapter, sizeof(adapter));
	at += sizeof(adapter);
	Console* const selected = s;
	bool saved = true;
	for (int i = 0; i < players && saved; ++i) {
		s = &consoles[i];
		saved = saveOne(at, s->stateSize);
		at += s->stateSize;
	}
	for (int i = 0; i < players && saved; ++i) {
		std::memcpy(at, consoles[i].pixels, sizeof(consoles[i].pixels));
		at += sizeof(consoles[i].pixels);
		std::memcpy(at, consoles[i].shown, sizeof(consoles[i].shown));
		at += sizeof(consoles[i].shown);
	}
	s = selected;
	return saved;
}

bool libretroGbLoadState(const void* data, size_t size) {
	if (players < 2) {
		return loadOne(data, size);
	}
	if (size < libretroGbStateSize()) {
		return false;
	}
	const char* at = static_cast<const char*>(data);
	int32_t ahead[MAX_PLAYERS];
	for (int i = 0; i < players; ++i) {
		if (!consoles[i].gb) {
			return false;
		}
		std::memcpy(&ahead[i], at, sizeof(ahead[i]));
		at += sizeof(ahead[i]);
	}
	std::memcpy(&adapter, at, sizeof(adapter));
	at += sizeof(adapter);
	Console* const selected = s;
	bool loaded = true;
	for (int i = 0; i < players && loaded; ++i) {
		s = &consoles[i];
		loaded = loadOne(at, s->stateSize);
		s->ahead = ahead[i];
		at += s->stateSize;
	}
	for (int i = 0; i < players && loaded; ++i) {
		std::memcpy(consoles[i].pixels, at, sizeof(consoles[i].pixels));
		at += sizeof(consoles[i].pixels);
		std::memcpy(consoles[i].shown, at, sizeof(consoles[i].shown));
		at += sizeof(consoles[i].shown);
	}
	s = selected;
	return loaded;
}

}
