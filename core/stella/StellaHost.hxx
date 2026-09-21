/*
 * StellaHost -- the one thread that owns Stella, and everything the rest of
 * the app is allowed to do with it.
 *
 * Stella has no internal thread safety beyond Event and AudioQueue:
 * lockSystem(), Debugger::start(), Console::setControllers(),
 * OSystem::createConsole() are plain single-threaded mutations. So exactly
 * ONE thread (the "Stella thread", started by start()) ever touches the
 * OSystem, and every other thread talks to it through this class:
 *
 *   - input arrives through enqueueEvent() (thread-safe, drained once per
 *     frame inside the Event input window);
 *   - video leaves through copyFrame() (serial-based publish/copy, the
 *     family's contract);
 *   - audio leaves through fillAudio() (the device callback pulls);
 *   - everything else is a JOB: withStella(fn) runs fn(OSystem&) on the
 *     Stella thread and returns its value. Jobs run between frames while
 *     emulating, and immediately while the machine is stopped in the
 *     debugger -- which is exactly when the native debugger windows need to
 *     read CPU/TIA/RIOT state and issue commands.
 *
 * The frame loop follows OSystem::dispatchEmulation/mainLoop: tia.update()
 * until a frame is pending (a breakpoint returns Status::debugger mid-frame
 * and parks the loop in DEBUGGER state), takePendingSwap() for a FujiNet
 * network boot, then TIASurface::render() so palette / phosphor / NTSC
 * filtering are Stella's own, and a copy into the published frame slot.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef STELLA_HOST_HXX
#define STELLA_HOST_HXX

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <pthread.h>

#include "bspf.hxx"
#include "Event.hxx"
#include "EventHandlerConstants.hxx"
#include "Settings.hxx"

class OSystem;
class OSystemFNGO;
class SoundFNGO;
class EventHandlerFNGO;

class StellaHost
{
  public:
    struct Config
    {
      std::string baseDir;        // Stella's base/home dir, trailing separator
      Settings::Options options;  // pushed into Settings before anything else
      std::string startRom;       // empty = boot FujiNet's CONFIG client
      uInt32 audioRate{48000};
      bool audioStereo{true};
    };

    enum class StopReason : uInt8 { Breakpoint, Fatal, Paused };

    struct Callbacks
    {
      // All called on the Stella thread. Keep them short and never call
      // back into the host from them.
      std::function<void(StopReason, const std::string& message, int address)> onStopped;
      std::function<void()> onResumed;
      std::function<void()> onConsoleReplaced;
      std::function<void(const std::string&)> onMessage;
      std::function<void(const std::string&)> onError;
    };

    struct FrameInfo
    {
      uInt32 width{0}, height{0};
      uInt64 serial{0};
      int refreshRate{60};
    };

    StellaHost();
    ~StellaHost();

    void setCallbacks(Callbacks cb) { myCallbacks = std::move(cb); }

    // Starts the Stella thread, initialises the OSystem and creates the
    // first console. Synchronous: returns false with `error` set if the
    // console could not be created (the thread is then stopped again).
    bool start(const Config& config, std::string& error);
    void stop();
    bool running() const { return myRunning.load(); }
    const std::string& lastError() const { return myLastError; }

    // ---- video --------------------------------------------------------
    // Copies the latest frame iff its serial differs from *serialInOut,
    // updates it and returns true. dst is resized to width*height pixels
    // (XRGB8888, one uInt32 per pixel, 160 wide). Pass 0 to force a copy.
    bool copyFrame(std::vector<uInt32>& dst, FrameInfo& info, uInt64* serialInOut);
    // Feed the UI's frame-clock ticks (CLOCK_MONOTONIC ns). While a steady
    // stream arrives the emulator phase-locks one frame per tick.
    void notifyVsync(Int64 frameTimeNs);

    // ---- audio --------------------------------------------------------
    void fillAudio(float* out, uInt32 frames);

    // ---- input --------------------------------------------------------
    void enqueueEvent(Event::Type type, Int32 value);

    // ---- jobs ---------------------------------------------------------
    // Runs fn(OSystem&) on the Stella thread and returns its result.
    // Blocks the caller until done; a job posted from the Stella thread
    // itself runs inline.
    template<typename F>
    auto withStella(F&& fn) -> std::invoke_result_t<F, OSystem&>
    {
      using R = std::invoke_result_t<F, OSystem&>;
      if(std::this_thread::get_id() == myThreadId)
        return fn(*osystem());
      std::packaged_task<R(OSystem&)> task(std::forward<F>(fn));
      auto fut = task.get_future();
      post([&task, this] { task(*osystem()); });
      return fut.get();
    }

    // ---- console lifecycle (each is a job; returns "" or an error) ----
    std::string loadRom(const std::string& path);
    std::string rebootToConfig();
    // Momentary console switches / actions, as Stella events.
    void reset();

    // ---- debugger -----------------------------------------------------
    // Run a DebuggerParser command; returns its text (with Stella's
    // _EXIT_DEBUGGER sentinel stripped). Enters the debugger first if the
    // machine is running.
    std::string dbgRun(const std::string& command);
    // Stop the machine at the next instruction boundary (Event::DebuggerMode).
    void dbgBreak();
    // Resume ("run").
    void dbgContinue();
    bool isStopped() const { return myState.load() == EventHandlerState::DEBUGGER; }
    EventHandlerState state() const { return myState.load(); }

    OSystem* osystem() const;

  private:
    void post(std::function<void()> job);
    void threadMain();
    void runJobs();
    bool waitForJob();
    void emulateFrame();
    void publishFrame();
    void pace(uInt64 cyclesRun);
    void enterStopped(StopReason reason, const std::string& msg, int addr);
    std::string createConsoleFrom(const std::string& path);
    void disconnectCartridge();
    void onStateChanged(EventHandlerState state);

    Callbacks myCallbacks;
    Config myConfig;

    std::unique_ptr<OSystemFNGO> myOSystem;
    // A pthread rather than a std::thread so the stack size is explicit:
    // std::thread takes the platform default, which is 512 KB on macOS --
    // Stella's own frontend runs the emulator on the 8 MB main thread and
    // was never sized for less.
    pthread_t myThread{};
    bool myThreadStarted{false};
    std::thread::id myThreadId;
    static void* threadEntry(void* arg);
    void threadBody(std::promise<std::string>& ready);
    std::atomic<bool> myRunning{false};
    std::atomic<bool> myQuit{false};
    std::atomic<EventHandlerState> myState{EventHandlerState::NONE};
    std::string myLastError;

    // jobs
    std::mutex myJobMutex;
    std::condition_variable myJobCv;
    std::deque<std::function<void()>> myJobs;

    // frame slot
    std::mutex myFrameMutex;
    std::vector<uInt32> myFrame;
    FrameInfo myFrameInfo;

    // pacing
    std::mutex myVsyncMutex;
    std::condition_variable myVsyncCv;
    uInt64 myVsyncSerial{0};
    Int64 myVsyncLastNs{0};
    Int64 myNextFrameNs{0};

    StellaHost(const StellaHost&) = delete;
    StellaHost& operator=(const StellaHost&) = delete;
};

#endif // STELLA_HOST_HXX
