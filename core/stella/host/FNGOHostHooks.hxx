/*
 * FNGOHostHooks -- the few things Stella's platform layer asks the host for.
 *
 * Stella's backends normally reach the desktop directly (SDL opens URLs,
 * shows the window title, prints OSD messages). Here the desktop belongs to
 * the native frontend, so the FNGO backend classes route those requests
 * through this table, which the session layer fills in before Stella is
 * initialised. Everything is optional: an unset hook is a no-op, so the core
 * tests can run with none of them.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef FNGO_HOST_HOOKS_HXX
#define FNGO_HOST_HOOKS_HXX

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

  bool openURL(const std::string& url);
  void title(std::string_view t);
  void message(std::string_view m);
}

#endif // FNGO_HOST_HOOKS_HXX
