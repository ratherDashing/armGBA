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
	LOG_SKIP_LINE,
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
	// Signalled when there is something logged. Each worker has its own: libctru's condition
	// variable keeps one "signalled" mark, which a second thread starting to wait resets, so with
	// both workers waiting on one, a wake sent between the first's unlock and its sleep was lost.
	// That worker then slept on with the log unread, and the frame's end waited for it for good.
	pthread_cond_t wake;
};

struct RenderThread {
	pthread_mutex_t mutex;
	pthread_cond_t idle;
	struct Worker workers[2];
	int count;
	// A ring: positions only grow, and an entry is at its position modulo LOG_SIZE.
	uint8_t* log;
	// How much is logged, as far as the workers know. The emulation thread's end of the log is head:
	// entries that only set up a line (register writes, line starts, latches) wait there to be made
	// visible with the next entry that is published, since a barrier for each of them cost more than
	// the write itself on ARM.
	atomic_ullong written;
	uint64_t head;
	// The emulation thread's: up to where the log is known to be clear of what the workers have yet
	// to do.
	uint64_t roomUntil;
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
	// Sharing a frame's lines three ways (see "Shared lines" below). Each line's word says who has
	// it in which frame; lastOwner is the copy whose frame holds the line as last drawn.
	bool share;
	atomic_uint lineState[SCREEN_HEIGHT];
	uint8_t lastOwner[SCREEN_HEIGHT];
	uint32_t claimedAt[SCREEN_HEIGHT];
	// The emulation thread's: the frame being logged, the lines logged in it, and its own copy,
	// which it brings up to date and draws with where each frame ends.
	uint32_t frameSeq;
	uint32_t logged[(SCREEN_HEIGHT + 31) / 32];
	struct GBA* helper;
	uint64_t helperDone;
	bool helperActive;
	int idleFrames;
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

// How far every copy that still needs the log has got: the workers, and the emulation thread's own
// copy while it is kept up to date.
static uint64_t logNeededFrom(struct RenderThread* t) {
	uint64_t done = doneByAll(t);
	return t->helperActive && t->helperDone < done ? t->helperDone : done;
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

// Shared lines. With two workers, the emulation thread used to wait at each frame's end for both to
// have drawn their lines: in Mario Kart's races 2 to 6 ms of every frame, and 20 to 30 ms whenever
// the system took a worker's core away for that long, which was heard (New 3DS, 2026-11-12). Now it
// draws too. It has a third copy, which it takes through the frame's log where the frame ends,
// drawing every line no worker has started; a line a worker has held for longer than a line takes
// it draws itself and takes over. The workers keep to their even and odd lines.
//
// A line's word holds the frame's number, CLAIMED or DONE, and who by (1 and 2: the workers, 3: the
// emulation thread's copy); words of an earlier frame count as free, and a copy still in an earlier
// frame than the word leaves the line alone, so a worker that fell behind can't draw into a later
// frame. Each copy redraws a line it didn't draw the last time, whether or not it changed.
enum {
	LINE_CLAIMED = 1,
	LINE_DONE = 2,
	HELPER = 3,
	// Microseconds a worker may hold a line before the emulation thread draws it itself, and that
	// the emulation thread then waits for lines it passed while they were being drawn before it
	// takes them as they were last frame.
	STALL_MICROS = 300,
	RESCUE_MICROS = 2000,
};
#define LINE_WORD(seq, status, id) (((uint32_t) (seq) & 0x0FFFFFFF) << 4 | (uint32_t) (status) << 2 | (uint32_t) (id))

static uint32_t micros(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint32_t) (now.tv_sec * 1000000u + now.tv_nsec / 1000);
}

// The copies that share lines: which thread's, which of its copies, and the frame it is in.
struct Sharer {
	_Atomic(struct GBA*) copy;
	struct RenderThread* t;
	uint32_t seq;
	int id;
};
// For the frontend's log and for tests: lines the emulation thread's copy drew, lines it took over
// from a worker, lines taken as they were last frame, and times its copy was made anew.
unsigned long renderShareCounts[4];
#if !defined(__3DS__)
// ARMGBA_RENDER_STALL=n: a worker stops for 2 ms holding about one line in n, to test taking over.
static int stallEvery = -1;
#endif
static struct Sharer sharers[12];
static atomic_int sharerCount;

static struct Sharer* sharerOf(const struct GBA* copy) {
	if (!atomic_load(&sharerCount)) {
		return NULL;
	}
	for (int i = 0; i < 12; ++i) {
		if (atomic_load(&sharers[i].copy) == copy) {
			return &sharers[i];
		}
	}
	return NULL;
}

static void sharerSet(struct GBA* copy, struct RenderThread* t, int id, uint32_t seq) {
	struct Sharer* s = sharerOf(copy);
	for (int i = 0; !s && i < 12; ++i) {
		if (!atomic_load(&sharers[i].copy)) {
			s = &sharers[i];
		}
	}
	if (s) {
		s->t = t;
		s->id = id;
		s->seq = seq;
		if (atomic_load(&s->copy) != copy) {
			atomic_store(&s->copy, copy);
			atomic_fetch_add(&sharerCount, 1);
		}
	}
}

static void sharerDrop(struct GBA* copy) {
	struct Sharer* s = sharerOf(copy);
	if (s) {
		atomic_store(&s->copy, NULL);
		atomic_fetch_sub(&sharerCount, 1);
	}
}

// For the renderer, where a copy comes to draw a line: -1 when lines aren't shared (the copy keeps
// to its own half), 0 when the line is another copy's, 1 when it is this copy's, 2 when it is and
// this copy's frame doesn't hold it as last drawn.
int renderThreadClaim(struct GBA* copy, int y) {
	struct Sharer* s = sharerOf(copy);
	if (!s) {
		return -1;
	}
	struct RenderThread* t = s->t;
	if (s->id != HELPER && (y & 1) != s->id - 1) {
		return 0;
	}
	uint32_t const mine = LINE_WORD(s->seq, LINE_CLAIMED, s->id);
	uint32_t cur = atomic_load(&t->lineState[y]);
	if (cur) {
		uint32_t const ahead = ((cur >> 4) - s->seq) & 0x0FFFFFFF;
		if (ahead && ahead < 0x08000000) {
			return 0;
		}
		if (!ahead) {
			// Taken in this frame. The emulation thread's copy takes over a line held too long.
			if (s->id != HELPER || ((cur >> 2) & 3) != LINE_CLAIMED || (uint32_t) (micros() - t->claimedAt[y]) < STALL_MICROS) {
				return 0;
			}
		}
	}
	if (!atomic_compare_exchange_strong(&t->lineState[y], &cur, mine)) {
		return 0;
	}
	t->claimedAt[y] = micros();
	if (s->id == HELPER) {
		++renderShareCounts[cur && !(((cur >> 4) - s->seq) & 0x0FFFFFFF) ? 1 : 0];
	}
#if !defined(__3DS__)
	if (stallEvery < 0) {
		const char* v = getenv("ARMGBA_RENDER_STALL");
		stallEvery = v ? atoi(v) : 0;
	}
	if (stallEvery && s->id != HELPER && (micros() * 2654435761u >> 8) % (unsigned) stallEvery == 0) {
		struct timespec nap = { 0, 2000000 };
		nanosleep(&nap, NULL);
	}
#endif
	return t->lastOwner[y] == s->id ? 1 : 2;
}

// The line is in the copy's frame, unless another copy took it over meanwhile.
void renderThreadLineDone(struct GBA* copy, int y) {
	struct Sharer* s = sharerOf(copy);
	if (!s) {
		return;
	}
	// Who drew it is in the word (see settleOwners).
	uint32_t mine = LINE_WORD(s->seq, LINE_CLAIMED, s->id);
	atomic_compare_exchange_strong(&s->t->lineState[y], &mine, LINE_WORD(s->seq, LINE_DONE, s->id));
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
	case LOG_FINISH: {
		renderFinishFrame(copy);
		struct Sharer* s = sharerOf(copy);
		if (s) {
			++s->seq;
		}
		break;
	}
	case LOG_SKIP:
		*size = head[1];
		break;
	case LOG_LINE:
	case LOG_SKIP_LINE: {
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
		if (head[0] == LOG_LINE) {
			renderScanline(copy, head[1] & 0xFF);
		} else {
			renderSkipScanline(copy, head[1] & 0xFF);
		}
		*size = (data - entry + 3) & ~3u;
		break;
	}
	}
	return head[0];
}

// The frame's lines a worker draws, from its copy to where the frame is taken from.
static void takeLines(const struct Worker* w, uint16_t (*out)[SCREEN_WIDTH]) {
	const struct Renderer* renderer = &w->copy->renderer;
	struct RenderThread* t = w->owner;
	if (t->share) {
		// Each line from the copy that drew it last; the emulation thread's copy's on its turn.
		for (int y = 0; y < SCREEN_HEIGHT; ++y) {
			int const owner = t->lastOwner[y] ? t->lastOwner[y] : 1 + (y & 1);
			if (owner == HELPER ? w->index == 0 : owner - 1 == w->index) {
				const struct GBA* from = owner == HELPER ? t->helper : w->copy;
				memcpy(out[y], from->renderer.frame[y], sizeof(renderer->frame[y]));
			}
		}
		return;
	}
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
					pthread_cond_wait(&w->wake, &t->mutex);
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

// Makes everything logged so far, with an entry of size bytes, visible to the worker, and wakes it
// if it sleeps.
static void publish(struct RenderThread* t, uint32_t size) {
	t->head += size;
	atomic_store(&t->written, t->head);
	if (atomic_load(&t->asleep)) {
		pthread_mutex_lock(&t->mutex);
		pthread_cond_signal(&t->workers[0].wake);
		pthread_cond_signal(&t->workers[1].wake);
		pthread_mutex_unlock(&t->mutex);
	}
}

// Waits until the worker has done everything logged so far.
static void drain(struct RenderThread* t) {
	if (atomic_load_explicit(&t->written, memory_order_relaxed) != t->head) {
		publish(t, 0);
	}
	await(t, t->head);
}

static void finishShared(struct GBA* gba);
static void shareBegin(struct GBA* gba);

// Finds room for the biggest entry, in one piece and clear of what the worker has yet to do.
static uint8_t* reserve(struct RenderThread* t) {
	uint64_t written = t->head;
	uint32_t at = written & (LOG_SIZE - 1);
	if (at + LOG_MOST > LOG_SIZE) {
		uint32_t head[4] = { LOG_SKIP, LOG_SIZE - at, 0, 0 };
		memcpy(&t->log[at], head, sizeof(head));
		publish(t, LOG_SIZE - at);
		written += LOG_SIZE - at;
		at = 0;
	}
	// The workers only get further, so room found once is still there: they are asked how far they
	// are only when the log has grown past what was known to be free.
	if (written + LOG_MOST > t->roomUntil) {
		if (written + LOG_MOST - logNeededFrom(t) > LOG_SIZE) {
			// The emulation thread's copy gives up its place in the log; it starts again from a
			// worker's copy where a frame ends.
			t->helperActive = false;
			drain(t);
		}
		t->roomUntil = logNeededFrom(t) + LOG_SIZE;
	}
	return &t->log[at];
}

// Logs an entry that is a call with up to three arguments. One that only sets up a line waits to be
// published with the line, or whatever comes next.
static void entry(struct GBA* gba, uint32_t kind, uint32_t a, uint32_t b, uint32_t c) {
	struct RenderThread* t = gba->renderThread;
	uint32_t head[4] = { kind, a, b, c };
	memcpy(reserve(t), head, sizeof(head));
	// A sleeping worker is woken by whatever comes first in a frame, as before these entries waited for
	// their line: woken only by the first line, it started on a frame's lines a wake-up late, and the
	// emulation thread waited that much longer where the frame ends (3DS, one worker, 2026-11-24).
	if (kind == LOG_FINISH || atomic_load_explicit(&t->asleep, memory_order_relaxed)) {
		publish(t, sizeof(head));
	} else {
		t->head += sizeof(head);
	}
}

// The calls the emulation thread logs instead of making: a register write, a line start, a latch, a
// line to draw or one left undrawn, and the end of a frame.
void renderThreadRegister(struct GBA* gba, uint32_t address, uint16_t value) {
	entry(gba, LOG_REGISTER, address, value, timingCurrentTime(gba) - gba->video.lineStart);
}

void renderThreadStartLine(struct GBA* gba, int y) {
	entry(gba, LOG_START_LINE, y, 0, 0);
}

void renderThreadLatch(struct GBA* gba) {
	entry(gba, LOG_LATCH, 0, 0, 0);
}

// A line to draw, or one left undrawn, with the memory it reads that changed since the last line.
static void logLine(struct GBA* gba, uint32_t kind, int y) {
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
	uint32_t head[4] = { kind, y | changed << 8, (uint32_t) pages, (uint32_t) (pages >> 32) };
	if (kind == LOG_LINE) {
		t->logged[y >> 5] |= 1u << (y & 31);
	}
	memcpy(start, head, sizeof(head));
	// Entries start on word boundaries.
	publish(t, (data - start + 3) & ~3u);
}

void renderThreadScanline(struct GBA* gba, int y) {
	logLine(gba, LOG_LINE, y);
}

void renderThreadSkipScanline(struct GBA* gba, int y) {
	logLine(gba, LOG_SKIP_LINE, y);
}

// Whether every line logged in the frame is drawn.
static bool linesDone(struct RenderThread* t) {
	for (int y = 0; y < SCREEN_HEIGHT; ++y) {
		if ((t->logged[y >> 5] >> (y & 31) & 1) && atomic_load(&t->lineState[y]) >> 2 != (LINE_WORD(t->frameSeq, LINE_DONE, 0) >> 2)) {
			return false;
		}
	}
	return true;
}

// The emulation thread's copy becomes a worker's as it is with the whole log done: where a frame
// ends, with the workers waited for.
static void helperClone(struct RenderThread* t) {
	drain(t);
	const struct GBA* from = t->workers[0].copy;
	memcpy(&t->helper->renderer, &from->renderer, sizeof(from->renderer));
	memcpy(t->helper->vram, from->vram, sizeof(from->vram));
	memcpy(t->helper->oam, from->oam, sizeof(from->oam));
	memcpy(t->helper->palette, from->palette, sizeof(from->palette));
	t->helper->renderer.copy = 4;
	// Its frame holds none of the lines as last drawn, and it draws whatever line it takes.
	memset(t->helper->renderer.scanlineDirty, 0xFF, sizeof(t->helper->renderer.scanlineDirty));
	for (int y = 0; y < SCREEN_HEIGHT; ++y) {
		if (t->lastOwner[y] == HELPER) {
			t->lastOwner[y] = 0;
		}
	}
	sharerSet(t->helper, t, HELPER, t->frameSeq);
	t->helperDone = t->head;
	t->helperActive = true;
	++renderShareCounts[3];
	t->idleFrames = 0;
	t->roomUntil = 0;
}

// Lines are shared from here on, if they can be: two workers, each frame waited for, a copy for the
// emulation thread, and no frontend hooks (whose second picture has no part in this). Called with
// the workers waited for.
static void shareBegin(struct GBA* gba) {
	struct RenderThread* t = gba->renderThread;
	if (t->share || t->count < 2 || t->late || renderHooksActive() || getenv("ARMGBA_RENDER_NO_HELP")) {
		return;
	}
	if (!t->helper) {
		t->helper = calloc(1, sizeof(struct GBA));
		if (!t->helper) {
			return;
		}
	}
	memset(t->lastOwner, 0, sizeof(t->lastOwner));
	memset(t->logged, 0, sizeof(t->logged));
	++t->frameSeq;
	for (int i = 0; i < 2; ++i) {
		sharerSet(t->workers[i].copy, t, 1 + i, t->frameSeq);
	}
	t->share = true;
	helperClone(t);
}

// Back to each worker drawing its half and being waited for. Called with the workers waited for.
static void shareEnd(struct RenderThread* t) {
	if (!t->share) {
		return;
	}
	t->share = false;
	t->helperActive = false;
	for (int i = 0; i < 2; ++i) {
		sharerDrop(t->workers[i].copy);
		// The lines the other copy draws are never brought up to date in this one, and its own
		// may be in another copy's frame.
		memset(t->workers[i].copy->renderer.scanlineDirty, 0xFF, sizeof(t->workers[i].copy->renderer.scanlineDirty));
	}
	if (t->helper) {
		sharerDrop(t->helper);
	}
}

// Each line drawn in the frame so far is in the frame of the copy whose word says it drew it (the
// emulation thread sets lastOwner, never a worker: a worker's store could land after the emulation
// thread had read it). A line taken as it was last frame (no one's) stays where it was.
static void settleOwners(struct RenderThread* t) {
	for (int y = 0; y < SCREEN_HEIGHT; ++y) {
		uint32_t const cur = atomic_load(&t->lineState[y]);
		if ((t->logged[y >> 5] >> (y & 31) & 1) && cur >> 2 == LINE_WORD(t->frameSeq, LINE_DONE, 0) >> 2 && (cur & 3)) {
			t->lastOwner[y] = (uint8_t) (cur & 3);
		}
	}
}

// A frame's end with shared lines: the emulation thread's copy goes through the frame's log and
// draws what no worker has; then the frame is every line from whoever drew it.
static void finishShared(struct GBA* gba) {
	struct RenderThread* t = gba->renderThread;
	// The workers have it all drawn already (lines logged through the frame, not in a burst at its
	// end): nothing to help with. After half a second of that the copy is no longer kept up to
	// date, which costs a little every frame; it starts again from a worker's when it is wanted.
	bool const wanted = !linesDone(t);
	entry(gba, LOG_FINISH, 0, 0, 0);
#if defined(__3DS__)
	uint64_t const began = HOST_TICKS();
#endif
	if (t->helperActive) {
		t->idleFrames = wanted ? 0 : t->idleFrames + 1;
		if (t->idleFrames > 30) {
			t->helperActive = false;
		} else {
			uint64_t const end = t->head;
			while (t->helperDone < end) {
				uint32_t size;
				play(t->helper, &t->log[t->helperDone & (LOG_SIZE - 1)], &size);
				t->helperDone += size;
			}
		}
	}
	// Lines the workers still hold. Ones held too long when the emulation thread's copy came by
	// are drawn already; one whose worker stopped just after is waited for a little, then taken as
	// it was in the last frame (where the copy is up to date; without it, the workers are waited
	// for as before).
	bool waited = false;
	uint32_t const from = micros();
	for (int i = 0; !linesDone(t); ++i) {
		waited = true;
#if defined(__3DS__)
		if (t->helperActive && !(i & 0x3F) && (uint32_t) (micros() - from) > RESCUE_MICROS) {
			for (int y = 0; y < SCREEN_HEIGHT; ++y) {
				uint32_t cur = atomic_load(&t->lineState[y]);
				if ((t->logged[y >> 5] >> (y & 31) & 1) && cur >> 2 == LINE_WORD(t->frameSeq, LINE_CLAIMED, 0) >> 2) {
					renderShareCounts[2] += atomic_compare_exchange_strong(&t->lineState[y], &cur, LINE_WORD(t->frameSeq, LINE_DONE, 0));
				}
			}
		}
#else
		(void) from;
#endif
#if defined(__x86_64__)
		__builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
		__asm__ volatile("yield");
#endif
	}
#if defined(__3DS__)
	renderWaitTicks += HOST_TICKS() - began;
#endif
	settleOwners(t);
	for (int i = 0; i < t->count; ++i) {
		takeLines(&t->workers[i], gba->renderer.frame);
	}
	memset(t->logged, 0, sizeof(t->logged));
	++t->frameSeq;
	if (renderHooksActive()) {
		drain(t);
		shareEnd(t);
	} else if (!t->helperActive && waited) {
		helperClone(t);
	}
}

void renderThreadFinishFrame(struct GBA* gba) {
	struct RenderThread* t = gba->renderThread;
	if (!t->late) {
		if (t->share) {
			finishShared(gba);
			return;
		}
		entry(gba, LOG_FINISH, 0, 0, 0);
		drain(t);
		for (int i = 0; i < t->count; ++i) {
			takeLines(&t->workers[i], gba->renderer.frame);
		}
		renderHooksPublish(gba);
		shareBegin(gba);
		return;
	}
	// Takes the frame before this one, before the worker can finish another over it.
	if (t->finishLogged) {
		await(t, t->finishLogged);
		memcpy(gba->renderer.frame, t->finished, sizeof(gba->renderer.frame));
		renderHooksPublish(gba);
	}
	entry(gba, LOG_FINISH, 0, 0, 0);
	t->finishLogged = t->head;
}

int renderThreadCopies(struct GBA* gba, struct GBA* copies[2]) {
	struct RenderThread* t = gba->renderThread;
	if (!t) {
		return 0;
	}
	for (int i = 0; i < t->count; ++i) {
		copies[i] = t->workers[i].copy;
	}
	return t->count;
}

// The copy takes on the renderer and its memory as they are, with nothing logged outstanding.
void renderThreadClone(struct GBA* gba) {
	struct RenderThread* t = gba->renderThread;
	drain(t);
	shareEnd(t);
	memset(t->logged, 0, sizeof(t->logged));
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
		renderHooksResync(copy);
	}
	// When late, the frame handed over next is the one this renderer last finished.
	memcpy(t->finished, gba->renderer.frame, sizeof(t->finished));
	shareBegin(gba);
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
	if (t->share) {
		settleOwners(t);
		takeLines(&t->workers[0], t->merged->frame);
	}
	takeLines(&t->workers[1], t->merged->frame);
	// What a copy keeps from the lines it drew depends on which lines it happened to draw.
	renderForget(t->merged);
	return t->merged;
}

// Starts the worker with a copy of the renderer and of video memory as they are now. With late,
// each frame is handed over one frame after it was made, so the emulation thread never waits.
bool renderThreadStart(struct GBA* gba, bool late, bool second) {
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
	pthread_cond_init(&t->workers[0].wake, NULL);
	pthread_cond_init(&t->workers[1].wake, NULL);
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
	second = true;
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
	shareEnd(t);
	pthread_mutex_lock(&t->mutex);
	t->quit = true;
	pthread_cond_signal(&t->workers[0].wake);
	pthread_cond_signal(&t->workers[1].wake);
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
	free(t->helper);
	free(t->merged);
	free(t->log);
	free(t);
}
