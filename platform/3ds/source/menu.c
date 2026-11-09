/* Copyright (c) 2026 armGBA 3DS port contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The bottom screen, a text console: the game browser, the menu a touch opens during a game, and the
// core's options.

#include "frontend.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

enum {
	COLS = 40,
	ROWS = 30,
	LIST_ROWS = 24,
};

static PrintConsole console;
static char message[COLS * 2 + 1];
static unsigned messageFrames;

#define CLEAR "\x1b[2J"
#define AT(row, col) "\x1b[" #row ";" #col "H"
#define INVERT "\x1b[7m"
#define DIM "\x1b[2m"
#define RESET "\x1b[0m"

void menuInit(void) {
	consoleInit(GFX_BOTTOM, &console);
	consoleSelect(&console);
}

void menuMessage(const char* text, unsigned frames) {
	snprintf(message, sizeof(message), "%s", text);
	messageFrames = frames ? frames : 180;
}

// One line of the list, cut or padded to the screen's width.
static void line(int row, bool selected, const char* format, ...) __attribute__((format(printf, 3, 4)));
static void line(int row, bool selected, const char* format, ...) {
	char text[256];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	printf("\x1b[%d;1H%s%-*.*s" RESET, row + 1, selected ? INVERT : "", COLS, COLS, text);
}

// Waits for the next frame, keeping the top screen drawn; false when the app should close.
static bool frame(void) {
	if (!aptMainLoop() || scriptQuitting()) {
		return false;
	}
	displayPresent();
	inputPoll();
	return true;
}

// --- The game browser -------------------------------------------------------------------------

struct Entry {
	char name[256];
	bool dir;
};

static struct Entry* entries;
static int entryCount;

static bool isGameFile(const char* name) {
	const char* dot = strrchr(name, '.');
	if (!dot) {
		return false;
	}
	static const char* const extensions[] = { ".gba", ".agb", ".gb", ".gbc", ".sgb", ".bin", ".zip", ".7z" };
	for (size_t i = 0; i < sizeof(extensions) / sizeof(*extensions); ++i) {
		if (!strcasecmp(dot, extensions[i])) {
			return true;
		}
	}
	return false;
}

static int compareEntries(const void* a, const void* b) {
	const struct Entry* x = a;
	const struct Entry* y = b;
	if (x->dir != y->dir) {
		return x->dir ? -1 : 1;
	}
	return strcasecmp(x->name, y->name);
}

static bool listDir(const char* path) {
	DIR* d = opendir(path);
	if (!d) {
		return false;
	}
	int capacity = 256;
	free(entries);
	entries = malloc(capacity * sizeof(*entries));
	entryCount = 0;
	struct dirent* e;
	while (entries && (e = readdir(d))) {
		if (e->d_name[0] == '.') {
			continue;
		}
		bool const dir = e->d_type == DT_DIR;
		if (!dir && !isGameFile(e->d_name)) {
			continue;
		}
		if (entryCount == capacity) {
			capacity *= 2;
			struct Entry* bigger = realloc(entries, capacity * sizeof(*entries));
			if (!bigger) {
				break;
			}
			entries = bigger;
		}
		snprintf(entries[entryCount].name, sizeof(entries[0].name), "%s", e->d_name);
		entries[entryCount].dir = dir;
		++entryCount;
	}
	closedir(d);
	qsort(entries, entryCount, sizeof(*entries), compareEntries);
	return true;
}

static void parentOf(char* path) {
	char* slash = strrchr(path, '/');
	if (slash && slash > strchr(path, '/')) {
		*slash = 0;
	} else if (slash) {
		slash[1] = 0;
	}
}

static void joinPath(char* out, size_t size, const char* dir, const char* name) {
	size_t const length = strlen(dir);
	snprintf(out, size, "%s%s%s", dir, length && dir[length - 1] == '/' ? "" : "/", name);
}

bool menuBrowse(char* path, size_t size) {
	char dir[512];
	snprintf(dir, sizeof(dir), "%s", config.lastDir);
	if (!listDir(dir)) {
		snprintf(dir, sizeof(dir), "sdmc:/");
		listDir(dir);
	}
	int selected = 0, scroll = 0;
	bool redraw = true;
	while (frame()) {
		u32 const down = keysDownNow;
		u32 const repeat = keysRepeatNow;
		if (down & KEY_START) {
			return false;
		}
		if (repeat & KEY_DOWN) {
			selected = entryCount ? (selected + 1) % entryCount : 0;
			redraw = true;
		} else if (repeat & KEY_UP) {
			selected = entryCount ? (selected + entryCount - 1) % entryCount : 0;
			redraw = true;
		} else if (repeat & KEY_RIGHT) {
			selected = selected + LIST_ROWS < entryCount ? selected + LIST_ROWS : (entryCount ? entryCount - 1 : 0);
			redraw = true;
		} else if (repeat & KEY_LEFT) {
			selected = selected > LIST_ROWS ? selected - LIST_ROWS : 0;
			redraw = true;
		} else if (down & KEY_B) {
			char before[256];
			const char* slash = strrchr(dir, '/');
			snprintf(before, sizeof(before), "%s", slash ? slash + 1 : "");
			parentOf(dir);
			listDir(dir);
			selected = 0;
			for (int i = 0; i < entryCount; ++i) {
				if (!strcmp(entries[i].name, before)) {
					selected = i;
				}
			}
			redraw = true;
		} else if ((down & KEY_A) && entryCount) {
			char next[768];
			joinPath(next, sizeof(next), dir, entries[selected].name);
			if (entries[selected].dir) {
				if (strlen(next) < sizeof(dir)) {
					snprintf(dir, sizeof(dir), "%s", next);
					listDir(dir);
					selected = 0;
					redraw = true;
				}
			} else {
				snprintf(config.lastDir, sizeof(config.lastDir), "%s", dir);
				configSave();
				snprintf(path, size, "%s", next);
				printf(CLEAR AT(14, 1) "  Loading %.30s...", entries[selected].name);
				return true;
			}
		}
		if (selected < scroll) {
			scroll = selected;
		} else if (selected >= scroll + LIST_ROWS) {
			scroll = selected - LIST_ROWS + 1;
		}
		if (redraw) {
			redraw = false;
			printf(CLEAR);
			line(0, false, "armGBA %s", ARMGBA_3DS_VERSION);
			line(1, false, DIM "%.40s", dir);
			for (int r = 0; r < LIST_ROWS; ++r) {
				int i = scroll + r;
				if (i < entryCount) {
					line(r + 3, i == selected, "%s%s", entries[i].name, entries[i].dir ? "/" : "");
				}
			}
			if (!entryCount) {
				line(4, false, "  No games here. Put them in sdmc:/roms");
			}
			line(ROWS - 1, false, DIM "A open  B back  </> page  START exit");
		}
	}
	return false;
}

// --- Core options -----------------------------------------------------------------------------

static void optionsMenu(void) {
	int selected = 0, scroll = 0;
	bool redraw = true;
	int shown[256];
	while (frame()) {
		int count = 0;
		for (int i = 0; i < feOptionCount && count < 256; ++i) {
			if (feOptions[i].visible && feOptions[i].valueCount > 1) {
				shown[count++] = i;
			}
		}
		if (!count) {
			return;
		}
		if (selected >= count) {
			selected = count - 1;
		}
		u32 const down = keysDownNow;
		u32 const repeat = keysRepeatNow;
		if (down & KEY_B) {
			configSave();
			return;
		}
		if (repeat & KEY_DOWN) {
			selected = (selected + 1) % count;
			redraw = true;
		} else if (repeat & KEY_UP) {
			selected = (selected + count - 1) % count;
			redraw = true;
		} else if (repeat & (KEY_LEFT | KEY_RIGHT | KEY_A)) {
			struct CoreOption* o = &feOptions[shown[selected]];
			int const step = repeat & KEY_LEFT ? o->valueCount - 1 : 1;
			o->current = (o->current + step) % o->valueCount;
			feOptionsChanged = true;
			redraw = true;
		} else if (down & KEY_X) {
			struct CoreOption* o = &feOptions[shown[selected]];
			o->current = o->defaultIndex;
			feOptionsChanged = true;
			redraw = true;
		}
		int const rows = 12;
		if (selected < scroll) {
			scroll = selected;
		} else if (selected >= scroll + rows) {
			scroll = selected - rows + 1;
		}
		if (redraw) {
			redraw = false;
			printf(CLEAR);
			line(0, false, "Core options");
			for (int r = 0; r < rows && scroll + r < count; ++r) {
				const struct CoreOption* o = &feOptions[shown[scroll + r]];
				bool const sel = scroll + r == selected;
				line(2 + r * 2, sel, "%s", o->label);
				line(3 + r * 2, sel, "    < %s >%s", o->valueLabels[o->current], o->current == o->defaultIndex ? "" : " *");
			}
			line(ROWS - 1, false, DIM "</> change  X default  B back");
		}
	}
}

// --- The pause menu ---------------------------------------------------------------------------

static const char* const scaleNames[SCALE_COUNT] = { "Pixel for pixel", "Fit", "Stretch" };

enum MenuAction menuPause(void) {
	enum {
		ITEM_RESUME, ITEM_SAVE, ITEM_LOAD, ITEM_SLOT, ITEM_SCALE, ITEM_FILTER, ITEM_OPTIONS, ITEM_RESET, ITEM_QUIT, ITEM_EXIT, ITEMS,
	};
	static int slot;
	int selected = 0;
	char status[COLS + 1] = "";
	bool redraw = true;
	soundPause(true);
	// The touch that opened the menu shouldn't count.
	while (frame()) {
		u32 const down = keysDownNow;
		u32 const repeat = keysRepeatNow;
		if (down & KEY_B) {
			break;
		}
		if (repeat & KEY_DOWN) {
			selected = (selected + 1) % ITEMS;
			redraw = true;
		} else if (repeat & KEY_UP) {
			selected = (selected + ITEMS - 1) % ITEMS;
			redraw = true;
		} else if (repeat & (KEY_LEFT | KEY_RIGHT)) {
			int const step = repeat & KEY_LEFT ? -1 : 1;
			if (selected == ITEM_SLOT) {
				slot = (slot + 10 + step) % 10;
			} else if (selected == ITEM_SCALE) {
				config.scale = (config.scale + SCALE_COUNT + step) % SCALE_COUNT;
				configSave();
			} else if (selected == ITEM_FILTER) {
				config.filter = !config.filter;
				configSave();
			}
			redraw = true;
		} else if (down & KEY_A) {
			switch (selected) {
			case ITEM_RESUME:
				soundPause(false);
				return MENU_RESUME;
			case ITEM_SAVE:
				snprintf(status, sizeof(status), gameSaveState(slot) ? "Saved to slot %d" : "Couldn't save slot %d", slot);
				break;
			case ITEM_LOAD:
				if (gameLoadState(slot)) {
					soundPause(false);
					menuMessage("State loaded", 120);
					return MENU_RESUME;
				}
				snprintf(status, sizeof(status), "Couldn't load slot %d", slot);
				break;
			case ITEM_OPTIONS:
				optionsMenu();
				break;
			case ITEM_RESET:
				gameReset();
				soundPause(false);
				return MENU_RESUME;
			case ITEM_QUIT:
				return MENU_QUIT_GAME;
			case ITEM_EXIT:
				return MENU_EXIT_APP;
			case ITEM_SCALE:
				config.scale = (config.scale + 1) % SCALE_COUNT;
				configSave();
				break;
			case ITEM_FILTER:
				config.filter = !config.filter;
				configSave();
				break;
			default:
				break;
			}
			redraw = true;
		}
		if (redraw) {
			redraw = false;
			printf(CLEAR);
			line(0, false, "%.40s", gameTitle());
			line(2, selected == ITEM_RESUME, "  Resume");
			line(3, selected == ITEM_SAVE, "  Save state");
			line(4, selected == ITEM_LOAD, "  Load state%s", gameStateExists(slot) ? "" : DIM " (empty)");
			line(5, selected == ITEM_SLOT, "  Slot            < %d >", slot);
			line(7, selected == ITEM_SCALE, "  Screen          < %s >", scaleNames[config.scale]);
			line(8, selected == ITEM_FILTER, "  Smoothing       < %s >", config.filter ? "On" : "Off");
			line(9, selected == ITEM_OPTIONS, "  Core options...");
			line(11, selected == ITEM_RESET, "  Restart game");
			line(12, selected == ITEM_QUIT, "  Close game");
			line(13, selected == ITEM_EXIT, "  Exit armGBA");
			line(16, false, "%s", status);
			line(ROWS - 1, false, DIM "A choose  B back to the game");
		}
	}
	soundPause(false);
	return aptMainLoop() ? MENU_RESUME : MENU_EXIT_APP;
}

// The bottom screen during a game: a line or two, redrawn only when they change.
void menuInGame(void) {
	static uint64_t lastTime;
	static uint32_t lastFrames;
	static char shown[COLS * 4];
	static float fps;
	uint64_t const now = osGetTime();
	if (now - lastTime >= 1000) {
		fps = (gameFrames - lastFrames) * 1000.0f / (float) (now - lastTime);
		lastTime = now;
		lastFrames = gameFrames;
	}
	if (messageFrames) {
		--messageFrames;
	}
	char text[COLS * 4];
	snprintf(text, sizeof(text), "%.40s|%.1f fps|%s", gameTitle(), fps, messageFrames ? message : "");
	if (strcmp(text, shown)) {
		snprintf(shown, sizeof(shown), "%s", text);
		printf(CLEAR);
		line(0, false, "%.40s", gameTitle());
		line(2, false, "%.1f fps%s", fps, isNew3ds() ? "" : "  (Old 3DS)");
		if (messageFrames) {
			line(4, false, "%.40s", message);
		}
		line(ROWS - 1, false, DIM "Touch the screen for the menu");
	}
}
