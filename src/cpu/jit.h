/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_JIT_H
#define ARMGBA_JIT_H

#include "gba.h"

// The recompiler as the rest of the emulator sees it.

bool jitInit(struct GBA*);
void jitDeinit(struct GBA*);

// Runs a frame: compiled code, with the exact CPU around events.
void jitRunLoop(struct GBA*);

// Called when guest memory changed under compiled code.
void jitInvalidate(struct GBA*, uint32_t address, uint32_t size);
// Called after a store to IWRAM or EWRAM that the code maps flag.
void jitCodeWritten(struct GBA*, uint32_t address, int size);
bool jitCovers(struct GBA*, uint32_t address);
// Brings r15 and the prefetch up to date when a slow memory access from compiled code needs them.
void jitSyncPc(struct GBA*);
// Returns the cycles compiled code adds to a data access because its next fetch is nonsequential,
// or -1 where it doesn't.
int32_t jitFetchGap(struct GBA*);
// gap: jitFetchGap from before the change.
void jitWaitstatesChanged(struct GBA*, int32_t gap);
void jitFlush(struct GBA*);
void jitRomChanged(struct GBA*);
void jitWorkRamWaitsChanged(struct GBA*);
// Forgets which code the game rewrites, and starts learning again.
void jitForgetRewrites(struct GBA*);
void jitStateLoaded(struct GBA*, bool codeChanged);
// While gba->idleProbing, receives the exact CPU's data reads and writes, before a write lands.
void jitIdleRead(struct GBA*, uint32_t address, int size, uint32_t value);
void jitIdleWrite(struct GBA*, uint32_t address, uint32_t value, int size);


// The warm list: which cartridge code a game ran compiled, for a frontend to keep between runs so
// the next run compiles it in time it has to spare, before the game gets there, instead of when
// it does. Entries are pairs of words, a block's key and the wait states it was compiled for.
// Takes up to max pairs: what was compiled this run and what was given and not yet used.
size_t jitWarmTake(struct GBA*, uint32_t* pairs, size_t max);
// Gives a list from an earlier run of the same cartridge. Any list will do: an entry is only ever
// a place in the cartridge to compile from, and code compiled from a place the game never reaches
// never runs.
void jitWarmGive(struct GBA*, const uint32_t* pairs, size_t count);
// Compiles the next block of the list that fits the wait states as they are; false when there is
// none to compile now. Called between frames.
bool jitWarmStep(struct GBA*);

#endif
