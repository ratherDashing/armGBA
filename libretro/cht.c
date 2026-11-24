/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "core.h"
#include "link.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

// RetroArch's cheat database: a .cht file for each game, in a folder for each system, named as
// No-Intro names the game, often with a tag for the kind of codes ("(GameShark)", "(Game Genie)",
// "(Code Breaker)"). Each cheat is a description and its codes, joined by +.
//
// The core can't ask RetroArch where the database is, so it reads RetroArch's own setting
// (cheat_database_path in retroarch.cfg) where RetroArch keeps its configuration, and otherwise looks
// where RetroArch keeps it by default.

static bool isFolder(const char* path) {
	struct stat info;
	return !stat(path, &info) && S_ISDIR(info.st_mode);
}

// Whether folder holds the database: its Game Boy folder.
static bool isDatabase(const char* folder) {
	char path[PATH_SIZE];
	snprintf(path, sizeof(path), "%s/Nintendo - Game Boy", folder);
	return isFolder(path);
}

// cheat_database_path from the retroarch.cfg in folder, with ~ for the home folder.
static bool configuredDatabase(const char* folder, char* out, size_t outSize) {
	char path[PATH_SIZE + 16];
	snprintf(path, sizeof(path), "%.*s/retroarch.cfg", PATH_SIZE - 1, folder);
	FILE* f = fopen(path, "r");
	if (!f) {
		return false;
	}
	char line[PATH_SIZE + 64];
	bool found = false;
	while (!found && fgets(line, sizeof(line), f)) {
		char* p = line;
		while (*p == ' ' || *p == '\t') {
			++p;
		}
		if (strncmp(p, "cheat_database_path", 19)) {
			continue;
		}
		char* const open = strchr(p, '"');
		char* const close = open ? strrchr(open + 1, '"') : NULL;
		if (!close) {
			continue;
		}
		*close = '\0';
		const char* value = open + 1;
		const char* const home = getenv("HOME");
		if (value[0] == '~' && home) {
			snprintf(out, outSize, "%s%s", home, value + 1);
		} else {
			snprintf(out, outSize, "%s", value);
		}
		found = *out != '\0';
	}
	fclose(f);
	return found;
}

bool chtDatabase(const char* systemFolder, char* out, size_t outSize) {
	const char* const home = getenv("HOME");
	const char* const xdg = getenv("XDG_CONFIG_HOME");
	char configs[6][PATH_SIZE];
	int n = 0;
	if (xdg && *xdg) {
		snprintf(configs[n++], PATH_SIZE, "%s/retroarch", xdg);
	}
	if (home) {
		snprintf(configs[n++], PATH_SIZE, "%s/.config/retroarch", home);
		snprintf(configs[n++], PATH_SIZE, "%s/.var/app/org.libretro.RetroArch/config/retroarch", home);
	}
	snprintf(configs[n++], PATH_SIZE, "/storage/.config/retroarch");
	if (systemFolder && *systemFolder) {
		// RetroArch's own layout keeps the system folder beside its configuration.
		snprintf(configs[n], PATH_SIZE, "%s", systemFolder);
		char* const slash = strrchr(configs[n], '/');
		if (slash) {
			*slash = '\0';
			++n;
		}
	}
	for (int i = 0; i < n; ++i) {
		if (configuredDatabase(configs[i], out, outSize) && isDatabase(out)) {
			return true;
		}
	}
	// Where RetroArch keeps it by default: ROCKNIX's, and the home folder's
	const char* const defaults[] = { "/tmp/database/cht", "/usr/share/libretro-database/cht", "/usr/share/retroarch/database/cht" };
	for (size_t i = 0; i < sizeof(defaults) / sizeof(*defaults); ++i) {
		if (isDatabase(defaults[i])) {
			snprintf(out, outSize, "%s", defaults[i]);
			return true;
		}
	}
	for (int i = 0; home && i < 2; ++i) {
		snprintf(out, outSize, i ? "%s/.var/app/org.libretro.RetroArch/config/retroarch/cheats" : "%s/.config/retroarch/cheats", home);
		if (isDatabase(out)) {
			return true;
		}
	}
	return false;
}

// A No-Intro name: the title, before the first " (", and the regions, the first group's.
struct Name {
	char title[256];
	char regions[128];
};

static void nameOf(const char* fileName, struct Name* name) {
	snprintf(name->title, sizeof(name->title), "%s", fileName);
	char* const dot = strrchr(name->title, '.');
	if (dot) {
		*dot = '\0';
	}
	name->regions[0] = '\0';
	char* const group = strstr(name->title, " (");
	if (!group) {
		return;
	}
	*group = '\0';
	snprintf(name->regions, sizeof(name->regions), "%s", group + 2);
	char* const end = strchr(name->regions, ')');
	if (end) {
		*end = '\0';
	}
}

// Whether two lists of regions ("USA, Europe") share one.
static bool regionsMeet(const char* a, const char* b) {
	if (!*a || !*b) {
		return !*a && !*b;
	}
	char copy[128];
	snprintf(copy, sizeof(copy), "%s", a);
	for (char* region = strtok(copy, ","); region; region = strtok(NULL, ",")) {
		while (*region == ' ') {
			++region;
		}
		size_t const length = strlen(region);
		for (const char* at = strstr(b, region); at; at = strstr(at + 1, region)) {
			bool const starts = at == b || at[-1] == ' ' || at[-1] == ',';
			bool const ends = at[length] == '\0' || at[length] == ',';
			if (starts && ends) {
				return true;
			}
		}
	}
	return false;
}

// The value of a line `key = "value"`.
static bool quoted(char* line, char** value) {
	char* const open = strchr(line, '"');
	char* const close = open ? strrchr(open + 1, '"') : NULL;
	if (!close) {
		return false;
	}
	*close = '\0';
	*value = open + 1;
	return true;
}

// The game's own name: inside an archive, the file's in it
static const char* ownName(const char* gamePath) {
	const char* file = strrchr(gamePath, '#');
	file = file ? file + 1 : gamePath;
	const char* const slash = strrchr(file, '/');
	return slash ? slash + 1 : file;
}

bool chtFileFits(const char* chtName, const char* gamePath) {
	struct Name game, cht;
	nameOf(ownName(gamePath), &game);
	nameOf(chtName, &cht);
	return !strcasecmp(cht.title, game.title) && regionsMeet(cht.regions, game.regions);
}

// An XX, YY, ZZ or ?? in a code's value (a CodeBreaker code's 4 digits, a GameShark code's second
// byte), what a database template leaves to fill, as 0; true if there was one. Anywhere else (an
// address), the code is left as it is, which accepts doesn't take.
static bool fillValue(char* code, bool (*accepts)(const char* code)) {
	size_t const length = strlen(code);
	size_t from, to;
	if (accepts == chtCodeBreakerPart && length == 4) {
		from = 0, to = 4;
	} else if (accepts == chtGameSharkCode && length == 8) {
		from = 2, to = 4;
	} else {
		return false;
	}
	bool mark = false;
	for (size_t i = 0; i < length; ++i) {
		bool const placeholder = strchr("XxYyZz?", code[i]) != NULL;
		if (placeholder && (i < from || i >= to)) {
			return false;
		}
		if (!placeholder && !isxdigit((unsigned char) code[i])) {
			return false;
		}
		mark |= placeholder;
	}
	for (size_t i = from; mark && i < to; ++i) {
		code[i] = strchr("XxYyZz?", code[i]) ? '0' : code[i];
	}
	return mark;
}

// Adds the cheats in one .cht file whose every code accepts takes, leaving out those already there; a
// CodeBreaker code written as 12 digits without its + is split in two, and a value left to fill is 0
// (*zeroed counts those cheats). *rejected counts the others with a code, Game Genie codes for a Game
// Boy GameShark's aside (the same cheats in its form).
static size_t readCheats(const char* path, unsigned file, bool (*accepts)(const char* code), struct ChtCheat* cheats, size_t count, size_t most,
                         size_t* rejected, size_t* zeroed) {
	FILE* f = fopen(path, "r");
	if (!f) {
		return count;
	}
	char line[1024];
	char description[sizeof(cheats->description)] = "";
	int describedAs = -1;
	while (count < most && fgets(line, sizeof(line), f)) {
		int index;
		char key[16];
		if (sscanf(line, "cheat%d_%15[a-z]", &index, key) != 2) {
			continue;
		}
		char* value;
		if (!quoted(line, &value)) {
			continue;
		}
		if (!strcmp(key, "desc")) {
			snprintf(description, sizeof(description), "%s", value);
			describedAs = index;
			continue;
		}
		if (strcmp(key, "code") || !*value) {
			continue;
		}
		// Codes are joined by +, and a CodeBreaker code's two parts sometimes by a space, or by nothing
		char normal[sizeof(cheats->code)];
		size_t used = 0;
		bool fits = strlen(value) < sizeof(normal);
		char codes[1024];
		snprintf(codes, sizeof(codes), "%s", value);
		bool genie = true;
		bool filled = false;
		for (char* code = strtok(codes, "+ "); fits && code; code = strtok(NULL, "+ ")) {
			genie &= chtGameGenieCode(code);
			filled |= fillValue(code, accepts);
			bool twelve = accepts == chtCodeBreakerPart && strlen(code) == 12;
			for (size_t i = 0; twelve && i < 12; ++i) {
				twelve = isxdigit((unsigned char) code[i]);
			}
			if (twelve) {
				used += (size_t) snprintf(normal + used, sizeof(normal) - used, "%s%.8s+%s", used ? "+" : "", code, code + 8);
			} else {
				fits = accepts(code);
				used += (size_t) snprintf(normal + used, sizeof(normal) - used, "%s%s", used ? "+" : "", code);
			}
			fits &= used < sizeof(normal);
		}
		if (!used) {
			continue;
		}
		if (!fits) {
			if (rejected && !(genie && accepts == chtGameSharkCode)) {
				++*rejected;
			}
			continue;
		}
		const char* const written = value;
		value = normal;
		bool known = false;
		for (size_t i = 0; i < count && !known; ++i) {
			known = !strcasecmp(cheats[i].code, value);
		}
		if (!fits || known) {
			continue;
		}
		snprintf(cheats[count].code, sizeof(cheats[count].code), "%s", value);
		snprintf(cheats[count].written, sizeof(cheats[count].written), "%.*s", (int) sizeof(cheats[count].written) - 1, written);
		snprintf(cheats[count].description, sizeof(cheats[count].description), "%.*s", (int) sizeof(cheats[count].description) - 1,
		         describedAs == index && *description ? description : value);
		cheats[count].file = file;
		++count;
		if (filled && zeroed) {
			++*zeroed;
		}
	}
	fclose(f);
	return count;
}

// The build a .cht file's codes are for: the game ID in its first CodeBreaker master code (type 0,
// 0000XXXX, a cheat's first code), or -1 without one, or with 0000 there, which many of RetroArch's
// files have in place of an ID.
static int masterId(const char* path) {
	FILE* f = fopen(path, "r");
	if (!f) {
		return -1;
	}
	int id = -1;
	char line[1024];
	while (id < 0 && fgets(line, sizeof(line), f)) {
		int index;
		char key[16];
		char* value;
		if (sscanf(line, "cheat%d_%15[a-z]", &index, key) != 2 || strcmp(key, "code") || !quoted(line, &value)) {
			continue;
		}
		// A cheat's first code only: a slide code's second line (type 4's) can read 0000xxxx too
		char* const code = strtok(value, "+ ");
		if (code && strlen(code) == 8 && !strncmp(code, "0000", 4) && chtCodeBreakerPart(code)) {
			id = (int) strtol(code + 4, NULL, 16);
			id = id ? id : -2;
		}
	}
	fclose(f);
	return id < 0 ? -1 : id;
}

size_t chtCheats(const char* database, bool advance, const char* gamePath, int gameId, int* otherId, bool (*accepts)(const char* code),
                 struct ChtCheat* cheats, size_t most, size_t* rejected, size_t* zeroed) {
	if (otherId) {
		*otherId = -1;
	}
	if (rejected) {
		*rejected = 0;
	}
	if (zeroed) {
		*zeroed = 0;
	}
	if (!database || !gamePath || !most) {
		return 0;
	}
	const char* const own = ownName(gamePath);
	static const char* const gameBoy[] = { "Nintendo - Game Boy", "Nintendo - Game Boy Color", NULL };
	static const char* const gameBoyAdvance[] = { "Nintendo - Game Boy Advance", NULL };
	size_t count = 0;
	unsigned files = 0;
	for (const char* const* system = advance ? gameBoyAdvance : gameBoy; *system; ++system) {
		char folder[PATH_SIZE];
		snprintf(folder, sizeof(folder), "%s/%s", database, *system);
		DIR* d = opendir(folder);
		if (!d) {
			continue;
		}
		// Each file for the same title and a region of the game's: its codes for that version
		char (*names)[256] = NULL;
		size_t found = 0;
		for (struct dirent* e; (e = readdir(d));) {
			size_t const length = strlen(e->d_name);
			if (length < 5 || strcasecmp(e->d_name + length - 4, ".cht")) {
				continue;
			}
			if (!chtFileFits(e->d_name, gamePath)) {
				continue;
			}
			char (*more)[256] = realloc(names, (found + 1) * sizeof(*names));
			if (!more) {
				break;
			}
			names = more;
			snprintf(names[found++], sizeof(*names), "%s", e->d_name);
		}
		closedir(d);
		// In a fixed order, the file named as the game first
		qsort(names, found, sizeof(*names), (int (*)(const void*, const void*)) strcmp);
		// The files whose master code names another build than the game's go when one names the game's:
		// a file's name covers the releases, its codes are for one build ("(USA, Europe)" for USA Rev 1)
		int* const ids = found ? malloc(found * sizeof(int)) : NULL;
		bool matched = false;
		for (size_t i = 0; ids && i < found; ++i) {
			char path[PATH_SIZE + 260];
			snprintf(path, sizeof(path), "%.*s/%s", PATH_SIZE - 1, folder, names[i]);
			ids[i] = gameId >= 0 ? masterId(path) : -1;
			matched |= gameId >= 0 && ids[i] == gameId;
		}
		for (size_t pass = 0; pass < 2; ++pass) {
			for (size_t i = 0; i < found; ++i) {
				if (ids && ids[i] >= 0 && ids[i] != gameId) {
					if (matched) {
						continue;
					}
					if (otherId && *otherId < 0) {
						*otherId = ids[i];
					}
				}
				char base[300];
				snprintf(base, sizeof(base), "%s", own);
				char* const dot = strrchr(base, '.');
				if (dot) {
					*dot = '\0';
				}
				size_t const baseLength = strlen(base);
				bool const exact = !strncmp(names[i], base, baseLength) && !strcmp(names[i] + baseLength, ".cht");
				if (exact != !pass) {
					continue;
				}
				char path[PATH_SIZE + 260];
				snprintf(path, sizeof(path), "%.*s/%s", PATH_SIZE - 1, folder, names[i]);
				count = readCheats(path, files++, accepts, cheats, count, most, rejected, zeroed);
			}
		}
		free(ids);
		free(names);
	}
	return count;
}

// A Game Genie code for the Game Boy: ABC-DEF, or ABC-DEF-GHI, in hexadecimal digits.
bool chtGameGenieCode(const char* code) {
	size_t const length = strlen(code);
	if (length != 7 && length != 11) {
		return false;
	}
	for (size_t i = 0; i < length; ++i) {
		if (i % 4 == 3 ? code[i] != '-' : !isxdigit((unsigned char) code[i])) {
			return false;
		}
	}
	return true;
}

// A part of a CodeBreaker code for the Game Boy Advance: its 8 digits, or the 4 after them.
bool chtCodeBreakerPart(const char* code) {
	size_t const length = strlen(code);
	if (length != 8 && length != 4) {
		return false;
	}
	for (size_t i = 0; i < length; ++i) {
		if (!isxdigit((unsigned char) code[i])) {
			return false;
		}
	}
	return true;
}

// A GameShark code for the Game Boy: eight hexadecimal digits, the type, the value, then the
// address low byte first.
bool chtGameSharkCode(const char* code) {
	if (strlen(code) != 8) {
		return false;
	}
	for (int i = 0; i < 8; ++i) {
		if (!isxdigit((unsigned char) code[i])) {
			return false;
		}
	}
	return true;
}
