/*
 * vidcap.cpp -- the C face of the host's Video tab capture, for
 * core/src/vidtab.c (a2600vidcap.h).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "StellaHost.hxx"
#include "a2600vidcap.h"
#include "session_internal.h"

StellaHost* a2600session_host(a2600session* sess);

namespace {

StellaHost* host_of_running(a2600session* sess)
{
  if(!sess || !sess->running) return nullptr;
  StellaHost* host = a2600session_host(sess);
  return host && host->running() ? host : nullptr;
}

}

extern "C" int a2600vid_capture_running(a2600session* sess)
{
  return host_of_running(sess) != nullptr;
}

extern "C" void a2600vid_capture_enable(a2600session* sess, int enable)
{
  if(StellaHost* host = host_of_running(sess)) host->captureEnable(enable != 0);
}

extern "C" int a2600vid_capture_get(a2600session* sess, a2600vid_capture* dst, uint64_t* serial)
{
  StellaHost* host = host_of_running(sess);
  if(!host || !dst) return 0;
  uInt64 seen = serial ? *serial : 0;
  const bool copied = host->captureGet(dst, &seen);
  if(copied && serial) *serial = seen;
  return copied ? 1 : 0;
}

extern "C" int a2600vid_capture_fill(a2600session* sess, a2600vid_capture* dst)
{
  StellaHost* host = host_of_running(sess);
  return host && dst && host->captureFill(dst) ? 1 : 0;
}
