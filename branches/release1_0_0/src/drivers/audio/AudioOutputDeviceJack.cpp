/***************************************************************************
 *                                                                         *
 *   LinuxSampler - modular, streaming capable sampler                     *
 *                                                                         *
 *   Copyright (C) 2003, 2004 by Benno Senoner and Christian Schoenebeck   *
 *   Copyright (C) 2005 - 2008 Christian Schoenebeck                       *
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

#include "AudioOutputDeviceJack.h"
#include "AudioOutputDeviceFactory.h"

#include <errno.h>

#if HAVE_JACK

#ifndef HAVE_JACK_CLIENT_NAME_SIZE
#define jack_client_name_size() 33
#endif

namespace LinuxSampler {

    /// number of currently existing JACK audio output devices in LinuxSampler
    static int existingJackDevices = 0;

// *************** AudioChannelJack::ParameterName ***************
// *

    AudioOutputDeviceJack::AudioChannelJack::ParameterName::ParameterName(AudioChannelJack* pChannel) : AudioChannel::ParameterName(ToString(pChannel->ChannelNr)) {
        this->pChannel = pChannel;
    }

    void AudioOutputDeviceJack::AudioChannelJack::ParameterName::OnSetValue(String s) {
        if (jack_port_set_name(pChannel->hJackPort, s.c_str())) throw AudioOutputException("Failed to rename JACK port");
    }



// *************** AudioChannelJack::ParameterJackBindings ***************
// *

    AudioOutputDeviceJack::AudioChannelJack::ParameterJackBindings::ParameterJackBindings(AudioChannelJack* pChannel) : DeviceRuntimeParameterStrings(std::vector<String>()) {
        this->pChannel = pChannel;
    }

    String AudioOutputDeviceJack::AudioChannelJack::ParameterJackBindings::Description() {
        return "Bindings to other JACK clients";
    }

    bool AudioOutputDeviceJack::AudioChannelJack::ParameterJackBindings::Fix() {
        return false;
    }

    std::vector<String> AudioOutputDeviceJack::AudioChannelJack::ParameterJackBindings::PossibilitiesAsString() {
        const char** pPortNames = jack_get_ports(pChannel->pDevice->hJackClient, NULL, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput);
        if (!pPortNames) return std::vector<String>();
        std::vector<String> result;
        for (int i = 0; pPortNames[i]; i++) result.push_back(pPortNames[i]);
        free(pPortNames);
        return result;
    }

    void AudioOutputDeviceJack::AudioChannelJack::ParameterJackBindings::OnSetValue(std::vector<String> vS) {
        String src_name = ((DeviceCreationParameterString*)pChannel->pDevice->Parameters["NAME"])->ValueAsString() + ":" +
                          ((DeviceRuntimeParameterString*)pChannel->Parameters["NAME"])->ValueAsString();
        // disconnect all current bindings first
        for (int i = 0; i < Bindings.size(); i++) {
            String dst_name = Bindings[i];
            int res = jack_disconnect(pChannel->pDevice->hJackClient, src_name.c_str(), dst_name.c_str());
        }
        // connect new bindings
        for (int i = 0; i < vS.size(); i++) {
            String dst_name = vS[i];
            int res = jack_connect(pChannel->pDevice->hJackClient, src_name.c_str(), dst_name.c_str());
            if (res == EEXIST) throw AudioOutputException("Jack: Connection to port '" + dst_name + "' already established");
            else if (res)      throw AudioOutputException("Jack: Cannot connect port '" + src_name + "' to port '" + dst_name + "'");
        }
        // remember bindings
        Bindings = vS;
    }

    String AudioOutputDeviceJack::AudioChannelJack::ParameterJackBindings::Name() {
        return "JACK_BINDINGS";
    }



// *************** AudioChannelJack ***************
// *

    AudioOutputDeviceJack::AudioChannelJack::AudioChannelJack(uint ChannelNr, AudioOutputDeviceJack* pDevice) throw (AudioOutputException) : AudioChannel(ChannelNr, CreateJackPort(ChannelNr, pDevice), pDevice->uiMaxSamplesPerCycle) {
        this->pDevice   = pDevice;
        this->ChannelNr = ChannelNr;
        delete Parameters["NAME"];
        Parameters["NAME"]          = new ParameterName(this);
        Parameters["JACK_BINDINGS"] = new ParameterJackBindings(this);
    }

    AudioOutputDeviceJack::AudioChannelJack::~AudioChannelJack() {
        //TODO: delete JACK port
    }

    float* AudioOutputDeviceJack::AudioChannelJack::CreateJackPort(uint ChannelNr, AudioOutputDeviceJack* pDevice) throw (AudioOutputException) {
        String port_id = ToString(ChannelNr);
        hJackPort = jack_port_register(pDevice->hJackClient, port_id.c_str(), JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
        if (!hJackPort) throw AudioOutputException("Jack: Cannot register Jack output port.");
        return (float*) jack_port_get_buffer(hJackPort, pDevice->uiMaxSamplesPerCycle);
    }



// *************** AudioOutputDeviceJack::ParameterName ***************
// *

    AudioOutputDeviceJack::ParameterName::ParameterName() : DeviceCreationParameterString() {
        InitWithDefault(); // use default name
    }

    AudioOutputDeviceJack::ParameterName::ParameterName(String s) throw (Exception) : DeviceCreationParameterString(s) {
    }

    String AudioOutputDeviceJack::ParameterName::Description() {
        return "Arbitrary JACK client name";
    }

    bool AudioOutputDeviceJack::ParameterName::Fix() {
        return true;
    }

    bool AudioOutputDeviceJack::ParameterName::Mandatory() {
        return false;
    }

    std::map<String,DeviceCreationParameter*> AudioOutputDeviceJack::ParameterName::DependsAsParameters() {
        return std::map<String,DeviceCreationParameter*>(); // no dependencies
    }

    std::vector<String> AudioOutputDeviceJack::ParameterName::PossibilitiesAsString(std::map<String,String> Parameters) {
        return std::vector<String>();
    }

    optional<String> AudioOutputDeviceJack::ParameterName::DefaultAsString(std::map<String,String> Parameters) {
        return (existingJackDevices) ? "LinuxSampler" + ToString(existingJackDevices) : "LinuxSampler";
    }

    void AudioOutputDeviceJack::ParameterName::OnSetValue(String s) throw (Exception) {
        // not possible, as parameter is fix
    }

    String AudioOutputDeviceJack::ParameterName::Name() {
        return "NAME";
    }



// *************** AudioOutputDeviceJack ***************
// *

    /**
     * Open and initialize connection to the JACK system.
     *
     * @param Parameters - optional parameters
     * @throws AudioOutputException  on error
     * @see AcquireChannels()
     */
    AudioOutputDeviceJack::AudioOutputDeviceJack(std::map<String,DeviceCreationParameter*> Parameters) : AudioOutputDevice(Parameters) {
        JackClient* pJackClient = JackClient::CreateAudio(((DeviceCreationParameterString*)Parameters["NAME"])->ValueAsString());
        existingJackDevices++;
        pJackClient->SetAudioOutputDevice(this);
        hJackClient = pJackClient->hJackClient;
        uiMaxSamplesPerCycle = jack_get_buffer_size(hJackClient);

        // create audio channels
        AcquireChannels(((DeviceCreationParameterInt*)Parameters["CHANNELS"])->ValueAsInt());

        // finally activate device if desired
        if (((DeviceCreationParameterBool*)Parameters["ACTIVE"])->ValueAsBool()) Play();
    }

    AudioOutputDeviceJack::~AudioOutputDeviceJack() {
        // destroy jack client if there is no midi device associated with it
        JackClient::ReleaseAudio(((DeviceCreationParameterString*)Parameters["NAME"])->ValueAsString());
        existingJackDevices--;
    }

    /**
     * This method should not be called directly! It will be called by
     * libjack to demand transmission of further sample points.
     */
    int AudioOutputDeviceJack::Process(uint Samples) {
        int res;
        if (csIsPlaying.Pop()) {
            // let all connected engines render 'Samples' sample points
            res = RenderAudio(Samples);
        }
        else {
            // playback stop by zeroing output buffer(s) and not calling connected sampler engines to render audio
            res = RenderSilence(Samples);
        }
        csIsPlaying.RttDone();
        return res;
    }

    void AudioOutputDeviceJack::Play() {
        csIsPlaying.PushAndUnlock(true);
    }

    bool AudioOutputDeviceJack::IsPlaying() {
        return csIsPlaying.GetUnsafe();
    }

    void AudioOutputDeviceJack::Stop() {
        csIsPlaying.PushAndUnlock(false);
    }

    AudioChannel* AudioOutputDeviceJack::CreateChannel(uint ChannelNr) {
        return new AudioChannelJack(ChannelNr, this);
    }

    uint AudioOutputDeviceJack::MaxSamplesPerCycle() {
        return jack_get_buffer_size(hJackClient);
    }

    uint AudioOutputDeviceJack::SampleRate() {
        return jack_get_sample_rate(hJackClient);
    }

    String AudioOutputDeviceJack::Name() {
        return "JACK";
    }

    String AudioOutputDeviceJack::Driver() {
        return Name();
    }

    String AudioOutputDeviceJack::Description() {
        return "JACK Audio Connection Kit";
    }

    String AudioOutputDeviceJack::Version() {
       String s = "$Revision: 1.25 $";
       return s.substr(11, s.size() - 13); // cut dollar signs, spaces and CVS macro keyword
    }



// *************** JackClient ***************
// *

    // libjack callback functions

    int linuxsampler_libjack_process_callback(jack_nframes_t nframes, void* arg) {
        return static_cast<JackClient*>(arg)->Process(nframes);
    }

    void linuxsampler_libjack_shutdown_callback(void* arg) {
        static_cast<JackClient*>(arg)->Stop();
        fprintf(stderr, "Jack: Jack server shutdown, exiting.\n");
    }

    std::map<String, JackClient*> JackClient::Clients;

    int JackClient::Process(uint Samples) {
        const config_t& config = ConfigReader.Lock();
#if HAVE_JACK_MIDI
        if (config.MidiDevice) config.MidiDevice->Process(Samples);
#endif
        int res = config.AudioDevice ? config.AudioDevice->Process(Samples) : 0;
        ConfigReader.Unlock();
        return res;
    }

    void JackClient::Stop() {
        const config_t& config = ConfigReader.Lock();
        if (config.AudioDevice) config.AudioDevice->Stop();
        ConfigReader.Unlock();
    }

    JackClient::JackClient(String Name) : ConfigReader(Config) {
        {
            config_t& config = Config.GetConfigForUpdate();
            config.AudioDevice = 0;
#if HAVE_JACK_MIDI
            config.MidiDevice = 0;
#endif
        }
        {
            config_t& config = Config.SwitchConfig();
            config.AudioDevice = 0;
#if HAVE_JACK_MIDI
            config.MidiDevice = 0;
#endif
        }
        audio = midi = false;
        if (Name.size() >= jack_client_name_size())
            throw Exception("JACK client name too long");
#if HAVE_JACK_CLIENT_OPEN
        hJackClient = jack_client_open(Name.c_str(), JackNullOption, NULL);
#else
        hJackClient = jack_client_new(Name.c_str());
#endif
        if (!hJackClient)
            throw Exception("Seems Jack server is not running.");
        jack_set_process_callback(hJackClient, linuxsampler_libjack_process_callback, this);
        jack_on_shutdown(hJackClient, linuxsampler_libjack_shutdown_callback, this);
        if (jack_activate(hJackClient))
            throw Exception("Jack: Cannot activate Jack client.");
    }

    JackClient::~JackClient() {
        jack_client_close(hJackClient);
    }

    void JackClient::SetAudioOutputDevice(AudioOutputDeviceJack* device) {
        Config.GetConfigForUpdate().AudioDevice = device;
        Config.SwitchConfig().AudioDevice = device;
    }

#if HAVE_JACK_MIDI
    void JackClient::SetMidiInputDevice(MidiInputDeviceJack* device) {
        Config.GetConfigForUpdate().MidiDevice = device;
        Config.SwitchConfig().MidiDevice = device;
    }
#endif

    JackClient* JackClient::CreateAudio(String Name) { // static
        JackClient* client;
        std::map<String, JackClient*>::const_iterator it = Clients.find(Name);
        if (it == Clients.end()) {
            client = new JackClient(Name);
            Clients[Name] = client;
        } else {
            client = it->second;
            if (client->audio) throw Exception("Jack audio device '" + Name + "' already exists");
        }
        client->audio = true;
        return client;
    }

    JackClient* JackClient::CreateMidi(String Name) { // static
        JackClient* client;
        std::map<String, JackClient*>::const_iterator it = Clients.find(Name);
        if (it == Clients.end()) {
            client = new JackClient(Name);
            Clients[Name] = client;
        } else {
            client = it->second;
            if (client->midi) throw Exception("Jack MIDI device '" + Name + "' already exists");
        }
        client->midi = true;
        return client;
    }

    void JackClient::ReleaseAudio(String Name) { // static
        JackClient* client = Clients[Name];
        client->SetAudioOutputDevice(0);
        client->audio = false;
        if (!client->midi) {
            Clients.erase(Name);
            delete client;
        }
    }

    void JackClient::ReleaseMidi(String Name) { // static
        JackClient* client = Clients[Name];
#if HAVE_JACK_MIDI
        client->SetMidiInputDevice(0);
#endif
        client->midi = false;
        if (!client->audio) {
            Clients.erase(Name);
            delete client;
        }
    }

} // namespace LinuxSampler

#endif // HAVE_JACK
