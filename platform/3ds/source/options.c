/* Copyright (c) 2026 armGBA 3DS port contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The core's options as it declares them (RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2), with the value
// chosen for each. Values chosen in the menu are kept in config.ini (config.c).

#include "frontend.h"

#include <stdio.h>
#include <string.h>

enum {
	MAX_OPTIONS = 128,
};

struct CoreOption feOptions[MAX_OPTIONS];
int feOptionCount;
bool feOptionsChanged;

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
};

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
	feOptionsChanged = true;
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

void optionsDefineV2(const struct retro_core_options_v2* options) {
	keepChoices();
	feOptionCount = 0;
	for (const struct retro_core_option_v2_definition* d = options->definitions; d->key && feOptionCount < MAX_OPTIONS; ++d) {
		struct CoreOption* o = &feOptions[feOptionCount++];
		memset(o, 0, sizeof(*o));
		snprintf(o->key, sizeof(o->key), "%s", d->key);
		snprintf(o->label, sizeof(o->label), "%s", d->desc_categorized ? d->desc_categorized : d->desc);
		snprintf(o->category, sizeof(o->category), "%s", d->category_key ? d->category_key : "");
		o->visible = true;
		for (int v = 0; v < RETRO_NUM_CORE_OPTION_VALUES_MAX && d->values[v].value && o->valueCount < 64; ++v) {
			snprintf(o->values[o->valueCount], sizeof(o->values[0]), "%s", d->values[v].value);
			snprintf(o->valueLabels[o->valueCount], sizeof(o->valueLabels[0]), "%s",
			         d->values[v].label ? d->values[v].label : d->values[v].value);
			++o->valueCount;
		}
		// Options without values (numbers the menu would type in) keep the default only.
		if (!o->valueCount && d->default_value) {
			snprintf(o->values[0], sizeof(o->values[0]), "%s", d->default_value);
			snprintf(o->valueLabels[0], sizeof(o->valueLabels[0]), "%s", d->default_value);
			o->valueCount = 1;
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
	keepChoices();
	feOptionCount = 0;
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
		while (*p && o->valueCount < 64) {
			const char* bar = strchr(p, '|');
			int length = bar ? (int) (bar - p) : (int) strlen(p);
			snprintf(o->values[o->valueCount], sizeof(o->values[0]), "%.*s", length, p);
			snprintf(o->valueLabels[o->valueCount], sizeof(o->valueLabels[0]), "%.*s", length, p);
			++o->valueCount;
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
		o->visible = visible;
	}
}

void optionsApplySaved(const char* key, const char* value) {
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
