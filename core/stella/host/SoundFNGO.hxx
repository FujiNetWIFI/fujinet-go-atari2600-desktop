/*
 * SoundFNGO -- Stella's Sound for the FujiNet Go host.
 *
 * SoundSDL minus SDL. Stella's TIA produces one sample per scanline
 * (31 440 Hz NTSC, 31 200 Hz PAL) into the AudioQueue the console hands us
 * in open(); the session's own audio device (SDL3 in audio_sdl.c) runs at a
 * conventional rate. Rather than reinvent that conversion, this class builds
 * Stella's own resampler (src/common/audio/) exactly as SoundSDL::open does
 * -- same prebuffer/underrun policy, same Lanczos filter -- and exposes
 * fill() for the device callback, which is the only cross-thread entry
 * point. Everything else runs on the Stella thread.
 *
 * KidVid WAV playback is not supported (playWav returns false).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifdef SOUND_SUPPORT
#ifndef SOUND_FNGO_HXX
#define SOUND_FNGO_HXX

#include <mutex>

class OSystem;
class AudioQueue;
class EmulationTiming;
class AudioSettings;
class Resampler;

#include "bspf.hxx"
#include "Sound.hxx"

class SoundFNGO : public Sound
{
  public:
    SoundFNGO(OSystem& osystem, AudioSettings& audioSettings);
    ~SoundFNGO() override;

    // The device the host will pull from: rate in Hz, channel count.
    // Applies to the next open(); call before the console is created (or
    // reopen afterwards).
    void setOutputFormat(uInt32 rate, bool stereo);
    uInt32 outputRate() const { return myOutRate; }
    bool outputStereo() const { return myOutStereo; }

    // Device-callback side. Writes `frames` interleaved float frames
    // (2 floats per frame when stereo) at the output rate; zero-fills when
    // paused, muted, not yet open, or if the Stella thread holds the lock.
    void fill(float* out, uInt32 frames);

    // Sound overrides
    void setEnabled(bool enable) override;
    void open(shared_ptr<AudioQueue> audioQueue,
              shared_ptr<const EmulationTiming> emulationTiming) override;
    void mute(bool enable) override;
    void toggleMute() override;
    bool pause(bool state) override;
    void setVolume(uInt32 volume, bool persist = true) override;
    void adjustVolume(int direction = 1) override;
    string about() const override;
    bool playWav(const string&, uInt32 = 0, uInt32 = 0) override { return false; }

  private:
    void initResampler();

    AudioSettings& myAudioSettings;
    std::mutex myMutex;
    shared_ptr<AudioQueue> myAudioQueue;
    shared_ptr<const EmulationTiming> myEmulationTiming;
    unique_ptr<Resampler> myResampler;
    Int16* myCurrentFragment{nullptr};
    bool myUnderrun{true};
    bool myIsInitializedFlag{false};
    bool myPaused{true};
    bool myMuted{false};
    uInt32 myVolume{100};
    float myVolumeFactor{1.F};
    uInt32 myOutRate{48000};
    bool myOutStereo{true};

    SoundFNGO() = delete;
    SoundFNGO(const SoundFNGO&) = delete;
    SoundFNGO(SoundFNGO&&) = delete;
    SoundFNGO& operator=(const SoundFNGO&) = delete;
    SoundFNGO& operator=(SoundFNGO&&) = delete;
};

#endif // SOUND_FNGO_HXX
#endif // SOUND_SUPPORT
