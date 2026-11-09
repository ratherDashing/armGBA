/* Copyright (c) 2026 armGBA 3DS port contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_3DS_FRONTEND_H
#define ARMGBA_3DS_FRONTEND_H

// The 3DS frontend: a small libretro frontend for the armGBA core, with the game on the top screen
// and a menu on the bottom one.

#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "libretro.h"

#define APP_DIR "sdmc:/3ds/armgba"
#define SYSTEM_DIR APP_DIR "/system"
#define SAVE_DIR APP_DIR "/saves"
#define STATE_DIR APP_DIR "/states"
#define CONFIG_FILE APP_DIR "/config.ini"
#define LOG_FILE APP_DIR "/log.txt"

// util.c
void logInit(void);
void logLine(const char* format, ...) __attribute__((format(printf, 1, 2)));
void logClose(void);
// logupload.c: development builds send the logs to the network (make LOG_URL=...).
void logUpload(void);
bool isEmulator(void);
bool isNew3ds(void);
void makeDirs(void);
bool writeFile(const char* path, const void* data, size_t size);
void* readFile(const char* path, size_t* size);
bool fileExists(const char* path);
// The name of a path without its folder and extension.
void baseName(const char* path, char* out, size_t size);
// The top screen (and bottom one) as a 400x480 BMP: top screen above the bottom.
bool screenshotScreens(const char* path);
bool writeBmp565(const char* path, const uint16_t* pixels, int width, int height, int pitch);

// config.c: the frontend's own settings and the core's options, kept in config.ini.
// How the picture fills the top screen. LCD3x draws it three times as wide on the top screen's
// 800-pixel mode, under lcd3x's subpixel and scanline mask.
enum {
	SCALE_1X, SCALE_FIT, SCALE_STRETCH, SCALE_LCD3X, SCALE_COUNT,
};
struct Config {
	int scale;
	bool filter;
	char lastDir[512];
};
extern struct Config config;
void configLoad(void);
void configSave(void);

// options.c: the core options, as the core declares them.
struct CoreOption {
	char key[64];
	char label[64];
	char category[32];
	char* info;
	int valueCount;
	char** values;
	char** valueLabels;
	int current;
	int defaultIndex;
	bool visible;
};
struct OptionCategory {
	char key[32];
	char label[48];
	char info[256];
};
extern struct CoreOption feOptions[];
extern int feOptionCount;
extern bool feOptionsChanged;
extern struct OptionCategory feCategories[];
extern int feCategoryCount;
void optionsDefineV2(const struct retro_core_options_v2* options);
void optionsDefineV0(const struct retro_variable* variables);
const char* optionValue(const char* key);
void optionSetVisible(const char* key, bool visible);
void optionsApplySaved(const char* key, const char* value);
int optionsPendingCount(void);
void optionsPending(int i, const char** key, const char** value);

// video.c
void displayInit(void);
void displayExit(void);
void displayFrame(const void* data, unsigned width, unsigned height, size_t pitch);
void displayPresent(void);
// The same without waiting for the screen's refresh, for the game loop, which paces itself.
void displayPresentGame(void);
void displaySetFormat(enum retro_pixel_format format);
void displayClearTop(void);
// The core's last frame, as RGB565 (for screenshots in tests).
const uint16_t* displayLastFrame(int* width, int* height);

// audio.c
bool soundInit(double rate);
void soundExit(void);
size_t soundBatch(const int16_t* data, size_t frames);
// How full the queue is, 0-100, and whether it is about to run dry.
unsigned soundOccupancy(void);
void soundPause(bool paused);

// romload.c: a game from a plain file, or the first game in a .zip or .7z. GBA games are read into
// a buffer of the full cartridge space that the core takes over (gbaOfferRom).
void* romLoad(const char* path, size_t* size, char* innerName, size_t innerSize, bool* offered);

// menu.c: the bottom screen.
void menuInit(void);
// The game browser; returns false to quit the app.
bool menuBrowse(char* path, size_t size);
enum MenuAction {
	MENU_RESUME, MENU_QUIT_GAME, MENU_EXIT_APP,
};
enum MenuAction menuPause(void);
void menuInGame(void);
// Every setting: the 3DS screen's and the core's options by category.
void menuSettings(void);
void menuMessage(const char* text, unsigned frames);

// game.c: the loaded game.
void gameCoreInit(void);
// Why the last gameLoad failed, for the browser to show.
const char* gameLoadError(void);
void gameCoreExit(void);
bool gameFastForward(void);
double gameFps(void);
// Performance, for the bottom screen: the emulation's milliseconds a frame, cache flushes.
extern float perfEmulationMs;
extern float cpuMhz;
// render-thread.c: the game thread waiting for the picture, and core 2 drawing it.
extern uint64_t renderWaitTicks, renderWorkTicks;
extern uint64_t displayUploadTicks, displayPresentTicks;
// jit.c: time spent compiling, the time compiling may take in a frame and where the frame began,
// and blocks put off to a later frame for it.
extern uint64_t jitCompileTicks, jitCompileBudget, jitCompileFrameStart, jitBudgetPutOff;
// jit.c: time spent compiling.
extern uint64_t jitCompileTicks;
extern uint32_t jitFlushes;
extern uint64_t jitFlushTicks;
extern bool jitFlushWhole;
// The recompiler's running totals.
struct JitCounts {
	uint64_t compiled, invalidations, patches, revived, stepped, codeWrites;
	// Everything compiled thrown out: [0] asked for (cartridge changed, work RAM waits), [1] out of room.
	uint64_t fullFlushes[2];
	// Why compiled code was left: jit.c's exit reasons.
	uint64_t exits[5];
	uint32_t codeUsed;
	uint32_t codeSize;
	uint64_t hotBytes, coldBytes, wastedBytes, carefulCompiles, carefulBytes, carefulPutOff, deferred;
};
void jitCounts(struct JitCounts* counts);
extern int renderThreadCore;
// After an option changes in the menu: the core shows or hides the options that depend on it.
void gameOptionsChanged(void);
// Has the core show and hide its options for the current settings and game, as a menu opens.
void gameRefreshOptionDisplay(void);
bool gameLoad(const char* path);
void gameUnload(void);
void gameRunFrame(void);
void gameSaveSram(bool force);
bool gameSaveState(int slot);
bool gameLoadState(int slot);
void gameReset(void);
bool gameStateExists(int slot);
const char* gameTitle(void);
extern uint32_t gameFrames;

// input.c
void inputPoll(void);
int16_t inputState(unsigned port, unsigned device, unsigned index, unsigned id);
extern u32 keysHeldNow, keysDownNow, keysRepeatNow;
extern uint32_t uiFrames;
extern u32 scriptedKeys, scriptedUiKeys;
// main.c: a test script's steps for this frame of the app.
void scriptUiFrame(uint32_t n);
bool scriptQuitting(void);

#endif
