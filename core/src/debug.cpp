/*
 * debug.cpp -- the debugger contract (core/include/a2600debug.h) over
 * Stella's own engine, through StellaHost jobs.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "BreakpointMap.hxx"
#include "Cart.hxx"
#include "CartDebug.hxx"
#include "Console.hxx"
#include "CpuDebug.hxx"
#include "Debugger.hxx"
#include "DebuggerParser.hxx"
#include "EventHandler.hxx"
#include "FrameBuffer.hxx"
#include "M6502.hxx"
#include "OSystem.hxx"
#include "RiotDebug.hxx"
#include "StateManager.hxx"
#include "System.hxx"
#include "TIA.hxx"
#include "TIADebug.hxx"
#include "TIASurface.hxx"
#include "TrapArray.hxx"

#include "StellaHost.hxx"

extern "C" {
#include "session_internal.h"
#include "a2600debug.h"
}

StellaHost* a2600session_host(a2600session* s);

struct a2600debug
{
  a2600session* session{nullptr};
  StellaHost* host{nullptr};
  std::atomic<unsigned> generation{0};
  std::mutex mutex;
  std::string reason;
  int reasonAddr{-1};
};

namespace {

bool stopped(OSystem& os)
{
  return os.hasConsole() && os.eventHandler().state() == EventHandlerState::DEBUGGER;
}

// Every inspection needs the debugger's state locked; a running machine is
// asked at a frame boundary by the job mechanism itself, so only the
// "no console" case is refused.
template<typename F>
auto with_dbg(a2600debug* d, F&& fn) -> decltype(fn(std::declval<OSystem&>()))
{
  return d->host->withStella([&](OSystem& os) {
    return fn(os);
  });
}

int put(char* dst, int dstsz, const std::string& s)
{
  if(!dst || dstsz <= 0) return 0;
  return snprintf(dst, static_cast<size_t>(dstsz), "%s", s.c_str());
}

} // namespace

extern "C" a2600debug* a2600debug_get(a2600session* s)
{
  if(!s) return nullptr;
  if(!s->debugger)
  {
    auto* d = new a2600debug();
    d->session = s;
    d->host = a2600session_host(s);
    s->debugger = d;
  }
  return static_cast<a2600debug*>(s->debugger);
}

extern "C" int a2600debug_is_stopped(a2600debug* d)
{
  return d && d->host->isStopped() ? 1 : 0;
}

extern "C" void a2600debug_stop(a2600debug* d)
{
  if(!d || !d->session->running) return;
  d->host->dbgBreak();
  {
    const std::lock_guard<std::mutex> lock(d->mutex);
    d->reason = "stopped";
    d->reasonAddr = -1;
  }
  ++d->generation;
}

extern "C" void a2600debug_resume(a2600debug* d)
{
  if(!d || !d->session->running) return;
  d->host->dbgContinue();
  ++d->generation;
}

extern "C" int a2600debug_stop_reason(a2600debug* d, char* dst, int dstsz, int* address)
{
  if(!d) return 0;
  const std::lock_guard<std::mutex> lock(d->mutex);
  if(address) *address = d->reasonAddr;
  return put(dst, dstsz, d->reason);
}

extern "C" unsigned a2600debug_generation(a2600debug* d)
{
  return d ? d->generation.load() : 0;
}

void a2600debug_destroy(a2600session* s)
{
  if(!s || !s->debugger) return;
  delete static_cast<a2600debug*>(s->debugger);
  s->debugger = nullptr;
}

// Called by session.cpp's stop callback so windows see why.
void a2600debug_note_stop(a2600session* s, const char* msg, int addr)
{
  auto* d = a2600debug_get(s);
  if(!d) return;
  {
    const std::lock_guard<std::mutex> lock(d->mutex);
    d->reason = msg ? msg : "";
    d->reasonAddr = addr;
  }
  ++d->generation;
}

extern "C" int a2600debug_command(a2600debug* d, const char* command, char* dst, int dstsz)
{
  if(!d || !command || !d->session->running) return put(dst, dstsz, "");
  std::string cmd(command);
  // Trim, and refuse the two commands that cannot work headless: exitRom
  // opens Stella's own launcher, stepWhile pumps an offscreen progress
  // dialog whose Cancel is unreachable.
  while(!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r' || cmd.back() == ' '))
    cmd.pop_back();
  std::string lower;
  for(char c : cmd) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if(lower.rfind("exitrom", 0) == 0)
    return put(dst, dstsz, "exitRom is not available here: use Eject Cartridge");
  if(lower.rfind("stepwhile", 0) == 0)
    return put(dst, dstsz, "stepWhile is not available here (it cannot be cancelled)");
  const std::string out = d->host->dbgRun(cmd);
  ++d->generation;
  return put(dst, dstsz, out);
}

extern "C" int a2600debug_completions(a2600debug* d, const char* prefix, char* dst, int dstsz)
{
  if(!d || !prefix || !dst || dstsz <= 0) return 0;
  dst[0] = '\0';
  if(!d->session->running) return 0;
  const std::string pre(prefix);
  return with_dbg(d, [&](OSystem& os) {
    if(!os.hasConsole()) return 0;
    // The parser knows the commands; the debugger knows labels, functions
    // and pseudo-registers. A prompt wants both.
    StringList list;
    DebuggerParser::getCompletions(pre, list);
    os.debugger().getCompletions(pre, list);
    int len = 0, n = 0;
    for(const auto& s : list)
    {
      if(len + static_cast<int>(s.size()) + 2 >= dstsz) break;
      len += snprintf(dst + len, static_cast<size_t>(dstsz - len), "%s\n", s.c_str());
      ++n;
    }
    return n;
  });
}

// ---- stepping ----------------------------------------------------------------

namespace {
void run_quiet(a2600debug* d, const std::string& cmd)
{
  if(!d || !d->session->running) return;
  d->host->dbgRun(cmd);
  ++d->generation;
}
}

extern "C" void a2600debug_step(a2600debug* d) { run_quiet(d, "step"); }
extern "C" void a2600debug_trace(a2600debug* d) { run_quiet(d, "trace"); }
extern "C" void a2600debug_scanline(a2600debug* d, int n) { run_quiet(d, "scanLine " + std::to_string(n > 0 ? n : 1)); }
extern "C" void a2600debug_frame(a2600debug* d, int n) { run_quiet(d, "frame " + std::to_string(n > 0 ? n : 1)); }
extern "C" void a2600debug_rewind(a2600debug* d, int n) { run_quiet(d, "rewind " + std::to_string(n > 0 ? n : 1)); }
extern "C" void a2600debug_unwind(a2600debug* d, int n) { run_quiet(d, "unwind " + std::to_string(n > 0 ? n : 1)); }
extern "C" void a2600debug_run_to(a2600debug* d, uint16_t addr)
{
  char buf[32];
  snprintf(buf, sizeof buf, "runToPc $%04x", addr);
  run_quiet(d, buf);
}

// ---- CPU -----------------------------------------------------------------------

extern "C" void a2600debug_cpu_get(a2600debug* d, a2600debug_cpu* out)
{
  if(!out) return;
  memset(out, 0, sizeof *out);
  if(!d || !d->session->running) return;
  with_dbg(d, [&](OSystem& os) {
    if(!os.hasConsole()) return 0;
    CpuDebug& cpu = os.debugger().cpuDebug();
    out->pc = cpu.pc(); out->sp = cpu.sp(); out->a = cpu.a(); out->x = cpu.x(); out->y = cpu.y();
    out->n = cpu.n(); out->v = cpu.v(); out->b = cpu.b(); out->d = cpu.d();
    out->i = cpu.i(); out->z = cpu.z(); out->c = cpu.c();
    // CpuDebug exposes the flags, not the packed register; bit 5 is always set
    out->ps = (out->n << 7) | (out->v << 6) | 0x20 | (out->b << 4) | (out->d << 3)
            | (out->i << 2) | (out->z << 1) | out->c;
    out->cycles = cpu.icycles();
    out->total_cycles = os.console().system().cycles();
    return 0;
  });
}

extern "C" void a2600debug_cpu_set(a2600debug* d, int reg, int value)
{
  if(!d || !d->session->running) return;
  with_dbg(d, [&](OSystem& os) {
    if(!os.hasConsole()) return 0;
    CpuDebug& cpu = os.debugger().cpuDebug();
    switch(reg)
    {
      case A2600_REG_PC: cpu.setPC(value); break;
      case A2600_REG_SP: cpu.setSP(value); break;
      case A2600_REG_A: cpu.setA(value); break;
      case A2600_REG_X: cpu.setX(value); break;
      case A2600_REG_Y: cpu.setY(value); break;
      case A2600_REG_PS: cpu.setPS(value); break;
      case A2600_FLAG_N: cpu.setN(value != 0); break;
      case A2600_FLAG_V: cpu.setV(value != 0); break;
      case A2600_FLAG_B: cpu.setB(value != 0); break;
      case A2600_FLAG_D: cpu.setD(value != 0); break;
      case A2600_FLAG_I: cpu.setI(value != 0); break;
      case A2600_FLAG_Z: cpu.setZ(value != 0); break;
      case A2600_FLAG_C: cpu.setC(value != 0); break;
      default: break;
    }
    return 0;
  });
  ++d->generation;
}

// ---- RIOT ----------------------------------------------------------------------

extern "C" void a2600debug_riot_get(a2600debug* d, a2600debug_riot* out)
{
  if(!out) return;
  memset(out, 0, sizeof *out);
  if(!d || !d->session->running) return;
  with_dbg(d, [&](OSystem& os) {
    if(!os.hasConsole()) return 0;
    RiotDebug& r = os.debugger().riotDebug();
    out->swcha = r.swcha(); out->swacnt = r.swacnt();
    out->swchb = r.swchb(); out->swbcnt = r.swbcnt();
    for(int i = 0; i < 6; ++i) out->inpt[i] = r.inpt(i);
    out->intim = r.intim(); out->timint = r.timint();
    out->tim_clocks = r.timClocks(); out->tim_divider = r.timDivider();
    out->select = r.select(); out->reset = r.reset(); out->color = r.tvType();
    out->diff_left_a = r.diffP0(); out->diff_right_a = r.diffP1();
    snprintf(out->dir_left, sizeof out->dir_left, "%s", r.dirP0String().c_str());
    snprintf(out->dir_right, sizeof out->dir_right, "%s", r.dirP1String().c_str());
    return 0;
  });
}

extern "C" void a2600debug_riot_set(a2600debug* d, int reg, int value)
{
  if(!d || !d->session->running) return;
  with_dbg(d, [&](OSystem& os) {
    if(!os.hasConsole()) return 0;
    RiotDebug& r = os.debugger().riotDebug();
    switch(reg)
    {
      case A2600_RIOT_SWCHA: r.swcha(value); break;
      case A2600_RIOT_SWACNT: r.swacnt(value); break;
      case A2600_RIOT_SWCHB: r.swchb(value); break;
      case A2600_RIOT_SWBCNT: r.swbcnt(value); break;
      case A2600_RIOT_TIM1T: r.tim1T(value); break;
      case A2600_RIOT_TIM8T: r.tim8T(value); break;
      case A2600_RIOT_TIM64T: r.tim64T(value); break;
      case A2600_RIOT_TIM1024T: r.tim1024T(value); break;
      case A2600_RIOT_SELECT: r.select(value); break;
      case A2600_RIOT_RESET: r.reset(value); break;
      case A2600_RIOT_COLOR: r.tvType(value); break;
      case A2600_RIOT_DIFF_LEFT: r.diffP0(value); break;
      case A2600_RIOT_DIFF_RIGHT: r.diffP1(value); break;
      default: break;
    }
    return 0;
  });
  ++d->generation;
}

// ---- TIA -----------------------------------------------------------------------

extern "C" void a2600debug_tia_get(a2600debug* d, a2600debug_tia* out)
{
  if(!out) return;
  memset(out, 0, sizeof *out);
  out->beam_x = out->beam_y = -1;
  if(!d || !d->session->running) return;
  with_dbg(d, [&](OSystem& os) {
    if(!os.hasConsole()) return 0;
    TIADebug& t = os.debugger().tiaDebug();
    out->nusiz0 = t.nusiz0(); out->nusiz1 = t.nusiz1();
    out->colup0 = t.coluP0(); out->colup1 = t.coluP1(); out->colupf = t.coluPF(); out->colubk = t.coluBK();
    out->ctrlpf = t.ctrlPF(); out->pf0 = t.pf0(); out->pf1 = t.pf1(); out->pf2 = t.pf2();
    out->grp0 = t.grP0(); out->grp1 = t.grP1();
    out->pos_p0 = t.posP0(); out->pos_p1 = t.posP1(); out->pos_m0 = t.posM0(); out->pos_m1 = t.posM1(); out->pos_bl = t.posBL();
    out->hm_p0 = t.hmP0(); out->hm_p1 = t.hmP1(); out->hm_m0 = t.hmM0(); out->hm_m1 = t.hmM1(); out->hm_bl = t.hmBL();
    out->audc0 = t.audC0(); out->audc1 = t.audC1(); out->audf0 = t.audF0(); out->audf1 = t.audF1();
    out->audv0 = t.audV0(); out->audv1 = t.audV1();
    snprintf(out->aud_freq0, sizeof out->aud_freq0, "%s", t.audFreq0().c_str());
    snprintf(out->aud_freq1, sizeof out->aud_freq1, "%s", t.audFreq1().c_str());
    out->refp0 = t.refP0(); out->refp1 = t.refP1(); out->enam0 = t.enaM0(); out->enam1 = t.enaM1(); out->enabl = t.enaBL();
    out->vdelp0 = t.vdelP0(); out->vdelp1 = t.vdelP1(); out->vdelbl = t.vdelBL();
    out->resmp0 = t.resMP0(); out->resmp1 = t.resMP1();
    out->refpf = t.refPF(); out->scorepf = t.scorePF(); out->pripf = t.priorityPF();
    out->vsync = t.vsync(); out->vblank = t.vblank();
    const bool coll[15] = {
      t.collM0_P1(), t.collM0_P0(), t.collM1_P0(), t.collM1_P1(), t.collP0_PF(),
      t.collP0_BL(), t.collP1_PF(), t.collP1_BL(), t.collM0_PF(), t.collM0_BL(),
      t.collM1_PF(), t.collM1_BL(), t.collBL_PF(), t.collP0_P1(), t.collM0_M1() };
    for(int i = 0; i < 15; ++i) if(coll[i]) out->collisions |= static_cast<uint16_t>(1u << i);
    out->scanlines = t.scanlines(); out->scanlines_last = t.scanlinesLastFrame();
    out->frame_count = t.frameCount(); out->frame_cycles = t.frameCycles();
    out->wsync_cycles = t.frameWsyncCycles();
    out->clocks_this_line = t.clocksThisLine(); out->cycles_this_line = t.cyclesThisLine();
    uInt32 bx = 0, by = 0;
    if(os.console().tia().electronBeamPos(bx, by)) { out->beam_x = static_cast<int>(bx); out->beam_y = static_cast<int>(by); }
    return 0;
  });
}

extern "C" int a2600debug_tia_set(a2600debug* d, const char* reg, int value)
{
  if(!d || !reg || !d->session->running) return -1;
  const std::string name(reg);
  const int rc = with_dbg(d, [&](OSystem& os) {
    if(!os.hasConsole()) return -1;
    TIADebug& t = os.debugger().tiaDebug();
    struct { const char* n; void (*f)(TIADebug&, int); } table[] = {
      { "nusiz0", [](TIADebug& t, int v) { t.nusiz0(v); } }, { "nusiz1", [](TIADebug& t, int v) { t.nusiz1(v); } },
      { "colup0", [](TIADebug& t, int v) { t.coluP0(v); } }, { "colup1", [](TIADebug& t, int v) { t.coluP1(v); } },
      { "colupf", [](TIADebug& t, int v) { t.coluPF(v); } }, { "colubk", [](TIADebug& t, int v) { t.coluBK(v); } },
      { "ctrlpf", [](TIADebug& t, int v) { t.ctrlPF(v); } },
      { "pf0", [](TIADebug& t, int v) { t.pf0(v); } }, { "pf1", [](TIADebug& t, int v) { t.pf1(v); } }, { "pf2", [](TIADebug& t, int v) { t.pf2(v); } },
      { "grp0", [](TIADebug& t, int v) { t.grP0(v); } }, { "grp1", [](TIADebug& t, int v) { t.grP1(v); } },
      { "posp0", [](TIADebug& t, int v) { t.posP0(v); } }, { "posp1", [](TIADebug& t, int v) { t.posP1(v); } },
      { "posm0", [](TIADebug& t, int v) { t.posM0(v); } }, { "posm1", [](TIADebug& t, int v) { t.posM1(v); } },
      { "posbl", [](TIADebug& t, int v) { t.posBL(v); } },
      { "hmp0", [](TIADebug& t, int v) { t.hmP0(v); } }, { "hmp1", [](TIADebug& t, int v) { t.hmP1(v); } },
      { "hmm0", [](TIADebug& t, int v) { t.hmM0(v); } }, { "hmm1", [](TIADebug& t, int v) { t.hmM1(v); } },
      { "hmbl", [](TIADebug& t, int v) { t.hmBL(v); } },
      { "audc0", [](TIADebug& t, int v) { t.audC0(v); } }, { "audc1", [](TIADebug& t, int v) { t.audC1(v); } },
      { "audf0", [](TIADebug& t, int v) { t.audF0(v); } }, { "audf1", [](TIADebug& t, int v) { t.audF1(v); } },
      { "audv0", [](TIADebug& t, int v) { t.audV0(v); } }, { "audv1", [](TIADebug& t, int v) { t.audV1(v); } },
      { "refp0", [](TIADebug& t, int v) { t.refP0(v); } }, { "refp1", [](TIADebug& t, int v) { t.refP1(v); } },
      { "enam0", [](TIADebug& t, int v) { t.enaM0(v); } }, { "enam1", [](TIADebug& t, int v) { t.enaM1(v); } },
      { "enabl", [](TIADebug& t, int v) { t.enaBL(v); } },
      { "vdelp0", [](TIADebug& t, int v) { t.vdelP0(v); } }, { "vdelp1", [](TIADebug& t, int v) { t.vdelP1(v); } },
      { "vdelbl", [](TIADebug& t, int v) { t.vdelBL(v); } },
      { "resmp0", [](TIADebug& t, int v) { t.resMP0(v); } }, { "resmp1", [](TIADebug& t, int v) { t.resMP1(v); } },
      { "refpf", [](TIADebug& t, int v) { t.refPF(v); } }, { "scorepf", [](TIADebug& t, int v) { t.scorePF(v); } },
      { "pripf", [](TIADebug& t, int v) { t.priorityPF(v); } },
      { "vsync", [](TIADebug& t, int v) { t.vsync(v); } }, { "vblank", [](TIADebug& t, int v) { t.vblank(v); } },
    };
    for(const auto& e : table)
      if(name == e.n) { e.f(t, value); return 0; }
    return -1;
  });
  ++d->generation;
  return rc;
}

extern "C" int a2600debug_tia_strobe(a2600debug* d, const char* name)
{
  if(!d || !name || !d->session->running) return -1;
  const std::string n(name);
  const int rc = with_dbg(d, [&](OSystem& os) {
    if(!os.hasConsole()) return -1;
    TIADebug& t = os.debugger().tiaDebug();
    if(n == "wsync") t.strobeWsync(); else if(n == "rsync") t.strobeRsync();
    else if(n == "resp0") t.strobeResP0(); else if(n == "resp1") t.strobeResP1();
    else if(n == "resm0") t.strobeResM0(); else if(n == "resm1") t.strobeResM1();
    else if(n == "resbl") t.strobeResBL(); else if(n == "hmove") t.strobeHmove();
    else if(n == "hmclr") t.strobeHmclr(); else if(n == "cxclr") t.strobeCxclr();
    else return -1;
    return 0;
  });
  ++d->generation;
  return rc;
}

extern "C" uint32_t a2600debug_tia_color(a2600debug* d, uint8_t index)
{
  if(!d || !d->session->running) return 0;
  return with_dbg(d, [&](OSystem& os) -> uint32_t {
    if(!os.hasConsole()) return 0;
    return os.frameBuffer().tiaSurface().mapIndexedPixel(index);
  });
}

extern "C" int a2600debug_frame_snapshot(a2600debug* d, uint32_t* dst, int partial)
{
  if(!d || !dst || !d->session->running) return 0;
  return with_dbg(d, [&](OSystem& os) {
    if(!os.hasConsole()) return 0;
    TIA& tia = os.console().tia();
    TIASurface& surf = os.frameBuffer().tiaSurface();
    const uInt32 w = tia.width();
    const uInt32 h = std::min<uInt32>(tia.height(), A2600SESSION_FB_MAX_HEIGHT);
    const uInt8* src = partial ? tia.outputBuffer() : tia.frameBuffer();
    uInt32 bx = 0, by = 0;
    const bool beam = partial && tia.electronBeamPos(bx, by);
    for(uInt32 y = 0; y < h; ++y)
      for(uInt32 x = 0; x < w; ++x)
      {
        const size_t i = static_cast<size_t>(y) * w + x;
        // Past the beam the partial frame is stale: dim it, so the window
        // shows exactly how far the TIA has drawn.
        uint32_t px = surf.mapIndexedPixel(src[i]);
        if(beam && (y > by || (y == by && x >= bx)))
          px = (px >> 2) & 0x3F3F3F;
        dst[i] = px;
      }
    return static_cast<int>(h);
  });
}

// ---- memory --------------------------------------------------------------------

extern "C" int a2600debug_read(a2600debug* d, uint16_t addr, uint8_t* dst, int n)
{
  if(!d || !dst || n <= 0 || !d->session->running) return 0;
  return with_dbg(d, [&](OSystem& os) {
    if(!os.hasConsole()) return 0;
    System& sys = os.console().system();
    for(int i = 0; i < n; ++i)
      dst[i] = sys.peekOob(static_cast<uInt16>(addr + i));
    return n;
  });
}

extern "C" void a2600debug_write(a2600debug* d, uint16_t addr, uint8_t value)
{
  if(!d || !d->session->running) return;
  with_dbg(d, [&](OSystem& os) {
    if(os.hasConsole()) os.debugger().poke(addr, value);
    return 0;
  });
  ++d->generation;
}

extern "C" void a2600debug_ram_get(a2600debug* d, uint8_t out[128])
{
  if(!out) return;
  memset(out, 0, 128);
  a2600debug_read(d, 0x80, out, 128);
}

extern "C" int a2600debug_patch_rom(a2600debug* d, uint16_t addr, uint8_t value)
{
  if(!d || !d->session->running) return -1;
  const int rc = with_dbg(d, [&](OSystem& os) {
    if(!os.hasConsole()) return -1;
    return os.debugger().patchROM(addr, value) ? 0 : -1;
  });
  ++d->generation;
  return rc;
}

// ---- disassembly ---------------------------------------------------------------

extern "C" int a2600debug_disassemble(a2600debug* d, int bank, int first,
                                      a2600debug_line* out, int max,
                                      int* total, int* pc_line)
{
  if(pc_line) *pc_line = -1;
  if(total) *total = 0;
  if(!d || !out || max <= 0 || !d->session->running) return 0;
  return with_dbg(d, [&](OSystem& os) {
    if(!os.hasConsole()) return 0;
    Debugger& dbg = os.debugger();
    CartDebug& cart = dbg.cartDebug();
    const int pcBank = cart.getPCBank();
    const bool pcHere = bank < 0 || bank == pcBank;
    if(pcHere)
      cart.disassemblePC(true);
    else
      cart.disassembleBank(bank);
    const auto& list = cart.disassembly().list;
    const int pc = dbg.cpuDebug().pc();
    const int count = static_cast<int>(list.size());
    if(total) *total = count;
    if(pcHere && pc_line)
    {
      const int idx = cart.addressToLine(static_cast<uInt16>(pc));
      *pc_line = idx >= 0 && idx < count ? idx : -1;
    }
    const uInt16 bankSel = static_cast<uInt16>(bank < 0 ? pcBank : bank);
    int n = 0;
    for(int i = std::max(0, first); i < count && n < max; ++i)
    {
      const auto& tag = list[static_cast<size_t>(i)];
      a2600debug_line& l = out[n];
      memset(&l, 0, sizeof l);
      l.address = tag.address;
      l.type = static_cast<int>(tag.type);
      l.is_pc = pcHere && tag.address == pc;
      l.has_breakpoint = dbg.checkBreakPoint(tag.address, bankSel);
      snprintf(l.bytes, sizeof l.bytes, "%s", tag.bytes.c_str());
      snprintf(l.label, sizeof l.label, "%s", tag.label.c_str());
      snprintf(l.disasm, sizeof l.disasm, "%s", tag.disasm.c_str());
      snprintf(l.cycles, sizeof l.cycles, "%s", tag.ccount.c_str());
      snprintf(l.cycles_total, sizeof l.cycles_total, "%s", tag.ctotal.c_str());
      ++n;
    }
    return n;
  });
}

extern "C" int a2600debug_bank_count(a2600debug* d)
{
  if(!d || !d->session->running) return 0;
  return with_dbg(d, [&](OSystem& os) {
    return os.hasConsole() ? os.debugger().cartDebug().romBankCount() : 0;
  });
}

extern "C" int a2600debug_current_bank(a2600debug* d)
{
  if(!d || !d->session->running) return 0;
  return with_dbg(d, [&](OSystem& os) {
    return os.hasConsole() ? os.debugger().cartDebug().getPCBank() : 0;
  });
}

extern "C" int a2600debug_label_address(a2600debug* d, const char* label)
{
  if(!d || !label || !d->session->running) return -1;
  const std::string l(label);
  return with_dbg(d, [&](OSystem& os) {
    return os.hasConsole() ? os.debugger().cartDebug().getAddress(l) : -1;
  });
}

extern "C" int a2600debug_address_label(a2600debug* d, uint16_t addr, char* dst, int dstsz)
{
  if(!d || !dst || dstsz <= 0) return 0;
  dst[0] = '\0';
  if(!d->session->running) return 0;
  return with_dbg(d, [&](OSystem& os) {
    if(!os.hasConsole()) return 0;
    return put(dst, dstsz, os.debugger().cartDebug().getLabel(addr, true));
  });
}

extern "C" int a2600debug_load_symbols(a2600debug* d, char* msg, int msgsz)
{
  if(!d || !d->session->running) return -1;
  const std::string out = with_dbg(d, [&](OSystem& os) -> std::string {
    if(!os.hasConsole()) return "no console";
    CartDebug& cart = os.debugger().cartDebug();
    return cart.loadSymbolFile() + "; " + cart.loadListFile();
  });
  ++d->generation;
  put(msg, msgsz, out);
  return 0;
}

extern "C" int a2600debug_cart_info(a2600debug* d, char* dst, int dstsz)
{
  if(!d || !dst || dstsz <= 0) return 0;
  dst[0] = '\0';
  if(!d->session->running) return 0;
  return with_dbg(d, [&](OSystem& os) {
    if(!os.hasConsole()) return 0;
    Cartridge& c = os.console().cartridge();
    std::string s = c.name() + " (" + c.detectedType() + ")";
    const int banks = os.debugger().cartDebug().romBankCount();
    if(banks > 1)
      s += ", bank " + std::to_string(os.debugger().cartDebug().getPCBank()) + " of " + std::to_string(banks);
    const std::string ext = c.externalStateInfo();
    if(!ext.empty()) s += ", FujiNet: " + ext;
    return put(dst, dstsz, s);
  });
}

// ---- breakpoints ---------------------------------------------------------------

extern "C" int a2600debug_breakpoint_toggle(a2600debug* d, uint16_t addr, int bank)
{
  if(!d || !d->session->running) return 0;
  const int rc = with_dbg(d, [&](OSystem& os) {
    if(!os.hasConsole()) return 0;
    return os.debugger().toggleBreakPoint(addr, static_cast<uInt16>(bank)) ? 1 : 0;
  });
  ++d->generation;
  return rc;
}

extern "C" int a2600debug_breakpoint_check(a2600debug* d, uint16_t addr, int bank)
{
  if(!d || !d->session->running) return 0;
  return with_dbg(d, [&](OSystem& os) {
    if(!os.hasConsole()) return 0;
    return os.debugger().checkBreakPoint(addr, static_cast<uInt16>(bank)) ? 1 : 0;
  });
}

extern "C" int a2600debug_breakpoint_list(a2600debug* d, uint32_t* out, int max)
{
  if(!d || !out || max <= 0 || !d->session->running) return 0;
  return with_dbg(d, [&](OSystem& os) {
    if(!os.hasConsole()) return 0;
    int n = 0;
    for(const auto& bp : os.debugger().breakPoints().getBreakpoints())
    {
      if(n >= max) break;
      out[n++] = static_cast<uint32_t>(bp.addr) | (static_cast<uint32_t>(bp.bank) << 16);
    }
    return n;
  });
}

extern "C" void a2600debug_breakpoint_clear(a2600debug* d)
{
  run_quiet(d, "clearBreaks");
}

extern "C" int a2600debug_trap_read(a2600debug* d, uint16_t addr)
{
  if(!d || !d->session->running) return 0;
  return with_dbg(d, [&](OSystem& os) {
    return os.hasConsole() && os.debugger().readTraps().isSet(addr) ? 1 : 0;
  });
}

extern "C" int a2600debug_trap_write(a2600debug* d, uint16_t addr)
{
  if(!d || !d->session->running) return 0;
  return with_dbg(d, [&](OSystem& os) {
    return os.hasConsole() && os.debugger().writeTraps().isSet(addr) ? 1 : 0;
  });
}

// ---- states / files ------------------------------------------------------------

extern "C" void a2600debug_state_save(a2600debug* d, int slot)
{
  run_quiet(d, "saveState " + std::to_string(slot < 0 ? 0 : slot > 9 ? 9 : slot));
}

extern "C" void a2600debug_state_load(a2600debug* d, int slot)
{
  run_quiet(d, "loadState " + std::to_string(slot < 0 ? 0 : slot > 9 ? 9 : slot));
}

extern "C" int a2600debug_save(a2600debug* d, const char* kind, const char* path,
                               char* msg, int msgsz)
{
  if(!d || !kind || !path || !*path || !d->session->running) return -1;
  const std::string k(kind), p(path);
  std::string cmd;
  if(k == "dis") cmd = "saveDis";
  else if(k == "rom") cmd = "saveRom";
  else if(k == "access") cmd = "saveAccess";
  else if(k == "ses") cmd = "saveSes";
  else if(k == "snap") cmd = "saveSnap";
  else { put(msg, msgsz, "unknown save kind"); return -1; }
  const std::string out = d->host->dbgRun(cmd + " \"" + p + "\"");
  ++d->generation;
  put(msg, msgsz, out);
  return 0;
}
