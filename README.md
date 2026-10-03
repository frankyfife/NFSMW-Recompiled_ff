# NFSMW Recompiled — ff fork

A static native recompilation of **Need for Speed: Most Wanted (2005)**, Xbox 360,
built on the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk).

> **This is a modified version** of
> [madelrandel-blip/NFSMW-Recompiled](https://github.com/madelrandel-blip/NFSMW-Recompiled),
> changed by [frankyfife](https://github.com/frankyfife) since 29 September 2026.
> It is distributed under the same license, the GNU GPL v3.0 (see [License](#license)).
> Everything below that says *this fork* describes those changes; the rest is the
> original project's work. A dated list is in [CHANGELOG.md](CHANGELOG.md).

> Reference title ID: `454107D9`

This is **not an emulator** for the game's code. The PowerPC code inside the game's
`default.xex` is translated ahead of time into C++, then compiled into a native x86-64
binary. There is no JIT and no instruction interpreter at runtime — the game's own logic
runs as native code. What the SDK provides is everything *around* that: the Xbox 360
kernel calls, the filesystem, audio, input, and a translation of the Xenos GPU to
Direct3D 12 (that part is emulation: the game talks to the console's GPU directly).

**This fork goes further: the game's frames are drawn by a native Direct3D 12
renderer.** It takes the game's draw calls where its Direct3D library issues them and
draws them itself, at up to 4× the resolution. The GPU emulation still reads the game's
command stream for what the game waits on (fences, swaps), but no longer draws. See
[Native renderer](#native-renderer).

**You need your own copy of the game.** This repository contains no game data, no
`default.xex`, no generated C++, and no compiled binary — and it never will. See
[Legal](#legal).

---

## What this fork does better

At a glance:

- **Native Direct3D 12 renderer** with supersampling up to 4×, MSAA, anisotropic filtering
  and mipmaps; 1.8× the frame rate of the emulation path in the same run.
- **Any frame rate** (30, 60, 120, unlimited, custom) with even pacing, lower input latency,
  V-Sync on by default and a **G-Sync / FreeSync mode**.
- **Free camera and photo mode** (the game's own debug camera), zoom, field of view for the
  driving camera.
- **The game's settings menu** (Esc) in English, usable with the controller (Back + Start),
  with this fork's options, and a new **Qt launcher**.
- **Fixes** for crashes, cut sound effects, white flashes, texture hitches and more.

Every number below was measured on this fork (RTX 5090, 120 Hz VRR display, 3840 × 2160)
with the statistics the fork adds to the log. The details and the reasoning are in the
documents linked from each section and in the patch scripts' comments.

### Native renderer

The game's own Direct3D calls (draws, resolves, the register shadow its D3D library
keeps) are recorded on the game thread and drawn by a Direct3D 12 renderer of its own
on a second thread. The picture goes to the game's window through the emulator's swap.
Details, every stage and every measurement: [docs/NATIVE_RENDERER.md](docs/NATIVE_RENDERER.md).

| Free roam, no frame limit | This fork's GPU emulation path | Native renderer |
|---|---|---|
| Game frames per second, same run (2026-10-01) | 148 | **263–271** |
| Game frames per second now (2026-10-03, 2160p, render scale 2, driving) | — | **298–326** |
| Frames actually drawn | 148 | **every one** (1.8–2.3 ms each on the renderer thread, 2026-10-03) |
| At 4× render scale (5120 × 2880, measured 2026-10-01) | — | about 210 |
| Frame times at 60 fps | 16.3–17.1 ms | **16.0–17.7 ms**, every swap shows its own frame |

The emulation column is this fork's own emulation path (`--native_renderer=false`), which
already carries the fork's frame pacing and command processor work; the original project
was not measured here.

Picture and quality:

- **Supersampling:** *Render scale* draws the frame at 2×, 3× or 4× 1280 × 720 (on top of
  the game's own 4× MSAA): much smoother edges, sharper textures.
- **MSAA:** the game's multisampled targets with 1, 2, 4 (the game's) or 8 samples.
- **Anisotropic filtering:** off to 16×.
- **Mipmaps:** the game's, one level sharper, or off (full-size textures only), live. Until
  2026-10-02 only the full-size level of every texture was loaded (distant roads shimmered
  and the setting did nothing); now every level, matching the emulation's picture.
- **Post-processing switch** (acts on the game itself, with either renderer): the game's
  visual treatment (the green-yellow colour grading, bloom, vignette, motion blur) can be
  switched off, live; the game then copies the plain picture with its own `screen_passthru`
  technique.
- **Cars at full detail at any distance** (also with either renderer): the game drops a car
  to simpler models once it covers less than 120 pixels of its 720p picture (about half of
  the cars on screen in free roam); now every car keeps its full model. On by default,
  live in the Esc menu.
- Sun glare, shadows, reflections, the exposure and the HUD come out as on the
  emulation; the picture was compared pixel by pixel against it.

Smoothness and correctness:

- **Textures load in parallel:** entering an area, ~300 textures took 76 ms in one frame
  (the picture stalled); now 6 ms: placed in large heaps instead of one allocation each,
  untiled by worker threads while the renderer goes on.
- **Pipelines in the background and on disk:** a shader the driver has never seen is
  compiled on a worker thread (one took 288–355 ms), and created pipelines are kept in
  `native_pipelines.bin` next to the game, so a second run does not wait for them.
- **No white flashes or stripes:** single frames with giant white triangles or white
  stripes over the HUD had three causes, each fixed and measured with screen recordings
  (`tools/audiodiag/glitchscan.py`): vertex data the game had already refilled (now copied
  when the game draws), frames missing draws whose pipeline was still being created, and
  HUD draws recorded with a vertex shader patched for another layout. A frame that would
  be incomplete is not shown (the previous one stays for a frame). Before: 1–4 such frames
  per minute of driving; after: none in four minutes of recordings and a 200 s drive.
- **No half-loaded textures:** the renderer draws a frame or two after the game; a
  texture whose memory changed is only reloaded once the new content is seen again a
  frame later (white or garbage textures flashed for a frame when the game was already
  streaming new data into memory it had freed).

What the GPU emulation still does next to it: it reads the whole command stream
(registers, fences, waits, interrupts, swaps), but no longer draws, resolves, counts
occlusion samples, loads shaders or fetches its own front buffer (GPU thread busy 25.7 →
22.0 %, 2026-10-03). It hands over only once its swap can show the native frame
(Direct3D 12 on the same GPU); otherwise the emulation keeps drawing. `nfsmw.exe` started
on its own, without the Qt launcher, uses the GPU emulation unless its `nfsmw.toml` turns
the native renderer on (*Save settings* in the Esc menu writes the running values there).

Limits: the native renderer does not draw points, lines or rectangle lists (the D3D
library's own rectangle-list clears and resolves are handled as resolves), and a frame whose HUD vertex shader was patched for another layout is
held back instead of shown wrong (about one per second while driving with sparks and skid
marks, not visible at 120 fps).

### Smooth frame rates, low latency

| | Before | This fork |
|---|---|---|
| Frame rate | the original: the console's 30 fps with V-Sync on; with V-Sync off (its launcher's default) unpaced, 18–38 fps in its own README, with an optional fps limit | 30, 60, uncapped or any custom value, evenly paced; game speed stays correct because the simulation runs on real time |
| Frame pacing at 60 fps | flips every 16 / 33 / 50 ms (vblank slots) | **16.3–17.1 ms**, a precise clock instead of vblank slots |
| G-Sync / FreeSync, presents | the UI repainted on every monitor refresh, **180 presents for 60 frames**, tearing despite VRR | **one present per game frame**, VRR stays in range |
| V-Sync | locked to the display's *nominal* rate, a hitch every 8.3 s on a 120.24 Hz panel | frame clock locked to the compositor's real refresh |
| Tearing at 120 fps on a 120 Hz VRR display | V-Sync off: **23 % of the presents** came sooner than a refresh, each one a tear | V-Sync on by default: every frame shown 8.2–8.5 ms after the previous, none twice, same latency (11.8 ms frame start → present) |
| Unlimited on a 120 Hz VRR display | 195–530 fps, tearing (V-Sync off) or a refresh of extra latency (V-Sync on) | **G-Sync / FreeSync mode**: paced at 116 fps (refresh − refresh² / 3600), presented with V-Sync |

"Before" is the original where it says so, otherwise this fork's earlier state on the way
there.

| From the game handing over a frame to the screen | Before | This fork |
|---|---|---|
| 60 fps | **49.8 ms** with the pacer in the GPU thread (the game queued ~3 frames) | **19.1 ms**, paced where the game hands over the frame |
| uncapped, GPU emulation as the bottleneck (frame start → screen) | 6.0 ms without the low-latency mode | **3.1 ms** with the low-latency mode |

The game now waits for its turn where it hands over a frame, as on the console, and a
Reflex-like mode lets it start the next frame (and read the controller) only once the
previous one is out. The G-Sync / FreeSync mode is off by default: on a display with a
fixed refresh rate a cap just under it would repeat a frame now and then.

### Free camera and photo mode

**F6** (or **L3 + R3**, both stick clicks) switches the player's view to the game's own
debug world camera, which the retail game has but no button reaches. **F8** (or **Y** on
the controller while the free camera is on) is a photo mode: the world stands still (the
simulation stops: traffic, physics, sparks) while the camera keeps flying; F8 also
switches the free camera on. Both are also in the in-game settings menu. The HUD is
hidden, the car gets no input meanwhile.

| | Keyboard | Controller |
|---|---|---|
| Move | W A S D | left stick |
| Look | arrow keys | right stick |
| Up / down | E / Q | right / left trigger |
| Faster | Space, Backspace (even faster) | A, B |
| Zoom (field of view) | 1 wider, 3 narrower, K resets | LB, RB; a right stick click resets |
| On / off | F6 | L3 + R3 |
| World pause (photo mode) | F8 | Y |

At more than 60 fps the zoom used to show double images: frames without a simulation step
(the world steps every 1/60 s) ran no camera and were drawn at the game's 71.5°. Fixed
2026-10-02, and the free camera now moves on every frame. How it was found in the game's
code: [docs/FREECAM.md](docs/FREECAM.md).

**Field of view of the driving camera:** 50–160 % of the game's own (78° at rest; it still
widens with speed), in the launcher and live in the Esc menu. Menus and cutscenes keep
theirs.

### Controls

This fork added the controller column for the settings menu, F6 / L3 + R3, F8 / Y and F10;
Esc and the menu's keyboard keys are the original's, F3 / F4 / the console the SDK's.

| | Keyboard | Controller |
|---|---|---|
| Settings menu | Esc | Back + Start |
| In the menu: move, select | the mouse, or Shift + arrows and Space (the keyboard controller's D-pad and A) | D-pad or left stick, A |
| In the menu: section | Up / Down | LB / RB |
| In the menu: back, close | Esc closes the menu | B closes an open list first, then the menu; Start alone closes |
| Free camera | F6 | L3 + R3 |
| Photo mode | F8 | Y (free camera on) |
| Frame time recording (one CSV per recording into `logs\frametimes`; when started from the Qt launcher, or with `frame_times_dir` set) | F10 | — |
| SDK debug overlay, settings overlay, console | F3, F4, the key left of 1 | — |

The keyboard also works as a controller (on by default): Space = A, Backspace = B, L = X,
P = Y, 1 / 3 = LB / RB, Q / E = triggers, WASD and the arrow keys = sticks, F / K = stick
clicks, Shift + arrows = D-pad, Z or Tab = Back, X or Return = Start. The game's own
controls: [docs/controles.md](docs/controles.md).

### Settings menu in the game

The original's menu (**Esc**), now in English, usable with the controller (**Back + Start**)
and with this fork's options; the game gets no input while it is open.

| Tab | What is there |
|---|---|
| VIDEO | Fullscreen, V-Sync, G-Sync / FreeSync, frame rate (30, 60, 120, 144, 165, unlimited); window resolution and render scale (after a restart); anisotropic filtering, MSAA, mipmaps, cars at full detail, post-processing |
| GAME | Black Edition content, *Unlock everything*, free camera and its field of view, photo mode, game speed (20–200 %), field of view of the driving camera |
| SYSTEM | Resume, save settings, restore defaults, apply and restart, **quit to desktop** (so the game can be quit without a keyboard) |
| DEBUG | Read-only values (frame rate, graphics, video, system) |

*Restore defaults* puts back V-Sync, G-Sync / FreeSync, both fields of view, the free
camera, game speed, anisotropic filtering, MSAA, mipmaps, post-processing and car detail;
fullscreen, frame rate, resolution and the content options keep the launcher's values.
Changes to settings the launcher also has are overridden on its next start; game speed,
the free camera's field of view and the gamertag stay in `nfsmw.toml`.

### A launcher that is pleasant to use

New **Qt 6 launcher** (`launcher-windows/`): DPI-aware, English, dark theme. It picks an ISO
or an extracted folder (an ISO is extracted once, about 7 GB), always starts the native
renderer, keeps its settings in `launcher.ini` and has the game write each run's log to
`logs\launcher.log` (read after the game exits, to report errors).
Full description: [docs/LAUNCHER.md](docs/LAUNCHER.md).

| Card | Options (default) |
|---|---|
| Display | Resolution (1080p), render scale 1–4× (2×), fullscreen / windowed, monitor |
| Game | Language, Black Edition (on), *Unlock everything* (off), field of view 50–160 % (100 %) |
| Effects & detail | Post-processing (on), cars at full detail (on), mipmaps game / sharper / off (game) |
| Frame rate | 30 · Original / 60 / Unlimited / Custom 10–240 (60), V-Sync (on), G-Sync / FreeSync (off) |
| Image quality | Anisotropic off–16× (8×), MSAA off / 2× / 4× game / 8× (4×), output filter bilinear / CAS / FSR, sharpness (50 %) |

The **Advanced** tab has latency and frame pacing (pace in the game thread, low-latency
mode, adaptive pacing, smoothing, display lock, one present per frame) and diagnostics,
each with what it does and what was measured, plus a reset. All of them still act with
the native renderer; the pacing ones need a frame rate target (or the G-Sync / FreeSync
mode) and are greyed out where they do nothing. The old WinForms launcher is only built
as a fallback when Qt is missing; it starts the GPU emulation and has none of this
fork's options.

### It runs, from a Windows machine, start to finish

| Problem in the original | This fork |
|---|---|
| Crash before the menu with `0xC000008F` (floating-point inexact result) | Floating-point exceptions always masked (`fpscr.h`) |
| *New game* crashed with the Black Edition patch on | The Black Edition flag was written as a 32-bit value and hit the wrong byte; it is now the single byte `0x82A2CE06` |
| Build broke on a fresh Windows checkout (libmspack symlinks checked out as text, missing first codegen, Clang not on `PATH`) | `bootstrap.ps1` pins the SDK commit and fixes the symlinks, `CONSTRUIR.bat` runs the first codegen, the environment script finds LLVM |
| "Requires the PAL **Spain** disc" | Built and played with the **German** PAL disc; the language is picked in the launcher |
| Sound effects (menu clicks, police radio) cut or garbled after driving a while | The XMA decoder handed a reused context's stale buffer to the game; a new sound now starts on a silent buffer ([tools/audiodiag/README.txt](tools/audiodiag/README.txt)) |
| Crash at start without a default audio device (e.g. the TV switched off) | Silent audio output instead |
| Windowed mode at 225 % display scaling: window larger than the screen, HUD cut off | The window opens inside the usable area of the display |

### GPU emulation path

These apply when the frames are drawn by the GPU emulation (`--native_renderer=false`, or
`nfsmw.exe` started without the Qt launcher and without `native_renderer` in its
`nfsmw.toml`).

| | Original | This fork |
|---|---|---|
| Sun shining through buildings (D3D12) | every occlusion query returned "fully visible" | Xenos occlusion queries emulated as the console's running sample counter; the sun disappears behind buildings |
| Texture streaming hitches | a new texture cost **0.30–0.37 ms**; 50–60 arrive in one frame while driving (**11 ms**) | textures placed in shared heaps, **0.04–0.05 ms** each |
| Register writes on the GPU thread | every register through a virtual call (`WriteRegister` 10.4 % of the GPU thread) | written in blocks (0.6 %) |
| Resolves copied back to the CPU | also the large ones, every frame | only small ones (≤ 64 KB, such as the exposure) |

The texture cache limits can be raised with `texture_cache_memory_limit_soft` / `_hard`
(F4 or `nfsmw.toml`); the launcher no longer passes them since it starts the native
renderer.

### Readable diagnostics

- **English log.** The original's log messages were Spanish.
- Every 10 s: frames the game really presents, frame-time range, present rate, latency,
  pacing, and the native renderer's statistics (frames drawn, time per frame, draws,
  uploads, occlusion reports, new shaders and pipelines, frames held back); one line for
  every late or slow frame.
- Frame time recording with F10 (one CSV per recording in `logs\frametimes`, when started
  from the Qt launcher), and
  `tools/measure_frametimes.bat` / `tools/analyze_frametimes.py` for PresentMon and
  CapFrameX captures.
- Host crashes write a symbolized stack to `nfsmw_crash.log` next to the game.

---

## Status

| Area | State |
|---|---|
| Boot, menus, career, free roam, races | Working |
| Audio | Working, 5.1 output. A decoder deadlock that killed sound and froze the game on returning to the menu is fixed — see [docs/diario/audio-cuelgue.md](docs/diario/audio-cuelgue.md); sound effects cut after driving are fixed too |
| Graphics: native renderer (D3D12) | Working, the launcher's default. Needs the emulator on Direct3D 12 and the same GPU; see [docs/NATIVE_RENDERER.md](docs/NATIVE_RENDERER.md) |
| Graphics: GPU emulation (D3D12) | Working (`--native_renderer=false`) |
| Graphics: GPU emulation (Vulkan) | Compiles and loads, renders black on Intel. Untested elsewhere; the fork's occlusion and texture fixes are D3D12 only |
| Controller (with rumble) and keyboard | Working |
| Render scale, MSAA, anisotropic, mipmaps | Working (native renderer; launcher and Esc menu) |
| Post-processing switch, cars at full detail | Working (either renderer; launcher and Esc menu) |
| Frame pacing, V-Sync, G-Sync / FreeSync mode | Working |
| Free camera, photo mode, field of view | Working (F6 / L3 + R3, F8 / Y), with zoom |
| Settings menu and quitting with the controller | Working (Back + Start) |
| Save games | Working |
| Multiplayer | **Not working.** The privilege gate is solved; the network layer underneath is not. See [docs/diario/red-y-privilegios.md](docs/diario/red-y-privilegios.md) |

Game discs: built and played with the German PAL disc. Other regions are untested.

## What you need

- Windows 10 or 11, x64
- Visual Studio 2022 Build Tools (MSVC + Windows SDK)
- CMake 3.28+, Ninja, Clang 20+
- Python 3.10+
- Qt 6.8 (msvc2022_64), for the launcher
- A copy of the ReXGlue SDK checked out next to this repository (`tools\bootstrap.ps1`
  does it)
- Your own ISO or GOD dump of Need for Speed: Most Wanted for Xbox 360

Full setup instructions: [docs/00-entorno.md](docs/00-entorno.md) (Spanish).

## Build

```bat
tools\bootstrap.ps1          :: clones the ReXGlue SDK at the pinned commit into ..\rexglue-sdk
EXTRAER_XEX.bat              :: pulls default.xex out of your ISO into assets\
CONSTRUIR.bat                :: patches the SDK, recompiles it, builds the game and the launcher, packages build\
```

`CONSTRUIR.bat` is the whole pipeline. It applies every patch this project carries,
rebuilds the SDK (that is where the fixes live), runs the code generator, compiles the
game, builds the Qt launcher, assembles a portable `build\` folder, verifies that
folder is self-contained, and finally writes two ready-to-send folders into
`..\build release\`:

- `NFSMW Windows x64\` — playable, with the game executable inside. Zip it and send it
  to someone who owns the game; **never** publish it (see [Legal](#legal)).
- `NFSMW Windows x64 - Portable\` — everything except the game executable. This one is
  safe to publish.

The ISO is left out of both. The script refuses to finish if anything that looks like
game data ends up in the publishable folder.

Step-by-step detail, including what to do when something fails:
[docs/compilar.md](docs/compilar.md).

## Documentation

The README is in English. The original project's technical documentation and source
comments are in Spanish. This fork's documents in `docs/` and its newer code comments are
in English, and its notes in the Spanish documents are English blocks marked as such; the
header of `tools/parche_ff.py` is largely Spanish, its newer entries English.

| Document | What it covers |
|---|---|
| [CHANGELOG.md](CHANGELOG.md) | What changed, by release; this fork's changes at the top |
| [docs/NATIVE_RENDERER.md](docs/NATIVE_RENDERER.md) | The native Direct3D 12 renderer: how it works, every stage, every measurement, what the GPU emulation still does (English) |
| [docs/FREECAM.md](docs/FREECAM.md) | Free camera and photo mode: controls and the game code behind them (English) |
| [docs/LAUNCHER.md](docs/LAUNCHER.md) | The Qt launcher: every option, its default and what it passes (English) |
| [docs/controles.md](docs/controles.md) | The game's controls, keyboard and controller, with this fork's keys |
| [docs/arquitectura.md](docs/arquitectura.md) | How the pieces fit: SDK, app, patches, launcher |
| [docs/compilar.md](docs/compilar.md) | Building from a clean checkout |
| [docs/parches.md](docs/parches.md) | Every patch: what it changes, why, and how it was verified |
| [docs/lanzador.md](docs/lanzador.md) | The original WinForms launcher (now the fallback), its settings and how it is built |
| [docs/rendimiento.md](docs/rendimiento.md) | Measured findings: EDRAM paths, resolution scaling, frame pacing |
| [docs/problemas-conocidos.md](docs/problemas-conocidos.md) | What is broken and how far each one was traced |
| [docs/diario/](docs/diario/) | Long-form write-ups of the harder diagnoses |

The fork's own SDK changes are documented where they live: the header of
[tools/parche_ff.py](tools/parche_ff.py) lists every SDK change with its measurements,
and each change carries a `PARCHE LOCAL` comment in the code.

## Layout

```
NFSMW Recompiled/
├── app/                 the game application: CMake, codegen config, app subclass
│   ├── src/             main.cpp and the ReXApp subclass with the game's quirks;
│   │   │                the Esc menu (nfsmw_menu.cpp); this fork's free camera
│   │   │                (freecam.cpp), car detail (car_lod.cpp), post-processing
│   │   │                switch, crash log, render capture
│   │   └── native/      the native Direct3D 12 renderer (this fork)
│   ├── nfsmw_manifest.toml   what the code generator reads
│   ├── overrides.toml   hand-written codegen fixes, each with its reason
│   └── huecos.toml      generated gap list (774 entries), see HUECOS.bat
├── launcher-windows/    the Qt 6 launcher (this fork)
├── launcher-linux/      the original project's Linux launcher
├── tools/
│   ├── parche_*.py      the patches, applied to the SDK before building it
│   ├── parche_ff.py     this fork's SDK changes (generated by generar_parche_ff.py)
│   ├── sdk_nuevos/      new SDK files the patches add
│   ├── lanzador/        the original launcher (C#, WinForms), kept as a fallback
│   ├── dev/             build helpers and the patch end-to-end check (this fork)
│   ├── audiodiag/       scripted test runs, screen recording, glitch detection (this fork)
│   ├── cpuprof/         a sampling profiler for one thread (this fork)
│   ├── replay/          draws a captured frame with the native renderer (this fork)
│   ├── renderprobe/     the native renderer's first probes (this fork)
│   └── diagnostico/     instrumentation, not part of a normal build
├── docs/
└── CONSTRUIR.bat        the build
```

### Why the fixes are patches against the SDK

Much of what this project fixes lives outside the game application. The audio
deadlock, the frame pacing, the occlusion queries, the texture heaps, the graphics API
selector, the Xbox Live privilege gate — all of them are in ReXGlue, and they end up
compiled into `rexruntime.dll` and `rexgpu-xenos.dll`, not into the game executable. So
the build patches the SDK source, rebuilds it, and only then builds the game. This fork's
game-side features (the native renderer, free camera, the Esc menu's additions, car
detail, post-processing switch, field of view) are in `app/`, with small SDK hooks where they
meet the emulator (the swap, the input).

Every patch is a Python script that applies and reverts by exact text replacement,
block by block. They refuse to touch anything if an anchor does not match exactly once,
they are idempotent, and `--revertir` restores the original. Run any of them with
`--estado` to see what is applied. The reasoning behind that design, and the bugs that
forced it, are in [docs/parches.md](docs/parches.md).

## Contributing

Pull requests are welcome. Please read [CONTRIBUTING.md](CONTRIBUTING.md) first — the
short version is that patches carry their reasoning in the code, and a change that
fixes something should say what evidence says it is fixed.

## Legal

Static recompilation of a game you own, for your own use, sits on the same ground as
emulation: the translated code derives from a binary you bought.

What must **never** be distributed:

- `default.xex` or any other game file
- the C++ the code generator produces from it
- **the compiled game executable** — it contains the game's own code, translated

What is shared here is the *patch*: configuration, hooks, stubs, scripts and
documentation. Never the game.

The launcher's cover art is Electronic Arts' artwork and is **not** in this repository.
The launcher builds and runs without it; see [docs/lanzador.md](docs/lanzador.md) if you
want to supply your own. The Qt launcher's icon is original artwork made for this fork.

Need for Speed and Most Wanted are trademarks of Electronic Arts Inc. This project is
not affiliated with, endorsed by, or connected to Electronic Arts in any way.

## License

This project is licensed under the GNU General Public License v3.0 — see
[LICENSE](LICENSE). It is a modified version of
[madelrandel-blip/NFSMW-Recompiled](https://github.com/madelrandel-blip/NFSMW-Recompiled);
the changes are marked above and in the git history. The ReXGlue SDK it builds against
is BSD 3-Clause and is a separate work with its own terms; the SDK changes this project
carries are the patch scripts in `tools/`.

## Credits

- The original [NFSMW-Recompiled](https://github.com/madelrandel-blip/NFSMW-Recompiled)
  and its contributors — the recompilation, the audio fix, the patch system and the
  documentation this fork builds on
- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) — the runtime this is built on
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) — the static recompilation approach
- [Xenia](https://xenia.jp/) — the kernel and GPU emulation ReXGlue descends from; the
  occlusion query approach follows [Xenia Edge](https://github.com/has207/xenia-edge)
