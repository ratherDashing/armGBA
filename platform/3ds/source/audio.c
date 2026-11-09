/* Copyright (c) 2026 armGBA 3DS port contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Sound through the DSP (ndsp): the core's samples go into a ring of wave buffers, each queued as
// it fills. The frontend paces frames by the screen, which refreshes a little faster than the GBA
// did, so the DSP plays at the core's rate scaled by the same amount and the queue stays level.

#include "frontend.h"

#include <stdlib.h>
#include <string.h>

enum {
	BUFFERS = 8,
	BUFFER_FRAMES = 512,
	CHANNEL = 0,
};

static ndspWaveBuf waveBufs[BUFFERS];
static int16_t* samples;
static int filling;
static size_t filled;
static bool running;

bool soundInit(double rate) {
	Result rc = ndspInit();
	// An emulator does the DSP's work itself and never runs its firmware, but ndsp won't start
	// without one: it gets a blank one there. Never on a console, where the DSP would run it.
	if (R_FAILED(rc) && isEmulator()) {
		static uint8_t blankFirmware[4096];
		ndspUseComponent(blankFirmware, sizeof(blankFirmware), 0xFF, 0xFF);
		rc = ndspInit();
	}
	if (R_FAILED(rc)) {
		logLine("audio: ndspInit %08lX: no sdmc:/3ds/dspfirm.cdc? Dump it with DSP1 for sound", rc);
		return false;
	}
	samples = linearAlloc(BUFFERS * BUFFER_FRAMES * 4);
	if (!samples) {
		ndspExit();
		return false;
	}
	memset(samples, 0, BUFFERS * BUFFER_FRAMES * 4);
	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	ndspChnReset(CHANNEL);
	ndspChnSetInterp(CHANNEL, NDSP_INTERP_LINEAR);
	ndspChnSetRate(CHANNEL, (float) rate);
	ndspChnSetFormat(CHANNEL, NDSP_FORMAT_STEREO_PCM16);
	float mix[12] = { 1.0f, 1.0f };
	ndspChnSetMix(CHANNEL, mix);
	memset(waveBufs, 0, sizeof(waveBufs));
	for (int i = 0; i < BUFFERS; ++i) {
		waveBufs[i].data_pcm16 = samples + i * BUFFER_FRAMES * 2;
		waveBufs[i].status = NDSP_WBUF_DONE;
	}
	filling = 0;
	filled = 0;
	running = true;
	logLine("audio: %.1f Hz", rate);
	return true;
}

void soundExit(void) {
	if (!running) {
		return;
	}
	ndspChnWaveBufClear(CHANNEL);
	ndspExit();
	linearFree(samples);
	samples = NULL;
	running = false;
}

static int queued(void) {
	int n = 0;
	for (int i = 0; i < BUFFERS; ++i) {
		n += waveBufs[i].status == NDSP_WBUF_QUEUED || waveBufs[i].status == NDSP_WBUF_PLAYING;
	}
	return n;
}

size_t soundBatch(const int16_t* data, size_t frames) {
	if (!running) {
		return frames;
	}
	size_t const total = frames;
	while (frames) {
		ndspWaveBuf* buf = &waveBufs[filling];
		if (buf->status == NDSP_WBUF_QUEUED || buf->status == NDSP_WBUF_PLAYING) {
			// Every buffer is waiting to play: the game runs ahead of the sound, which drops the rest.
			break;
		}
		size_t take = BUFFER_FRAMES - filled;
		if (take > frames) {
			take = frames;
		}
		memcpy(buf->data_pcm16 + filled * 2, data, take * 4);
		filled += take;
		data += take * 2;
		frames -= take;
		if (filled == BUFFER_FRAMES) {
			buf->nsamples = BUFFER_FRAMES;
			DSP_FlushDataCache(buf->data_pcm16, BUFFER_FRAMES * 4);
			ndspChnWaveBufAdd(CHANNEL, buf);
			filling = (filling + 1) % BUFFERS;
			filled = 0;
		}
	}
	return total;
}

unsigned soundOccupancy(void) {
	if (!running) {
		return 50;
	}
	return (unsigned) (queued() * 100 / BUFFERS);
}

void soundPause(bool paused) {
	if (running) {
		ndspChnSetPaused(CHANNEL, paused);
	}
}
