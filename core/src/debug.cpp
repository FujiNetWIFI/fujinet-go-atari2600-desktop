/*
 * debug.cpp -- the debugger contract (core/include/a2600debug.h) over
 * Stella's own engine, through StellaHost jobs.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "BreakpointMap.hxx"
#include "Cart.hxx"
#include "CartDebug.hxx"
#include "CartFUJI.hxx"
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
#include "YaccParser.hxx"

#include "StellaHost.hxx"
#include "host/FNGOHostHooks.hxx"

extern "C" {
#include "session_internal.h"
#include "a2600debug.h"
#include "a2600memtools.h"
#include "a2600vidtab.h"
#include "fuji_trace.h"
}

StellaHost* a2600session_host(a2600session* s);

namespace {

// One row of the breakpoint table (see a2600debug.h): Stella has no ids, hit
// counts or disabled state, so the core keeps them.
struct Bp
{
  int id{0};
  int type{A2600DEBUG_BP_EXEC};
  uint16_t start{0}, end{0};
  int bank{A2600DEBUG_ANY_BANK};
  bool enabled{true};
  unsigned hits{0};
  std::string cond;
  bool raw{false};     // a condition-only break the console set
  std::string tname;   // the name Stella gave an installed trap
};

constexpr size_t kRing = 65536;

} // namespace

struct a2600debug
{
  a2600session* session{nullptr};
  StellaHost* host{nullptr};
  std::atomic<unsigned> generation{0};
  std::atomic<uint64_t> stopSerial{0};
  std::mutex mutex;
  std::string reason;
  int reasonAddr{-1};
  fuji_trace* fuji{nullptr};

  // Never held across a host call: the Stella thread takes it too.
  std::mutex bpMutex;
  std::vector<Bp> bps;
  int nextBpId{1};

  std::mutex traceMutex;
  std::vector<a2600debug_trace_entry> ring;
  size_t ringHead{0}, ringCount{0};
  std::atomic<bool> traceOn{false};

  const void* lastCpu{nullptr};   // the console the hooks are installed on
  int tempBreak{-1};              // run-to's breakpoint, removed at the next stop
};

namespace {

// `old` and `value` for Stella's `_old` and `_value`, whole words only.
std::string alias_names(const std::string& in)
{
  std::string out;
  size_t i = 0;
  while(i < in.size())
  {
    const auto c = static_cast<unsigned char>(in[i]);
    if(std::isalnum(c) || c == '_' || c == '$')
    {
      size_t j = i;
      while(j < in.size() && (std::isalnum(static_cast<unsigned char>(in[j])) || in[j] == '_' || in[j] == '$')) ++j;
      std::string word = in.substr(i, j - i), low;
      for(char ch : word) low += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      out += low == "old" ? "_old" : low == "value" ? "_value" : word;
      i = j;
    }
    else
      out += in[i++];
  }
  return out;
}

std::string trim(std::string t)
{
  while(!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
  size_t bpt = 0;
  while(bpt < t.size() && std::isspace(static_cast<unsigned char>(t[bpt]))) ++bpt;
  return t.substr(bpt);
}

// A read with no side effects: the cartridge's hotspots and the data bus
// locked for the scope (as the debugger does while stopped).
class QuietBus
{
  public:
    explicit QuietBus(System& sys) : mySys(sys), myTook(!sys.cart().hotspotsLocked())
    {
      if(myTook) { mySys.lockDataBus(); mySys.cart().lockHotspots(); }
    }
    ~QuietBus()
    {
      if(myTook) { mySys.unlockDataBus(); mySys.cart().unlockHotspots(); }
    }
  private:
    System& mySys;
    bool myTook;
};

// Inside a QuietBus. The RIOT's timer registers ($280-$29F and mirrors, A2
// set) clear their interrupt flag when read: the RIOT's own getters instead.
// With no RiotDebug (the trace tap) such a read gives 0.
uint8_t quiet_peek(System& sys, uInt16 addr, const RiotDebug* riot)
{
  if((addr & 0x1280) == 0x280 && (addr & 0x04))
    return !riot ? 0 : (addr & 1) ? riot->timint() : riot->intim();
  return sys.peekOob(addr);
}

bool plain_exec(const Bp& bpt)
{
  return bpt.type == A2600DEBUG_BP_EXEC && !bpt.raw && bpt.start == bpt.end && bpt.cond.empty();
}

std::string exec_expr(const Bp& bpt)
{
  const std::string cond = alias_names(bpt.cond);
  if(bpt.raw) return cond;
  char head[64];
  if(bpt.start == bpt.end)
    snprintf(head, sizeof head, "(pc==$%04x)", bpt.start);
  else
    snprintf(head, sizeof head, "(pc>=$%04x&&pc<=$%04x)", bpt.start, bpt.end);
  std::string e = head;
  if(bpt.bank != A2600DEBUG_ANY_BANK) e += "&&(_bank==" + std::to_string(bpt.bank) + ")";
  if(!cond.empty()) e += "&&(" + cond + ")";
  return e;
}

std::string trap_command(const Bp& bpt)
{
  const bool reading = (bpt.type & A2600DEBUG_BP_READ) != 0, writing = (bpt.type & A2600DEBUG_BP_WRITE) != 0;
  const std::string cond = bpt.tname.empty() ? alias_names(bpt.cond) : bpt.tname;
  std::string cmd = reading && writing ? "trap" : reading ? "trapRead" : "trapWrite";
  if(!cond.empty()) cmd += "If {" + cond + "}";
  char addr[40];
  if(bpt.start == bpt.end) snprintf(addr, sizeof addr, " $%04x", bpt.start);
  else snprintf(addr, sizeof addr, " $%04x $%04x", bpt.start, bpt.end);
  return cmd + addr;
}

bool installed(OSystem& os, const Bp& bpt)
{
  Debugger& dbg = os.debugger();
  if(plain_exec(bpt)) return dbg.checkBreakPoint(bpt.start, static_cast<uInt16>(bpt.bank));
  if(bpt.type == A2600DEBUG_BP_EXEC)
  {
    const std::string expr = exec_expr(bpt);
    const auto& names = dbg.m6502().getCondBreakNames();
    return std::find(names.begin(), names.end(), expr) != names.end();
  }
  const bool reading = (bpt.type & A2600DEBUG_BP_READ) != 0, writing = (bpt.type & A2600DEBUG_BP_WRITE) != 0;
  const std::string name = bpt.tname.empty() ? alias_names(bpt.cond) : bpt.tname;
  for(const auto& t : dbg.m6502().getCondTraps())
    if(t.begin == bpt.start && t.end == bpt.end && t.read == reading && t.write == writing && t.name == name)
      return true;
  return false;
}

// false if Stella refused it (a condition that does not parse).
bool install(OSystem& os, Bp& bpt)
{
  if(installed(os, bpt)) return true;
  Debugger& dbg = os.debugger();
  if(plain_exec(bpt))
  {
    dbg.setBreakPoint(bpt.start, static_cast<uInt16>(bpt.bank));
    return true;
  }
  if(bpt.type == A2600DEBUG_BP_EXEC)
  {
    const std::string expr = exec_expr(bpt);
    auto e = YaccParser::parse(expr);
    if(!e) return false;
    dbg.m6502().addCondBreak(std::move(e), expr);
    return true;
  }
  const size_t before = dbg.m6502().getCondTraps().size();
  dbg.run(trap_command(bpt));
  const auto& traps = dbg.m6502().getCondTraps();
  if(traps.size() != before + 1) return false;
  bpt.tname = traps.back().name;
  return true;
}

void uninstall(OSystem& os, const Bp& bpt)
{
  if(!installed(os, bpt)) return;
  Debugger& dbg = os.debugger();
  if(plain_exec(bpt))
    dbg.clearBreakPoint(bpt.start, static_cast<uInt16>(bpt.bank));
  else if(bpt.type == A2600DEBUG_BP_EXEC)
  {
    const std::string expr = exec_expr(bpt);
    const auto& names = dbg.m6502().getCondBreakNames();
    const auto it = std::find(names.begin(), names.end(), expr);
    if(it != names.end()) dbg.m6502().delCondBreak(static_cast<uInt32>(it - names.begin()));
  }
  else
    dbg.run(trap_command(bpt));   // the same trap again removes it
}

bool same_addr(uint16_t a, uint16_t bpt) { return (a & 0x1fff) == (bpt & 0x1fff); }

// Make the table and Stella agree after a console command: what Stella no
// longer has leaves the table, what it gained joins it. Caller: Stella thread.
void reconcile(a2600debug* d, OSystem& os)
{
  if(!os.hasConsole()) return;
  Debugger& dbg = os.debugger();
  const std::lock_guard<std::mutex> lock(d->bpMutex);
  d->bps.erase(std::remove_if(d->bps.begin(), d->bps.end(),
                              [&](const Bp& bpt) { return bpt.enabled && !installed(os, bpt); }),
               d->bps.end());
  BreakpointMap& map = dbg.breakPoints();
  for(const auto& stellabp : map.getBreakpoints())
  {
    if(map.get(stellabp) & BreakpointMap::ONE_SHOT) continue;
    if(d->tempBreak >= 0 && same_addr(static_cast<uint16_t>(d->tempBreak), stellabp.addr)) continue;
    const bool known = std::any_of(d->bps.begin(), d->bps.end(), [&](const Bp& bpt) {
      return plain_exec(bpt) && same_addr(bpt.start, stellabp.addr) && bpt.bank == stellabp.bank; });
    if(known) continue;
    Bp bpt;
    bpt.id = d->nextBpId++;
    bpt.start = bpt.end = stellabp.addr;
    bpt.bank = stellabp.bank;
    d->bps.push_back(bpt);
  }
  for(const auto& name : dbg.m6502().getCondBreakNames())
  {
    const bool known = std::any_of(d->bps.begin(), d->bps.end(), [&](const Bp& bpt) {
      return bpt.type == A2600DEBUG_BP_EXEC && !plain_exec(bpt) && exec_expr(bpt) == name; });
    if(known) continue;
    Bp bpt;
    bpt.id = d->nextBpId++;
    bpt.start = 0; bpt.end = 0xffff;
    bpt.raw = true;
    bpt.cond = name;
    d->bps.push_back(bpt);
  }
  for(const auto& t : dbg.m6502().getCondTraps())
  {
    const bool known = std::any_of(d->bps.begin(), d->bps.end(), [&](const Bp& bpt) {
      return bpt.type != A2600DEBUG_BP_EXEC && bpt.start == t.begin && bpt.end == t.end
             && bpt.type == ((t.read ? A2600DEBUG_BP_READ : 0) | (t.write ? A2600DEBUG_BP_WRITE : 0))
             && (bpt.tname.empty() ? alias_names(bpt.cond) : bpt.tname) == t.name; });
    if(known) continue;
    Bp bpt;
    bpt.id = d->nextBpId++;
    bpt.type = (t.read ? A2600DEBUG_BP_READ : 0) | (t.write ? A2600DEBUG_BP_WRITE : 0);
    bpt.start = static_cast<uint16_t>(t.begin);
    bpt.end = static_cast<uint16_t>(t.end);
    bpt.cond = t.name;
    bpt.tname = t.name;
    d->bps.push_back(bpt);
  }
}

// ---- the trace tap ----------------------------------------------------------------

void trace_add(a2600debug* d, const a2600debug_trace_entry& t)
{
  const std::lock_guard<std::mutex> lock(d->traceMutex);
  if(d->ring.size() != kRing) return;
  d->ring[d->ringHead] = t;
  d->ringHead = (d->ringHead + 1) & (kRing - 1);
  if(d->ringCount < kRing) ++d->ringCount;
}

void set_trace_tap(a2600debug* d, OSystem& os, bool on)
{
  if(!os.hasConsole()) return;
  System* sys = &os.console().system();
  if(!on)
  {
    sys->m6502().setInstructionTap(nullptr);
    return;
  }
  sys->m6502().setInstructionTap([d, sys](uInt16 pc, uInt8 acc, uInt8 idx, uInt8 idy, uInt8 stackp, uInt8 status, uInt64 cycles) {
    a2600debug_trace_entry t{};
    t.pc = pc; t.a = acc; t.x = idx; t.y = idy; t.sp = stackp; t.p = status;
    t.cycles = cycles;
    {
      const QuietBus quiet(*sys);
      for(int i = 0; i < 3; ++i)
        t.bytes[i] = quiet_peek(*sys, static_cast<uInt16>(pc + i), nullptr);
      t.bank = static_cast<int16_t>(sys->cart().getBank(pc));
    }
    // bring the TIA up to this instruction, so the beam is where it is
    sys->tia().updateEmulation();
    uInt32 bx = 0, by = 0;
    sys->tia().electronBeamPos(bx, by);
    t.scanline = static_cast<int16_t>(by);
    t.dot = static_cast<int16_t>(sys->tia().clocksThisLine());
    trace_add(d, t);
  });
}

// A new console has none of our hooks: put them back. Stella thread.
void sync_console(a2600debug* d, OSystem& os)
{
  if(!os.hasConsole()) { d->lastCpu = nullptr; return; }
  const void* cpu = &os.console().system().m6502();
  if(cpu == d->lastCpu) return;
  d->lastCpu = cpu;
  d->tempBreak = -1;
  if(d->traceOn.load()) set_trace_tap(d, os, true);
  const std::lock_guard<std::mutex> lock(d->bpMutex);
  for(Bp& bpt : d->bps)
    if(bpt.enabled) install(os, bpt);
}

// Every inspection needs the debugger's state locked; a running machine is
// asked at a frame boundary by the job mechanism itself, so only the
// "no console" case is refused.
template<typename F>
auto with_dbg(a2600debug* d, F&& fn) -> decltype(fn(std::declval<OSystem&>()))
{
  return d->host->withStella([&](OSystem& os) {
    sync_console(d, os);
    return fn(os);
  });
}

int put(char* dst, int dstsz, const std::string& s)
{
  if(!dst || dstsz <= 0) return 0;
  return snprintf(dst, static_cast<size_t>(dstsz), "%s", s.c_str());
}

a2600debug* gTraced = nullptr;     // the one the link's hook feeds

} // namespace

extern "C" a2600debug* a2600debug_get(a2600session* s)
{
  if(!s) return nullptr;
  if(!s->debugger)
  {
    auto* d = new a2600debug();
    d->session = s;
    d->host = a2600session_host(s);
    d->fuji = fuji_trace_new();
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
  d->host->dbgBreak();      // the stop callback sets the reason
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

extern "C" uint64_t a2600debug_stop_serial(a2600debug* d)
{
  return d ? d->stopSerial.load() : 0;
}

extern "C" unsigned a2600debug_generation(a2600debug* d)
{
  return d ? d->generation.load() : 0;
}

void a2600debug_destroy(a2600session* s)
{
  if(!s || !s->debugger) return;
  auto* d = static_cast<a2600debug*>(s->debugger);
  if(gTraced == d)
  {
    FNGOHostHooks::onFujiNetBytes = nullptr;
    gTraced = nullptr;
  }
  fuji_trace_free(d->fuji);
  delete d;
  s->debugger = nullptr;
}

// The FujiNet trace is always on: it is cheap, and it is most useful about
// what happened before anyone thought to look. Installed before Stella
// starts, so no cartridge worker is running while the hook changes.
void a2600debug_trace_install(a2600session* s)
{
  a2600debug* d = a2600debug_get(s);
  if(!d) return;
  gTraced = d;
  fuji_trace* t = d->fuji;
  FNGOHostHooks::onFujiNetBytes = [t](int dir, const uint8_t* buf, size_t len) {
    fuji_trace_feed(t, dir, buf, len);
  };
}

namespace {

void set_reason(a2600debug* d, const std::string& text, int addr)
{
  {
    const std::lock_guard<std::mutex> lock(d->mutex);
    d->reason = text;
    d->reasonAddr = addr;
  }
  ++d->stopSerial;
  ++d->generation;
}

// Credit the stop to the entries that caused it. Stella thread.
void count_hits(a2600debug* d, OSystem& os, const std::string& msg, int addr)
{
  if(!os.hasConsole()) return;
  Debugger& dbg = os.debugger();
  M6502& cpu = dbg.m6502();
  const std::lock_guard<std::mutex> lock(d->bpMutex);
  if(msg.rfind("BP:", 0) == 0)
  {
    const int bank = os.console().cartridge().getBank(static_cast<uInt16>(addr));
    for(Bp& bpt : d->bps)
      if(bpt.enabled && plain_exec(bpt) && same_addr(bpt.start, static_cast<uint16_t>(addr))
         && (bpt.bank == A2600DEBUG_ANY_BANK || bpt.bank == bank))
        ++bpt.hits;
    return;
  }
  const size_t open = msg.find('[');
  if(open == std::string::npos) return;
  const size_t idx = std::strtoul(msg.c_str() + open + 1, nullptr, 16);
  if(msg.rfind("CBP[", 0) == 0)
  {
    const auto& names = cpu.getCondBreakNames();
    if(idx >= names.size()) return;
    for(Bp& bpt : d->bps)
      if(bpt.enabled && bpt.type == A2600DEBUG_BP_EXEC && !plain_exec(bpt) && exec_expr(bpt) == names[idx])
        ++bpt.hits;
  }
  else if(msg.rfind("RTrap", 0) == 0 || msg.rfind("WTrap", 0) == 0)
  {
    const auto& traps = cpu.getCondTraps();
    if(idx >= traps.size()) return;
    const auto& t = traps[idx];
    const int type = (t.read ? A2600DEBUG_BP_READ : 0) | (t.write ? A2600DEBUG_BP_WRITE : 0);
    for(Bp& bpt : d->bps)
      if(bpt.enabled && bpt.type == type && bpt.start == t.begin && bpt.end == t.end
         && (bpt.tname.empty() ? alias_names(bpt.cond) : bpt.tname) == t.name)
        ++bpt.hits;
  }
}

} // namespace

// Called by session.cpp's stop callback (the Stella thread), so windows see why.
void a2600debug_note_stop(a2600session* s, const char* msg, int addr)
{
  auto* d = a2600debug_get(s);
  if(!d) return;
  std::string text = msg ? msg : "";
  OSystem* os = d->host->osystem();
  if(os && os->hasConsole())
  {
    if(d->tempBreak >= 0)
    {
      os->debugger().clearBreakPoint(static_cast<uInt16>(d->tempBreak), BreakpointMap::ANY_BANK);
      d->tempBreak = -1;
    }
    M6502& cpu = os->debugger().m6502();
    if(text.empty() || text == "stopped")
    {
      addr = os->debugger().cpuDebug().pc();
      char buf[32];
      snprintf(buf, sizeof buf, "stopped at $%04X", addr);
      text = buf;
    }
    else
    {
      // a breakpoint's stop carries no address: it lands on the instruction
      if(addr < 0) addr = os->debugger().cpuDebug().pc();
      count_hits(d, *os, text, addr);
      if(text.rfind("WTrap", 0) == 0 && cpu.lastWriteAddress() == addr)
      {
        char buf[64];
        snprintf(buf, sizeof buf, " wrote $%04X = $%02X (was $%02X)", addr & 0xffff,
                 cpu.lastPokeValue(), cpu.lastPokeOld());
        text += buf;
      }
    }
  }
  set_reason(d, text, addr);
}

// Called by session.cpp when a cartridge swap or a new console replaced
// Stella's console (the Stella thread): put the hooks back.
void a2600debug_console_replaced(a2600session* s)
{
  auto* d = a2600debug_get(s);
  OSystem* os = d ? d->host->osystem() : nullptr;
  if(os) sync_console(d, *os);
}

namespace {

std::string first_word(const std::string& s)
{
  const size_t bpt = s.find_first_not_of(" \t");
  if(bpt == std::string::npos) return {};
  const size_t e = s.find_first_of(" \t,", bpt);
  std::string w = s.substr(bpt, e == std::string::npos ? std::string::npos : e - bpt);
  for(char& c : w) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return w;
}

// What follows the command word.
std::string rest_of(const std::string& s)
{
  const size_t bpt = s.find_first_not_of(" \t");
  if(bpt == std::string::npos) return {};
  const size_t e = s.find_first_of(" \t", bpt);
  if(e == std::string::npos) return {};
  return trim(s.substr(e));
}

std::string hex4(unsigned v)
{
  char buf[16];
  snprintf(buf, sizeof buf, "%04X", v & 0xffff);
  return buf;
}

// The commands that leave the machine stopped somewhere new.
bool is_stepping(const std::string& w)
{
  static const char* const words[] = { "step", "trace", "scanline", "frame", "rewind", "unwind" };
  for(const char* x : words) if(w == x) return true;
  return false;
}

// Commands taking a Stella expression: `old` and `value` work in them.
bool takes_expression(const std::string& w)
{
  static const char* const words[] = { "breakif", "trapif", "trapreadif", "trapwriteif",
                                       "savestateif", "print", "eval" };
  for(const char* x : words) if(w == x) return true;
  return false;
}

const char* const kFamilyCommands[] = { "cart", "watch", "unwatch", "find", "video" };

std::string help_text()
{
  return
    "FujiNet Go's own commands:\n"
    "  cart                the cartridge: scheme, bank, FujiNet link\n"
    "  watch [range]       add to the watch list (an address, label, lo-hi), or list it\n"
    "                      (replaces Stella's watch)\n"
    "  unwatch <n|all>     drop a watch\n"
    "  video [section]     the Video tab as text: pixel, registers, colours, lines, writes, timing\n"
    "  find [hex] <bytes>  search memory (stopped): find A9 00 8D, find text <string>\n"
    "  breakIf / trapIf / trapReadIf / trapWriteIf: a condition may use old (the byte\n"
    "                      a write replaces) and value (the byte moving)\n"
    "Everything else is Stella's debugger.\n\n";
}

std::string find_command(a2600debug* d, const std::string& cmd)
{
  std::string args = rest_of(cmd);
  int kind = A2600SEARCH_HEX;
  if(args.rfind("text", 0) == 0 && (args.size() == 4 || std::isspace(static_cast<unsigned char>(args[4]))))
  {
    kind = A2600SEARCH_TEXT;
    args = trim(args.substr(4));
  }
  else if(args.rfind("hex", 0) == 0 && (args.size() == 3 || std::isspace(static_cast<unsigned char>(args[3]))))
    args = trim(args.substr(3));
  uint8_t pat[A2600SEARCH_MAX], mask;
  int n = 0, flags = 0;
  if(args.empty() || !a2600search_parse(kind, args.c_str(), pat, &n, &mask, &flags))
    return "find [hex] <bytes> | find text <string>\n";
  uint16_t hit[32];
  const int count = a2600debug_search(d, pat, n, mask, flags, 0, 0x1fff, hit, 32);
  if(count < 0) return "stop the machine first\n";
  if(count == 0) return "not found\n";
  std::string out;
  for(int i = 0; i < count; i++) out += "$" + hex4(hit[i]) + "\n";
  return out;
}

std::string watch_list(a2600debug* d)
{
  if(!a2600watch_count()) return "no watches (watch <addr|label|lo-hi>)\n";
  std::string out;
  for(int i = 0; i < a2600watch_count(); i++)
  {
    char text[160], row[200];
    a2600watch_format(d, i, text, sizeof text);
    snprintf(row, sizeof row, "%2d  %s\n", i, text);
    out += row;
  }
  return out;
}

void note_step(a2600debug* d, const std::string& what)
{
  const int pc = with_dbg(d, [&](OSystem& os) {
    return os.hasConsole() ? os.debugger().cpuDebug().pc() : -1;
  });
  char buf[64];
  snprintf(buf, sizeof buf, "%s at $%04X", what.c_str(), pc & 0xffff);
  set_reason(d, buf, pc);
}

} // namespace

extern "C" int a2600debug_command(a2600debug* d, const char* command, char* dst, int dstsz)
{
  if(!d || !command || !d->session->running) return put(dst, dstsz, "");
  std::string cmd(command);
  // Trim, and refuse the two commands that cannot work headless: exitRom
  // opens Stella's own launcher, stepWhile pumps an offscreen progress
  // dialog whose Cancel is unreachable.
  while(!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r' || cmd.back() == ' '))
    cmd.pop_back();
  const std::string word = first_word(cmd);
  if(word == "exitrom")
    return put(dst, dstsz, "exitRom is not available here: use Eject Cartridge");
  if(word == "stepwhile")
    return put(dst, dstsz, "stepWhile is not available here (it cannot be cancelled)");

  // Ours, before Stella's parser.
  if(word == "cart")
  {
    char line[256];
    a2600debug_cart_info(d, line, sizeof line);
    return put(dst, dstsz, std::string(line) + "\n");
  }
  if(word == "watch")
  {
    const std::string text = rest_of(cmd);
    std::string out;
    if(!text.empty() && a2600watch_add(text.c_str()) < 0) out = "no room for another watch\n";
    return put(dst, dstsz, out + watch_list(d));
  }
  if(word == "unwatch")
  {
    const std::string text = rest_of(cmd);
    if(text == "all") a2600watch_clear();
    else if(!text.empty()) a2600watch_remove(static_cast<int>(std::strtol(text.c_str(), nullptr, 0)));
    else return put(dst, dstsz, "unwatch <n|all>\n");
    return put(dst, dstsz, watch_list(d));
  }
  if(word == "find")
    return put(dst, dstsz, find_command(d, cmd));
  if(word == "video")
  {
    // a short-lived Video tab: it asks for captures only while it exists
    std::string arg = rest_of(cmd);
    int section = A2600VID_S_REGISTERS;
    if(!arg.empty())
    {
      section = -1;
      for(int idx = 0; idx < A2600VID_S_COUNT; idx++)
        if(arg == a2600vid_section_key(idx)) section = idx;
      if(section < 0)
        return put(dst, dstsz, "video: pixel, registers, colours, lines, writes or timing\n");
    }
    a2600vid_tab* tab = a2600vid_tab_new(d->session);
    if(!tab) return put(dst, dstsz, "video: not available\n");
    a2600vid_tab_refresh(tab, 1);
    std::string text(16384, '\0');
    const int len = a2600vid_tab_text(tab, section, text.data(), static_cast<int>(text.size()));
    a2600vid_tab_free(tab);
    text.resize(static_cast<size_t>(std::max(0, std::min<int>(len, static_cast<int>(text.size()) - 1))));
    return put(dst, dstsz, text.empty() ? std::string("video: no frame captured yet (stop the machine)\n") : text + "\n");
  }

  std::string send = cmd;
  if(takes_expression(word))
  {
    const std::string tail = rest_of(cmd);
    send = word + " " + alias_names(tail);
  }
  std::string out = d->host->dbgRun(send);
  if(word == "help" && rest_of(cmd).empty())
    out = help_text() + out;
  with_dbg(d, [&](OSystem& os) { reconcile(d, os); return 0; });
  if(is_stepping(word))
    note_step(d, word == "trace" ? "step over" : word);
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
    for(const char* c : kFamilyCommands)
      if(std::strncmp(c, pre.c_str(), pre.size()) == 0 && std::find(list.begin(), list.end(), c) == list.end())
        list.emplace_back(c);
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
void run_quiet(a2600debug* d, const std::string& cmd, const char* what = nullptr)
{
  if(!d || !d->session->running) return;
  d->host->dbgRun(cmd);
  if(what) note_step(d, what);
  ++d->generation;
}
}

extern "C" void a2600debug_step(a2600debug* d) { run_quiet(d, "step", "step"); }
extern "C" void a2600debug_trace(a2600debug* d) { run_quiet(d, "trace", "step over"); }
extern "C" void a2600debug_step_over(a2600debug* d) { run_quiet(d, "trace", "step over"); }
extern "C" void a2600debug_scanline(a2600debug* d, int n) { run_quiet(d, "scanLine " + std::to_string(n > 0 ? n : 1), "scanline"); }
extern "C" void a2600debug_frame(a2600debug* d, int n) { run_quiet(d, "frame " + std::to_string(n > 0 ? n : 1), "frame"); }
extern "C" void a2600debug_rewind(a2600debug* d, int n) { run_quiet(d, "rewind " + std::to_string(n > 0 ? n : 1), "rewind"); }
extern "C" void a2600debug_unwind(a2600debug* d, int n) { run_quiet(d, "unwind " + std::to_string(n > 0 ? n : 1), "unwind"); }
// Runs on and stops when pc reaches addr: a breakpoint taken off at the next
// stop. (Stella's runToPc steps in a loop on the Stella thread, which never
// comes back if the address is not reached, and its one-shot breakpoints only
// work for its own synchronous "trace".)
extern "C" void a2600debug_run_to(a2600debug* d, uint16_t addr)
{
  if(!d || !d->session->running) return;
  with_dbg(d, [&](OSystem& os) {
    if(!os.hasConsole()) return 0;
    Debugger& dbg = os.debugger();
    if(os.eventHandler().state() != EventHandlerState::DEBUGGER)
      os.eventHandler().enterDebugMode();
    if(d->tempBreak >= 0) dbg.clearBreakPoint(static_cast<uInt16>(d->tempBreak), BreakpointMap::ANY_BANK);
    d->tempBreak = -1;
    if(!dbg.checkBreakPoint(addr, BreakpointMap::ANY_BANK))
    {
      dbg.setBreakPoint(addr, BreakpointMap::ANY_BANK);
      d->tempBreak = addr;
    }
    dbg.run("run");
    return 0;
  });
  ++d->generation;
}

// Out: run to the return address on the stack (an RTS returns to it + 1).
extern "C" void a2600debug_step_out(a2600debug* d)
{
  if(!d || !d->session->running) return;
  const int target = with_dbg(d, [&](OSystem& os) {
    if(!os.hasConsole()) return -1;
    System& sys = os.console().system();
    const int sp = os.debugger().cpuDebug().sp();
    const QuietBus quiet(sys);
    const int low = quiet_peek(sys, static_cast<uInt16>(0x100 | ((sp + 1) & 0xff)), &os.debugger().riotDebug());
    const int high = quiet_peek(sys, static_cast<uInt16>(0x100 | ((sp + 2) & 0xff)), &os.debugger().riotDebug());
    return ((high << 8 | low) + 1) & 0xffff;
  });
  if(target >= 0) a2600debug_run_to(d, static_cast<uint16_t>(target));
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
    out->graphics_p0 = t.grP0(); out->graphics_p1 = t.grP1();
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
    const QuietBus quiet(sys);
    for(int i = 0; i < n; ++i)
      dst[i] = quiet_peek(sys, static_cast<uInt16>(addr + i), &os.debugger().riotDebug());
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

extern "C" int a2600debug_eval(a2600debug* d, const char* expression, long* out, char* err, int errsz)
{
  if(!d || !expression || !out || !*expression || !d->session->running)
  {
    if(err && errsz > 0) snprintf(err, static_cast<size_t>(errsz), "not an expression");
    return -1;
  }
  const std::string text = alias_names(expression);
  std::string message;
  const int rc = with_dbg(d, [&](OSystem& os) {
    if(!os.hasConsole()) { message = "no console"; return -1; }
    auto expr = YaccParser::parse(text);
    if(!expr) { message = YaccParser::errorMessage(); return -1; }
    const QuietBus quiet(os.console().system());
    *out = expr->evaluate();
    return 0;
  });
  if(rc != 0 && err && errsz > 0) snprintf(err, static_cast<size_t>(errsz), "%s", message.c_str());
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
    std::vector<Bp> table;
    {
      const std::lock_guard<std::mutex> lock(d->bpMutex);
      table = d->bps;
    }
    int n = 0;
    for(int i = std::max(0, first); i < count && n < max; ++i)
    {
      const auto& tag = list[static_cast<size_t>(i)];
      a2600debug_line& l = out[n];
      memset(&l, 0, sizeof l);
      l.address = tag.address;
      l.type = static_cast<int>(tag.type);
      l.is_pc = pcHere && tag.address == pc;
      for(const Bp& bpt : table)
      {
        if(bpt.type != A2600DEBUG_BP_EXEC || bpt.raw) continue;
        if(!(bpt.bank == A2600DEBUG_ANY_BANK || bpt.bank == bankSel)) continue;
        const unsigned at = tag.address & 0x1fffu, low = bpt.start & 0x1fffu, high = bpt.end & 0x1fffu;
        if(at < low || at > (high < low ? low : high)) continue;
        if(bpt.enabled) l.has_breakpoint = 1; else l.bp_disabled = 1;
      }
      if(l.has_breakpoint) l.bp_disabled = 0;
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

extern "C" int a2600debug_set_label(a2600debug* d, uint16_t addr, const char* label)
{
  if(!d || !label || !*label || !d->session->running) return -1;
  const std::string name(label);
  const int rc = with_dbg(d, [&](OSystem& os) {
    return os.hasConsole() && os.debugger().cartDebug().addLabel(name, addr) ? 0 : -1;
  });
  ++d->generation;
  return rc;
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
    const std::lock_guard<std::mutex> lock(d->bpMutex);
    for(auto it = d->bps.begin(); it != d->bps.end(); ++it)
      if(plain_exec(*it) && same_addr(it->start, addr) && it->bank == bank)
      {
        uninstall(os, *it);
        d->bps.erase(it);
        return 0;
      }
    Bp bpt;
    bpt.id = d->nextBpId++;
    bpt.start = bpt.end = addr;
    bpt.bank = bank;
    if(!install(os, bpt)) return 0;
    d->bps.push_back(bpt);
    return 1;
  });
  ++d->generation;
  return rc;
}

extern "C" int a2600debug_breakpoint_check(a2600debug* d, uint16_t addr, int bank)
{
  if(!d) return 0;
  const std::lock_guard<std::mutex> lock(d->bpMutex);
  for(const Bp& bpt : d->bps)
    if(bpt.enabled && plain_exec(bpt) && same_addr(bpt.start, addr) && bpt.bank == bank) return 1;
  return 0;
}

extern "C" int a2600debug_breakpoint_list(a2600debug* d, uint32_t* out, int max)
{
  if(!d || !out || max <= 0 || !d->session->running) return 0;
  with_dbg(d, [&](OSystem& os) { reconcile(d, os); return 0; });
  const std::lock_guard<std::mutex> lock(d->bpMutex);
  int n = 0;
  for(const Bp& bpt : d->bps)
    if(bpt.enabled && plain_exec(bpt) && n < max)
      out[n++] = static_cast<uint32_t>(bpt.start) | (static_cast<uint32_t>(bpt.bank) << 16);
  return n;
}

extern "C" void a2600debug_breakpoint_clear(a2600debug* d)
{
  if(!d || !d->session->running) return;
  with_dbg(d, [&](OSystem& os) {
    if(!os.hasConsole()) return 0;
    os.debugger().run("clearBreaks");
    os.debugger().run("clearTraps");
    const std::lock_guard<std::mutex> lock(d->bpMutex);
    d->bps.clear();
    return 0;
  });
  ++d->generation;
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

extern "C" int a2600debug_bp_add(a2600debug* d, int type, uint16_t start, uint16_t end,
                                 int bank, const char* condition)
{
  if(!d || !d->session->running) return -1;
  const int bpid = with_dbg(d, [&](OSystem& os) {
    if(!os.hasConsole()) return -1;
    Bp bpt;
    bpt.type = (type & A2600DEBUG_BP_EXEC) ? A2600DEBUG_BP_EXEC : (type & A2600DEBUG_BP_ACCESS);
    if(!bpt.type) return -1;
    bpt.start = start;
    bpt.end = end >= start ? end : start;
    bpt.bank = bpt.type == A2600DEBUG_BP_EXEC ? bank : A2600DEBUG_ANY_BANK;
    bpt.cond = trim(condition ? condition : "");
    if(!bpt.cond.empty() && !YaccParser::parse(alias_names(bpt.cond))) return -1;
    const std::lock_guard<std::mutex> lock(d->bpMutex);
    for(Bp& slot : d->bps)
      if(!slot.raw && slot.type == bpt.type && slot.start == bpt.start && slot.end == bpt.end && slot.bank == bpt.bank && slot.cond == bpt.cond)
      {
        slot.enabled = true;
        install(os, slot);
        return slot.id;
      }
    bpt.id = d->nextBpId;
    if(!install(os, bpt)) return -1;
    ++d->nextBpId;
    d->bps.push_back(bpt);
    return bpt.id;
  });
  ++d->generation;
  return bpid;
}

extern "C" void a2600debug_bp_remove(a2600debug* d, int bpid)
{
  if(!d || !d->session->running) return;
  with_dbg(d, [&](OSystem& os) {
    if(!os.hasConsole()) return 0;
    const std::lock_guard<std::mutex> lock(d->bpMutex);
    for(auto it = d->bps.begin(); it != d->bps.end(); ++it)
      if(it->id == bpid)
      {
        if(it->enabled) uninstall(os, *it);
        d->bps.erase(it);
        break;
      }
    return 0;
  });
  ++d->generation;
}

extern "C" void a2600debug_bp_enable(a2600debug* d, int bpid, int enabled)
{
  if(!d || !d->session->running) return;
  with_dbg(d, [&](OSystem& os) {
    if(!os.hasConsole()) return 0;
    const std::lock_guard<std::mutex> lock(d->bpMutex);
    for(Bp& bpt : d->bps)
      if(bpt.id == bpid && bpt.enabled != (enabled != 0))
      {
        bpt.enabled = enabled != 0;
        if(bpt.enabled) install(os, bpt); else uninstall(os, bpt);
      }
    return 0;
  });
  ++d->generation;
}

extern "C" int a2600debug_bp_list(a2600debug* d, a2600debug_breakpoint* out, int max)
{
  if(!d || !d->session->running) return 0;
  with_dbg(d, [&](OSystem& os) { reconcile(d, os); return 0; });
  const std::lock_guard<std::mutex> lock(d->bpMutex);
  int n = 0;
  for(const Bp& bpt : d->bps)
  {
    if(out && n < max)
    {
      a2600debug_breakpoint& slot = out[n];
      memset(&slot, 0, sizeof slot);
      slot.id = bpt.id;
      slot.type = bpt.type;
      slot.start = bpt.start;
      slot.end = bpt.end;
      slot.bank = bpt.bank;
      slot.enabled = bpt.enabled;
      slot.hits = bpt.hits;
      snprintf(slot.condition, sizeof slot.condition, "%s", bpt.cond.c_str());
    }
    ++n;
  }
  return out ? std::min(n, max) : n;
}

extern "C" void a2600debug_bp_clear_hits(a2600debug* d)
{
  if(!d) return;
  {
    const std::lock_guard<std::mutex> lock(d->bpMutex);
    for(Bp& bpt : d->bps) bpt.hits = 0;
  }
  ++d->generation;
}

// ---- symbols ---------------------------------------------------------------------

namespace {

struct Sym { std::string name; uint16_t addr; };

std::vector<Sym> user_symbols(OSystem& os)
{
  std::vector<Sym> out;
  if(!os.hasConsole()) return out;
  for(const auto& kv : os.debugger().cartDebug().userAddresses())
    out.push_back({ kv.first, static_cast<uint16_t>(kv.second) });
  return out;
}

bool contains_nocase(const std::string& hay, const std::string& needle)
{
  return std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                     [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); })
         != hay.end();
}

} // namespace

extern "C" int a2600debug_symbols_count(a2600debug* d)
{
  if(!d || !d->session->running) return 0;
  return with_dbg(d, [&](OSystem& os) { return static_cast<int>(user_symbols(os).size()); });
}

extern "C" int a2600debug_symbols_get(a2600debug* d, int index, uint16_t* addr, char* name, int namesz)
{
  if(!d || !d->session->running) return 0;
  return with_dbg(d, [&](OSystem& os) {
    const auto syms = user_symbols(os);
    if(index < 0 || index >= static_cast<int>(syms.size())) return 0;
    if(addr) *addr = syms[static_cast<size_t>(index)].addr;
    if(name && namesz > 0) snprintf(name, static_cast<size_t>(namesz), "%s", syms[static_cast<size_t>(index)].name.c_str());
    return 1;
  });
}

extern "C" int a2600debug_symbols_filter(a2600debug* d, const char* text, int* out, int max)
{
  if(!d || !out || max <= 0 || !d->session->running) return 0;
  const std::string needle = text ? text : "";
  return with_dbg(d, [&](OSystem& os) {
    const auto syms = user_symbols(os);
    std::vector<int> pick;
    for(size_t i = 0; i < syms.size(); ++i)
      if(contains_nocase(syms[i].name, needle)) pick.push_back(static_cast<int>(i));
    std::stable_sort(pick.begin(), pick.end(), [&](int x, int y) {
      const Sym& symx = syms[static_cast<size_t>(x)];
      const Sym& symy = syms[static_cast<size_t>(y)];
      if(symx.addr != symy.addr) return symx.addr < symy.addr;
      return std::lexicographical_compare(symx.name.begin(), symx.name.end(), symy.name.begin(), symy.name.end(),
        [](char p, char q) { return std::tolower(static_cast<unsigned char>(p)) < std::tolower(static_cast<unsigned char>(q)); });
    });
    const int n = std::min<int>(static_cast<int>(pick.size()), max);
    for(int i = 0; i < n; ++i) out[i] = pick[static_cast<size_t>(i)];
    return n;
  });
}

extern "C" void a2600debug_clear_symbols(a2600debug* d)
{
  if(!d || !d->session->running) return;
  with_dbg(d, [&](OSystem& os) {
    if(!os.hasConsole()) return 0;
    os.debugger().cartDebug().clearLabels();
    os.debugger().cartDebug().disassemblePC(true);
    return 0;
  });
  ++d->generation;
}

// ---- the trace ring -----------------------------------------------------------------

extern "C" void a2600debug_trace_enable(a2600debug* d, int on)
{
  if(!d || !d->session->running) return;
  with_dbg(d, [&](OSystem& os) {
    {
      const std::lock_guard<std::mutex> lock(d->traceMutex);
      if(on && d->ring.empty()) d->ring.resize(kRing);
    }
    d->traceOn = on != 0;
    set_trace_tap(d, os, on != 0);
    return 0;
  });
  ++d->generation;
}

extern "C" int a2600debug_trace_enabled(a2600debug* d)
{
  return d && d->traceOn.load() ? 1 : 0;
}

extern "C" int a2600debug_trace_read(a2600debug* d, a2600debug_trace_entry* out, int max)
{
  if(!d || !out || max <= 0) return 0;
  const std::lock_guard<std::mutex> lock(d->traceMutex);
  const size_t n = std::min<size_t>(d->ringCount, static_cast<size_t>(max));
  for(size_t i = 0; i < n; ++i)
    out[i] = d->ring[(d->ringHead + kRing - 1 - i) & (kRing - 1)];
  return static_cast<int>(n);
}

extern "C" void a2600debug_trace_clear(a2600debug* d)
{
  if(!d) return;
  {
    const std::lock_guard<std::mutex> lock(d->traceMutex);
    d->ringHead = d->ringCount = 0;
  }
  ++d->generation;
}

extern "C" int a2600debug_trace_count(a2600debug* d)
{
  if(!d) return 0;
  const std::lock_guard<std::mutex> lock(d->traceMutex);
  return static_cast<int>(d->ringCount);
}

// ---- memory search ---------------------------------------------------------------------

namespace {

constexpr unsigned kBusEnd = 0x1fff;

bool match_at(const std::vector<uint8_t>& mem, unsigned address, const uint8_t* pat, int n, uint8_t mask, int flags)
{
  for(int i = 0; i < n; i++)
  {
    uint8_t bpt = mem[address + static_cast<unsigned>(i)];
    uint8_t p = pat[i];
    if(flags & A2600DEBUG_SEARCH_NOCASE)
    {
      if(bpt >= 'a' && bpt <= 'z') bpt = static_cast<uint8_t>(bpt - 32);
      if(p >= 'a' && p <= 'z') p = static_cast<uint8_t>(p - 32);
    }
    if((bpt & mask) != (p & mask)) return false;
  }
  return true;
}

// The 13-bit bus as mapped now, side-effect free; false unless stopped.
bool snapshot(a2600debug* d, std::vector<uint8_t>& mem)
{
  if(!a2600debug_is_stopped(d)) return false;
  mem.assign(kBusEnd + 1, 0);
  return a2600debug_read(d, 0, mem.data(), static_cast<int>(kBusEnd + 1)) > 0;
}

int search_buffer(const std::vector<uint8_t>& mem, const uint8_t* pat, int n, uint8_t mask, int flags,
                  unsigned from, unsigned to, uint16_t* out, int max)
{
  int found = 0;
  for(unsigned a = from; a <= to && a + static_cast<unsigned>(n) - 1 <= kBusEnd && found < max; a++)
    if(match_at(mem, a, pat, n, mask, flags)) out[found++] = static_cast<uint16_t>(a);
  return found;
}

} // namespace

extern "C" int a2600debug_search(a2600debug* d, const uint8_t* pat, int n, uint8_t mask, int flags,
                                 uint16_t from, uint16_t to, uint16_t* out, int max)
{
  std::vector<uint8_t> mem;
  if(!d || !pat || n <= 0 || !out || max <= 0 || !snapshot(d, mem)) return -1;
  if(from > to) std::swap(from, to);
  if(from > kBusEnd) return 0;
  return search_buffer(mem, pat, n, mask, flags, from, std::min<unsigned>(to, kBusEnd), out, max);
}

extern "C" int a2600debug_search_next(a2600debug* d, const uint8_t* pat, int n, uint8_t mask, int flags,
                                      uint16_t from, int backward, uint16_t* found)
{
  std::vector<uint8_t> mem;
  uint16_t hit = 0;
  int result = 0;
  if(!d || !pat || n <= 0 || !found || !snapshot(d, mem)) return -1;
  const unsigned start = std::min<unsigned>(from, kBusEnd);
  if(!backward)
  {
    // after `from` to the end, then the start up to `from`
    if(start < kBusEnd && search_buffer(mem, pat, n, mask, flags, start + 1, kBusEnd, &hit, 1))
      result = 1;
    else if(search_buffer(mem, pat, n, mask, flags, 0, start, &hit, 1))
      result = 1;
  }
  else
  {
    // the nearest match before `from`, else the last one from `from` up
    for(int pass = 0; pass < 2 && !result; pass++)
    {
      const int first = pass == 0 ? static_cast<int>(start) - 1 : static_cast<int>(kBusEnd);
      const int last = pass == 0 ? 0 : static_cast<int>(start);
      for(int a = first; a >= last; a--)
        if(static_cast<unsigned>(a) + static_cast<unsigned>(n) - 1 <= kBusEnd
           && match_at(mem, static_cast<unsigned>(a), pat, n, mask, flags))
        {
          hit = static_cast<uint16_t>(a);
          result = 1;
          break;
        }
    }
  }
  if(result) *found = hit;
  return result;
}

// ---- the memory map --------------------------------------------------------------------

// The 6507 decodes 13 address lines: A12 the cartridge; below it A7 clear is
// the TIA, A7 set and A9 clear the RIOT's RAM, both set the RIOT's I/O
// ($0280-$029F, A4-A2 picking the register). The cartridge's pages come
// from the page table.
extern "C" int a2600debug_memory_map(a2600debug* d, a2600debug_region* out, int max)
{
  if(!d || !out || max <= 0 || !d->session->running) return 0;
  return with_dbg(d, [&](OSystem& os) {
    if(!os.hasConsole()) return 0;
    System& sys = os.console().system();
    struct Row { uint16_t low, high; int readable, writable; std::string name; };
    std::vector<Row> rows;
    auto push = [&](unsigned low, unsigned high, int readable, int writable, const std::string& name) {
      if(!rows.empty() && rows.back().name == name && rows.back().readable == readable
         && rows.back().writable == writable && rows.back().high + 1u == low)
        rows.back().high = static_cast<uint16_t>(high);
      else
        rows.push_back({ static_cast<uint16_t>(low), static_cast<uint16_t>(high), readable, writable, name });
    };
    for(unsigned base = 0; base < 0x1000; base += 0x20)
    {
      if(!(base & 0x80))
        push(base, base + 0x1f, 1, 1, (base & 0xff) < 0x40 && base < 0x100 ? "TIA registers" : "TIA (mirror)");
      else if(!(base & 0x200))
        push(base, base + 0x1f, 1, 1, base >= 0x80 && base < 0x100 ? "RIOT RAM (128 bytes)" : "RIOT RAM (mirror)");
      else
        push(base, base + 0x1f, 1, 1, base == 0x280 ? "RIOT I/O and timer" : "RIOT I/O (mirror)");
    }
    const int banks = os.debugger().cartDebug().romBankCount();
    for(unsigned base = 0x1000; base < 0x2000; base += 0x40)
    {
      const System::PageAccessType type = sys.getPageAccessType(static_cast<uInt16>(base));
      const int readable = type != System::PageAccessType::WRITE;
      const int writable = type != System::PageAccessType::READ;
      std::string name = type == System::PageAccessType::READWRITE ? "Cartridge RAM"
                       : type == System::PageAccessType::WRITE ? "Cartridge RAM (write port)"
                       : "Cartridge";
      if(type == System::PageAccessType::READ && banks > 1)
        name += " bank " + std::to_string(sys.cart().getBank(static_cast<uInt16>(base)));
      push(base, base + 0x3f, readable, writable, name);
    }
    int n = 0;
    for(const Row& row : rows)
    {
      if(n >= max) break;
      a2600debug_region& slot = out[n++];
      slot.lo = row.low; slot.hi = row.high; slot.readable = row.readable; slot.writable = row.writable;
      snprintf(slot.name, sizeof slot.name, "%s", row.name.c_str());
    }
    return n;
  });
}

// ---- sound ------------------------------------------------------------------------------

extern "C" void a2600debug_sound_get(a2600debug* d, a2600debug_sound* out)
{
  if(!out) return;
  memset(out, 0, sizeof *out);
  a2600debug_tia tia;
  a2600debug_tia_get(d, &tia);
  out->audc[0] = tia.audc0; out->audc[1] = tia.audc1;
  out->audf[0] = tia.audf0; out->audf[1] = tia.audf1;
  out->audv[0] = tia.audv0; out->audv[1] = tia.audv1;
  snprintf(out->freq[0], sizeof out->freq[0], "%s", tia.aud_freq0);
  snprintf(out->freq[1], sizeof out->freq[1], "%s", tia.aud_freq1);
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

// ---- FujiNet -------------------------------------------------------------------

extern "C" int a2600debug_fujinet_trace(a2600debug* d, a2600_fuji_packet* out, int max)
{
  return d ? fuji_trace_copy(d->fuji, out, max) : 0;
}

extern "C" void a2600debug_fujinet_trace_clear(a2600debug* d)
{
  if(d) fuji_trace_clear(d->fuji);
}

extern "C" int a2600debug_fujinet_state(a2600debug* d, char* dst, int dstsz)
{
  if(!d || !dst || dstsz <= 0) return 0;
  dst[0] = '\0';
  a2600session* s = d->session;
  if(!s->running) return put(dst, dstsz, "The 2600 is off\n");
  std::string out;
  if(s->fujinet_running)
    out = "FujiNet    running\nBoIP       127.0.0.1:" + std::to_string(A2600SESSION_BOIP_PORT)
          + "\nWeb admin  " + s->webui_url + "\n";
  else
    out = "FujiNet    off\n";
  out += with_dbg(d, [&](OSystem& os) -> std::string {
    if(!os.hasConsole()) return "Cartridge  none\n";
    Cartridge& c = os.console().cartridge();
    auto* fuji = dynamic_cast<CartridgeFUJI*>(&c);
    std::string r = "Cartridge  " + c.name() + " (" + c.detectedType() + ")\n";
    if(!fuji) return r + "Link       none: a local cartridge\n";
    const std::string status = fuji->linkStatus();
    r += "Link       ";
    r += status.rfind("connected", 0) == 0 ? "up" : "down";
    r += "\nStatus     " + status + "\n";
    r += "Booted     ";
    r += fuji->isFujiNetActive() ? "no game yet" : "a game (mailbox closed)";
    r += "\n";
    return r;
  });
  uint64_t packets[2] = { 0, 0 }, bytes[2] = { 0, 0 };
  fuji_trace_totals(d->fuji, packets, bytes);
  char line[200];
  snprintf(line, sizeof line, "\nSince the 2600 started\n"
           "  sent      %llu packets, %llu bytes\n"
           "  received  %llu packets, %llu bytes\n",
           (unsigned long long)packets[0], (unsigned long long)bytes[0],
           (unsigned long long)packets[1], (unsigned long long)bytes[1]);
  out += line;
  return put(dst, dstsz, out);
}
