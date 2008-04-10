/***************************************************************************
 *                                                                         *
 *   Copyright (C) 2008 Christian Schoenebeck                              *
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

#include "Effect.h"

namespace LinuxSampler {

Effect::~Effect() {
    for (int i = 0; i < vInputChannels.size(); ++i)
        delete vInputChannels[i];
    for (int i = 0; i < vOutputChannels.size(); ++i)
        delete vOutputChannels[i];
}

void Effect::InitEffect(AudioOutputDevice* pDevice) {
}

AudioChannel* Effect::InputChannel(uint ChannelIndex) const {
    if (ChannelIndex >= vInputChannels.size()) return NULL;
    return vInputChannels[ChannelIndex];
}

uint Effect::InputChannelCount() const {
    return vInputChannels.size();
}

AudioChannel* Effect::OutputChannel(uint ChannelIndex) const {
    if (ChannelIndex >= vOutputChannels.size()) return NULL;
    return vOutputChannels[ChannelIndex];
}

uint Effect::OutputChannelCount() const {
    return vOutputChannels.size();
}

} // namespace LinuxSampler
