/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The core's LCD effects on the picture it sends: LCD Ghosting, then LCD Grid with the Game Boy's
// pixel shadow (lcd.h has the screens' values). Both share their rows out among the CPU's cores.

#include "core.h"
#include "gb.h"
#include "lcd.h"
#include "workers.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#elif defined(__AVX2__)
#include <immintrin.h>
#endif

int ghosting;
bool interleaved;
int gridHalves = 2;
int gridWay;
bool gridGaps;
bool gridFit;
unsigned gridFitWidth = 1280, gridFitHeight = 720;
float libretroLcdShadowDistance = 1.5f;
float libretroLcdSubpixelMix;
int lcdThreads;

// LCD Ghosting: the screens' liquid crystal takes time to change, darkening, as it is driven,
// faster than it lightens, as it relaxes. Each subpixel moves in linear light from where it was
// towards the new frame by a share each frame. The time constants, in milliseconds to darken and to
// lighten, are estimates for each kind of panel: the first Game Boy's STN, the Pocket's and Light's
// FSTN, and the Color's and Advance's reflective TFTs; 25%, 50% and 75% take that share of them.
// With Interlaced Rows on, the Color's and Advance's also drive alternate rows harder each frame, the
// other rows the next: the harder driven rows' times shorter, the others' longer, so that over two
// frames every row moves as far as without it, and a picture that alternates each frame shows in
// stripes, as it does on the consoles (as SameBoy's accurate frame blending and BGB's Color
// interlacing also have it), the Color's a little more than the Advance's.
// Each subpixel's level is kept 8-bit, gamma encoded, and its next level read from a table built for
// the screen and the picture's channel widths, the rows shared out among the CPU's cores. A frame
// RetroArch runs again, and doesn't show, leaves the screen as it is.
enum { SCREEN_DMG, SCREEN_POCKET, SCREEN_TFT, SCREEN_KINDS };
static const float screenTimes[SCREEN_KINDS][2] = { { 50, 110 }, { 25, 50 }, { 15, 30 } };

static int screenKind(void) {
	if (!gameBoy || libretroGbOnAdvance() || libretroGbIsColor()) {
		return SCREEN_TFT;
	}
	return gbModel == LIBRETRO_GB_MGB || gbShades == 2 || gbShades == 3 ? SCREEN_POCKET : SCREEN_DMG;
}

// Whether the picture is on the Game Boy Advance's panel: a Game Boy Advance game's, or a Game Boy
// game's on the Advance model unless the Color's panel was chosen.
static bool advancePanel(void) {
	return !gameBoy || (libretroGbOnAdvance() && !gbColorPanel);
}

bool lcdCastsShadow(void) {
	return screenKind() != SCREEN_TFT;
}

bool lcdHasSubpixels(void) {
	return screenKind() == SCREEN_TFT;
}

uint16_t libretroLcdLuminance[3][256];

void libretroLcdShadowFrom(uint32_t unlit, uint32_t darkest, int* start, int* scale) {
	if (!libretroLcdLuminance[1][255]) {
		static const float weights[3] = { 0.2126f, 0.7152f, 0.0722f };
		for (int v = 0; v < 256; ++v) {
			float const c = v / 255.0f, l = c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
			for (int k = 0; k < 3; ++k) {
				libretroLcdLuminance[k][v] = (uint16_t) floorf(l * weights[k] * 65535);
			}
		}
	}
	int const lit = libretroLcdLuminanceOf(unlit), range = lit - lit / 16 - libretroLcdLuminanceOf(darkest);
	*start = lit - lit / 16;
	*scale = range > 0 ? (LIBRETRO_LCD_SHADOW << 16) / range : 0;
}

bool libretroLcdShadow(void) {
	return screenKind() != SCREEN_TFT && gbShades != 3;
}

enum LibretroLcd libretroLcd(uint32_t* gap) {
	int const kind = screenKind();
	*gap = kind == SCREEN_TFT ? 0 : libretroGbOffColor();
	if (kind != SCREEN_TFT) {
		return kind == SCREEN_POCKET ? LIBRETRO_LCD_POCKET : LIBRETRO_LCD_DMG;
	}
	return advancePanel() ? LIBRETRO_LCD_ADVANCE : LIBRETRO_LCD_COLOR;
}

// LCD Grid: the picture gridHalves / 2 times larger, with the gaps between the console screen's pixels
// drawn in and, on the first Game Boy and the Pocket, the shadow its pixels cast, as the standalone
// frontend draws them on the GPU (lcd.h has the values), for RetroArch without its shader presets. A
// block of pixels, one at a whole scale and two by two at a half step, makes a whole number of output
// pixels, so every block has the same gaps: each source row is blended toward the gap color once for
// each share of gap the block uses, those rows woven into the larger one, and rows alike copied. At a
// half step, each row's even and odd pixels are first put in two halves, woven as rows of their own. The shadow is the picture's darkness below the unlit screen, moved and sampled smoothly
// once for each square, which darkens the square's blends, gaps and all: much cheaper than shading
// every pixel of the larger picture, as the GPU does, and at the size shown hardly different. The
// loops work on whole rows in vectors, NEON on ARM, SSE or AVX on x86, the rows shared out among the
// CPU's cores.

#if defined(__AVX2__)
#define GRID_VECTOR 32
#else
#define GRID_VECTOR 16
#endif
typedef uint16_t GridU16 __attribute__((vector_size(GRID_VECTOR)));
typedef uint32_t GridU32 __attribute__((vector_size(GRID_VECTOR)));
enum { GRID_LANES16 = GRID_VECTOR / 2, GRID_LANES32 = GRID_VECTOR / 4 };
// Bytes, as many as a vector's 16- or 32-bit lanes, to widen into them.
typedef uint8_t GridBytes16 __attribute__((vector_size(GRID_LANES16)));
typedef uint8_t GridBytes32 __attribute__((vector_size(GRID_LANES32)));

// x / 255, rounded, for x up to 255 * 255.
#define GRID_DIV255(x) (((x) + 128 + (((x) + 128) >> 8)) >> 8)

// The blends below move a row's pixels toward a gap color by a of 255, then take each channel times its
// share of 256 in k (red, green, blue), LCD Subpixels' stripes, in the same pass; 256 leaves it.

// A row of RGB565 pixels blended toward the gap color; without stripes (tint false) the channels'
// multiplies are left out, the body inlined for each.
static inline __attribute__((always_inline)) void gridBlend565Body(const uint16_t* from, uint16_t* to, unsigned count, unsigned a, unsigned gapR,
                                                                   unsigned gapG, unsigned gapB, const uint16_t k[3], bool tint) {
	unsigned const keep = 255 - a;
	uint16_t const keep16 = (uint16_t) keep, addR = (uint16_t) (gapR * a), addG = (uint16_t) (gapG * a), addB = (uint16_t) (gapB * a);
	uint16_t const kr = k[0], kg = k[1], kb = k[2];
	unsigned i = 0;
	for (; i + GRID_LANES16 <= count; i += GRID_LANES16) {
		GridU16 c;
		memcpy(&c, from + i, sizeof(c));
		GridU16 r = c >> 11, g = c >> 5 & 0x3F, b = c & 0x1F;
		r = (GridU16) (r << 3 | r >> 2);
		g = (GridU16) (g << 2 | g >> 4);
		b = (GridU16) (b << 3 | b >> 2);
		r = GRID_DIV255(r * keep16 + addR);
		g = GRID_DIV255(g * keep16 + addG);
		b = GRID_DIV255(b * keep16 + addB);
		if (tint) {
			r = r * kr >> 8;
			g = g * kg >> 8;
			b = b * kb >> 8;
		}
		GridU16 const out = (GridU16) (GRID_DIV255(r * 31) << 11 | GRID_DIV255(g * 63) << 5 | GRID_DIV255(b * 31));
		memcpy(to + i, &out, sizeof(out));
	}
	for (; i < count; ++i) {
		unsigned const c = from[i];
		unsigned r = (c >> 11) << 3 | c >> 13, g = (c >> 5 & 0x3F) << 2 | (c >> 9 & 3), b = (c & 0x1F) << 3 | (c >> 2 & 7);
		r = GRID_DIV255(r * keep + gapR * a);
		g = GRID_DIV255(g * keep + gapG * a);
		b = GRID_DIV255(b * keep + gapB * a);
		if (tint) {
			r = r * kr >> 8;
			g = g * kg >> 8;
			b = b * kb >> 8;
		}
		to[i] = (uint16_t) (GRID_DIV255(r * 31) << 11 | GRID_DIV255(g * 63) << 5 | GRID_DIV255(b * 31));
	}
}

static bool gridTinted(const uint16_t k[3]) {
	return k[0] != 256 || k[1] != 256 || k[2] != 256;
}

static void gridBlend565(const uint16_t* from, uint16_t* to, unsigned count, unsigned a, unsigned gapR, unsigned gapG, unsigned gapB, const uint16_t k[3]) {
	if (gridTinted(k)) {
		gridBlend565Body(from, to, count, a, gapR, gapG, gapB, k, true);
	} else {
		gridBlend565Body(from, to, count, a, gapR, gapG, gapB, k, false);
	}
}

#if !defined(__aarch64__)
// Each 8-bit channel of XRGB8888 pixels times k, a share of 256 for each pixel, in 16-bit multiplies,
// which SSE2 has: red and blue together in one lane pair, green in another. NEON shades with its own.
static inline GridU32 gridScale8888(GridU32 c, GridU32 k) {
	GridU16 const both = (GridU16) (k | k << 16);
	GridU16 const rb = (GridU16) (c & 0xFF00FF) * both >> 8;
	GridU16 const g = (GridU16) (c >> 8 & 0xFF) * both >> 8;
	return (c & 0xFF000000) | (GridU32) rb | (GridU32) g << 8;
}
#endif

static inline uint32_t gridScale8888One(uint32_t c, uint32_t k) {
	return (c & 0xFF000000) | ((c & 0xFF00FF) * k >> 8 & 0xFF00FF) | ((c & 0xFF00) * k >> 8 & 0xFF00);
}

// The same with each channel's own share: rb has red's in the upper half of each lane and blue's in
// the lower, g green's.
static inline GridU32 gridChannels8888(GridU32 c, GridU32 rb, GridU32 g) {
	GridU16 const both = (GridU16) (c & 0xFF00FF) * (GridU16) rb >> 8;
	GridU16 const green = (GridU16) (c >> 8 & 0xFF) * (GridU16) g >> 8;
	return (c & 0xFF000000) | (GridU32) both | (GridU32) green << 8;
}

static inline uint32_t gridChannels8888One(uint32_t c, uint32_t r, uint32_t g, uint32_t b) {
	return (c & 0xFF000000) | ((c >> 16 & 0xFF) * r >> 8) << 16 | ((c >> 8 & 0xFF) * g >> 8) << 8 | (c & 0xFF) * b >> 8;
}

// A row of RGB565 pixels blended toward each pixel's own gap color.
static inline __attribute__((always_inline)) void gridBlendGaps565Body(const uint16_t* from, const uint16_t* gaps, uint16_t* to, unsigned count, unsigned a,
                                                                       const uint16_t k[3], bool tint) {
	uint16_t const keep16 = (uint16_t) (255 - a), a16 = (uint16_t) a, kr = k[0], kg = k[1], kb = k[2];
	unsigned i = 0;
	for (; i + GRID_LANES16 <= count; i += GRID_LANES16) {
		GridU16 c, g;
		memcpy(&c, from + i, sizeof(c));
		memcpy(&g, gaps + i, sizeof(g));
		GridU16 r = c >> 11, gr = c >> 5 & 0x3F, b = c & 0x1F, R = g >> 11, G = g >> 5 & 0x3F, B = g & 0x1F;
		r = GRID_DIV255((GridU16) (r << 3 | r >> 2) * keep16 + (GridU16) (R << 3 | R >> 2) * a16);
		gr = GRID_DIV255((GridU16) (gr << 2 | gr >> 4) * keep16 + (GridU16) (G << 2 | G >> 4) * a16);
		b = GRID_DIV255((GridU16) (b << 3 | b >> 2) * keep16 + (GridU16) (B << 3 | B >> 2) * a16);
		if (tint) {
			r = r * kr >> 8;
			gr = gr * kg >> 8;
			b = b * kb >> 8;
		}
		GridU16 const out = (GridU16) (GRID_DIV255(r * 31) << 11 | GRID_DIV255(gr * 63) << 5 | GRID_DIV255(b * 31));
		memcpy(to + i, &out, sizeof(out));
	}
	for (; i < count; ++i) {
		unsigned const c = from[i], g = gaps[i], keep = 255 - a;
		unsigned r = GRID_DIV255(((c >> 11) << 3 | c >> 13) * keep + ((g >> 11) << 3 | g >> 13) * a);
		unsigned gr = GRID_DIV255(((c >> 5 & 0x3F) << 2 | (c >> 9 & 3)) * keep + ((g >> 5 & 0x3F) << 2 | (g >> 9 & 3)) * a);
		unsigned b = GRID_DIV255(((c & 0x1F) << 3 | (c >> 2 & 7)) * keep + ((g & 0x1F) << 3 | (g >> 2 & 7)) * a);
		if (tint) {
			r = r * kr >> 8;
			gr = gr * kg >> 8;
			b = b * kb >> 8;
		}
		to[i] = (uint16_t) (GRID_DIV255(r * 31) << 11 | GRID_DIV255(gr * 63) << 5 | GRID_DIV255(b * 31));
	}
}

static void gridBlendGaps565(const uint16_t* from, const uint16_t* gaps, uint16_t* to, unsigned count, unsigned a, const uint16_t k[3]) {
	if (gridTinted(k)) {
		gridBlendGaps565Body(from, gaps, to, count, a, k, true);
	} else {
		gridBlendGaps565Body(from, gaps, to, count, a, k, false);
	}
}

// A row blended toward each pixel's own gap color, in gaps.
static void gridBlendGaps8888(const uint32_t* from, const uint32_t* gaps, uint32_t* to, unsigned count, unsigned a, const uint16_t k[3]) {
	uint32_t const toward = a + (a >> 7), keep = 256 - toward;
	uint32_t const keepR = keep * k[0] >> 8, keepG = keep * k[1] >> 8, keepB = keep * k[2] >> 8;
	uint32_t const towardR = toward * k[0] >> 8, towardG = toward * k[1] >> 8, towardB = toward * k[2] >> 8;
	GridU32 const keepRb = (GridU32) { 0 } + (keepR << 16 | keepB), keepG4 = (GridU32) { 0 } + keepG;
	GridU32 const towardRb = (GridU32) { 0 } + (towardR << 16 | towardB), towardG4 = (GridU32) { 0 } + towardG;
	unsigned i = 0;
	for (; i + GRID_LANES32 <= count; i += GRID_LANES32) {
		GridU32 c, g;
		memcpy(&c, from + i, sizeof(c));
		memcpy(&g, gaps + i, sizeof(g));
		GridU32 const out = gridChannels8888(c, keepRb, keepG4) + gridChannels8888(g, towardRb, towardG4);
		memcpy(to + i, &out, sizeof(out));
	}
	for (; i < count; ++i) {
		to[i] = gridChannels8888One(from[i], keepR, keepG, keepB) + gridChannels8888One(gaps[i], towardR, towardG, towardB);
	}
}

static void gridBlend8888(const uint32_t* from, uint32_t* to, unsigned count, unsigned a, unsigned gapR, unsigned gapG, unsigned gapB, const uint16_t k[3]) {
	// Shares of 256: a of 255 rounds to within one level.
	uint32_t const toward = a + (a >> 7), keep = 256 - toward;
	uint32_t const keepR = keep * k[0] >> 8, keepG = keep * k[1] >> 8, keepB = keep * k[2] >> 8;
	uint32_t const gap = ((gapR * toward >> 8) * k[0] >> 8) << 16 | ((gapG * toward >> 8) * k[1] >> 8) << 8 | (gapB * toward >> 8) * k[2] >> 8;
	GridU32 const keepRb = (GridU32) { 0 } + (keepR << 16 | keepB), keepG4 = (GridU32) { 0 } + keepG;
	unsigned i = 0;
	for (; i + GRID_LANES32 <= count; i += GRID_LANES32) {
		GridU32 c;
		memcpy(&c, from + i, sizeof(c));
		GridU32 const out = gridChannels8888(c, keepRb, keepG4) + gap;
		memcpy(to + i, &out, sizeof(out));
	}
	for (; i < count; ++i) {
		to[i] = gridChannels8888One(from[i], keepR, keepG, keepB) + gap;
	}
}

#if defined(__AVX2__) && !defined(__aarch64__)
// Rows woven with AVX2, 32 bytes of each at a time. Elements of 2, 4 or 8 bytes are interleaved
// within each 128-bit half by unpacking, the halves then put in order; three rows of 4- or 8-byte
// elements by permuting each across the register and blending. Six and eight rows are three and
// four rows of pairs.

// a and b interleaved, size-byte elements, in order: first holds the first half of the pairs.
static inline void gridAvxPairs(__m256i a, __m256i b, int size, __m256i* first, __m256i* second) {
	__m256i low, high;
	if (size == 2) {
		low = _mm256_unpacklo_epi16(a, b);
		high = _mm256_unpackhi_epi16(a, b);
	} else if (size == 4) {
		low = _mm256_unpacklo_epi32(a, b);
		high = _mm256_unpackhi_epi32(a, b);
	} else {
		low = _mm256_unpacklo_epi64(a, b);
		high = _mm256_unpackhi_epi64(a, b);
	}
	*first = _mm256_permute2x128_si256(low, high, 0x20);
	*second = _mm256_permute2x128_si256(low, high, 0x31);
}

// Four rows: two levels of pairs. 64 bytes of elements of 2 * size bytes, in order, per pair level.
static inline void gridAvxFour(__m256i a, __m256i b, __m256i c, __m256i d, int size, __m256i* to) {
	__m256i ab0, ab1, cd0, cd1;
	gridAvxPairs(a, b, size, &ab0, &ab1);
	gridAvxPairs(c, d, size, &cd0, &cd1);
	gridAvxPairs(ab0, cd0, size * 2, &to[0], &to[1]);
	gridAvxPairs(ab1, cd1, size * 2, &to[2], &to[3]);
}

// Three rows of 4-byte elements, 8 each: a0 b0 c0 a1 b1 c1 ...
static inline void gridAvxThree32(__m256i a, __m256i b, __m256i c, __m256i* to) {
	__m256i const ia0 = _mm256_setr_epi32(0, 0, 0, 1, 0, 0, 2, 0), ib0 = _mm256_setr_epi32(0, 0, 0, 0, 1, 0, 0, 2), ic0 = _mm256_setr_epi32(0, 0, 0, 0, 0, 1, 0, 0);
	__m256i const ia1 = _mm256_setr_epi32(0, 3, 0, 0, 4, 0, 0, 5), ib1 = _mm256_setr_epi32(0, 0, 3, 0, 0, 4, 0, 0), ic1 = _mm256_setr_epi32(2, 0, 0, 3, 0, 0, 4, 0);
	__m256i const ia2 = _mm256_setr_epi32(0, 0, 6, 0, 0, 7, 0, 0), ib2 = _mm256_setr_epi32(5, 0, 0, 6, 0, 0, 7, 0), ic2 = _mm256_setr_epi32(0, 5, 0, 0, 6, 0, 0, 7);
	__m256i const b0 = _mm256_permutevar8x32_epi32(b, ib0), c0 = _mm256_permutevar8x32_epi32(c, ic0);
	to[0] = _mm256_blend_epi32(_mm256_blend_epi32(_mm256_permutevar8x32_epi32(a, ia0), b0, 0x92), c0, 0x24);
	to[1] = _mm256_blend_epi32(_mm256_blend_epi32(_mm256_permutevar8x32_epi32(a, ia1), _mm256_permutevar8x32_epi32(b, ib1), 0x24),
	                           _mm256_permutevar8x32_epi32(c, ic1), 0x49);
	to[2] = _mm256_blend_epi32(_mm256_blend_epi32(_mm256_permutevar8x32_epi32(a, ia2), _mm256_permutevar8x32_epi32(b, ib2), 0x49),
	                           _mm256_permutevar8x32_epi32(c, ic2), 0x92);
}

// Three rows of 8-byte elements, 4 each: a0 b0 c0 a1 | b1 c1 a2 b2 | c2 a3 b3 c3.
static inline void gridAvxThree64(__m256i a, __m256i b, __m256i c, __m256i* to) {
	__m256i const ia0 = _mm256_setr_epi32(0, 1, 0, 0, 0, 0, 2, 3), ib0 = _mm256_setr_epi32(0, 0, 0, 1, 0, 0, 0, 0), ic0 = _mm256_setr_epi32(0, 0, 0, 0, 0, 1, 0, 0);
	__m256i const ia1 = _mm256_setr_epi32(0, 0, 0, 0, 4, 5, 0, 0), ib1 = _mm256_setr_epi32(2, 3, 0, 0, 0, 0, 4, 5), ic1 = _mm256_setr_epi32(0, 0, 2, 3, 0, 0, 0, 0);
	__m256i const ia2 = _mm256_setr_epi32(0, 0, 6, 7, 0, 0, 0, 0), ib2 = _mm256_setr_epi32(0, 0, 0, 0, 6, 7, 0, 0), ic2 = _mm256_setr_epi32(4, 5, 0, 0, 0, 0, 6, 7);
	to[0] = _mm256_blend_epi32(_mm256_blend_epi32(_mm256_permutevar8x32_epi32(a, ia0), _mm256_permutevar8x32_epi32(b, ib0), 0x0C),
	                           _mm256_permutevar8x32_epi32(c, ic0), 0x30);
	to[1] = _mm256_blend_epi32(_mm256_blend_epi32(_mm256_permutevar8x32_epi32(a, ia1), _mm256_permutevar8x32_epi32(b, ib1), 0xC3),
	                           _mm256_permutevar8x32_epi32(c, ic1), 0x0C);
	to[2] = _mm256_blend_epi32(_mm256_blend_epi32(_mm256_permutevar8x32_epi32(a, ia2), _mm256_permutevar8x32_epi32(b, ib2), 0x30),
	                           _mm256_permutevar8x32_epi32(c, ic2), 0xC3);
}

static inline __m256i gridAvxLoad(const uint8_t* row, unsigned at, int size) {
	return _mm256_loadu_si256((const __m256i*) (row + (size_t) at * size));
}

// Weaves what it can; returns how many of each row it did.
static unsigned gridWeaveAvx2(uint8_t* to, const uint8_t* const* from, int s, unsigned count, int size) {
	unsigned const step = 32 / size;
	unsigned done = 0;
	for (; done + step <= count; done += step) {
		__m256i* const t = (__m256i*) (to + (size_t) done * s * size);
		__m256i r[8];
		if (s == 2) {
			gridAvxPairs(gridAvxLoad(from[0], done, size), gridAvxLoad(from[1], done, size), size, &r[0], &r[1]);
			_mm256_storeu_si256(t, r[0]);
			_mm256_storeu_si256(t + 1, r[1]);
		} else if (s == 4) {
			gridAvxFour(gridAvxLoad(from[0], done, size), gridAvxLoad(from[1], done, size), gridAvxLoad(from[2], done, size),
			            gridAvxLoad(from[3], done, size), size, r);
			for (int k = 0; k < 4; ++k) {
				_mm256_storeu_si256(t + k, r[k]);
			}
		} else if (s == 3 && size == 4) {
			gridAvxThree32(gridAvxLoad(from[0], done, 4), gridAvxLoad(from[1], done, 4), gridAvxLoad(from[2], done, 4), r);
			for (int k = 0; k < 3; ++k) {
				_mm256_storeu_si256(t + k, r[k]);
			}
		} else if (s == 6 && (size == 2 || size == 4)) {
			// Three rows of pairs, each pair row two registers long.
			__m256i p[3][2];
			for (int k = 0; k < 3; ++k) {
				gridAvxPairs(gridAvxLoad(from[k * 2], done, size), gridAvxLoad(from[k * 2 + 1], done, size), size, &p[k][0], &p[k][1]);
			}
			for (int h = 0; h < 2; ++h) {
				if (size == 2) {
					gridAvxThree32(p[0][h], p[1][h], p[2][h], r + h * 3);
				} else {
					gridAvxThree64(p[0][h], p[1][h], p[2][h], r + h * 3);
				}
			}
			for (int k = 0; k < 6; ++k) {
				_mm256_storeu_si256(t + k, r[k]);
			}
		} else if (s == 8 && size == 2) {
			// Four rows of pairs.
			__m256i p[4][2];
			for (int k = 0; k < 4; ++k) {
				gridAvxPairs(gridAvxLoad(from[k * 2], done, size), gridAvxLoad(from[k * 2 + 1], done, size), size, &p[k][0], &p[k][1]);
			}
			for (int h = 0; h < 2; ++h) {
				gridAvxFour(p[0][h], p[1][h], p[2][h], p[3][h], size * 2, r + h * 4);
			}
			for (int k = 0; k < 8; ++k) {
				_mm256_storeu_si256(t + k, r[k]);
			}
		} else {
			break;
		}
	}
	return done;
}
#endif

// gridWeave16 and 32: rows woven together, to[x * s + k] = from[k][x], for s rows of count
// elements of that many bits. On 64-bit ARM the interleaving stores do it, two to four rows at once,
// and six or eight as three or four rows of pairs zipped into elements twice as wide; with AVX2,
// gridWeaveAvx2; what is left, one element at a time. bits, lanes: the element and how many fill a
// NEON register; wide, wideLanes: the same for a pair.
#if defined(__aarch64__)
#define GRID_WEAVE_VECTOR(bits, lanes, wide, wideLanes)                                                                       \
	if (s == 2 || s == 3 || s == 4) {                                                                                         \
		for (; done + lanes <= count; done += lanes) {                                                                        \
			uint##bits##x##lanes##_t const a = vld1q_u##bits(from[0] + done), b = vld1q_u##bits(from[1] + done);             \
			if (s == 2) {                                                                                                     \
				vst2q_u##bits(to + done * 2, ((uint##bits##x##lanes##x2_t) { { a, b } }));                                    \
			} else if (s == 3) {                                                                                              \
				vst3q_u##bits(to + done * 3, ((uint##bits##x##lanes##x3_t) { { a, b, vld1q_u##bits(from[2] + done) } }));     \
			} else {                                                                                                          \
				vst4q_u##bits(to + done * 4,                                                                                  \
				              ((uint##bits##x##lanes##x4_t) { { a, b, vld1q_u##bits(from[2] + done), vld1q_u##bits(from[3] + done) } })); \
			}                                                                                                                 \
		}                                                                                                                     \
	} else if (s == 6 || s == 8) {                                                                                            \
		for (; done + lanes <= count; done += lanes) {                                                                        \
			uint##wide##x##wideLanes##_t lows[4], highs[4];                                                                   \
			for (int k = 0; k < s / 2; ++k) {                                                                                 \
				uint##bits##x##lanes##_t const a = vld1q_u##bits(from[k * 2] + done), b = vld1q_u##bits(from[k * 2 + 1] + done); \
				lows[k] = vreinterpretq_u##wide##_u##bits(vzip1q_u##bits(a, b));                                              \
				highs[k] = vreinterpretq_u##wide##_u##bits(vzip2q_u##bits(a, b));                                             \
			}                                                                                                                 \
			uint##wide##_t* const t = (uint##wide##_t*) (to + done * s);                                                      \
			if (s == 6) {                                                                                                     \
				vst3q_u##wide(t, ((uint##wide##x##wideLanes##x3_t) { { lows[0], lows[1], lows[2] } }));                       \
				vst3q_u##wide(t + 3 * wideLanes, ((uint##wide##x##wideLanes##x3_t) { { highs[0], highs[1], highs[2] } }));    \
			} else {                                                                                                          \
				vst4q_u##wide(t, ((uint##wide##x##wideLanes##x4_t) { { lows[0], lows[1], lows[2], lows[3] } }));              \
				vst4q_u##wide(t + 4 * wideLanes, ((uint##wide##x##wideLanes##x4_t) { { highs[0], highs[1], highs[2], highs[3] } })); \
			}                                                                                                                 \
		}                                                                                                                     \
	}
#elif defined(__AVX2__)
#define GRID_WEAVE_VECTOR(bits, lanes, wide, wideLanes) done = gridWeaveAvx2((uint8_t*) to, (const uint8_t* const*) from, s, count, bits / 8)
#else
#define GRID_WEAVE_VECTOR(bits, lanes, wide, wideLanes)
#endif

#define GRID_WEAVE(bits, lanes, wide, wideLanes)                                                                     \
	static void gridWeave##bits(uint##bits##_t* to, const uint##bits##_t* const* from, int s, unsigned count) { \
		unsigned done = 0;                                                                                       \
		GRID_WEAVE_VECTOR(bits, lanes, wide, wideLanes);                                                         \
		for (int k = 0; k < s; ++k) {                                                                            \
			for (unsigned x = done; x < count; ++x) {                                                            \
				to[x * s + k] = from[k][x];                                                                      \
			}                                                                                                    \
		}                                                                                                        \
	}
GRID_WEAVE(16, 8, 32, 4)
GRID_WEAVE(32, 4, 64, 2)

// A buffer kept from frame to frame, grown when a frame needs more.
struct GridBuffer {
	uint8_t* data;
	size_t size;
};

// The rows are shared out among up to this many threads, the core's own and workers (workers.h)
// started for them unless linked consoles have them already; each has its own scratch rows. Auto
// leaves a core for the frontend's own threads, and one for each of Threaded Video's threads, which
// wait on cores of their own as the frame ends: on an RK3566, 3 threads, or 2, were the fastest.
enum { GRID_JOBS = 4 };
static int postJobs = 1;

// The larger picture; for each thread, the block's source rows blended for each of its blends, each row
// of a block unlike those above it, and at a half step a row, its gaps and its shadow split in halves; the shadow's darkness at the source's size, with 8 more on each side
// and 2 more rows above and below; for each thread, a row of each square's share kept from the shadow.
// Rows of no darkness above and below the shadow's picture of darkness, as far as the farthest shadow
// reads past it; 8 columns each side.
enum { DARK_EDGE = 3 };
static struct GridBuffer gridOut, gridBlends, gridTemplates, gridSplits, gridDark, gridKeeps, gridKeptRows;

// Whether b holds wanted bytes, growing it if it didn't.
static bool gridReserve(struct GridBuffer* b, size_t wanted) {
	if (wanted > b->size) {
		uint8_t* const grown = realloc(b->data, wanted);
		if (!grown) {
			return false;
		}
		b->data = grown;
		b->size = wanted;
	}
	return true;
}

enum { GRID_SHARES_MAX = 256, GRID_BLOCK_MAX = 16 };

// A block's layout for a scale, in halves, a screen and an LCD Subpixels mix: period source pixels
// each way, 1, or 2 at a half step, making block output pixels each way. The different blends its
// pixels have, each a share of gap, 0 to 255, and with LCD Subpixels each channel's factor from its
// column's stripes, as a share of 256 of the gamma-encoded value; each pixel's, as an index into them;
// the source pixel in the block each output column and row shows; and each row's first row alike, as
// rows alike share a template.
static struct {
	int halves, lcd, period, block, shareCount;
	bool gaps;
	float mix;
	uint8_t shares[GRID_SHARES_MAX];
	uint16_t channels[GRID_SHARES_MAX][3];
	uint8_t shareOf[GRID_BLOCK_MAX][GRID_BLOCK_MAX];
	uint8_t source[GRID_BLOCK_MAX];
	int firstAlike[GRID_BLOCK_MAX];
	// For each source row of the block, which halves of it each blend is woven from: 1 the even
	// pixels', 2 the odd ones', 3 both (at a whole scale, the row).
	uint8_t used[2][GRID_SHARES_MAX];
} gridLayout = { .lcd = -1 };

static void gridLay(int halves, enum LibretroLcd lcd) {
	float const pitch = halves / 2.0f;
	float const mix = libretroLcdHasSubpixels(lcd) ? libretroLcdSubpixelMix * libretroLcdSubpixelFade(pitch) : 0;
	if (gridLayout.halves == halves && gridLayout.lcd == (int) lcd && gridLayout.gaps == gridGaps && gridLayout.mix == mix) {
		return;
	}
	int const period = halves % 2 ? 2 : 1, block = halves * period / 2;
	const struct LibretroLcdLook* const look = &libretroLcdLooks[lcd];
	int across[GRID_BLOCK_MAX], down[GRID_BLOCK_MAX];
	uint16_t columns[GRID_BLOCK_MAX][3];
	for (int i = 0; i < block; ++i) {
		float const covered = gridGaps ? libretroLcdGapCoverage(i, pitch, look->share * pitch) : 0;
		across[i] = (int) lroundf(covered * look->across * 255);
		down[i] = (int) lroundf(covered * look->down * 255);
		gridLayout.source[i] = (uint8_t) ((i + 0.5f) / pitch);
	}
	// Each pixel's stripes across the output columns it takes, two or three at a half step, so that
	// they add up to its color in each pixel, as bands (libretroLcdBandShare).
	int first[GRID_BLOCK_MAX], taken[GRID_BLOCK_MAX], bands = block;
	for (int i = 0; i < block; ++i) {
		first[i] = i > 0 && gridLayout.source[i - 1] == gridLayout.source[i] ? first[i - 1] : i;
	}
	for (int i = block - 1; i >= 0; --i) {
		taken[i] = i + 1 < block && gridLayout.source[i + 1] == gridLayout.source[i] ? taken[i + 1] : i - first[i] + 1;
		bands = taken[i] < bands ? taken[i] : bands;
	}
	for (int i = 0; i < block; ++i) {
		float const from = (float) (i - first[i]) / taken[i], to = (float) (i - first[i] + 1) / taken[i];
		for (int c = 0; c < 3; ++c) {
			float const share = libretroLcdBandShare(from, to, bands, libretroLcdStripeOf(lcd, c));
			columns[i][c] = (uint16_t) lroundf(powf(libretroLcdStripeFactor(share, mix), 1 / 2.2f) * 256);
		}
	}
	gridLayout.shareCount = 0;
	for (int y = 0; y < block; ++y) {
		for (int x = 0; x < block; ++x) {
			int const a = across[x] + down[y] - (across[x] * down[y] + 127) / 255;
			int k = 0;
			while (k < gridLayout.shareCount && (gridLayout.shares[k] != a || memcmp(gridLayout.channels[k], columns[x], sizeof(columns[x])))) {
				++k;
			}
			if (k == gridLayout.shareCount) {
				gridLayout.shares[gridLayout.shareCount] = (uint8_t) a;
				memcpy(gridLayout.channels[gridLayout.shareCount++], columns[x], sizeof(columns[x]));
			}
			gridLayout.shareOf[y][x] = (uint8_t) k;
		}
	}
	memset(gridLayout.used, 0, sizeof(gridLayout.used));
	for (int y = 0; y < block; ++y) {
		for (int x = 0; x < block; ++x) {
			gridLayout.used[gridLayout.source[y]][gridLayout.shareOf[y][x]] |= (uint8_t) (1 << gridLayout.source[x]);
		}
	}
	for (int y = 0; y < block; ++y) {
		gridLayout.firstAlike[y] = y;
		for (int k = 0; k < y && gridLayout.firstAlike[y] == y; ++k) {
			bool const alike = gridLayout.source[k] == gridLayout.source[y] && !memcmp(gridLayout.shareOf[k], gridLayout.shareOf[y], (size_t) block);
			gridLayout.firstAlike[y] = alike ? k : y;
		}
	}
	gridLayout.halves = halves;
	gridLayout.lcd = lcd;
	gridLayout.gaps = gridGaps;
	gridLayout.period = period;
	gridLayout.block = block;
	gridLayout.mix = mix;
}

// Keeping a Game Boy screen's brightness under the grid. A camera out of focus sees each pixel with
// the gap around it as one color, and the measured screens are those colors, so drawing the gaps over
// them would count the gaps twice: dark shades would come out far lighter. Instead each pixel's own
// color is darkened, through a table for each channel, until its square, averaged in linear light, is
// the color it shows without the grid (BGB's LCD filters keep it too). The gap is the unlit screen's
// color, darker only as far as the darkest shade needs; Grayscale's, whose white and black no one gap
// keeps, light gray. A pixel too dark for the gap has its own gaps darken, as a second table for each
// channel has them: a driven pixel's field reaches into the gaps beside it. Grayscale's white, too
// bright for light gray gaps, is the one color whose square comes out darker, a tenth, so the grid
// still shows on it. The Color's and Advance's black gaps instead lighten beside colors too bright for
// them, only until the square is nine tenths of the color, so the grid shows on white there too.
static float keepLinear[256];

// A square's average in linear light with level inside drawn and gap in the gaps, blended by each of
// keep's shares.
static float keepSquare(const struct LibretroLcdKeep* keep, const float* weights, float total, int inside, int gap) {
	float sum = 0;
	for (int i = 0; i < keep->count; ++i) {
		float const share = keep->shares[i];
		sum += weights[i] * keepLinear[(int) lroundf(inside * (1 - share) + gap * share)];
	}
	return sum / total;
}

// For an RGB565 picture: the same with the inside at a 5- or 6-bit level, each pixel blended in 8 bits
// and rounded back to bits, as the 565 blends draw it.
static float keepSquare565(const struct LibretroLcdKeep* keep, const float* weights, float total, int level, int gap, int bits) {
	int const top = (1 << bits) - 1, wide = bits == 5 ? level << 3 | level >> 2 : level << 2 | level >> 4;
	float sum = 0;
	for (int i = 0; i < keep->count; ++i) {
		int const a = (int) lroundf(keep->shares[i] * 255);
		int const mixed = GRID_DIV255(wide * (255 - a) + gap * a), q = GRID_DIV255(mixed * top);
		sum += weights[i] * keepLinear[bits == 5 ? q << 3 | q >> 2 : q << 2 | q >> 4];
	}
	return sum / total;
}

// The level, of the inside or of the gap with the other fixed, whose square comes nearest want.
static int keepNearest(const struct LibretroLcdKeep* keep, const float* weights, float total, float want, int fixed, bool gapVaries) {
	int lo = 0, hi = 255;
	while (lo < hi) {
		int const mid = (lo + hi) / 2;
		if ((gapVaries ? keepSquare(keep, weights, total, fixed, mid) : keepSquare(keep, weights, total, mid, fixed)) < want) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	if (lo) {
		float const below = gapVaries ? keepSquare(keep, weights, total, fixed, lo - 1) : keepSquare(keep, weights, total, lo - 1, fixed);
		float const at = gapVaries ? keepSquare(keep, weights, total, fixed, lo) : keepSquare(keep, weights, total, lo, fixed);
		lo -= want - below < at - want;
	}
	return lo;
}

bool libretroLcdKeep(struct LibretroLcdKeep* keep) {
	uint32_t off;
	enum LibretroLcd const lcd = libretroLcd(&off);
	bool const shades = lcd == LIBRETRO_LCD_DMG || lcd == LIBRETRO_LCD_POCKET;
	if (!shades && lcd != LIBRETRO_LCD_COLOR && lcd != LIBRETRO_LCD_ADVANCE) {
		return false;
	}
	uint32_t colors[5] = { off };
	for (int k = 0; shades && k < 4; ++k) {
		colors[k + 1] = libretroGbShade(k);
	}
	if (!keep->stale && keep->lcd == (int) lcd && !memcmp(keep->colors, colors, sizeof(colors))) {
		return true;
	}
	if (!keepLinear[255]) {
		for (int v = 0; v < 256; ++v) {
			float const c = v / 255.0f;
			keepLinear[v] = c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
		}
	}
	uint32_t const base = shades && gbShades == 0 ? 0xE0E0E0 : off;
	uint32_t gap = 0;
	float totals[3];
	for (int c = 0; c < 3; ++c) {
		// With LCD Subpixels, each channel's square is weighed by its own stripes' factors, which sit
		// under the gaps for some channels and between them for others.
		const float* const weights = keep->perChannel ? keep->channelWeights[c] : keep->weights;
		totals[c] = 0;
		for (int i = 0; i < keep->count; ++i) {
			totals[c] += weights[i];
		}
		float const total = totals[c];
		int const bits = 16 - 8 * c;
		int best = (int) (base >> bits & 0xFF);
		for (int g = best; shades && g >= 0; --g) {
			float const low = keepSquare(keep, weights, total, 0, g), high = keepSquare(keep, weights, total, 255, g);
			float miss = 0;
			for (int k = 0; k < 5; ++k) {
				float const want = keepLinear[colors[k] >> bits & 0xFF];
				miss = fmaxf(miss, fmaxf(low - want, want - high));
			}
			if (miss == 0) {
				best = g;
				break;
			}
		}
		gap |= (uint32_t) best << bits;
		float const darkest = keepSquare(keep, weights, total, 0, best), lightest = keepSquare(keep, weights, total, 255, best);
		for (int v = 0; v < 256; ++v) {
			float const want = keepLinear[v];
			if (want > lightest) {
				keep->table[c][v] = 255;
				// The black matrix lightens beside such a color only until the square is nine tenths of it,
				// so the grid still shows on white.
				keep->gaps[c][v] = (uint8_t) (shades ? best : keepNearest(keep, weights, total, fmaxf(want * 0.9f, lightest), 255, true));
			} else if (want < darkest) {
				keep->table[c][v] = 0;
				keep->gaps[c][v] = (uint8_t) keepNearest(keep, weights, total, want, 0, true);
			} else {
				keep->table[c][v] = (uint8_t) keepNearest(keep, weights, total, want, best, false);
				keep->gaps[c][v] = (uint8_t) best;
			}
		}
	}
	// For RGB565 pictures, an inside between none and full is the level whose square, as the 565 blends
	// draw it, comes nearest: rounding the 8-bit one would leave the square up to half a level off, and
	// a gap beside it more.
	for (int c = 0; c < 3; ++c) {
		const float* const weights = keep->perChannel ? keep->channelWeights[c] : keep->weights;
		float const total = totals[c];
		int const bits = c == 1 ? 6 : 5, shift = c == 0 ? 11 : c == 1 ? 5 : 0, top = (1 << bits) - 1;
		int const gapLevel = (int) (gap >> (16 - 8 * c) & 0xFF);
		for (int v = 0; v <= top; ++v) {
			int const wide = bits == 5 ? v << 3 | v >> 2 : v << 2 | v >> 4;
			int level = (keep->table[c][wide] * top + 127) / 255;
			if (keep->table[c][wide] > 0 && keep->table[c][wide] < 255) {
				float const want = keepLinear[wide];
				float nearest = 2;
				for (int l = 0; l <= top; ++l) {
					float const miss = fabsf(keepSquare565(keep, weights, total, l, gapLevel, bits) - want);
					level = miss < nearest ? l : level;
					nearest = miss < nearest ? miss : nearest;
				}
			}
			keep->table565[0][c][v] = (uint16_t) (level << shift);
			keep->table565[1][c][v] = (uint16_t) ((keep->gaps[c][wide] * top + 127) / 255 << shift);
		}
	}
	keep->gap = gap;
	keep->lcd = (int) lcd;
	memcpy(keep->colors, colors, sizeof(colors));
	keep->stale = false;
	++keep->solved;
	return true;
}

// The CPU grid's, for the scale and screen its shares are from.
static struct LibretroLcdKeep gridKept;
static int keptHalves, keptLcd = -1;
static bool keptGaps;
static float keptMix;

// The frame the threads draw.
static struct GridFrame {
	const uint8_t* picture;
	unsigned width, height;
	size_t pitch, bytes;
	uint32_t gap;
	// Whether each row goes through gridKept's tables first.
	bool kept;
	bool shadow;
	// Where the shadow on a square comes from: the source pixel above and left of that point, as
	// offsets, and the weights of the ones after; the darkness from start down, by scale / 65536.
	int fromX, fromY, weightX, weightY, start, scale;
} gridFrame;

// The rows of height a thread draws: an even share of them.
static void rowsOf(int job, unsigned height, unsigned* from, unsigned* to) {
	*from = height * (unsigned) job / (unsigned) postJobs;
	*to = height * (unsigned) (job + 1) / (unsigned) postJobs;
}

// The threads for this frame's post-processing, and workers for them unless linked consoles' are
// running.
static void startJobs(void) {
	static long cores;
	if (!cores) {
		cores = sysconf(_SC_NPROCESSORS_ONLN);
	}
	int const spare = (int) cores - 1 - (threadedVideo ? videoThreads : 0);
	postJobs = lcdThreads ? lcdThreads : spare > GRID_JOBS ? GRID_JOBS : spare < 1 ? 1 : spare;
	if (!gbaTogether && workersAsked() != (postJobs > 1 ? postJobs : 0)) {
		if (postJobs > 1) {
			workersStart(postJobs);
		} else {
			workersStop();
		}
	}
}

// Runs job for each post-processing thread.
static void runJobs(void (*job)(int index)) {
	if (postJobs > 1) {
		workersRun(job, postJobs);
	} else {
		job(0);
	}
}

// Each pixel's darkness, in a row of count XRGB8888 pixels: 0 down to start, in luminance, then rising
// by scale / 65536 a level; returns whether any is dark.
static bool gridDarkRow(const uint32_t* row, uint8_t* to, unsigned count, int start, int scale) {
	bool any = false;
	for (unsigned x = 0; x < count; ++x) {
		int const level = libretroLcdLuminanceOf(row[x]);
		int const dark = level < start ? (int) ((int64_t) (start - level) * scale >> 16) : 0;
		to[x] = (uint8_t) (dark < 255 ? dark : 255);
		any |= to[x] != 0;
	}
	return any;
}

// A thread's rows of the shadow's darkness map.
static void gridDarkJob(int job) {
	unsigned from, to;
	rowsOf(job, gridFrame.height, &from, &to);
	for (unsigned y = from; y < to; ++y) {
		uint8_t* const d = gridDark.data + (size_t) (y + DARK_EDGE) * (gridFrame.width + 16) + 8;
		gridDarkRow((const uint32_t*) (gridFrame.picture + y * gridFrame.pitch), d, gridFrame.width, gridFrame.start, gridFrame.scale);
	}
}

// How much of each square of source row y the shadow leaves, 255 for all: the darkness map sampled
// smoothly where the shadow over the square's middle comes from, beyond the picture none.
static void gridKeepRow(unsigned y, uint8_t* keep) {
	unsigned const width = gridFrame.width;
	size_t const stride = width + 16;
	const uint8_t* const top = gridDark.data + (size_t) (y + DARK_EDGE + gridFrame.fromY) * stride + 8 + gridFrame.fromX;
	const uint8_t* const bottom = top + stride;
	uint16_t const right = (uint16_t) gridFrame.weightX, left = (uint16_t) (256 - right);
	uint16_t const below = (uint16_t) gridFrame.weightY, above = (uint16_t) (256 - below);
	unsigned x = 0;
	for (; x + GRID_LANES16 <= width; x += GRID_LANES16) {
		GridBytes16 a, b, c, d;
		memcpy(&a, top + x, sizeof(a));
		memcpy(&b, top + x + 1, sizeof(b));
		memcpy(&c, bottom + x, sizeof(c));
		memcpy(&d, bottom + x + 1, sizeof(d));
		GridU16 const upper = (__builtin_convertvector(a, GridU16) * left + __builtin_convertvector(b, GridU16) * right) >> 8;
		GridU16 const lower = (__builtin_convertvector(c, GridU16) * left + __builtin_convertvector(d, GridU16) * right) >> 8;
		GridBytes16 const kept = __builtin_convertvector(255 - ((upper * above + lower * below) >> 8), GridBytes16);
		memcpy(keep + x, &kept, sizeof(kept));
	}
	for (; x < width; ++x) {
		unsigned const upper = (top[x] * left + top[x + 1] * right) >> 8, lower = (bottom[x] * left + bottom[x + 1] * right) >> 8;
		keep[x] = (uint8_t) (255 - ((upper * above + lower * below) >> 8));
	}
}

// A blended XRGB8888 row darkened by the shadow: each byte times keep + 1, of 256. Runs the shadow
// leaves alone are passed over.
static void gridShadeRow(uint32_t* row, const uint8_t* keep, unsigned width) {
	unsigned x = 0;
#if defined(__aarch64__)
	// 16 pixels at a time: each pixel's keep copied to its four bytes by a table lookup, then 8-bit
	// multiplies that widen to 16 bits.
	static const uint8_t spreads[4][16] = {
		{ 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3 },
		{ 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7 },
		{ 8, 8, 8, 8, 9, 9, 9, 9, 10, 10, 10, 10, 11, 11, 11, 11 },
		{ 12, 12, 12, 12, 13, 13, 13, 13, 14, 14, 14, 14, 15, 15, 15, 15 },
	};
	for (; x + 16 <= width; x += 16) {
		uint8x16_t const kept = vld1q_u8(keep + x);
		if (vminvq_u8(kept) == 255) {
			continue;
		}
		for (int q = 0; q < 4; ++q) {
			uint8_t* const at = (uint8_t*) (row + x + q * 4);
			uint8x16_t const c = vld1q_u8(at), k = vqtbl1q_u8(kept, vld1q_u8(spreads[q]));
			uint16x8_t const low = vaddw_u8(vmull_u8(vget_low_u8(c), vget_low_u8(k)), vget_low_u8(c));
			uint16x8_t const high = vaddw_high_u8(vmull_high_u8(c, k), c);
			vst1q_u8(at, vshrn_high_n_u16(vshrn_n_u16(low, 8), high, 8));
		}
	}
#else
	for (; x + GRID_LANES32 <= width; x += GRID_LANES32) {
		uint64_t all = 0;
		memcpy(&all, keep + x, GRID_LANES32);
		if (all == ~0ull >> (64 - 8 * GRID_LANES32)) {
			continue;
		}
		GridBytes32 k;
		memcpy(&k, keep + x, sizeof(k));
		GridU32 const keeps = __builtin_convertvector(k, GridU32);
		GridU32 c;
		memcpy(&c, row + x, sizeof(c));
		GridU32 const out = gridScale8888(c, keeps + 1);
		memcpy(row + x, &out, sizeof(out));
	}
#endif
	for (; x < width; ++x) {
		row[x] = gridScale8888One(row[x], keep[x] + 1u);
	}
}

// One source row of the block, row (0, or 1 at a half step), blended for each of the blends its rows
// use, in the halves they use, darkened by the shadow when there is one.
static void gridBlendRow(int row, const uint8_t* pixels, const uint8_t* gaps, uint8_t* blends, const uint8_t* keep, unsigned width) {
	unsigned const gap = gridFrame.gap;
	unsigned const gapR = gap >> 16 & 0xFF, gapG = gap >> 8 & 0xFF, gapB = gap & 0xFF;
	size_t const bytes = gridFrame.bytes, rowBytes = width * bytes;
	unsigned const half = width / 2;
	for (int k = 0; k < gridLayout.shareCount; ++k) {
		int const used = gridLayout.used[row][k];
		if (!used) {
			continue;
		}
		// The even pixels' half, the odd ones', or the whole row.
		unsigned const at = used == 2 ? half : 0, count = used == 3 || gridLayout.period == 1 ? width : half;
		const uint8_t* const in = pixels + at * bytes;
		const uint8_t* const around = gaps ? gaps + at * bytes : NULL;
		void* const blended = blends + k * rowBytes + at * bytes;
		const uint16_t* const channels = gridLayout.channels[k];
		unsigned const a = gridLayout.shares[k];
		if (!a && !gridTinted(channels)) {
			memcpy(blended, in, count * bytes);
		} else if (around && bytes == 4) {
			gridBlendGaps8888((const uint32_t*) in, (const uint32_t*) around, blended, count, a, channels);
		} else if (around) {
			gridBlendGaps565((const uint16_t*) in, (const uint16_t*) around, blended, count, a, channels);
		} else if (bytes == 4) {
			gridBlend8888((const uint32_t*) in, blended, count, a, gapR, gapG, gapB, channels);
		} else {
			gridBlend565((const uint16_t*) in, blended, count, a, gapR, gapG, gapB, channels);
		}
		if (keep) {
			gridShadeRow(blended, keep + at, count);
		}
	}
}

// A row's pixels of size bytes put in two halves, the even ones then the odd ones.
static void gridSplit(const uint8_t* row, uint8_t* to, unsigned width, size_t size) {
	unsigned const half = width / 2;
	if (size == 4) {
		for (unsigned x = 0; x < width; ++x) {
			((uint32_t*) to)[(x & 1) * half + x / 2] = ((const uint32_t*) row)[x];
		}
	} else if (size == 2) {
		for (unsigned x = 0; x < width; ++x) {
			((uint16_t*) to)[(x & 1) * half + x / 2] = ((const uint16_t*) row)[x];
		}
	} else {
		for (unsigned x = 0; x < width; ++x) {
			to[(x & 1) * half + x / 2] = row[x];
		}
	}
}

// A thread's blocks of rows of the larger picture, each row from its template.
static void gridDrawJob(int job) {
	int const period = gridLayout.period, block = gridLayout.block, blendCount = gridLayout.shareCount;
	unsigned const width = gridFrame.width, count = width / (unsigned) period;
	size_t const bytes = gridFrame.bytes, rowBytes = width * bytes, half = count * bytes, outPitch = (size_t) count * block * bytes;
	unsigned from, to;
	rowsOf(job, gridFrame.height / (unsigned) period, &from, &to);
	uint8_t* const blends = gridBlends.data + (size_t) job * period * blendCount * rowBytes;
	uint8_t* const templates = gridTemplates.data + (size_t) job * block * outPitch;
	uint8_t* const split = gridSplits.data + (size_t) job * 3 * rowBytes;
	uint8_t* const keep = gridFrame.shadow ? gridKeeps.data + (size_t) job * width : NULL;
	uint8_t* const kept = gridFrame.kept ? gridKeptRows.data + (size_t) job * 2 * rowBytes : NULL;
	uint8_t* const gaps = kept ? kept + rowBytes : NULL;
	for (unsigned group = from; group < to; ++group) {
		for (int i = 0; i < period; ++i) {
			unsigned const y = group * (unsigned) period + (unsigned) i;
			if (keep) {
				gridKeepRow(y, keep);
			}
			const uint8_t* row = gridFrame.picture + y * gridFrame.pitch;
			if (kept && bytes == 2) {
				const uint16_t* const in = (const uint16_t*) row;
				const uint16_t(*const inside)[64] = gridKept.table565[0];
				const uint16_t(*const around)[64] = gridKept.table565[1];
				for (unsigned x = 0; x < width; ++x) {
					unsigned const c = in[x], r = c >> 11, g = c >> 5 & 0x3F, b = c & 0x1F;
					((uint16_t*) kept)[x] = (uint16_t) (inside[0][r] | inside[1][g] | inside[2][b]);
					((uint16_t*) gaps)[x] = (uint16_t) (around[0][r] | around[1][g] | around[2][b]);
				}
				row = kept;
			} else if (kept) {
				for (size_t x = 0; x < rowBytes; x += 4) {
					kept[x] = gridKept.table[2][row[x]];
					kept[x + 1] = gridKept.table[1][row[x + 1]];
					kept[x + 2] = gridKept.table[0][row[x + 2]];
					kept[x + 3] = row[x + 3];
					gaps[x] = gridKept.gaps[2][row[x]];
					gaps[x + 1] = gridKept.gaps[1][row[x + 1]];
					gaps[x + 2] = gridKept.gaps[0][row[x + 2]];
					gaps[x + 3] = 0;
				}
				row = kept;
			}
			const uint8_t* rowGaps = gaps;
			const uint8_t* rowKeep = keep;
			if (period == 2) {
				gridSplit(row, split, width, bytes);
				row = split;
				if (rowGaps) {
					gridSplit(rowGaps, split + rowBytes, width, bytes);
					rowGaps = split + rowBytes;
				}
				if (rowKeep) {
					gridSplit(rowKeep, split + 2 * rowBytes, width, 1);
					rowKeep = split + 2 * rowBytes;
				}
			}
			gridBlendRow(i, row, rowGaps, blends + (size_t) i * blendCount * rowBytes, rowKeep, width);
		}
		for (int r = 0; r < block; ++r) {
			if (gridLayout.firstAlike[r] != r) {
				continue;
			}
			const uint8_t* const rowBlends = blends + (size_t) gridLayout.source[r] * blendCount * rowBytes;
			const void* columns[GRID_BLOCK_MAX];
			for (int j = 0; j < block; ++j) {
				columns[j] = rowBlends + gridLayout.shareOf[r][j] * rowBytes + gridLayout.source[j] * half;
			}
			uint8_t* const line = templates + r * outPitch;
			if (bytes == 4) {
				gridWeave32((uint32_t*) line, (const uint32_t* const*) columns, block, count);
			} else {
				gridWeave16((uint16_t*) line, (const uint16_t* const*) columns, block, count);
			}
		}
		for (int r = 0; r < block; ++r) {
			memcpy(gridOut.data + ((size_t) group * block + r) * outPitch, templates + (size_t) gridLayout.firstAlike[r] * outPitch, outPitch);
		}
	}
}

// The shadow's frame: where each square's shadow comes from, and the darkness map, none beside, above
// and below the picture.
static void prepareShadow(uint32_t gap, unsigned width, unsigned height) {
	libretroLcdShadowFrom(gap, libretroGbDarkestColor(), &gridFrame.start, &gridFrame.scale);
	int const way = gridWay - 1;
	float const reach = libretroLcdReach(way);
	float const x = -libretroLcdWays[way][0] * reach, y = -libretroLcdWays[way][1] * reach;
	gridFrame.fromX = (int) floorf(x);
	gridFrame.fromY = (int) floorf(y);
	gridFrame.weightX = (int) lroundf((x - gridFrame.fromX) * 256);
	gridFrame.weightY = (int) lroundf((y - gridFrame.fromY) * 256);
	size_t const stride = width + 16;
	memset(gridDark.data, 0, DARK_EDGE * stride);
	memset(gridDark.data + (height + DARK_EDGE) * stride, 0, DARK_EDGE * stride);
	for (unsigned row = 0; row < height; ++row) {
		memset(gridDark.data + (row + DARK_EDGE) * stride, 0, 8);
		memset(gridDark.data + (row + DARK_EDGE) * stride + 8 + width, 0, 8);
	}
	runJobs(gridDarkJob);
}

// LCD Scale's Fit Screen: the picture as large as fits gridFitWidth by gridFitHeight, to the output
// pixel, so the frontend shows it one to one. No block repeats at such a size, so the grid is laid out
// by output column and row instead: each column's source pixel, share of gap across and LCD Subpixels'
// factors, and each row's source row and share of gap down. For each source row, two rows are made at the output's width: its pixels with
// the column gaps and stripes, and its gaps with the stripes, both darkened by the shadow. Each output
// row is then the first moved towards the second by its row's share of gap, since a pixel's share of
// gap is across + down - across * down; rows with none are copies.
static struct {
	unsigned width, height, outWidth, outHeight;
	int lcd;
	bool gaps;
	float mix;
	uint16_t *columnFrom, *rowFrom;
	uint8_t *across, *down;
	// Each column's share of gap and stripe factors, as 16- and 32-bit lanes for the vector loops.
	uint16_t *across16, *tint16[3];
	uint32_t *across32, *tint32[3];
	bool tinted;
} fit = { .lcd = -1 };

float libretroLcdShownScale = 1;
unsigned libretroLcdScreenWidth, libretroLcdScreenHeight;

void libretroLcdFitSize(unsigned width, unsigned height, unsigned* outWidth, unsigned* outHeight) {
	float const scale = fminf((float) gridFitWidth / width, (float) gridFitHeight / height);
	unsigned const w = (unsigned) lroundf(width * scale), h = (unsigned) lroundf(height * scale);
	*outWidth = w < width ? width : w > gridFitWidth ? gridFitWidth : w;
	*outHeight = h < height ? height : h > gridFitHeight ? gridFitHeight : h;
}

// One way's layout: for each of count output pixels, the source pixel it shows and its share of gap,
// 0 to 255; pitch output pixels a source pixel. Each gap is centered on the output pixel edge where
// one source pixel gives way to the next, the picture's own edges too, as the whole scales have them,
// so every gap is drawn alike however the source pixels fall.
static void fitLine(unsigned count, float pitch, float share, float strength, uint16_t* from, uint8_t* gap) {
	for (unsigned i = 0; i < count; ++i) {
		from[i] = (uint16_t) ((i + 0.5f) / pitch);
	}
	float const half = share * pitch / 2;
	for (unsigned i = 0, first = 0; i < count; ++i) {
		first = i && from[i - 1] != from[i] ? i : first;
		unsigned next = i + 1;
		while (next < count && from[next] == from[i]) {
			++next;
		}
		float const covered = fminf(fmaxf(half - (float) (i - first), 0), 1) + fminf(fmaxf(half - (float) (next - i - 1), 0), 1);
		gap[i] = gridGaps ? (uint8_t) lroundf(fminf(covered, 1) * strength * 255) : 0;
	}
}

static bool fitLay(unsigned width, unsigned height, enum LibretroLcd lcd) {
	unsigned outWidth, outHeight;
	libretroLcdFitSize(width, height, &outWidth, &outHeight);
	float const pitchX = (float) outWidth / width, pitchY = (float) outHeight / height;
	float const mix = libretroLcdHasSubpixels(lcd) ? libretroLcdSubpixelMix * libretroLcdSubpixelFade(pitchX) : 0;
	if (fit.width == width && fit.height == height && fit.outWidth == outWidth && fit.outHeight == outHeight && fit.lcd == (int) lcd &&
	    fit.gaps == gridGaps && fit.mix == mix) {
		return true;
	}
	void** const all[] = { (void**) &fit.columnFrom, (void**) &fit.rowFrom, (void**) &fit.across, (void**) &fit.down, (void**) &fit.across16,
		                   (void**) &fit.across32, (void**) &fit.tint16[0], (void**) &fit.tint16[1], (void**) &fit.tint16[2],
		                   (void**) &fit.tint32[0], (void**) &fit.tint32[1], (void**) &fit.tint32[2] };
	// Columns rounded up to whole vectors, so the vector loops need no tail.
	size_t const columns = (outWidth + GRID_LANES16) & ~(size_t) (GRID_LANES16 - 1);
	size_t const sizes[] = { columns * 2, outHeight * 2, columns, outHeight, columns * 2, columns * 4, columns * 2, columns * 2, columns * 2,
		                     columns * 4, columns * 4, columns * 4 };
	fit.width = 0;
	bool made = true;
	for (size_t i = 0; i < sizeof(all) / sizeof(*all); ++i) {
		free(*all[i]);
		*all[i] = calloc(sizes[i], 1);
		made &= *all[i] != NULL;
	}
	if (!made) {
		return false;
	}
	const struct LibretroLcdLook* const look = &libretroLcdLooks[lcd];
	fitLine(outWidth, pitchX, look->share, look->across, fit.columnFrom, fit.across);
	fitLine(outHeight, pitchY, look->share, look->down, fit.rowFrom, fit.down);
	// Each pixel's stripes across the columns it takes, as bands, as many as the narrowest pixel's
	// columns, leaving out the first and last, which rounding may trim (libretroLcdBandShare).
	unsigned bands = (unsigned) pitchX;
	for (unsigned x = 0, first = 0; x < outWidth; ++x) {
		bool const last = x + 1 == outWidth || fit.columnFrom[x + 1] != fit.columnFrom[x];
		if (last && fit.columnFrom[x] && fit.columnFrom[x] + 1u < width) {
			bands = x - first + 1 < bands ? x - first + 1 : bands;
		}
		first = last ? x + 1 : first;
	}
	bands = bands ? bands : 1;
	fit.tinted = false;
	for (unsigned x = 0, first = 0; x < outWidth; ++x) {
		first = x && fit.columnFrom[x - 1] != fit.columnFrom[x] ? x : first;
		unsigned end = x;
		while (end + 1 < outWidth && fit.columnFrom[end + 1] == fit.columnFrom[x]) {
			++end;
		}
		float const taken = (float) (end - first + 1), from = (x - first) / taken, to = (x - first + 1) / taken;
		for (int c = 0; c < 3; ++c) {
			float const share = libretroLcdBandShare(from, to, (int) bands, libretroLcdStripeOf(lcd, c));
			fit.tint16[c][x] = (uint16_t) lroundf(powf(libretroLcdStripeFactor(share, mix), 1 / 2.2f) * 256);
			fit.tint32[c][x] = fit.tint16[c][x];
			fit.tinted |= fit.tint16[c][x] != 256;
		}
		fit.across16[x] = fit.across[x];
		fit.across32[x] = fit.across[x];
	}
	fit.width = width;
	fit.height = height;
	fit.outWidth = outWidth;
	fit.outHeight = outHeight;
	fit.lcd = (int) lcd;
	fit.gaps = gridGaps;
	fit.mix = mix;
	// Keeping the Game Boy screens' brightness: how many output pixels take each share of gap.
	unsigned acrossCount[256] = { 0 }, downCount[256] = { 0 };
	for (unsigned x = 0; x < outWidth; ++x) {
		++acrossCount[fit.across[x]];
	}
	for (unsigned y = 0; y < outHeight; ++y) {
		++downCount[fit.down[y]];
	}
	memset(gridKept.weights, 0, sizeof(gridKept.weights));
	for (int a = 0; a < 256; ++a) {
		for (int d = 0; a < 256 && acrossCount[a] && d < 256; ++d) {
			if (downCount[d]) {
				gridKept.weights[a + d - (a * d + 127) / 255] += (float) acrossCount[a] * downCount[d];
			}
		}
	}
	// With LCD Subpixels, each channel's own: each column's pixels weighed by its stripe factor.
	memset(gridKept.channelWeights, 0, sizeof(gridKept.channelWeights));
	gridKept.perChannel = fit.tinted;
	for (unsigned x = 0; fit.tinted && x < outWidth; ++x) {
		for (int c = 0; c < 3; ++c) {
			float const factor = powf(fit.tint16[c][x] / 256.0f, 2.2f);
			for (int d = 0; d < 256; ++d) {
				if (downCount[d]) {
					unsigned const a = fit.across[x];
					gridKept.channelWeights[c][a + d - (a * d + 127) / 255] += factor * downCount[d];
				}
			}
		}
	}
	for (int i = 0; i < 256; ++i) {
		gridKept.shares[i] = i / 255.0f;
	}
	gridKept.count = 256;
	gridKept.stale = true;
	keptHalves = -1;
	return true;
}

// For source row y, its two rows at the output's width (fit's comment has them): each column's source
// pixel and gap gathered, then in vectors each channel moved towards the gap by the column's share
// (x / 255 exact as (x + 128 + (x + 128 >> 8)) >> 8) and taken times its stripe factor and the
// shadow, each a share of 256.
static void fitRows(unsigned y, uint8_t* pixels, uint8_t* gaps, const uint8_t* keep, uint32_t* shades, uint8_t* inside, uint8_t* outside) {
	unsigned const outWidth = fit.outWidth, gap = gridFrame.gap, width = gridFrame.width;
	const uint8_t* const row = gridFrame.picture + y * gridFrame.pitch;
	bool const kept = gridFrame.kept;
	const uint16_t* const from = fit.columnFrom;
	if (gridFrame.bytes == 4) {
		const uint32_t* const in = (const uint32_t*) row;
		uint32_t* const keptRow = (uint32_t*) pixels;
		uint32_t* const gapRow = (uint32_t*) gaps;
		for (unsigned x = 0; kept && x < width; ++x) {
			uint32_t const c = in[x];
			keptRow[x] = (uint32_t) gridKept.table[0][c >> 16 & 0xFF] << 16 | (uint32_t) gridKept.table[1][c >> 8 & 0xFF] << 8 | gridKept.table[2][c & 0xFF];
			gapRow[x] = (uint32_t) gridKept.gaps[0][c >> 16 & 0xFF] << 16 | (uint32_t) gridKept.gaps[1][c >> 8 & 0xFF] << 8 | gridKept.gaps[2][c & 0xFF];
		}
		const uint32_t* const source = kept ? keptRow : in;
		uint32_t* const a = (uint32_t*) inside;
		uint32_t* const b = (uint32_t*) outside;
		for (unsigned x = 0; x < outWidth; ++x) {
			a[x] = source[from[x]];
			b[x] = kept ? gapRow[from[x]] : gap;
			shades[x] = keep ? keep[from[x]] + 1u : 256;
		}
		for (unsigned x = 0; x < outWidth; x += GRID_LANES32) {
			GridU32 c, g, k, s, t[3];
			memcpy(&c, a + x, sizeof(c));
			memcpy(&g, b + x, sizeof(g));
			memcpy(&k, fit.across32 + x, sizeof(k));
			memcpy(&s, shades + x, sizeof(s));
			GridU32 inner = { 0 }, outer = { 0 };
			for (int ch = 0; ch < 3; ++ch) {
				memcpy(&t[ch], fit.tint32[ch] + x, sizeof(t[ch]));
				int const shift = 16 - 8 * ch;
				GridU32 const tint = t[ch] * s >> 8;
				GridU32 const cc = c >> shift & 0xFF, gg = g >> shift & 0xFF;
				GridU32 const v = cc * (255 - k) + gg * k + 128;
				inner |= (((v + (v >> 8)) >> 8) * tint >> 8) << shift;
				outer |= (gg * tint >> 8) << shift;
			}
			memcpy(a + x, &inner, sizeof(inner));
			memcpy(b + x, &outer, sizeof(outer));
		}
		return;
	}
	const uint16_t* const in = (const uint16_t*) row;
	uint16_t* const keptRow = (uint16_t*) pixels;
	uint16_t* const gapRow = (uint16_t*) gaps;
	uint16_t const flatGap = (uint16_t) ((gap >> 19 & 0x1F) << 11 | (gap >> 10 & 0x3F) << 5 | (gap >> 3 & 0x1F));
	for (unsigned x = 0; kept && x < width; ++x) {
		unsigned const c = in[x], r = c >> 11, g = c >> 5 & 0x3F, b = c & 0x1F;
		keptRow[x] = (uint16_t) (gridKept.table565[0][0][r] | gridKept.table565[0][1][g] | gridKept.table565[0][2][b]);
		gapRow[x] = (uint16_t) (gridKept.table565[1][0][r] | gridKept.table565[1][1][g] | gridKept.table565[1][2][b]);
	}
	const uint16_t* const source = kept ? keptRow : in;
	uint16_t* const a = (uint16_t*) inside;
	uint16_t* const b = (uint16_t*) outside;
	for (unsigned x = 0; x < outWidth; ++x) {
		a[x] = source[from[x]];
		b[x] = kept ? gapRow[from[x]] : flatGap;
	}
	// Each channel widened to 8 bits, blended and rounded back, as the 565 blends do it.
	static const int shifts[3] = { 11, 5, 0 }, widen[3] = { 3, 2, 3 }, back[3] = { 2, 4, 2 };
	static const unsigned masks[3] = { 0x1F, 0x3F, 0x1F };
	for (unsigned x = 0; x < outWidth; x += GRID_LANES16) {
		GridU16 c, g, k;
		memcpy(&c, a + x, sizeof(c));
		memcpy(&g, b + x, sizeof(g));
		memcpy(&k, fit.across16 + x, sizeof(k));
		GridU16 inner = { 0 }, outer = { 0 };
		for (int ch = 0; ch < 3; ++ch) {
			GridU16 t;
			memcpy(&t, fit.tint16[ch] + x, sizeof(t));
			GridU16 const cc = c >> shifts[ch] & (uint16_t) masks[ch], gg = g >> shifts[ch] & (uint16_t) masks[ch];
			GridU16 const c8 = cc << widen[ch] | cc >> back[ch], g8 = gg << widen[ch] | gg >> back[ch];
			GridU16 const v = GRID_DIV255(c8 * (255 - k) + g8 * k) * t >> 8, u = g8 * t >> 8;
			inner |= GRID_DIV255(v * (uint16_t) masks[ch]) << shifts[ch];
			outer |= GRID_DIV255(u * (uint16_t) masks[ch]) << shifts[ch];
		}
		memcpy(a + x, &inner, sizeof(inner));
		memcpy(b + x, &outer, sizeof(outer));
	}
}

// A thread's output rows: each source row's two rows made once, then each output row from them.
static void fitDrawJob(int job) {
	unsigned from, to;
	rowsOf(job, fit.outHeight, &from, &to);
	size_t const bytes = gridFrame.bytes, outPitch = fit.outWidth * bytes, rowBytes = gridFrame.width * bytes;
	size_t const rowRoom = ((size_t) fit.outWidth + GRID_LANES16) * 4;
	uint8_t* const scratch = gridBlends.data + (size_t) job * (3 * rowRoom + 2 * rowBytes + gridFrame.width);
	uint8_t* const inside = scratch;
	uint8_t* const outside = inside + rowRoom;
	uint32_t* const shades = (uint32_t*) (outside + rowRoom);
	uint8_t* const pixels = (uint8_t*) shades + rowRoom;
	uint8_t* const gaps = pixels + rowBytes;
	uint8_t* const keep = gridFrame.shadow ? gaps + rowBytes : NULL;
	static const uint16_t untinted[3] = { 256, 256, 256 };
	unsigned made = ~0u;
	for (unsigned y = from; y < to; ++y) {
		unsigned const source = fit.rowFrom[y];
		if (source != made) {
			if (keep) {
				gridKeepRow(source, keep);
			}
			fitRows(source, pixels, gaps, keep, shades, inside, outside);
			made = source;
		}
		uint8_t* const out = gridOut.data + (size_t) y * outPitch;
		unsigned const down = fit.down[y];
		if (!down) {
			memcpy(out, inside, outPitch);
		} else if (down == 255) {
			memcpy(out, outside, outPitch);
		} else if (bytes == 4) {
			gridBlendGaps8888((const uint32_t*) inside, (const uint32_t*) outside, (uint32_t*) out, fit.outWidth, down, untinted);
		} else {
			gridBlendGaps565((const uint16_t*) inside, (const uint16_t*) outside, (uint16_t*) out, fit.outWidth, down, untinted);
		}
	}
}

// Fit Screen's frame: as sendPicture's, by output rows.
static void sendFitted(const void* picture, unsigned width, unsigned height, size_t pitch) {
	// The block grid's keeping tables may have taken gridKept since.
	if (keptHalves != -1) {
		fit.width = 0;
	}
	size_t const bytes = pitch / width;
	unsigned outWidth, outHeight;
	libretroLcdFitSize(width, height, &outWidth, &outHeight);
	size_t const outPitch = (size_t) outWidth * bytes;
	if (!picture || frameSkipped || rerunning()) {
		videoRefresh(NULL, outWidth, outHeight, outPitch);
		return;
	}
	uint32_t gap;
	enum LibretroLcd const lcd = libretroLcd(&gap);
	bool const shadow = gridWay && bytes == 4 && libretroLcdShadow();
	bool const room = fitLay(width, height, lcd) && gridReserve(&gridOut, outPitch * outHeight) &&
	                  gridReserve(&gridBlends, (size_t) GRID_JOBS * (3 * ((size_t) outWidth + GRID_LANES16) * 4 + 2 * width * bytes + width)) &&
	                  (!shadow || gridReserve(&gridDark, (size_t) (width + 16) * (height + 2 * DARK_EDGE)));
	if (!room) {
		videoRefresh(NULL, outWidth, outHeight, outPitch);
		return;
	}
	startJobs();
	bool const kept = gridGaps && libretroLcdKeep(&gridKept);
	gridFrame = (struct GridFrame) { .picture = picture, .width = width, .height = height, .pitch = pitch, .bytes = bytes,
		                             .gap = kept ? gridKept.gap : gap, .kept = kept, .shadow = shadow };
	if (shadow) {
		prepareShadow(gap, width, height);
	}
	runJobs(fitDrawJob);
	videoRefresh(gridOut.data, outWidth, outHeight, outPitch);
}

void sendPicture(const void* picture, unsigned width, unsigned height, size_t pitch) {
	if (gridFit) {
		sendFitted(picture, width, height, pitch);
		return;
	}
	// A half step needs whole blocks of two pixels.
	int const halves = gridHalves % 2 && (width | height) & 1 ? gridHalves - 1 : gridHalves;
	if (halves <= 2) {
		videoRefresh(frameSkipped ? NULL : picture, width, height, pitch);
		return;
	}
	size_t const bytes = pitch / width;
	unsigned const outWidth = width * (unsigned) halves / 2, outHeight = height * (unsigned) halves / 2;
	size_t const outPitch = (size_t) outWidth * bytes;
	// A frame RetroArch runs again and doesn't show, or Frame Skip leaves out, isn't drawn.
	if (!picture || frameSkipped || rerunning()) {
		videoRefresh(NULL, outWidth, outHeight, outPitch);
		return;
	}
	uint32_t gap;
	enum LibretroLcd const lcd = libretroLcd(&gap);
	gridLay(halves, lcd);
	size_t const rowBytes = width * bytes;
	bool const shadow = gridWay && bytes == 4 && libretroLcdShadow();
	bool const room = gridReserve(&gridOut, outPitch * outHeight) &&
	                  gridReserve(&gridBlends, (size_t) GRID_JOBS * gridLayout.period * gridLayout.shareCount * rowBytes) &&
	                  gridReserve(&gridTemplates, (size_t) GRID_JOBS * gridLayout.block * outPitch) &&
	                  gridReserve(&gridSplits, (size_t) GRID_JOBS * 3 * rowBytes) && gridReserve(&gridKeptRows, (size_t) GRID_JOBS * 2 * rowBytes) &&
	                  (!shadow || (gridReserve(&gridDark, (size_t) (width + 16) * (height + 2 * DARK_EDGE)) &&
	                               gridReserve(&gridKeeps, (size_t) GRID_JOBS * width)));
	// Without the memory, the last picture stays.
	if (!room) {
		videoRefresh(NULL, outWidth, outHeight, outPitch);
		return;
	}
	startJobs();
	if (keptHalves != halves || keptLcd != (int) lcd || keptGaps != gridGaps || keptMix != gridLayout.mix) {
		int const block = gridLayout.block;
		gridKept.count = block * block;
		gridKept.perChannel = gridLayout.mix > 0;
		for (int i = 0; i < gridKept.count; ++i) {
			const uint16_t* const tint = gridLayout.channels[gridLayout.shareOf[i / block][i % block]];
			for (int c = 0; c < 3; ++c) {
				gridKept.channelWeights[c][i] = powf(tint[c] / 256.0f, 2.2f);
			}
			gridKept.shares[i] = gridLayout.shares[gridLayout.shareOf[i / block][i % block]] / 255.0f;
			gridKept.weights[i] = 1;
		}
		gridKept.stale = true;
		keptHalves = halves;
		keptLcd = (int) lcd;
		keptGaps = gridGaps;
		keptMix = gridLayout.mix;
	}
	bool const kept = libretroLcdKeep(&gridKept);
	gridFrame = (struct GridFrame) { .picture = picture, .width = width, .height = height, .pitch = pitch, .bytes = bytes,
		                             .gap = kept ? gridKept.gap : gap, .kept = kept, .shadow = shadow };
	if (shadow) {
		prepareShadow(gap, width, height);
	}
	runJobs(gridDrawJob);
	videoRefresh(gridOut.data, outWidth, outHeight, outPitch);
}

// Each channel's next level, from its level now and the channel's value in the new picture: tables for
// 5- and 6-bit channels (RGB565) and 8-bit ones (XRGB8888), for the rows driven harder (all rows
// without Interlaced Rows) and the others.
static uint8_t next5[2][256 * 32], next6[2][256 * 64], next8[2][256 * 256];
static int builtFor = -1;

static void buildNext(int kind, int strength, int bits, uint8_t* table, float scale) {
	// Each level in linear light, and where in linear light each level's rounding gives way to the next.
	static float linear[256], bounds[255];
	if (!linear[255]) {
		for (int v = 0; v < 256; ++v) {
			linear[v] = (float) pow(v / 255.0, 2.2);
			if (v < 255) {
				bounds[v] = (float) pow((v + 0.5) / 255, 2.2);
			}
		}
	}
	float const dark = (float) (1 - exp(-16.743 * 4 / strength / (screenTimes[kind][0] * scale)));
	float const light = (float) (1 - exp(-16.743 * 4 / strength / (screenTimes[kind][1] * scale)));
	int const codes = 1 << bits;
	for (int now = 0; now < 256; ++now) {
		for (int code = 0; code < codes; ++code) {
			int const full = bits == 8 ? code : bits == 6 ? code << 2 | code >> 4 : code << 3 | code >> 2;
			float const diff = linear[full] - linear[now];
			float const moved = linear[now] + diff * (diff < 0 ? dark : light);
			int next = 0;
			for (int step = 128; step; step >>= 1) {
				next += next + step <= 255 && bounds[next + step - 1] < moved ? step : 0;
			}
			if (next == now && now != full) {
				next += full > now ? 1 : -1;
			}
			table[now * codes + code] = (uint8_t) next;
		}
	}
}

// The screen between frames: each subpixel's level, whether each pixel is still moving and how many
// of each row's are, the last picture in and the picture shown; and this frame's picture in, its
// pixels' size and whether its rows alternate.
static struct {
	uint8_t* levels;
	uint8_t* busy;
	uint16_t* rowBusy;
	uint8_t* last;
	uint8_t* shown;
	unsigned width, height;
	size_t pitch;
	unsigned frames;
	const uint8_t* picture;
	size_t bytes;
	bool alternate;
} ghost;

// A thread's rows of the screen, a frame on. A row as it was, settled, is copied.
static void ghostJob(int job) {
	unsigned from, to;
	rowsOf(job, ghost.height, &from, &to);
	size_t const pitch = ghost.pitch, rowPixels = pitch / ghost.bytes;
	for (unsigned y = from; y < to; ++y) {
		const uint8_t* const in = ghost.picture + y * pitch;
		uint8_t* const before = ghost.last + y * pitch;
		uint8_t* const out = ghost.shown + y * pitch;
		if (!ghost.rowBusy[y] && !memcmp(in, before, pitch)) {
			memcpy(out, in, pitch);
			continue;
		}
		int const weak = ghost.alternate && (y + ghost.frames) % 2;
		uint8_t* level = ghost.levels + y * rowPixels * 3;
		uint8_t* moving = ghost.busy + y * rowPixels;
		unsigned count = 0;
		if (ghost.bytes == 4) {
			const uint8_t* const t = next8[weak];
			for (size_t x = 0; x < rowPixels; ++x, level += 3) {
				uint32_t const input = ((const uint32_t*) in)[x];
				if (input == ((const uint32_t*) before)[x] && !moving[x]) {
					((uint32_t*) out)[x] = input;
					continue;
				}
				uint32_t const r = t[level[0] << 8 | (input >> 16 & 0xFF)];
				uint32_t const g = t[level[1] << 8 | (input >> 8 & 0xFF)];
				uint32_t const b = t[level[2] << 8 | (input & 0xFF)];
				level[0] = (uint8_t) r;
				level[1] = (uint8_t) g;
				level[2] = (uint8_t) b;
				uint32_t const output = (input & 0xFF000000) | r << 16 | g << 8 | b;
				bool const still = output != input;
				moving[x] = still;
				count += still;
				((uint32_t*) out)[x] = output;
			}
		} else {
			const uint8_t* const t5 = next5[weak];
			const uint8_t* const t6 = next6[weak];
			for (size_t x = 0; x < rowPixels; ++x, level += 3) {
				uint32_t const input = ((const uint16_t*) in)[x];
				if (input == ((const uint16_t*) before)[x] && !moving[x]) {
					((uint16_t*) out)[x] = (uint16_t) input;
					continue;
				}
				uint32_t const r = t5[level[0] << 5 | input >> 11];
				uint32_t const g = t6[level[1] << 6 | (input >> 5 & 0x3F)];
				uint32_t const b = t5[level[2] << 5 | (input & 0x1F)];
				level[0] = (uint8_t) r;
				level[1] = (uint8_t) g;
				level[2] = (uint8_t) b;
				// Settled when each level is its channel's value widened to 8 bits.
				uint32_t const output = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3;
				bool const still = output != input || (r & 7) != r >> 5 || (g & 3) != g >> 6 || (b & 7) != b >> 5;
				moving[x] = still;
				count += still;
				((uint16_t*) out)[x] = (uint16_t) output;
			}
		}
		memcpy(before, in, pitch);
		ghost.rowBusy[y] = (uint16_t) count;
	}
}

// Interlaced Rows without LCD Ghosting: each frame one set of alternate rows takes the new picture
// and the others keep what they showed, as the Color's and Advance's screens drive alternate rows.
static struct GridBuffer interlacedShown;
static unsigned interlacedWidth, interlacedHeight, interlacedFrames;
static size_t interlacedPitch;

static void showInterlaced(const void* picture, unsigned width, unsigned height, size_t pitch) {
	if (!gridReserve(&interlacedShown, pitch * height)) {
		sendPicture(picture, width, height, pitch);
		return;
	}
	if (width != interlacedWidth || height != interlacedHeight || pitch != interlacedPitch) {
		memcpy(interlacedShown.data, picture, pitch * height);
		interlacedWidth = width;
		interlacedHeight = height;
		interlacedPitch = pitch;
	} else {
		++interlacedFrames;
		// Rows of both sets were driven in frames left out.
		for (unsigned y = framesSkipped ? 0 : interlacedFrames & 1; y < height; y += framesSkipped ? 1 : 2) {
			memcpy(interlacedShown.data + y * pitch, (const uint8_t*) picture + y * pitch, pitch);
		}
	}
	sendPicture(interlacedShown.data, width, height, pitch);
}

void showPicture(const void* picture, unsigned width, unsigned height, size_t pitch) {
	bool const alone = !ghosting && interleaved && screenKind() == SCREEN_TFT;
	if (!alone) {
		// Turned off, it starts again from the picture when it's turned on.
		interlacedWidth = 0;
	}
	if (!ghosting || !picture || frameSkipped || rerunning()) {
		// Turned off, the screen starts again from the picture when it's turned on.
		ghost.width = ghosting ? ghost.width : 0;
		if (alone && picture && !frameSkipped && !rerunning()) {
			showInterlaced(picture, width, height, pitch);
			return;
		}
		sendPicture(picture, width, height, pitch);
		return;
	}
	size_t const bytes = pitch / width;
	size_t const pixels = pitch / bytes * height;
	int const kind = screenKind();
	// Interlaced rows: the harder driven rows' share of the time, and the others' so that the two
	// frames' rates add up to two plain ones (1 / lead + 1 / lag = 2).
	bool const alternate = kind == SCREEN_TFT && interleaved;
	float const lead = !alternate ? 1 : advancePanel() ? 0.8f : 0.75f;
	int const wanted = ((kind * 4 + ghosting - 1) * 3 + (alternate ? 1 + advancePanel() : 0)) * 2 + (bytes == 4);
	if (builtFor != wanted) {
		builtFor = wanted;
		for (int d = 0; d < (alternate ? 2 : 1); ++d) {
			float const scale = d ? 1 / (2 - 1 / lead) : lead;
			if (bytes == 4) {
				buildNext(kind, ghosting, 8, next8[d], scale);
			} else {
				buildNext(kind, ghosting, 5, next5[d], scale);
				buildNext(kind, ghosting, 6, next6[d], scale);
			}
		}
	}
	++ghost.frames;
	if (!ghost.levels || width != ghost.width || height != ghost.height || pitch != ghost.pitch) {
		uint8_t* const newLevels = realloc(ghost.levels, pixels * 3);
		ghost.levels = newLevels ? newLevels : ghost.levels;
		uint8_t* const newBusy = realloc(ghost.busy, pixels);
		ghost.busy = newBusy ? newBusy : ghost.busy;
		uint16_t* const newRowBusy = realloc(ghost.rowBusy, height * sizeof(*ghost.rowBusy));
		ghost.rowBusy = newRowBusy ? newRowBusy : ghost.rowBusy;
		uint8_t* const newLast = realloc(ghost.last, pitch * height);
		ghost.last = newLast ? newLast : ghost.last;
		uint8_t* const newShown = realloc(ghost.shown, pitch * height);
		ghost.shown = newShown ? newShown : ghost.shown;
		ghost.width = 0;
		if (newLevels && newBusy && newRowBusy && newLast && newShown) {
			ghost.width = width;
			ghost.height = height;
			ghost.pitch = pitch;
			memcpy(ghost.last, picture, pitch * height);
			memset(ghost.busy, 0, pixels);
			memset(ghost.rowBusy, 0, height * sizeof(*ghost.rowBusy));
			for (size_t i = 0; i < pixels; ++i) {
				if (bytes == 4) {
					uint32_t const c = ((const uint32_t*) picture)[i];
					ghost.levels[i * 3] = c >> 16 & 0xFF;
					ghost.levels[i * 3 + 1] = c >> 8 & 0xFF;
					ghost.levels[i * 3 + 2] = c & 0xFF;
				} else {
					uint16_t const c = ((const uint16_t*) picture)[i];
					ghost.levels[i * 3] = (uint8_t) ((c >> 11) << 3 | (c >> 13));
					ghost.levels[i * 3 + 1] = (uint8_t) ((c >> 5 & 0x3F) << 2 | (c >> 9 & 3));
					ghost.levels[i * 3 + 2] = (uint8_t) ((c & 0x1F) << 3 | (c >> 2 & 7));
				}
			}
		}
		sendPicture(picture, width, height, pitch);
		return;
	}
	ghost.picture = picture;
	ghost.bytes = bytes;
	ghost.alternate = alternate;
	startJobs();
	runJobs(ghostJob);
	// Frames left out moved the screen on too, towards a picture taken to be this one; up to three.
	for (int step = 0; step < framesSkipped && step < 3; ++step) {
		++ghost.frames;
		runJobs(ghostJob);
	}
	sendPicture(ghost.shown, width, height, pitch);
}
