/***************************************************************************
 *                                                                         *
 *   LinuxSampler - modular, streaming capable sampler                     *
 *                                                                         *
 *   Copyright (C) 2003, 2004 by Benno Senoner and Christian Schoenebeck   *
 *   Copyright (C) 2005 Christian Schoenebeck                              *
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

#ifndef __LS_GIG_ENGINE_H__
#define __LS_GIG_ENGINE_H__

#include "../../common/global.h"

#if DEBUG_HEADERS
# warning Engine.h included
#endif // DEBUG_HEADERS

#include <map>

#include "EngineGlobals.h"
#include "../../common/RingBuffer.h"
#include "../../common/Pool.h"
#include "../../common/ConditionServer.h"
#include "../common/Engine.h"
#include "../common/Event.h"
#include "../common/BiquadFilter.h"
#include "../../lib/fileloader/libgig/gig.h"
#include "../../network/lscp.h"
#include "EngineChannel.h"

namespace LinuxSampler { namespace gig {

    // just symbol prototyping
    class Voice;
    class DiskThread;
    class InstrumentResourceManager;
    class midi_key_info_t;
    class EngineChannel;

    /**
     * Sampler engine for the Gigasampler format.
     */
    class Engine : public LinuxSampler::Engine {
        public:           
            // methods
            Engine();
           ~Engine();
            int  RenderAudio(LinuxSampler::gig::EngineChannel* pEngineChannel, uint Samples);
            void Connect(AudioOutputDevice* pAudioOut);

            // implementation of abstract methods derived from class 'LinuxSampler::Engine'
            virtual void   SendSysex(void* pData, uint Size);
            virtual void   Reset();
            virtual void   Enable();
            virtual void   Disable();
            virtual uint   VoiceCount();
            virtual uint   VoiceCountMax();
            virtual bool   DiskStreamSupported();
            virtual uint   DiskStreamCount();
            virtual uint   DiskStreamCountMax();
            virtual String DiskStreamBufferFillBytes();
            virtual String DiskStreamBufferFillPercentage();
            virtual String Description();
            virtual String Version();
            virtual String EngineName();
                        
        //protected:            
            static InstrumentResourceManager instruments;

            AudioOutputDevice*      pAudioOutputDevice;
            uint                    SampleRate;            ///< Sample rate of the engines output audio signal (in Hz)
            uint                    MaxSamplesPerCycle;    ///< Size of each audio output buffer
            DiskThread*             pDiskThread;          
            RingBuffer<Event>*      pEventQueue;           ///< Input event queue for engine global events (e.g. SysEx messages).
            Pool<Voice>*            pVoicePool;            ///< Contains all voices that can be activated.                                    
            EventGenerator*         pEventGenerator;            
            RTList<Event>*          pVoiceStealingQueue;   ///< All voice-launching events which had to be postponed due to free voice shortage.
            RTList<Event>*          pEvents;               ///< All events for the current audio fragment.
            Pool<Event>*            pEventPool;            ///< Contains all Event objects that can be used.
            RTList<Event>*          pCCEvents;             ///< All control change events for the current audio fragment.
            RingBuffer<uint8_t>*    pSysexBuffer;          ///< Input buffer for MIDI system exclusive messages.
            RTList<Event>*          pSynthesisEvents[Event::destination_count];     ///< Events directly affecting synthesis parameter (like pitch, volume and filter).
            float*                  pSynthesisParameters[Event::destination_count]; ///< Matrix with final synthesis parameters for the current audio fragment which will be used in the main synthesis loop.
            biquad_param_t*         pBasicFilterParameters; ///< Biquad parameters of the basic bandpass filter.
            biquad_param_t*         pMainFilterParameters;  ///< Main biquad parameters of the individual filter (lowpass / bandpass / highpass).
            int                     ActiveVoiceCount;      ///< number of currently active voices
            int                     ActiveVoiceCountMax;   ///< the maximum voice usage since application start
            bool                    SuspensionRequested;
            ConditionServer         EngineDisabled;
            int8_t                  ScaleTuning[12];       ///< contains optional detune factors (-64..+63 cents) for all 12 semitones of an octave
            RTList<Voice>::Iterator itLastStolenVoice;      ///< Only for voice stealing: points to the last voice which was theft in current audio fragment, NULL otherwise.
            RTList<uint>::Iterator  iuiLastStolenKey;      ///< Only for voice stealing: key number of last key on which the last voice was theft in current audio fragment, NULL otherwise.
            int                     MaxFadeOutPos;         ///< The last position in an audio fragment to allow an instant fade out (e.g. for voice stealing) without leading to clicks.

            void ProcessNoteOn(EngineChannel* pEngineChannel, Pool<Event>::Iterator& itNoteOnEvent);
            void ProcessNoteOff(EngineChannel* pEngineChannel, Pool<Event>::Iterator& itNoteOffEvent);
            void ProcessPitchbend(EngineChannel* pEngineChannel, Pool<Event>::Iterator& itPitchbendEvent);
            void ProcessControlChange(EngineChannel* pEngineChannel, Pool<Event>::Iterator& itControlChangeEvent);
            void ProcessSysex(Pool<Event>::Iterator& itSysexEvent);
            Pool<Voice>::Iterator LaunchVoice(EngineChannel* pEngineChannel, Pool<Event>::Iterator& itNoteOnEvent, int iLayer, bool ReleaseTriggerVoice, bool VoiceStealing);
            void StealVoice(EngineChannel* pEngineChannel, Pool<Event>::Iterator& itNoteOnEvent);
            void FreeVoice(EngineChannel* pEngineChannel, Pool<Voice>::Iterator& itVoice);
            void FreeKey(EngineChannel* pEngineChannel, midi_key_info_t* pKey);
            void ResetSynthesisParameters(Event::destination_t dst, float val);
            void ResetInternal();

            static Engine* AcquireEngine(LinuxSampler::gig::EngineChannel* pChannel, AudioOutputDevice* pDevice);
            static void    FreeEngine(LinuxSampler::gig::EngineChannel* pChannel, AudioOutputDevice* pDevice);

            void DisableAndLock(); // FIXME: should at least be protected

            friend class Voice;
            friend class EGADSR;
            friend class EGDecay;
            friend class VCAManipulator;
            friend class VCFCManipulator;
            friend class VCOManipulator;            
        private:
            std::list<EngineChannel*> samplerChannels; ///< All sampler channels of a gig::Engine instance.
                        
            static std::map<AudioOutputDevice*,Engine*> engines; ///< All instances of gig::Engine.
                        
            uint8_t GSCheckSum(const RingBuffer<uint8_t>::NonVolatileReader AddrReader, uint DataSize);
            void    AdjustScale(int8_t ScaleTunes[12]);
    };

}} // namespace LinuxSampler::gig

#endif // __LS_GIG_ENGINE_H__
