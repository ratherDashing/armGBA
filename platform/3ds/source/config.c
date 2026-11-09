/* Copyright (c) 2026 armGBA 3DS port contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// config.ini: "key=value" lines. The frontend's own settings are frontend_*; the rest are the
// core's options, kept only where they differ from the default.

#include "frontend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Config config = {
	.scale = SCALE_FIT,
	.filter = true,
	.lastDir = "sdmc:/roms",
};

void configLoad(void) {
	FILE* f = fopen(CONFIG_FILE, "r");
	if (!f) {
		return;
	}
	char line[700];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		char* eq = strchr(line, '=');
		if (!eq) {
			continue;
		}
		*eq = 0;
		const char* key = line;
		const char* value = eq + 1;
		if (!strcmp(key, "frontend_scale")) {
			config.scale = atoi(value) % SCALE_COUNT;
		} else if (!strcmp(key, "frontend_filter")) {
			config.filter = atoi(value) != 0;
		} else if (!strcmp(key, "frontend_last_dir")) {
			snprintf(config.lastDir, sizeof(config.lastDir), "%s", value);
		} else {
			optionsApplySaved(key, value);
		}
	}
	fclose(f);
}

void configSave(void) {
	char temp[] = CONFIG_FILE ".tmp";
	FILE* f = fopen(temp, "w");
	if (!f) {
		return;
	}
	fprintf(f, "frontend_scale=%d\nfrontend_filter=%d\nfrontend_last_dir=%s\n", config.scale, config.filter, config.lastDir);
	for (int i = 0; i < feOptionCount; ++i) {
		const struct CoreOption* o = &feOptions[i];
		if (o->current != o->defaultIndex) {
			fprintf(f, "%s=%s\n", o->key, o->values[o->current]);
		}
	}
	// Ones the core hasn't declared this time (a Game Boy game hides the GBA's, and the other way).
	for (int i = 0; i < optionsPendingCount(); ++i) {
		const char *key, *value;
		optionsPending(i, &key, &value);
		bool declared = false;
		for (int j = 0; j < feOptionCount && !declared; ++j) {
			declared = !strcmp(feOptions[j].key, key);
		}
		if (!declared) {
			fprintf(f, "%s=%s\n", key, value);
		}
	}
	fclose(f);
	remove(CONFIG_FILE);
	rename(temp, CONFIG_FILE);
}
