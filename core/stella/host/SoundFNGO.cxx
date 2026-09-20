/*
 * SoundFNGO -- see the header.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifdef SOUND_SUPPORT

#include <algorithm>
#include <cmath>
#include <cstring>

#include "AudioQueue.hxx"
#include "AudioSettings.hxx"
#include "EmulationTiming.hxx"
#include "Logger.hxx"
#include "OSystem.hxx"
#include "audio/LanczosResampler.hxx"
#include "audio/SimpleResampler.hxx"

#include "SoundFNGO.hxx"

SoundFNGO::SoundFNGO(OSystem& osystem, AudioSettings& audioSettings)
  : Sound(osystem),
    myAudioSettings{audioSettings}
{
  Logger::debug("SoundFNGO::SoundFNGO started ...");
  myVolume = myAudioSettings.volume();
  myVolumeFactor = static_cast<float>(myVolume) / 100.F;
  Logger::debug("SoundFNGO::SoundFNGO initialized");
}

SoundFNGO::~SoundFNGO()
{
  const std::lock_guard<std::mutex> lock(myMutex);
  myResampler.reset();
  myAudioQueue.reset();
}

void SoundFNGO::setOutputFormat(uInt32 rate, bool stereo)
{
  const std::lock_guard<std::mutex> lock(myMutex);
  myOutRate = rate ? rate : 48000;
  myOutStereo = stereo;
  if(myIsInitializedFlag && myAudioQueue && myEmulationTiming)
    initResampler();
}

void SoundFNGO::setEnabled(bool enable)
{
  myAudioSettings.setEnabled(enable);
  if(myAudioQueue)
    myAudioQueue->ignoreOverflows(!enable);
}

void SoundFNGO::open(shared_ptr<AudioQueue> audioQueue,
                     shared_ptr<const EmulationTiming> emulationTiming)
{
  const std::lock_guard<std::mutex> lock(myMutex);

  myAudioQueue = std::move(audioQueue);
  myEmulationTiming = std::move(emulationTiming);
  myUnderrun = true;
  myCurrentFragment = nullptr;
  myAudioQueue->ignoreOverflows(!myAudioSettings.enabled());

  initResampler();
  myIsInitializedFlag = true;
  myPaused = false;
}

void SoundFNGO::initResampler()
{
  const Resampler::NextFragmentCallback nextFragmentCallback = [this] -> Int16* {
    Int16* nextFragment = nullptr;

    if(myUnderrun)
      nextFragment = myAudioQueue->size() >= myEmulationTiming->prebufferFragmentCount()
        ? myAudioQueue->dequeue(myCurrentFragment)
        : nullptr;
    else
      nextFragment = myAudioQueue->dequeue(myCurrentFragment);

    myUnderrun = nextFragment == nullptr;
    if(nextFragment)
      myCurrentFragment = nextFragment;

    return nextFragment;
  };

  const Resampler::Format formatFrom =
    Resampler::Format(myEmulationTiming->audioSampleRate(),
    myAudioQueue->fragmentSize(), myAudioQueue->isStereo());
  const Resampler::Format formatTo =
    Resampler::Format(myOutRate, 1024, myOutStereo);

  switch(myAudioSettings.resamplingQuality())
  {
    using enum AudioSettings::ResamplingQuality;
    case nearestNeighbour:
      myResampler = std::make_unique<SimpleResampler>(formatFrom, formatTo,
                                                      nextFragmentCallback);
      break;
    case lanczos_2:
      myResampler = std::make_unique<LanczosResampler>(formatFrom, formatTo,
                                                       nextFragmentCallback, 2);
      break;
    case lanczos_3:
      myResampler = std::make_unique<LanczosResampler>(formatFrom, formatTo,
                                                       nextFragmentCallback, 3);
      break;
    default:
      myResampler = std::make_unique<LanczosResampler>(formatFrom, formatTo,
                                                       nextFragmentCallback, 2);
      break;
  }
}

void SoundFNGO::fill(float* out, uInt32 frames)
{
  const uInt32 samples = frames * (myOutStereo ? 2 : 1);
  // try_lock: the device thread must never wait on the Stella thread
  // (which may be tearing a console down). A skipped period is silence.
  if(!myMutex.try_lock())
  {
    std::memset(out, 0, sizeof(float) * samples);
    return;
  }
  if(!myIsInitializedFlag || !myResampler || myPaused || myMuted
     || !myAudioSettings.enabled())
  {
    std::memset(out, 0, sizeof(float) * samples);
    myMutex.unlock();
    return;
  }
  myResampler->fillFragment(out, frames);
  if(myVolumeFactor != 1.F)
    for(uInt32 i = 0; i < samples; ++i)
      out[i] *= myVolumeFactor;
  myMutex.unlock();
}

void SoundFNGO::mute(bool enable)
{
  myMuted = enable;
}

void SoundFNGO::toggleMute()
{
  myMuted = !myMuted;
}

bool SoundFNGO::pause(bool state)
{
  const bool oldstate = myPaused;
  myPaused = state;
  return oldstate;
}

void SoundFNGO::setVolume(uInt32 volume, bool persist)
{
  if(volume <= 100)
  {
    myVolume = volume;
    if(persist)
      myAudioSettings.setVolume(volume);
    myVolumeFactor = static_cast<float>(volume) / 100.F;
  }
}

void SoundFNGO::adjustVolume(int direction)
{
  const int vol = std::clamp(static_cast<int>(myVolume) + direction * 2, 0, 100);
  setVolume(static_cast<uInt32>(vol));
}

string SoundFNGO::about() const
{
  return std::format("Sound enabled:\n  Volume: {}%\n  Output: {} Hz {}\n",
                     myVolume, myOutRate, myOutStereo ? "stereo" : "mono");
}

#endif // SOUND_SUPPORT
