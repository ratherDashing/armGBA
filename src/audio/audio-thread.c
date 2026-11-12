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
	// Only its sound state is used, and the clock the calls read. Which of the two it is changes
	// when the emulation thread mixes anew (rescue): the worker reads it once it has the claim.
	struct GBA* _Atomic copy;
	// The other one, for mixing anew, and whether a worker that was passed over still mixes on it.
	struct GBA* spare;
	atomic_bool spareInUse;
	// The copy's sound state when the samples were last taken, less the samples, and how far the
	// log was made then: calls from there on make every sample since.
	struct Audio* checkpoint;
	unsigned checkpointAt;
	// A ring: positions only grow, and an entry is at its position modulo LOG_ENTRIES.
	struct Entry log[LOG_ENTRIES];
	atomic_uint written;
	atomic_uint done;
	atomic_bool asleep;
	atomic_bool awaited;
	// Who is making the logged calls on the copy, in the low two bits: nobody (0), the worker (1),
	// the emulation thread in its place (2), or the worker storing how far it got (3). The rest
	// counts the times the emulation thread took the calls from a worker in the middle of them
	// (rescue); a worker that finds the word changed leaves its work as it is.
	atomic_uint busy;
	unsigned sinceWake;
	bool quit;
};

// Times the emulation thread made the worker's calls for it, and times it made them anew on the
// other copy with the worker stopped in the middle of them.
unsigned long audioStoodIn, audioMixedAnew;

enum {
	WHO = 3,
	NOBODY = 0,
	WORKER = 1,
	EMULATION = 2,
	STORING = 3,
	TURN = 4,
};

// What of the sound state the checkpoint keeps: all but the samples made, which are empty then.
#define CHECKPOINT_SIZE offsetof(struct Audio, out)

static void checkpoint(struct AudioThread* t) {
	memcpy(t->checkpoint, &atomic_load(&t->copy)->audio, CHECKPOINT_SIZE);
	t->checkpointAt = atomic_load(&t->done);
}

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
#if !defined(__3DS__)
		// ARMGBA_AUDIO_STALL=n: the worker is late by 3 ms about one time in n, to test the
		// emulation thread standing in.
		static int stallEvery = -1;
		if (stallEvery < 0) {
			const char* v = getenv("ARMGBA_AUDIO_STALL");
			stallEvery = v ? atoi(v) : 0;
		}
		if (stallEvery && (done * 2654435761u >> 12) % (unsigned) stallEvery == 0) {
			struct timespec nap = { 0, 3000000 };
			nanosleep(&nap, NULL);
		}
#endif
		unsigned free = atomic_load(&t->busy);
		if ((free & WHO) != NOBODY || !atomic_compare_exchange_strong(&t->busy, &free, free | WORKER)) {
			// The emulation thread is making the calls itself (drain): it makes all there are.
#if defined(__x86_64__)
			__builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
			__asm__ volatile("yield");
#endif
			continue;
		}
		unsigned const mine = free | WORKER;
#if defined(__3DS__)
		uint64_t const began = HOST_TICKS();
#endif
		// It may have made some since this thread looked, and past what was written then: both
		// are read again now that the copy is this thread's. The word is as it was after they are
		// read, or they may be of after a rescue.
		struct GBA* const copy = atomic_load(&t->copy);
		unsigned const upTo = atomic_load(&t->written);
		done = atomic_load(&t->done);
		if (atomic_load(&t->busy) != mine) {
			atomic_store(&t->spareInUse, false);
			continue;
		}
#if !defined(__3DS__)
		// ARMGBA_AUDIO_STALL_MID=n: the worker stops for 5 ms in the middle of its calls about one
		// time in n, to test the emulation thread mixing anew.
		static int stallMidEvery = -1;
		if (stallMidEvery < 0) {
			const char* v = getenv("ARMGBA_AUDIO_STALL_MID");
			stallMidEvery = v ? atoi(v) : 0;
		}
		unsigned const stallAt = stallMidEvery && (done * 2246822519u >> 12) % (unsigned) stallMidEvery == 0 ? done + (upTo - done) / 2 : upTo;
#endif
		bool passedOver = false;
		for (; done != upTo; ++done) {
#if !defined(__3DS__)
			if (done == stallAt) {
				struct timespec nap = { 0, 5000000 };
				nanosleep(&nap, NULL);
			}
#endif
			// The entry is copied before the word is looked at: with the word unchanged, the
			// emulation thread hasn't written over it since.
			struct Entry const e = t->log[done % LOG_ENTRIES];
			if (atomic_load(&t->busy) != mine) {
				passedOver = true;
				break;
			}
			play(copy, &e);
		}
#if defined(__3DS__)
		audioWorkTicks += HOST_TICKS() - began;
#endif
		unsigned expected = mine;
		if (passedOver || !atomic_compare_exchange_strong(&t->busy, &expected, free | STORING)) {
			// The emulation thread made these calls anew on the other copy: this one is its spare
			// from now.
			atomic_store(&t->spareInUse, false);
			continue;
		}
		atomic_store(&t->done, done);
		atomic_store(&t->busy, free);
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
		if (!(i & 0x3F)) {
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			long long const waited = (now.tv_sec - start.tv_sec) * 1000000000ll + now.tv_nsec - start.tv_nsec;
			// The worker hasn't started on it in a tenth of a millisecond: its core is slow to wake,
			// or has been taken away (on a New 3DS, for 20 to 30 ms about twice a minute, which
			// emptied the sound queue: 2026-11-12). This thread makes the calls in its place; the
			// worker, when it comes, finds them made.
			unsigned word = atomic_load(&t->busy);
			if (waited > 100000 && (word & WHO) == NOBODY && atomic_compare_exchange_strong(&t->busy, &word, word | EMULATION)) {
				struct GBA* const copy = atomic_load(&t->copy);
				unsigned done = atomic_load(&t->done);
				for (; done != target; ++done) {
					play(copy, &t->log[done % LOG_ENTRIES]);
				}
				atomic_store(&t->done, done);
				atomic_store(&t->busy, word);
				++audioStoodIn;
				break;
			}
			// The worker has the calls and has been stopped in the middle of them (its core taken
			// away while it mixed: three times in two minutes of Mario Kart on a New 3DS, for 3.5,
			// 10.6 and 26.7 ms, 2026-11-12). They are made anew on the other copy, from the sound
			// state of when the samples were last taken; that copy is the one from now, and the
			// worker leaves its own as it is when it comes back. Not while the other copy is still
			// with a worker passed over before, nor when the log has gone round since.
			bool mayMixAnew = waited > 1000000 && (word & WHO) == WORKER && !atomic_load(&t->spareInUse) && target - t->checkpointAt <= LOG_ENTRIES;
			if (mayMixAnew) {
				// Set first: the worker clears it as soon as it finds the word changed.
				atomic_store(&t->spareInUse, true);
				mayMixAnew = atomic_compare_exchange_strong(&t->busy, &word, (word & ~(unsigned) WHO) + TURN + EMULATION);
				if (!mayMixAnew) {
					atomic_store(&t->spareInUse, false);
				}
			}
			if (mayMixAnew) {
				struct GBA* const old = atomic_load(&t->copy);
				struct GBA* const copy = t->spare;
				memcpy(&copy->audio, t->checkpoint, CHECKPOINT_SIZE);
				copy->audio.outCount = 0;
				for (unsigned at = t->checkpointAt; at != target; ++at) {
					play(copy, &t->log[at % LOG_ENTRIES]);
				}
				t->spare = old;
				atomic_store(&t->copy, copy);
				atomic_store(&t->done, target);
				atomic_store(&t->busy, (word & ~(unsigned) WHO) + TURN);
				++audioMixedAnew;
				break;
			}
			if (waited > 1000000) {
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
	struct Audio* mixed = &atomic_load(&t->copy)->audio;
	// As one thread does, what doesn't fit is dropped.
	uint32_t const room = sizeof(audio->out) / sizeof(*audio->out) - audio->outCount;
	uint32_t const count = mixed->outCount < room ? mixed->outCount : room;
	memcpy(&audio->out[audio->outCount], mixed->out, count * sizeof(*audio->out));
	audio->outCount += count;
	mixed->outCount = 0;
	checkpoint(t);
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
	const struct Audio* mixed = &atomic_load(&thread->copy)->audio;
	struct Event const frameEvent = audio->psg.frameEvent;
#if defined(ARMGBA_AUDIO_CHECK)
	// A test build: with channel 3 brought up to now in both, everything but where the other
	// channels are in their waves must already be the same.
	{
		struct GBA* copy = atomic_load(&thread->copy);
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
	struct GBA* const copy = atomic_load(&t->copy);
	copy->audio = gba->audio;
	copy->audio.outCount = 0;
	checkpoint(t);
}

bool audioThreadStart(struct GBA* gba) {
	if (audioThreaded) {
		return audioThreaded == gba;
	}
	struct AudioThread* t = calloc(1, sizeof(*t));
	struct GBA* const copy = calloc(1, sizeof(struct GBA));
	if (t) {
		t->spare = calloc(1, sizeof(struct GBA));
		t->checkpoint = calloc(1, sizeof(struct Audio));
	}
	if (!t || !copy || !t->spare || !t->checkpoint) {
		if (t) {
			free(t->spare);
			free(t->checkpoint);
		}
		free(copy);
		free(t);
		return false;
	}
	atomic_init(&t->copy, copy);
	pthread_mutex_init(&t->mutex, NULL);
	pthread_cond_init(&t->wake, NULL);
	pthread_cond_init(&t->idle, NULL);
	copy->audio = gba->audio;
	copy->audio.outCount = 0;
	checkpoint(t);
#if defined(__3DS__)
	if (!audioThreadHostCreate(&t->thread, work, t)) {
#else
	if (pthread_create(&t->thread, NULL, work, t)) {
#endif
		free(copy);
		free(t->spare);
		free(t->checkpoint);
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
	free(atomic_load(&t->copy));
	free(t->spare);
	free(t->checkpoint);
	free(t);
}
