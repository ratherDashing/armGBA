/* Copyright (c) 2026 armGBA 3DS port contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The game on the top screen: each frame is copied into linear memory as RGB565, turned into a
// tiled texture by the GPU's transfer engine, and drawn scaled with citro2d.

#include "frontend.h"

#include <citro2d.h>
#include <citro3d.h>
#include <string.h>

enum {
	TEX_W = 512,
	TEX_H = 256,
};

static C3D_RenderTarget* top;
static C3D_Tex texture;
static uint16_t* staging;
static unsigned frameWidth = 240, frameHeight = 160;
static enum retro_pixel_format format = RETRO_PIXEL_FORMAT_0RGB1555;
static bool haveFrame;
// The last frame as RGB565, tightly packed, for test screenshots.
static uint16_t lastFrame[TEX_W * TEX_H];

void displayInit(void) {
	gfxInitDefault();
	gfxSet3D(false);
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
	C2D_Prepare();
	top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	C3D_TexInit(&texture, TEX_W, TEX_H, GPU_RGB565);
	staging = linearAlloc(TEX_W * TEX_H * 2);
	memset(staging, 0, TEX_W * TEX_H * 2);
}

void displayExit(void) {
	linearFree(staging);
	C3D_TexDelete(&texture);
	C2D_Fini();
	C3D_Fini();
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

// The core's frame (NULL: the same one again) into the staging buffer.
void displayFrame(const void* data, unsigned width, unsigned height, size_t pitch) {
	if (!data) {
		return;
	}
	if (width > TEX_W) {
		width = TEX_W;
	}
	if (height > TEX_H) {
		height = TEX_H;
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
		memcpy(&staging[y * TEX_W], out, width * 2);
	}
	haveFrame = true;
	GSPGPU_FlushDataCache(staging, TEX_W * height * 2);
	C3D_SyncDisplayTransfer((u32*) staging, GX_BUFFER_DIM(TEX_W, TEX_H), (u32*) texture.data, GX_BUFFER_DIM(TEX_W, TEX_H),
	                        GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0) |
	                            GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) |
	                            GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
}

// Draws the last frame scaled to the top screen and waits for the screen to take it.
void displayPresent(void) {
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
	C2D_TargetClear(top, C2D_Color32(0, 0, 0, 255));
	C2D_SceneBegin(top);
	if (haveFrame) {
		C3D_TexSetFilter(&texture, config.filter && config.scale != SCALE_1X ? GPU_LINEAR : GPU_NEAREST,
		                 config.filter && config.scale != SCALE_1X ? GPU_LINEAR : GPU_NEAREST);
		Tex3DS_SubTexture sub = {
			.width = frameWidth,
			.height = frameHeight,
			.left = 0.0f,
			.top = 1.0f,
			.right = (float) frameWidth / TEX_W,
			.bottom = 1.0f - (float) frameHeight / TEX_H,
		};
		C2D_Image image = { &texture, &sub };
		float sx = 1.0f, sy = 1.0f;
		switch (config.scale) {
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
	C3D_FrameEnd(0);
}

void displayClearTop(void) {
	haveFrame = false;
	displayPresent();
}
