/* Copyright (c) 2026 armGBA 3DS port contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The game on the top screen: each frame is copied into linear memory as RGB565, turned into a
// tiled texture by the GPU's transfer engine, and drawn with citro2d.
//
// Fit and Stretch, as mGBA's 3DS port draws: the frame is first drawn pixel for pixel at a whole
// multiple (2x) into a texture the GPU renders to, then that is drawn scaled down to the screen with
// bilinear filtering. Every game pixel comes out the same size, edges stay sharp, and there's none of
// the uneven rows a 1.5x nearest-neighbour scale makes. With Smoothing on, the frame is filtered
// straight to the screen instead.
//
// LCD3x: the 3DS's GPU has no fragment shaders, so lcd3x (libretro's handheld/lcd3x) is done the way
// fixed-function hardware can. The frame is drawn at exactly 3x (as lcd3x draws) into the render
// texture and multiplied (blending dst * src) by a mask holding lcd3x's own factors at each of those
// pixels: (4 + sin(2*pi*x + offset[channel])) / 5 across for the red, green and blue stripes, and
// (16 + sin(2*pi*y)) / 17 down for the scanlines, x and y in game pixels. That is shown on the top
// screen's 800-column wide mode, column for column across (three columns a game pixel, the stripes
// exact) and two rows to one down, averaged by the filter.

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

// What the frame is drawn into at 2x or 3x before it's scaled to the screen.
enum {
	RENDER_W = 1024,
	RENDER_H = 512,
};
static C3D_Tex render;
static C3D_RenderTarget* renderTarget;

// lcd3x's mask, for the frame's size at 3x.
static C3D_Tex mask;
static bool maskReady;
static unsigned maskFor[2];

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
	if (C3D_TexInitVRAM(&render, RENDER_W, RENDER_H, GPU_RGBA8)) {
		renderTarget = C3D_RenderTargetCreateFromTex(&render, GPU_TEXFACE_2D, 0, -1);
	}
	if (!renderTarget) {
		logLine("video: no render texture: the picture is scaled directly");
	}
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
	if (renderTarget) {
		C3D_RenderTargetDelete(renderTarget);
		C3D_TexDelete(&render);
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

// lcd3x's factors for a frame of width by height game pixels drawn at 3x, into an RGBA8 texture
// (made linear then tiled by the transfer engine, as the frames are).
static void buildMask(unsigned width, unsigned height) {
	unsigned const want[2] = { width, height };
	if (maskReady && !memcmp(want, maskFor, sizeof(want))) {
		return;
	}
	if (maskReady) {
		C3D_TexDelete(&mask);
		maskReady = false;
	}
	int const w = RENDER_W, h = RENDER_H;
	unsigned const outW = width * 3, outH = height * 3;
	uint32_t* pixels = linearAlloc(w * h * 4);
	if (!pixels || !C3D_TexInit(&mask, w, h, GPU_RGBA8)) {
		linearFree(pixels);
		logLine("video: no memory for the LCD3x mask");
		return;
	}
	memset(pixels, 0xFF, w * h * 4);
	float const pi = 3.14159265f;
	float const offsets[3] = { pi * 0.5f, pi * (0.5f - 2.0f / 3.0f), pi * (0.5f - 4.0f / 3.0f) };
	// Each 3x pixel's centre, in game pixels: the same three values in every game pixel.
	float rows[3], cols[3][3];
	for (int i = 0; i < 3; ++i) {
		float const g = (i + 0.5f) / 3.0f;
		rows[i] = (16.0f + sinf(2 * pi * g)) / 17.0f;
		for (int c = 0; c < 3; ++c) {
			cols[i][c] = (4.0f + sinf(2 * pi * g + offsets[c])) / 5.0f;
		}
	}
	for (unsigned y = 0; y < outH && y < (unsigned) h; ++y) {
		for (unsigned x = 0; x < outW && x < (unsigned) w; ++x) {
			float const r = rows[y % 3];
			uint32_t const cr = (uint32_t) (cols[x % 3][0] * r * 255.0f + 0.5f);
			uint32_t const cg = (uint32_t) (cols[x % 3][1] * r * 255.0f + 0.5f);
			uint32_t const cb = (uint32_t) (cols[x % 3][2] * r * 255.0f + 0.5f);
			// GPU_RGBA8 is 0xRRGGBBAA in a word.
			pixels[y * w + x] = cr << 24 | cg << 16 | cb << 8 | 0xFF;
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
	logLine("video: LCD3x mask for %ux%u", width, height);
}

// The part of a texture of texW by texH a picture of width by height takes, from its top left.
static Tex3DS_SubTexture subOf(unsigned width, unsigned height, int textureW, int textureH) {
	Tex3DS_SubTexture sub = {
		.width = width,
		.height = height,
		.left = 0.0f,
		.top = 1.0f,
		.right = (float) width / textureW,
		.bottom = 1.0f - (float) height / textureH,
	};
	return sub;
}

// The frame drawn pixel for pixel at scale times its size into the render texture; with lcd3x's
// mask multiplied in. Then the render texture's image of it.
static C2D_Image prescale(C2D_Image frame, unsigned scale, bool lcd, Tex3DS_SubTexture* sub) {
	C3D_TexSetFilter(&texture, GPU_NEAREST, GPU_NEAREST);
	C2D_TargetClear(renderTarget, C2D_Color32(0, 0, 0, 255));
	C2D_SceneBegin(renderTarget);
	C2D_DrawImageAt(frame, 0, 0, 0.5f, NULL, (float) scale, (float) scale);
	if (lcd) {
		buildMask(frameWidth, frameHeight);
		if (maskReady) {
			Tex3DS_SubTexture maskSub = subOf(frameWidth * 3, frameHeight * 3, RENDER_W, RENDER_H);
			C2D_Image maskImage = { &mask, &maskSub };
			// The mask times what's drawn: dst = dst * src.
			C2D_Flush();
			C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_DST_COLOR, GPU_ZERO, GPU_ZERO, GPU_ONE);
			C2D_DrawImageAt(maskImage, 0, 0, 0.6f, NULL, 1.0f, 1.0f);
			C2D_Flush();
			C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
		}
	}
	*sub = subOf(frameWidth * scale, frameHeight * scale, RENDER_W, RENDER_H);
	C3D_TexSetFilter(&render, GPU_LINEAR, GPU_LINEAR);
	C2D_Image image = { &render, sub };
	return image;
}

// The largest whole scale, up to most, at which the frame fits the render texture.
static unsigned fitsAt(unsigned most) {
	unsigned scale = most;
	while (scale > 1 && (frameWidth * scale > RENDER_W || frameHeight * scale > RENDER_H)) {
		--scale;
	}
	return renderTarget ? scale : 1;
}

// Draws the last frame on the top screen. With wait (the menus), first waits for the screen's next
// refresh. The game doesn't: a frame that took a little over 16.7 ms would then wait for the refresh
// after, and the game would run at 30 frames a second; it paces itself (main.c).
static void present(bool wait) {
	// Closed from the HOME Menu, the app no longer has the GPU: a frame submitted then never finishes,
	// and waiting for it hung the system on "Closing software" (2026-11-09). Nothing is drawn.
	if (aptShouldClose()) {
		return;
	}
	// LCD3x needs three columns a pixel: the wide mode's 800 hold 266 game pixels.
	bool const lcd3x = config.scale == SCALE_LCD3X && renderTarget && frameWidth * 3 <= 800 && fitsAt(3) == 3;
	setWide(lcd3x);
	C3D_FrameBegin(wait ? C3D_FRAME_SYNCDRAW : 0);
	C2D_Image image = { NULL, NULL };
	Tex3DS_SubTexture frameSub = subOf(frameWidth, frameHeight, texW, texH), renderSub;
	float sx = 1.0f, sy = 1.0f;
	if (haveFrame) {
		image = (C2D_Image) { &texture, &frameSub };
		if (lcd3x) {
			image = prescale(image, 3, true, &renderSub);
			// citro2d keeps the top screen 400 units across in wide mode too, each two columns: one
			// column to each 3x pixel across, two rows to one down.
			sx = 0.5f;
			sy = 0.5f;
		} else {
			// A frame bigger than the screen is fitted.
			int const mode = config.scale == SCALE_1X && (frameWidth > 400 || frameHeight > 240) ? SCALE_FIT : config.scale;
			float fx = 1.0f, fy = 1.0f;
			if (mode == SCALE_FIT || mode == SCALE_LCD3X) {
				fx = fy = 400.0f / frameWidth < 240.0f / frameHeight ? 400.0f / frameWidth : 240.0f / frameHeight;
			} else if (mode == SCALE_STRETCH) {
				fx = 400.0f / frameWidth;
				fy = 240.0f / frameHeight;
			}
			unsigned const scale = mode == SCALE_1X || config.filter ? 1 : fitsAt(2);
			if (scale > 1) {
				image = prescale(image, scale, false, &renderSub);
			} else {
				bool const smooth = mode != SCALE_1X && config.filter;
				C3D_TexSetFilter(&texture, smooth ? GPU_LINEAR : GPU_NEAREST, smooth ? GPU_LINEAR : GPU_NEAREST);
			}
			sx = fx / scale;
			sy = fy / scale;
		}
	}
	C2D_TargetClear(top, C2D_Color32(0, 0, 0, 255));
	C2D_SceneBegin(top);
	if (image.tex) {
		float const w = image.subtex->width * sx, h = image.subtex->height * sy;
		C2D_DrawImageAt(image, (int) ((400 - w) / 2), (int) ((240 - h) / 2), 0.5f, NULL, sx, sy);
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
