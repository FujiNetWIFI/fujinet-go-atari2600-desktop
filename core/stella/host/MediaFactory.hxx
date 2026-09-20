//============================================================================
//
//   SSSS    tt          lll  lll
//  SS  SS   tt           ll   ll
//  SS     tttttt  eeee   ll   ll   aaaa
//   SSSS    tt   ee  ee  ll   ll      aa
//      SS   tt   eeeeee  ll   ll   aaaaa  --  "An Atari 2600 VCS Emulator"
//  SS  SS   tt   ee      ll   ll  aa  aa
//   SSSS     tt   eeeee llll llll  aaaaa
//
// Copyright (c) 1995-2026 by Bradford W. Mott, Stephen Anthony
// and the Stella Team
//
// See the file "License.txt" for information on usage and redistribution of
// this file, and for a DISCLAIMER OF ALL WARRANTIES.
//============================================================================

// fujinet-go-atari2600-desktop: the ONE file overridden in the staged Stella
// tree (cmake/StageStella.cmake copies this over src/common/MediaFactory.hxx).
//
// Stella's own copy selects an OSystem / FBBackend / Sound / EventHandler
// implementation by BSPF_* platform and by __LIB_RETRO__ vs SDL_SUPPORT. This
// build is neither the SDL desktop nor a libretro core: it is the FujiNet Go
// host, whose windowing, audio device and input all belong to the native
// frontend (GTK4 / Qt6 / AppKit / Win32) and the SDL-free session layer. So
// every factory here returns the FNGO classes in core/stella/host/, which are
// modelled file-for-file on Stella's src/os/libretro/ ones. The signatures
// are Stella's; only the choices differ. Keep this in step with upstream's
// MediaFactory.hxx when the pin moves.

#ifndef MEDIA_FACTORY_HXX
#define MEDIA_FACTORY_HXX

#include "bspf.hxx"

#include "OSystem.hxx"
#include "Settings.hxx"
#include "SerialPort.hxx"
#if defined(BSPF_UNIX) || defined(BSPF_MACOS)
  #include "SerialPortPOSIX.hxx"
#elifdef BSPF_WINDOWS
  #include "SerialPortWINDOWS.hxx"
#else
  #error Unsupported platform!
#endif

#include "FNGOHostHooks.hxx"
#include "OSystemFNGO.hxx"
#include "EventHandlerFNGO.hxx"
#include "FBBackendFNGO.hxx"
#ifdef SOUND_SUPPORT
  #include "SoundFNGO.hxx"
#else
  #include "SoundNull.hxx"
#endif

class AudioSettings;

/**
  This class deals with the different framebuffer/sound/event
  implementations for the various ports of Stella, and always returns a
  valid object based on the specific port and restrictions on that port.

  @author  Stephen Anthony
*/
class MediaFactory
{
  public:
    static unique_ptr<OSystem> createOSystem()
    {
      return std::make_unique<OSystemFNGO>();
    }

    static unique_ptr<Settings> createSettings()
    {
      return std::make_unique<Settings>();
    }

    static unique_ptr<SerialPort> createSerialPort()
    {
    #if defined(BSPF_UNIX) || defined(BSPF_MACOS)
      return std::make_unique<SerialPortPOSIX>();
    #elifdef BSPF_WINDOWS
      return std::make_unique<SerialPortWINDOWS>();
    #else
      return std::make_unique<SerialPort>();
    #endif
    }

    static unique_ptr<FBBackend> createVideoBackend(OSystem& osystem)
    {
      return std::make_unique<FBBackendFNGO>(osystem);
    }

    static unique_ptr<Sound> createAudio(OSystem& osystem, AudioSettings& audioSettings)
    {
    #ifdef SOUND_SUPPORT
      return std::make_unique<SoundFNGO>(osystem, audioSettings);
    #else
      return std::make_unique<SoundNull>(osystem);
    #endif
    }

    static unique_ptr<EventHandler> createEventHandler(OSystem& osystem)
    {
      return std::make_unique<EventHandlerFNGO>(osystem);
    }

    static void cleanUp() { }

    static string backendName() { return "FujiNet Go host"; }

    static bool openURL(const string& url)
    {
      return FNGOHostHooks::openURL(url);
    }

  private:
    // Following constructors and assignment operators not supported
    MediaFactory() = delete;
    ~MediaFactory() = delete;
    MediaFactory(const MediaFactory&) = delete;
    MediaFactory(MediaFactory&&) = delete;
    MediaFactory& operator=(const MediaFactory&) = delete;
    MediaFactory& operator=(MediaFactory&&) = delete;
};

#endif
