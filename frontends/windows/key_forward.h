/*
 * Win32 key messages to X11/xkb keysyms, by hardware scan code through the
 * session's HID table -- the same physical-key route the other frontends
 * take, so a binding names the key whatever Shift or the layout is doing.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <stdint.h>
#include <windows.h>

#include "a2600session.h"

static inline uint32_t a2600_keysym_from_msg(WPARAM vk, LPARAM lp)
{
    const unsigned scancode = (unsigned)((lp >> 16) & 0xff);
    const int extended = (lp >> 24) & 1;
    uint32_t ks = a2600session_keysym_from_win_scancode(scancode, extended);
    if (ks) return ks;
    /* Fallbacks for keys the scan-code table has no usage for. */
    switch (vk) {
    case VK_UP: return A2600_KEYSYM_UP;
    case VK_DOWN: return A2600_KEYSYM_DOWN;
    case VK_LEFT: return A2600_KEYSYM_LEFT;
    case VK_RIGHT: return A2600_KEYSYM_RIGHT;
    case VK_ESCAPE: return A2600_KEYSYM_ESCAPE;
    case VK_RETURN: return A2600_KEYSYM_RETURN;
    case VK_SPACE: return A2600_KEYSYM_SPACE;
    case VK_TAB: return A2600_KEYSYM_TAB;
    case VK_BACK: return A2600_KEYSYM_BACKSPACE;
    default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F12) return A2600_KEYSYM_F1 + (uint32_t)(vk - VK_F1);
    if (vk >= '0' && vk <= '9') return (uint32_t)vk;
    if (vk >= 'A' && vk <= 'Z') return (uint32_t)(vk - 'A' + 'a');
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return (uint32_t)('0' + (vk - VK_NUMPAD0));
    return 0;
}
