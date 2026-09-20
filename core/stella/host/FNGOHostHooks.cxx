/*
 * FNGOHostHooks -- see the header.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "FNGOHostHooks.hxx"

namespace FNGOHostHooks
{
  std::function<void(std::string_view)> onTitle;
  std::function<void(std::string_view)> onMessage;
  std::function<bool(const std::string&)> onOpenURL;

  std::string& baseDir()
  {
    static std::string dir;
    return dir;
  }

  bool openURL(const std::string& url)
  {
    return onOpenURL ? onOpenURL(url) : false;
  }

  void title(std::string_view t)
  {
    if(onTitle) onTitle(t);
  }

  void message(std::string_view m)
  {
    if(onMessage) onMessage(m);
  }
}
