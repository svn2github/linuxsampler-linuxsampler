/***************************************************************************
 *                                                                         *
 *   LinuxSampler - modular, streaming capable sampler                     *
 *                                                                         *
 *   Copyright (C) 2003,2004 by Benno Senoner and Christian Schoenebeck    *
 *   Copyright (C) 2005-2009 Christian Schoenebeck                         *
 *   Copyright (C) 2009-2010 Grigor Iliev                                  *
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

#include "Engine.h"
#include "EngineChannel.h"

namespace LinuxSampler { namespace sfz {
    Engine::Format Engine::GetEngineFormat() { return SFZ; }

    /**
     *  Reacts on supported control change commands (e.g. pitch bend wheel,
     *  modulation wheel, aftertouch).
     *
     *  @param pEngineChannel - engine channel on which this event occured on
     *  @param itControlChangeEvent - controller, value and time stamp of the event
     */
    void Engine::ProcessControlChange (
        LinuxSampler::EngineChannel*  pEngineChannel,
        Pool<Event>::Iterator&        itControlChangeEvent
    ) {
        dmsg(4,("Engine::ContinuousController cc=%d v=%d\n", itControlChangeEvent->Param.CC.Controller, itControlChangeEvent->Param.CC.Value));

        EngineChannel* pChannel = static_cast<EngineChannel*>(pEngineChannel);
        // handle the "control triggered" MIDI rule: a control change
        // event can trigger a new note on or note off event
        if (pChannel->pInstrument) {

            // TODO: 
        }

        // update controller value in the engine channel's controller table
        pChannel->ControllerTable[itControlChangeEvent->Param.CC.Controller] = itControlChangeEvent->Param.CC.Value;

        ProcessHardcodedControllers(pEngineChannel, itControlChangeEvent);

        // handle FX send controllers
        ProcessFxSendControllers(pChannel, itControlChangeEvent);
    }

    DiskThread* Engine::CreateDiskThread() {
        return new DiskThread (
            iMaxDiskStreams,
            ((pAudioOutputDevice->MaxSamplesPerCycle() << CONFIG_MAX_PITCH) << 1) + 6, //FIXME: assuming stereo
            &instruments
        );
    }

    void Engine::TriggerNewVoices (
        LinuxSampler::EngineChannel* pEngineChannel,
        RTList<Event>::Iterator&     itNoteOnEvent,
        bool                         HandleKeyGroupConflicts
    ) {
        EngineChannel* pChannel = static_cast<EngineChannel*>(pEngineChannel);
        ::sfz::Query q;
        q.chan        = pChannel->MidiChannel();
        q.key         = itNoteOnEvent->Param.Note.Key;
        q.vel         = itNoteOnEvent->Param.Note.Velocity;
        q.bend        = pChannel->Pitch;
        q.bpm         = 0;
        q.chanaft     = pChannel->ControllerTable[128];
        q.polyaft     = 0;
        q.prog        = 0;
        q.rand        = Random();
        q.cc          = pChannel->ControllerTable;
        q.timer       = 0;
        q.sw          = pChannel->PressedKeys;
        q.last_sw_key = pChannel->LastKeySwitch;
        q.prev_sw_key = pChannel->LastKey;
        q.trig        = TRIGGER_ATTACK |
            ((pChannel->LastKey != -1 &&
              pChannel->PressedKeys[pChannel->LastKey] &&
              pChannel->LastKey != q.key) ?
             TRIGGER_LEGATO : TRIGGER_FIRST);

        q.search(pChannel->pInstrument);

        int i = 0;
        while (::sfz::Region* region = q.next()) {
            if (!RegionSuspended(region)) {
                itNoteOnEvent->Param.Note.pRegion = region;
                LaunchVoice(pChannel, itNoteOnEvent, i, false, true, HandleKeyGroupConflicts);
            }
            i++;
        }
    }

    void Engine::TriggerReleaseVoices (
        LinuxSampler::EngineChannel*  pEngineChannel,
        RTList<Event>::Iterator&      itNoteOffEvent
    ) {
        EngineChannel* pChannel = static_cast<EngineChannel*>(pEngineChannel);
        ::sfz::Query q;
        q.chan        = pChannel->MidiChannel();
        q.key         = itNoteOffEvent->Param.Note.Key;

        // MIDI note-on velocity is used instead of note-off velocity
        q.vel         = pChannel->pMIDIKeyInfo[q.key].Velocity;
        itNoteOffEvent->Param.Note.Velocity = q.vel;

        q.bend        = pChannel->Pitch;
        q.bpm         = 0;
        q.chanaft     = pChannel->ControllerTable[128];
        q.polyaft     = 0;
        q.prog        = 0;
        q.rand        = Random();
        q.cc          = pChannel->ControllerTable;
        q.timer       = 0;
        q.sw          = pChannel->PressedKeys;
        q.last_sw_key = pChannel->LastKeySwitch;
        q.prev_sw_key = pChannel->LastKey;
        q.trig        = TRIGGER_RELEASE;

        q.search(pChannel->pInstrument);

        // now launch the required amount of voices
        int i = 0;
        while (::sfz::Region* region = q.next()) {
            itNoteOffEvent->Param.Note.pRegion = region;
            LaunchVoice(pChannel, itNoteOffEvent, i, true, false, false); //FIXME: for the moment we don't perform voice stealing for release triggered samples
            i++;
        }
    }

    Pool<Voice>::Iterator Engine::LaunchVoice (
        LinuxSampler::EngineChannel*  pEngineChannel,
        Pool<Event>::Iterator&        itNoteOnEvent,
        int                           iLayer,
        bool                          ReleaseTriggerVoice,
        bool                          VoiceStealing,
        bool                          HandleKeyGroupConflicts
    ) {
        EngineChannel* pChannel = static_cast<EngineChannel*>(pEngineChannel);
        int key = itNoteOnEvent->Param.Note.Key;
        EngineChannel::MidiKey* pKey  = &pChannel->pMIDIKeyInfo[key];
        Voice::type_t VoiceType = (ReleaseTriggerVoice) ? Voice::type_release_trigger : (!iLayer) ? Voice::type_release_trigger_required : Voice::type_normal;

        Pool<Voice>::Iterator itNewVoice;
        ::sfz::Region* pRgn = static_cast< ::sfz::Region*>(itNoteOnEvent->Param.Note.pRegion);

        // no need to process if sample is silent
        if (!pRgn->GetSample() || !pRgn->GetSample()->GetTotalFrameCount()) return Pool<Voice>::Iterator();

        // only mark the first voice of a layered voice (group) to be in a
        // key group, so the layered voices won't kill each other
        int iKeyGroup = (iLayer == 0 && !ReleaseTriggerVoice) ? pRgn->group : 0;
        if (HandleKeyGroupConflicts) pChannel->HandleKeyGroupConflicts(iKeyGroup, itNoteOnEvent, pRgn->off_mode == ::sfz::OFF_NORMAL);

        // allocate a new voice for the key
        itNewVoice = pKey->pActiveVoices->allocAppend();
        int res = InitNewVoice (
                pChannel, pRgn, itNoteOnEvent, VoiceType, iLayer,
                iKeyGroup, ReleaseTriggerVoice, VoiceStealing, itNewVoice
        );
        if (!res) return itNewVoice;

        // return if this is a release triggered voice and there is no
        // releasetrigger dimension (could happen if an instrument
        // change has occured between note on and off)
        //if (ReleaseTriggerVoice && VoiceType != Voice::type_release_trigger) return Pool<Voice>::Iterator();

        return Pool<Voice>::Iterator(); // no free voice or error
    }

    bool Engine::DiskStreamSupported() {
        return true;
    }

    String Engine::Description() {
        return "SFZ Format Engine";
    }

    String Engine::Version() {
        String s = "$Revision: 1.9 $";
        return s.substr(11, s.size() - 13); // cut dollar signs, spaces and CVS macro keyword
    }

}} // namespace LinuxSampler::sfz
