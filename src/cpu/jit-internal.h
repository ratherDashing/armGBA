/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_JIT_INTERNAL_H
#define ARMGBA_JIT_INTERNAL_H

#include "decode.h"
#include "jit.h"

// The recompiler's own structures (blocks, links, code maps, idle loops) and what jit.c and the
// backends share.

enum {
	MAX_BLOCK_INSTS = 64,
	// The lookup entries a careful copy can have in the middle of it, and the bit that sets their keys
	// apart from the keys of the same instructions (jit.c: carefulEntry)
	CAREFUL_ENTRIES = 4,
	CAREFUL_KEY = 0x80000000,
	// The code memory's stretches come from it in slots of this size (jit-layout.h's STRETCH)
	JIT_SLOT = 1 << 20,
	LOOKUP_BITS = 16,
	LOOKUP_SIZE = 1 << LOOKUP_BITS,
	// The code memory: an eighth of the device's RAM (jit.c: codeSizeFor), from CODE_SIZE_MIN to
	// CODE_SIZE. A full code memory reclaims the slot least in use (jitReclaimSlot) rather than
	// throwing all the code away, so more of it only puts a reclaim off. 16 MiB is what a 128 MB
	// device (a Miyoo Mini) can spare with a game that keeps compiling filling it, its whole process
	// taking about 1.5 times the code memory and 25 MiB more. 32 MiB holds the code every released
	// game keeps in use: at most about 19 MiB on 32-bit ARM and 16 on AArch64 in a library sweep
	// (an unreleased prototype 29, x86-64's code about twice ARM's), most games 1 to 5. It is also
	// A32's reach: its branches go 32 MiB from themselves plus 8, which compiled code never needs to
	// the full, as it goes only in whole 1 MiB stretches after the shared routines (jit-layout.h), so
	// the last MiB is never filled. The encoders abort on a branch out of reach. Only what a game
	// fills is in memory. The 3DS has 64 MiB for everything on an Old 3DS, the cartridge buffer
	// taking 32 of it.
#if defined(__3DS__)
	// The most; the 3DS takes what the heap can spare (platform/3ds/source/jitmem.c).
	CODE_SIZE = 24 << 20,
	CODE_SIZE_MIN = CODE_SIZE,
#else
	CODE_SIZE = 32 << 20,
	CODE_SIZE_MIN = 16 << 20,
#endif
	MARKS_PER_MIB = 8,
	BLOCK_HASH_SIZE = 1 << 16,
	SITE_CACHE_SIZE = 1024,
	TINY_ROUTINES = 1024,
};

// How the compiled code covering a word of IWRAM or EWRAM depends on it.
enum {
	CODE_MAP_BAKED = 1,
	CODE_MAP_PATCHABLE = 2,
	CODE_MAP_TAIL = 3,
	// Compiled to check the word when it runs
	CODE_MAP_CHECKED = 4,
};

// How a kilobyte of work RAM handles words the game rewrites: compiled for each of their forms, until
// one of them goes through more forms than its FormSet keeps; from then on (plain) as they stand,
// their blocks thrown away and brought back as they change. A game that flips a few words between
// two forms as its loops run does best with the first; one that copies whole routines in and out of
// the same memory with the second.
struct PagePolicy {
	bool plain;
	// A word here dropped a form: the page turns plain when the run loop next starts
	bool dropped;
};

enum {
	POLICY_PAGES = (SIZE_IWRAM + SIZE_EWRAM) >> 10,
};

enum {
	MAX_FORMS = 4,
	FORM_SET_BITS = 11,
	FORM_SETS = 1 << FORM_SET_BITS,
};

// A word the game keeps rewriting, compiled to run whichever of the forms seen there it currently
// holds.
struct Poly {
	int count;
	struct Inst forms[MAX_FORMS];
	// The bits that tell the forms apart; a patchable load or store reads its offset when it runs,
	// so its offset doesn't count.
	uint32_t masks[MAX_FORMS];
};

// The forms seen at a word the game keeps rewriting, most recent first
struct FormSet {
	// Its index in smcWord plus one; 0 when the slot is free.
	uint32_t word;
	uint32_t ops[MAX_FORMS];
	uint8_t count;
	// Forms that had to make room for another. Past a limit, the word is compiled as it stands and
	// thrown away when it changes, like other code.
	uint8_t dropped;
};

enum ExitReason {
	// Stopped at an instruction boundary inside a block; exitPc and the prefetched opcodes are
	// valid.
	EXIT_CHECK,
	// An indirect branch target wasn't in the lookup table.
	EXIT_LOOKUP,
	// A direct branch that isn't linked yet; exitSite is the jump to patch.
	EXIT_LINK,
	// Stopped before an instruction compiled code leaves to the exact CPU.
	EXIT_STEP,
	// The exact CPU ran an instruction for a careful copy and left the state as it is.
	EXIT_EXACT,
};

// Keys combine the address with the instruction set: bit 0 is set for Thumb.
static inline uint32_t blockKey(uint32_t pc, bool thumb) {
	return pc | thumb;
}

static inline uint32_t lookupHash(uint32_t key) {
	return (key ^ (key >> 13)) & (LOOKUP_SIZE - 1);
}

struct LookupEntry {
	uint32_t key;
	uint32_t pad;
	void* entry;
};

struct Block;

struct Link {
	uint8_t* site;
	uint8_t* stub;
	uint32_t targetKey;
	// A rewritten word holds a form its block wasn't compiled for.
	bool formMiss;
	// Leaves a block's check for its careful copy, at an instruction index.
	bool careful;
	int index;
	struct Block* from;
	struct Block* to;
	struct Link* nextIncoming;
};

struct PageNode {
	struct Block* block;
	struct PageNode* next;
};

// A compiled run of guest instructions, entered at its first.
struct Block {
	// The idle span (see jit.c) this block lies in, plus one, or 0. Worked out again when the spans
	// change.
	uint8_t idleSpan;
	// Times an event fell inside it before it had a careful copy (jit.c: CAREFUL_AFTER), counted
	// from the frame in firesFrom.
	uint8_t checkFires;
	uint32_t firesFrom;
	uint32_t idleSpanGen;
	// The guest addresses it runs from and to, and its instruction set
	uint32_t pc;
	uint32_t end;
	bool thumb;
	// In the dispatcher's direct-mapped table
	bool cached;
	// Cleared once the code it was compiled from has changed; then nothing links or dispatches to
	// it.
	bool valid;
	// Whether this is a careful copy, which checks for events before each instruction and runs the
	// one an event falls in on the exact CPU. Entries are where each instruction's code starts.
	bool careful;
	// Cartridge code compiled without the prefetch buffer, which adds a nonsequential fetch's
	// cycles to the instruction before it.
	bool precharged;
	// A block's careful copy, and the copy's block
	struct Block* twin;
	uint8_t** entries;
	// Instructions in it
	int count;
	// One bit per instruction: set when it's compiled to read its operand from memory as it runs.
	uint64_t patchable;
	// Instructions compiled for each form their word has had.
	uint64_t polymorphic;
	// The memory it was compiled from, and the offsets there of its first word, the end of its
	// instructions, and the end of what it prefetched.
	int codeKind;
	uint32_t start;
	uint32_t bodyStop;
	uint32_t stop;
	// Its host code
	uint8_t* entry;
	// A cartridge block's first instruction: a no-op, or a jump to headStub, its idle loop head's
	// check, while it is one (backendIdleHead)
	uint8_t* headSite;
	uint8_t* headStub;
	// Where its cold code starts, when a backend keeps that apart from the bodies; NULL when it
	// doesn't.
	uint8_t* cold;
	// Jumps into it from other blocks, and its own jumps out
	struct Link* incoming;
	struct Link* out;
	int outCount;
	// Its calls out to C, in the order they were emitted
	struct Site* sites;
	int siteCount;
	int siteCap;
	// The hash chain, and the list of every block
	struct Block* hashNext;
	struct Block* allNext;
	struct Block* allPrev;
	// Blocks from RAM keep a copy of their code, so they can come back when that code is copied in
	// again.
	uint8_t* bytes;
	// Removed blocks from RAM wait on this list in case the same code returns.
	struct Block* dormantNext;
	// The wait states cartridge code's costs were compiled for (WAITCNT, plus bit 16 when the
	// cartridge turned the prefetch buffer off). A block set aside when they change comes back when
	// they change back.
	uint32_t waits;
	bool setAside;
	// Compiled as an idle loop's head, with the check that leaves for the run loop at its start.
	bool idleHead;
	// On the list of live cartridge blocks that a wait-state change goes through.
	bool listed;
	// The bytes of code it took, body and cold code, for choosing a slot to reclaim
	uint32_t codeBytes;
	// A careful copy's block, for as long as that block keeps it
	struct Block* parent;
	// A careful copy's: the instructions in it the lookup was given (see jit.c: carefulEntry)
	uint32_t midKeys[CAREFUL_ENTRIES];
	int midCount;
	// Going away with a slot of the code memory being reclaimed
	bool evicting;
};

// The memories code can run from. Blocks are kept per kind.
enum CodeKind {
	CODE_NONE,
	CODE_BIOS,
	CODE_EWRAM,
	CODE_IWRAM,
	CODE_PALETTE,
	CODE_VRAM,
	CODE_OAM,
	CODE_ROM,
	CODE_KINDS,
};

// Where a memory access in compiled code came from, so a slow path can rebuild the PC and the
// prefetched opcodes that open-bus reads and DMA see.
struct Site {
	// Bytes from the block's entry to the call's return address.
	uint32_t offset;
	// The instruction's address, and the two opcodes after it, which the pipeline holds.
	uint32_t pc;
	uint32_t ops[2];
};

// The last sites looked up, by return address.
struct SiteCacheEntry {
	// A return address and what was found for it
	const uint8_t* ret;
	uint32_t r15;
	uint32_t ops[2];
};

// A loop the CPU was seen going round twice the same way: from the same state back to it, in the
// same number of cycles, with the second time reading and writing just as the first. As long as
// what it reads before writing is unchanged, each time round is the same until an event comes.
enum {
	IDLE_LOOPS = 32,
	IDLE_READS = 8,
	// The instructions a probe follows looking for two times round.
	IDLE_PROBE_STEPS = 128,
	// Each time round's reads and writes of memory that can change.
	IDLE_ACCESSES = 32,
	IDLE_SPANS = 64,
	IDLE_HEADS = 8,
	// Events landing in a loop this many times in a frame make its head an idle head; a head where
	// this many probes skipped nothing stops being one.
	IDLE_NOMINATE = 16,
	IDLE_GIVE_UP = 64,
	// A waiting loop spans at most this many bytes of code.
	IDLE_SPAN_BYTES = 1024,
};

// Whether compiled code for the block with this key starts by checking for events since the run
// loop last looked at it as an idle loop's head.
bool jitIsIdleHead(const struct Jit*, uint32_t key);

// The CPU and bus state a time round starts from, with times relative to now. Where the cartridge's
// next sequential read would be is left out: CPU data accesses start nonsequential, and compiled
// code doesn't track it either.
struct IdleState {
	uint32_t r[16];
	uint32_t cpsr;
	uint32_t prefetch[2];
	int32_t prefetcherLead;
	int32_t prefetcherWidth;
	int32_t prefetcherDuty;
	int32_t prefetcherFull;
	uint32_t prefetcherHead;
	int32_t activeRegion;
	bool fetchSeq;
	bool prefetching;
	bool irqMasked;
	bool lastAccessDma;
};

struct IdleRead {
	uint32_t address;
	uint32_t value;
	int size;
};

struct IdleAccess {
	uint32_t address;
	uint32_t value;
	int size;
	bool write;
};

struct IdleLoop {
	bool valid;
	struct IdleState start;
	int32_t period;
	// What a time round reads without having written it first, and what it writes.
	int reads;
	struct IdleRead read[IDLE_READS];
	int writes;
	struct IdleRead write[IDLE_READS];
};

// An instruction the exact CPU runs over and over for compiled code, kept decoded.
struct DecodedInst {
	uint32_t opcode;
	uint32_t address;
	bool valid;
	struct Inst in;
};

struct Jit {
	struct GBA* gba;
	struct DecodedInst decoded[64];
	struct IdleLoop idleLoops[IDLE_LOOPS];
	int idleNext;
	// Loops compiled code may go round: from a block ending in a branch back to an earlier block
	// (the head) through that branch, as block keys, with how many times events ran in one of their
	// blocks.
	struct {
		uint32_t head;
		uint32_t end;
		uint32_t hits;
		uint32_t frame;
	} idleSpans[IDLE_SPANS];
	int idleSpanNext;
	// Changes whenever a span is added, so blocks know to look up their span again.
	uint32_t idleSpanGen;
	// Heads compiled to leave for the run loop once events have run since it last looked
	// (idleCheck), with the probes made at each and the times round skipped there.
	struct {
		uint32_t key;
		uint32_t probes;
		uint32_t skips;
	} idleHeads[IDLE_HEADS];
	// Heads that never skipped, which aren't made heads again until a flush.
	uint32_t idleRejected[IDLE_HEADS];
	int idleRejectedNext;

	// While probing: which time round it is (1 or 2), each one's accesses, and whether anything
	// rules the loop out.
	int idleLap;
	int idleAccesses[2];
	struct IdleAccess idleAccess[2][IDLE_ACCESSES];
	bool idleBad;
	// Arrivals by a hash of their state. A probe is made only at a state seen before, since an idle
	// loop comes round again and again; a failed probe puts off the next one at that state.
	struct {
		uint32_t hash;
		uint8_t wait;
		uint8_t fails;
	} idleSeen[256];
	// The code memory in slots of JIT_SLOT bytes from codeStart, each the stretches jit-layout.h
	// lays out in it: the unit a full code memory reclaims (jitReclaimSlot). In each, its blocks in
	// the order of their bodies, and those with cold code in its order, to find the block a return
	// address is in.
	struct CodeSlot {
		struct Block** hot;
		size_t hotCount;
		size_t hotCap;
		struct Block** cold;
		size_t coldCount;
		size_t coldCap;
		// Bytes of the code of its blocks still in use
		uint64_t live;
	}* slots;
	int slotCount;
	// The slot the stretch being filled is in, and the block whose careful copy is being compiled,
	// whose slot isn't reclaimed under it.
	int currentSlot;
	const struct Block* pinned;
	// For a slot being reclaimed: its blocks, and the pages they were listed on
	struct Block** evicted;
	int evictedCount;
	int evictedCap;
	uint32_t* evictPages;
	int evictPageCount;
	int evictPageCap;
	// Bumped whenever compiled code goes away (a flush or a reclaimed slot), so the run loop knows
	// what it held on to may be gone.
	uint64_t codeEpoch;
	uint64_t reclaims;
	// Bytes of compiled code in use, and the most there has been (ARMGBA_STATS)
	uint64_t liveBytes;
	uint64_t peakLiveBytes;
	// Sites found from their return addresses, valid until a flush reuses the code memory.
	struct SiteCacheEntry siteCache[SITE_CACHE_SIZE];
	struct FormSet formSets[FORM_SETS];
	uint8_t* code;
	uint8_t* codeEnd;
	size_t codeSize;
	uint8_t* codeStart;
	uint8_t* codeNext;
	// For a backend that keeps blocks' bodies apart from their cold code (see the AArch64 backend's
	// backendReset): the end of the bodies' part, the cold cursor and the end of its part, and the
	// blocks in the order of their cold code.
	uint8_t* hotEnd;
	uint8_t* coldNext;
	uint8_t* coldEnd;
	struct LookupEntry* lookup;
	struct Block* hash[BLOCK_HASH_SIZE];
	struct Block* dormant[BLOCK_HASH_SIZE];
	struct Block* allBlocks;
	struct PageNode** pages[CODE_KINDS];
	uint32_t pageCount[CODE_KINDS];

	// The warm list (jit.c): cartridge blocks compiled this run, as pairs of key and wait states,
	// and those a frontend handed back from an earlier run, compiled ahead of need. warmNext is
	// how far through them the current wait states have been looked for.
	uint32_t* made;
	uint32_t madeCount;
	uint32_t* warm;
	uint32_t warmCount;
	uint32_t warmNext;
	uint32_t warmWaits;

	bool flushRequested;
	// The table-driven interpreter runs instead of compiled code (jitSetInterpreter)
	bool interpret;
	// Set when the wait states changed, for the run loop to look at before compiled code runs again
	// (jit.c: awayFromWaits).
	bool waitsChanged;
	// The wait states the cartridge's blocks in use were compiled for. While they're others for a
	// short time (away, until awayUntil), the exact CPU runs instead of compiled code, and the
	// blocks stay; only a longer time sets them aside.
	uint32_t homeWaits;
	bool away;
	int32_t awayUntil;
	// The cartridge's blocks in use, which a wait-state change goes through instead of all blocks.
	struct Block** romBlocks;
	int romBlockCount;
	int romBlockCap;
	// Set when blocks were compiled from the cartridge's first 8 KiB, which a mapper can change.
	bool romWindowCode;
	// Instructions the exact CPU still has to run after compiled code left, and a time it runs
	// until.
	int steps;
	int32_t stepUntil;
	bool stepping;
	// The frame the run loop runs to the end of.
	uint32_t frame;
	// A store into the two instructions already prefetched leaves their old opcodes here.
	bool staleValid;
	uint32_t stalePc;
	uint32_t staleOps[2];

	// Short timing sequences compiled code would otherwise repeat at every memory access, emitted
	// once each and called (the AArch64 backend's; emptied with the code memory).
	struct TinyRoutine {
		uint64_t key;
		uint8_t* code;
	} tiny[TINY_ROUTINES];
	// Shared routines
	int (*enter)(struct GBA*, void* entry);
	uint8_t* exitRoutine;
	uint8_t* spillRoutine;
	uint8_t* reloadRoutine;
	uint8_t* dispatchArm;
	uint8_t* dispatchThumb;
	uint8_t* dispatchBx;
	uint8_t* dispatchKey;
	// The dispatch routine's miss path, the key still where the routine takes it: an empty or dropped
	// lookup entry points here, so a key equal to the empty mark (~0u, a Thumb branch to FFFFFFFF)
	// leaves for the run loop rather than jumping through a stale or null pointer.
	uint8_t* lookupMiss;
	uint8_t* shiftRoutines[4];
	uint8_t* multiplyCarryRoutine;
	// The shared tail of the exit stubs, for backends that use one.
	uint8_t* exitLinkRoutine;
	// load32, load16, load8, loadS8, loadS16, store32, store16, store8
	uint8_t* memRoutines[8];
	// The same for cartridge code with the prefetch buffer.
	uint8_t* romMemRoutines[8];

	// For each IWRAM and EWRAM word: the opcode last compiled there, and how many times in a row
	// only its 12-bit offset field has been rewritten.
	uint32_t smcWord[(SIZE_IWRAM + SIZE_EWRAM) / 4];
	uint8_t smcCount[(SIZE_IWRAM + SIZE_EWRAM) / 4];
	// Rewrites that changed more than a patchable field, each soon after the one before, and the
	// frame of the last one.
	uint8_t smcShapes[(SIZE_IWRAM + SIZE_EWRAM) / 4];
	struct PagePolicy policy[POLICY_PAGES];
	bool policyDue;
	uint16_t smcShapeFrame[(SIZE_IWRAM + SIZE_EWRAM) / 4];

	uint64_t compiled;
	// Code bytes: block bodies, cold code, and parts of stretches left unused.
	uint64_t hotBytes, coldBytes, wastedBytes;
	// Careful copies (all cold code) among them.
	uint64_t carefulCompiles, carefulBytes;
	// carefulBytes when the code memory was last emptied (CAREFUL_SHARE)
	uint64_t carefulAtFlush;
	// Events that fell in a block stepped through instead, before it earned a careful copy.
	uint64_t carefulPutOff;
	// Block starts stepped through instead of compiled, being met for the first time; how many times
	// each (by lookup slot) has been; and the straight line being stepped through.
	uint64_t deferred;
	uint8_t seenCount[LOOKUP_SIZE];
	uint32_t coldPc;
	// Where the CPU comes back to at a point no branch chose: after the exact CPU has stepped up to
	// an event (midResume), or from an interrupt (irqReturn, ~0 when none is pending).
	bool midResume;
	uint32_t irqReturn;
	bool coldThumb;
	int coldLeft;
	uint8_t* hotMark;
	// Where the code memory's stretches and their parts begin (jit-layout.h), in address order: from
	// each mark's start up to the next mark, code at or past its hotEnd is cold code. freshNext: the
	// code memory no stretch has taken yet.
	// markCap: as many as MARKS_PER_MIB for each MiB of code memory, as a game that compiles all the
	// time (Payback) lays out about 3.4 a MiB, and running out empties the code memory early.
	struct StretchMark {
		uint8_t* start;
		uint8_t* hotEnd;
	}* marks;
	int markCount;
	int markCap;
	uint8_t* freshNext;
	uint8_t* coldMark;
	uint64_t flushes;
	uint64_t invalidations;
	uint64_t revived;
	uint64_t stepped;
	// Changing writes to words compiled code covers.
	uint64_t codeWrites;
	uint64_t patchWrites;
	uint64_t patchableCompiles;
	uint64_t formMisses;
	uint64_t writeReasons[4];
	uint64_t flushReasons[2];
	uint64_t exits[5];
	uint64_t carefulSteps;
	uint64_t midSteps;
};

enum MemKind {
	MEM_LOAD32,
	MEM_LOAD16,
	MEM_LOAD8,
	MEM_LOADS8,
	MEM_LOADS16,
	MEM_STORE32,
	MEM_STORE16,
	MEM_STORE8,
};

// Bytes moved by each kind of memory routine.
static inline int accessSize(int kind) {
	switch (kind) {
	case MEM_LOAD32:
	case MEM_STORE32:
		return 4;
	case MEM_LOAD16:
	case MEM_LOADS16:
	case MEM_STORE16:
		return 2;
	default:
		return 1;
	}
}

int codeKindOf(struct GBA*, uint32_t address);

void jitRecordSite(struct Block*, uint32_t offset, uint32_t pc, uint32_t op0, uint32_t op1);
// With ARMGBA_PERFMAP set, names generated code for perf in /tmp/perf-<pid>.map.
void jitNameCode(const void* start, const void* end, const char* name);
void jitNameBlock(const struct Block*, const void* bodyEnd, const void* cold, const void* end);
uint32_t jitSlowAccess(struct GBA*, int kind, uint32_t address, uint32_t value);
// Whether a word load from the cartridge is PC-relative, so it compiles to a constant and a fixed
// wait.
bool jitRomLiteral(struct GBA*, const struct Inst*, uint32_t* value, int* wait);

// Backend
#if defined(__3DS__)
// No mmap on the 3DS: the frontend makes memory executable and keeps the caches in step with it
// (platform/3ds), through the kernel calls Luma3DS adds.
// Gets executable memory of at least size bytes; may give more (up to 32 MiB, A32's branch reach),
// and says how much in *size.
void* jitHostAllocCode(size_t* size);
void jitHostFreeCode(void* code, size_t size);
// Whether the heap is too low to compile more: the recompiler flushes.
bool jitHostMemoryLow(void);
void jitHostFlushCode(void* start, void* end);
#endif

bool backendInit(struct Jit*);
bool backendCompile(struct Jit*, struct Block*, struct Inst* insts, int count, const uint32_t* nextOps);
void backendPatchJump(uint8_t* site, const void* target);
void backendIdleHead(struct Block*, bool on);
// Called when the code memory is empty again (at start and after a flush); codeNext is where blocks
// begin.
void backendReset(struct Jit*);
// Whether the address is in the part of the code memory kept for cold code.
bool backendColdSide(const struct Jit*, const uint8_t* p);
// For jit-layout.h when the code memory is full: empties the slot whose compiled code is least in
// use and returns it, or NULL when none can go (then the caller flushes). Only called while
// compiling, when no compiled code is running.
uint8_t* jitReclaimSlot(struct Jit*);

bool jitStepOnly(const struct Inst*);
// Writes the CPSR; compiled as a call to jitHelperSystem.
bool jitSystem(const struct Inst*);

// Runtime helpers called from generated code
uint32_t jitHelperExecute(struct GBA*, uint32_t opcode, uint32_t address);
// jitHelperSystem's return value for the exact CPU taking over at the next instruction.
#define JIT_EXACT (~1u)
uint32_t jitHelperSystem(struct GBA*, uint32_t opcode, uint32_t address);
uint32_t jitHelperStepOne(struct GBA*, uint32_t pc);
enum {
	CAREFUL_RUN,
	CAREFUL_STEP,
	CAREFUL_LEAVE,
};
int jitHelperCareful(struct GBA*, int32_t cost, int deferrable, const struct Block*);
int jitHelperDeferred(struct GBA*, const struct Block*);
void jitHelperReload(struct GBA*, uint32_t target);
// Returns the C flag a flag-setting multiply leaves: kind 0 is MUL, 1 UMULL, 2 SMULL; operands are
// in jitScratch.
uint32_t jitMultiplyCarry(struct GBA*, uint32_t kind);
// An STM with writeback whose base isn't the first register in the list stores the already-updated
// base.
static inline bool storesMovedBase(const struct Inst* in) {
	return !in->load && in->writeback && (in->list & (1 << in->rn)) && (in->list & ((1 << in->rn) - 1));
}

void jitHelperCodeWrite(struct GBA*, uint32_t address);
void jitCodeWritten(struct GBA*, uint32_t address, int size);

#endif
