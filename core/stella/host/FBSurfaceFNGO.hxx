/*
 * FBSurfaceFNGO -- an FBSurface backed by a plain pixel array.
 *
 * Modelled on src/os/libretro/FBSurfaceLIBRETRO.hxx with three differences
 * that matter here, because Stella's GUI (the debugger dialog that the
 * engine insists on constructing) draws into these surfaces offscreen:
 *
 *   - fillRect is NOT overridden to a no-op: the base-class primitives all
 *     write through myPixels, and letting them run is what keeps the
 *     offscreen dialogs consistent (and costs nothing visible).
 *   - src/dst rectangles are stored, because Dialog::open() and
 *     TIASurface::initialize() set and later read them.
 *   - resize() really reallocates: Dialog::open() resizes its surface on
 *     every open after the first.
 *
 * The TIA surface is the one whose pixels the host publishes to the frontend
 * (XRGB8888, one uInt32 per pixel, pitch in pixels).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef FB_SURFACE_FNGO_HXX
#define FB_SURFACE_FNGO_HXX

#include "bspf.hxx"
#include "FBSurface.hxx"

class FBSurfaceFNGO : public FBSurface
{
  public:
    FBSurfaceFNGO(uInt32 width, uInt32 height)
    {
      allocate(width, height);
      mySrcGUIR = Common::Rect(0, 0, width, height);
      myDstGUIR = Common::Rect(0, 0, width, height);
    }
    ~FBSurfaceFNGO() override = default;

    uInt32 width() const override { return myWidth; }
    uInt32 height() const override { return myHeight; }

    const Common::Rect& srcRect() const override { return mySrcGUIR; }
    const Common::Rect& dstRect() const override { return myDstGUIR; }
    void setSrcPos(uInt32 x, uInt32 y) override { mySrcGUIR.moveTo(x, y); }
    void setSrcSize(uInt32 w, uInt32 h) override { mySrcGUIR.setWidth(w); mySrcGUIR.setHeight(h); }
    void setSrcRect(const Common::Rect& r) override { mySrcGUIR = r; }
    void setDstPos(uInt32 x, uInt32 y) override { myDstGUIR.moveTo(x, y); }
    void setDstSize(uInt32 w, uInt32 h) override { myDstGUIR.setWidth(w); myDstGUIR.setHeight(h); }
    void setDstRect(const Common::Rect& r) override { myDstGUIR = r; }
    void setVisible(bool visible) override { myVisible = visible; }
    bool visible() const { return myVisible; }

    void translateCoords(Int32& x, Int32& y) const override
    {
      x -= static_cast<Int32>(myDstGUIR.x());
      y -= static_cast<Int32>(myDstGUIR.y());
    }

    bool render() override { return true; }
    void invalidate() override { }
    void invalidateRect(uInt32, uInt32, uInt32, uInt32) override { }
    void reload() override { }
    void resize(uInt32 width, uInt32 height) override
    {
      if(width == myWidth && height == myHeight) return;
      allocate(width, height);
      mySrcGUIR = Common::Rect(0, 0, width, height);
      myDstGUIR.setWidth(width);
      myDstGUIR.setHeight(height);
    }
    void setScalingInterpolation(ScalingInterpolation) override { }
    void enableBlend(bool) override { }
    void setBlendLevel(uInt32) override { }

  private:
    void allocate(uInt32 width, uInt32 height)
    {
      myWidth = std::max(1U, width);
      myHeight = std::max(1U, height);
      myPixelData = std::make_unique<uInt32[]>(static_cast<size_t>(myWidth) * myHeight);
      // These *must* be set for the parent class
      myPixels = myPixelData.get();
      myPitch = myWidth;
    }

    uInt32 myWidth{0}, myHeight{0};
    unique_ptr<uInt32[]> myPixelData;
    Common::Rect mySrcGUIR, myDstGUIR;
    bool myVisible{true};

    FBSurfaceFNGO() = delete;
    FBSurfaceFNGO(const FBSurfaceFNGO&) = delete;
    FBSurfaceFNGO(FBSurfaceFNGO&&) = delete;
    FBSurfaceFNGO& operator=(const FBSurfaceFNGO&) = delete;
    FBSurfaceFNGO& operator=(FBSurfaceFNGO&&) = delete;
};

#endif // FB_SURFACE_FNGO_HXX
