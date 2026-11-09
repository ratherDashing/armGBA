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
static char message[COLS * 4 + 1];
static unsigned messageFrames;

#define CLEAR "\x1b[2J"
#define AT(row, col) "\x1b[" #row ";" #col "H"
#define INVERT "\x1b[7m"
#define DIM "\x1b[2m"
#define RESET "\x1b[0m"

// The console draws into this, and it's copied to the screen whole once drawn: drawn straight onto
// the screen, a page cleared then redrawn flashed while scrolling.
static u16 back[320 * 240];

void menuInit(void) {
	consoleInit(GFX_BOTTOM, &console);
	console.frameBuffer = back;
	consoleSelect(&console);
}

static void show(void) {
	u16* screen = (u16*) gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
	memcpy(screen, back, sizeof(back));
	GSPGPU_FlushDataCache(screen, sizeof(back));
}

void menuMessage(const char* text, unsigned frames) {
	snprintf(message, sizeof(message), "%s", text);
	messageFrames = frames ? frames : 180;
}

// One line of the list, cut or padded to the screen's width.
static void line(int row, bool selected, const char* format, ...) __attribute__((format(printf, 3, 4)));
static void line(int row, bool selected, const char* format, ...) {
	char text[512];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	// Cut and padded to the width in visible characters: escape sequences take none.
	char out[600];
	size_t o = 0;
	int visible = 0;
	for (const char* p = text; *p && o < sizeof(out) - 8; ++p) {
		if (*p == '\x1b') {
			while (*p && o < sizeof(out) - 8) {
				out[o++] = *p;
				if (*p >= 'A' && *p <= 'z' && *p != '[') {
					break;
				}
				++p;
			}
			if (!*p) {
				break;
			}
			continue;
		}
		if (visible < COLS) {
			out[o++] = *p;
			++visible;
		}
	}
	while (visible++ < COLS && o < sizeof(out) - 8) {
		out[o++] = ' ';
	}
	out[o] = 0;
	printf("\x1b[%d;1H%s%s" RESET, row + 1, selected ? INVERT : "", out);
}

// Waits for the next frame, keeping the top screen drawn; false when the app should close.
static bool frame(void) {
	if (!aptMainLoop() || scriptQuitting()) {
		return false;
	}
	show();
	displayPresent();
	inputPoll();
	return true;
}

static void wrapped(int row, int rows, const char* text);
static int wrapLines(int row, int rows, const char* style, const char* text);
static void wrappedText(int row, int rows, const char* text) {
	wrapped(row, rows, text);
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
		if (down && messageFrames) {
			messageFrames = 0;
			redraw = true;
		}
		if (down & KEY_START) {
			return false;
		}
		if (down & KEY_Y) {
			menuSettings();
			redraw = true;
			continue;
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
				printf(CLEAR);
				wrapLines(13, 4, "", entries[selected].name);
				line(18, false, DIM "Loading...");
				show();
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
			if (messageFrames && message[0]) {
				wrappedText(ROWS - 4, 3, message);
			}
			line(ROWS - 1, false, DIM "A open  B back  Y settings  START exit");
		}
	}
	return false;
}

// --- Settings ---------------------------------------------------------------------------------

static const char* const scaleNames[SCALE_COUNT] = { "Pixel for pixel", "Fit", "Stretch", "LCD3x (wide)" };

// Prints text wrapped to the screen's width from row on, at most rows lines.
// Text in up to rows lines, broken between words; the number of lines it took.
static int wrapLines(int row, int rows, const char* style, const char* text) {
	const char* p = text ? text : "";
	int r = 0;
	for (; r < rows && *p; ++r) {
		while (*p == ' ') {
			++p;
		}
		int length = (int) strlen(p);
		if (length > COLS) {
			length = COLS;
			while (length > 0 && p[length] != ' ') {
				--length;
			}
			if (!length) {
				length = COLS;
			}
		}
		line(row + r, false, "%s%.*s", style, length, p);
		p += length;
	}
	return r;
}

static void wrapped(int row, int rows, const char* text) {
	wrapLines(row, rows, DIM, text);
}

static int visibleIn(const char* category) {
	int n = 0;
	for (int i = 0; i < feOptionCount; ++i) {
		n += feOptions[i].visible && !strcmp(feOptions[i].category, category);
	}
	return n;
}

static const char* categoryInfo(const char* key) {
	for (int i = 0; i < feCategoryCount; ++i) {
		if (!strcmp(feCategories[i].key, key)) {
			return feCategories[i].info;
		}
	}
	return "";
}

// The 3DS's own settings: how the picture fills the top screen.
static void screenPage(void) {
	int selected = 0;
	bool redraw = true;
	while (frame()) {
		u32 const down = keysDownNow, repeat = keysRepeatNow;
		if (down & KEY_B) {
			configSave();
			return;
		}
		if (repeat & (KEY_UP | KEY_DOWN)) {
			selected ^= 1;
			redraw = true;
		} else if (repeat & (KEY_LEFT | KEY_RIGHT | KEY_A)) {
			int const step = repeat & KEY_LEFT ? -1 : 1;
			if (selected == 0) {
				config.scale = (config.scale + SCALE_COUNT + step) % SCALE_COUNT;
			} else {
				config.filter = !config.filter;
			}
			redraw = true;
		}
		if (redraw) {
			redraw = false;
			printf(CLEAR);
			line(0, false, "Settings > 3DS Screen");
			line(2, selected == 0, "Screen");
			line(3, selected == 0, "    < %s >", scaleNames[config.scale]);
			line(4, selected == 1, "Smoothing");
			line(5, selected == 1, "    < %s >", config.filter ? "On" : "Off");
			static const char* const help[2] = {
				"Pixel for pixel: the game's own size. Fit: as large as fits. Stretch: the whole screen. LCD3x: lcd3x's "
				"subpixel stripes and scanlines, three screen columns to a pixel on the top screen's 800-column mode "
				"(not on the original 2DS).",
				"Smooths the picture when it's scaled (Fit and Stretch).",
			};
			wrapped(18, 10, help[selected]);
			line(ROWS - 1, false, DIM "</> change  B back");
		}
	}
}

// One category's options: each with its value, and the selected one's help text below.
static void categoryPage(const char* category, const char* title) {
	int selected = 0, scroll = 0;
	bool redraw = true;
	enum { PER_PAGE = 7 };
	while (frame()) {
		int shown[160];
		int count = 0;
		for (int i = 0; i < feOptionCount && count < 160; ++i) {
			if (feOptions[i].visible && !strcmp(feOptions[i].category, category)) {
				shown[count++] = i;
			}
		}
		if (!count) {
			return;
		}
		if (selected >= count) {
			selected = count - 1;
		}
		u32 const down = keysDownNow, repeat = keysRepeatNow;
		if (down & KEY_B) {
			configSave();
			return;
		}
		struct CoreOption* o = &feOptions[shown[selected]];
		int step = 0;
		if (repeat & KEY_DOWN) {
			selected = (selected + 1) % count;
			redraw = true;
		} else if (repeat & KEY_UP) {
			selected = (selected + count - 1) % count;
			redraw = true;
		} else if (repeat & (KEY_RIGHT | KEY_A)) {
			step = 1;
		} else if (repeat & KEY_LEFT) {
			step = -1;
		} else if (repeat & KEY_R) {
			step = 10;
		} else if (repeat & KEY_L) {
			step = -10;
		} else if ((down & KEY_X) && o->current != o->defaultIndex) {
			o->current = o->defaultIndex;
			gameOptionsChanged();
			redraw = true;
		}
		if (step && o->valueCount > 1) {
			int const n = o->valueCount;
			o->current = ((o->current + step) % n + n) % n;
			gameOptionsChanged();
			redraw = true;
		}
		if (redraw) {
			// A change can show or hide other options: the list again, as it is now.
			count = 0;
			for (int i = 0; i < feOptionCount && count < 160; ++i) {
				if (feOptions[i].visible && !strcmp(feOptions[i].category, category)) {
					shown[count++] = i;
				}
			}
			if (!count) {
				return;
			}
			if (selected >= count) {
				selected = count - 1;
			}
		}
		if (selected < scroll) {
			scroll = selected;
		} else if (selected >= scroll + PER_PAGE) {
			scroll = selected - PER_PAGE + 1;
		}
		if (redraw) {
			redraw = false;
			printf(CLEAR);
			line(0, false, "Settings > %.28s", title);
			for (int r = 0; r < PER_PAGE && scroll + r < count; ++r) {
				const struct CoreOption* p = &feOptions[shown[scroll + r]];
				bool const sel = scroll + r == selected;
				line(2 + r * 2, sel, "%s", p->label);
				line(3 + r * 2, sel, "    < %s >%s", p->valueLabels[p->current], p->current == p->defaultIndex ? "" : " *");
			}
			line(16, false, DIM "%d of %d%s", selected + 1, count, count > PER_PAGE ? "  (more: up/down)" : "");
			o = &feOptions[shown[selected]];
			wrapped(18, 10, o->info && *o->info ? o->info : categoryInfo(category));
			line(ROWS - 1, false, DIM "</> change  L/R x10  X default  B back");
		}
	}
}

// The core's categories under a few headed sections, each shown without the prefix its section
// already says ("Video: LCD Effects" under Display is "LCD Effects").
static const struct {
	const char* title;
	const char* keys[6];
} sections[] = {
	{ "Display", { "", "video", "video_lcd" } },
	{ "Game", { "system", "clock", "audio", "input" } },
	{ "Saves", { "saves", "saves_settings" } },
	{ "Link", { "link", "link_players", "link_connection", "link_screens" } },
};

static const char* shortLabel(const char* label) {
	const char* colon = strstr(label, ": ");
	return colon ? colon + 2 : label;
}

static bool sectioned(const char* key) {
	for (size_t s = 0; s < sizeof(sections) / sizeof(*sections); ++s) {
		for (int k = 0; k < 6 && sections[s].keys[k]; ++k) {
			if (!strcmp(sections[s].keys[k], key)) {
				return true;
			}
		}
	}
	return false;
}

void menuSettings(void) {
	int selected = 0;
	bool redraw = true;
	gameRefreshOptionDisplay();
	while (frame()) {
		// Rows: a section's title (key NULL, not selectable), then its pages. "" is the 3DS Screen page.
		struct {
			const char* key;
			const char* label;
			int count;
			bool title;
		} rows[48];
		int n = 0;
		for (size_t sec = 0; sec <= sizeof(sections) / sizeof(*sections) && n < 44; ++sec) {
			bool const other = sec == sizeof(sections) / sizeof(*sections);
			int const titleRow = n;
			rows[n++] = (typeof(rows[0])) { NULL, other ? "Other" : sections[sec].title, 0, true };
			if (!other) {
				for (int k = 0; k < 6 && sections[sec].keys[k] && n < 46; ++k) {
					const char* key = sections[sec].keys[k];
					if (!*key) {
						rows[n++] = (typeof(rows[0])) { "", "3DS Screen", 2, false };
						continue;
					}
					for (int c = 0; c < feCategoryCount; ++c) {
						int const v = visibleIn(key);
						if (!strcmp(feCategories[c].key, key) && v) {
							rows[n++] = (typeof(rows[0])) { feCategories[c].key, shortLabel(feCategories[c].label), v, false };
						}
					}
				}
			} else {
				for (int c = 0; c < feCategoryCount && n < 46; ++c) {
					int const v = visibleIn(feCategories[c].key);
					if (v && !sectioned(feCategories[c].key)) {
						rows[n++] = (typeof(rows[0])) { feCategories[c].key, feCategories[c].label, v, false };
					}
				}
			}
			// A section with nothing showing has no title.
			if (n == titleRow + 1) {
				--n;
			}
		}
		if (selected >= n || rows[selected].title) {
			selected = 1;
		}
		u32 const down = keysDownNow, repeat = keysRepeatNow;
		if (down & KEY_B) {
			configSave();
			return;
		}
		if (repeat & (KEY_DOWN | KEY_UP)) {
			int const step = repeat & KEY_DOWN ? 1 : n - 1;
			do {
				selected = (selected + step) % n;
			} while (rows[selected].title);
			redraw = true;
		} else if (down & KEY_A) {
			if (!*rows[selected].key) {
				screenPage();
			} else {
				char key[32], label[48];
				snprintf(key, sizeof(key), "%s", rows[selected].key);
				snprintf(label, sizeof(label), "%s", rows[selected].label);
				categoryPage(key, label);
			}
			gameRefreshOptionDisplay();
			redraw = true;
			continue;
		}
		if (redraw) {
			redraw = false;
			printf(CLEAR);
			line(0, false, "Settings");
			for (int i = 0; i < n && i < 20; ++i) {
				if (rows[i].title) {
					line(1 + i, false, "\x1b[4m%s", rows[i].label);
				} else {
					line(1 + i, i == selected, "   %-29.29s %3d", rows[i].label, rows[i].count);
				}
			}
			wrapped(22, 6, !*rows[selected].key ? "How the picture fills the top screen, including LCD3x." : categoryInfo(rows[selected].key));
			line(ROWS - 1, false, DIM "A open  B back");
		}
	}
}

// --- The pause menu ---------------------------------------------------------------------------

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
				menuSettings();
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
			line(9, selected == ITEM_OPTIONS, "  Settings (all options)...");
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

// The bottom screen during a game: a few lines, redrawn once a second. Where the time goes, for
// finding out what keeps a game from full speed on a console.
void menuInGame(void) {
	static uint64_t lastTime, lastFlushTicks, lastCompiled, lastInvalidations, lastPatches;
	static uint32_t lastFrames, lastFlushes;
	static float fps, flushMs;
	static uint32_t flushes, compiled, invalidations, patches;
	uint64_t const now = osGetTime();
	bool redraw = false;
	if (now - lastTime >= 1000) {
		float const seconds = (now - lastTime) / 1000.0f;
		uint32_t const frames = gameFrames - lastFrames;
		uint64_t c, i, p;
		jitCounts(&c, &i, &p);
		fps = frames / seconds;
		flushes = (uint32_t) ((jitFlushes - lastFlushes) / seconds);
		flushMs = frames ? (float) (jitFlushTicks - lastFlushTicks) / CPU_TICKS_PER_MSEC / frames : 0;
		compiled = (uint32_t) ((c - lastCompiled) / seconds);
		invalidations = (uint32_t) ((i - lastInvalidations) / seconds);
		patches = (uint32_t) ((p - lastPatches) / seconds);
		lastTime = now;
		lastFrames = gameFrames;
		lastFlushes = jitFlushes;
		lastFlushTicks = jitFlushTicks;
		lastCompiled = c;
		lastInvalidations = i;
		lastPatches = p;
		redraw = true;
	}
	if (messageFrames && !--messageFrames) {
		redraw = true;
	}
	static bool messageShown;
	if (messageFrames && !messageShown) {
		redraw = true;
	}
	messageShown = messageFrames != 0;
	if (!redraw) {
		return;
	}
	printf(CLEAR);
	int row = wrapLines(0, 3, "", gameTitle()) + 1;
	line(row++, false, "%.1f fps (the GBA's is 59.7)", fps);
	line(row++, false, DIM "emulation %.1f ms/frame (16.7 max)", perfEmulationMs);
	line(row++, false, DIM "  cache flushes %.2f ms/frame", flushMs);
	line(row++, false, DIM "sound queue %u%%", soundOccupancy());
	line(row++, false, DIM "recompiler, a second:");
	line(row++, false, DIM "  %lu blocks compiled", (unsigned long) compiled);
	line(row++, false, DIM "  %lu thrown out by code writes", (unsigned long) invalidations);
	line(row++, false, DIM "  %lu patched, %lu cache flushes", (unsigned long) patches, (unsigned long) flushes);
	line(row++, false, DIM "%s at %.0f MHz, picture drawn", isNew3ds() ? "New 3DS" : "Old 3DS", cpuMhz);
	line(row++, false, DIM "  %s",
	     renderThreadCore == 2 ? "on core 2" : renderThreadCore == 1 ? "on core 1" : "with the game");
	if (messageFrames) {
		wrapped(row + 1, 3, message);
	}
	line(ROWS - 1, false, DIM "Touch the screen for the menu");
	show();
}
