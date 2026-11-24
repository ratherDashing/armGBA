/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_LIBRETRO_LCD_H
#define ARMGBA_LIBRETRO_LCD_H

// The loaded console's screen, for the LCD grid: the gaps between its pixels and the shadow its pixels
// cast, which the standalone frontend draws on the GPU and the core can draw into a larger picture.

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

enum LibretroLcd {
	// The first Game Boy's, and the Pocket's and Light's: dark dots, with the liquid crystal between
	// them never driven, so it is the screen's color while off there.
	LIBRETRO_LCD_DMG,
	LIBRETRO_LCD_POCKET,
	// The Game Boy Color's and Advance's: a black matrix between the pixels.
	LIBRETRO_LCD_COLOR,
	LIBRETRO_LCD_ADVANCE,
};

// The screen, and the color between its pixels as XRGB8888.
enum LibretroLcd libretroLcd(uint32_t* gap);

// Keeping the Game Boy screens' and the Color's brightness under the grid (lcd.c has why). The caller
// gives the shares of gap, 0 to 1, a square's pixels are blended with, and how many pixels take each,
// and sets stale when they change. The gap to draw instead, a table for each channel (red, green,
// blue) that darkens the picture, and one of each pixel's own gap color for the ones the gap can't
// keep, are solved again when stale or the screen or its colors change, counted in solved. Returns
// whether the screen's brightness is kept.
struct LibretroLcdKeep {
	float shares[256], weights[256];
	// With LCD Subpixels, each channel's weights instead: the pixels' counts times their stripe factor
	// for the channel, in linear light.
	float channelWeights[3][256];
	bool perChannel;
	int count;
	bool stale;
	int lcd;
	uint32_t colors[5], gap;
	uint8_t table[3][256], gaps[3][256];
	// The same for RGB565 pictures, each channel's value to its bits in place: the tables, then the gaps.
	uint16_t table565[2][3][64];
	unsigned solved;
};
bool libretroLcdKeep(struct LibretroLcdKeep* keep);

// Each screen's gaps: their width as a share of the pixel pitch, and how much of the gap color shows
// across (between columns) and down (between rows). From photos of the consoles: the first Game Boy's
// are a quarter of the pitch and brighter than its lightest shade; the Advance's darken as much as a
// black line a ninth of the pitch would; the Color's do so between columns, but hardly between rows.
// The Pocket's are thinner than the first Game Boy's, by an estimate.
struct LibretroLcdLook {
	float share, across, down;
};
static const struct LibretroLcdLook libretroLcdLooks[] = {
	[LIBRETRO_LCD_DMG] = { 0.25f, 1, 1 },
	[LIBRETRO_LCD_POCKET] = { 0.15f, 1, 1 },
	[LIBRETRO_LCD_COLOR] = { 0.15f, 0.6f, 0.1f },
	[LIBRETRO_LCD_ADVANCE] = { 0.15f, 0.75f, 0.75f },
};

// LCD Subpixels: the Color's and Advance's pixels are each three stripes side by side, red, green and
// blue left to right on the Color, blue, green and red on the Advance (the GBCC emulator's and the
// gba-tutorial's write-ups, from photos). Each output pixel is mixed between the pixel's own color and
// its stripes, in linear light: (1 - mix) * color + mix * stripes, a stripe lighting only its own
// channel. At 100% a pixel is its stripes alone, a third as bright on average. They fade in from one
// output pixel a dot to two; below three, a pixel's columns show them blended (lcd.c's gridLay).
extern float libretroLcdSubpixelMix;

static inline bool libretroLcdHasSubpixels(enum LibretroLcd lcd) {
	return lcd == LIBRETRO_LCD_COLOR || lcd == LIBRETRO_LCD_ADVANCE;
}

static inline float libretroLcdSubpixelFade(float pitch) {
	return fminf(fmaxf(pitch - 1, 0), 1);
}

// The stripe, from the left, that lights channel (0 red, 1 green, 2 blue).
static inline int libretroLcdStripeOf(enum LibretroLcd lcd, int channel) {
	return lcd == LIBRETRO_LCD_ADVANCE ? 2 - channel : channel;
}

// How much of the span from to to, in dots from a pixel's left edge, stripe (0 to 2) covers.
static inline float libretroLcdStripeBelow(float u, int stripe) {
	return floorf(u) / 3 + fminf(fmaxf(u - floorf(u) - stripe / 3.0f, 0), 1 / 3.0f);
}

static inline float libretroLcdStripeShare(float from, float to, int stripe) {
	return (libretroLcdStripeBelow(to, stripe) - libretroLcdStripeBelow(from, stripe)) / (to - from);
}

// A pixel's stripes as bands, as many as the narrowest pixel has output columns, so that every pixel
// shows them in the same colors (at 2.5x, two bands, a two-column pixel's, which a three-column pixel
// shows with their mix between): stripe's share of the span from to to of the pixel, 0 to 1.
static inline float libretroLcdBandShare(float from, float to, int bands, int stripe) {
	float share = 0;
	for (int b = 0; b < bands; ++b) {
		float const overlap = fminf(to, (float) (b + 1) / bands) - fmaxf(from, (float) b / bands);
		share += overlap > 0 ? overlap * libretroLcdStripeShare((float) b / bands, (float) (b + 1) / bands, stripe) : 0;
	}
	return share / (to - from);
}

// The factor, in linear light, on a channel whose stripe covers share of the output pixel.
static inline float libretroLcdStripeFactor(float share, float mix) {
	return 1 - mix + mix * share;
}

// The first Game Boy's and the Pocket's pixels' shadow on the reflector behind them: a pixel as dark
// as the screen goes darkens it by a fifth (51 of 255), 1.4 dots away the way chosen (1 dot each way
// on a diagonal), from a Reddit photo of a first Game Boy's screen and Brick Boy's model of it (a
// shadow 1.35 dots away), so that is the screen's darkest shade's at the middle of its contrast wheel.
// A pixel's darkness is how far its luminance in linear light, the light it keeps from the reflector,
// is below a sixteenth under the unlit screen's: the first Game Boy's lightest shade, as bright as the
// unlit screen, casts none, and darker shades, as the wheel turns up, cast more. The ways, up and on
// clockwise.
enum { LIBRETRO_LCD_SHADOW = 51 };

// Each channel's share of an XRGB8888 color's luminance, in linear light, white 65535.
extern uint16_t libretroLcdLuminance[3][256];

static inline int libretroLcdLuminanceOf(uint32_t c) {
	return libretroLcdLuminance[0][c >> 16 & 0xFF] + libretroLcdLuminance[1][c >> 8 & 0xFF] + libretroLcdLuminance[2][c & 0xFF];
}

// Where the shadow of a screen unlit as unlit starts, in luminance, and its scale: a pixel of
// luminance y below start is (start - y) * scale >> 16 dark, LIBRETRO_LCD_SHADOW at the darkest shade.
// Fills libretroLcdLuminance.
void libretroLcdShadowFrom(uint32_t unlit, uint32_t darkest, int* start, int* scale);

// Whether the screen's pixels cast that shadow: the first Game Boy's and the Pocket's, lit from the
// front, but not the Light's, lit from behind.
bool libretroLcdShadow(void);
static const int8_t libretroLcdWays[8][2] = { { 0, -1 }, { 1, -1 }, { 1, 0 }, { 1, 1 }, { 0, 1 }, { -1, 1 }, { -1, 0 }, { -1, -1 } };

// How far the shadow falls, in the screen's dots (Game Boy Pixel Shadow Distance), and so how far
// each way reaches across and down: a diagonal's as far in all.
extern float libretroLcdShadowDistance;

static inline float libretroLcdReach(int way) {
	return libretroLcdWays[way][0] && libretroLcdWays[way][1] ? libretroLcdShadowDistance * 0.70710678f : libretroLcdShadowDistance;
}

// LCD Scale's Fit Screen: the size a width by height picture is drawn at, as large as fits the screen.
void libretroLcdFitSize(unsigned width, unsigned height, unsigned* outWidth, unsigned* outHeight);
// How many times larger than the console's the picture the core sends is, by LCD Scale or Fit Screen.
extern float libretroLcdShownScale;
// The size the frontend draws the picture into, for Fit Screen's Auto; 0 while it doesn't say.
extern unsigned libretroLcdScreenWidth, libretroLcdScreenHeight;
// Whether the frontend takes a larger picture during a game as it comes (the standalone app). RetroArch
// makes its video again for one, which some systems' compositors refuse (it quits), so there a larger
// LCD Scale or HD scale waits for the game to be started again.
extern bool libretroAnySize;
// A buffer of pictures given to the frontend, which the core no longer draws into: freed once the
// frontend has shown a newer picture. RetroArch keeps the last picture it showed, to show again under
// its menu, and a frame it runs without showing (run-ahead, a netplay rollback) or a picture left out
// (Frame Skip, a link wait) doesn't replace it, so freed at once it could read freed memory.
void pictureRetire(void* buffer);
// All of them freed, the game unloaded.
void pictureRetiredFree(void);

// How strongly a grid drawn at any size shows when a source pixel is pitch output pixels wide: none at
// 1x, all of it from 3x on. Smaller, a gap is under a pixel wide and reads as heavy lines.
static inline float libretroLcdFade(float pitch) {
	return fminf(fmaxf((pitch - 1) / 2, 0), 1);
}

static inline float libretroLcdGapCoverage(int at, float pitch, float gap) {
	float covered = 0;
	for (int edge = (int) ceilf((at - gap / 2) / pitch); edge * pitch - gap / 2 < at + 1; ++edge) {
		float const from = fmaxf(edge * pitch - gap / 2, (float) at), to = fminf(edge * pitch + gap / 2, (float) at + 1);
		covered += to > from ? to - from : 0;
	}
	return fminf(covered, 1);
}

// How much of the gaps either side output pixel at shows, along one way: each gap gap output pixels
// wide, centered on the output pixel edge where one source pixel gives way to the next, the picture's
// edges too, source pixels pitch output pixels apart. Every gap is drawn alike however the pixels fall,
// as the core's Fit Screen and the GPU's grid draw them.
static inline float libretroLcdEdgeGap(int at, float pitch, float gap) {
	int const s = (int) ((at + 0.5f) / pitch);
	float const first = ceilf(s * pitch - 0.5f), next = ceilf((s + 1) * pitch - 0.5f);
	return fminf(fminf(fmaxf(gap / 2 - (at - first), 0), 1) + fminf(fmaxf(gap / 2 - (next - at - 1), 0), 1), 1);
}

#endif
