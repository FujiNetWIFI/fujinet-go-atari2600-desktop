/*
 * session.cpp -- the frontend contract (core/include/a2600session.h) over
 * the Stella host (core/stella/StellaHost.hxx).
 *
 * C++ because it owns the StellaHost and talks to Stella's objects inside
 * withStella() jobs; everything it exposes is the plain C API, and the C
 * modules (settings, paths, media, audio, gamepads, bindings) share the
 * struct through session_internal.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "CartFUJI.hxx"
#include "Console.hxx"
#include "Control.hxx"
#include "Event.hxx"
#include "OSystem.hxx"
#include "Props.hxx"
#include "PropsSet.hxx"
#include "Sound.hxx"

#include "StellaHost.hxx"
#include "host/FNGOHostHooks.hxx"

extern "C" {
#include "session_internal.h"
#include "a2600debug.h"
}

// debug.cpp
void a2600debug_note_stop(a2600session* s, const char* msg, int addr);
void a2600debug_destroy(a2600session* s);

namespace {

StellaHost* host_of(struct a2600session* s)
{
  return static_cast<StellaHost*>(s->host);
}

// ---- Stella event tables ------------------------------------------------

Event::Type port_event(int port, int act)
{
  static const Event::Type left[A2600_ACT_PER_PORT] = {
    Event::LeftJoystickUp, Event::LeftJoystickDown, Event::LeftJoystickLeft,
    Event::LeftJoystickRight, Event::LeftJoystickFire,
    Event::LeftPaddleADecrease, Event::LeftPaddleAIncrease, Event::LeftPaddleAFire,
    Event::LeftPaddleBDecrease, Event::LeftPaddleBIncrease, Event::LeftPaddleBFire,
    Event::LeftDrivingCCW, Event::LeftDrivingCW, Event::LeftDrivingFire,
    Event::LeftKeyboard1, Event::LeftKeyboard2, Event::LeftKeyboard3,
    Event::LeftKeyboard4, Event::LeftKeyboard5, Event::LeftKeyboard6,
    Event::LeftKeyboard7, Event::LeftKeyboard8, Event::LeftKeyboard9,
    Event::LeftKeyboardStar, Event::LeftKeyboard0, Event::LeftKeyboardPound,
  };
  static const Event::Type right[A2600_ACT_PER_PORT] = {
    Event::RightJoystickUp, Event::RightJoystickDown, Event::RightJoystickLeft,
    Event::RightJoystickRight, Event::RightJoystickFire,
    Event::RightPaddleADecrease, Event::RightPaddleAIncrease, Event::RightPaddleAFire,
    Event::RightPaddleBDecrease, Event::RightPaddleBIncrease, Event::RightPaddleBFire,
    Event::RightDrivingCCW, Event::RightDrivingCW, Event::RightDrivingFire,
    Event::RightKeyboard1, Event::RightKeyboard2, Event::RightKeyboard3,
    Event::RightKeyboard4, Event::RightKeyboard5, Event::RightKeyboard6,
    Event::RightKeyboard7, Event::RightKeyboard8, Event::RightKeyboard9,
    Event::RightKeyboardStar, Event::RightKeyboard0, Event::RightKeyboardPound,
  };
  return port ? right[act] : left[act];
}

Controller::Type stella_type(int t)
{
  switch(t)
  {
    case A2600_CTRL_JOYSTICK: return Controller::Type::Joystick;
    case A2600_CTRL_PADDLES:  return Controller::Type::Paddles;
    case A2600_CTRL_DRIVING:  return Controller::Type::Driving;
    case A2600_CTRL_KEYPAD:   return Controller::Type::Keyboard;
    default:                  return Controller::Type::Unknown;   // "AUTO"
  }
}

int our_type(Controller::Type t)
{
  switch(t)
  {
    case Controller::Type::Joystick:
    case Controller::Type::Genesis:
    case Controller::Type::BoosterGrip:
    case Controller::Type::Joy2BPlus:   return A2600_CTRL_JOYSTICK;
    case Controller::Type::Paddles:
    case Controller::Type::PaddlesIAxis:
    case Controller::Type::PaddlesIAxDr: return A2600_CTRL_PADDLES;
    case Controller::Type::Driving:     return A2600_CTRL_DRIVING;
    case Controller::Type::Keyboard:    return A2600_CTRL_KEYPAD;
    default:                            return A2600_CTRL_AUTO;
  }
}

// Stella's Console::setFormat index for our TV format.
uInt32 stella_format(int f)
{
  switch(f)
  {
    case A2600_TV_NTSC:  return 1;
    case A2600_TV_PAL:   return 2;
    case A2600_TV_SECAM: return 3;
    case A2600_TV_PAL60: return 5;
    default:             return 0;
  }
}

// Apply the session's controller choice to the running console: what
// Console::changeLeftController does internally, without the cycling. An
// explicit type is honoured by setControllers; AUTO ("Unknown") re-runs
// Stella's detector. CompuMate re-seats the cartridge and is left alone.
void apply_port_types(OSystem& os, struct a2600session* s)
{
  if(!os.hasConsole()) return;
  Console& console = os.console();
  if(console.cartridge().detectedType() != "CM")
  {
    Properties p = console.properties();
    p.set(PropType::Controller_Left, Controller::getPropName(stella_type(s->opts.port_type[0])));
    p.set(PropType::Controller_Right, Controller::getPropName(stella_type(s->opts.port_type[1])));
    console.setProperties(p);
    console.setControllers(p.get(PropType::Cart_MD5));
    os.propSet().insert(p, false);
  }
  // What the gamepad thread maps against: the type actually attached.
  s->effective_type[0] = our_type(console.leftController().type());
  s->effective_type[1] = our_type(console.rightController().type());
}

void apply_tv_format(OSystem& os, int format)
{
  if(!os.hasConsole()) return;
  os.console().setFormat(stella_format(format), true);
}

CartridgeFUJI* fuji_cart(OSystem& os)
{
  if(!os.hasConsole()) return nullptr;
  return dynamic_cast<CartridgeFUJI*>(&os.console().cartridge());
}

} // namespace

// ---- helpers shared with the C modules -----------------------------------

extern "C" void session_set_error(struct a2600session* s, const char* fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s->last_error, sizeof s->last_error, fmt, ap);
  va_end(ap);
}

#ifdef A2600_GAMEPAD_STUB
extern "C" int gamepad_start(struct a2600session* s)
{
  session_set_error(s, "gamepad support not built");
  return -1;
}
extern "C" void gamepad_stop(struct a2600session*) { }
extern "C" int a2600session_gamepad_count(a2600session*) { return 0; }
extern "C" int a2600session_gamepad_name(a2600session*, int, char* dst, int dstsz)
{
  if(dst && dstsz > 0) dst[0] = '\0';
  return 0;
}
extern "C" void a2600session_gamepad_assign(a2600session*, int, int) { }
extern "C" int a2600session_gamepad_assignment(a2600session*, int) { return -1; }
extern "C" int a2600session_gamepad_effective_port(a2600session*, int) { return -1; }
extern "C" unsigned a2600session_gamepad_generation(a2600session*) { return 0; }
extern "C" void a2600session_gamepad_capture_begin(a2600session*) { }
extern "C" void a2600session_gamepad_capture_cancel(a2600session*) { }
extern "C" int a2600session_gamepad_capture_poll(a2600session*, int*) { return 0; }
#endif

// ---- names -----------------------------------------------------------------

extern "C" const char* a2600_tv_format_name(int f)
{
  static const char* const names[A2600_TV_COUNT + 1] =
    { "Auto", "NTSC", "PAL", "PAL60", "SECAM", nullptr };
  return (f >= 0 && f < A2600_TV_COUNT) ? names[f] : nullptr;
}

extern "C" const char* a2600_ctrl_type_name(int t)
{
  static const char* const names[A2600_CTRL_COUNT + 1] =
    { "Auto", "Joystick", "Paddles", "Driving", "Keypad", nullptr };
  return (t >= 0 && t < A2600_CTRL_COUNT) ? names[t] : nullptr;
}

// ---- lifecycle -------------------------------------------------------------

extern "C" a2600session* a2600session_new(const a2600session_paths* paths)
{
  auto* s = static_cast<struct a2600session*>(calloc(1, sizeof(struct a2600session)));
  if(!s) return nullptr;

  pthread_mutex_init(&s->settings_mtx, nullptr);
  pthread_mutex_init(&s->sysact_mtx, nullptr);

  if(paths_init(s, paths ? paths->config_dir : nullptr,
                paths ? paths->data_dir : nullptr) != 0)
  {
    a2600session_free(s);
    return nullptr;
  }
  settings_init(s);
  bindings_init(s);

  snprintf(s->webui_url, sizeof s->webui_url, "http://127.0.0.1:%d/",
           A2600SESSION_WEBUI_PORT);
  if(paths && paths->fujinet_lib)
    snprintf(s->fujinet_lib, sizeof s->fujinet_lib, "%s", paths->fujinet_lib);
  if(paths && paths->fujinet_runtime_src)
    snprintf(s->fujinet_runtime_src, sizeof s->fujinet_runtime_src, "%s",
             paths->fujinet_runtime_src);

  // toggling switches start where a console powers up: colour, both A
  s->switch_state[A2600_SW_COLOR_BW] = 1;
  s->switch_state[A2600_SW_LEFT_DIFF] = 0;
  s->switch_state[A2600_SW_RIGHT_DIFF] = 0;

  s->host = new StellaHost();
  return s;
}

extern "C" void a2600session_free(a2600session* s)
{
  if(!s) return;
  a2600session_stop(s);
  a2600session_settings_flush(s);
  settings_free_all(s);
  a2600debug_destroy(s);
  delete host_of(s);
  pthread_mutex_destroy(&s->sysact_mtx);
  free(s);
}

extern "C" void a2600session_default_opts(a2600session* s, a2600session_start_opts* opts)
{
  memset(opts, 0, sizeof *opts);
  opts->tv_format = a2600session_get_int(s, "tv_format", A2600_TV_AUTO);
  opts->port_type[0] = a2600session_get_int(s, "port0_type", A2600_CTRL_AUTO);
  opts->port_type[1] = a2600session_get_int(s, "port1_type", A2600_CTRL_AUTO);
  opts->analog_joystick = a2600session_get_int(s, "analog_joystick", 1);
  opts->analog_paddle = a2600session_get_int(s, "analog_paddle", 1);
  opts->analog_driving = a2600session_get_int(s, "analog_driving", 1);
  opts->enable_fujinet = a2600session_get_int(s, "enable_fujinet", 1);
  opts->enable_audio = a2600session_get_int(s, "enable_audio", 1);
  opts->enable_gamepad = a2600session_get_int(s, "enable_gamepad", 1);
  opts->cart_path = a2600session_get_str(s, "cart", nullptr);
  if(opts->cart_path && !opts->cart_path[0])
    opts->cart_path = nullptr;
}

extern "C" int a2600session_start(a2600session* s, const a2600session_start_opts* opts)
{
  a2600session_start_opts local;

  if(s->running) return 0;
  s->last_error[0] = '\0';

  if(!opts)
  {
    a2600session_default_opts(s, &local);
    opts = &local;
  }
  s->opts = *opts;
  if(opts->cart_path)
    snprintf(s->cart_path, sizeof s->cart_path, "%s", opts->cart_path);
  else
    s->cart_path[0] = '\0';
  s->opts.cart_path = s->cart_path[0] ? s->cart_path : nullptr;

  // FujiNet FIRST: it listens and the cartridge dials in, so the listener
  // has to exist before the machine's first transaction or the CONFIG
  // client boots reporting no link. Failing to start is NOT fatal.
  if(opts->enable_fujinet)
  {
    if(fujinet_start(s) == 0)
      fujinet_wait_for_boip(s, 3000);
  }

  StellaHost::Config cfg;
  cfg.baseDir = s->stella_dir;
  cfg.startRom = s->cart_path;
  cfg.audioRate = A2600SESSION_AUDIO_RATE;
  cfg.audioStereo = true;
  cfg.options["fujinet"] = true;
  cfg.options["fujinet.host"] = "127.0.0.1";
  cfg.options["fujinet.port"] = static_cast<Int32>(A2600SESSION_BOIP_PORT);
  cfg.options["audio.volume"] = static_cast<Int32>(a2600session_get_int(s, "volume", 100));
  cfg.options["audio.preset"] = static_cast<Int32>(0);      // custom
  cfg.options["audio.resampling_quality"] = static_cast<Int32>(2);   // lanczos_2

  StellaHost::Callbacks cb;
  cb.onStopped = [s](StellaHost::StopReason r, const std::string& msg, int addr) {
    a2600debug_note_stop(s, r == StellaHost::StopReason::Paused ? "stopped" : msg.c_str(), addr);
  };
  cb.onConsoleReplaced = [s] {
    // A network boot (or our own reboot) replaced the console's
    // properties: re-apply the user's controller choice.
    if(auto* os = host_of(s)->osystem())
      apply_port_types(*os, s);
  };
  host_of(s)->setCallbacks(cb);

  std::string err;
  if(!host_of(s)->start(cfg, err))
  {
    session_set_error(s, "%s", err.c_str());
    fujinet_stop(s);
    return -1;
  }

  host_of(s)->withStella([s](OSystem& os) {
    apply_tv_format(os, s->opts.tv_format);
    apply_port_types(os, s);
    return 0;
  });

  if(opts->enable_gamepad && gamepad_start(s) != 0)
  {
    fprintf(stderr, "a2600: gamepads unavailable (%s)\n", s->last_error);
    s->last_error[0] = '\0';
  }
  if(opts->enable_audio && audio_start(s) != 0)
  {
    fprintf(stderr, "a2600: audio unavailable (%s); continuing silent\n", s->last_error);
    s->last_error[0] = '\0';
  }

  s->running = 1;
  return 0;
}

extern "C" void a2600session_stop(a2600session* s)
{
  if(!s->running) return;
  audio_stop(s);
  gamepad_stop(s);
  host_of(s)->stop();
  fujinet_stop(s);
  s->running = 0;
}

extern "C" int a2600session_is_running(const a2600session* s) { return s->running; }
extern "C" const char* a2600session_last_error(const a2600session* s) { return s->last_error; }

// ---- cartridges ------------------------------------------------------------

extern "C" int a2600session_load_cart(a2600session* s, const char* path)
{
  if(!s->running || !path || !*path) return -1;
  const std::string err = host_of(s)->loadRom(path);
  if(!err.empty())
  {
    session_set_error(s, "%s", err.c_str());
    return -1;
  }
  snprintf(s->cart_path, sizeof s->cart_path, "%s", path);
  s->opts.cart_path = s->cart_path;
  a2600session_set_str(s, "cart", path);
  return 0;
}

extern "C" const char* a2600session_cart_path(const a2600session* s)
{
  return s->cart_path;
}

extern "C" int a2600session_reboot_to_config(a2600session* s)
{
  if(!s->running) return -1;
  const std::string err = host_of(s)->rebootToConfig();
  if(!err.empty())
  {
    session_set_error(s, "%s", err.c_str());
    return -1;
  }
  s->cart_path[0] = '\0';
  s->opts.cart_path = nullptr;
  a2600session_set_str(s, "cart", "");
  return 0;
}

extern "C" int a2600session_eject(a2600session* s)
{
  return a2600session_reboot_to_config(s);
}

// ---- video / audio ---------------------------------------------------------

extern "C" int a2600session_copy_frame(a2600session* s, uint32_t* dst, int* height,
                                       uint64_t* serial_inout)
{
  static thread_local std::vector<uInt32> buf;
  StellaHost::FrameInfo info;
  if(!s->host) return 0;
  if(!host_of(s)->copyFrame(buf, info, serial_inout)) return 0;
  const size_t n = std::min(buf.size(),
    static_cast<size_t>(A2600SESSION_FB_WIDTH) * A2600SESSION_FB_MAX_HEIGHT);
  memcpy(dst, buf.data(), n * sizeof(uint32_t));
  if(height) *height = static_cast<int>(std::min<uInt32>(info.height, A2600SESSION_FB_MAX_HEIGHT));
  return 1;
}

extern "C" int a2600session_refresh_rate(a2600session* s)
{
  StellaHost::FrameInfo info;
  std::vector<uInt32> tmp;
  uint64_t zero = 0;
  if(s->host && host_of(s)->copyFrame(tmp, info, &zero))
    return info.refreshRate;
  return 60;
}

extern "C" void a2600session_notify_vsync(a2600session* s, int64_t frame_time_ns)
{
  if(s->host) host_of(s)->notifyVsync(frame_time_ns);
}

extern "C" int a2600session_render_audio(a2600session* s, float* out, int nframes)
{
  if(nframes <= 0) return 0;
  if(!s->host || !s->running)
  {
    memset(out, 0, sizeof(float) * 2 * static_cast<size_t>(nframes));
    return nframes;
  }
  host_of(s)->fillAudio(out, static_cast<uInt32>(nframes));
  return nframes;
}

extern "C" void a2600session_set_volume(a2600session* s, int percent)
{
  if(percent < 0) percent = 0;
  if(percent > 100) percent = 100;
  a2600session_set_int(s, "volume", percent);
  if(s->running)
    host_of(s)->withStella([percent](OSystem& os) {
      os.sound().setVolume(static_cast<uInt32>(percent), true);
      return 0;
    });
}

// ---- input -----------------------------------------------------------------

extern "C" void a2600session_press(a2600session* s, int target, int down)
{
  if(!s->running || target < 0 || target >= A2600_TARGET_COUNT) return;
  StellaHost& host = *host_of(s);

  if(target < 2 * A2600_ACT_PER_PORT)
  {
    host.enqueueEvent(port_event(target / A2600_ACT_PER_PORT,
                                 target % A2600_ACT_PER_PORT), down ? 1 : 0);
    return;
  }
  target -= 2 * A2600_ACT_PER_PORT;
  if(target < A2600_SW_COUNT)
  {
    switch(target)
    {
      case A2600_SW_SELECT: host.enqueueEvent(Event::ConsoleSelect, down ? 1 : 0); break;
      case A2600_SW_RESET:  host.enqueueEvent(Event::ConsoleReset, down ? 1 : 0); break;
      default:
        if(down) a2600session_switch_set(s, target, !s->switch_state[target]);
        break;
    }
    return;
  }
  if(down)
    a2600session_sysaction(s, target - A2600_SW_COUNT);
}

extern "C" void a2600session_analog(a2600session* s, int port, int paddle, int value)
{
  if(!s->running) return;
  if(value < -32767) value = -32767;
  if(value > 32767) value = 32767;
  Event::Type t;
  if(paddle < 0)
    t = port ? Event::RightDrivingAnalog : Event::LeftDrivingAnalog;
  else if(paddle == 0)
    t = port ? Event::RightPaddleAAnalog : Event::LeftPaddleAAnalog;
  else
    t = port ? Event::RightPaddleBAnalog : Event::LeftPaddleBAnalog;
  host_of(s)->enqueueEvent(t, value);
}

extern "C" void a2600session_switch_pulse(a2600session* s, int sw)
{
  if(!s->running) return;
  const Event::Type t = (sw == A2600_SW_SELECT) ? Event::ConsoleSelect : Event::ConsoleReset;
  // Held for a few frames: the machine samples the switches once a frame,
  // and real software debounces them over several.
  host_of(s)->enqueueEvent(t, 1);
  host_of(s)->withStella([t](OSystem&) { return 0; });
  struct timespec ts = { 0, 120 * 1000000L };
  nanosleep(&ts, nullptr);
  host_of(s)->enqueueEvent(t, 0);
}

extern "C" int a2600session_switch_get(a2600session* s, int sw)
{
  if(sw < 0 || sw >= A2600_SW_COUNT) return 0;
  return s->switch_state[sw];
}

extern "C" void a2600session_switch_set(a2600session* s, int sw, int on)
{
  if(sw < 0 || sw >= A2600_SW_COUNT) return;
  s->switch_state[sw] = on ? 1 : 0;
  if(!s->running) return;
  Event::Type t;
  switch(sw)
  {
    case A2600_SW_COLOR_BW:   t = on ? Event::ConsoleColor : Event::ConsoleBlackWhite; break;
    case A2600_SW_LEFT_DIFF:  t = on ? Event::ConsoleLeftDiffA : Event::ConsoleLeftDiffB; break;
    case A2600_SW_RIGHT_DIFF: t = on ? Event::ConsoleRightDiffA : Event::ConsoleRightDiffB; break;
    default: return;
  }
  // These are set-style events in Stella: the press selects the position
  // and it stays selected. Press then release so the handler's edge logic
  // sees one clean push.
  host_of(s)->enqueueEvent(t, 1);
  host_of(s)->enqueueEvent(t, 0);
}

extern "C" void a2600session_sysaction(a2600session* s, int sysact)
{
  switch(sysact)
  {
    case A2600_SYSACT_REBOOT_CONFIG: a2600session_reboot_to_config(s); break;
    case A2600_SYSACT_PAUSE:
      if(s->running) host_of(s)->dbgBreak();
      break;
    default: break;
  }
}

extern "C" void a2600session_sysaction_post(a2600session* s, int sysact)
{
  if(sysact < 0 || sysact >= A2600_SYSACT_COUNT) return;
  pthread_mutex_lock(&s->sysact_mtx);
  s->sysact_pending |= 1u << sysact;
  pthread_mutex_unlock(&s->sysact_mtx);
}

extern "C" int a2600session_sysaction_take(a2600session* s, int* out)
{
  int found = 0;
  pthread_mutex_lock(&s->sysact_mtx);
  for(int i = 0; i < A2600_SYSACT_COUNT; i++)
    if(s->sysact_pending & (1u << i))
    {
      s->sysact_pending &= ~(1u << i);
      if(out) *out = i;
      found = 1;
      break;
    }
  pthread_mutex_unlock(&s->sysact_mtx);
  return found;
}

// The gamepad thread's entry points: same routing as press/analog, but
// never touching the settings or the debugger from that thread.
extern "C" void session_gamepad_apply(struct a2600session* s, int port, int act, int down)
{
  if(!s->running || port < 0 || port > 1 || act < 0 || act >= A2600_ACT_PER_PORT) return;
  host_of(s)->enqueueEvent(port_event(port, act), down ? 1 : 0);
}

extern "C" void session_gamepad_analog(struct a2600session* s, int port, int paddle, int value)
{
  a2600session_analog(s, port, paddle, value);
}

// ---- controller types (live) -----------------------------------------------

extern "C" void a2600session_set_port_type(a2600session* s, int port, int type)
{
  if(port < 0 || port > 1 || type < 0 || type >= A2600_CTRL_COUNT) return;
  s->opts.port_type[port] = type;
  a2600session_set_int(s, port ? "port1_type" : "port0_type", type);
  if(!s->running) return;
  host_of(s)->withStella([s](OSystem& os) {
    apply_port_types(os, s);
    return 0;
  });
}

extern "C" int a2600session_port_type(a2600session* s, int port)
{
  if(port < 0 || port > 1) return A2600_CTRL_AUTO;
  if(s->running) return s->opts.port_type[port];
  return a2600session_get_int(s, port ? "port1_type" : "port0_type", A2600_CTRL_AUTO);
}

extern "C" int a2600session_detected_port_type(a2600session* s, int port)
{
  if(!s->running || port < 0 || port > 1) return A2600_CTRL_AUTO;
  return host_of(s)->withStella([port](OSystem& os) {
    if(!os.hasConsole()) return static_cast<int>(A2600_CTRL_AUTO);
    return our_type(port ? os.console().rightController().type()
                         : os.console().leftController().type());
  });
}

extern "C" void a2600session_set_analog(a2600session* s, int joystick, int paddle, int driving)
{
  s->opts.analog_joystick = joystick ? 1 : 0;
  s->opts.analog_paddle = paddle ? 1 : 0;
  s->opts.analog_driving = driving ? 1 : 0;
  a2600session_set_int(s, "analog_joystick", s->opts.analog_joystick);
  a2600session_set_int(s, "analog_paddle", s->opts.analog_paddle);
  a2600session_set_int(s, "analog_driving", s->opts.analog_driving);
}

// ---- FujiNet ---------------------------------------------------------------

extern "C" int a2600session_fujinet_running(const a2600session* s)
{
  return s->fujinet_running;
}

extern "C" const char* a2600session_fujinet_webui_url(const a2600session* s)
{
  return s->webui_url;
}

extern "C" int a2600session_cart_link_up(a2600session* s)
{
  if(!s->running) return -1;
  return host_of(s)->withStella([](OSystem& os) {
    CartridgeFUJI* fuji = fuji_cart(os);
    if(!fuji) return -1;
    return fuji->linkStatus().rfind("connected", 0) == 0 ? 1 : 0;
  });
}

extern "C" int a2600session_cart_status(a2600session* s, char* dst, int dstsz)
{
  if(!dst || dstsz <= 0) return 0;
  dst[0] = '\0';
  if(!s->running) return snprintf(dst, static_cast<size_t>(dstsz), "stopped");
  const std::string status = host_of(s)->withStella([](OSystem& os) -> std::string {
    CartridgeFUJI* fuji = fuji_cart(os);
    if(!fuji) return os.hasConsole() ? "local cartridge" : "no console";
    return fuji->linkStatus();
  });
  return snprintf(dst, static_cast<size_t>(dstsz), "%s", status.c_str());
}

extern "C" int a2600session_cart_booted_game(a2600session* s)
{
  if(!s->running) return 0;
  return host_of(s)->withStella([](OSystem& os) {
    CartridgeFUJI* fuji = fuji_cart(os);
    return (fuji && !fuji->isFujiNetActive()) ? 1 : 0;
  });
}

// ---- paths -----------------------------------------------------------------

extern "C" const char* a2600session_config_path(const a2600session* s) { return s->config_dir; }
extern "C" const char* a2600session_data_path(const a2600session* s) { return s->data_dir; }
extern "C" const char* a2600session_carts_path(const a2600session* s) { return s->carts_dir; }
extern "C" const char* a2600session_sd_path(const a2600session* s) { return s->fujinet_sd; }

// ---- debugger --------------------------------------------------------------

extern "C" a2600debug* a2600session_debugger(a2600session* s)
{
  return a2600debug_get(s);
}

// Reachable from the debugger module (core/src/debug.cpp) without a public
// C++ header: the host behind a session.
StellaHost* a2600session_host(a2600session* s)
{
  return host_of(s);
}
