/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// LCD Effects: off, on the GPU (lcd-gl.c) or by the core on the CPU, their rows in the
// menus and their settings.

#include "hooks.h"
#include "lcd-gl.h"
#include "ui.h"
#include "gb.h"
#include "lcd.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// Where the GPU's Game Boy pixels cast their shadow: 0 nowhere, then up and on clockwise, then
// SHADOW_TILT, the way the device tilts.
#define SHADOW_TILT 9

// LCD Effects: off, drawn by the core on the CPU, or all of them here on the GPU, never some of each,
// or the GPU's fast shaders for slower GPUs; the GPU's grid, and its pixel shadow.
enum { LCD_OFF, LCD_CPU, LCD_GPU, LCD_GPU_FAST };
static struct {
	int effects;
	bool grid;
	int shadow;
} lcdSettings;

// The effects are on the GPU when they're set to be and the renderer can draw them; otherwise the
// core draws them.
static bool gpuOn(void) {
	return (lcdSettings.effects == LCD_GPU || lcdSettings.effects == LCD_GPU_FAST) && lcdGlUsable();
}

// The fast shaders draw no LCD Subpixels and no pixel shadow.
static bool gpuFast(void) {
	return lcdSettings.effects == LCD_GPU_FAST && gpuOn();
}

// How many times larger the core makes the picture, by LCD Scale or Fit Screen; 1 with the effects on
// the GPU.
float lcdCoreScale(void) {
	return gpuOn() || libretroLcdShownScale < 1 ? 1 : libretroLcdShownScale;
}

// Tilt: the shadow falls the way gravity pulls across the screen, its Distance with the screen
// upright and none with it lying flat, from the accelerometer (wanted only while it's used), smoothed.
static float tiltX, tiltY;

static bool tiltAvailable(void) {
	return motionAvailable(MOTION_ACCEL);
}

static void followTilt(bool on) {
	float a[3];
	if (!motionWant(MOTION_ACCEL, MOTION_FOR_APP, on) || !on || !motionRead(MOTION_ACCEL, a)) {
		return;
	}
	// The accelerometer reads the push against gravity, so the pull is the other way; down the
	// screen is against its up.
	float x = -a[0], y = a[1];
	float const reach = libretroLcdShadowDistance;
	x *= reach / SDL_STANDARD_GRAVITY;
	y *= reach / SDL_STANDARD_GRAVITY;
	float const length = sqrtf(x * x + y * y);
	if (length > reach) {
		x *= reach / length;
		y *= reach / length;
	}
	tiltX += (x - tiltX) * 0.1f;
	tiltY += (y - tiltY) * 0.1f;
}

// The gap color that keeps the screen's brightness under the grid at this size (lcd.h's
// libretroLcdKeep), from how much gap each output pixel shows, as an alpha blended in linear light.
static uint32_t keptGap(enum LibretroLcd lcd, uint32_t off, int w, int h) {
	static struct LibretroLcdKeep keep;
	static int keptWidth, keptHeight, keptLcd = -1;
	if (w != keptWidth || h != keptHeight || (int) lcd != keptLcd) {
		keptWidth = w;
		keptHeight = h;
		keptLcd = (int) lcd;
		uint8_t linear[256];
		for (int a = 0; a < 256; ++a) {
			linear[a] = (uint8_t) lroundf((1 - powf(1 - a / 255.0f, 1 / 2.2f)) * 255);
		}
		const struct LibretroLcdLook* const look = &libretroLcdLooks[lcd];
		float const pitchX = (float) w / app.gameWidth, pitchY = (float) h / app.gameHeight;
		float const fade = libretroLcdFade(fminf(pitchX, pitchY));
		int* const across = malloc(sizeof(int) * (size_t) w);
		if (!across) {
			return off;
		}
		for (int x = 0; x < w; ++x) {
			across[x] = (int) lroundf(libretroLcdEdgeGap(x, pitchX, look->share * pitchX) * look->across * fade * 255);
		}
		memset(keep.weights, 0, sizeof(keep.weights));
		for (int y = 0; y < h; ++y) {
			int const down = (int) lroundf(libretroLcdEdgeGap(y, pitchY, look->share * pitchY) * look->down * fade * 255);
			for (int x = 0; x < w; ++x) {
				++keep.weights[linear[across[x] + down - (across[x] * down + 127) / 255]];
			}
		}
		free(across);
		for (int a = 0; a < 256; ++a) {
			keep.shares[a] = a / 255.0f;
		}
		keep.count = 256;
		keep.stale = true;
	}
	return libretroLcdKeep(&keep) ? keep.gap : off;
}

// What the GPU draws: the core's LCD Ghosting, Interlaced Rows, LCD Subpixels and Pixel Shadow
// Distance settings, with the GPU's grid and pixel shadow, for the screen loaded (lcd.h has its values).
static void fillLook(struct LcdGlLook* look, SDL_Rect to) {
	memset(look, 0, sizeof(*look));
	uint32_t off = 0;
	enum LibretroLcd const lcd = libretroLcd(&off);
	bool const tft = libretroLcdHasSubpixels(lcd);
	struct Option* const ghosting = optionFind("armgba_lcd_ghosting");
	static const char* const levels[] = { "25", "half", "75", "enabled" };
	for (int i = 0; ghosting && i < 4; ++i) {
		look->ghosting = !strcmp(ghosting->value, levels[i]) ? i + 1 : look->ghosting;
	}
	struct Option* const interlaced = optionFind("armgba_lcd_interleave");
	look->interlaced = tft && interlaced && !strcmp(interlaced->value, "enabled");
	// The times to darken and to lighten, in milliseconds, as lcd.c's screenTimes.
	look->darken = lcd == LIBRETRO_LCD_DMG ? 50 : lcd == LIBRETRO_LCD_POCKET ? 25 : 15;
	look->lighten = lcd == LIBRETRO_LCD_DMG ? 110 : lcd == LIBRETRO_LCD_POCKET ? 50 : 30;
	look->lead = lcd == LIBRETRO_LCD_ADVANCE ? 0.8f : 0.75f;
	look->gaps = lcdSettings.grid;
	look->share = libretroLcdLooks[lcd].share;
	look->across = libretroLcdLooks[lcd].across;
	look->down = libretroLcdLooks[lcd].down;
	look->gap = look->gaps ? keptGap(lcd, off, to.w, to.h) : off;
	look->unlit = off;
	look->subpixels = tft ? libretroLcdSubpixelMix : 0;
	look->bgr = lcd == LIBRETRO_LCD_ADVANCE;
	look->smooth = settings.filter;
	look->fast = gpuFast();
	if (look->fast) {
		look->subpixels = 0;
		followTilt(false);
		return;
	}
	bool const shadowed = lcdSettings.shadow && app.format == RETRO_PIXEL_FORMAT_XRGB8888 && libretroLcdShadow();
	followTilt(shadowed && lcdSettings.shadow == SHADOW_TILT);
	if (shadowed) {
		// A pixel's darkness as lcd.h has it: (start - luminance) * scale >> 16 of 255.
		int start, scale;
		libretroLcdShadowFrom(off, libretroGbDarkestColor(), &start, &scale);
		look->shadowStart = start / 65535.0f;
		look->shadowScale = scale * (65535.0f / 65536 / 255);
		look->shadowX = tiltX;
		look->shadowY = tiltY;
		if (lcdSettings.shadow != SHADOW_TILT) {
			float const reach = libretroLcdReach(lcdSettings.shadow - 1);
			look->shadowX = libretroLcdWays[lcdSettings.shadow - 1][0] * reach;
			look->shadowY = libretroLcdWays[lcdSettings.shadow - 1][1] * reach;
		}
	}
}

bool lcdPictureCame(void) {
	lcdGlNewPicture();
	return false;
}

void lcdPictureUpload(void) {
	SDL_UpdateTexture(app.game, NULL, app.picture, (int) app.picturePitch);
}

// Fit Screen's Auto draws at the size of the window, which the core reads with its options.
static void tellScreenSize(void) {
	int width, height;
	if (SDL_GetRendererOutputSize(app.renderer, &width, &height) == 0 && width > 0 && height > 0 &&
	    ((unsigned) width != libretroLcdScreenWidth || (unsigned) height != libretroLcdScreenHeight)) {
		libretroLcdScreenWidth = (unsigned) width;
		libretroLcdScreenHeight = (unsigned) height;
		optionsChanged = true;
	}
}

void lcdPictureDraw(SDL_Rect to) {
	tellScreenSize();
	if (gpuOn()) {
		struct LcdGlLook look;
		fillLook(&look, to);
		if ((look.ghosting || look.interlaced || look.gaps || look.subpixels || look.shadowScale) && lcdGlDraw(to, &look)) {
			return;
		}
	} else {
		followTilt(false);
	}
	drawPicture(to);
}

void lcdRendererGoing(void) {
	lcdGlStop();
}

// LCD Effects stands for the core's CPU LCD Effects: the core draws its effects only on the CPU.
const char* lcdOptionRead(const struct Option* option) {
	if (strcmp(option->key, "armgba_lcd_effects")) {
		return NULL;
	}
	return lcdSettings.effects == LCD_OFF || gpuOn() ? "disabled" : "enabled";
}

// LCD Effects replaces CPU LCD Effects' row. With the effects on the GPU, the core's own rows go too
// (LCD Scale, Pixel Gaps, its pixel shadow and LCD Effect Threads), and the settings the GPU shares
// show as the screen has them: LCD Ghosting; Interlaced Rows and LCD Subpixels on the Color's and
// Advance's screens; Pixel Shadow Distance with the GPU's shadow on.
bool lcdOptionShown(const struct Option* option, bool shown) {
	if (!strcmp(option->key, "armgba_lcd_effects")) {
		return false;
	}
	if (!gpuOn()) {
		return shown;
	}
	uint32_t gap;
	bool const tft = libretroLcdHasSubpixels(libretroLcd(&gap));
	static const char* const cpuOnly[] = { "armgba_lcd_grid", "armgba_lcd_gaps", "armgba_lcd_shadow", "armgba_lcd_threads" };
	for (size_t i = 0; i < sizeof(cpuOnly) / sizeof(*cpuOnly); ++i) {
		if (!strcmp(option->key, cpuOnly[i])) {
			return false;
		}
	}
	if (!strcmp(option->key, "armgba_lcd_ghosting")) {
		return true;
	}
	if (!strcmp(option->key, "armgba_lcd_interleave")) {
		return tft;
	}
	if (!strcmp(option->key, "armgba_lcd_shadow_distance")) {
		return lcdSettings.shadow && libretroLcdShadow() && !gpuFast();
	}
	if (!strcmp(option->key, "armgba_lcd_subpixels")) {
		return tft && !gpuFast();
	}
	return shown;
}

enum {
	SET_LCD_RENDERING,
	SET_GPU_GRID,
	SET_SHADOW,
};

static void changeLcd(int which, int delta) {
	int const d = delta ? delta : 1;
	switch (which) {
	// Off, CPU, then GPU and GPU Fast where the renderer can draw them. The core reads its effects
	// again, now off or back as they're set.
	case SET_LCD_RENDERING:
		lcdSettings.effects = wrap(lcdSettings.effects >= LCD_GPU && !lcdGlUsable() ? LCD_CPU : lcdSettings.effects, d, lcdGlUsable() ? 4 : 2);
		optionsChanged = true;
		refreshShownOptions();
		break;
	case SET_GPU_GRID:
		lcdSettings.grid = !lcdSettings.grid;
		break;
	// Off, then the eight directions.
	case SET_SHADOW:
		lcdSettings.shadow = wrap(lcdSettings.shadow, d, tiltAvailable() ? SHADOW_TILT + 1 : SHADOW_TILT);
		break;
	}
	settingsWrite();
}

// LCD Effects: off, or drawn by the core on the CPU or the app on the GPU. Without a GL renderer, only
// the CPU can.
static void addLcdRendering(void) {
	static const char* const names[] = { "Off", "CPU", "GPU", "GPU Fast" };
	addValue("LCD Effects", names[lcdSettings.effects == LCD_OFF ? LCD_OFF : gpuOn() ? lcdSettings.effects : LCD_CPU], changeLcd, SET_LCD_RENDERING);
	helpFor(lcdGlUsable() ? "Off, or drawn on the CPU or the GPU. GPU Fast is for slower GPUs: ghosting and gaps, no subpixels or shadow."
	                      : "Off, or drawn on the CPU; this renderer can't draw them on the GPU. Every effect keeps its setting.");
}

// The LCD Effects group starts with LCD Effects.
void lcdGroupRows(const char* category) {
	if (!strcmp(category, "video_lcd")) {
		addLcdRendering();
	}
}

// Pixel Gaps and the pixel shadow, only with LCD Effects on the GPU, the shadow only on the first
// Game Boy's and the Pocket's screens.
static void addGpuRows(void) {
	if (!gpuOn()) {
		return;
	}
	addValue("Pixel Gaps", onOff(lcdSettings.grid), changeLcd, SET_GPU_GRID);
	helpFor("The gaps between the screen's pixels, at any size, fainter when smaller.");
	if (libretroLcdShadow() && !gpuFast()) {
		static const char* const shadows[SHADOW_TILT + 1] = { "Off", "Up", "Up Right", "Right", "Down Right", "Down", "Down Left", "Left", "Up Left", "Tilt" };
		addValue("Game Boy Pixel Shadow", shadows[lcdSettings.shadow], changeLcd, SET_SHADOW);
		helpFor(tiltAvailable() ? "Which way the Game Boy's and Pocket's pixels cast a shadow, or Off. Tilt follows the device."
		                        : "Which way the Game Boy's and Pocket's pixels cast a shadow, or Off.");
	}
}

// On LCD Effects, the GPU's rows come where the CPU's Pixel Gaps does: after Interlaced Rows on the
// Color's and Advance's screens, else after LCD Ghosting.
void lcdOptionRows(const struct Option* option) {
	uint32_t gap;
	bool const tft = libretroLcdHasSubpixels(libretroLcd(&gap));
	if (!strcmp(option->key, tft ? "armgba_lcd_interleave" : "armgba_lcd_ghosting")) {
		addGpuRows();
	}
}

void lcdSettingsDefaults(void) {
	lcdSettings.effects = LCD_GPU;
	lcdSettings.grid = false;
	lcdSettings.shadow = 0;
}

bool lcdSettingRead(const char* key, const char* value, int n, bool yes) {
	if (!strcmp(key, "lcd_rendering")) {
		lcdSettings.effects = !strcmp(value, "off") ? LCD_OFF : !strcmp(value, "cpu") ? LCD_CPU : !strcmp(value, "gpu_fast") ? LCD_GPU_FAST : LCD_GPU;
	} else if (!strcmp(key, "gpu_lcd_grid")) {
		lcdSettings.grid = yes;
	} else if (!strcmp(key, "lcd_shadow")) {
		lcdSettings.shadow = n >= 0 && n <= SHADOW_TILT ? n : 0;
	} else {
		return false;
	}
	return true;
}

void lcdSettingsWrite(FILE* f) {
	static const char* const names[] = { "off", "cpu", "gpu", "gpu_fast" };
	fprintf(f, "lcd_rendering = %s\ngpu_lcd_grid = %s\nlcd_shadow = %d\n", names[lcdSettings.effects], lcdSettings.grid ? "true" : "false", lcdSettings.shadow);
}
