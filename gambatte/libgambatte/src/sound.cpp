//
//   Copyright (C) 2007 by sinamas <sinamas at users.sourceforge.net>
//
//   This program is free software; you can redistribute it and/or modify
//   it under the terms of the GNU General Public License version 2 as
//   published by the Free Software Foundation.
//
//   This program is distributed in the hope that it will be useful,
//   but WITHOUT ANY WARRANTY; without even the implied warranty of
//   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//   GNU General Public License version 2 for more details.
//
//   You should have received a copy of the GNU General Public License
//   version 2 along with this program; if not, write to the
//   Free Software Foundation, Inc.,
//   51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA.
//

#include "gbint.h"
#include "sound.h"
#include "gambatte.h"
#include "savestate.h"

#include <algorithm>
#include <cstring>

/*
	Frame Sequencer

	Step    Length Ctr  Vol Env     Sweep
	- - - - - - - - - - - - - - - - - - - -
S0	0       Clock       -           Clock
S1	1       -           Clock       -
	2       Clock       -           -
	3       -           -           -
	4       Clock       -           Clock
	5       -           -           -
	6       Clock       -           -
	7       -           -           -
	- - - - - - - - - - - - - - - - - - - -
	Rate    256 Hz      64 Hz       128 Hz

S0) start step on sound power on.
S1) step gets immediately incremented at power on if closer to previous edge than next.
Clock) clock timer on transition to step.

*/

using namespace gambatte;

PSG::PSG()
: buffer_(0)
, bufferPos_(0)
, lastUpdate_(0)
, cycleCounter_(0)
, soVol_(0)
, rsum_(0x8000) // initialize to 0x8000 to prevent borrows from high word, xor away later
, enabled_(false)
, speedupFlags_(0)
, dacsLast_(false)
, dacsAtStart_(false)
, dacEdgeCount_(0)
, muted_(0)
, mutedNext_(0)
, heardSum_(0x8000)
{
}

void PSG::init(bool cgb, bool agb) {
	ch1_.init(cgb);
	ch3_.init(cgb, agb);
}

void PSG::reset(bool ds) {
	int const divOffset = lastUpdate_ & ds;
	gb_ulong const cc = cycleCounter_ + divOffset;
	// cycleCounter >> 12 & 7 represents the frame sequencer position.
	cycleCounter_ = (cc & 0xFFF) + 2 * (~(cc + 1 + !ds) & 0x800);
	lastUpdate_ = ((lastUpdate_ + 3) & -4) - !ds;
	ch1_.reset();
	ch2_.reset();
	ch3_.reset();
	ch4_.reset(cycleCounter_);
	noteDacs();
}

void PSG::divReset(bool ds) {
	int const divOffset = lastUpdate_ & ds;
	gb_ulong const cc = cycleCounter_ + divOffset;
	cycleCounter_ = ((cc & -0x1000) + 2 * (cc & 0x800) - divOffset) % SoundUnit::counter_max;
	ch1_.resetCc(cc - divOffset, cycleCounter_);
	ch2_.resetCc(cc - divOffset, cycleCounter_);
	ch3_.resetCc(cc - divOffset, cycleCounter_);
	ch4_.resetCc(cc - divOffset, cycleCounter_);
}

void PSG::speedChange(gb_ulong const cpuCc, bool const ds) {
	generateSamples(cpuCc, ds);
	lastUpdate_ -= ds;
	// correct for cycles since DIV reset (if any).
	if (!ds) {
		gb_ulong const cc = cycleCounter_;
		unsigned const divCycles = cc & 0xFFF;
		cycleCounter_ = cc - divCycles / 2 - lastUpdate_ % 2;
		ch1_.resetCc(cc, cycleCounter_);
		ch2_.resetCc(cc, cycleCounter_);
		ch3_.resetCc(cc, cycleCounter_);
		ch4_.resetCc(cc, cycleCounter_);
	}
}

void PSG::setStatePtrs(SaveState &state) {
	ch3_.setStatePtrs(state);
}

void PSG::saveState(SaveState &state) {
	state.spu.cycleCounter = cycleCounter_;
	state.spu.lastUpdate = (lastUpdate_ + 1) % 4;
	state.spu.rsum = rsum_ ^ 0x8000;
	ch1_.saveState(state, cycleCounter_);
	ch2_.saveState(state, cycleCounter_);
	ch3_.saveState(state);
	ch4_.saveState(state, cycleCounter_);
}

void PSG::loadState(SaveState const &state) {
	ch1_.loadState(state);
	ch2_.loadState(state);
	ch3_.loadState(state);
	ch4_.loadState(state);

	cycleCounter_ = state.spu.cycleCounter;
	lastUpdate_ = state.cpu.cycleCounter - (1 - state.spu.lastUpdate) % 4u;
	rsum_ = state.spu.rsum ^ 0x8000;
	setSoVolume(state.mem.ioamhram.get()[0x124]);
	mapSo(state.mem.ioamhram.get()[0x125]);
	enabled_ = state.mem.ioamhram.get()[0x126] >> 7 & 1;
	ch1_.restoreEvents(state);
	ch2_.restoreEvents(state);
	ch4_.restoreCounting(state);
	dacsLast_ = dacsAtStart_ = dacsOn();
	dacEdgeCount_ = 0;
	// What's heard picks up from here; a left-out channel's level then is a constant the output's
	// filter takes away.
	heardSum_ = rsum_;
}

inline void PSG::accumulateChannels(gb_ulong const cycles) {
	if (muted_) {
		accumulateHeard(cycles);
		return;
	}
	gb_ulong const cc = cycleCounter_;
	uint_least32_t *const buf = buffer_ + bufferPos_;
	std::memset(buf, 0, cycles * sizeof *buf);
	ch1_.update(buf, soVol_, cc, cc + cycles);
	ch2_.update(buf, soVol_, cc, cc + cycles);
	ch3_.update(buf, soVol_, cc, cc + cycles);
	ch4_.update(buf, soVol_, cc, cc + cycles);
	cycleCounter_ = (cc + cycles) % SoundUnit::counter_max;
}

// armGBA: with channels left out (Channel Selection), they add into a scratch buffer, so the buffer
// holds what's heard for a moment, kept in heard_, then all of them as before.
void PSG::accumulateHeard(gb_ulong const cycles) {
	gb_ulong const cc = cycleCounter_;
	uint_least32_t *const buf = buffer_ + bufferPos_;
	std::memset(buf, 0, cycles * sizeof *buf);
	if (mutedScratch_.size() < cycles + 16)
		mutedScratch_.resize(cycles + 16);
	if (heard_.size() < bufferPos_ + cycles + 16)
		heard_.resize(bufferPos_ + cycles + 16);
	uint_least32_t *const scratch = &mutedScratch_[0];
	std::memset(scratch, 0, cycles * sizeof *scratch);
	ch1_.update(muted_ & 1 ? scratch : buf, soVol_, cc, cc + cycles);
	ch2_.update(muted_ & 2 ? scratch : buf, soVol_, cc, cc + cycles);
	ch3_.update(muted_ & 4 ? scratch : buf, soVol_, cc, cc + cycles);
	ch4_.update(muted_ & 8 ? scratch : buf, soVol_, cc, cc + cycles);
	std::memcpy(&heard_[bufferPos_], buf, cycles * sizeof *buf);
	for (gb_ulong i = 0; i < cycles; ++i)
		buf[i] += scratch[i];
	cycleCounter_ = (cc + cycles) % SoundUnit::counter_max;
}

void PSG::generateSamples(gb_ulong const cpuCc, bool const doubleSpeed) {
	gb_ulong const cycles = (cpuCc - lastUpdate_) >> (1 + doubleSpeed);
	lastUpdate_ += cycles << (1 + doubleSpeed);

	if (!(speedupFlags_ & GB::NO_SOUND)) {
		if (cycles)
			accumulateChannels(cycles);
	}

	bufferPos_ += cycles;
}

void PSG::resetCounter(gb_ulong newCc, gb_ulong oldCc, bool doubleSpeed) {
	generateSamples(oldCc, doubleSpeed);
	lastUpdate_ = newCc - (oldCc - lastUpdate_);
}

std::size_t PSG::fillBuffer() {
	uint_least32_t sum = rsum_;
	uint_least32_t *b = buffer_;
	std::size_t n = bufferPos_;

	if (std::size_t n2 = n >> 3) {
		n -= n2 << 3;

		do {
			sum += b[0];
			b[0] = sum ^ 0x8000;
			sum += b[1];
			b[1] = sum ^ 0x8000;
			sum += b[2];
			b[2] = sum ^ 0x8000;
			sum += b[3];
			b[3] = sum ^ 0x8000;
			sum += b[4];
			b[4] = sum ^ 0x8000;
			sum += b[5];
			b[5] = sum ^ 0x8000;
			sum += b[6];
			b[6] = sum ^ 0x8000;
			sum += b[7];
			b[7] = sum ^ 0x8000;

			b += 8;
		} while (--n2);
	}

	while (n--) {
		sum += *b;
		// xor away the initial rsum value of 0x8000 (which prevents
		// borrows from the high word) from the low word
		*b++ = sum ^ 0x8000;
	}

	rsum_ = sum;

	// armGBA: with channels left out, what's heard is summed apart, into heard_ (heardSamples).
	if (muted_) {
		if (heard_.size() < bufferPos_ + 1)
			heard_.resize(bufferPos_ + 1);
		uint_least32_t heardSum = heardSum_;
		uint_least32_t *const h = &heard_[0];
		for (std::size_t i = 0; i < bufferPos_; ++i) {
			heardSum += speedupFlags_ & GB::NO_SOUND ? 0 : h[i];
			h[i] = heardSum ^ 0x8000;
		}
		heardSum_ = heardSum;
	} else
		heardSum_ = rsum_;

	return bufferPos_;
}

static bool isBigEndianSampleOrder() {
	union {
		uint_least32_t ul32;
		unsigned char uc[sizeof(uint_least32_t)];
	} u;
	u.ul32 = -0x10000;
	return u.uc[0];
}

static gb_ulong so1Mul() { return isBigEndianSampleOrder() ? 0x00000001 : 0x00010000; }
static gb_ulong so2Mul() { return isBigEndianSampleOrder() ? 0x00010000 : 0x00000001; }

void PSG::setSoVolume(unsigned nr50) {
	soVol_ = ((nr50      & 0x7) + 1) * so1Mul() * 64
	       + ((nr50 >> 4 & 0x7) + 1) * so2Mul() * 64;
}

void PSG::mapSo(unsigned nr51) {
	gb_ulong so = nr51 * so1Mul() + (nr51 >> 4) * so2Mul();
	ch1_.setSo((so      & 0x00010001) * 0xFFFF, cycleCounter_);
	ch2_.setSo((so >> 1 & 0x00010001) * 0xFFFF, cycleCounter_);
	ch3_.setSo((so >> 2 & 0x00010001) * 0xFFFF);
	ch4_.setSo((so >> 3 & 0x00010001) * 0xFFFF, cycleCounter_);
}

unsigned PSG::getStatus() const {
	return ch1_.isActive()
	     | ch2_.isActive() << 1
	     | ch3_.isActive() << 2
	     | ch4_.isActive() << 3;
}

gb_ulong PSG::callbackCycleOffset(gb_ulong const cpuCc, bool const doubleSpeed) const {
	gb_ulong const cyclesOff = (cpuCc - lastUpdate_) >> (1 + doubleSpeed);
	return bufferPos_ + cyclesOff;
}

unsigned char PSG::pcm12Read(gb_ulong const cpuCc, bool const doubleSpeed) {
	generateSamples(cpuCc, doubleSpeed);
	return ((ch2_.isActive() ? ch2_.getVolume() : 0) << 4)
	| (ch1_.isActive() ? ch1_.getVolume() : 0);
}

unsigned char PSG::pcm34Read(gb_ulong const cpuCc, bool const doubleSpeed) {
	generateSamples(cpuCc, doubleSpeed);
	return ((ch4_.isActive() ? ch4_.getVolume() : 0) << 4)
	| (ch3_.isActive() ? ch3_.getVolume() : 0);
}

// the buffer and position are not saved, as they're set and flushed on each runfor() call
SYNCFUNC(PSG) {
	SSS(ch1_);
	SSS(ch2_);
	SSS(ch3_);
	SSS(ch4_);
	NSS(lastUpdate_);
	NSS(cycleCounter_);
	NSS(soVol_);
	NSS(rsum_);
	NSS(enabled_);
}
