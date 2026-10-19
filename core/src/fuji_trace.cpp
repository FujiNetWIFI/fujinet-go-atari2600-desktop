/*
 * fuji_trace.cpp -- see fuji_trace.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#include "fuji_trace.h"
#include "fuji_names.h"

namespace {

const char* fuji_command_name(int id)
{
  for(const auto& c : fuji_command_names)
    if(c.id == id)
      return c.name;
  return "";
}

// A packet and the hash of all its bytes, so a repeat is told from a
// packet that only shares its first 64.
struct Entry
{
  a2600_fuji_packet p;
  uint64_t hash;
};

uint64_t fnv1a(const std::vector<uint8_t>& v)
{
  uint64_t h = 1469598103934665603ULL;
  for(const uint8_t b : v)
  {
    h ^= b;
    h *= 1099511628211ULL;
  }
  return h;
}

bool same(const Entry& a, const Entry& b)
{
  return a.p.to_host == b.p.to_host && a.p.nbytes == b.p.nbytes && a.hash == b.hash
         && memcmp(a.p.data, b.p.data, std::min<size_t>(a.p.nbytes, sizeof a.p.data)) == 0;
}

void describe(a2600_fuji_packet& p)
{
  const char* way = p.to_host ? "FujiNet \xe2\x86\x92 2600" : "2600 \xe2\x86\x92 FujiNet";
  int n;
  if(p.device >= 0)
    n = snprintf(p.text, sizeof p.text, "%s %02X %02X %s (%d bytes)", way, p.device,
                 p.command, p.to_host ? "" : fuji_command_name(p.command), p.nbytes);
  else
    n = snprintf(p.text, sizeof p.text, "%s %d bytes", way, p.nbytes);
  if(p.repeat > 1 && n > 0 && n < static_cast<int>(sizeof p.text))
    snprintf(p.text + n, sizeof p.text - static_cast<size_t>(n), " \xc3\x97%u", p.repeat);
}

} // namespace

struct fuji_trace
{
  std::mutex mtx;
  std::deque<Entry> ring;
  uint64_t serial = 0;
  std::vector<uint8_t> slip[2];     // by to_host
  bool esc[2] = { false, false };
  uint64_t packets[2] = { 0, 0 }, bytes[2] = { 0, 0 };

  void Push(int to_host, const std::vector<uint8_t>& acc);
};

// One whole SLIP frame, decoded. A repeat of the last packet, or of the
// last exchange (this packet and the one before it, each way), folds into
// what is already there instead of growing the log.
void fuji_trace::Push(int to_host, const std::vector<uint8_t>& acc)
{
  Entry e = {};
  e.p.to_host = to_host;
  e.p.nbytes = static_cast<int>(acc.size());
  memcpy(e.p.data, acc.data(), std::min(acc.size(), sizeof e.p.data));
  e.p.device = acc.size() >= 6 ? acc[0] : -1;
  e.p.command = acc.size() >= 6 ? acc[1] : -1;
  e.p.length = acc.size() >= 6 ? (acc[2] | (acc[3] << 8)) : -1;
  e.p.repeat = 1;
  e.hash = fnv1a(acc);
  ++packets[to_host];

  const size_t n = ring.size();
  if(n >= 1 && same(ring[n - 1], e))
  {
    Entry& last = ring[n - 1];
    ++last.p.repeat;
    last.p.serial = ++serial;
    describe(last.p);
    return;
  }
  e.p.serial = ++serial;
  describe(e.p);
  ring.push_back(e);
  if(ring.size() >= 4)
  {
    Entry& x = ring[ring.size() - 4];
    Entry& y = ring[ring.size() - 3];
    const Entry& x2 = ring[ring.size() - 2];
    const Entry& y2 = ring[ring.size() - 1];
    if(x.p.to_host != y.p.to_host && x.p.repeat == y.p.repeat
       && x2.p.repeat == 1 && y2.p.repeat == 1 && same(x, x2) && same(y, y2))
    {
      ++x.p.repeat;
      ++y.p.repeat;
      y.p.serial = ++serial;
      describe(x.p);
      describe(y.p);
      ring.pop_back();
      ring.pop_back();
    }
  }
  while(ring.size() > FUJI_TRACE_MAX)
    ring.pop_front();
}

extern "C" fuji_trace* fuji_trace_new(void)
{
  return new fuji_trace();
}

extern "C" void fuji_trace_free(fuji_trace* t)
{
  delete t;
}

extern "C" void fuji_trace_feed(fuji_trace* t, int dir, const uint8_t* buf, size_t len)
{
  if(!t || !buf || !len) return;
  const std::lock_guard<std::mutex> lock(t->mtx);
  const int to_host = dir == 0 ? 1 : 0;
  // A request starts a new exchange: what was left of a reply never
  // finished, and must not glue onto the next one.
  if(!to_host)
  {
    t->slip[1].clear();
    t->esc[1] = false;
  }
  t->bytes[to_host] += len;
  std::vector<uint8_t>& acc = t->slip[to_host];
  for(size_t i = 0; i < len; ++i)
  {
    const uint8_t b = buf[i];
    if(b == 0xC0)
    {
      if(!acc.empty())
      {
        t->Push(to_host, acc);
        acc.clear();
      }
      t->esc[to_host] = false;
      continue;
    }
    if(t->esc[to_host])
    {
      acc.push_back(b == 0xDC ? 0xC0 : b == 0xDD ? 0xDB : b);
      t->esc[to_host] = false;
    }
    else if(b == 0xDB)
      t->esc[to_host] = true;
    else if(acc.size() < 4096)
      acc.push_back(b);
  }
}

extern "C" int fuji_trace_copy(fuji_trace* t, a2600_fuji_packet* out, int max)
{
  if(!t || !out || max <= 0) return 0;
  const std::lock_guard<std::mutex> lock(t->mtx);
  const size_t start = t->ring.size() > static_cast<size_t>(max) ? t->ring.size() - static_cast<size_t>(max) : 0;
  int n = 0;
  for(size_t i = start; i < t->ring.size(); ++i)
    out[n++] = t->ring[i].p;
  return n;
}

extern "C" void fuji_trace_clear(fuji_trace* t)
{
  if(!t) return;
  const std::lock_guard<std::mutex> lock(t->mtx);
  t->ring.clear();
}

extern "C" void fuji_trace_totals(fuji_trace* t, uint64_t packets[2], uint64_t bytes[2])
{
  if(!t) return;
  const std::lock_guard<std::mutex> lock(t->mtx);
  for(int i = 0; i < 2; ++i)
  {
    packets[i] = t->packets[i];
    bytes[i] = t->bytes[i];
  }
}
