/*
 * StellaHost -- see the header.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <chrono>
#include <cstring>

#if defined(_WIN32)
  // timeBeginPeriod: Windows sleeps in 15.6 ms steps by default, which
  // turns the per-frame sleep in pace() into ~30 fps. Asking for 1 ms
  // granularity while the emulator runs is what every emulator on Windows
  // does; it is process-scoped and undone in stop().
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <timeapi.h>
#endif

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
#include "TIADebug.hxx"
#include "TIASurface.hxx"
#include "TimerManager.hxx"

#include "host/EventHandlerFNGO.hxx"
#include "host/FNGOHostHooks.hxx"
#include "host/OSystemFNGO.hxx"
#include "host/SoundFNGO.hxx"

#include "StellaHost.hxx"
#include "../include/a2600vidcap.h"

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
  // What Stella's own frontend gets (the main thread); see StellaHost.hxx.
  constexpr size_t EMULATION_THREAD_STACK = 8u << 20;
}

// What the line and poke taps record (Stella thread only): the registers of
// the latest drawing of every visible row, and a ring of TIA writes tagged
// with the frame counter they happened under.
struct StellaHost::CapCur
{
  struct Entry { a2600vid_poke poke; uInt16 tag; };
  a2600vid_line lines[A2600VID_ROWS];
  Entry ring[A2600VID_POKES];
  int head{0}, count{0}, dropped{0};
  uInt16 rowTag{0};       // the frame counter at the latest row
  uInt16 pokeTag{0};
  uInt16 rowPokes{0};     // writes since the last row ended
};

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
#if defined(_WIN32)
  timeBeginPeriod(1);
#endif

  std::promise<std::string> ready;
  auto fut = ready.get_future();
  {
    struct Ctx { StellaHost* self; std::promise<std::string>* ready; };
    auto* ctx = new Ctx{this, &ready};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, EMULATION_THREAD_STACK);
    const int rc = pthread_create(&myThread, &attr,
      [](void* arg) -> void* {
        std::unique_ptr<Ctx> c(static_cast<Ctx*>(arg));
        c->self->threadBody(*c->ready);
        return nullptr;
      }, ctx);
    pthread_attr_destroy(&attr);
    if(rc != 0)
    {
      delete ctx;
      error = myLastError = "could not start the emulation thread";
#if defined(_WIN32)
      timeEndPeriod(1);
#endif
      return false;
    }
    myThreadStarted = true;
  }

  error = fut.get();
  if(!error.empty())
  {
    myLastError = error;
    pthread_join(myThread, nullptr);
    myThreadStarted = false;
#if defined(_WIN32)
    timeEndPeriod(1);
#endif
    return false;
  }
  return true;
}

void StellaHost::threadBody(std::promise<std::string>& ready)
{
  {
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
  }
}

void StellaHost::stop()
{
  if(!myThreadStarted)
    return;
  myQuit.store(true);
  myJobCv.notify_all();
  myVsyncCv.notify_all();
  pthread_join(myThread, nullptr);
  myThreadStarted = false;
  myRunning.store(false);
#if defined(_WIN32)
  timeEndPeriod(1);
#endif
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
  ++myFrameCount;
  releasePulses();
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

  const bool frameEnd = tia.newFramePending();
  tia.renderToFrameBuffer();
  os.frameBuffer().tiaSurface().render();
  if(frameEnd && myCapUsers.load() > 0)
    commitCapture();

  const FBSurface& surface = os.frameBuffer().tiaSurface().tiaSurface();
  uInt32* pixels = nullptr; uInt32 pitch = 0;
  surface.basePtr(pixels, pitch);
  if(!pixels) return;

  const uInt32 w = tia.width(), h = std::min(tia.height(), surface.height());

  {
    const std::lock_guard<std::mutex> lock(myFrameMutex);
    // A frame identical to the one in the slot is not a new frame: the
    // serial stays and nobody is called. The machine runs at 60 Hz whatever
    // it shows and a CONFIG screen or menu does not move, so without this
    // the frontends would copy, upload and draw the same picture sixty
    // times a second.
    bool same = myFrameInfo.serial != 0 && myFrameInfo.width == w && myFrameInfo.height == h &&
                myFrame.size() == static_cast<size_t>(w) * h;
    for(uInt32 y = 0; same && y < h; ++y)
      same = std::memcmp(&myFrame[static_cast<size_t>(y) * w], pixels + static_cast<size_t>(y) * pitch,
                         sizeof(uInt32) * w) == 0;
    if(same) return;
    myFrame.resize(static_cast<size_t>(w) * h);
    for(uInt32 y = 0; y < h; ++y)
      std::memcpy(&myFrame[static_cast<size_t>(y) * w], pixels + static_cast<size_t>(y) * pitch,
                  sizeof(uInt32) * w);
    myFrameInfo.width = w;
    myFrameInfo.height = h;
    myFrameInfo.refreshRate = console.gameRefreshRate();
    ++myFrameInfo.serial;
  }

  const std::lock_guard<std::mutex> lock(myCbMutex);
  if(myFrameCb) myFrameCb(myFrameCbCtx);
}

void StellaHost::setFrameCallback(FrameCallback cb, void* ctx)
{
  const std::lock_guard<std::mutex> lock(myCbMutex);
  myFrameCb = cb;
  myFrameCbCtx = ctx;
}

void StellaHost::pulseEvent(Event::Type type, int frames)
{
  if(!myRunning.load()) return;
  enqueueEvent(type, 1);
  const std::lock_guard<std::mutex> lock(myPulseMutex);
  myPulses.emplace_back(myFrameCount.load() + static_cast<uInt64>(frames), type);
}

void StellaHost::releasePulses()
{
  std::vector<Event::Type> due;
  {
    const std::lock_guard<std::mutex> lock(myPulseMutex);
    const uInt64 now = myFrameCount.load();
    for(size_t i = 0; i < myPulses.size();)
      if(myPulses[i].first <= now) { due.push_back(myPulses[i].second); myPulses.erase(myPulses.begin() + i); }
      else ++i;
  }
  for(Event::Type t : due) enqueueEvent(t, 0);
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
    // Only a tick stream near the machine's own rate can drive it: a 120 or
    // 144 Hz display would run it too fast, a 50 Hz one too slow. +/-8%
    // locks 59.94, 60 and 59.997 Hz panels; the rest pace by the wall clock.
    const bool recent = myVsyncLastNs != 0 && (monoNs() - myVsyncLastNs) < VSYNC_RECENT_NS &&
                        myVsyncPeriodNs > frameNs * 92 / 100 &&
                        myVsyncPeriodNs < frameNs * 108 / 100;
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
  installCaptureTaps();
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
    const Int64 now = monoNs();
    const Int64 dt = now - myVsyncLastNs;
    if(myVsyncLastNs == 0 || dt <= 0 || dt > VSYNC_RECENT_NS)
      myVsyncPeriodNs = 0;
    else if(myVsyncPeriodNs == 0)
      myVsyncPeriodNs = dt;
    else
      myVsyncPeriodNs += (dt - myVsyncPeriodNs) / 8;
    myVsyncLastNs = now;
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

// ---- the Video tab's capture -------------------------------------------

namespace {
  // The registers of the row that just ended, from the debugger's own TIA
  // state (what TIADebug shows: positions, delayed copies and all).
  void fillLine(a2600vid_line& line, TIADebug& tia)
  {
    const TiaState& st = static_cast<const TiaState&>(tia.getState());
    line.valid = 1;
    line.colup0 = static_cast<uInt8>(st.coluRegs[0]); line.colup1 = static_cast<uInt8>(st.coluRegs[1]);
    line.colupf = static_cast<uInt8>(st.coluRegs[2]); line.colubk = static_cast<uInt8>(st.coluRegs[3]);
    line.pf0 = static_cast<uInt8>(st.pf[0]); line.pf1 = static_cast<uInt8>(st.pf[1]);
    line.pf2 = static_cast<uInt8>(st.pf[2]);
    line.ctrlpf = tia.ctrlPF();
    line.grp0 = static_cast<uInt8>(st.gr[0]); line.grp1 = static_cast<uInt8>(st.gr[1]);
    line.grp0_old = static_cast<uInt8>(st.gr[2]); line.grp1_old = static_cast<uInt8>(st.gr[3]);
    line.enabl = st.gr[4] != 0; line.enabl_old = st.gr[5] != 0;
    line.enam0 = st.gr[6] != 0; line.enam1 = st.gr[7] != 0;
    line.nusiz0 = tia.nusiz0(); line.nusiz1 = tia.nusiz1();
    line.flags = static_cast<uInt8>((st.ref[0] ? A2600VID_F_REFP0 : 0) | (st.ref[1] ? A2600VID_F_REFP1 : 0) |
                                    (st.vdel[0] ? A2600VID_F_VDELP0 : 0) | (st.vdel[1] ? A2600VID_F_VDELP1 : 0) |
                                    (st.vdel[2] ? A2600VID_F_VDELBL : 0) |
                                    (st.resm[0] ? A2600VID_F_RESMP0 : 0) | (st.resm[1] ? A2600VID_F_RESMP1 : 0) |
                                    (st.vsb[1] ? A2600VID_F_VBLANK : 0));
    for(int obj = 0; obj < A2600VID_NOBJ; ++obj)
    {
      line.pos[obj] = static_cast<uInt8>(st.pos[static_cast<size_t>(obj)]);
      line.hmove[obj] = static_cast<uInt8>(st.hm[static_cast<size_t>(obj)]);
    }
    line.audf[0] = static_cast<uInt8>(st.aud[0]); line.audf[1] = static_cast<uInt8>(st.aud[1]);
    line.audc[0] = static_cast<uInt8>(st.aud[2]); line.audc[1] = static_cast<uInt8>(st.aud[3]);
    line.audv[0] = static_cast<uInt8>(st.aud[4]); line.audv[1] = static_cast<uInt8>(st.aud[5]);
    line.collisions = 0;
    for(size_t bit = 0; bit < st.cx.size() && bit < 16; ++bit)
      if(st.cx[bit]) line.collisions = static_cast<uInt16>(line.collisions | (1u << bit));
    line.pokes = 0;
  }
}

void StellaHost::captureEnable(bool on)
{
  if(!myRunning.load()) return;
  if(on) ++myCapUsers;
  else if(myCapUsers.load() > 0) --myCapUsers;
  withStella([this](OSystem&) {
    {
      const std::lock_guard<std::mutex> lock(myCapMutex);
      if(!myCapPub) myCapPub = std::make_unique<a2600vid_capture>();
    }
    installCaptureTaps();
    return 0;
  });
}

void StellaHost::installCaptureTaps()
{
  if(!myOSystem || !myOSystem->hasConsole()) return;
  TIA& tia = myOSystem->console().tia();
  if(myCapUsers.load() > 0)
  {
    myCapCur = std::make_unique<CapCur>();
    tia.setLineTap([this](int row) { captureLine(row); });
    tia.setPokeTap([this](uInt8 reg, uInt8 value, uInt8 old, int col, int row) {
      capturePoke(reg, value, old, col, row);
    });
  }
  else
  {
    tia.setLineTap(nullptr);
    tia.setPokeTap(nullptr);
  }
}

void StellaHost::captureLine(int row)
{
  if(!myCapCur || row < 0 || row >= A2600VID_ROWS || !myOSystem->hasConsole()) return;
  Console& console = myOSystem->console();
  uInt32 beamX = 0, beamY = 0;
  if(!console.tia().electronBeamPos(beamX, beamY)) return;    // not drawing the picture
  TIADebug& tia = myOSystem->debugger().tiaDebug();
  a2600vid_line& line = myCapCur->lines[row];
  fillLine(line, tia);
  line.pokes = myCapCur->rowPokes;
  myCapCur->rowPokes = 0;
  myCapCur->rowTag = static_cast<uInt16>(tia.frameCount());
}

void StellaHost::capturePoke(uInt8 reg, uInt8 value, uInt8 old, int col, int row)
{
  if(!myCapCur || !myOSystem->hasConsole()) return;
  CapCur& cur = *myCapCur;
  Console& console = myOSystem->console();
  uInt32 beamX = 0, beamY = 0;
  const bool drawing = console.tia().electronBeamPos(beamX, beamY);
  const uInt16 tag = static_cast<uInt16>(myOSystem->debugger().tiaDebug().frameCount());
  if(tag != cur.pokeTag) { cur.pokeTag = tag; cur.dropped = 0; }
  const int slot = (cur.head + cur.count) % A2600VID_POKES;
  CapCur::Entry& entry = cur.ring[slot];
  entry.poke.col = static_cast<int16_t>(col);
  entry.poke.row = static_cast<int16_t>(drawing && row >= 0 && row < A2600VID_ROWS ? row : -1);
  entry.poke.reg = reg; entry.poke.value = value; entry.poke.old = old; entry.poke.pad = 0;
  entry.tag = tag;
  if(cur.count == A2600VID_POKES) { cur.head = (cur.head + 1) % A2600VID_POKES; ++cur.dropped; }
  else ++cur.count;
  if(drawing) ++cur.rowPokes;
}

void StellaHost::commitCapture()
{
  if(!myCapCur || !myCapPub || !myOSystem->hasConsole()) return;
  const std::lock_guard<std::mutex> lock(myCapMutex);
  buildCapture(myCapPub.get(), false);
  ++myCapPub->serial;
}

// The newest frame the row tap saw, as a capture: lines from the taps, the
// picture the TIA finished last, the writes of the frame the rows belong to.
// live: also the beam and the registers right now.
void StellaHost::buildCapture(a2600vid_capture* dst, bool live)
{
  OSystem& osys = *myOSystem;
  TIA& tia = osys.console().tia();
  TIADebug& tdbg = osys.debugger().tiaDebug();
  CapCur& cur = *myCapCur;
  const uInt32 high = std::min<uInt32>(tia.height(), A2600VID_ROWS);
  const size_t count = static_cast<size_t>(A2600VID_FB_W) * high;

  dst->fb_w = A2600VID_FB_W;
  dst->fb_h = static_cast<int>(high);
  dst->vis_top = static_cast<int>(tia.startLine());
  dst->total_lines = static_cast<int>(tia.scanlinesLastFrame());
  if(dst->total_lines <= 0) dst->total_lines = 262;
  dst->pal = tia.frameLayout() == FrameLayout::pal;
  dst->frame = static_cast<uint64_t>(tdbg.frameCount());
  dst->wsync_cycles = tdbg.frameWsyncCycles();

  const uInt8* src = tia.frameBuffer();
  const TIASurface& surface = osys.frameBuffer().tiaSurface();
  for(size_t pix = 0; pix < count; ++pix)
  {
    dst->idx[pix] = src[pix];
    dst->pixels[pix] = surface.mapIndexedPixel(src[pix]);
  }
  std::memcpy(dst->lines, cur.lines, sizeof dst->lines);

  dst->npokes = 0;
  dst->pokes_dropped = cur.dropped;
  for(int ent = 0; ent < cur.count; ++ent)
  {
    const CapCur::Entry& entry = cur.ring[(cur.head + ent) % A2600VID_POKES];
    if(entry.tag == cur.rowTag && dst->npokes < A2600VID_POKES)
      dst->pokes[dst->npokes++] = entry.poke;
  }

  std::memset(&dst->now, 0, sizeof dst->now);
  dst->vpos = static_cast<int>(tia.scanlinesLastFrame());
  dst->hpos = 0;
  dst->beam_row = dst->fb_h;
  if(live)
  {
    uInt32 beamX = 0, beamY = 0;
    const bool drawing = tia.electronBeamPos(beamX, beamY);
    dst->vpos = static_cast<int>(tia.scanlines());
    dst->hpos = static_cast<int>(tia.clocksThisLine());
    // rows above the beam are this frame; between the picture and the next
    // frame's first row there is none yet (or, past the picture, all)
    dst->beam_row = drawing ? static_cast<int>(std::min<uInt32>(beamY, high))
                            : (dst->vpos >= dst->vis_top + dst->fb_h ? dst->fb_h : 0);
    fillLine(dst->now, tdbg);
    dst->serial = dst->frame > 0 ? 1 : 0;
  }
}

bool StellaHost::captureGet(a2600vid_capture* dst, uInt64* serialInOut)
{
  const std::lock_guard<std::mutex> lock(myCapMutex);
  if(!myCapPub || myCapPub->serial == 0) return false;
  if(serialInOut && *serialInOut == myCapPub->serial) return false;
  std::memcpy(static_cast<void*>(dst), myCapPub.get(), sizeof *dst);
  if(serialInOut) *serialInOut = myCapPub->serial;
  return true;
}

bool StellaHost::captureFill(a2600vid_capture* dst)
{
  return withStella([this, dst](OSystem& osys) {
    if(!osys.hasConsole() || !myCapCur) return false;
    buildCapture(dst, true);
    return true;
  });
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
