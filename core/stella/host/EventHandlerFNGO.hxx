/*
 * EventHandlerFNGO -- Stella's event handler for the FujiNet Go host.
 *
 * Modelled on src/os/libretro/EventHandlerLIBRETRO.hxx. The native frontend
 * and the SDL gamepad thread translate their input into Stella Event::Type
 * values and enqueue them here from ANY thread; pollEvent() -- which
 * EventHandler::pollInput() calls once per frame on the Stella thread, with
 * the Event input window already open -- drains the queue into
 * handleEvent(). Doing it inside the window is what lets a press and release
 * that both landed within one frame be seen by the machine (Event records
 * transitions only while the window is open).
 *
 * Continuous inputs (paddle/driving analog, mouse) are read whole per window,
 * so the queue coalesces them: only the last value of a given analog event
 * per frame is delivered.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef EVENT_HANDLER_FNGO_HXX
#define EVENT_HANDLER_FNGO_HXX

#include <mutex>
#include <vector>

#include "Event.hxx"
#include "EventHandler.hxx"

class EventHandlerFNGO : public EventHandler
{
  public:
    explicit EventHandlerFNGO(OSystem& osystem) : EventHandler(osystem) { }
    ~EventHandlerFNGO() override = default;

    // Thread-safe: called by the frontends and the gamepad thread.
    void enqueue(Event::Type type, Int32 value)
    {
      const std::lock_guard<std::mutex> lock(myMutex);
      if(Event::isContinuous(type))
      {
        for(auto& e : myPending)
          if(e.type == type) { e.value = value; return; }
      }
      myPending.push_back({type, value});
    }

    void copyText(const string&) const override { }
    string pasteText(string& text) const override { text.clear(); return text; }
    bool hasClipboardText() const override { return false; }

  protected:
    void pollEvent() override
    {
      {
        const std::lock_guard<std::mutex> lock(myMutex);
        myDraining.swap(myPending);
      }
      for(const auto& e : myDraining)
        handleEvent(e.type, e.value);
      myDraining.clear();
    }

  private:
    struct Pending { Event::Type type; Int32 value; };
    std::mutex myMutex;
    std::vector<Pending> myPending, myDraining;

    EventHandlerFNGO() = delete;
    EventHandlerFNGO(const EventHandlerFNGO&) = delete;
    EventHandlerFNGO(EventHandlerFNGO&&) = delete;
    EventHandlerFNGO& operator=(const EventHandlerFNGO&) = delete;
    EventHandlerFNGO& operator=(EventHandlerFNGO&&) = delete;
};

#endif // EVENT_HANDLER_FNGO_HXX
