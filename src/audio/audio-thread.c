/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The mixer on a second thread, as render-thread.c does for the renderer. The emulation thread
// keeps what the game can see of the sound hardware: the FIFOs and their DMA requests, the tone
// channels' lengths, sweep and envelopes, the status bits, and channel 3's wave RAM. It makes no
// samples: it logs each call that makes or changes them, with its time, and the worker makes the
// same calls in the same order on a copy of the sound state, so the samples are the ones one thread
// would have made. They are waited for where the frontend takes them, and the copy's state is taken
// back for a save state.
//
// One console at a time: it is named by audioThreaded rather than by a pointer in struct GBA, so
// states are laid out as before.

#include "gba.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__3DS__)
bool audioThreadHostCreate(pthread_t* thread, void* (*function)(void*), void* argument);
uint64_t renderThreadHostTicks(void);
// For the frontend's numbers: time the emulation thread waits for the worker, and the worker spends
// mixing, in the host's ticks.
uint64_t audioWaitTicks, audioWorkTicks;
#define HOST_TICKS() renderThreadHostTicks()
#else
#define HOST_TICKS() 0
#endif

enum {
	LOG_ENTRIES = 4096,
	// The worker sleeps when it has nothing, and is woken once this many calls wait: waking it for
	// each would cost more than the mixing does.
	WAKE_AFTER = 128,
};

struct Entry {
	uint8_t kind;
	uint8_t a;
	uint16_t b;
	uint32_t time;
	uint32_t value;
};

struct AudioThread {
	pthread_mutex_t mutex;
	pthread_cond_t wake;
	pthread_cond_t idle;
	pthread_t thread;
	// Only its sound state is used, and the clock the calls read.
	struct GBA* copy;
	// A ring: positions only grow, and an entry is at its position modulo LOG_ENTRIES.
	struct Entry log[LOG_ENTRIES];
	atomic_uint written;
	atomic_uint done;
	atomic_bool asleep;
	atomic_bool awaited;
	unsigned sinceWake;
	bool quit;
};

struct GBA* audioThreaded;
int audioThreadQuiet;
static struct AudioThread* thread;

// Makes a logged call on the copy.
static void play(struct GBA* copy, const struct Entry* e) {
	// What the calls take as now.
	copy->timing.masterCycles = e->time;
	switch (e->kind) {
	case AUDIO_LOG_SAMPLE:
		audioSample(copy, e->time);
		break;
	case AUDIO_LOG_UNCACHE:
		copy->audio.psg.cached = false;
		break;
	case AUDIO_LOG_FINISH:
		audioFinishInterval(copy, e->time);
		copy->audio.nextSample += CPU_FREQUENCY / 0x4000;
		break;
	case AUDIO_LOG_FILL:
		audioFillFifo(copy, e->a, e->time, e->value);
		break;
	case AUDIO_LOG_PSG:
		psgWrite(copy, e->a, e->b);
		break;
	case AUDIO_LOG_FRAME:
		psgUpdateFrame(copy, e->time);
		break;
	case AUDIO_LOG_CNT_HI:
		audioWriteSoundcntHi(copy, e->b);
		break;
	case AUDIO_LOG_CNT_X:
		audioWriteSoundcntX(copy, e->b);
		break;
	case AUDIO_LOG_BIAS:
		audioWriteSoundbias(copy, e->b);
		break;
	case AUDIO_LOG_WAVE_WRITE:
		audioWriteWaveRam(copy, e->a, e->value);
		break;
	case AUDIO_LOG_WAVE_READ:
		audioReadWaveRam(copy, e->a);
		break;
	case AUDIO_LOG_WAVE_SHAPE:
		audioWriteWaveShape(copy, e->a, e->b);
		break;
	case AUDIO_LOG_WAVE_VOLUME:
		audioWriteWaveVolume(copy, e->a);
		break;
	}
}

// The worker: makes the logged calls as far as they are written, then sleeps until woken. It never
// spins: on the 3DS it shares its core with a render worker, at a higher priority.
static void* work(void* context) {
	struct AudioThread* t = context;
	for (;;) {
		unsigned done = atomic_load(&t->done);
		unsigned const written = atomic_load(&t->written);
		if (done == written) {
			pthread_mutex_lock(&t->mutex);
			if (atomic_load(&t->awaited)) {
				pthread_cond_signal(&t->idle);
			}
			atomic_store(&t->asleep, true);
			while (done == atomic_load(&t->written) && !t->quit) {
				pthread_cond_wait(&t->wake, &t->mutex);
			}
			atomic_store(&t->asleep, false);
			bool const quit = t->quit && done == atomic_load(&t->written);
			pthread_mutex_unlock(&t->mutex);
			if (quit) {
				return NULL;
			}
			continue;
		}
#if defined(__3DS__)
		uint64_t const began = HOST_TICKS();
#endif
		for (; done != written; ++done) {
			play(t->copy, &t->log[done % LOG_ENTRIES]);
		}
#if defined(__3DS__)
		audioWorkTicks += HOST_TICKS() - began;
#endif
		atomic_store(&t->done, done);
	}
}

static void wake(struct AudioThread* t) {
	t->sinceWake = 0;
	if (atomic_load(&t->asleep)) {
		pthread_mutex_lock(&t->mutex);
		pthread_cond_signal(&t->wake);
		pthread_mutex_unlock(&t->mutex);
	}
}

// Waits until the worker has made every call logged. What is left is short, so it spins for it
// first, up to a millisecond.
static void drain(struct AudioThread* t) {
	unsigned const target = atomic_load(&t->written);
	if (atomic_load(&t->done) == target) {
		return;
	}
#if defined(__3DS__)
	uint64_t const began = HOST_TICKS();
#endif
	wake(t);
	struct timespec start;
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (int i = 1; atomic_load(&t->done) != target; ++i) {
		if (!(i & 0xFF)) {
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			if ((now.tv_sec - start.tv_sec) * 1000000000 + now.tv_nsec - start.tv_nsec > 1000000) {
				pthread_mutex_lock(&t->mutex);
				atomic_store(&t->awaited, true);
				while (atomic_load(&t->done) != target) {
					pthread_cond_wait(&t->idle, &t->mutex);
				}
				atomic_store(&t->awaited, false);
				pthread_mutex_unlock(&t->mutex);
				break;
			}
		}
#if defined(__x86_64__)
		__builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
		__asm__ volatile("yield");
#endif
	}
#if defined(__3DS__)
	audioWaitTicks += HOST_TICKS() - began;
#endif
}

void audioThreadWrite(uint32_t kind, uint32_t a, uint32_t b, uint32_t time, uint32_t value) {
	struct AudioThread* t = thread;
	unsigned const written = atomic_load_explicit(&t->written, memory_order_relaxed);
	if (written - atomic_load(&t->done) >= LOG_ENTRIES) {
		drain(t);
	}
	t->log[written % LOG_ENTRIES] = (struct Entry) { kind, a, b, time, value };
	atomic_store(&t->written, written + 1);
	if (++t->sinceWake >= WAKE_AFTER) {
		wake(t);
	}
}

void audioThreadKick(struct GBA* gba) {
	if (gba == audioThreaded) {
		wake(thread);
	}
}

void audioThreadTake(struct GBA* gba) {
	if (gba != audioThreaded) {
		return;
	}
	struct AudioThread* t = thread;
	drain(t);
	struct Audio* audio = &gba->audio;
	struct Audio* mixed = &t->copy->audio;
	// As one thread does, what doesn't fit is dropped.
	uint32_t const room = sizeof(audio->out) / sizeof(*audio->out) - audio->outCount;
	uint32_t const count = mixed->outCount < room ? mixed->outCount : room;
	memcpy(&audio->out[audio->outCount], mixed->out, count * sizeof(*audio->out));
	audio->outCount += count;
	mixed->outCount = 0;
}

// The copy has made every sample, so where each channel is in its wave is the copy's: the tone
// channels' state, the FIFOs' held samples and the interval being mixed. The FIFOs themselves, the
// frame sequencer's event and the output are this console's own.
void audioThreadSync(struct GBA* gba) {
	if (gba != audioThreaded) {
		return;
	}
	audioThreadTake(gba);
	struct Audio* audio = &gba->audio;
	const struct Audio* mixed = &thread->copy->audio;
	struct Event const frameEvent = audio->psg.frameEvent;
#if defined(ARMGBA_AUDIO_CHECK)
	// A test build: with channel 3 brought up to now in both, everything but where the other
	// channels are in their waves must already be the same.
	{
		struct GBA* copy = thread->copy;
		struct Psg const keptMine = audio->psg;
		struct Psg const keptIts = copy->audio.psg;
		psgRun(gba, timingCurrentTime(gba), 0x4);
		psgRun(copy, timingCurrentTime(gba), 0x4);
		struct Psg mine = audio->psg;
		struct Psg its = copy->audio.psg;
		audio->psg = keptMine;
		copy->audio.psg = keptIts;
		struct Psg* both[2] = { &mine, &its };
		for (int i = 0; i < 2; ++i) {
			struct Psg* p = both[i];
			memset(&p->frameEvent, 0, sizeof(p->frameEvent));
			p->ch1.index = p->ch2.index = 0;
			p->ch1.lastUpdate = p->ch2.lastUpdate = 0;
			p->ch1.sample = p->ch2.sample = p->ch3.sample = p->ch4.sample = 0;
			p->ch4.lfsr = 0;
			p->ch4.lastEvent = 0;
			p->ch4.samples = p->ch4.nSamples = 0;
			p->cached = false;
			p->cachedLeft = p->cachedRight = 0;
			p->nextChange = 0;
		}
		extern int audioCheckFailed;
		if (memcmp(&mine, &its, sizeof(mine)) || audio->nextSample != mixed->nextSample || audio->soundbias != mixed->soundbias ||
		    audio->enable != mixed->enable || audio->volume != mixed->volume) {
			++audioCheckFailed;
			for (size_t k = 0; k < sizeof(mine) && audioCheckFailed < 4; ++k) {
				if (((uint8_t*) &mine)[k] != ((uint8_t*) &its)[k]) {
					fprintf(stderr, "audio check: psg+%zu mine %02x its %02x (ch3 at %zu, wavedata %zu, nextUpdate %zu, ch4 %zu)\n", k, ((uint8_t*) &mine)[k], ((uint8_t*) &its)[k],
					        offsetof(struct Psg, ch3), offsetof(struct Psg, ch3.wavedata32), offsetof(struct Psg, ch3.nextUpdate), offsetof(struct Psg, ch4));
				}
			}
		}
	}
#endif
	audio->psg = mixed->psg;
	audio->psg.frameEvent = frameEvent;
	memcpy(audio->chA.samples, mixed->chA.samples, sizeof(audio->chA.samples));
	memcpy(audio->chB.samples, mixed->chB.samples, sizeof(audio->chB.samples));
	audio->lastSample = mixed->lastSample;
	audio->sampleIndex = mixed->sampleIndex;
	memcpy(audio->currentSamples, mixed->currentSamples, sizeof(audio->currentSamples));
}

void audioThreadClone(struct GBA* gba) {
	if (gba != audioThreaded) {
		return;
	}
	struct AudioThread* t = thread;
	drain(t);
	t->copy->audio = gba->audio;
	t->copy->audio.outCount = 0;
}

bool audioThreadStart(struct GBA* gba) {
	if (audioThreaded) {
		return audioThreaded == gba;
	}
	struct AudioThread* t = calloc(1, sizeof(*t));
	if (t) {
		t->copy = calloc(1, sizeof(struct GBA));
	}
	if (!t || !t->copy) {
		free(t);
		return false;
	}
	pthread_mutex_init(&t->mutex, NULL);
	pthread_cond_init(&t->wake, NULL);
	pthread_cond_init(&t->idle, NULL);
	t->copy->audio = gba->audio;
	t->copy->audio.outCount = 0;
#if defined(__3DS__)
	if (!audioThreadHostCreate(&t->thread, work, t)) {
#else
	if (pthread_create(&t->thread, NULL, work, t)) {
#endif
		free(t->copy);
		free(t);
		return false;
	}
	thread = t;
	audioThreadQuiet = 0;
	audioThreaded = gba;
	return true;
}

void audioThreadStop(struct GBA* gba) {
	if (gba != audioThreaded) {
		return;
	}
	struct AudioThread* t = thread;
	audioThreadSync(gba);
	audioThreaded = NULL;
	pthread_mutex_lock(&t->mutex);
	t->quit = true;
	pthread_cond_signal(&t->wake);
	pthread_mutex_unlock(&t->mutex);
	pthread_join(t->thread, NULL);
	thread = NULL;
	free(t->copy);
	free(t);
}
