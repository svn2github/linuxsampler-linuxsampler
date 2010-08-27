/***************************************************************************
 *                                                                         *
 *   LinuxSampler - modular, streaming capable sampler                     *
 *                                                                         *
 *   Copyright (C) 2003, 2004 by Benno Senoner and Christian Schoenebeck   *
 *   Copyright (C) 2005 - 2009 Christian Schoenebeck                       *
 *   Copyright (C) 2009 Grigor Iliev                                       *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This library is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this library; if not, write to the Free Software           *
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston,                 *
 *   MA  02111-1307  USA                                                   *
 ***************************************************************************/

#ifndef __LS_INSTRUMENTMANAGERBASE_H__
#define __LS_INSTRUMENTMANAGERBASE_H__

#include "InstrumentManager.h"
#include "AbstractEngine.h"
#include "AbstractEngineChannel.h"
#include "../common/ResourceManager.h"
#include "../common/global_private.h"

// We need to know the maximum number of sample points which are going to
// be processed for each render cycle of the audio output driver, to know
// how much initial sample points we need to cache into RAM. If the given
// sampler channel does not have an audio output device assigned yet
// though, we simply use this default value.
#define GIG_RESOURCE_MANAGER_DEFAULT_MAX_SAMPLES_PER_CYCLE     128

namespace LinuxSampler {

    template <class F /* Instrument File */, class I /* Instrument */, class R /* Regions */, class S /*Sample */>
    class InstrumentManagerBase : virtual public InstrumentManager, virtual public ResourceManager<InstrumentManager::instrument_id_t, I> {
        public:
            struct region_info_t {
                int    refCount;
                F*     file;
                void*  pArg;

                region_info_t() {
                    refCount = 0; file = NULL; pArg = NULL;
                }
            };

            typedef ResourceConsumer<I> InstrumentConsumer;

            InstrumentManagerBase() { }
            virtual ~InstrumentManagerBase() { }

            virtual InstrumentEditor* LaunchInstrumentEditor(instrument_id_t ID, void* pUserData = NULL) throw (InstrumentManagerException) {
                 throw InstrumentManagerException(
                    "Instrument editing is not supported for this instrument format"
                );
            }

            virtual String GetInstrumentDataStructureName(instrument_id_t ID) {
                throw InstrumentManagerException("Not implemented");
            }

            virtual String GetInstrumentDataStructureVersion(instrument_id_t ID) {
                throw InstrumentManagerException("Not implemented");
            }

            /**
             * Give back an instrument. This should be used instead of
             * HandBack if there are some regions that are still in
             * use. (When an instrument is changed, the voices currently
             * playing are allowed to keep playing with the old instrument
             * until note off arrives. New notes will use the new instrument.)
             */
            void HandBackInstrument (
                I*                   pResource,
                InstrumentConsumer*  pConsumer,
                RTList<R*>*          pRegionsInUse
            ) {
                RegionInfoMutex.Lock();
                for (typename RTList<R*>::Iterator i = pRegionsInUse->first() ; i != pRegionsInUse->end() ; i++) {
                    RegionInfo[*i].refCount++;
                    SampleRefCount[(*i)->pSample]++;
                }
                HandBack(pResource, pConsumer, true);
                RegionInfoMutex.Unlock();
            }

            /**
             * Give back a region that belongs to an instrument that
             * was previously handed back.
             */
            virtual void HandBackRegion(R* pRegion) {
                RegionInfoMutex.Lock();
                if (RegionInfo.find(pRegion) == RegionInfo.end()) {
                    std::cerr << "Handing back unknown region. This is a BUG!!!" << std::endl;
                }
                region_info_t& regInfo = RegionInfo[pRegion];
                int regionRefCount = --regInfo.refCount;
                int sampleRefCount = --SampleRefCount[pRegion->pSample];
                if (regionRefCount == 0) {
                    S* pSample = pRegion->pSample;

                    DeleteRegionIfNotUsed(pRegion, &regInfo);

                    if (sampleRefCount == 0) {
                        SampleRefCount.erase(pSample);
                        DeleteSampleIfNotUsed(pSample, &regInfo);
                    }
                    RegionInfo.erase(pRegion);
                }
                RegionInfoMutex.Unlock();
            }

            virtual InstrumentManager::mode_t GetMode(const InstrumentManager::instrument_id_t& ID) {
                return static_cast<InstrumentManager::mode_t>(ResourceManager<instrument_id_t, I>::AvailabilityMode(ID));
            }

            virtual void SetMode(const InstrumentManager::instrument_id_t& ID, InstrumentManager::mode_t Mode) {
                dmsg(2,("InstrumentManagerBase: setting mode for %s (Index=%d) to %d\n",ID.FileName.c_str(),ID.Index,Mode));
                SetAvailabilityMode(ID, static_cast<typename ResourceManager<instrument_id_t, I>::mode_t>(Mode));
            }

        protected:
            Mutex RegionInfoMutex; ///< protects the RegionInfo and SampleRefCount maps from concurrent access by the instrument loader and disk threads
            std::map< R*, region_info_t> RegionInfo; ///< contains dimension regions that are still in use but belong to released instrument
            std::map< S*, int> SampleRefCount; ///< contains samples that are still in use but belong to a released instrument

            /**
             *  Caches a certain size at the beginning of the given sample in RAM. If the
             *  sample is very short, the whole sample will be loaded into RAM and thus
             *  no disk streaming is needed for this sample. Caching an initial part of
             *  samples is needed to compensate disk reading latency.
             *
             *  @param pSample - points to the sample to be cached
             *  @param pEngine - pointer to Engine which caused this call
             *                   (may be NULL, in this case default amount of samples
             *                   will be cached)
             */
            virtual void CacheInitialSamples(S* pSample, AbstractEngine* pEngine) = 0;

            virtual void DeleteRegionIfNotUsed(R* pRegion, region_info_t* pRegInfo) = 0;
            virtual void DeleteSampleIfNotUsed(S* pSample, region_info_t* pRegInfo) = 0;

            void SetKeyBindings(uint8_t* bindingsArray, int low, int high, int undefined = -1) {
                if (low == undefined || high == undefined) return;
                if (low < 0 || low > 127 || high < 0 || high > 127 || low > high) {
                    std::cerr << "Invalid key range: " << low << " - " << high << std::endl;
                    return;
                }

                for (int i = low; i <= high; i++) bindingsArray[i] = 1;
            }

    };

    template <class F /* Instrument File */, class I /* Instrument */, class R /* Region */, class S /* Sample */>
    class InstrumentManagerDefaultImpl : public InstrumentManagerBase<F, I, R, S> {
        public:
            InstrumentManagerDefaultImpl() { }
            virtual ~InstrumentManagerDefaultImpl() { }
        protected:
            virtual void CacheInitialSamples(S* pSample, AbstractEngine* pEngine)  {
                if (!pSample) {
                    dmsg(4,("InstrumentManagerBase: Skipping sample (pSample == NULL)\n"));
                    return;
                }
                if (!pSample->GetTotalFrameCount()) return; // skip zero size samples

                if (pSample->GetTotalFrameCount() <= CONFIG_PRELOAD_SAMPLES) {
                    // Sample is too short for disk streaming, so we load the whole
                    // sample into RAM and place 'pAudioIO->FragmentSize << CONFIG_MAX_PITCH'
                    // number of '0' samples (silence samples) behind the official buffer
                    // border, to allow the interpolator do it's work even at the end of
                    // the sample.
                    const uint maxSamplesPerCycle =
                        (pEngine) ? pEngine->pAudioOutputDevice->MaxSamplesPerCycle()
                                  : GIG_RESOURCE_MANAGER_DEFAULT_MAX_SAMPLES_PER_CYCLE;
                    const uint neededSilenceSamples = (maxSamplesPerCycle << CONFIG_MAX_PITCH) + 3;
                    const uint currentlyCachedSilenceSamples = pSample->GetCache().NullExtensionSize / pSample->GetFrameSize();
                    if (currentlyCachedSilenceSamples < neededSilenceSamples) {
                        dmsg(3,("Caching whole sample (sample name: \"%s\", sample size: %d)\n", pSample->GetName().c_str(), pSample->GetTotalFrameCount()));
                        typename S::buffer_t buf = pSample->LoadSampleDataWithNullSamplesExtension(neededSilenceSamples);
                        dmsg(4,("Cached %d Bytes, %d silence bytes.\n", buf.Size, buf.NullExtensionSize));
                    }
                }
                else { // we only cache CONFIG_PRELOAD_SAMPLES and stream the other sample points from disk
                    if (!pSample->GetCache().Size) pSample->LoadSampleData(CONFIG_PRELOAD_SAMPLES);
                }

                if (!pSample->GetCache().Size) std::cerr << "Unable to cache sample - maybe memory full!" << std::endl << std::flush;
            }

            /**
             * Just a wrapper around the other @c CacheInitialSamples() method.
             *
             *  @param pSample - points to the sample to be cached
             *  @param pEngine - pointer to Gig Engine Channel which caused this call
             *                   (may be NULL, in this case default amount of samples
             *                   will be cached)
             */
            virtual void CacheInitialSamples(S* pSample, EngineChannel* pEngineChannel) {
                 AbstractEngine* pEngine =
                    (pEngineChannel && pEngineChannel->GetEngine()) ?
                        dynamic_cast<AbstractEngine*>(pEngineChannel->GetEngine()) : NULL;
                CacheInitialSamples(pSample, pEngine);
            }
    };

} // namespace LinuxSampler

#endif // __LS_INSTRUMENTMANAGERBASE_H__