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

#include <sstream>
#include "DiskThread.h"
#include "Voice.h"
#include "EGADSR.h"
#include "../EngineFactory.h"

#include "Engine.h"

#if defined(__APPLE__)
# include <stdlib.h>
#else
# include <malloc.h>
#endif

namespace LinuxSampler { namespace gig {

    InstrumentResourceManager Engine::instruments;

    std::map<AudioOutputDevice*,Engine*> Engine::engines;

    /**
     * Get a gig::Engine object for the given gig::EngineChannel and the
     * given AudioOutputDevice. All engine channels which are connected to
     * the same audio output device will use the same engine instance. This
     * method will be called by a gig::EngineChannel whenever it's
     * connecting to a audio output device.
     *
     * @param pChannel - engine channel which acquires an engine object
     * @param pDevice  - the audio output device \a pChannel is connected to
     */
    Engine* Engine::AcquireEngine(LinuxSampler::gig::EngineChannel* pChannel, AudioOutputDevice* pDevice) {
        Engine* pEngine = NULL;
        // check if there's already an engine for the given audio output device
        if (engines.count(pDevice)) {
            dmsg(4,("Using existing gig::Engine.\n"));
            pEngine = engines[pDevice];
        } else { // create a new engine (and disk thread) instance for the given audio output device
            dmsg(4,("Creating new gig::Engine.\n"));
            pEngine = (Engine*) EngineFactory::Create("gig");
            pEngine->Connect(pDevice);
            engines[pDevice] = pEngine;
        }
        // register engine channel to the engine instance
        pEngine->engineChannels.push_back(pChannel);
        dmsg(4,("This gig::Engine has now %d EngineChannels.\n",pEngine->engineChannels.size()));
        return pEngine;
    }

    /**
     * Once an engine channel is disconnected from an audio output device,
     * it wil immediately call this method to unregister itself from the
     * engine instance and if that engine instance is not used by any other
     * engine channel anymore, then that engine instance will be destroyed.
     *
     * @param pChannel - engine channel which wants to disconnect from it's
     *                   engine instance
     * @param pDevice  - audio output device \a pChannel was connected to
     */
    void Engine::FreeEngine(LinuxSampler::gig::EngineChannel* pChannel, AudioOutputDevice* pDevice) {
        dmsg(4,("Disconnecting EngineChannel from gig::Engine.\n"));
        Engine* pEngine = engines[pDevice];
        // unregister EngineChannel from the Engine instance
        pEngine->engineChannels.remove(pChannel);
        // if the used Engine instance is not used anymore, then destroy it
        if (pEngine->engineChannels.empty()) {
            pDevice->Disconnect(pEngine);
            engines.erase(pDevice);
            delete pEngine;
            dmsg(4,("Destroying gig::Engine.\n"));
        }
        else dmsg(4,("This gig::Engine has now %d EngineChannels.\n",pEngine->engineChannels.size()));
    }

    Engine::Engine() {
        pAudioOutputDevice = NULL;
        pDiskThread        = NULL;
        pEventGenerator    = NULL;
        pSysexBuffer       = new RingBuffer<uint8_t>(SYSEX_BUFFER_SIZE, 0);
        pEventQueue        = new RingBuffer<Event>(MAX_EVENTS_PER_FRAGMENT, 0);
        pEventPool         = new Pool<Event>(MAX_EVENTS_PER_FRAGMENT);
        pVoicePool         = new Pool<Voice>(MAX_AUDIO_VOICES);
        pVoiceStealingQueue = new RTList<Event>(pEventPool);
        pEvents            = new RTList<Event>(pEventPool);
        pCCEvents          = new RTList<Event>(pEventPool);

        for (uint i = 0; i < Event::destination_count; i++) {
            pSynthesisEvents[i] = new RTList<Event>(pEventPool);
        }
        for (RTList<Voice>::Iterator iterVoice = pVoicePool->allocAppend(); iterVoice == pVoicePool->last(); iterVoice = pVoicePool->allocAppend()) {
            iterVoice->SetEngine(this);
        }
        pVoicePool->clear();

        pSynthesisParameters[0] = NULL; // we allocate when an audio device is connected
        pBasicFilterParameters  = NULL;
        pMainFilterParameters   = NULL;

        ResetInternal();
    }

    Engine::~Engine() {
        if (pDiskThread) {
            dmsg(1,("Stopping disk thread..."));
            pDiskThread->StopThread();
            delete pDiskThread;
            dmsg(1,("OK\n"));
        }
        for (uint i = 0; i < Event::destination_count; i++) {
            if (pSynthesisEvents[i]) delete pSynthesisEvents[i];
        }
        if (pEvents)     delete pEvents;
        if (pCCEvents)   delete pCCEvents;
        if (pEventQueue) delete pEventQueue;
        if (pEventPool)  delete pEventPool;
        if (pVoicePool) {
            pVoicePool->clear();
            delete pVoicePool;
        }
        if (pEventGenerator) delete pEventGenerator;
        if (pMainFilterParameters) delete[] pMainFilterParameters;
        if (pBasicFilterParameters) delete[] pBasicFilterParameters;
        if (pSynthesisParameters[0]) free(pSynthesisParameters[0]);
        if (pVoiceStealingQueue) delete pVoiceStealingQueue;
        if (pSysexBuffer) delete pSysexBuffer;
        EngineFactory::Destroy(this);
    }

    void Engine::Enable() {
        dmsg(3,("gig::Engine: enabling\n"));
        EngineDisabled.PushAndUnlock(false, 2); // set condition object 'EngineDisabled' to false (wait max. 2s)
        dmsg(3,("gig::Engine: enabled (val=%d)\n", EngineDisabled.GetUnsafe()));
    }

    void Engine::Disable() {
        dmsg(3,("gig::Engine: disabling\n"));
        bool* pWasDisabled = EngineDisabled.PushAndUnlock(true, 2); // wait max. 2s
        if (!pWasDisabled) dmsg(3,("gig::Engine warning: Timeout waiting to disable engine.\n"));
    }

    void Engine::DisableAndLock() {
        dmsg(3,("gig::Engine: disabling\n"));
        bool* pWasDisabled = EngineDisabled.Push(true, 2); // wait max. 2s
        if (!pWasDisabled) dmsg(3,("gig::Engine warning: Timeout waiting to disable engine.\n"));
    }

    /**
     *  Reset all voices and disk thread and clear input event queue and all
     *  control and status variables.
     */
    void Engine::Reset() {
        DisableAndLock();
        ResetInternal();
        Enable();
    }

    /**
     *  Reset all voices and disk thread and clear input event queue and all
     *  control and status variables. This method is not thread safe!
     */
    void Engine::ResetInternal() {
        ActiveVoiceCount    = 0;
        ActiveVoiceCountMax = 0;

        // reset voice stealing parameters
        pVoiceStealingQueue->clear();

        // reset to normal chromatic scale (means equal temper)
        memset(&ScaleTuning[0], 0x00, 12);

        // reset all voices
        for (RTList<Voice>::Iterator iterVoice = pVoicePool->allocAppend(); iterVoice == pVoicePool->last(); iterVoice = pVoicePool->allocAppend()) {
            iterVoice->Reset();
        }
        pVoicePool->clear();

        // reset disk thread
        if (pDiskThread) pDiskThread->Reset();

        // delete all input events
        pEventQueue->init();
    }

    void Engine::Connect(AudioOutputDevice* pAudioOut) {
        pAudioOutputDevice = pAudioOut;

        ResetInternal();

        // inform audio driver for the need of two channels
        try {
            pAudioOutputDevice->AcquireChannels(2); // gig engine only stereo
        }
        catch (AudioOutputException e) {
            String msg = "Audio output device unable to provide 2 audio channels, cause: " + e.Message();
            throw LinuxSamplerException(msg);
        }

        this->MaxSamplesPerCycle      = pAudioOutputDevice->MaxSamplesPerCycle();
        this->SampleRate              = pAudioOutputDevice->SampleRate();

        // FIXME: audio drivers with varying fragment sizes might be a problem here
        MaxFadeOutPos = MaxSamplesPerCycle - int(double(SampleRate) * EG_MIN_RELEASE_TIME) - 1;
        if (MaxFadeOutPos < 0)
            throw LinuxSamplerException("EG_MIN_RELEASE_TIME in EGADSR.h too big for current audio fragment size / sampling rate!");

        // (re)create disk thread
        if (this->pDiskThread) {
            dmsg(1,("Stopping disk thread..."));
            this->pDiskThread->StopThread();
            delete this->pDiskThread;
            dmsg(1,("OK\n"));
        }
        this->pDiskThread = new DiskThread(((pAudioOut->MaxSamplesPerCycle() << MAX_PITCH) << 1) + 6); //FIXME: assuming stereo
        if (!pDiskThread) {
            dmsg(0,("gig::Engine  new diskthread = NULL\n"));
            exit(EXIT_FAILURE);
        }

        for (RTList<Voice>::Iterator iterVoice = pVoicePool->allocAppend(); iterVoice == pVoicePool->last(); iterVoice = pVoicePool->allocAppend()) {
            iterVoice->pDiskThread = this->pDiskThread;
            dmsg(3,("d"));
        }
        pVoicePool->clear();

        // (re)create event generator
        if (pEventGenerator) delete pEventGenerator;
        pEventGenerator = new EventGenerator(pAudioOut->SampleRate());

        // (re)allocate synthesis parameter matrix
        if (pSynthesisParameters[0]) free(pSynthesisParameters[0]);

        #if defined(__APPLE__)
        pSynthesisParameters[0] = (float *) malloc(Event::destination_count * sizeof(float) * pAudioOut->MaxSamplesPerCycle());
        #else
        pSynthesisParameters[0] = (float *) memalign(16,(Event::destination_count * sizeof(float) * pAudioOut->MaxSamplesPerCycle()));
        #endif
        for (int dst = 1; dst < Event::destination_count; dst++)
            pSynthesisParameters[dst] = pSynthesisParameters[dst - 1] + pAudioOut->MaxSamplesPerCycle();

        // (re)allocate biquad filter parameter sequence
        if (pBasicFilterParameters) delete[] pBasicFilterParameters;
        if (pMainFilterParameters)  delete[] pMainFilterParameters;
        pBasicFilterParameters = new biquad_param_t[pAudioOut->MaxSamplesPerCycle()];
        pMainFilterParameters  = new biquad_param_t[pAudioOut->MaxSamplesPerCycle()];

        dmsg(1,("Starting disk thread..."));
        pDiskThread->StartThread();
        dmsg(1,("OK\n"));

        for (RTList<Voice>::Iterator iterVoice = pVoicePool->allocAppend(); iterVoice == pVoicePool->last(); iterVoice = pVoicePool->allocAppend()) {
            if (!iterVoice->pDiskThread) {
                dmsg(0,("Engine -> voice::trigger: !pDiskThread\n"));
                exit(EXIT_FAILURE);
            }
        }
    }

    void Engine::ClearEventLists() {
        pEvents->clear();
        pCCEvents->clear();
        for (uint i = 0; i < Event::destination_count; i++) {
            pSynthesisEvents[i]->clear();
        }
    }

    /**
     * Copy all events from the given input queue buffer to the engine's
     * internal event list. This will be done at the beginning of each audio
     * cycle (that is each RenderAudio() call) to get all events which have
     * to be processed in the current audio cycle. Each EngineChannel has
     * it's own input event queue for the common channel specific events
     * (like NoteOn, NoteOff and ControlChange events). Beside that, the
     * engine also has a input event queue for global events (usually SysEx
     * message).
     *
     * @param pEventQueue - input event buffer to read from
     * @param Samples     - number of sample points to be processed in the
     *                      current audio cycle
     */
    void Engine::ImportEvents(RingBuffer<Event>* pEventQueue, uint Samples) {
        RingBuffer<Event>::NonVolatileReader eventQueueReader = pEventQueue->get_non_volatile_reader();
        Event* pEvent;
        while (true) {
            // get next event from input event queue
            if (!(pEvent = eventQueueReader.pop())) break;
            // if younger event reached, ignore that and all subsequent ones for now
            if (pEvent->FragmentPos() >= Samples) {
                eventQueueReader--;
                dmsg(2,("Younger Event, pos=%d ,Samples=%d!\n",pEvent->FragmentPos(),Samples));
                pEvent->ResetFragmentPos();
                break;
            }
            // copy event to internal event list
            if (pEvents->poolIsEmpty()) {
                dmsg(1,("Event pool emtpy!\n"));
                break;
            }
            *pEvents->allocAppend() = *pEvent;
        }
        eventQueueReader.free(); // free all copied events from input queue
    }

    /**
     *  Let this engine proceed to render the given amount of sample points. The
     *  calculated audio data of all voices of this engine will be placed into
     *  the engine's audio sum buffer which has to be copied and eventually be
     *  converted to the appropriate value range by the audio output class (e.g.
     *  AlsaIO or JackIO) right after.
     *
     *  @param Samples - number of sample points to be rendered
     *  @returns       0 on success
     */
    int Engine::RenderAudio(uint Samples) {
        dmsg(5,("RenderAudio(Samples=%d)\n", Samples));

        // return if engine disabled
        if (EngineDisabled.Pop()) {
            dmsg(5,("gig::Engine: engine disabled (val=%d)\n",EngineDisabled.GetUnsafe()));
            return 0;
        }

        // update time of start and end of this audio fragment (as events' time stamps relate to this)
        pEventGenerator->UpdateFragmentTime(Samples);

        // empty the engine's event lists for the new fragment
        ClearEventLists();

        // get all events from the engine's global input event queue which belong to the current fragment
        // (these are usually just SysEx messages)
        ImportEvents(this->pEventQueue, Samples);

        // process engine global events (these are currently only MIDI System Exclusive messages)
        {
            RTList<Event>::Iterator itEvent = pEvents->first();
            RTList<Event>::Iterator end     = pEvents->end();
            for (; itEvent != end; ++itEvent) {
                switch (itEvent->Type) {
                    case Event::type_sysex:
                        dmsg(5,("Engine: Sysex received\n"));
                        ProcessSysex(itEvent);
                        break;
                }
            }
        }

        // reset internal voice counter (just for statistic of active voices)
        ActiveVoiceCountTemp = 0;

        // render audio for all engine channels
        // TODO: should we make voice stealing engine globally? unfortunately this would mean other disadvantages so I left voice stealing in the engine channel space for now
        {
            std::list<EngineChannel*>::iterator itChannel = engineChannels.begin();
            std::list<EngineChannel*>::iterator end       = engineChannels.end();
            for (; itChannel != end; itChannel++) {
                if (!(*itChannel)->pInstrument) continue; // ignore if no instrument loaded
                RenderAudio(*itChannel, Samples);
            }
        }

        // just some statistics about this engine instance
        ActiveVoiceCount = ActiveVoiceCountTemp;
        if (ActiveVoiceCount > ActiveVoiceCountMax) ActiveVoiceCountMax = ActiveVoiceCount;

        return 0;
    }

    void Engine::RenderAudio(EngineChannel* pEngineChannel, uint Samples) {
        // empty the engine's event lists for the new fragment
        ClearEventLists();
        // empty the engine channel's, MIDI key specific event lists
        {
            RTList<uint>::Iterator iuiKey = pEngineChannel->pActiveKeys->first();
            RTList<uint>::Iterator end    = pEngineChannel->pActiveKeys->end();
            for(; iuiKey != end; ++iuiKey) {
                pEngineChannel->pMIDIKeyInfo[*iuiKey].pEvents->clear(); // free all events on the key
            }
        }


        // get all events from the engine channels's input event queue which belong to the current fragment
        // (these are the common events like NoteOn, NoteOff, ControlChange, etc.)
        ImportEvents(pEngineChannel->pEventQueue, Samples);


        // process events
        {
            RTList<Event>::Iterator itEvent = pEvents->first();
            RTList<Event>::Iterator end     = pEvents->end();
            for (; itEvent != end; ++itEvent) {
                switch (itEvent->Type) {
                    case Event::type_note_on:
                        dmsg(5,("Engine: Note on received\n"));
                        ProcessNoteOn((EngineChannel*)itEvent->pEngineChannel, itEvent);
                        break;
                    case Event::type_note_off:
                        dmsg(5,("Engine: Note off received\n"));
                        ProcessNoteOff((EngineChannel*)itEvent->pEngineChannel, itEvent);
                        break;
                    case Event::type_control_change:
                        dmsg(5,("Engine: MIDI CC received\n"));
                        ProcessControlChange((EngineChannel*)itEvent->pEngineChannel, itEvent);
                        break;
                    case Event::type_pitchbend:
                        dmsg(5,("Engine: Pitchbend received\n"));
                        ProcessPitchbend((EngineChannel*)itEvent->pEngineChannel, itEvent);
                        break;
                }
            }
        }


        // render audio from all active voices
        {
            RTList<uint>::Iterator iuiKey = pEngineChannel->pActiveKeys->first();
            RTList<uint>::Iterator end    = pEngineChannel->pActiveKeys->end();
            while (iuiKey != end) { // iterate through all active keys
                midi_key_info_t* pKey = &pEngineChannel->pMIDIKeyInfo[*iuiKey];
                ++iuiKey;

                RTList<Voice>::Iterator itVoice     = pKey->pActiveVoices->first();
                RTList<Voice>::Iterator itVoicesEnd = pKey->pActiveVoices->end();
                for (; itVoice != itVoicesEnd; ++itVoice) { // iterate through all voices on this key
                    // now render current voice
                    itVoice->Render(Samples);
                    if (itVoice->IsActive()) ActiveVoiceCountTemp++; // still active
                    else { // voice reached end, is now inactive
                        FreeVoice(pEngineChannel, itVoice); // remove voice from the list of active voices
                    }
                }
            }
        }


        // now render all postponed voices from voice stealing
        {
            RTList<Event>::Iterator itVoiceStealEvent = pVoiceStealingQueue->first();
            RTList<Event>::Iterator end               = pVoiceStealingQueue->end();
            for (; itVoiceStealEvent != end; ++itVoiceStealEvent) {
                Pool<Voice>::Iterator itNewVoice =
                    LaunchVoice(pEngineChannel, itVoiceStealEvent, itVoiceStealEvent->Param.Note.Layer, itVoiceStealEvent->Param.Note.ReleaseTrigger, false);
                if (itNewVoice) {
                    for (; itNewVoice; itNewVoice = itNewVoice->itChildVoice) {
                        itNewVoice->Render(Samples);
                        if (itNewVoice->IsActive()) ActiveVoiceCountTemp++; // still active
                        else { // voice reached end, is now inactive
                            FreeVoice(pEngineChannel, itNewVoice); // remove voice from the list of active voices
                        }
                    }
                }
                else dmsg(1,("gig::Engine: ERROR, voice stealing didn't work out!\n"));
            }
        }
        // reset voice stealing for the new fragment
        pVoiceStealingQueue->clear();
        pEngineChannel->itLastStolenVoice = RTList<Voice>::Iterator();
        pEngineChannel->iuiLastStolenKey  = RTList<uint>::Iterator();


        // free all keys which have no active voices left
        {
            RTList<uint>::Iterator iuiKey = pEngineChannel->pActiveKeys->first();
            RTList<uint>::Iterator end    = pEngineChannel->pActiveKeys->end();
            while (iuiKey != end) { // iterate through all active keys
                midi_key_info_t* pKey = &pEngineChannel->pMIDIKeyInfo[*iuiKey];
                ++iuiKey;
                if (pKey->pActiveVoices->isEmpty()) FreeKey(pEngineChannel, pKey);
                #if DEVMODE
                else { // FIXME: should be removed before the final release (purpose: just a sanity check for debugging)
                    RTList<Voice>::Iterator itVoice     = pKey->pActiveVoices->first();
                    RTList<Voice>::Iterator itVoicesEnd = pKey->pActiveVoices->end();
                    for (; itVoice != itVoicesEnd; ++itVoice) { // iterate through all voices on this key
                        if (itVoice->itKillEvent) {
                            dmsg(1,("gig::Engine: ERROR, killed voice survived !!!\n"));
                        }
                    }
                }
                #endif // DEVMODE
            }
        }
    }

    /**
     *  Will be called by the MIDI input device whenever a MIDI system
     *  exclusive message has arrived.
     *
     *  @param pData - pointer to sysex data
     *  @param Size  - lenght of sysex data (in bytes)
     */
    void Engine::SendSysex(void* pData, uint Size) {
        Event event             = pEventGenerator->CreateEvent();
        event.Type              = Event::type_sysex;
        event.Param.Sysex.Size  = Size;
        event.pEngineChannel    = NULL; // as Engine global event
        if (pEventQueue->write_space() > 0) {
            if (pSysexBuffer->write_space() >= Size) {
                // copy sysex data to input buffer
                uint toWrite = Size;
                uint8_t* pPos = (uint8_t*) pData;
                while (toWrite) {
                    const uint writeNow = RTMath::Min(toWrite, pSysexBuffer->write_space_to_end());
                    pSysexBuffer->write(pPos, writeNow);
                    toWrite -= writeNow;
                    pPos    += writeNow;

                }
                // finally place sysex event into input event queue
                pEventQueue->push(&event);
            }
            else dmsg(1,("Engine: Sysex message too large (%d byte) for input buffer (%d byte)!",Size,SYSEX_BUFFER_SIZE));
        }
        else dmsg(1,("Engine: Input event queue full!"));
    }

    /**
     *  Assigns and triggers a new voice for the respective MIDI key.
     *
     *  @param pEngineChannel - engine channel on which this event occured on
     *  @param itNoteOnEvent - key, velocity and time stamp of the event
     */
    void Engine::ProcessNoteOn(EngineChannel* pEngineChannel, Pool<Event>::Iterator& itNoteOnEvent) {

        const int key = itNoteOnEvent->Param.Note.Key;

        // Change key dimension value if key is in keyswitching area
        {
            const ::gig::Instrument* pInstrument = pEngineChannel->pInstrument;
            if (key >= pInstrument->DimensionKeyRange.low && key <= pInstrument->DimensionKeyRange.high)
                pEngineChannel->CurrentKeyDimension = ((key - pInstrument->DimensionKeyRange.low) * 128) /
                    (pInstrument->DimensionKeyRange.high - pInstrument->DimensionKeyRange.low + 1);
        }

        midi_key_info_t* pKey = &pEngineChannel->pMIDIKeyInfo[key];

        pKey->KeyPressed = true; // the MIDI key was now pressed down

        // cancel release process of voices on this key if needed
        if (pKey->Active && !pEngineChannel->SustainPedal) {
            RTList<Event>::Iterator itCancelReleaseEvent = pKey->pEvents->allocAppend();
            if (itCancelReleaseEvent) {
                *itCancelReleaseEvent = *itNoteOnEvent;                  // copy event
                itCancelReleaseEvent->Type = Event::type_cancel_release; // transform event type
            }
            else dmsg(1,("Event pool emtpy!\n"));
        }

        // move note on event to the key's own event list
        RTList<Event>::Iterator itNoteOnEventOnKeyList = itNoteOnEvent.moveToEndOf(pKey->pEvents);

        // allocate and trigger a new voice for the key
        LaunchVoice(pEngineChannel, itNoteOnEventOnKeyList, 0, false, true);

        pKey->RoundRobinIndex++;
    }

    /**
     *  Releases the voices on the given key if sustain pedal is not pressed.
     *  If sustain is pressed, the release of the note will be postponed until
     *  sustain pedal will be released or voice turned inactive by itself (e.g.
     *  due to completion of sample playback).
     *
     *  @param pEngineChannel - engine channel on which this event occured on
     *  @param itNoteOffEvent - key, velocity and time stamp of the event
     */
    void Engine::ProcessNoteOff(EngineChannel* pEngineChannel, Pool<Event>::Iterator& itNoteOffEvent) {
        midi_key_info_t* pKey = &pEngineChannel->pMIDIKeyInfo[itNoteOffEvent->Param.Note.Key];

        pKey->KeyPressed = false; // the MIDI key was now released

        // release voices on this key if needed
        if (pKey->Active && !pEngineChannel->SustainPedal) {
            itNoteOffEvent->Type = Event::type_release; // transform event type
        }

        // move event to the key's own event list
        RTList<Event>::Iterator itNoteOffEventOnKeyList = itNoteOffEvent.moveToEndOf(pKey->pEvents);

        // spawn release triggered voice(s) if needed
        if (pKey->ReleaseTrigger) {
            LaunchVoice(pEngineChannel, itNoteOffEventOnKeyList, 0, true, false); //FIXME: for the moment we don't perform voice stealing for release triggered samples
            pKey->ReleaseTrigger = false;
        }
    }

    /**
     *  Moves pitchbend event from the general (input) event list to the pitch
     *  event list.
     *
     *  @param pEngineChannel - engine channel on which this event occured on
     *  @param itPitchbendEvent - absolute pitch value and time stamp of the event
     */
    void Engine::ProcessPitchbend(EngineChannel* pEngineChannel, Pool<Event>::Iterator& itPitchbendEvent) {
        pEngineChannel->Pitch = itPitchbendEvent->Param.Pitch.Pitch; // store current pitch value
        itPitchbendEvent.moveToEndOf(pSynthesisEvents[Event::destination_vco]);
    }

    /**
     *  Allocates and triggers a new voice. This method will usually be
     *  called by the ProcessNoteOn() method and by the voices itself
     *  (e.g. to spawn further voices on the same key for layered sounds).
     *
     *  @param pEngineChannel      - engine channel on which this event occured on
     *  @param itNoteOnEvent       - key, velocity and time stamp of the event
     *  @param iLayer              - layer index for the new voice (optional - only
     *                               in case of layered sounds of course)
     *  @param ReleaseTriggerVoice - if new voice is a release triggered voice
     *                               (optional, default = false)
     *  @param VoiceStealing       - if voice stealing should be performed
     *                               when there is no free voice
     *                               (optional, default = true)
     *  @returns pointer to new voice or NULL if there was no free voice or
     *           if the voice wasn't triggered (for example when no region is
     *           defined for the given key).
     */
    Pool<Voice>::Iterator Engine::LaunchVoice(EngineChannel* pEngineChannel, Pool<Event>::Iterator& itNoteOnEvent, int iLayer, bool ReleaseTriggerVoice, bool VoiceStealing) {
        midi_key_info_t* pKey = &pEngineChannel->pMIDIKeyInfo[itNoteOnEvent->Param.Note.Key];

        // allocate a new voice for the key
        Pool<Voice>::Iterator itNewVoice = pKey->pActiveVoices->allocAppend();
        if (itNewVoice) {
            // launch the new voice
            if (itNewVoice->Trigger(pEngineChannel, itNoteOnEvent, pEngineChannel->Pitch, pEngineChannel->pInstrument, iLayer, ReleaseTriggerVoice, VoiceStealing) < 0) {
                dmsg(4,("Voice not triggered\n"));
                pKey->pActiveVoices->free(itNewVoice);
            }
            else { // on success
                uint** ppKeyGroup = NULL;
                if (itNewVoice->KeyGroup) { // if this voice / key belongs to a key group
                    ppKeyGroup = &pEngineChannel->ActiveKeyGroups[itNewVoice->KeyGroup];
                    if (*ppKeyGroup) { // if there's already an active key in that key group
                        midi_key_info_t* pOtherKey = &pEngineChannel->pMIDIKeyInfo[**ppKeyGroup];
                        // kill all voices on the (other) key
                        RTList<Voice>::Iterator itVoiceToBeKilled = pOtherKey->pActiveVoices->first();
                        RTList<Voice>::Iterator end               = pOtherKey->pActiveVoices->end();
                        for (; itVoiceToBeKilled != end; ++itVoiceToBeKilled) {
                            if (itVoiceToBeKilled->Type != Voice::type_release_trigger) itVoiceToBeKilled->Kill(itNoteOnEvent);
                        }
                    }
                }
                if (!pKey->Active) { // mark as active key
                    pKey->Active = true;
                    pKey->itSelf = pEngineChannel->pActiveKeys->allocAppend();
                    *pKey->itSelf = itNoteOnEvent->Param.Note.Key;
                }
                if (itNewVoice->KeyGroup) {
                    *ppKeyGroup = &*pKey->itSelf; // put key as the (new) active key to its key group
                }
                if (itNewVoice->Type == Voice::type_release_trigger_required) pKey->ReleaseTrigger = true; // mark key for the need of release triggered voice(s)
                return itNewVoice; // success
            }
        }
        else if (VoiceStealing) {
            // first, get total amount of required voices (dependant on amount of layers)
            ::gig::Region* pRegion = pEngineChannel->pInstrument->GetRegion(itNoteOnEvent->Param.Note.Key);
            if (!pRegion) return Pool<Voice>::Iterator(); // nothing defined for this MIDI key, so no voice needed
            int voicesRequired = pRegion->Layers;

            // now steal the (remaining) amount of voices
            for (int i = iLayer; i < voicesRequired; i++)
                StealVoice(pEngineChannel, itNoteOnEvent);

            // put note-on event into voice-stealing queue, so it will be reprocessed after killed voice died
            RTList<Event>::Iterator itStealEvent = pVoiceStealingQueue->allocAppend();
            if (itStealEvent) {
                *itStealEvent = *itNoteOnEvent; // copy event
                itStealEvent->Param.Note.Layer = iLayer;
                itStealEvent->Param.Note.ReleaseTrigger = ReleaseTriggerVoice;
            }
            else dmsg(1,("Voice stealing queue full!\n"));
        }

        return Pool<Voice>::Iterator(); // no free voice or error
    }

    /**
     *  Will be called by LaunchVoice() method in case there are no free
     *  voices left. This method will select and kill one old voice for
     *  voice stealing and postpone the note-on event until the selected
     *  voice actually died.
     *
     *  @param pEngineChannel - engine channel on which this event occured on
     *  @param itNoteOnEvent - key, velocity and time stamp of the event
     */
    void Engine::StealVoice(EngineChannel* pEngineChannel, Pool<Event>::Iterator& itNoteOnEvent) {
        if (!pEventPool->poolIsEmpty()) {

            RTList<uint>::Iterator  iuiOldestKey;
            RTList<Voice>::Iterator itOldestVoice;

            // Select one voice for voice stealing
            switch (VOICE_STEAL_ALGORITHM) {

                // try to pick the oldest voice on the key where the new
                // voice should be spawned, if there is no voice on that
                // key, or no voice left to kill there, then procceed with
                // 'oldestkey' algorithm
                case voice_steal_algo_keymask: {
                    midi_key_info_t* pOldestKey = &pEngineChannel->pMIDIKeyInfo[itNoteOnEvent->Param.Note.Key];
                    if (pEngineChannel->itLastStolenVoice) {
                        itOldestVoice = pEngineChannel->itLastStolenVoice;
                        ++itOldestVoice;
                    }
                    else { // no voice stolen in this audio fragment cycle yet
                        itOldestVoice = pOldestKey->pActiveVoices->first();
                    }
                    if (itOldestVoice) {
                        iuiOldestKey = pOldestKey->itSelf;
                        break; // selection succeeded
                    }
                } // no break - intentional !

                // try to pick the oldest voice on the oldest active key
                // (caution: must stay after 'keymask' algorithm !)
                case voice_steal_algo_oldestkey: {
                    if (pEngineChannel->itLastStolenVoice) {
                        midi_key_info_t* pOldestKey = &pEngineChannel->pMIDIKeyInfo[*pEngineChannel->iuiLastStolenKey];
                        itOldestVoice = pEngineChannel->itLastStolenVoice;
                        ++itOldestVoice;
                        if (!itOldestVoice) {
                            iuiOldestKey = pEngineChannel->iuiLastStolenKey;
                            ++iuiOldestKey;
                            if (iuiOldestKey) {
                                midi_key_info_t* pOldestKey = &pEngineChannel->pMIDIKeyInfo[*iuiOldestKey];
                                itOldestVoice = pOldestKey->pActiveVoices->first();
                            }
                            else {
                                dmsg(1,("gig::Engine: Warning, too less voices, even for voice stealing! - Better recompile with higher MAX_AUDIO_VOICES.\n"));
                                return;
                            }
                        }
                        else iuiOldestKey = pEngineChannel->iuiLastStolenKey;
                    }
                    else { // no voice stolen in this audio fragment cycle yet
                        iuiOldestKey = pEngineChannel->pActiveKeys->first();
                        midi_key_info_t* pOldestKey = &pEngineChannel->pMIDIKeyInfo[*iuiOldestKey];
                        itOldestVoice = pOldestKey->pActiveVoices->first();
                    }
                    break;
                }

                // don't steal anything
                case voice_steal_algo_none:
                default: {
                    dmsg(1,("No free voice (voice stealing disabled)!\n"));
                    return;
                }
            }

            //FIXME: can be removed, just a sanity check for debugging
            if (!itOldestVoice->IsActive()) dmsg(1,("gig::Engine: ERROR, tried to steal a voice which was not active !!!\n"));

            // now kill the selected voice
            itOldestVoice->Kill(itNoteOnEvent);
            // remember which voice on which key we stole, so we can simply proceed for the next voice stealing
            pEngineChannel->itLastStolenVoice = itOldestVoice;
            pEngineChannel->iuiLastStolenKey = iuiOldestKey;
        }
        else dmsg(1,("Event pool emtpy!\n"));
    }

    /**
     *  Removes the given voice from the MIDI key's list of active voices.
     *  This method will be called when a voice went inactive, e.g. because
     *  it finished to playback its sample, finished its release stage or
     *  just was killed.
     *
     *  @param pEngineChannel - engine channel on which this event occured on
     *  @param itVoice - points to the voice to be freed
     */
    void Engine::FreeVoice(EngineChannel* pEngineChannel, Pool<Voice>::Iterator& itVoice) {
        if (itVoice) {
            midi_key_info_t* pKey = &pEngineChannel->pMIDIKeyInfo[itVoice->MIDIKey];

            uint keygroup = itVoice->KeyGroup;

            // free the voice object
            pVoicePool->free(itVoice);

            // if no other voices left and member of a key group, remove from key group
            if (pKey->pActiveVoices->isEmpty() && keygroup) {
                uint** ppKeyGroup = &pEngineChannel->ActiveKeyGroups[keygroup];
                if (*ppKeyGroup == &*pKey->itSelf) *ppKeyGroup = NULL; // remove key from key group
            }
        }
        else std::cerr << "Couldn't release voice! (!itVoice)\n" << std::flush;
    }

    /**
     *  Called when there's no more voice left on a key, this call will
     *  update the key info respectively.
     *
     *  @param pEngineChannel - engine channel on which this event occured on
     *  @param pKey - key which is now inactive
     */
    void Engine::FreeKey(EngineChannel* pEngineChannel, midi_key_info_t* pKey) {
        if (pKey->pActiveVoices->isEmpty()) {
            pKey->Active = false;
            pEngineChannel->pActiveKeys->free(pKey->itSelf); // remove key from list of active keys
            pKey->itSelf = RTList<uint>::Iterator();
            pKey->ReleaseTrigger = false;
            pKey->pEvents->clear();
            dmsg(3,("Key has no more voices now\n"));
        }
        else dmsg(1,("gig::Engine: Oops, tried to free a key which contains voices.\n"));
    }

    /**
     *  Reacts on supported control change commands (e.g. pitch bend wheel,
     *  modulation wheel, aftertouch).
     *
     *  @param pEngineChannel - engine channel on which this event occured on
     *  @param itControlChangeEvent - controller, value and time stamp of the event
     */
    void Engine::ProcessControlChange(EngineChannel* pEngineChannel, Pool<Event>::Iterator& itControlChangeEvent) {
        dmsg(4,("Engine::ContinuousController cc=%d v=%d\n", itControlChangeEvent->Param.CC.Controller, itControlChangeEvent->Param.CC.Value));

        switch (itControlChangeEvent->Param.CC.Controller) {
            case 7: { // volume
                //TODO: not sample accurate yet
                pEngineChannel->GlobalVolume = (float) itControlChangeEvent->Param.CC.Value / 127.0f;
                break;
            }
            case 10: { // panpot
                //TODO: not sample accurate yet
                const int pan = (int) itControlChangeEvent->Param.CC.Value - 64;
                pEngineChannel->GlobalPanLeft  = 1.0f - float(RTMath::Max(pan, 0)) /  63.0f;
                pEngineChannel->GlobalPanRight = 1.0f - float(RTMath::Min(pan, 0)) / -64.0f;
                break;
            }
            case 64: { // sustain
                if (itControlChangeEvent->Param.CC.Value >= 64 && !pEngineChannel->SustainPedal) {
                    dmsg(4,("PEDAL DOWN\n"));
                    pEngineChannel->SustainPedal = true;

                    // cancel release process of voices if necessary
                    RTList<uint>::Iterator iuiKey = pEngineChannel->pActiveKeys->first();
                    if (iuiKey) {
                        itControlChangeEvent->Type = Event::type_cancel_release; // transform event type
                        while (iuiKey) {
                            midi_key_info_t* pKey = &pEngineChannel->pMIDIKeyInfo[*iuiKey];
                            ++iuiKey;
                            if (!pKey->KeyPressed) {
                                RTList<Event>::Iterator itNewEvent = pKey->pEvents->allocAppend();
                                if (itNewEvent) *itNewEvent = *itControlChangeEvent; // copy event to the key's own event list
                                else dmsg(1,("Event pool emtpy!\n"));
                            }
                        }
                    }
                }
                if (itControlChangeEvent->Param.CC.Value < 64 && pEngineChannel->SustainPedal) {
                    dmsg(4,("PEDAL UP\n"));
                    pEngineChannel->SustainPedal = false;

                    // release voices if their respective key is not pressed
                    RTList<uint>::Iterator iuiKey = pEngineChannel->pActiveKeys->first();
                    if (iuiKey) {
                        itControlChangeEvent->Type = Event::type_release; // transform event type
                        while (iuiKey) {
                            midi_key_info_t* pKey = &pEngineChannel->pMIDIKeyInfo[*iuiKey];
                            ++iuiKey;
                            if (!pKey->KeyPressed) {
                                RTList<Event>::Iterator itNewEvent = pKey->pEvents->allocAppend();
                                if (itNewEvent) *itNewEvent = *itControlChangeEvent; // copy event to the key's own event list
                                else dmsg(1,("Event pool emtpy!\n"));
                            }
                        }
                    }
                }
                break;
            }
        }

        // update controller value in the engine's controller table
        pEngineChannel->ControllerTable[itControlChangeEvent->Param.CC.Controller] = itControlChangeEvent->Param.CC.Value;

        // move event from the unsorted event list to the control change event list
        itControlChangeEvent.moveToEndOf(pCCEvents);
    }

    /**
     *  Reacts on MIDI system exclusive messages.
     *
     *  @param itSysexEvent - sysex data size and time stamp of the sysex event
     */
    void Engine::ProcessSysex(Pool<Event>::Iterator& itSysexEvent) {
        RingBuffer<uint8_t>::NonVolatileReader reader = pSysexBuffer->get_non_volatile_reader();

        uint8_t exclusive_status, id;
        if (!reader.pop(&exclusive_status)) goto free_sysex_data;
        if (!reader.pop(&id))               goto free_sysex_data;
        if (exclusive_status != 0xF0)       goto free_sysex_data;

        switch (id) {
            case 0x41: { // Roland
                uint8_t device_id, model_id, cmd_id;
                if (!reader.pop(&device_id)) goto free_sysex_data;
                if (!reader.pop(&model_id))  goto free_sysex_data;
                if (!reader.pop(&cmd_id))    goto free_sysex_data;
                if (model_id != 0x42 /*GS*/) goto free_sysex_data;
                if (cmd_id != 0x12 /*DT1*/)  goto free_sysex_data;

                // command address
                uint8_t addr[3]; // 2 byte addr MSB, followed by 1 byte addr LSB)
                const RingBuffer<uint8_t>::NonVolatileReader checksum_reader = reader; // so we can calculate the check sum later
                if (reader.read(&addr[0], 3) != 3) goto free_sysex_data;
                if (addr[0] == 0x40 && addr[1] == 0x00) { // System Parameters
                }
                else if (addr[0] == 0x40 && addr[1] == 0x01) { // Common Parameters
                }
                else if (addr[0] == 0x40 && (addr[1] & 0xf0) == 0x10) { // Part Parameters (1)
                    switch (addr[3]) {
                        case 0x40: { // scale tuning
                            uint8_t scale_tunes[12]; // detuning of all 12 semitones of an octave
                            if (reader.read(&scale_tunes[0], 12) != 12) goto free_sysex_data;
                            uint8_t checksum;
                            if (!reader.pop(&checksum))                      goto free_sysex_data;
                            if (GSCheckSum(checksum_reader, 12) != checksum) goto free_sysex_data;
                            for (int i = 0; i < 12; i++) scale_tunes[i] -= 64;
                            AdjustScale((int8_t*) scale_tunes);
                            break;
                        }
                    }
                }
                else if (addr[0] == 0x40 && (addr[1] & 0xf0) == 0x20) { // Part Parameters (2)
                }
                else if (addr[0] == 0x41) { // Drum Setup Parameters
                }
                break;
            }
        }

        free_sysex_data: // finally free sysex data
        pSysexBuffer->increment_read_ptr(itSysexEvent->Param.Sysex.Size);
    }

    /**
     * Calculates the Roland GS sysex check sum.
     *
     * @param AddrReader - reader which currently points to the first GS
     *                     command address byte of the GS sysex message in
     *                     question
     * @param DataSize   - size of the GS message data (in bytes)
     */
    uint8_t Engine::GSCheckSum(const RingBuffer<uint8_t>::NonVolatileReader AddrReader, uint DataSize) {
        RingBuffer<uint8_t>::NonVolatileReader reader = AddrReader;
        uint bytes = 3 /*addr*/ + DataSize;
        uint8_t addr_and_data[bytes];
        reader.read(&addr_and_data[0], bytes);
        uint8_t sum = 0;
        for (uint i = 0; i < bytes; i++) sum += addr_and_data[i];
        return 128 - sum % 128;
    }

    /**
     * Allows to tune each of the twelve semitones of an octave.
     *
     * @param ScaleTunes - detuning of all twelve semitones (in cents)
     */
    void Engine::AdjustScale(int8_t ScaleTunes[12]) {
        memcpy(&this->ScaleTuning[0], &ScaleTunes[0], 12); //TODO: currently not sample accurate
    }

    /**
     * Initialize the parameter sequence for the modulation destination given by
     * by 'dst' with the constant value given by val.
     */
    void Engine::ResetSynthesisParameters(Event::destination_t dst, float val) {
        int maxsamples = pAudioOutputDevice->MaxSamplesPerCycle();
        float* m = &pSynthesisParameters[dst][0];
        for (int i = 0; i < maxsamples; i += 4) {
           m[i]   = val;
           m[i+1] = val;
           m[i+2] = val;
           m[i+3] = val;
        }
    }

    uint Engine::VoiceCount() {
        return ActiveVoiceCount;
    }

    uint Engine::VoiceCountMax() {
        return ActiveVoiceCountMax;
    }

    bool Engine::DiskStreamSupported() {
        return true;
    }

    uint Engine::DiskStreamCount() {
        return (pDiskThread) ? pDiskThread->ActiveStreamCount : 0;
    }

    uint Engine::DiskStreamCountMax() {
        return (pDiskThread) ? pDiskThread->ActiveStreamCountMax : 0;
    }

    String Engine::DiskStreamBufferFillBytes() {
        return pDiskThread->GetBufferFillBytes();
    }

    String Engine::DiskStreamBufferFillPercentage() {
        return pDiskThread->GetBufferFillPercentage();
    }

    String Engine::EngineName() {
        return "GigEngine";
    }

    String Engine::Description() {
        return "Gigasampler Engine";
    }

    String Engine::Version() {
        String s = "$Revision: 1.30 $";
        return s.substr(11, s.size() - 13); // cut dollar signs, spaces and CVS macro keyword
    }

}} // namespace LinuxSampler::gig
