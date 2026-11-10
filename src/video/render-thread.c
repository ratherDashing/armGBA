/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The renderer on a second thread. The emulation thread keeps the renderer's registers as before
// but draws nothing: it logs each call to the renderer, and with each line, the VRAM pages, OAM and
// palette changed since the last one. The worker makes the same calls in the same order on a copy
// of the renderer and of that memory, so it draws what one thread would have. The frame is waited
// for where it ends; or, when started late, the frame before it is handed over there instead, so
// drawing a frame overlaps the next one's emulation, at the cost of a frame's delay.
//
// With two workers (the New 3DS has two cores to spare), each has a copy and goes through the whole
// log, one drawing the even lines and the other the odd ones: a line that isn't drawn leaves the
// renderer as a line that hasn't changed does, so each copy's state stays what one renderer's
// would be.

#include "gba.h"

#include <pthread.h>
#include <stdatomic.h>

#if defined(__3DS__)
bool renderThreadHostCreate(pthread_t* thread, void* (*function)(void*), void* argument);
// A second worker on another spare core, if there is one.
bool renderThreadHostCreateSecond(pthread_t* thread, void* (*function)(void*), void* argument);
// For the frontend's numbers: time the emulation thread spends waiting for the worker, and the
// worker spends drawing, in the host's ticks.
uint64_t renderThreadHostTicks(void);
uint64_t renderWaitTicks, renderWorkTicks;
#define HOST_TICKS() renderThreadHostTicks()
#else
#define HOST_TICKS() 0
#endif
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
#if defined(__3DS__)
	// The 3DS's heap is small: the ring waits for the worker when it's full (reserve), so a smaller
	// one only costs a wait now and then.
	LOG_SIZE = 2 << 20,
#else
	LOG_SIZE = 8 << 20,
#endif
	// The most one entry takes: a line with all of VRAM, OAM and the palette.
	LOG_MOST = 16 + SIZE_VRAM + SIZE_OAM + SIZE_PALETTE,
	VRAM_PAGE = 0x800,
	VRAM_PAGES = SIZE_VRAM / VRAM_PAGE,
};

enum {
	LOG_REGISTER,
	// The rest of the log up to its end is unused, since entries don't wrap around.
	LOG_SKIP,
	LOG_LINE,
	LOG_START_LINE,
	LOG_LATCH,
	LOG_FINISH,
};

struct RenderThread;

struct Worker {
	struct RenderThread* owner;
	pthread_t thread;
	// The copy the worker draws from; only its renderer, VRAM, OAM and palette are used.
	struct GBA* copy;
	// How much of the log this worker has done.
	atomic_ullong done;
	int index;
};

struct RenderThread {
	pthread_mutex_t mutex;
	pthread_cond_t wake;
	pthread_cond_t idle;
	struct Worker workers[2];
	int count;
	// A ring: positions only grow, and an entry is at its position modulo LOG_SIZE.
	uint8_t* log;
	// How much is logged.
	atomic_ullong written;
	atomic_int asleep;
	atomic_bool awaited;
	// With two workers, their renderers put together for whoever asks for the renderer.
	struct Renderer* merged;
	bool quit;
	// When late: the last frame the worker finished, and where in the log the last frame logged
	// ends.
	bool late;
	uint64_t finishLogged;
	uint16_t finished[SCREEN_HEIGHT][SCREEN_WIDTH];
};

// Waking a sleeping core takes longer than most waits here, so a thread first spins for a while on
// what it waits for: the worker for something logged, the other thread for the worker to have done
// up to target. It spins until then or until a millisecond has passed, and returns whether the wait
// ended.
// How much of the log every worker has done.
static uint64_t doneByAll(struct RenderThread* t) {
	uint64_t done = atomic_load(&t->workers[0].done);
	if (t->count > 1) {
		uint64_t other = atomic_load(&t->workers[1].done);
		done = other < done ? other : done;
	}
	return done;
}

static bool spin(struct RenderThread* t, const struct Worker* worker, uint64_t target) {
	struct timespec start;
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (int i = 0; worker ? atomic_load(&worker->done) == atomic_load(&t->written) : doneByAll(t) < target; ++i) {
		if (!(i & 0xFF)) {
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			if ((now.tv_sec - start.tv_sec) * 1000000000 + now.tv_nsec - start.tv_nsec > 1000000) {
				return false;
			}
		}
#if defined(__x86_64__)
		__builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
		__asm__ volatile("yield");
#endif
	}
	return true;
}

// Makes a logged call on the copy; returns the entry's kind.
static uint32_t play(struct GBA* copy, const uint8_t* entry, uint32_t* size) {
	uint32_t head[4];
	memcpy(head, entry, sizeof(head));
	*size = 16;
	switch (head[0]) {
	case LOG_REGISTER:
		copy->renderer.copyCycle = head[3];
		renderWriteRegister(copy, head[1], head[2]);
		break;
	case LOG_START_LINE:
		renderStartLine(copy, head[1]);
		break;
	case LOG_LATCH:
		renderLatchDispcnt(copy);
		break;
	case LOG_FINISH:
		renderFinishFrame(copy);
		break;
	case LOG_SKIP:
		*size = head[1];
		break;
	case LOG_LINE: {
		const uint8_t* data = entry + 16;
		uint64_t pages = head[2] | (uint64_t) head[3] << 32;
		uint32_t changed = head[1] >> 8;
		if (changed & RENDER_OAM) {
			memcpy(copy->oam, data, SIZE_OAM);
			data += SIZE_OAM;
		}
		if (changed & RENDER_PALETTE) {
			memcpy(copy->palette, data, SIZE_PALETTE);
			data += SIZE_PALETTE;
		}
		for (int page = 0; page < VRAM_PAGES; ++page) {
			if (pages & (1ull << page)) {
				memcpy(&copy->vram[page * VRAM_PAGE], data, VRAM_PAGE);
				copy->renderer.vramChanged[page] = 1;
				data += VRAM_PAGE;
			}
		}
		copy->renderer.memoryChanged |= changed;
		renderScanline(copy, head[1] & 0xFF);
		*size = (data - entry + 3) & ~3u;
		break;
	}
	}
	return head[0];
}

// The frame's lines a worker draws, from its copy to where the frame is taken from.
static void takeLines(const struct Worker* w, uint16_t (*out)[SCREEN_WIDTH]) {
	const struct Renderer* renderer = &w->copy->renderer;
	if (renderer->copy <= 1) {
		memcpy(out, renderer->frame, sizeof(renderer->frame));
		return;
	}
	for (int y = renderer->copy & 1; y < SCREEN_HEIGHT; y += 2) {
		memcpy(out[y], renderer->frame[y], sizeof(renderer->frame[y]));
	}
}

// A worker: takes entries off the log as they are written and makes the renderer calls they
// describe on its own copy. With nothing to do, it sleeps until woken.
static void* work(void* context) {
	struct Worker* w = context;
	struct RenderThread* t = w->owner;
	for (;;) {
		uint64_t done = atomic_load(&w->done);
		if (done == atomic_load(&t->written)) {
			if (atomic_load(&t->awaited)) {
				pthread_mutex_lock(&t->mutex);
				pthread_cond_signal(&t->idle);
				pthread_mutex_unlock(&t->mutex);
			}
			if (!spin(t, w, 0)) {
				pthread_mutex_lock(&t->mutex);
				atomic_fetch_add(&t->asleep, 1);
				while (done == atomic_load(&t->written) && !t->quit) {
					pthread_cond_wait(&t->wake, &t->mutex);
				}
				atomic_fetch_sub(&t->asleep, 1);
				bool quit = t->quit;
				pthread_mutex_unlock(&t->mutex);
				if (quit) {
					return NULL;
				}
			}
			continue;
		}
		uint32_t size;
#if defined(__3DS__)
		uint64_t const began = HOST_TICKS();
#endif
		bool finish = play(w->copy, &t->log[done & (LOG_SIZE - 1)], &size) == LOG_FINISH;
#if defined(__3DS__)
		// One worker's time: with two, each draws half the lines.
		if (!w->index) {
			renderWorkTicks += HOST_TICKS() - began;
		}
#endif
		if (finish && t->late) {
			takeLines(w, t->finished);
		}
		atomic_store(&w->done, done + size);
		if (finish && atomic_load(&t->awaited)) {
			pthread_mutex_lock(&t->mutex);
			pthread_cond_signal(&t->idle);
			pthread_mutex_unlock(&t->mutex);
		}
	}
}

// Waits until the worker has done the log up to target: the end of a frame, or everything logged.
static void await(struct RenderThread* t, uint64_t target) {
#if defined(__3DS__)
	uint64_t const began = HOST_TICKS();
	if (spin(t, NULL, target)) {
		renderWaitTicks += HOST_TICKS() - began;
		return;
	}
#else
	if (spin(t, NULL, target)) {
		return;
	}
#endif
	pthread_mutex_lock(&t->mutex);
	atomic_store(&t->awaited, true);
	while (doneByAll(t) < target) {
		pthread_cond_wait(&t->idle, &t->mutex);
	}
	atomic_store(&t->awaited, false);
	pthread_mutex_unlock(&t->mutex);
#if defined(__3DS__)
	renderWaitTicks += HOST_TICKS() - began;
#endif
}

// Waits until the worker has done everything written so far.
static void drain(struct RenderThread* t) {
	await(t, atomic_load(&t->written));
}

// Makes an entry of size bytes visible to the worker, and wakes it if it sleeps.
static void publish(struct RenderThread* t, uint32_t size) {
	atomic_store(&t->written, atomic_load(&t->written) + size);
	if (atomic_load(&t->asleep)) {
		pthread_mutex_lock(&t->mutex);
		pthread_cond_broadcast(&t->wake);
		pthread_mutex_unlock(&t->mutex);
	}
}

// Finds room for the biggest entry, in one piece and clear of what the worker has yet to do.
static uint8_t* reserve(struct RenderThread* t) {
	uint64_t written = atomic_load(&t->written);
	uint32_t at = written & (LOG_SIZE - 1);
	if (at + LOG_MOST > LOG_SIZE) {
		uint32_t head[4] = { LOG_SKIP, LOG_SIZE - at, 0, 0 };
		memcpy(&t->log[at], head, sizeof(head));
		publish(t, LOG_SIZE - at);
		written += LOG_SIZE - at;
		at = 0;
	}
	if (written + LOG_MOST - doneByAll(t) > LOG_SIZE) {
		drain(t);
	}
	return &t->log[at];
}

// Logs an entry that is a call with up to three arguments.
static void entry(struct GBA* gba, uint32_t kind, uint32_t a, uint32_t b, uint32_t c) {
	struct RenderThread* t = gba->renderThread;
	uint32_t head[4] = { kind, a, b, c };
	memcpy(reserve(t), head, sizeof(head));
	publish(t, sizeof(head));
}

// The calls the emulation thread logs instead of making: a register write, a line start, a latch, a
// line to draw, and the end of a frame.
void renderThreadRegister(struct GBA* gba, uint32_t address, uint16_t value) {
	entry(gba, LOG_REGISTER, address, value, timingCurrentTime(gba) - gba->video.lineStart);
}

void renderThreadStartLine(struct GBA* gba, int y) {
	entry(gba, LOG_START_LINE, y, 0, 0);
}

void renderThreadLatch(struct GBA* gba) {
	entry(gba, LOG_LATCH, 0, 0, 0);
}

void renderThreadScanline(struct GBA* gba, int y) {
	struct RenderThread* t = gba->renderThread;
	struct Renderer* renderer = &gba->renderer;
	uint8_t* start = reserve(t);
	uint8_t* data = start + 16;
	uint32_t changed = renderer->memoryChanged;
	renderer->memoryChanged = 0;
	if (changed & RENDER_OAM) {
		memcpy(data, gba->oam, SIZE_OAM);
		data += SIZE_OAM;
	}
	if (changed & RENDER_PALETTE) {
		memcpy(data, gba->palette, SIZE_PALETTE);
		data += SIZE_PALETTE;
	}
	uint64_t pages = 0;
	for (int page = 0; page < VRAM_PAGES; ++page) {
		if (renderer->vramChanged[page]) {
			renderer->vramChanged[page] = 0;
			pages |= 1ull << page;
			memcpy(data, &gba->vram[page * VRAM_PAGE], VRAM_PAGE);
			data += VRAM_PAGE;
		}
	}
	uint32_t head[4] = { LOG_LINE, y | changed << 8, (uint32_t) pages, (uint32_t) (pages >> 32) };
	memcpy(start, head, sizeof(head));
	// Entries start on word boundaries.
	publish(t, (data - start + 3) & ~3u);
}

void renderThreadFinishFrame(struct GBA* gba) {
	struct RenderThread* t = gba->renderThread;
	if (!t->late) {
		entry(gba, LOG_FINISH, 0, 0, 0);
		drain(t);
		for (int i = 0; i < t->count; ++i) {
			takeLines(&t->workers[i], gba->renderer.frame);
		}
		return;
	}
	// Takes the frame before this one, before the worker can finish another over it.
	if (t->finishLogged) {
		await(t, t->finishLogged);
		memcpy(gba->renderer.frame, t->finished, sizeof(gba->renderer.frame));
	}
	entry(gba, LOG_FINISH, 0, 0, 0);
	t->finishLogged = atomic_load(&t->written);
}

// The copy takes on the renderer and its memory as they are, with nothing logged outstanding.
void renderThreadClone(struct GBA* gba) {
	struct RenderThread* t = gba->renderThread;
	drain(t);
	for (int i = 0; i < t->count; ++i) {
		struct GBA* copy = t->workers[i].copy;
		memcpy(&copy->renderer, &gba->renderer, sizeof(copy->renderer));
		memcpy(copy->vram, gba->vram, sizeof(copy->vram));
		memcpy(copy->oam, gba->oam, sizeof(copy->oam));
		memcpy(copy->palette, gba->palette, sizeof(copy->palette));
		copy->renderer.copy = t->count > 1 ? 2 + i : 1;
		if (t->count > 1) {
			// The lines the other copy draws are never brought up to date in this one.
			memset(copy->renderer.scanlineDirty, 0xFF, sizeof(copy->renderer.scanlineDirty));
		}
	}
	// When late, the frame handed over next is the one this renderer last finished.
	memcpy(t->finished, gba->renderer.frame, sizeof(t->finished));
}

// The worker's renderer, for the frontend to take the frame from.
const struct Renderer* renderThreadRenderer(struct GBA* gba) {
	struct RenderThread* t = gba->renderThread;
	drain(t);
	if (t->count == 1) {
		return &t->workers[0].copy->renderer;
	}
	// One copy's state with both copies' lines; every line is drawn again by whoever carries on
	// from it, since each copy kept only its own up to date.
	memcpy(t->merged, &t->workers[0].copy->renderer, sizeof(*t->merged));
	takeLines(&t->workers[1], t->merged->frame);
	memset(t->merged->scanlineDirty, 0xFF, sizeof(t->merged->scanlineDirty));
	return t->merged;
}

// Starts the worker with a copy of the renderer and of video memory as they are now. With late,
// each frame is handed over one frame after it was made, so the emulation thread never waits.
bool renderThreadStart(struct GBA* gba, bool late) {
	if (gba->renderThread) {
		return true;
	}
	struct RenderThread* t = calloc(1, sizeof(*t));
	if (t) {
		t->workers[0].copy = calloc(1, sizeof(struct GBA));
		t->log = malloc(LOG_SIZE);
	}
	if (!t || !t->workers[0].copy || !t->log) {
		if (t) {
			free(t->workers[0].copy);
			free(t->log);
		}
		free(t);
		return false;
	}
	pthread_mutex_init(&t->mutex, NULL);
	pthread_cond_init(&t->wake, NULL);
	pthread_cond_init(&t->idle, NULL);
	t->late = late;
	t->count = 1;
	for (int i = 0; i < 2; ++i) {
		t->workers[i].owner = t;
		t->workers[i].index = i;
	}
#if defined(__3DS__)
	// libctru's pthreads run on the game's own core at the lowest priority, where a renderer can't
	// help: the frontend puts it on a core of its own (the New 3DS's third), or says it can't.
	if (!renderThreadHostCreate(&t->workers[0].thread, work, &t->workers[0])) {
#else
	if (pthread_create(&t->workers[0].thread, NULL, work, &t->workers[0])) {
#endif
		free(t->workers[0].copy);
		free(t->log);
		free(t);
		return false;
	}
	// A second worker where there is a core for it. Nothing is logged yet, so the workers wait.
#if defined(__3DS__)
	bool second = true;
#else
	bool second = getenv("ARMGBA_RENDER_WORKERS") && atoi(getenv("ARMGBA_RENDER_WORKERS")) == 2;
#endif
	if (second) {
		t->workers[1].copy = calloc(1, sizeof(struct GBA));
		t->merged = malloc(sizeof(*t->merged));
		if (t->workers[1].copy && t->merged &&
#if defined(__3DS__)
		    renderThreadHostCreateSecond(&t->workers[1].thread, work, &t->workers[1])) {
#else
		    !pthread_create(&t->workers[1].thread, NULL, work, &t->workers[1])) {
#endif
			t->count = 2;
		} else {
			free(t->workers[1].copy);
			free(t->merged);
			t->workers[1].copy = NULL;
			t->merged = NULL;
		}
	}
	gba->renderThread = t;
	renderThreadClone(gba);
	return true;
}

// Lets the workers finish what is logged, stops them, and gives drawing back to the emulation
// thread.
void renderThreadStop(struct GBA* gba) {
	struct RenderThread* t = gba->renderThread;
	if (!t) {
		return;
	}
	// The copies have drawn lines this renderer hasn't, so this renderer takes over their state to
	// carry on alone, keeping its own record of changes not yet logged.
	const struct Renderer* drawn = renderThreadRenderer(gba);
	pthread_mutex_lock(&t->mutex);
	t->quit = true;
	pthread_cond_broadcast(&t->wake);
	pthread_mutex_unlock(&t->mutex);
	for (int i = 0; i < t->count; ++i) {
		pthread_join(t->workers[i].thread, NULL);
	}
	struct Renderer* renderer = &gba->renderer;
	uint8_t memoryChanged = renderer->memoryChanged;
	uint8_t vramChanged[VRAM_PAGES];
	memcpy(vramChanged, renderer->vramChanged, sizeof(vramChanged));
	memcpy(renderer, drawn, sizeof(*renderer));
	renderer->memoryChanged = memoryChanged;
	memcpy(renderer->vramChanged, vramChanged, sizeof(vramChanged));
	renderer->copy = false;
	gba->renderThread = NULL;
	free(t->workers[0].copy);
	free(t->workers[1].copy);
	free(t->merged);
	free(t->log);
	free(t);
}
