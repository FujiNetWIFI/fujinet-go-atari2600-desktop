/*
 * StellaHost -- see the header.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <chrono>
#include <cstring>

#include "CartFUJI.hxx"
#include "Console.hxx"
#include "Debugger.hxx"
#include "DebuggerParser.hxx"
#include "DispatchResult.hxx"
#include "EmulationTiming.hxx"
#include "EventHandler.hxx"
#include "FBSurface.hxx"
#include "FrameBuffer.hxx"
#include "FSNode.hxx"
#include "OSystem.hxx"
#include "Sound.hxx"
#include "TIA.hxx"
#include "TIASurface.hxx"
#include "TimerManager.hxx"

#include "host/EventHandlerFNGO.hxx"
#include "host/FNGOHostHooks.hxx"
#include "host/OSystemFNGO.hxx"
#include "host/SoundFNGO.hxx"

#include "StellaHost.hxx"

namespace {
  using Clock = std::chrono::steady_clock;

  Int64 monoNs()
  {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
      Clock::now().time_since_epoch()).count();
  }

  // A vsync stream counts as live if a tick arrived within this window.
  // Asked BEFORE blocking on a tick: waiting for one that never comes would
  // cost the timeout on every frame and halve the frame rate of a frontend
  // with no frame clock -- headless tests included.
  constexpr Int64 VSYNC_RECENT_NS = 250'000'000;
}

StellaHost::StellaHost() = default;

StellaHost::~StellaHost()
{
  stop();
}

OSystem* StellaHost::osystem() const
{
  return myOSystem.get();
}

// ---- lifecycle ---------------------------------------------------------

bool StellaHost::start(const Config& config, std::string& error)
{
  if(myRunning.load())
    return true;

  myConfig = config;
  myQuit.store(false);
  myLastError.clear();

  std::promise<std::string> ready;
  auto fut = ready.get_future();
  myThread = std::thread([this, &ready] {
    myThreadId = std::this_thread::get_id();

    FNGOHostHooks::baseDir() = myConfig.baseDir;
    myOSystem = std::make_unique<OSystemFNGO>();
    myOSystem->onStateChanged = [this](EventHandlerState s) { onStateChanged(s); };

    // Settings this host depends on, whatever the caller passed. The
    // companion TIA window would open a second backend; HiDPI would double
    // every offscreen dialog; the Time Machine would snapshot every frame
    // for a rewind buffer nobody asked for.
    Settings::Options options = myConfig.options;
    options["dbg.tiawindow"] = false;
    options["hidpi"] = false;
    options["plr.timemachine"] = false;
    options["dev.timemachine"] = false;
    options["uimessages"] = false;
    options["debug"] = false;
    if(!options.contains("audio.sample_rate"))
      options["audio.sample_rate"] = static_cast<Int32>(myConfig.audioRate);

    std::string err;
    if(!myOSystem->initialize(options))
      err = "Stella could not initialise";
    else
    {
      auto* snd = dynamic_cast<SoundFNGO*>(&myOSystem->sound());
      if(snd)
        snd->setOutputFormat(myConfig.audioRate, myConfig.audioStereo);
      err = createConsoleFrom(myConfig.startRom);
    }

    myRunning.store(err.empty());
    ready.set_value(err);
    if(err.empty())
    {
      myNextFrameNs = monoNs();
      threadMain();
    }

    // The OSystem (and its console, cartridge worker and socket) must die on
    // the thread that owned it.
    myOSystem.reset();
    myRunning.store(false);
  });

  error = fut.get();
  if(!error.empty())
  {
    myLastError = error;
    if(myThread.joinable())
      myThread.join();
    return false;
  }
  return true;
}

void StellaHost::stop()
{
  if(!myThread.joinable())
    return;
  myQuit.store(true);
  myJobCv.notify_all();
  myVsyncCv.notify_all();
  myThread.join();
  myRunning.store(false);
}

// ---- jobs --------------------------------------------------------------

void StellaHost::post(std::function<void()> job)
{
  {
    const std::lock_guard<std::mutex> lock(myJobMutex);
    myJobs.push_back(std::move(job));
  }
  myJobCv.notify_all();
  myVsyncCv.notify_all();
}

void StellaHost::runJobs()
{
  for(;;)
  {
    std::function<void()> job;
    {
      const std::lock_guard<std::mutex> lock(myJobMutex);
      if(myJobs.empty()) return;
      job = std::move(myJobs.front());
      myJobs.pop_front();
    }
    job();
  }
}

bool StellaHost::waitForJob()
{
  std::unique_lock<std::mutex> lock(myJobMutex);
  return myJobCv.wait_for(lock, std::chrono::milliseconds(50),
                          [this] { return !myJobs.empty() || myQuit.load(); });
}

// ---- the Stella thread -------------------------------------------------

void StellaHost::threadMain()
{
  while(!myQuit.load())
  {
    runJobs();
    if(myQuit.load()) break;

    if(!myOSystem->hasConsole()
       || myOSystem->eventHandler().state() != EventHandlerState::EMULATION)
    {
      // Stopped in the debugger, paused, or between consoles: only jobs
      // run, and the audio device keeps pulling silence.
      waitForJob();
      continue;
    }
    emulateFrame();
  }
}

void StellaHost::emulateFrame()
{
  OSystem& os = *myOSystem;
  Console& console = os.console();
  TIA& tia = console.tia();
  const EmulationTiming& timing = console.emulationTiming();

  // pollInput (our EventHandlerFNGO::pollEvent inside the input window),
  // then RIOT port latching and the Time Machine -- the same per-frame
  // housekeeping OSystem::mainLoop does.
  os.eventHandler().poll(TimerManager::getTicks());

  DispatchResult result;
  uInt64 cycles = 0;
  do
  {
    tia.update(result, timing.maxCyclesPerTimeslice());
    cycles += result.getCycles();
  }
  while(result.getStatus() == DispatchResult::Status::ok && !tia.newFramePending());

  // A FujiNet network boot swapped the cartridge image in (CartridgeFUJI's
  // doSwap); OSystem::dispatchEmulation does this with the CPU stopped, and
  // so do we.
  if(console.cartridge().takePendingSwap())
  {
    console.cartridgeSwapped();
    if(myCallbacks.onConsoleReplaced) myCallbacks.onConsoleReplaced();
  }

  switch(result.getStatus())
  {
    case DispatchResult::Status::ok:
      if(tia.newFramePending())
        publishFrame();
      if(os.eventHandler().frying())
        console.fry();
      break;

    case DispatchResult::Status::debugger:
    {
      const std::string msg = result.getMessage();
      const int addr = result.getAddress();
      os.debugger().start(msg, addr, result.wasReadTrap(), result.getToolTip());
      publishFrame();
      enterStopped(StopReason::Breakpoint, msg, addr);
      return;
    }

    case DispatchResult::Status::fatal:
    {
      const std::string msg = result.getMessage();
      os.debugger().start(msg);
      publishFrame();
      enterStopped(StopReason::Fatal, msg, -1);
      return;
    }

    default:
      break;
  }

  pace(cycles);
}

void StellaHost::publishFrame()
{
  OSystem& os = *myOSystem;
  Console& console = os.console();
  TIA& tia = console.tia();

  tia.renderToFrameBuffer();
  os.frameBuffer().tiaSurface().render();

  const FBSurface& surface = os.frameBuffer().tiaSurface().tiaSurface();
  uInt32* pixels = nullptr; uInt32 pitch = 0;
  surface.basePtr(pixels, pitch);
  if(!pixels) return;

  const uInt32 w = tia.width(), h = std::min(tia.height(), surface.height());

  const std::lock_guard<std::mutex> lock(myFrameMutex);
  myFrame.resize(static_cast<size_t>(w) * h);
  for(uInt32 y = 0; y < h; ++y)
    std::memcpy(&myFrame[static_cast<size_t>(y) * w], pixels + static_cast<size_t>(y) * pitch,
                sizeof(uInt32) * w);
  myFrameInfo.width = w;
  myFrameInfo.height = h;
  myFrameInfo.refreshRate = console.gameRefreshRate();
  ++myFrameInfo.serial;
}

void StellaHost::pace(uInt64 cyclesRun)
{
  const uInt32 cps = myOSystem->console().emulationTiming().cyclesPerSecond();
  const Int64 frameNs = cps ? static_cast<Int64>(cyclesRun * 1'000'000'000ULL / cps)
                            : 16'666'667;

  // Phase lock: while the frontend's frame clock is ticking, run one frame
  // per tick and inherit the display's cadence exactly (no slow beat
  // between the machine's rate and the panel's).
  {
    std::unique_lock<std::mutex> lock(myVsyncMutex);
    const bool recent = myVsyncLastNs != 0 && (monoNs() - myVsyncLastNs) < VSYNC_RECENT_NS;
    if(recent)
    {
      const uInt64 seen = myVsyncSerial;
      const bool ticked = myVsyncCv.wait_for(lock, std::chrono::nanoseconds(2 * frameNs),
        [&] { return myVsyncSerial != seen || myQuit.load(); });
      // Keep the wall-clock ladder rebased so dropping back out is seamless.
      myNextFrameNs = monoNs();
      if(ticked) return;
    }
  }

  // Absolute deadline ladder; resync when badly behind (a laptop resume,
  // a debugger stop) rather than fast-forwarding through the missed frames.
  myNextFrameNs += frameNs;
  const Int64 now = monoNs();
  const Int64 behind = now - myNextFrameNs;
  if(behind > 4 * frameNs)
    myNextFrameNs = now;
  else if(behind < 0)
    std::this_thread::sleep_for(std::chrono::nanoseconds(-behind));
}

void StellaHost::enterStopped(StopReason reason, const std::string& msg, int addr)
{
  if(myCallbacks.onStopped) myCallbacks.onStopped(reason, msg, addr);
}

void StellaHost::onStateChanged(EventHandlerState state)
{
  const EventHandlerState old = myState.exchange(state);
  if(old == EventHandlerState::DEBUGGER && state == EventHandlerState::EMULATION)
  {
    myNextFrameNs = monoNs();
    if(myCallbacks.onResumed) myCallbacks.onResumed();
  }
}

// ---- console lifecycle -------------------------------------------------

void StellaHost::disconnectCartridge()
{
  // The in-process FujiNet's BoIP listener has a backlog of one, and
  // createConsole builds the new console -- whose CartridgeFUJI dials in
  // from its worker thread -- BEFORE destroying the old one. Close the old
  // link first or the new one hangs behind it.
  if(!myOSystem->hasConsole()) return;
  if(auto* fuji = dynamic_cast<CartridgeFUJI*>(&myOSystem->console().cartridge()))
    fuji->enableFujiNet(false);
}

std::string StellaHost::createConsoleFrom(const std::string& path)
{
  OSystem& os = *myOSystem;
  disconnectCartridge();

  std::string err;
  if(path.empty())
    err = os.createConsole(os.fujiNetClientROM());
  else
    err = os.createConsole(FSNode(path));

  if(!err.empty())
  {
    myLastError = err;
    if(myCallbacks.onError) myCallbacks.onError(err);
    return err;
  }
  myNextFrameNs = monoNs();
  if(myCallbacks.onConsoleReplaced) myCallbacks.onConsoleReplaced();
  return {};
}

std::string StellaHost::loadRom(const std::string& path)
{
  return withStella([this, path](OSystem&) { return createConsoleFrom(path); });
}

std::string StellaHost::rebootToConfig()
{
  return withStella([this](OSystem&) { return createConsoleFrom(""); });
}

void StellaHost::reset()
{
  withStella([](OSystem& os) {
    if(os.hasConsole()) os.console().system().reset();
    return 0;
  });
}

// ---- video / audio / input --------------------------------------------

bool StellaHost::copyFrame(std::vector<uInt32>& dst, FrameInfo& info, uInt64* serialInOut)
{
  const std::lock_guard<std::mutex> lock(myFrameMutex);
  if(myFrameInfo.serial == 0) return false;
  if(serialInOut && *serialInOut == myFrameInfo.serial) return false;
  dst = myFrame;
  info = myFrameInfo;
  if(serialInOut) *serialInOut = myFrameInfo.serial;
  return true;
}

void StellaHost::notifyVsync(Int64)
{
  {
    const std::lock_guard<std::mutex> lock(myVsyncMutex);
    myVsyncLastNs = monoNs();
    ++myVsyncSerial;
  }
  myVsyncCv.notify_all();
}

void StellaHost::fillAudio(float* out, uInt32 frames)
{
  // The Sound object belongs to the OSystem, which is created and destroyed
  // on the Stella thread; between start() returning and stop() it is stable.
  if(!myRunning.load() || !myOSystem)
  {
    std::memset(out, 0, sizeof(float) * frames * (myConfig.audioStereo ? 2 : 1));
    return;
  }
  auto* snd = dynamic_cast<SoundFNGO*>(&myOSystem->sound());
  if(snd) snd->fill(out, frames);
  else std::memset(out, 0, sizeof(float) * frames * (myConfig.audioStereo ? 2 : 1));
}

void StellaHost::enqueueEvent(Event::Type type, Int32 value)
{
  if(!myRunning.load() || !myOSystem) return;
  auto* eh = dynamic_cast<EventHandlerFNGO*>(&myOSystem->eventHandler());
  if(eh) eh->enqueue(type, value);
}

// ---- debugger ----------------------------------------------------------

std::string StellaHost::dbgRun(const std::string& command)
{
  return withStella([command](OSystem& os) -> std::string {
    if(!os.hasConsole()) return "no console";
    if(os.eventHandler().state() != EventHandlerState::DEBUGGER)
      os.eventHandler().enterDebugMode();
    std::string out = os.debugger().run(command);
    // Stella's own prompt strips these; they are protocol, not text.
    for(const std::string_view sentinel : { DebuggerParser::kExitDebugger,
                                            DebuggerParser::kNoPrompt })
      for(size_t p = out.find(sentinel); p != std::string::npos; p = out.find(sentinel))
        out.erase(p, sentinel.size());
    return out;
  });
}

void StellaHost::dbgBreak()
{
  withStella([this](OSystem& os) {
    if(os.hasConsole() && os.eventHandler().state() != EventHandlerState::DEBUGGER)
    {
      os.eventHandler().enterDebugMode();
      publishFrame();
      enterStopped(StopReason::Paused, "", -1);
    }
    return 0;
  });
}

void StellaHost::dbgContinue()
{
  withStella([](OSystem& os) {
    if(os.hasConsole() && os.eventHandler().state() == EventHandlerState::DEBUGGER)
      os.debugger().run("run");
    return 0;
  });
}
