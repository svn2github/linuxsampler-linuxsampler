/***************************************************************************
 *                                                                         *
 *   LinuxSampler - modular, streaming capable sampler                     *
 *                                                                         *
 *   Copyright (C) 2003 by Benno Senoner                                   *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the Free Software           *
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston,                 *
 *   MA  02111-1307  USA                                                   *
 ***************************************************************************/

#include "stream.h"

// FIXME: already independent of audio channels, but assumes sample depth defined by 'sample_t' (usually 16 bit)


// *********** Stream **************
// *

uint Stream::UnusedStreams = 0;

/// Returns number of refilled sample points or a value < 0 on error.
int Stream::ReadAhead(unsigned long SampleCount) {
    if (this->State == state_unused) return -1;
    if (this->State == state_end)    return  0;
    if (!SampleCount)                return  0;
    if (!pRingBuffer->write_space()) return  0;

    long total_readsamples = 0, readsamples = 0;
    long samplestoread = SampleCount / pSample->Channels;
    sample_t* pBuf = pRingBuffer->get_write_ptr();
    bool endofsamplereached;

    // refill the disk stream buffer
    if (this->DoLoop) { // honor looping
        total_readsamples  = pSample->ReadAndLoop(pBuf, samplestoread, &this->PlaybackState);
        endofsamplereached = (this->PlaybackState.position >= pSample->SamplesTotal);
        dmsg(5,("Refilled stream %d with %d (SamplePos: %d)", this->hThis, total_readsamples, this->PlaybackState.position));
    }
    else { // normal forward playback

        pSample->SetPos(this->SampleOffset); // recover old position

        do {
            readsamples        = pSample->Read(&pBuf[total_readsamples * pSample->Channels], samplestoread);
            samplestoread     -= readsamples;
            total_readsamples += readsamples;
        } while (samplestoread && readsamples > 0);

        // we have to store the position within the sample, because other streams might use the same sample
        this->SampleOffset = pSample->GetPos();

        endofsamplereached = (SampleOffset >= pSample->SamplesTotal);
        dmsg(5,("Refilled stream %d with %d (SamplePos: %d)", this->hThis, total_readsamples, this->SampleOffset));
    }

    // we must delay the increment_write_ptr_with_wrap() after the while() loop because we need to
    // ensure that we read exactly SampleCount sample, otherwise the buffer wrapping code will fail
    pRingBuffer->increment_write_ptr_with_wrap(total_readsamples * pSample->Channels);

    // update stream state
    if (endofsamplereached) SetState(state_end);
    else                    SetState(state_active);

    return total_readsamples;
}

void Stream::WriteSilence(unsigned long SilenceSampleWords) {
    memset(pRingBuffer->get_write_ptr(), 0, SilenceSampleWords);
    pRingBuffer->increment_write_ptr_with_wrap(SilenceSampleWords);
}

Stream::Stream(uint BufferSize, uint BufferWrapElements) {
    this->pExportReference               = NULL;
    this->State                          = state_unused;
    this->hThis                          = 0;
    this->pSample                        = NULL;
    this->SampleOffset                   = 0;
    this->PlaybackState.position         = 0;
    this->PlaybackState.reverse          = false;
    this->pRingBuffer                    = new RingBuffer<sample_t>(BufferSize, BufferWrapElements);
    UnusedStreams++;
}

Stream::~Stream() {
    Reset();
    if (pRingBuffer) delete pRingBuffer;
}

/// Called by disk thread to activate the disk stream.
void Stream::Launch(Stream::Handle hStream, reference_t* pExportReference, gig::Sample* pSample, unsigned long SampleOffset, bool DoLoop) {
    UnusedStreams--;
    this->pExportReference               = pExportReference;
    this->hThis                          = hStream;
    this->pSample                        = pSample;
    this->SampleOffset                   = SampleOffset;
    this->PlaybackState.position         = SampleOffset;
    this->PlaybackState.reverse          = false;
    this->PlaybackState.loop_cycles_left = pSample->LoopPlayCount;
    this->DoLoop                         = DoLoop;
    SetState(state_active);
}
