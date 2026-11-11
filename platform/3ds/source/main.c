/* Copyright (c) 2026 armGBA 3DS port contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// armGBA for the 3DS: pick a game on the bottom screen, play it on the top one.

#include "frontend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The core's paths are long and some of its functions keep several on the stack.
u32 __stacksize__ = 512 * 1024;
// libctru would split the app's memory evenly, up to 32 MiB of linear memory (what the GPU and DSP
// read), leaving about 28 MiB of heap on an Old 3DS or a New 3DS without its extended memory: less
// than the core's 32 MiB cartridge buffer alone. The frontend's textures, the LCD3x mask and the
// sound need about 6.5 MiB of linear memory at most (LCD3x), so it gets 8 and the heap the rest.
u32 __ctru_linear_heap_size = 8 * 1024 * 1024;

// libctru's own split gives the heap everything the linear heap doesn't take, but the heap's address
// range is 96 MiB: in the New 3DS's 124 MB mode that is more, its allocation fails and the app stops
// before main (2026-11-09; Azahar doesn't hold the heap to its range). The heap gets all the range
// can hold and the linear heap the rest, at least 8 MiB. The RSF asks for the 80 MB mode (about
// 67 MiB of heap): asked for 124 MB, the New 3DS gave it only some launches and 64 MB the others.
extern char* fake_heap_start;
extern char* fake_heap_end;
u32 __ctru_heap;
u32 __ctru_heap_size;
u32 __ctru_linear_heap;
void mappableInit(u32 addrMin, u32 addrMax);

void __system_allocateHeaps(void) {
	Handle reslimit = 0;
	if (R_FAILED(svcGetResourceLimit(&reslimit, CUR_PROCESS_HANDLE))) {
		svcBreak(USERBREAK_PANIC);
	}
	s64 maxCommit = 0, currentCommit = 0;
	ResourceLimitType type = RESLIMIT_COMMIT;
	svcGetResourceLimitLimitValues(&maxCommit, reslimit, &type, 1);
	svcGetResourceLimitCurrentValues(&currentCommit, reslimit, &type, 1);
	svcCloseHandle(reslimit);
	u32 const remaining = (u32) (maxCommit - currentCommit) & ~0xFFFu;
	u32 const area = OS_HEAP_AREA_END - OS_HEAP_AREA_BEGIN;
	u32 heap = remaining > __ctru_linear_heap_size ? remaining - __ctru_linear_heap_size : 0;
	if (heap > area) {
		heap = area;
	}
	__ctru_heap_size = heap;
	__ctru_linear_heap_size = remaining - heap;
	if (R_FAILED(svcControlMemory(&__ctru_heap, OS_HEAP_AREA_BEGIN, 0, __ctru_heap_size, MEMOP_ALLOC, MEMPERM_READ | MEMPERM_WRITE))) {
		svcBreak(USERBREAK_PANIC);
	}
	if (R_FAILED(svcControlMemory(&__ctru_linear_heap, 0, 0, __ctru_linear_heap_size, MEMOP_ALLOC_LINEAR, MEMPERM_READ | MEMPERM_WRITE))) {
		svcBreak(USERBREAK_PANIC);
	}
	mappableInit(OS_MAP_AREA_BEGIN, OS_MAP_AREA_END);
	fake_heap_start = (char*) __ctru_heap;
	fake_heap_end = fake_heap_start + __ctru_heap_size;
}


// --- Test scripts (sdmc:/3ds/armgba/autotest.txt) -----------------------------------------------
// For running in an emulator without hands: loads a game, presses buttons, takes screenshots, logs
// speed, then exits. One command a line:
//   rom=<path>              the game
//   frames=<n>              exit after this many frames
//   unthrottled=1           don't wait for the screen: measures how fast the emulation runs
//   pressevery=<from>:<to>:<period>:<buttons>   a 3-frame press every period frames from..to
//   press=<frame>:<buttons>:<frames>   buttons (A,B,X,Y,L,R,START,SELECT,UP,DOWN,LEFT,RIGHT, joined by +)
//   shot=<frame>            the core's frame as shot_<frame>.bmp and both screens as screen_<frame>.bmp
//   savestate=<frame>:<slot> / loadstate=<frame>:<slot>
//   option=<key>=<value>    a core option
// and, counted in frames of the whole app (the browser and menus too) rather than of the game:
//   uipress=<frame>:<buttons>:<frames>, uitouch=<frame> (a touch of the bottom screen),
//   uishot=<frame> (both screens as ui_<frame>.bmp), uiexit=<frame>
// With no rom=, the browser picks the game. It ends with "autotest done" in log.txt.

enum {
	MAX_STEPS = 512,
};

static struct {
	bool on;
	char rom[512];
	uint32_t frames;
	bool unthrottled;
	int count;
	uint32_t uiExit;
	bool quit;
	struct {
		char kind;
		bool ui;
		uint32_t frame;
		uint32_t length;
		u32 keys;
		int slot;
	} steps[MAX_STEPS];
} script;

static u32 keysNamed(char* names) {
	static const struct {
		const char* name;
		u32 key;
	} keys[] = {
		{ "A", KEY_A }, { "B", KEY_B }, { "X", KEY_X }, { "Y", KEY_Y }, { "L", KEY_L }, { "R", KEY_R }, { "ZL", KEY_ZL }, { "ZR", KEY_ZR },
		{ "TOUCH", KEY_TOUCH }, { "START", KEY_START }, { "SELECT", KEY_SELECT }, { "UP", KEY_DUP }, { "DOWN", KEY_DDOWN }, { "LEFT", KEY_DLEFT }, { "RIGHT", KEY_DRIGHT },
	};
	u32 result = 0;
	for (char* name = strtok(names, "+"); name; name = strtok(NULL, "+")) {
		for (size_t i = 0; i < sizeof(keys) / sizeof(*keys); ++i) {
			if (!strcmp(name, keys[i].name)) {
				result |= keys[i].key;
			}
		}
	}
	return result;
}

static void scriptRead(void) {
	FILE* f = fopen(APP_DIR "/autotest.txt", "r");
	if (!f) {
		return;
	}
	script.on = true;
	script.frames = 600;
	char line[600];
	while (fgets(line, sizeof(line), f) && script.count < MAX_STEPS) {
		line[strcspn(line, "\r\n")] = 0;
		char* value = strchr(line, '=');
		if (!value) {
			continue;
		}
		*value++ = 0;
		if (!strcmp(line, "rom")) {
			snprintf(script.rom, sizeof(script.rom), "%s", value);
		} else if (!strcmp(line, "frames")) {
			script.frames = strtoul(value, NULL, 10);
		} else if (!strcmp(line, "unthrottled")) {
			script.unthrottled = atoi(value) != 0;
		} else if (!strcmp(line, "uiexit")) {
			script.uiExit = strtoul(value, NULL, 10);
		} else if (!strcmp(line, "option")) {
			char* eq = strchr(value, '=');
			if (eq) {
				*eq = 0;
				optionsApplySaved(value, eq + 1);
			}
		} else {
			char* parts[3] = { value, NULL, NULL };
			for (int i = 1; i < 3; ++i) {
				parts[i] = parts[i - 1] ? strchr(parts[i - 1], ':') : NULL;
				if (parts[i]) {
					*parts[i]++ = 0;
				}
			}
			typeof(script.steps[0])* s = &script.steps[script.count];
			memset(s, 0, sizeof(*s));
			s->frame = strtoul(parts[0], NULL, 10);
			s->ui = !strncmp(line, "ui", 2);
			if (s->ui) {
				memmove(line, line + 2, strlen(line + 2) + 1);
			}
			if (!strcmp(line, "pressevery")) {
				// from:to:period:buttons, unrolled into presses.
				char* rest = parts[2];
				char* buttons = rest ? strchr(rest, ':') : NULL;
				if (buttons) {
					*buttons++ = 0;
				}
				uint32_t const to = parts[1] ? strtoul(parts[1], NULL, 10) : 0;
				uint32_t const period = rest ? strtoul(rest, NULL, 10) : 0;
				u32 const keys = buttons ? keysNamed(buttons) : 0;
				for (uint32_t f = s->frame; period && f < to && script.count < MAX_STEPS; f += period) {
					typeof(script.steps[0])* r = &script.steps[script.count++];
					memset(r, 0, sizeof(*r));
					r->kind = 'p';
					r->frame = f;
					r->length = 3;
					r->keys = keys;
				}
				continue;
			}
			if (!strcmp(line, "touch")) {
				s->kind = 'p';
				s->keys = KEY_TOUCH;
				s->length = 1;
			} else if (!strcmp(line, "press") && parts[1]) {
				s->kind = 'p';
				s->keys = keysNamed(parts[1]);
				s->length = parts[2] ? strtoul(parts[2], NULL, 10) : 1;
			} else if (!strcmp(line, "shot")) {
				s->kind = 's';
			} else if (!strcmp(line, "savestate") || !strcmp(line, "loadstate")) {
				s->kind = line[0] == 's' ? 'S' : 'L';
				s->slot = parts[1] ? atoi(parts[1]) : 0;
			} else {
				continue;
			}
			++script.count;
		}
	}
	fclose(f);
	remove(APP_DIR "/autotest.txt");
	logLine("autotest: %s, %lu frames, %d steps%s", script.rom, script.frames, script.count, script.unthrottled ? ", unthrottled" : "");
}

// Before frame n: the buttons held and the steps that fall on it.
static void scriptFrame(uint32_t n) {
	scriptedKeys = 0;
	for (int i = 0; i < script.count; ++i) {
		typeof(script.steps[0])* s = &script.steps[i];
		if (s->ui) {
			continue;
		}
		if (s->kind == 'p' && n >= s->frame && n < s->frame + s->length) {
			scriptedKeys |= s->keys;
		}
		if (n != s->frame) {
			continue;
		}
		char path[128];
		switch (s->kind) {
		case 's': {
			int w, h;
			const uint16_t* pixels = displayLastFrame(&w, &h);
			snprintf(path, sizeof(path), APP_DIR "/shot_%05lu.bmp", n);
			bool ok = pixels && writeBmp565(path, pixels, w, h, w);
			snprintf(path, sizeof(path), APP_DIR "/screen_%05lu.bmp", n);
			ok = screenshotScreens(path) && ok;
			logLine("autotest: shot at frame %lu (%dx%d) %s", n, w, h, ok ? "saved" : "FAILED");
			break;
		}
		case 'S':
			gameSaveState(s->slot);
			break;
		case 'L':
			gameLoadState(s->slot);
			break;
		default:
			break;
		}
	}
}

bool scriptQuitting(void) {
	return script.quit;
}

void scriptUiFrame(uint32_t n) {
	scriptedUiKeys = 0;
	if (!script.on) {
		return;
	}
	for (int i = 0; i < script.count; ++i) {
		typeof(script.steps[0])* s = &script.steps[i];
		if (!s->ui) {
			continue;
		}
		if (s->kind == 'p' && n >= s->frame && n < s->frame + s->length) {
			scriptedUiKeys |= s->keys;
		}
		if (n == s->frame && s->kind == 's') {
			char path[128];
			snprintf(path, sizeof(path), APP_DIR "/ui_%05lu.bmp", n);
			logLine("autotest: ui shot at %lu %s", n, screenshotScreens(path) ? "saved" : "FAILED");
		}
	}
	if (script.uiExit && n >= script.uiExit) {
		logLine("autotest: ui exit at %lu", n);
		script.quit = true;
	}
}

float perfEmulationMs;
float cpuMhz;

// The CPU's clock, measured: the ARM11's own cycle counter (read in kernel mode, svc.s) against the
// system tick, which stays 268 MHz whatever the CPU runs at. Shows whether the New 3DS's 804 MHz is
// really on. 0 in an emulator, which has neither.
u32 ctrCycles;
u32 ctrReadCycles(void);

static float measureCpuMhz(void) {
	if (isEmulator()) {
		return 0;
	}
	ctrReadCycles();
	u64 const before = svcGetSystemTick();
	u32 const start = ctrReadCycles();
	u32 n = 4 * 1000 * 1000;
	__asm__ volatile("1: subs %0, %0, #1\n\tbne 1b" : "+r"(n) : : "cc");
	u32 const cycles = ctrReadCycles() - start;
	u64 const ticks = svcGetSystemTick() - before;
	return ticks ? (float) cycles * SYSCLOCK_ARM11 / ticks / 1e6f : 0;
}

// A development build counts what the processor did while it emulated: its own counts of cycles and
// of two events at a time (instructions, cache and TLB misses, mispredicted branches, stalls), a
// different pair each second, for the log's perf line (menu.c). The counts are the core's, so they
// include whatever else ran on it meanwhile. Hardware only; an emulator has no such counters.
u32 pmuRead[3];
u32 pmuControl;
void ctrPmuKernel(void);
// ARM11 MPCore event numbers.
const uint8_t pmuEvents[PMU_PAIRS][2] = {
	{ 0x08, 0x0B }, // instructions executed, data cache read misses
	{ 0x00, 0x0D }, // instruction cache misses, data cache write misses
	{ 0x03, 0x04 }, // instruction and data MicroTLB misses
	{ 0x10, 0x07 }, // main TLB misses, mispredicted branches
	{ 0x02, 0x01 }, // stalls on a data dependency, stalls waiting for instructions
	{ 0x11, 0x12 }, // external memory requests, stalls on a full load/store queue
};
uint64_t pmuTotals[PMU_PAIRS][3];
uint32_t pmuFrames[PMU_PAIRS];
int pmuPair;
static bool pmuOn;

static void pmuBegin(void) {
#if defined(ARMGBA_LOG_URL)
	pmuOn = !isEmulator();
	if (pmuOn) {
		// Enabled, both event counts and the cycle count cleared (every cycle counted), overflow
		// flags cleared, no interrupts.
		pmuControl = (u32) pmuEvents[pmuPair][0] << 20 | (u32) pmuEvents[pmuPair][1] << 12 | 0x707;
		ctrPmuKernel();
	}
#endif
}

static void pmuEnd(void) {
	if (pmuOn) {
		ctrPmuKernel();
		for (int i = 0; i < 3; ++i) {
			pmuTotals[pmuPair][i] += pmuRead[i];
		}
		++pmuFrames[pmuPair];
	}
}

// --- The game loop ------------------------------------------------------------------------------

// Runs the game until it is closed; false when the app should exit.
static bool play(void) {
	uint64_t const start = osGetTime();
	uint64_t emulated = 0;
	uint64_t emulatedTicks = 0, ticksAtReport = 0;
	uint32_t framesAtReport = 0;
	uint64_t reportAt = svcGetSystemTick();
	// The GBA's frame, in system ticks: frames are paced to it, not to the screen's 59.83 Hz.
	double const period = SYSCLOCK_ARM11 / gameFps();
	double deadline = (double) svcGetSystemTick();
	uint32_t lastReport = 0;
	uint64_t reportTime = start, reportEmulated = 0;
	// What a frame takes without compiling, in ticks: the largest of late, forgotten slowly. What is
	// left of the frame's time is what the recompiler may spend compiling in it (jit.c); past that
	// it steps through new code and compiles it in a later frame.
	double typical = period / 2;
	uint64_t lastTop = 0, slept = 0;
	menuBottomForGame(true);
	while (aptMainLoop() && !script.quit) {
		uint64_t const top = svcGetSystemTick();
		if (lastTop) {
			double const work = (double) (top - lastTop - slept) - (double) (jitCompileTicks - jitCompileFrameStart);
			if (work < 3 * period) {
				typical = work > typical ? work : typical - (typical - work) / 32;
			}
		}
		lastTop = top;
		slept = 0;
		double budget = period - typical - 0.7 * CPU_TICKS_PER_MSEC;
		budget = budget < 0.25 * CPU_TICKS_PER_MSEC ? 0.25 * CPU_TICKS_PER_MSEC : budget > 6.0 * CPU_TICKS_PER_MSEC ? 6.0 * CPU_TICKS_PER_MSEC : budget;
		jitCompileFrameStart = jitCompileTicks;
		jitCompileBudget = gameFastForward() ? ~0ull : (uint64_t) budget;
		inputPoll();
		if (keysDownNow & KEY_TOUCH) {
			gameSaveSram(true);
			menuBottomForGame(false);
			enum MenuAction action = menuPause();
			if (action != MENU_RESUME) {
				return action == MENU_QUIT_GAME;
			}
			menuBottomForGame(true);
			lastTop = 0;
			deadline = (double) svcGetSystemTick();
			continue;
		}
		if (script.on) {
			scriptFrame(gameFrames);
			if (gameFrames >= script.frames) {
				float const seconds = (osGetTime() - start) / 1000.0f;
				logLine("autotest: %lu frames in %.2f s, %.1f fps; emulation alone %.1f fps", gameFrames, seconds, gameFrames / seconds,
				     gameFrames * 1000.0f / (float) (emulated ? emulated : 1));
				return false;
			}
		}
		// Fast-forward (ZL / ZR held, or the core's own turbo): several frames to each one shown.
		int const runs = gameFastForward() ? 4 : 1;
		for (int i = 0; i < runs; ++i) {
			uint64_t const before = svcGetSystemTick();
			pmuBegin();
			gameRunFrame();
			pmuEnd();
			emulatedTicks += svcGetSystemTick() - before;
		}
		emulated = emulatedTicks / CPU_TICKS_PER_MSEC;
		if (!script.unthrottled || gameFrames % 30 == 0) {
			displayPresentGame();
		}
		// Waits for the frame's time to come, kept a hair faster or slower to hold the sound queue
		// near half full. Behind (a slow frame), it carries on at once and doesn't try to catch up.
		uint64_t now = svcGetSystemTick();
		if (script.unthrottled || gameFastForward()) {
			deadline = (double) now;
		} else {
			unsigned const queue = soundOccupancy();
			deadline += period * (queue < 35 ? 0.995 : queue > 65 ? 1.005 : 1.0);
			// What is left of the frame's time goes to the warm list (jitmem.c), a block at a time,
			// while more than a block's worth is left.
			while ((double) now + 0.5 * CPU_TICKS_PER_MSEC < deadline && jitWarmOne()) {
				now = svcGetSystemTick();
			}
			if ((double) now < deadline) {
				svcSleepThread((s64) ((deadline - (double) now) * 1e9 / SYSCLOCK_ARM11));
				slept = svcGetSystemTick() - now;
			} else if ((double) now - deadline > 3 * period) {
				deadline = (double) now;
			}
		}
		now = svcGetSystemTick();
		if (now - reportAt >= SYSCLOCK_ARM11) {
			uint32_t const frames = gameFrames - framesAtReport;
			perfEmulationMs = frames ? (float) (emulatedTicks - ticksAtReport) / frames / CPU_TICKS_PER_MSEC : 0;
			framesAtReport = gameFrames;
			ticksAtReport = emulatedTicks;
			reportAt = now;
		}
		menuInGame();
		gameSaveSram(false);
		if (script.on && gameFrames - lastReport >= 300) {
			uint64_t const now = osGetTime();
			logLine("autotest: frame %lu, %.1f fps, emulation alone %.1f fps", gameFrames,
			     (gameFrames - lastReport) * 1000.0f / (float) (now - reportTime),
			     (gameFrames - lastReport) * 1000.0f / (float) ((emulated - reportEmulated) ? (emulated - reportEmulated) : 1));
			lastReport = gameFrames;
			reportTime = now;
			reportEmulated = emulated;
		}
	}
	return false;
}

int main(int argc, char** argv) {
	(void) argc;
	(void) argv;
	// The New 3DS's 804 MHz and L2 cache.
	osSetSpeedupEnable(true);
	displayInit();
	menuInit();
	logInit();
	logLine("armGBA-3DS %s on %s%s", ARMGBA_3DS_VERSION, isNew3ds() ? "New 3DS" : "Old 3DS", isEmulator() ? " (emulator)" : "");
	osSetSpeedupEnable(false);
	float const slow = measureCpuMhz();
	osSetSpeedupEnable(true);
	cpuMhz = measureCpuMhz();
	logLine("cpu: %.0f MHz with the New 3DS speed-up off, %.0f MHz with it on", slow, cpuMhz);
	extern u32 __ctru_heap_size;
	logLine("memory: %lu KiB of heap, %lu KiB linear", (unsigned long) __ctru_heap_size >> 10,
	        (unsigned long) __ctru_linear_heap_size >> 10);
	hidSetRepeatParameters(20, 4);
	configLoad();
	logUpload();
	scriptRead();
	gameCoreInit();
	char path[768];
	bool running = true;
	while (running && aptMainLoop()) {
		if (script.on && script.rom[0]) {
			snprintf(path, sizeof(path), "%s", script.rom);
		} else if (!menuBrowse(path, sizeof(path)) || script.quit) {
			break;
		}
		if (!gameLoad(path)) {
			menuMessage(gameLoadError(), 0xFFFFFFFF);
			if (script.on) {
				logLine("autotest done");
				break;
			}
			continue;
		}
		running = play() && !script.quit;
		menuBottomForGame(false);
		logLine("exit: closing the game (app %s)", aptShouldClose() ? "closed from HOME" : "running");
		gameUnload();
		logLine("exit: game closed");
		if (script.on && script.rom[0]) {
			break;
		}
	}
	logLine("exit: leaving, saving the settings");
	configSave();
	gameCoreExit();
	logLine("exit: core shut down");
	// Whatever closed the game, the code memory goes back to read-write before the app ends: a
	// process left with read-write-execute memory hangs the system on "Closing software".
	extern void jitHostRestoreMemory(void);
	jitHostRestoreMemory();
	// After the game's save is written: a test runner may stop the emulator as soon as it reads this.
	if (script.on) {
		logLine("autotest done");
	}
	logLine("exit: stopping the screens");
	displayExit();
	logLine("exit: done");
	logClose();
	return 0;
}
