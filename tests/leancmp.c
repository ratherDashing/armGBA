/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Runs a game twice side by side, on the exact CPU (step.c) and on the table-driven one (lean.c),
// an instruction at a time, and stops at the first instruction after which their CPUs or clocks
// differ, or the first frame after which their memory does.
// usage: leancmp bios.bin rom.gba frames ["frame:button[:hold],..."]

#include "gba.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct GBA* load(const char* biosPath, const uint8_t* rom, size_t romSize) {
	struct GBA* gba = gbaCreate();
	FILE* f = fopen(biosPath, "rb");
	uint8_t bios[SIZE_BIOS];
	if (!gba || !f || fread(bios, 1, sizeof(bios), f) != sizeof(bios)) {
		return NULL;
	}
	fclose(f);
	gbaLoadBios(gba, bios, sizeof(bios));
	gbaLoadRom(gba, rom, romSize);
	gbaReset(gba, false);
	return gba;
}

// One instruction, or a halted CPU's wait for the next event.
static void advance(struct GBA* gba, bool lean) {
	if (gba->cpu.halted) {
		if (gba->cpu.cycles < gba->cpu.nextEvent) {
			gba->cpu.cycles = gba->cpu.nextEvent;
		}
		processEvents(gba);
	} else if (lean) {
		leanStep(gba);
	} else {
		stepInstruction(gba);
	}
	gba->returnRequested = false;
}

static uint16_t keysAt(const char* script, uint32_t frame) {
	static const char* names[10] = { "a", "b", "select", "start", "right", "left", "up", "down", "r", "l" };
	uint16_t keys = 0;
	for (const char* p = script; p && *p;) {
		unsigned at = 0;
		unsigned hold = 1;
		char name[16] = { 0 };
		int n = sscanf(p, "%u:%15[a-z]:%u", &at, name, &hold);
		if (n >= 2 && frame >= at && frame < at + hold) {
			for (int i = 0; i < 10; ++i) {
				if (!strcmp(name, names[i])) {
					keys |= 1 << i;
				}
			}
		}
		p = strchr(p, ',');
		p = p ? p + 1 : NULL;
	}
	return keys;
}

static void report(const char* what, struct GBA* a, struct GBA* b, unsigned long long count) {
	printf("%s after instruction %llu, frame %u\n", what, count, a->video.frameCounter);
	for (int i = 0; i < 16; ++i) {
		if (a->cpu.r[i] != b->cpu.r[i]) {
			printf("  r%d step %08X lean %08X\n", i, a->cpu.r[i], b->cpu.r[i]);
		}
	}
	printf("  cpsr %08X/%08X cycles %d/%d time %u/%u prefetch %08X %08X / %08X %08X fetchSeq %d/%d\n", a->cpu.cpsr, b->cpu.cpsr,
	       a->cpu.cycles, b->cpu.cycles, timingCpuTime(a), timingCpuTime(b), a->cpu.prefetch[0], a->cpu.prefetch[1], b->cpu.prefetch[0],
	       b->cpu.prefetch[1], a->cpu.fetchSeq, b->cpu.fetchSeq);
}

int main(int argc, char** argv) {
	if (argc < 4) {
		fprintf(stderr, "usage: %s bios.bin rom.gba frames [\"frame:button[:hold],...\"]\n", argv[0]);
		return 2;
	}
	FILE* f = fopen(argv[2], "rb");
	if (!f) {
		return 2;
	}
	fseek(f, 0, SEEK_END);
	size_t romSize = (size_t) ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t* rom = malloc(romSize);
	if (!rom || fread(rom, 1, romSize, f) != romSize) {
		return 2;
	}
	fclose(f);
	struct GBA* a = load(argv[1], rom, romSize);
	struct GBA* b = load(argv[1], rom, romSize);
	if (!a || !b) {
		fprintf(stderr, "couldn't load\n");
		return 2;
	}
	uint32_t const frames = (uint32_t) atoi(argv[3]);
	const char* script = argc > 4 ? argv[4] : NULL;
	unsigned long long count = 0;
	while (a->video.frameCounter < frames) {
		uint32_t const frame = a->video.frameCounter;
		uint16_t const keys = keysAt(script, frame);
		gbaSetKeys(a, keys);
		gbaSetKeys(b, keys);
		while (a->video.frameCounter == frame) {
			advance(a, false);
			advance(b, true);
			++count;
			if (memcmp(&a->cpu, &b->cpu, sizeof(a->cpu)) || timingCpuTime(a) != timingCpuTime(b)) {
				report("CPU differs", a, b, count);
				return 1;
			}
		}
		if (b->video.frameCounter != a->video.frameCounter || memcmp(a->iwram, b->iwram, sizeof(a->iwram)) ||
		    memcmp(a->ewram, b->ewram, sizeof(a->ewram)) || memcmp(a->io, b->io, sizeof(a->io)) ||
		    memcmp(a->vram, b->vram, sizeof(a->vram)) || memcmp(a->palette, b->palette, sizeof(a->palette)) ||
		    memcmp(a->oam, b->oam, sizeof(a->oam))) {
			report("memory differs", a, b, count);
			return 1;
		}
	}
	printf("same: %u frames, %llu instructions\n", frames, count);
	return 0;
}
