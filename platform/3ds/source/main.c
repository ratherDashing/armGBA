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


// --- Test scripts (sdmc:/3ds/armgba/autotest.txt) -----------------------------------------------
// For running in an emulator without hands: loads a game, presses buttons, takes screenshots, logs
// speed, then exits. One command a line:
//   rom=<path>              the game
//   frames=<n>              exit after this many frames
//   unthrottled=1           don't wait for the screen: measures how fast the emulation runs
//   press=<frame>:<buttons>:<frames>   buttons (A,B,X,Y,L,R,START,SELECT,UP,DOWN,LEFT,RIGHT, joined by +)
//   shot=<frame>            the core's frame as shot_<frame>.bmp and both screens as screen_<frame>.bmp
//   savestate=<frame>:<slot> / loadstate=<frame>:<slot>
//   option=<key>=<value>    a core option
// and, counted in frames of the whole app (the browser and menus too) rather than of the game:
//   uipress=<frame>:<buttons>:<frames>, uitouch=<frame> (a touch of the bottom screen),
//   uishot=<frame> (both screens as ui_<frame>.bmp), uiexit=<frame>
// With no rom=, the browser picks the game. It ends with "autotest done" in log.txt.

enum {
	MAX_STEPS = 128,
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
		logLine("autotest done");
		script.quit = true;
	}
}

// --- The game loop ------------------------------------------------------------------------------

// Runs the game until it is closed; false when the app should exit.
static bool play(void) {
	uint64_t const start = osGetTime();
	uint64_t emulated = 0;
	uint32_t lastReport = 0;
	uint64_t reportTime = start, reportEmulated = 0;
	while (aptMainLoop() && !script.quit) {
		inputPoll();
		if (keysDownNow & KEY_TOUCH) {
			gameSaveSram(true);
			enum MenuAction action = menuPause();
			if (action != MENU_RESUME) {
				return action == MENU_QUIT_GAME;
			}
			continue;
		}
		if (script.on) {
			scriptFrame(gameFrames);
			if (gameFrames >= script.frames) {
				float const seconds = (osGetTime() - start) / 1000.0f;
				logLine("autotest: %lu frames in %.2f s, %.1f fps; emulation alone %.1f fps", gameFrames, seconds, gameFrames / seconds,
				     gameFrames * 1000.0f / (float) (emulated ? emulated : 1));
				logLine("autotest done");
				return false;
			}
		}
		// Fast-forward (ZL / ZR held, or the core's own turbo): several frames to each one shown.
		int const runs = gameFastForward() ? 4 : 1;
		for (int i = 0; i < runs; ++i) {
			uint64_t const before = svcGetSystemTick();
			gameRunFrame();
			emulated += (svcGetSystemTick() - before) / CPU_TICKS_PER_MSEC;
		}
		if (!script.unthrottled || gameFrames % 30 == 0) {
			displayPresent();
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
	logLine("armGBA %s on %s%s", ARMGBA_3DS_VERSION, isNew3ds() ? "New 3DS" : "Old 3DS", isEmulator() ? " (emulator)" : "");
	hidSetRepeatParameters(20, 4);
	configLoad();
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
			menuMessage("Couldn't open that game", 240);
			if (script.on) {
				logLine("autotest done");
				break;
			}
			continue;
		}
		running = play() && !script.quit;
		gameUnload();
		if (script.on && script.rom[0]) {
			break;
		}
	}
	configSave();
	gameCoreExit();
	logLine("exit");
	logClose();
	displayExit();
	return 0;
}
