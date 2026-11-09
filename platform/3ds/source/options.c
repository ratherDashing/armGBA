/* Copyright (c) 2026 armGBA 3DS port contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The core's options as it declares them (RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2): every value, its
// label, the option's help text and category, with the value chosen for each. Values chosen in the
// menu are kept in config.ini (config.c).

#include "frontend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
	MAX_OPTIONS = 160,
	MAX_CATEGORIES = 32,
};

struct CoreOption feOptions[MAX_OPTIONS];
int feOptionCount;
bool feOptionsChanged;
struct OptionCategory feCategories[MAX_CATEGORIES];
int feCategoryCount;

// Saved values read before the core declared its options, applied as it does.
static struct {
	char key[64];
	char value[48];
} pending[MAX_OPTIONS];
static int pendingCount;

// Defaults that suit the 3DS better than the core's own, where it has a choice.
static const struct {
	const char* key;
	const char* value;
} defaults3ds[] = {
	// The intro costs nothing to skip, and the BIOS file is often missing.
	{ "armgba_skip_bios", "enabled" },
	// Keeps the game at full speed when the CPU can't: sound never runs short.
	{ "armgba_frame_skip", "auto" },
	// Replaced on an Old 3DS by forcedValue. One Frame Late: the third core draws a frame while the
	// next is emulated. With On, the game's thread waited 4 ms a frame for the picture in Mario
	// Kart's races, which left 1 ms of the frame spare (2026-11-09).
	{ "armgba_threaded_video", "one frame late" },
};

static const char* forcedValue(const char* key);

static char* copyOf(const char* text) {
	if (!text) {
		return NULL;
	}
	size_t const n = strlen(text) + 1;
	char* copy = malloc(n);
	if (copy) {
		memcpy(copy, text, n);
	}
	return copy;
}

static void freeOption(struct CoreOption* o) {
	for (int v = 0; v < o->valueCount; ++v) {
		free(o->values[v]);
		free(o->valueLabels[v]);
	}
	free(o->values);
	free(o->valueLabels);
	free(o->info);
	memset(o, 0, sizeof(*o));
}

static struct CoreOption* find(const char* key) {
	for (int i = 0; i < feOptionCount; ++i) {
		if (!strcmp(feOptions[i].key, key)) {
			return &feOptions[i];
		}
	}
	return NULL;
}

static bool choose(struct CoreOption* o, const char* value) {
	for (int v = 0; v < o->valueCount; ++v) {
		if (!strcmp(o->values[v], value)) {
			o->current = v;
			return true;
		}
	}
	return false;
}

static void setPending(const char* key, const char* value) {
	for (int i = 0; i < pendingCount; ++i) {
		if (!strcmp(pending[i].key, key)) {
			snprintf(pending[i].value, sizeof(pending[0].value), "%s", value);
			return;
		}
	}
	if (pendingCount < MAX_OPTIONS) {
		snprintf(pending[pendingCount].key, sizeof(pending[0].key), "%s", key);
		snprintf(pending[pendingCount].value, sizeof(pending[0].value), "%s", value);
		++pendingCount;
	}
}

// The core declares its options again when their lists change: what was chosen stays chosen.
static void keepChoices(void) {
	for (int i = 0; i < feOptionCount; ++i) {
		if (feOptions[i].valueCount) {
			setPending(feOptions[i].key, feOptions[i].values[feOptions[i].current]);
		}
	}
}

static void clearAll(void) {
	keepChoices();
	for (int i = 0; i < feOptionCount; ++i) {
		freeOption(&feOptions[i]);
	}
	feOptionCount = 0;
}

static void applyDefaultsAndPending(void) {
	for (size_t i = 0; i < sizeof(defaults3ds) / sizeof(*defaults3ds); ++i) {
		struct CoreOption* o = find(defaults3ds[i].key);
		if (o && choose(o, defaults3ds[i].value)) {
			o->defaultIndex = o->current;
		}
	}
	for (int i = 0; i < pendingCount; ++i) {
		struct CoreOption* o = find(pending[i].key);
		if (o) {
			choose(o, pending[i].value);
		}
	}
	for (int i = 0; i < feOptionCount; ++i) {
		const char* forced = forcedValue(feOptions[i].key);
		if (forced && choose(&feOptions[i], forced)) {
			feOptions[i].defaultIndex = feOptions[i].current;
		}
	}
	feOptionsChanged = true;
}

static void addValue(struct CoreOption* o, const char* value, const char* label) {
	char** values = realloc(o->values, (o->valueCount + 1) * sizeof(char*));
	if (values) {
		o->values = values;
	}
	char** labels = realloc(o->valueLabels, (o->valueCount + 1) * sizeof(char*));
	if (labels) {
		o->valueLabels = labels;
	}
	if (!values || !labels) {
		return;
	}
	o->values[o->valueCount] = copyOf(value);
	o->valueLabels[o->valueCount] = copyOf(label ? label : value);
	if (o->values[o->valueCount] && o->valueLabels[o->valueCount]) {
		++o->valueCount;
	}
}

// Options the 3DS port decides itself, never shown, with the value it uses. The core's CPU LCD
// Effects cost the emulation's own time and, scaled to the 3DS's screen, made stripes and rainbows:
// the GPU draws the picture and LCD3x instead (video.c). Threaded Video: the New 3DS's third core
// draws the picture (jitmem.c: renderThreadHostCreate), and the Old 3DS has no core to spare.
static const char* forcedValue(const char* key) {
	if (!strcmp(key, "armgba_lcd_effects")) {
		return "disabled";
	}
	// The New 3DS's third core draws the picture: One Frame Late (the default there) or On. The Old
	// 3DS has no core to spare.
	if (!strcmp(key, "armgba_threaded_video") && !isNew3ds()) {
		return "disabled";
	}
	return NULL;
}

static bool neverShown(const char* key) {
	return forcedValue(key) || !strncmp(key, "armgba_lcd_", 11);
}

void optionsDefineV2(const struct retro_core_options_v2* options) {
	clearAll();
	feCategoryCount = 0;
	for (const struct retro_core_option_v2_category* c = options->categories; c && c->key && feCategoryCount < MAX_CATEGORIES; ++c) {
		struct OptionCategory* out = &feCategories[feCategoryCount++];
		snprintf(out->key, sizeof(out->key), "%s", c->key);
		snprintf(out->label, sizeof(out->label), "%s", c->desc ? c->desc : c->key);
		snprintf(out->info, sizeof(out->info), "%s", c->info ? c->info : "");
	}
	for (const struct retro_core_option_v2_definition* d = options->definitions; d->key && feOptionCount < MAX_OPTIONS; ++d) {
		struct CoreOption* o = &feOptions[feOptionCount++];
		memset(o, 0, sizeof(*o));
		snprintf(o->key, sizeof(o->key), "%s", d->key);
		// The label under its category's page, else the full one.
		snprintf(o->label, sizeof(o->label), "%s", d->desc_categorized && *d->desc_categorized ? d->desc_categorized : d->desc);
		snprintf(o->category, sizeof(o->category), "%s", d->category_key ? d->category_key : "");
		o->info = copyOf(d->info_categorized && *d->info_categorized ? d->info_categorized : d->info);
		o->visible = !neverShown(o->key);
		for (int v = 0; v < RETRO_NUM_CORE_OPTION_VALUES_MAX && d->values[v].value; ++v) {
			addValue(o, d->values[v].value, d->values[v].label);
		}
		if (!o->valueCount && d->default_value) {
			addValue(o, d->default_value, NULL);
		}
		if (d->default_value) {
			choose(o, d->default_value);
		}
		o->defaultIndex = o->current;
	}
	applyDefaultsAndPending();
}

// The first style: "Label; first|second|...", the first value the default.
void optionsDefineV0(const struct retro_variable* variables) {
	clearAll();
	feCategoryCount = 0;
	for (const struct retro_variable* v = variables; v->key && feOptionCount < MAX_OPTIONS; ++v) {
		struct CoreOption* o = &feOptions[feOptionCount++];
		memset(o, 0, sizeof(*o));
		snprintf(o->key, sizeof(o->key), "%s", v->key);
		const char* semi = strchr(v->value, ';');
		snprintf(o->label, sizeof(o->label), "%.*s", semi ? (int) (semi - v->value) : (int) strlen(v->value), v->value);
		o->visible = true;
		const char* p = semi ? semi + 1 : "";
		while (*p == ' ') {
			++p;
		}
		while (*p) {
			const char* bar = strchr(p, '|');
			int length = bar ? (int) (bar - p) : (int) strlen(p);
			char value[128];
			snprintf(value, sizeof(value), "%.*s", length, p);
			addValue(o, value, NULL);
			p += length + (bar ? 1 : 0);
		}
	}
	applyDefaultsAndPending();
}

const char* optionValue(const char* key) {
	struct CoreOption* o = find(key);
	return o && o->valueCount ? o->values[o->current] : NULL;
}

void optionSetVisible(const char* key, bool visible) {
	struct CoreOption* o = find(key);
	if (o) {
		o->visible = visible && !neverShown(key);
	}
}

void optionsApplySaved(const char* key, const char* value) {
	if (forcedValue(key)) {
		return;
	}
	struct CoreOption* o = find(key);
	if (o) {
		choose(o, value);
	}
	setPending(key, value);
}

// Saved values of options the core hasn't declared (yet), to write back to config.ini.
int optionsPendingCount(void) {
	return pendingCount;
}

void optionsPending(int i, const char** key, const char** value) {
	*key = pending[i].key;
	*value = pending[i].value;
}
