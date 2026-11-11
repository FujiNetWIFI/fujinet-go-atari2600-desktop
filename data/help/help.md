This is the text of FujiNet Go Atari 2600's built-in Help (Help ▸ FujiNet Go
Atari 2600 Help, or Ctrl+Shift+H; ⌘? on a Mac). The app shows each part only
to the frontend it is about; here you see them all. Every topic below is one
page in the app.

# Getting started

FujiNet Go Atari 2600 is an Atari 2600 with a FujiNet cartridge in its slot.
The console is Stella: every cartridge mapper, the cycle-exact TIA and the
6507 and RIOT. The FujiNet runs inside the app, so CONFIG comes up at once and
can boot games from the network.

1. **Start the app.** The console powers on and runs FujiNet's CONFIG. Nothing
   needs importing: the 2600 has no BIOS.
2. **Pick a game in CONFIG.** Choose a host (the SD folder or a server on the
   network) and a game on it. FujiNet sends it to the cartridge and it
   starts. See [FujiNet](#fujinet).
3. **Or open a cartridge of your own.** **Open Cartridge…**
   (<kbd>Ctrl+O</kbd>) takes an `.a26`, `.bin`, `.rom` or `.fuji` image. See
   [Cartridges](#cartridges).
4. **Play.** The keys, the gamepads and the Keypads window
   (<kbd>F9</kbd>) work the two ports and the console's switches. See
   [Controllers and keys](#controllers-and-keys).
5. **Change the console.** [Preferences](#preferences) picks the TV format,
   the controllers, the sound and FujiNet.

## This Help

The list on the left has a topic for each part of the app; the search box
above it finds every section that has all the words you type. A **Help**
button in Preferences, the Debugger, the Keypads window and the FujiNet
console log opens the topic for what it shows, and
<kbd>Ctrl+Shift+H</kbd> does the same for the window you are in.

<!-- only: gnome kde windows -->

In Preferences, the Debugger and the console log, <kbd>F1</kbd> does too.
(In the main window <kbd>F1</kbd> is the console's SELECT switch.)

<!-- end -->

The same text is on GitHub, as
[data/help/help.md](https://github.com/FujiNetWIFI/fujinet-go-atari2600-desktop/blob/main/data/help/help.md).

<!-- only: macos -->

On a Mac, the Help menu's search field also finds Help topics.

<!-- end -->

<!-- context: main -->
# The window

The console's picture fills the window. The menus have:

- **Open Cartridge…**, **Eject Cartridge**, **Import Cartridge to SD…** and
  **Reboot to CONFIG**: see [Cartridges](#cartridges).
- **Select**, **Reset**, **Color** (Color / B&W) and the two difficulty
  switches: the console's switches.
- **Keypads** (<kbd>F9</kbd>) and **Debugger** (<kbd>F12</kbd>).
- **TV Aspect**, **Smooth Scaling** and **Fullscreen** (<kbd>F11</kbd>).
- **FujiNet Configuration** (the web UI) and **Console Log**.
- **Preferences** (<kbd>Ctrl+,</kbd>), **Help**, **About** and **Quit**.

<!-- only: gnome -->

On GNOME, everything is in the main menu (the button at the right of the
header bar).

<!-- end -->

<!-- only: kde windows -->

The menu bar has Machine, Console, View, FujiNet and Help. The Machine menu
has the cartridge items, Preferences (Settings on Windows) and Quit (Exit on
Windows).

<!-- end -->

<!-- only: macos -->

On a Mac, Preferences (called Settings) and Quit are in the app menu, and the
cartridge items and the switches are in the Machine menu.

<!-- end -->

The status shows the cartridge and FujiNet's state:

| Status | Means |
|---|---|
| connected | The cartridge is talking to FujiNet. |
| link down | FujiNet is off or not answering yet; CONFIG tries again. |
| loading 42% | FujiNet is sending a program to the cartridge. |
| game running; mailbox closed | A game that does not use FujiNet is running. |

Messages (a cartridge opened, a gamepad plugged in, FujiNet restarted) show
for a few seconds over the picture.

## Power and reset

- **Reboot to CONFIG** (<kbd>Ctrl+R</kbd>, or <kbd>Escape</kbd>) is the power
  cycle. On the hardware a game booted through FujiNet cannot be undone with
  the RESET switch, so this ejects an opened cartridge and boots CONFIG, as at
  power-on.
- **Reset** (<kbd>F2</kbd>) presses the console's RESET switch.
- **Select** (<kbd>F1</kbd>) presses SELECT, and **Color** (<kbd>F3</kbd>)
  flips Color / B&W.
- **Left difficulty** (<kbd>Alt+L</kbd>) and **Right difficulty**
  (<kbd>Alt+R</kbd>) flip the difficulty switches, A (pro) or B (novice).

## The picture

A television shows the picture 4:3; **TV Aspect** switches between that and
square pixels. **Smooth Scaling** blends the pixels when the picture is
stretched, and leaves them sharp when it is off.

# Controllers and keys

Each of the two ports holds a **joystick** (a stick and one fire button), a
pair of **paddles**, a **driving controller** (a wheel and a button) or a
**keypad** (the 12-key controller of Star Raiders and BASIC Programming).
Preferences ▸ Machine ▸ Controllers picks what is plugged in.

## Default keys

Keys go by where they are on the keyboard, whatever your layout.

| Does | Left port | Right port |
|---|---|---|
| Joystick directions | The arrow keys | <kbd>Y</kbd>, <kbd>H</kbd>, <kbd>G</kbd>, <kbd>J</kbd> |
| Joystick fire | <kbd>Space</kbd> | <kbd>F</kbd> |
| Paddle A | <kbd>Left</kbd> and <kbd>Right</kbd>, fire <kbd>Space</kbd> | <kbd>G</kbd> and <kbd>J</kbd>, fire <kbd>F</kbd> |
| Paddle B | <kbd>Up</kbd> and <kbd>Down</kbd>, fire <kbd>Left Ctrl</kbd> | <kbd>Y</kbd> and <kbd>H</kbd>, fire <kbd>V</kbd> |
| Driving wheel | <kbd>Left</kbd> and <kbd>Right</kbd>, fire <kbd>Space</kbd> | <kbd>G</kbd> and <kbd>J</kbd>, fire <kbd>F</kbd> |

The keypads are 1 2 3 / 4 5 6 / 7 8 9 / `*` 0 #: the left port's are the keys
<kbd>1</kbd> <kbd>2</kbd> <kbd>3</kbd> / <kbd>Q</kbd> <kbd>W</kbd>
<kbd>E</kbd> / <kbd>A</kbd> <kbd>S</kbd> <kbd>D</kbd> / <kbd>Z</kbd>
<kbd>X</kbd> <kbd>C</kbd>, and the right port's are <kbd>8</kbd> <kbd>9</kbd>
<kbd>0</kbd> / <kbd>I</kbd> <kbd>O</kbd> <kbd>P</kbd> / <kbd>K</kbd>
<kbd>L</kbd> <kbd>;</kbd> / <kbd>,</kbd> <kbd>.</kbd> <kbd>/</kbd>.

| Console | Key |
|---|---|
| Select | <kbd>F1</kbd> |
| Reset | <kbd>F2</kbd> |
| Color | <kbd>F3</kbd> |
| Reboot to CONFIG | <kbd>Escape</kbd> |

A key does one thing: giving a key to a new job takes it from the old one.

The app keeps these keys for itself:

| Shortcut | Does |
|---|---|
| <kbd>Ctrl+O</kbd> | Open Cartridge |
| <kbd>Ctrl+R</kbd> | Reboot to CONFIG |
| <kbd>Ctrl+,</kbd> | Preferences |
| <kbd>Ctrl+Shift+H</kbd> | Help |
| <kbd>F9</kbd> | Keypads |
| <kbd>F11</kbd> | Fullscreen |
| <kbd>F12</kbd> | Debugger |

<!-- only: gnome kde windows -->

Keys with <kbd>Ctrl</kbd> never reach the console.

<!-- end -->

<!-- only: macos -->

Keys with ⌘ never reach the console.

<!-- end -->

<!-- only: linux -->

Key combinations with the Super (Windows) key are the desktop's.

<!-- end -->

## Gamepads

Gamepads are picked up as they are plugged in, and can come and go at any
time. A pad plugged in while a game runs is assigned to the next free port
within a second, and shown in Preferences; unplugging releases it, and a pad
that comes back gets its port back. A pad with no standard layout (a plain USB
adapter) uses its raw buttons and hats. The D-pad moves a joystick; whether
the analog sticks may drive a joystick, a paddle or a driving controller is
decided by three switches in Preferences ▸ Machine ▸ Analog sticks drive.

## Changing the controls

The controls are changed in the Keypads window (<kbd>F9</kbd>): click **Map**,
then click a control, then press the key or the gamepad button to give it. The
control's old key or button is taken from it. **Defaults** puts every control
back. The map is kept in the settings, and only what differs from the defaults
is written.

<!-- context: keypad -->
# Keypads window

The Keypads window (<kbd>F9</kbd>) shows both keyboard controllers side by
side, each 1 2 3 / 4 5 6 / 7 8 9 / `*` 0 #, with a row of fire buttons for the
joystick underneath and the console's switches (Select, Reset, Color / B&W,
both difficulty switches, Reboot to CONFIG). It floats on its own at a fixed
size. The controls that are held light up in the accent colour, whoever holds
them: the keyboard, a gamepad or the mouse.

| With the mouse | Does |
|---|---|
| Press a key or a button | Holds it for as long as the button is down |
| Press a switch | Presses it |

The machine samples its controls once a frame, so a button is *held*, not
pulsed, as the plastic one is. Switching to another window lets go of
everything the mouse holds. Keys typed in the Keypads window go to the
console. **Map** and **Defaults** change the controls (see
[Changing the controls](#changing-the-controls)).

# The machine

The 2600 has one machine, with Stella's palettes and its TV effects.
Preferences ▸ Machine ▸ **TV format** decides how the picture is drawn:

- **Auto**: each cartridge decides, by Stella's frame layout detection.
- **NTSC**, **PAL**, **PAL60** and **SECAM**: forced.

A change restarts the session when Preferences closes.

# Cartridges

A cartridge is a file: an `.a26`, `.bin`, `.rom` or `.fuji`. Stella detects
the mapper from the image, and its database names the game and the controllers
it wants.

- **A FujiNet app** is an image that claims the mailbox. It runs in the
  FujiNet cartridge (`CartFUJI`), which speaks the same mailbox protocol as the
  RP2040 cartridge from the firmware's own sources: it can talk to the
  network.
- **A game** is any other image, run on Stella's own mapper for it.

## Open Cartridge

**Open Cartridge…** (<kbd>Ctrl+O</kbd>) runs the file directly. The cartridge
is remembered and starts again next time; **Eject Cartridge** or **Reboot to
CONFIG** forgets it. Dropping a cartridge file on the window does the same. A
file the console cannot take is refused with the reason.

<!-- context: import-sd -->
## Import Cartridge to SD

**Import Cartridge to SD…** copies a cartridge into FujiNet's SD folder, so
CONFIG can list it and boot it through the cartridge. The FujiNet cartridge
refuses what it cannot serve, and the message says why.

# FujiNet

The FujiNet is FujiNet's own firmware, running inside the app, and the
cartridge in the slot is its connection to the console. The cartridge speaks
BoIP (FujiBus over TCP) over loopback to FujiNet. FujiNet listens; the
cartridge dials in, and keeps trying again if the link drops.

## CONFIG

CONFIG is FujiNet's program for choosing what to run:

- **Hosts** are where games come from: the SD folder, and servers on the
  network.
- Pick a host and a game on it. FujiNet sends the image to the cartridge,
  which starts it, and the game can keep talking to the network.
- **Reboot to CONFIG** brings CONFIG back.

## The FujiNet menu

- **FujiNet Configuration** opens FujiNet's settings page in your browser, at
  `http://127.0.0.1:11505/`. Only this computer can reach it.
- **Console Log** shows what FujiNet prints. See
  [The console log](#the-console-log).

<!-- context: fujilog -->
## The console log

The FujiNet Console Log window shows FujiNet's own messages as they come,
following the end while you are at the bottom. It is also kept in
`fujinet-console.log` in the app's `fujinet` folder.

## Turning it off

Preferences ▸ Host ▸ **Enable FujiNet** turns the FujiNet off. The cartridge
then reports *link down*, CONFIG runs without hosts, and nothing is dialled.
Games are not affected.

## An external FujiNet

Preferences ▸ Host ▸ **Use external FujiNet** skips the built-in FujiNet and
connects to another FujiNet instance at the address you give, for example
`192.168.1.20:11504`. Leave the field empty for `127.0.0.1:11504`. Start that
instance listening there (its `[BOIP]` port). FujiNet Configuration and
Console Log belong to the built-in FujiNet, so they say the FujiNet is
external; use the other instance's own. The Debugger's FujiNet tab still shows
the traffic.

## Behind the scenes

- The cartridge and FujiNet talk over TCP port 11504 on 127.0.0.1 (the
  BoIP port), and the web UI is on port 11505. Nothing from outside can
  reach either.
- FujiNet's settings are `fnconfig.ini` in the app's `fujinet` folder. It is
  written once and then left for FujiNet and you.

# Preferences

Preferences (<kbd>Ctrl+,</kbd>) set up the console. It is one page of groups.
The controllers apply at once, even to a game booted over FujiNet; the TV
format and the Host settings apply by restarting the session when Preferences
closes. The settings are one file, shared by the GNOME and KDE apps on the
computer.

<!-- only: macos -->

On a Mac this window is called **Settings** (in the app menu).

<!-- end -->

<!-- only: windows -->

On Windows this window is called **Settings** (in the Machine menu).

<!-- end -->

<!-- context: prefs/Machine -->
## Machine settings

- **TV format**: *Auto*, *NTSC*, *PAL*, *PAL60* or *SECAM* (setting
  `tv_format`).

## Controller settings

- **Left port** and **Right port**: what is plugged into each port: *Auto*
  (Stella's ROM database and controller detector), *Joystick*, *Paddles*,
  *Driving* or *Keypad* (settings `port0_type` and `port1_type`).

## Analog stick settings

Whether a gamepad's sticks move these controllers, or only its D-pad does:

- **Joystick**: the left stick as the four directions (setting
  `analog_joystick`).
- **Paddles**: the left stick's X is paddle A, the right stick's X is paddle B
  (setting `analog_paddle`).
- **Driving controller**: the left stick's X is the wheel (setting
  `analog_driving`).

## Gamepad settings

**Gamepads** lists each one plugged in, and which port it drives. A new
gamepad drives the first free port; choose *Left*, *Right* or *Automatic* to
fix it. The keys and buttons are changed in the Keypads window (see
[Changing the controls](#changing-the-controls)).

## Audio settings

- **Volume**: the output level, 0 to 100 (setting `volume`).

## Host settings

- **Enable FujiNet**: run the built-in FujiNet (setting `enable_fujinet`).
- **Use external FujiNet**: connect to another instance instead (setting
  `fujinet_external`).
- **Address**: its `host:port`, or just a port (see
  [An external FujiNet](#an-external-fujinet); setting
  `fujinet_external_addr`).
- **Audio**: open the system audio device (setting `enable_audio`).
- **Gamepads**: poll USB and Bluetooth gamepads (setting `enable_gamepad`).

## The settings file

The settings file is `settings.ini` (see [Files and folders](#files-and-folders)).
Each setting above names its key. The others are `tv_aspect` and `smooth` (the
View menu's picture options), `cart` (the opened cartridge, empty for CONFIG)
and `bindings` (the controls that differ from the defaults).

# Debugger

The Debugger (<kbd>F12</kbd>) stops the console and shows what it is doing.
It has thirteen tabs; closing it lets the console go on as it was. The 6507's
registers and flags sit in a strip under the tabs: edit a value while stopped
and press <kbd>Enter</kbd>. The engine is Stella's own debugger.

| Button | Does |
|---|---|
| Stop or Run (<kbd>F5</kbd>) | Stops before the next instruction, or runs on |
| Step (<kbd>F7</kbd>) | One instruction |
| Step Over (<kbd>F8</kbd>) | Over a JSR, to the instruction after it |
| Step Out (<kbd>Shift+F8</kbd>) | On until the routine returns |
| Scan+1 | On to the next scan line |
| Frame+1 | On to the next frame |
| Rewind and Unwind | Back and forward through the recorded states |
| Clear Breakpoints | Removes every breakpoint |
| Break at | A symbol name or hex address, then <kbd>Enter</kbd> |
| Load Symbols | Reads a DASM `.sym` or `.lst` file next to the cartridge (a batari Basic program's `.bas.sym` too) |
| Clear Symbols | Drops the loaded symbols |
| Pop Out | Moves the tab that shows into a window of its own; **Dock** in that window puts it back |
| Help | Opens the topic of the tab that shows |

Every stop brings the views back to where the machine is, even if you had
scrolled away, and the line at the top says why it stopped.

<!-- context: debugger/console -->
## Console

Stella's own command parser, with <kbd>Tab</kbd> completion: `help`, `break`,
`breakIf`, `trap`, `trapWrite`, `timer`, `saveStateIf`, `logTrace`, `define`,
`function`, the `code`, `data`, `gfx`, `pgfx` and `row` directives, `saveDis`,
`saveRom` and the rest. Type `help` for every command. A condition may use
`old`, the byte a write replaces, and `value`, the byte written. These are
FujiNet Go's own:

| Command | Does |
|---|---|
| `cart` | The cartridge: scheme, bank and the FujiNet link |
| `video` | The Video tab as text: `video colours`, `video lines`, `video writes` |
| `watch`, `unwatch` | The watch list: `watch score`, `unwatch 0` (replaces Stella's `watch`) |
| `find` | Searches memory for bytes (`find A9 00`) or text (`find text HELLO`) |

<!-- context: debugger/disassembly -->
## Disassembly

The DiStella listing of one bank, with the opcode bytes, cycles and labels. The
current instruction is in the app's accent colour and a breakpoint is a red dot
in the margin (a grey hollow one when it is disabled); click a line to toggle
its breakpoint. The bank chooser picks the bank shown, and **Follow PC** keeps
the current line in view.

<!-- context: debugger/basic -->
## BASIC

Debug a batari Basic program by its source. **Import File...** takes a `.bas`
file and **Import Folder...** a folder of them; the program's listing (the
`.bas.lst` that `2600basic.sh` leaves beside the `.bas`, with its `.bas.sym`)
is found next to it, and **Load Map...** picks a listing by hand. Loading a
cartridge whose `.bas` sits beside it imports that for you. The `.sym` gives
the variables their source names in Memory, Watches and the Console.

The source shows with a red dot on every line that has a breakpoint (a grey
hollow one when it is disabled) and the line the machine is on in the app's
accent colour; click a line to toggle its breakpoint. Lines with no code, such
as labels, `rem` and `dim`, are dimmed and cannot hold one.

While this tab shows, **Step** (<kbd>F7</kbd>), **Step Over** (<kbd>F8</kbd>)
and **Step Out** (<kbd>Shift+F8</kbd>) go by BASIC line instead of by
instruction: Step runs to the next line, into a `gosub`; Step Over does not go
into one (`drawscreen` runs a whole frame); Step Out runs until the routine
returns. A breakpoint or **Stop** ends a step early, and the line at the top
says why it stopped.

The line numbers come from a compiler newer than batari Basic 1.7, which writes
`;;line N;;` into the listing. An older one is matched to the source by text,
which can go wrong when two lines read the same; rebuild with a current
`2600basic` for exact lines.

<!-- context: debugger/memory -->
## Memory

The 8K the 6507 sees, 16 bytes to a row, each row labelled with what answers
there (TIA, RAM, RIOT, a cartridge bank). **Go To** takes an address, a label, an
expression or a range (`$80-$9F`) and highlights what it found. **Poke** writes
bytes. The search bar finds hex bytes (`A9 00`) or text (not case-sensitive)
with **Find Next** and **Find Previous**, wrapping once; the machine has to be
stopped. Reading memory never touches a bank-switch hotspot or the RIOT timer.

<!-- context: debugger/breakpoints -->
## Breakpoints

Every breakpoint and trap. **Add** one with a type, an address or range and, if
you like, a condition:

- **Execute** stops before an instruction runs, in one bank or any.
- **Read**, **Write** and **Access** (both) stop after the instruction that
  touched memory, and the line at the top says what it did, for instance
  `wrote $0009 = $41 (was $40)`.

A condition can use `old` and `value`: `value != old` stops only when a write
changes something. The box beside each entry turns it off, and the list shows
how many times it fired. **Clear Hits** zeroes the counts.

<!-- context: debugger/watches -->
## Watches

Things to keep an eye on: type an address, a symbol, an expression or a range
and press <kbd>Enter</kbd>. Each shows its value while the machine is stopped
(`-` while it runs). **Show in Memory** jumps to it in the Memory tab, **Break
on Change** adds a Write breakpoint that fires when its value changes, and
**Remove** drops it.

<!-- context: debugger/cart -->
## Cart and I/O

The cartridge (its scheme, bank and the FujiNet link), the RIOT (its ports, the
timer and the console switches), and the memory map as the console sees it. The
save and load state slots, and saving the disassembly, ROM, access counters,
session or a snapshot to a file through the native picker, are here too.

<!-- context: debugger/trace -->
## Trace

Every instruction the 6507 runs, live. **Record** turns it on (it slows the
console a little, so it starts off), **Clear** empties it, **Branches only**
shows just the jumps, branches and returns, and **In range** only a range of
addresses. Trace and History share one recording.

<!-- context: debugger/history -->
## History

The same recording, read at a stop: the lead-up to a breakpoint or a pause,
newest at the bottom, with the registers, scan line and colour clock at each
instruction. Turn **Record** on before you run to the place you want to look at.

<!-- context: debugger/video -->
## Video

The TIA, with the picture on the left and the sections on the right. A game
changes the TIA's registers while the beam is drawing (that is how it gets more
than two sprites, or a colour bar), so the console records what each line was
drawn with and the views use it.

| View | Shows |
|---|---|
| Display | The last finished frame, as the screen showed it |
| Decoded | Each pixel tinted by what drew it: background, playfield, the two players, the two missiles or the ball |
| Beam | Stopped only: this frame as far as the beam has drawn it, the last one dimmed below |
| Values | The raw colour indexes |

**Zoom** goes from 1 to 16 times, or **Fit**. **Overlays** switch on the beam,
the TIA writes of the frame (they fade), the registers that changed, the object
positions and the blanked lines.

Point at a pixel to see it in **Pixel**: the object, its register, its colour
and the scan line. Click to pin it, click again to let go. Double-click it, or
press **Break on writes**, to stop the machine when anything writes the
registers that drew it.

**Registers** shows the TIA's registers and what they mean; one that changed in
the last few refreshes is highlighted. **Colours** shows the palette with the
pixels that use it. **Lines** lists the lines where the colours, playfield,
graphics or positions change. **Writes** lists the TIA writes with their colour
clock and scan line. **Timing** shows the beam.

<!-- context: debugger/sound -->
## Sound

The TIA's two audio channels: AUDC, AUDF and AUDV, and the frequency each makes.

<!-- context: debugger/symbols -->
## Symbols

Every known symbol, from a loaded `.sym` or `.lst` file or defined at the
Console. **Filter** narrows the list, and activating a row goes to that address
in the Disassembly.

<!-- context: debugger/fujinet -->
## FujiNet packets

Every FujiBus packet between the 2600 and FujiNet, both ways, with hex,
recorded from the start, even before the Debugger opens. **Clear** empties the
list.

# Files and folders

The app keeps its settings in one file, and everything else in its data
folder:

<!-- only: linux -->

On Linux, the settings are `~/.config/fujinet-go-atari2600/settings.ini` and
the data is in `~/.local/share/fujinet-go-atari2600` (or under
`$XDG_CONFIG_HOME` and `$XDG_DATA_HOME`). The GNOME and KDE apps share them.
As a Flatpak, they are under `~/.var/app/`, in the app's own folder.

<!-- end -->

<!-- only: windows -->

On Windows, the settings are `%APPDATA%\fujinet-go-atari2600\settings.ini` and
the data is in `%LOCALAPPDATA%\fujinet-go-atari2600`.

<!-- end -->

<!-- only: macos -->

On a Mac, the settings are `~/.config/fujinet-go-atari2600/settings.ini` and
the data is in `~/.local/share/fujinet-go-atari2600`.

<!-- end -->

In the data folder:

| Folder | Has |
|---|---|
| `carts` | cartridges dropped on the window |
| `fujinet` | `fnconfig.ini`, the SD folder (`SD`), and the console log |

# Troubleshooting

- **link down**: FujiNet is off, not started yet, or the external address
  is wrong. CONFIG tries again; check Preferences ▸ Host.
- **The cartridge was refused**: the message says why: too big, empty, or not
  a cartridge image.
- **Import Cartridge to SD refused a game**: the FujiNet cartridge cannot
  serve it; open it with Open Cartridge instead.
- **Keys don't reach the console**: a dialog or another window is in front,
  or the key is one the app keeps (see
  [Controllers and keys](#controllers-and-keys)).
- **A gamepad does nothing**: it may not be on the port you expect (see
  [Gamepads](#gamepads)), the controller may be of another kind, or its
  buttons may be given other jobs.
- **A game does not respond to the joystick**: set the port's type in
  Preferences ▸ Machine, or leave it on *Auto*.
- **No sound**: the audio device could not be opened, or the volume is 0.
- **A port is taken**: another program is using 11504 or 11505. Close the
  other FujiNet, or point the app at it with **Use external FujiNet**.

# About and licences

FujiNet Go Atari 2600 is free software under the GNU General Public License,
version 3 or later. Copyright © 2026 Thomas Cherryhomes.

- **Stella** (GPL-2.0-or-later) is the emulator, compiled as a library with
  its debugger engine kept.
- **The FujiNet firmware** (GPL-3.0-or-later) runs inside the app, and the
  cartridge's own sources are the firmware's.
- No copyrighted ROM is part of the app.

[fujinet.online](https://fujinet.online/) ·
[Report an issue](https://github.com/FujiNetWIFI/fujinet-go-atari2600-desktop/issues)
