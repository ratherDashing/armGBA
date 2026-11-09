/* Copyright (c) 2026 armGBA 3DS port contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The 3DS's buttons as a RetroPad: A, B, X, Y, L, R, Start and Select as they are, ZL and ZR as L2
// and R2 (fast-forward in armGBA), the Circle Pad as the D-pad and the left stick, the C-Stick (New
// 3DS) as the right stick.

#include "frontend.h"

u32 keysHeldNow, keysDownNow, keysRepeatNow;
uint32_t uiFrames;
static circlePosition circle, cstick;
// Buttons a test script holds down (main.c), on top of the real ones, by game frame and by frame
// of the whole app (menus included).
u32 scriptedKeys, scriptedUiKeys;
static u32 scriptedBefore;

void inputPoll(void) {
	hidScanInput();
	++uiFrames;
	scriptUiFrame(uiFrames);
	u32 const scripted = scriptedKeys | scriptedUiKeys;
	keysHeldNow = hidKeysHeld() | scripted;
	keysDownNow = hidKeysDown() | (scripted & ~scriptedBefore);
	keysRepeatNow = hidKeysDownRepeat() | (scripted & ~scriptedBefore);
	scriptedBefore = scripted;
	hidCircleRead(&circle);
	hidCstickRead(&cstick);
}

static const u32 buttonFor[16] = {
	[RETRO_DEVICE_ID_JOYPAD_B] = KEY_B,
	[RETRO_DEVICE_ID_JOYPAD_Y] = KEY_Y,
	[RETRO_DEVICE_ID_JOYPAD_SELECT] = KEY_SELECT,
	[RETRO_DEVICE_ID_JOYPAD_START] = KEY_START,
	[RETRO_DEVICE_ID_JOYPAD_UP] = KEY_UP,
	[RETRO_DEVICE_ID_JOYPAD_DOWN] = KEY_DOWN,
	[RETRO_DEVICE_ID_JOYPAD_LEFT] = KEY_LEFT,
	[RETRO_DEVICE_ID_JOYPAD_RIGHT] = KEY_RIGHT,
	[RETRO_DEVICE_ID_JOYPAD_A] = KEY_A,
	[RETRO_DEVICE_ID_JOYPAD_X] = KEY_X,
	[RETRO_DEVICE_ID_JOYPAD_L] = KEY_L,
	[RETRO_DEVICE_ID_JOYPAD_R] = KEY_R,
	[RETRO_DEVICE_ID_JOYPAD_L2] = KEY_ZL,
	[RETRO_DEVICE_ID_JOYPAD_R2] = KEY_ZR,
};

static int16_t axis(int value) {
	// The Circle Pad reads about +-156.
	int scaled = value * 32767 / 150;
	return scaled > 32767 ? 32767 : scaled < -32767 ? -32767 : scaled;
}

int16_t inputState(unsigned port, unsigned device, unsigned index, unsigned id) {
	if (port) {
		return 0;
	}
	switch (device & RETRO_DEVICE_MASK) {
	case RETRO_DEVICE_JOYPAD:
		if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
			int16_t mask = 0;
			for (unsigned i = 0; i < 16; ++i) {
				if (buttonFor[i] && (keysHeldNow & buttonFor[i])) {
					mask |= 1 << i;
				}
			}
			return mask;
		}
		return id < 16 && buttonFor[id] && (keysHeldNow & buttonFor[id]) ? 1 : 0;
	case RETRO_DEVICE_ANALOG: {
		const circlePosition* p = index == RETRO_DEVICE_INDEX_ANALOG_RIGHT ? &cstick : index == RETRO_DEVICE_INDEX_ANALOG_LEFT ? &circle : NULL;
		if (!p) {
			return 0;
		}
		// Up is negative for libretro, positive for the 3DS.
		return id == RETRO_DEVICE_ID_ANALOG_X ? axis(p->dx) : id == RETRO_DEVICE_ID_ANALOG_Y ? axis(-p->dy) : 0;
	}
	default:
		return 0;
	}
}
