/***************************************************************************
 *                                                                         *
 *   LinuxSampler - modular, streaming capable sampler                     *
 *                                                                         *
 *   Copyright (C) 2003, 2004 by Benno Senoner and Christian Schoenebeck   *
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

#include "../../common/RingBuffer.h"
#include "../../common/RTELMemoryPool.h"
#include "../../common/ConditionServer.h"
#include "../common/Engine.h"
#include "../common/Event.h"
#include "../../lib/fileloader/libgig/gig.h"
#include "InstrumentResourceManager.h"
#include "../../network/lscp.h"

#define PITCHBEND_SEMITONES		12
#define MAX_AUDIO_VOICES		64

namespace LinuxSampler { namespace gig {

    // just symbol prototyping
    class Voice;
    class DiskThread;
    class InstrumentResourceManager;

    /**
     * Sampler engine for the Gigasampler format.
     */
    class gig::Engine : public LinuxSampler::Engine, public InstrumentConsumer {
        public:
            // methods
            Engine();
           ~Engine();

            // abstract methods derived from class 'LinuxSampler::Engine'
            virtual void   LoadInstrument(const char* FileName, uint Instrument);
            virtual void   Reset();
            virtual void   Enable();
            virtual void   Disable();
            virtual void   SendNoteOn(uint8_t Key, uint8_t Velocity);
            virtual void   SendNoteOff(uint8_t Key, uint8_t Velocity);
            virtual void   SendPitchbend(int Pitch);
            virtual void   SendControlChange(uint8_t Controller, uint8_t Value);
            virtual float  Volume();
            virtual void   Volume(float f);
            virtual void   Connect(AudioOutputDevice* pAudioOut);
            virtual void   DisconnectAudioOutputDevice();
            virtual int    RenderAudio(uint Samples);
            virtual uint   VoiceCount();
            virtual uint   VoiceCountMax();
            virtual bool   DiskStreamSupported();
            virtual uint   DiskStreamCount();
            virtual uint   DiskStreamCountMax();
            virtual String DiskStreamBufferFillBytes();
            virtual String DiskStreamBufferFillPercentage();
            virtual String Description();
            virtual String Version();

            // abstract methods derived from interface class 'InstrumentConsumer'
            virtual void ResourceToBeUpdated(::gig::Instrument* pResource, void*& pUpdateArg);
            virtual void ResourceUpdated(::gig::Instrument* pOldResource, ::gig::Instrument* pNewResource, void* pUpdateArg);
        protected:
            struct midi_key_info_t {
                RTEList<Voice>* pActiveVoices; ///< Contains the active voices associated with the MIDI key.
                bool            KeyPressed;    ///< Is true if the respective MIDI key is currently pressed.
                bool            Active;        ///< If the key contains active voices.
                uint*           pSelf;         ///< hack to allow fast deallocation of the key from the list of active keys
                RTEList<Event>* pEvents;       ///< Key specific events (only Note-on, Note-off and sustain pedal currently)
            };

            static InstrumentResourceManager Instruments;

            AudioOutputDevice*      pAudioOutputDevice;
            DiskThread*             pDiskThread;
            uint8_t                 ControllerTable[128];  ///< Reflects the current values (0-127) of all MIDI controllers for this engine / sampler channel.
            RingBuffer<Event>*      pEventQueue;           ///< Input event queue.
            midi_key_info_t         pMIDIKeyInfo[128];     ///< Contains all active voices sorted by MIDI key number and other informations to the respective MIDI key
            RTELMemoryPool<Voice>*  pVoicePool;            ///< Contains all voices that can be activated.
            RTELMemoryPool<uint>*   pActiveKeys;           ///< Holds all keys in it's allocation list with active voices.
            RTELMemoryPool<Event>*  pEventPool;            ///< Contains all Event objects that can be used.
            EventGenerator*         pEventGenerator;
            RTEList<Event>*         pEvents;               ///< All events for the current audio fragment.
            RTEList<Event>*         pCCEvents;             ///< All control change events for the current audio fragment.
            RTEList<Event>*         pSynthesisEvents[Event::destination_count];     ///< Events directly affecting synthesis parameter (like pitch, volume and filter).
            float*                  pSynthesisParameters[Event::destination_count]; ///< Matrix with final synthesis parameters for the current audio fragment which will be used in the main synthesis loop.
            RIFF::File*             pRIFF;
            ::gig::File*            pGig;
            ::gig::Instrument*      pInstrument;
            bool                    SustainPedal;          ///< true if sustain pedal is down
            double                  GlobalVolume;          ///< overall volume (a value < 1.0 means attenuation, a value > 1.0 means amplification)
            int                     Pitch;                 ///< Current (absolute) MIDI pitch value.
            int                     ActiveVoiceCount;      ///< number of currently active voices
            int                     ActiveVoiceCountMax;   ///< the maximum voice usage since application start
            bool                    SuspensionRequested;
            ConditionServer         EngineDisabled;

            void ProcessNoteOn(Event* pNoteOnEvent);
            void ProcessNoteOff(Event* pNoteOffEvent);
            void ProcessPitchbend(Event* pPitchbendEvent);
            void ProcessControlChange(Event* pControlChangeEvent);
            void KillVoice(Voice* pVoice);
            void ResetSynthesisParameters(Event::destination_t dst, float val);
            void ResetInternal();

            friend class Voice;
            friend class EGADSR;
            friend class EGDecay;
            friend class VCAManipulator;
            friend class VCFCManipulator;
            friend class VCOManipulator;
            friend class InstrumentResourceManager;
        private:
            //static void AllocateSynthesisParametersMatrix();

            void DisableAndLock();
    };

}} // namespace LinuxSampler::gig

#endif // __LS_GIG_ENGINE_H__