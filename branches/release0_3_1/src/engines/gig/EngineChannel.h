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

#ifndef __LS_GIG_ENGINECHANNEL_H__
#define __LS_GIG_ENGINECHANNEL_H__

#if DEBUG_HEADERS
# warning EngineChannel.h included
#endif // DEBUG_HEADERS

#include "../common/Event.h"
#include "../common/EngineChannel.h"
#include "EngineGlobals.h"
#include "Engine.h"
#include "Voice.h"
#include "InstrumentResourceManager.h"

namespace LinuxSampler { namespace gig {

    // just symbol prototyping
    class midi_key_info_t;
    class Voice;

    /** @brief Engine Channel of a gig::Engine
     *
     * Encapsulates a engine channel for the Gigasampler format capable
     * sampler engine.
     */
    class EngineChannel : public LinuxSampler::EngineChannel, public InstrumentConsumer {
        public:
            EngineChannel();
            virtual ~EngineChannel();

            // implementation of abstract methods derived from interface class 'LinuxSampler::EngineChannel'
            virtual void    PrepareLoadInstrument(const char* FileName, uint Instrument);
            virtual void    LoadInstrument();
            virtual void    SendNoteOn(uint8_t Key, uint8_t Velocity);
            virtual void    SendNoteOff(uint8_t Key, uint8_t Velocity);
            virtual void    SendPitchbend(int Pitch);
            virtual void    SendControlChange(uint8_t Controller, uint8_t Value);
            virtual float   Volume();
            virtual void    Volume(float f);
            virtual uint    Channels();
            virtual void    Connect(AudioOutputDevice* pAudioOut);
            virtual void    DisconnectAudioOutputDevice();
            virtual void    SetOutputChannel(uint EngineAudioChannel, uint AudioDeviceChannel);
            virtual int     OutputChannel(uint EngineAudioChannel);
            virtual String  InstrumentFileName();
            virtual String  InstrumentName();
            virtual int     InstrumentIndex();
            virtual int     InstrumentStatus();
            virtual LinuxSampler::Engine* GetEngine();
            virtual String  EngineName();

            // implementation of abstract methods derived from interface class 'InstrumentConsumer'
            virtual void ResourceToBeUpdated(::gig::Instrument* pResource, void*& pUpdateArg);
            virtual void ResourceUpdated(::gig::Instrument* pOldResource, ::gig::Instrument* pNewResource, void* pUpdateArg);
            virtual void OnResourceProgress(float fProgress);

        //protected:
            Engine*                 pEngine;
            float*                  pOutputLeft;              ///< Audio output channel buffer (left)
            float*                  pOutputRight;             ///< Audio output channel buffer (right)
            int                     AudioDeviceChannelLeft;   ///< audio device channel number to which the left channel is connected to
            int                     AudioDeviceChannelRight;  ///< audio device channel number to which the right channel is connected to
            RingBuffer<Event>*      pEventQueue;              ///< Input event queue.
            RTList<Event>*          pEvents;                  ///< All engine channel specific events for the current audio fragment.
            RTList<Event>*          pCCEvents;                ///< All control change events for the current audio fragment on this engine channel.
            RTList<Event>*          pSynthesisEvents[Event::destination_count]; ///< Events directly affecting synthesis parameters (like pitch, volume and filter).
            uint8_t                 ControllerTable[128];     ///< Reflects the current values (0-127) of all MIDI controllers for this engine / sampler channel.
            midi_key_info_t*        pMIDIKeyInfo;             ///< Contains all active voices sorted by MIDI key number and other informations to the respective MIDI key
            Pool<uint>*             pActiveKeys;              ///< Holds all keys in it's allocation list with active voices.
            std::map<uint,uint*>    ActiveKeyGroups;          ///< Contains active keys (in case they belong to a key group) ordered by key group ID.
            ::gig::Instrument*      pInstrument;
            bool                    SustainPedal;             ///< true if sustain pedal is down
            double                  GlobalVolume;             ///< overall volume (a value < 1.0 means attenuation, a value > 1.0 means amplification)
            float                   GlobalPanLeft;
            float                   GlobalPanRight;
            int                     Pitch;                    ///< Current (absolute) MIDI pitch value.
            int                     CurrentKeyDimension;      ///< Current value (0-127) for the keyboard dimension, altered by pressing a keyswitching key.
            String                  InstrumentFile;
            int                     InstrumentIdx;
            String                  InstrumentIdxName;
            int                     InstrumentStat;
            int                     iEngineIndexSelf;         ///< Reflects the index of this EngineChannel in the Engine's ArrayList.

            void ResetControllers();
            void ClearEventLists();
            void ImportEvents(uint Samples);

            friend class Engine;
            friend class Voice;
            friend class InstrumentResourceManager;

        private:
            void ResetInternal();
    };

}} // namespace LinuxSampler::gig

#endif // __LS_GIG_ENGINECHANNEL_H__
