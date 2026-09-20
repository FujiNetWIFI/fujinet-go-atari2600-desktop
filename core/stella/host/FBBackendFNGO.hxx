/*
 * FBBackendFNGO -- Stella's video backend for the FujiNet Go host.
 *
 * Modelled on src/os/libretro/FBBackendLIBRETRO.hxx: there is no window here
 * (the native frontend owns the display), so nearly everything is a no-op.
 * What it does keep is the last video mode Stella asked for, because that
 * mode's image rectangle is what tells the host which part of the TIA
 * surface holds the picture, and one virtual display large enough for the
 * debugger dialog Stella constructs offscreen (FrameBuffer::createDisplay
 * refuses a mode bigger than the desktop it was told about).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef FB_BACKEND_FNGO_HXX
#define FB_BACKEND_FNGO_HXX

class OSystem;

#include "bspf.hxx"
#include "FBBackend.hxx"
#include "FBSurfaceFNGO.hxx"
#include "FNGOHostHooks.hxx"

class FBBackendFNGO : public FBBackend
{
  public:
    explicit FBBackendFNGO(OSystem&) { }
    ~FBBackendFNGO() override = default;

    int scaleX(int x) const override { return x; }
    int scaleY(int y) const override { return y; }

    // The mode Stella last applied (FrameBuffer::applyVideoMode). imageR is
    // the rectangle of the TIA surface that holds the picture.
    const VideoModeHandler::Mode& mode() const { return myMode; }

  protected:
    void queryHardware(std::unordered_map<uInt32, Common::Size>& fullscreenRes,
                       std::unordered_map<uInt32, Common::Size>& windowedRes,
                       VariantList& renderers) override
    {
      // Big enough for any debugger layout Stella can ask for, and not so
      // big that FrameBuffer decides this is a HiDPI desktop (the host also
      // pins hidpi=false).
      fullscreenRes.emplace(0, Common::Size{1920, 1080});
      windowedRes.emplace(0, Common::Size{1920, 1080});
      VarList::push_back(renderers, "software", "Software");
    }

    unique_ptr<FBSurface>
      createSurface(uInt32 w, uInt32 h, ScalingInterpolation,
                    const uInt32*) override
    {
      return std::make_unique<FBSurfaceFNGO>(w, h);
    }

    string about() const override { return "Video system: FujiNet Go host"; }

    void showMessage(string_view message) override
    {
      if(message != myLastMessage)
      {
        myLastMessage = message;
        FNGOHostHooks::message(myLastMessage);
      }
    }
    void showGaugeMessage(string_view message, string_view valueText,
                          float, float, float) override
    {
      const string combined = valueText.empty()
        ? string{message}
        : std::format("{}: {}", message, valueText);
      if(combined != myLastMessage)
      {
        myLastMessage = combined;
        FNGOHostHooks::message(myLastMessage);
      }
    }

    void setTitle(string_view title) override { FNGOHostHooks::title(title); }
    void showCursor(bool) override { }
    bool fullScreen() const override { return false; }
    uInt32 rMask() const override { return 0x00FF0000; }
    uInt32 gMask() const override { return 0x0000FF00; }
    uInt32 bMask() const override { return 0x000000FF; }
    uInt32 aMask() const override { return 0xFF000000; }
    const FBSurface& compositedSurface() override
    {
      static const FBSurfaceFNGO tmp(1, 1); return tmp;
    }
    bool isCurrentWindowPositioned() const override { return true; }
    Common::Point getCurrentWindowPos() const override { return Common::Point{}; }
    uInt32 getCurrentDisplayID() const override { return 0; }
    void clear() override { }
    void flush() override { }
    bool setVideoMode(const VideoModeHandler::Mode& mode,
                      uInt32, const Common::Point&) override
    {
      myMode = mode;
      return true;
    }
    void grabMouse(bool) override { }
    void enableTextEvents(bool) override { }
    void renderToScreen() override { }
    int refreshRate() const override { return 60; }
    bool isLightTheme() const override { return true; }
    bool isDarkTheme() const override { return false; }

  private:
    string myLastMessage;
    VideoModeHandler::Mode myMode;

    FBBackendFNGO() = delete;
    FBBackendFNGO(const FBBackendFNGO&) = delete;
    FBBackendFNGO(FBBackendFNGO&&) = delete;
    FBBackendFNGO& operator=(const FBBackendFNGO&) = delete;
    FBBackendFNGO& operator=(FBBackendFNGO&&) = delete;
};

#endif // FB_BACKEND_FNGO_HXX
