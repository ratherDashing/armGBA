/* Copyright (c) 2026 armGBA 3DS port contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The game on the top screen: each frame is copied into linear memory as RGB565, turned into a
// tiled texture by the GPU's transfer engine, and drawn scaled with citro2d.
//
// LCD3x: the 3DS's GPU has no fragment shaders, so lcd3x (libretro's handheld/lcd3x) is done the way
// fixed-function hardware can: the picture is drawn three screen columns to each game pixel on the
// top screen's 800-pixel wide mode, then multiplied (blending dst * src) by a mask that holds
// lcd3x's own factors evaluated at each screen pixel: (4 + sin(2*pi*x + offset[channel])) / 5 across
// for the red, green and blue stripes, and (16 + sin(2*pi*y)) / 17 down for the scanlines, x and y in
// game pixels. It's exact across (three columns a pixel, as lcd3x draws at 3x) and sampled at the
// screen's rows down, where the top screen has 1.5 rows to a GBA pixel.

#include "frontend.h"

#include <citro2d.h>
#include <citro3d.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static C3D_RenderTarget* top;
static bool wide;
static C3D_Tex texture;
static int texW, texH;
static uint16_t* staging;
static unsigned frameWidth = 240, frameHeight = 160;
static enum retro_pixel_format format = RETRO_PIXEL_FORMAT_0RGB1555;
static bool haveFrame;
// The last frame as RGB565, tightly packed, for test screenshots.
static uint16_t* lastFrame;

// lcd3x's mask, for the picture's size and place on the wide screen.
static C3D_Tex mask;
static bool maskReady;
static unsigned maskFor[4];

static int pow2(int v) {
	int p = 8;
	while (p < v) {
		p <<= 1;
	}
	return p;
}

// A texture (and staging buffer) for frames up to width by height.
static bool textureFor(unsigned width, unsigned height) {
	int const w = pow2(width < 256 ? 256 : width), h = pow2(height < 256 ? 256 : height);
	if (w <= texW && h <= texH) {
		return true;
	}
	if (w > 1024 || h > 1024) {
		return false;
	}
	if (texW) {
		C3D_TexDelete(&texture);
		linearFree(staging);
		free(lastFrame);
	}
	texW = texH = 0;
	staging = linearAlloc(w * h * 2);
	lastFrame = malloc(w * h * 2);
	if (!staging || !lastFrame || !C3D_TexInit(&texture, w, h, GPU_RGB565)) {
		logLine("video: no memory for a %dx%d texture", w, h);
		return false;
	}
	memset(staging, 0, w * h * 2);
	texW = w;
	texH = h;
	logLine("video: %dx%d texture", w, h);
	return true;
}

static void setWide(bool on) {
	if (on == wide && top) {
		return;
	}
	if (top) {
		C3D_RenderTargetDelete(top);
	}
	gfxSetWide(on);
	wide = on;
	top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
}

void displayInit(void) {
	gfxInitDefault();
	gfxSet3D(false);
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
	C2D_Prepare();
	setWide(false);
	textureFor(240, 160);
}

void displayExit(void) {
	if (texW) {
		linearFree(staging);
		free(lastFrame);
		C3D_TexDelete(&texture);
	}
	if (maskReady) {
		C3D_TexDelete(&mask);
	}
	C3D_RenderTargetDelete(top);
	C2D_Fini();
	C3D_Fini();
	gfxSetWide(false);
	gfxExit();
}

void displaySetFormat(enum retro_pixel_format f) {
	format = f;
}

const uint16_t* displayLastFrame(int* width, int* height) {
	*width = frameWidth;
	*height = frameHeight;
	return haveFrame ? lastFrame : NULL;
}

static inline uint16_t from8888(uint32_t p) {
	return ((p >> 8) & 0xF800) | ((p >> 5) & 0x07E0) | ((p >> 3) & 0x001F);
}

static inline uint16_t from1555(uint16_t p) {
	return ((p << 1) & 0xFFC0) | ((p >> 4) & 0x0020) | (p & 0x001F);
}

// The core's frame (NULL: the same one again) into the texture.
void displayFrame(const void* data, unsigned width, unsigned height, size_t pitch) {
	if (!data || !width || !height) {
		return;
	}
	if (!textureFor(width, height)) {
		return;
	}
	frameWidth = width;
	frameHeight = height;
	for (unsigned y = 0; y < height; ++y) {
		uint16_t* out = &lastFrame[y * width];
		const uint8_t* row = (const uint8_t*) data + y * pitch;
		switch (format) {
		case RETRO_PIXEL_FORMAT_RGB565:
			memcpy(out, row, width * 2);
			break;
		case RETRO_PIXEL_FORMAT_XRGB8888:
			for (unsigned x = 0; x < width; ++x) {
				out[x] = from8888(((const uint32_t*) row)[x]);
			}
			break;
		default:
			for (unsigned x = 0; x < width; ++x) {
				out[x] = from1555(((const uint16_t*) row)[x]);
			}
			break;
		}
		memcpy(&staging[y * texW], out, width * 2);
	}
	haveFrame = true;
	GSPGPU_FlushDataCache(staging, texW * height * 2);
	C3D_SyncDisplayTransfer((u32*) staging, GX_BUFFER_DIM(texW, texH), (u32*) texture.data, GX_BUFFER_DIM(texW, texH),
	                        GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0) |
	                            GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) |
	                            GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
}

// lcd3x's factors for a picture of width by height game pixels drawn outW by outH screen pixels, into
// an RGBA8 texture (made linear then tiled by the transfer engine, as the frames are).
static void buildMask(unsigned width, unsigned height, unsigned outW, unsigned outH) {
	unsigned const want[4] = { width, height, outW, outH };
	if (maskReady && !memcmp(want, maskFor, sizeof(want))) {
		return;
	}
	if (maskReady) {
		C3D_TexDelete(&mask);
		maskReady = false;
	}
	int const w = 1024, h = 256;
	uint32_t* pixels = linearAlloc(w * h * 4);
	if (!pixels || !C3D_TexInit(&mask, w, h, GPU_RGBA8)) {
		linearFree(pixels);
		logLine("video: no memory for the LCD3x mask");
		return;
	}
	float const pi = 3.14159265f;
	float const offsets[3] = { pi * 0.5f, pi * (0.5f - 2.0f / 3.0f), pi * (0.5f - 4.0f / 3.0f) };
	float rows[256];
	for (unsigned y = 0; y < outH && y < (unsigned) h; ++y) {
		float const gy = (y + 0.5f) * height / outH;
		rows[y] = (16.0f + sinf(2 * pi * gy)) / 17.0f;
	}
	for (unsigned x = 0; x < outW && x < (unsigned) w; ++x) {
		float const gx = (x + 0.5f) * width / outW;
		float f[3];
		for (int c = 0; c < 3; ++c) {
			f[c] = (4.0f + sinf(2 * pi * gx + offsets[c])) / 5.0f;
		}
		for (unsigned y = 0; y < outH && y < (unsigned) h; ++y) {
			uint32_t const r = (uint32_t) (f[0] * rows[y] * 255.0f + 0.5f);
			uint32_t const g = (uint32_t) (f[1] * rows[y] * 255.0f + 0.5f);
			uint32_t const b = (uint32_t) (f[2] * rows[y] * 255.0f + 0.5f);
			// GPU_RGBA8 is 0xRRGGBBAA in a word.
			pixels[y * w + x] = r << 24 | g << 16 | b << 8 | 0xFF;
		}
	}
	GSPGPU_FlushDataCache(pixels, w * h * 4);
	C3D_SyncDisplayTransfer((u32*) pixels, GX_BUFFER_DIM(w, h), (u32*) mask.data, GX_BUFFER_DIM(w, h),
	                        GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0) |
	                            GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
	                            GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
	linearFree(pixels);
	C3D_TexSetFilter(&mask, GPU_NEAREST, GPU_NEAREST);
	memcpy(maskFor, want, sizeof(want));
	maskReady = true;
	logLine("video: LCD3x mask for %ux%u at %ux%u", width, height, outW, outH);
}

static void drawLcd3x(C2D_Image image) {
	// Three columns to a pixel where that fits the 800 columns, else as many whole ones as do.
	unsigned cols = 800 / frameWidth;
	cols = cols > 3 ? 3 : cols ? cols : 1;
	float const sx = (float) cols, sy = 240.0f / frameHeight;
	unsigned const outW = frameWidth * cols, outH = 240;
	int const x = (800 - (int) outW) / 2;
	// citro2d keeps the top screen 400 units across in wide mode too (each unit two columns): across,
	// positions and scales are halved.
	C2D_DrawImageAt(image, x / 2.0f, 0, 0.5f, NULL, sx / 2.0f, sy);
	buildMask(frameWidth, frameHeight, outW, outH);
	if (!maskReady) {
		return;
	}
	Tex3DS_SubTexture sub = {
		.width = outW,
		.height = outH,
		.left = 0.0f,
		.top = 1.0f,
		.right = outW / 1024.0f,
		.bottom = 1.0f - outH / 256.0f,
	};
	C2D_Image maskImage = { &mask, &sub };
	// The mask times what's drawn: dst = dst * src.
	C2D_Flush();
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_DST_COLOR, GPU_ZERO, GPU_ZERO, GPU_ONE);
	C2D_DrawImageAt(maskImage, x / 2.0f, 0, 0.6f, NULL, 0.5f, 1.0f);
	C2D_Flush();
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
}

// Draws the last frame scaled to the top screen. With wait (the menus), first waits for the screen's
// next refresh. The game doesn't: a frame that took a little over 16.7 ms would then wait for the
// refresh after, and the game would run at 30 frames a second; it paces itself (main.c).
static void present(bool wait) {
	// Closed from the HOME Menu, the app no longer has the GPU: a frame submitted then never finishes,
	// and waiting for it hung the system on "Closing software" (2026-11-09). Nothing is drawn.
	if (aptShouldClose()) {
		return;
	}
	bool const lcd3x = config.scale == SCALE_LCD3X;
	setWide(lcd3x);
	C3D_FrameBegin(wait ? C3D_FRAME_SYNCDRAW : 0);
	C2D_TargetClear(top, C2D_Color32(0, 0, 0, 255));
	C2D_SceneBegin(top);
	if (haveFrame) {
		bool const smooth = config.filter && config.scale != SCALE_1X && !lcd3x;
		C3D_TexSetFilter(&texture, smooth ? GPU_LINEAR : GPU_NEAREST, smooth ? GPU_LINEAR : GPU_NEAREST);
		Tex3DS_SubTexture sub = {
			.width = frameWidth,
			.height = frameHeight,
			.left = 0.0f,
			.top = 1.0f,
			.right = (float) frameWidth / texW,
			.bottom = 1.0f - (float) frameHeight / texH,
		};
		C2D_Image image = { &texture, &sub };
		if (lcd3x) {
			drawLcd3x(image);
		} else {
			float sx = 1.0f, sy = 1.0f;
			// A frame bigger than the screen (the core's CPU LCD Effects at a fixed scale) is fitted.
			int const mode = config.scale == SCALE_1X && (frameWidth > 400 || frameHeight > 240) ? SCALE_FIT : config.scale;
			switch (mode) {
			case SCALE_FIT: {
				float s = 400.0f / frameWidth < 240.0f / frameHeight ? 400.0f / frameWidth : 240.0f / frameHeight;
				sx = sy = s;
				break;
			}
			case SCALE_STRETCH:
				sx = 400.0f / frameWidth;
				sy = 240.0f / frameHeight;
				break;
			default:
				// Pixel for pixel.
				break;
			}
			float const w = frameWidth * sx, h = frameHeight * sy;
			C2D_DrawImageAt(image, (int) ((400 - w) / 2), (int) ((240 - h) / 2), 0.5f, NULL, sx, sy);
		}
	}
	C3D_FrameEnd(0);
}

void displayPresent(void) {
	present(true);
}

void displayPresentGame(void) {
	present(false);
}

void displayClearTop(void) {
	haveFrame = false;
	displayPresent();
}
