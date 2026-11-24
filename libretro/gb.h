/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_LIBRETRO_GB_H
#define ARMGBA_LIBRETRO_GB_H

// The libretro core's Game Boy and Game Boy Color side (gb-gambatte.cpp).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct retro_memory_descriptor;

enum {
	LIBRETRO_GB_WIDTH = 160,
	LIBRETRO_GB_HEIGHT = 144,
};

enum LibretroGbModel {
	LIBRETRO_GB_AUTO,
	LIBRETRO_GB_DMG,
	LIBRETRO_GB_CGB,
	LIBRETRO_GB_AGB,
	// The Game Boy Pocket; the Game Boy Light is the same with a backlight.
	LIBRETRO_GB_MGB,
};

// How games start: as a boot ROM would have left the machine, through Nintendo's boot ROM when the
// system directory has it (armGBA's otherwise), or through armGBA's own boot ROM built into the core.
enum LibretroGbBoot {
	LIBRETRO_GB_BOOT_OFF,
	LIBRETRO_GB_BOOT_NINTENDO,
	LIBRETRO_GB_BOOT_ARMGBA,
};

// Loads a game. Returns false, with a message for the user, if it can't be loaded.
bool libretroGbLoad(const void* rom, size_t size, const char* systemDirectory, enum LibretroGbModel, bool colorCorrection,
                    unsigned audioRate, char* error, size_t errorSize);
void libretroGbUnload(void);
// Takes effect when a game is loaded or reset.
void libretroGbSetBootRom(enum LibretroGbBoot);
// The Game Boy's output capacitor, which keeps DC out of its sound: as each model has it, the
// original Game Boy's on every model (removing the offset, keeping the bass), or none.
enum LibretroGbFilter {
	LIBRETRO_GB_FILTER_ACCURATE,
	LIBRETRO_GB_FILTER_OFFSET,
	LIBRETRO_GB_FILTER_OFF,
};
void libretroGbSetAudioFilter(enum LibretroGbFilter);
// GBA Audio Output: what the Game Boy Advance's sound leaves through (its service manual's AGB-CPU
// schematic). The amp's outputs sit at half its supply, and the speaker's other side and the
// headphones' shared return go to ground through one capacitor (CP4), which keeps that DC out of the
// sound and takes bass with it: under about 200 Hz through the speaker (8 ohms), about 70 Hz through
// headphones (32 ohms each side, behind 15, both sides charging it together), where only what the two
// sides have in common goes through it. Off leaves the sound without it. A Game Boy game on a Game Boy
// Advance has it too, with the Game Boy Audio Filter on Accurate.
enum LibretroAdvanceOutput {
	LIBRETRO_ADVANCE_SPEAKER,
	LIBRETRO_ADVANCE_HEADPHONES,
	LIBRETRO_ADVANCE_OFF,
};
#define ADVANCE_OUTPUT_FARADS 100e-6
// The resistance the capacitor charges through: the speaker, or the two headphone sides together
#define ADVANCE_OUTPUT_OHMS(output) ((output) == LIBRETRO_ADVANCE_HEADPHONES ? (32.0 + 15.0) / 2 : 8.0)
void libretroGbSetAdvanceOutput(enum LibretroAdvanceOutput);

// An output capacitor, one frontend sample at a time, in 32-bit fixed point so every CPU makes the
// same sound: each sample out is what's in less the charge (samples x 256), and the charge then takes
// up all but keep (out of 0x10000) of that difference (Pan Docs, Audio Details), with one widening
// multiply. The sample out is rounded to the nearest, so the sound stays centred.
static inline int16_t outputClip(int32_t scaled) {
	int32_t const sample = (scaled + 128) >> 8;
	return (int16_t) (sample < -32768 ? -32768 : sample > 32767 ? 32767 : sample);
}

static inline int16_t outputCapacitor(int32_t* charge, int32_t in, int32_t keep) {
	int32_t const out = in * 256 - *charge;
	*charge += (int32_t) (((int64_t) out * (0x10000 - keep)) >> 16);
	return outputClip(out);
}

// Headphones on a Game Boy Advance: both sides return through the one capacitor, which charges
// towards their mean, and each side is heard less that charge.
static inline void outputSharedReturn(int32_t* charge, int32_t left, int32_t right, int32_t keep, int16_t* out) {
	int32_t const now = *charge;
	*charge += (int32_t) (((int64_t) ((left + right) * 128 - now) * (0x10000 - keep)) >> 16);
	out[0] = outputClip(left * 256 - now);
	out[1] = outputClip(right * 256 - now);
}

// Whether the next game loaded is a Mega Duck's: an original Game Boy with the Mega Duck's registers
// and cartridges, starting without a boot ROM, as it has none. -1 for a Game Boy game, else the kind of
// cartridge: 0 either handheld kind (a .bin), 1 none (.duck), 2 MD0, 3 MD1, 4 MD2, 5 MD20S, 6 MD25S.
void libretroGbSetMegaDuck(int);
// The Mega Duck laptop's controller (keyboard, clock, printer, speech) on the serial port, for the next
// Mega Duck game loaded: printer 0 none, 1 one pass, 2 two passes; language 0 Spanish (Super QuiQue), 1
// German (Super Junior), for its speech.
void libretroGbSetMegaDuckLaptop(bool on, int printer, int language);
// The cartridge in the laptop's slot, which it runs when asked (NULL for none): its ROM takes the running
// one's place, which must have been loaded at least as big. kind as libretroGbSetMegaDuck has it.
void libretroGbSetMegaDuckSlot(const void* rom, size_t size, int kind);
// Datel's Action Replay for the next game loaded, its program these bytes (NULL for none); the game
// loaded is the one on top, or with game false a stand-in for nothing on top.
void libretroGbSetActionReplay(const void* program, size_t size, bool game);
// Another game on top of the selected console's cheat cartridge (with game false, a stand-in for
// none): the console starts again with it, the cartridge's chip as it was. States keep their size
// unless the game needs more. Returns false, with a message, if the game can't run, the one before
// going back in.
bool libretroGbSwapGame(const void* rom, size_t size, bool game, char* error, size_t errorSize);
// The codes the Game Genie puts in its empty slots: nine digits each (0x10 past the end), and 6, 9 or 0
void libretroGbSetGameGenieCodes(const uint8_t digits[27], const uint8_t counts[3]);
// Its button and switch (on: codes on)
void libretroGbActionReplayControls(bool button, bool switchOn);
// What it keeps with the power off (its flash chip, or the first one's battery-backed RAM), as the
// running console has it (NULL without an Action Replay), and whether that was written since this
// was last asked.
uint8_t* libretroGbActionReplayFlash(size_t* size);
bool libretroGbActionReplayFlashChanged(void);
// A key of the frontend's keyboard, for the laptop
void libretroGbKeyboard(bool down, unsigned keycode, uint32_t character, uint16_t modifiers);
// The console's sound with the laptop's speech over it
const int16_t* libretroGbMixSpeech(const int16_t* samples, size_t frames);
// Resets the game. With a different model than it started as, or the boot ROM switched, it restarts
// that way.
void libretroGbReset(enum LibretroGbModel);
// Restarts the selected console alone, as its power switch would.
void libretroGbRestart(void);
void libretroGbSetColorCorrection(bool);
// On the Game Boy Advance model, shows the Color's panel, its colors, instead of the Advance's.
void libretroGbSetColorPanel(bool color);
// Sets the original Game Boy's screen: 0 plain grays, 1 the first model's, 2 the Pocket's, 3 the
// Light's backlit.
void libretroGbSetScreen(int);
// Sets that screen's contrast wheel, -5 to 5.
void libretroGbSetContrast(int);
// Layer Selection's layers shown (1 background, 2 sprites, 4 window) and Channel Selection's channels
// left out (bits 0-3: channels 1-4), for every console. Only what's drawn and heard changes.
void libretroGbSetLayersShown(unsigned mask);
void libretroGbSetMutedChannels(unsigned mask);
// That screen's shades as shown now, 0 the lightest, as XRGB8888.
uint32_t libretroGbShade(int shade);
// That screen's color while the game has it off, as XRGB8888.
uint32_t libretroGbOffColor(void);
// That screen's darkest shade at the middle of its contrast wheel, as XRGB8888.
uint32_t libretroGbDarkestColor(void);
// Whether the game runs as on a Game Boy Advance, which shows it on its own larger screen.
bool libretroGbOnAdvance(void);
// The boot ROMs from the system folder the consoles started with, as a CRC; 0 when every one runs
// armGBA's or none.
uint32_t libretroGbBootCrc(void);
// Runs a frame with the pressed buttons as KEYINPUT bits. Returns the picture in XRGB8888 and the
// stereo samples.
const uint32_t* libretroGbRunFrame(uint16_t keys, const int16_t** samples, size_t* frames);
void* libretroGbSaveRam(size_t* size);
// An MBC3 or HuC3 cartridge's clock, which the frontend keeps as the .rtc file; NULL for other
// cartridges.
void* libretroGbRtc(size_t* size);
// The cartridge's clock: none, an MBC3's or a HuC3's.
enum LibretroGbClock { LIBRETRO_GB_NO_CLOCK, LIBRETRO_GB_MBC3_CLOCK, LIBRETRO_GB_HUC3_CLOCK };
enum LibretroGbClock libretroGbClock(void);
// The Clock options for every console: Game Time keeps the clock from counting the time the game was
// off; the speed is in percent of the console's (0 stopped); the offset, in seconds, moves an MBC3
// clock by its change.
void libretroGbSetClock(bool gameTime, unsigned percent, long long offset);
// Each frame: whether the clocks run on the device's time (Real Time outside netplay) or count the
// console's cycles, as every device in netplay must.
void libretroGbDeviceTime(bool on);
// Clears every MBC3 clock with its halt bit set, as a dead battery leaves it; returns whether there
// was one.
bool libretroGbResetClock(void);
// Sets how far a tilt-sensor cartridge is tilted, each axis from -32768 to 32767. Right and towards
// the player are positive, like a stick's right and down.
void libretroGbSetTilt(int x, int y);
// Flicks the console upwards: the sensor's next reading has its top thrown towards the player.
void libretroGbFlick(void);
// The work RAM, for a frontend's cheats and achievements.
void* libretroGbSystemRam(size_t* size);
// Fills in the machine's memory areas as gambatte-libretro describes them to a frontend (room for
// ten), and returns how many there are. They stay put while the game is loaded, but a Game Boy
// Color has more.
size_t libretroGbMemoryMap(struct retro_memory_descriptor*);
bool libretroGbIsColor(void);
// Sets the cheat codes: Game Genie (HHH-HHH or HHH-HHH-HHH) and GameShark (01VVAAAA), each split by
// '+' or spaces. They stay in force, including after a restart. Returns false if a code is neither.
bool libretroGbSetCheats(const char* const* codes, size_t count);
// The CPU Clock for every console, in quarters of the console's own clock (4); loaded right after a
// state loaded, whose carried part of a cycle is kept.
void libretroGbSetCpuClock(unsigned quarters, bool loaded);
// Whether a console is still running its boot ROM.
bool libretroGbBooting(void);
size_t libretroGbStateSize(void);
// States for rollback and run-ahead save the whole machine exactly; others, in gambatte's format.
void libretroGbExactStates(bool exact);
// States for netplay leave out what each device sets for itself, such as the output capacitor's
// charge, which the Game Boy Audio Filter and GBA Audio Output shape, so every player's match.
void libretroGbNetplayStates(bool netplay);
bool libretroGbSaveState(void* data, size_t size);
bool libretroGbLoadState(const void* data, size_t size);
// The link cable: the byte this Game Boy shifts out, and a byte shifted in when the other Game Boy
// clocks a transfer.
int libretroGbLinkOut(void);
void libretroGbLinkIn(int byte);
// The serial control register, SC.
int libretroGbLinkControl(void);
// The Game Boy Printer on player 1's link port, when no other Game Boy is linked; output gets each
// print (printer.h).
void libretroGbSetPrinter(bool on, void (*output)(const uint8_t* pixels, int rows, bool cut, uint32_t number));
// The consoles of a link here, two to four. The functions above work on the selected one (0 to 3).
// Setting how many there are joins their serial ports, by the cable (two) or the Four Player
// Adapter, and restarts their timing. States then hold them all.
void libretroGbSelect(int which);
void libretroGbSetPlayers(int count, bool adapter);
// Runs the consoles for a frame, with each one's buttons, returning each one's picture and sound.
void libretroGbRunPlayers(const uint16_t keys[], const uint32_t* pictures[], const int16_t* samples[], size_t frames[]);

#endif
