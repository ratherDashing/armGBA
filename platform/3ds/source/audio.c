/* Copyright (c) 2026 armGBA 3DS port contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Sound through the DSP (ndsp): the core's samples go into a ring of wave buffers, each queued as
// it fills. How much sound waits to be played is the sound's delay: with Low Lag about 30 ms of it
// is kept (two frames' worth), otherwise 60. With Low Lag the frames follow the screen, which
// refreshes a little faster than the GBA did, so the DSP plays at the core's rate scaled by the
// same amount, and a little faster or slower still to hold the queue level (soundPace).

#include "frontend.h"

#include <stdlib.h>
#include <string.h>

enum {
	BUFFERS = 16,
	BUFFER_FRAMES = 256,
	CHANNEL = 0,
};

static ndspWaveBuf waveBufs[BUFFERS];
static int16_t* samples;
static int filling;
static size_t filled;
static bool running;
static double baseRate;
static float paceNow;
// Times the queue was found empty when more sound arrived: a gap that was heard.
uint32_t soundGaps;

// The sound kept waiting, in sample frames. With Low Lag, 30 ms; for a quarter of a minute after the
// queue ran dry or a frame came very late (a game loading, a burst of new code to translate: those
// come in runs), 55 ms, since a frame of 35 to 45 ms empties the smaller one (Mario Kart's first
// race, 2026-11-12).
static u64 cushionUntil;
static unsigned target(void) {
	if (!config.lowLag) {
		return 2048;
	}
	return osGetTime() < cushionUntil ? 1792 : 1024;
}

void soundCushion(void) {
	cushionUntil = osGetTime() + 15000;
}

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
	baseRate = rate;
	paceNow = 1.0f;
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
	// A cushion of silence, so the queue starts at its level and a slow frame doesn't run it dry.
	static const int16_t silence[BUFFER_FRAMES * 2];
	for (unsigned i = 0; i < target() / BUFFER_FRAMES; ++i) {
		soundBatch(silence, BUFFER_FRAMES);
	}
	soundGaps = 0;
	cushionUntil = 0;
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
	static bool refilling;
	if (frames && !queued() && !refilling) {
		// Ran dry: the gap has been heard already. The queue starts again at the larger level, so
		// the next slow frame isn't another one.
		++soundGaps;
		soundCushion();
		static const int16_t silence[BUFFER_FRAMES * 2];
		refilling = true;
		for (unsigned i = 0; i + 1 < target() / BUFFER_FRAMES; ++i) {
			soundBatch(silence, BUFFER_FRAMES);
		}
		refilling = false;
	}
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

// The sound waiting to be played, in sample frames: the buffers queued, less what has played of the
// one playing, and what is in the one being filled.
unsigned soundQueuedFrames(void) {
	if (!running) {
		return target();
	}
	unsigned n = filled;
	bool playing = false;
	for (int i = 0; i < BUFFERS; ++i) {
		n += waveBufs[i].status == NDSP_WBUF_QUEUED || waveBufs[i].status == NDSP_WBUF_PLAYING ? BUFFER_FRAMES : 0;
		playing = playing || waveBufs[i].status == NDSP_WBUF_PLAYING;
	}
	if (playing) {
		u32 const played = ndspChnGetSamplePos(CHANNEL);
		n -= played < BUFFER_FRAMES ? played : BUFFER_FRAMES;
	}
	return n;
}

// The queue against its level: 50 at the level, 100 at twice it or more.
unsigned soundOccupancy(void) {
	if (!running) {
		return 50;
	}
	unsigned const percent = soundQueuedFrames() * 50 / target();
	return percent > 100 ? 100 : percent;
}

// Plays at ratio times the core's rate (the frames' pace against the GBA's), and up to half a
// percent faster or slower to bring the queue back to its level.
void soundPace(double ratio) {
	if (!running) {
		return;
	}
	double off = ((double) soundQueuedFrames() - target()) / target();
	off = off > 1 ? 1 : off < -1 ? -1 : off;
	float const pace = (float) (ratio * (1.0 + 0.005 * off));
	if (pace != paceNow) {
		paceNow = pace;
		ndspChnSetRate(CHANNEL, (float) baseRate * pace);
	}
}

void soundPause(bool paused) {
	if (running) {
		ndspChnSetPaused(CHANNEL, paused);
	}
}
