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

#ifndef SOUND_H
#define SOUND_H

#include "gbint.h"
#include <vector>
#include "sound/channel1.h"
#include "sound/channel2.h"
#include "sound/channel3.h"
#include "sound/channel4.h"
#include "newstate.h"

namespace gambatte {

class PSG {
public:
	PSG();
	void init(bool cgb, bool agb);
	void reset(bool ds);
	void divReset(bool ds);
	void setStatePtrs(SaveState &state);
	void saveState(SaveState &state);
	void loadState(SaveState const &state);

	void generateSamples(gb_ulong cycleCounter, bool doubleSpeed);
	void resetCounter(gb_ulong newCc, gb_ulong oldCc, bool doubleSpeed);
	void speedChange(gb_ulong cc, bool doubleSpeed);
	std::size_t fillBuffer();
	void setBuffer(uint_least32_t *buf) { buffer_ = buf; bufferPos_ = 0; dacsAtStart_ = dacsLast_; dacEdgeCount_ = 0; muted_ = mutedNext_; }

	// armGBA: Channel Selection's channels left out of what's heard (bit 0 channel 1 to bit 3 channel
	// 4), from the next buffer. The channels run, and the buffer and its running sum (saved) add up as
	// before; what's heard is summed apart (heardSamples).
	void setMutedChannels(unsigned mask) { mutedNext_ = mask & 0xF; }
	// What's heard, as fillBuffer made it, while channels are left out (else 0); and its running sum,
	// which a frontend keeps beside a state to carry on as it would have.
	uint_least32_t const * heardSamples() const { return muted_ && !heard_.empty() ? &heard_[0] : 0; }
	uint_least32_t heardSum() const { return heardSum_; }
	void setHeardSum(uint_least32_t sum) { heardSum_ = sum; }

	// armGBA: where in the buffer since setBuffer the DACs went from all off to any on, or back: the
	// output's high-pass filter is connected only while one is on (Pan Docs, Audio Details).
	enum { DAC_EDGES = 64 };
	bool dacsOn() const { return ch1_.dacOn() || ch2_.dacOn() || ch3_.dacOn() || ch4_.dacOn(); }
	bool dacsAtStart() const { return dacsAtStart_; }
	std::size_t dacEdges(std::size_t const **positions, bool const **on) const {
		*positions = dacEdgePos_;
		*on = dacEdgeOn_;
		return dacEdgeCount_;
	}

	bool isEnabled() const { return enabled_; }
	void setEnabled(bool value) { enabled_ = value; }

	void setNr10(unsigned data) { ch1_.setNr0(data); }
	void setNr11(unsigned data) { ch1_.setNr1(data, cycleCounter_); }
	void setNr12(unsigned data) { ch1_.setNr2(data, cycleCounter_); noteDacs(); }
	void setNr13(unsigned data) { ch1_.setNr3(data, cycleCounter_); }
	void setNr14(unsigned data, bool ds) { ch1_.setNr4(data, cycleCounter_, !(lastUpdate_ & ds)); }

	void setNr21(unsigned data) { ch2_.setNr1(data, cycleCounter_); }
	void setNr22(unsigned data) { ch2_.setNr2(data, cycleCounter_); noteDacs(); }
	void setNr23(unsigned data) { ch2_.setNr3(data, cycleCounter_); }
	void setNr24(unsigned data, bool ds) { ch2_.setNr4(data, cycleCounter_, !(lastUpdate_ & ds)); }

	void setNr30(unsigned data, unsigned short pc) { ch3_.setNr0(data, cycleCounter_, pc); noteDacs(); }
	void setNr31(unsigned data) { ch3_.setNr1(data, cycleCounter_); }
	void setNr32(unsigned data) { ch3_.setNr2(data); }
	void setNr33(unsigned data) { ch3_.setNr3(data); }
	void setNr34(unsigned data) { ch3_.setNr4(data, cycleCounter_); }
	unsigned waveRamRead(unsigned index) const { return ch3_.waveRamRead(index, cycleCounter_); }
	void waveRamWrite(unsigned index, unsigned data) { ch3_.waveRamWrite(index, data, cycleCounter_); }

	void setNr41(unsigned data) { ch4_.setNr1(data, cycleCounter_); }
	void setNr42(unsigned data) { ch4_.setNr2(data, cycleCounter_); noteDacs(); }
	void setNr43(unsigned data) { ch4_.setNr3(data, cycleCounter_); }
	void setNr44(unsigned data) { ch4_.setNr4(data, cycleCounter_); }

	void setSoVolume(unsigned nr50);
	void mapSo(unsigned nr51);
	unsigned getStatus() const;

	gb_ulong callbackCycleOffset(gb_ulong cycleCounter, bool doubleSpeed) const;
	unsigned char pcm12Read(gb_ulong cycleCounter, bool doubleSpeed);
	unsigned char pcm34Read(gb_ulong cycleCounter, bool doubleSpeed);

	void setSpeedupFlags(unsigned flags) { speedupFlags_ = flags; }

	template<bool isReader>void SyncState(NewState *ns);

private:
	Channel1 ch1_;
	Channel2 ch2_;
	Channel3 ch3_;
	Channel4 ch4_;
	uint_least32_t *buffer_;
	std::size_t bufferPos_;
	gb_ulong lastUpdate_;
	gb_ulong cycleCounter_;
	gb_ulong soVol_;
	uint_least32_t rsum_;
	bool enabled_;

	unsigned speedupFlags_;
	bool dacsLast_;
	bool dacsAtStart_;
	std::size_t dacEdgeCount_;
	std::size_t dacEdgePos_[DAC_EDGES];
	bool dacEdgeOn_[DAC_EDGES];

	// armGBA: what's heard with channels left out, and its running sum; not saved
	unsigned muted_;
	unsigned mutedNext_;
	std::vector<uint_least32_t> heard_;
	std::vector<uint_least32_t> mutedScratch_;
	uint_least32_t heardSum_;

	void accumulateChannels(gb_ulong cycles);
	void accumulateHeard(gb_ulong cycles);
	// After a write that can turn a DAC on or off, with the samples up to it already generated
	void noteDacs() {
		bool const on = dacsOn();
		if (on != dacsLast_) {
			if (dacEdgeCount_ < DAC_EDGES) {
				dacEdgePos_[dacEdgeCount_] = bufferPos_;
				dacEdgeOn_[dacEdgeCount_++] = on;
			}
			dacsLast_ = on;
		}
	}
};

}

#endif
