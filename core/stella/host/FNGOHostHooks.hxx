/*
 * FNGOHostHooks -- the few things Stella's platform layer asks the host for.
 *
 * Stella's backends normally reach the desktop directly (SDL opens URLs,
 * shows the window title, prints OSD messages). Here the desktop belongs to
 * the native frontend, so the FNGO backend classes route those requests
 * through this table, which the session layer fills in before Stella is
 * initialised. The shadowed FujiNetLink.cxx reports its bytes here too, for
 * the debugger's FujiNet trace. Everything is optional: an unset hook is a
 * no-op, so the core tests can run with none of them.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef FNGO_HOST_HOOKS_HXX
#define FNGO_HOST_HOOKS_HXX

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace FNGOHostHooks
{
  // The directory Stella treats as its base/home dir (settings, the spilled
  // CONFIG ROM under <base>/fujinet/). Must end with a path separator.
  // Set by StellaHost before OSystem::initialize().
  std::string& baseDir();

  // Window title / OSD message / gauge message, as Stella's SDL backend
  // would show them. Called on the Stella thread.
  extern std::function<void(std::string_view)> onTitle;
  extern std::function<void(std::string_view)> onMessage;

  // MediaFactory::openURL: a frontend opens it in the system browser.
  extern std::function<bool(const std::string&)> onOpenURL;

  // The FujiNet cartridge's link, every byte it sends (dir 1) and receives
  // (dir 0, whole SLIP frames), for the debugger's trace. Called on the
  // cartridge's worker thread; set while no console runs.
  extern std::function<void(int, const uint8_t*, size_t)> onFujiNetBytes;

  bool openURL(const std::string& url);
  void title(std::string_view t);
  void message(std::string_view m);
  void fujinetBytes(int dir, const uint8_t* buf, size_t len);
}

#endif // FNGO_HOST_HOOKS_HXX
