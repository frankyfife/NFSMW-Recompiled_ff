# NFSMW Recompiled — ff fork

A static native recompilation of **Need for Speed: Most Wanted (2005)**, Xbox 360,
built on the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk).

> **This is a modified version** of
> [madelrandel-blip/NFSMW-Recompiled](https://github.com/madelrandel-blip/NFSMW-Recompiled),
> changed by [frankyfife](https://github.com/frankyfife) since 28 September 2026.
> It is distributed under the same license, the GNU GPL v3.0 (see [License](#license)).
> Everything below that says *this fork* describes those changes; the rest is the
> original project's work.

> Reference title ID: `454107D9`

This is **not an emulator** for the game's code. The PowerPC code inside the game's
`default.xex` is translated ahead of time into C++, then compiled into a native x86-64
binary. There is no JIT and no instruction interpreter at runtime — the game's own logic
runs as native code. What the SDK provides is everything *around* that: the Xbox 360
kernel calls, the filesystem, audio, input, and a translation of the Xenos GPU to
Direct3D 12 (that part is emulation: the game talks to the console's GPU directly).

**This fork goes further: the game's frames are drawn by a native Direct3D 12
renderer.** It takes the game's draw calls where its Direct3D library issues them and
draws them itself, at up to 4× the resolution; the GPU emulation no longer draws
anything. See [Native renderer](#native-renderer).

**You need your own copy of the game.** This repository contains no game data, no
`default.xex`, no generated C++, and no compiled binary — and it never will. See
[Legal](#legal).

---

## What this fork does better

Every number below was measured on this fork (RTX 5090, 120 Hz VRR display) with the
statistics the fork adds to the log. The details and the reasoning are in the patch
scripts and their comments.

### Native renderer

The game's own Direct3D calls (draws, resolves, the register shadow its D3D library
keeps) are recorded on the game thread and drawn by a Direct3D 12 renderer of its own
on a second thread; the GPU emulation only keeps running what the game waits for
(fences, swaps). The picture goes to the game's window through the emulator's swap.
Details, every stage and every measurement: [docs/NATIVE_RENDERER.md](docs/NATIVE_RENDERER.md).

| Free roam, no frame limit | GPU emulation | Native renderer |
|---|---|---|
| Game frames per second | 148 | **282–301** |
| Frames actually drawn | 148 | **every one** (2.6–2.8 ms each on the renderer thread) |
| At 4× render scale (5120 × 2880) | — | about 210 |
| Frame times at 60 fps | 16.3–17.1 ms | **16.0–17.7 ms**, every swap shows its own frame |

- **Supersampling:** the launcher's *Render scale* draws the frame at 2×, 3× or 4×
  1280 × 720 (on top of the game's own 4× MSAA): much smoother edges, sharper textures.
- **MSAA:** the game's multisampled targets with 1, 2, 4 (the game's) or 8 samples.
- **Mipmaps:** the game's, one level sharper, or off (full-size textures only), live.
- **Cars at full detail at any distance:** the game drops a car to simpler models once it
  covers less than 120 pixels of its 720p picture (about half of the cars on screen in
  free roam); now every car keeps its full model. On by default, live in the Esc menu.
- **No white flashes or stripes:** single frames with giant white triangles or white
  stripes over the HUD had three causes, each fixed and measured with screen recordings
  (`tools/audiodiag/glitchscan.py`): vertex data the game had already refilled (now copied
  when the game draws), frames missing draws whose pipeline was still being created, and
  HUD draws recorded with a vertex shader patched for another layout. Frames that would
  be incomplete are not shown (the previous one stays for a frame). Before: 1-4 such
  frames per minute of driving; after: none in three minutes.
- **No half-loaded textures:** the renderer draws a frame or two after the game; a
  texture whose memory changed is only reloaded once the new content is seen again a
  frame later (white or garbage textures flashed for a frame when the game was already
  streaming new data into memory it had freed).
- **Post-processing switch:** the game's visual treatment (the green-yellow colour
  grading, bloom, vignette, motion blur) can be switched off, live; the game then
  copies the plain picture with its own `screen_passthru` technique.
- Sun glare, shadows, reflections, the exposure and the HUD come out as on the
  emulation; the picture was compared pixel by pixel against it.
- New pipelines are compiled in the background, so a shader the driver has never seen
  does not freeze the picture (one took 288–355 ms).

### Free camera and photo mode

**F6** switches the player's view to the game's own debug world camera, which the
retail game has but no button reaches. **F8** is a photo mode: the world stands still
(the simulation stops: traffic, physics, sparks) while the camera keeps flying. Both are also in the
in-game settings menu (Esc). The HUD is hidden, the car gets no input meanwhile.

| | Keyboard | Controller |
|---|---|---|
| Move | W A S D | left stick |
| Look | arrow keys | right stick |
| Up / down | E / Q | right / left trigger |
| Faster | Space, Backspace (even faster) | A, B |
| Zoom (field of view) | 1 wider, 3 narrower, K resets | LB, RB, right stick click |

How it was found in the game's code: [docs/FREECAM.md](docs/FREECAM.md).

### Field of view, settings menu on the controller

- **Field of view of the driving camera:** 50–160 % of the game's own (78° at rest; it
  still widens with speed), in the launcher and live in the Esc menu. Menus and
  cutscenes keep theirs.
- **Settings menu with the controller:** **Back + Start** opens the in-game settings
  menu (and closes it). D-pad or left stick move, A selects, LB / RB switch section,
  B closes. *System → Quit to desktop* ends the game, so it can be quit without a
  keyboard. The game gets no input while the menu is open.

### It runs, from a Windows machine, start to finish

| Problem in the original | This fork |
|---|---|
| Crash before the menu with `0xC000008F` (floating-point inexact result) | Floating-point exceptions always masked (`fpscr.h`) |
| *New game* crashed with the Black Edition patch on | The Black Edition flag was written as a 32-bit value and hit the wrong byte; it is now the single byte `0x82A2CE06` |
| Build broke on a fresh Windows checkout (libmspack symlinks checked out as text, missing first codegen, Clang not on `PATH`) | `bootstrap.ps1` pins the SDK commit and fixes the symlinks, `CONSTRUIR.bat` runs the first codegen, the environment script finds LLVM |
| "Requires the PAL **Spain** disc" | Built and played with the **German** PAL disc; the language is picked in the launcher |
| Sound effects (menu clicks, police radio) cut or garbled after driving a while | The XMA decoder handed a reused context's stale buffer to the game; a new sound now starts on a silent buffer ([tools/audiodiag/README.txt](tools/audiodiag/README.txt)) |
| Windowed mode at 225 % display scaling: window larger than the screen, HUD cut off | The window opens inside the usable area of the display |

### Smooth 60 fps (and more)

| | Original | This fork |
|---|---|---|
| Frame rate | 30 fps, the console's own cap | 30, 60, uncapped or any custom value; game speed stays correct because the simulation runs on real time |
| Frame pacing at 60 fps | flips every 16 / 33 / 50 ms | **16.3–17.1 ms**, a precise clock instead of vblank slots |
| G-Sync / FreeSync | tearing despite VRR: the UI repainted on every monitor refresh, **180 presents for 60 frames** | **one present per game frame**, VRR stays in range, no tearing |
| V-Sync | locked to the display's *nominal* rate, a hitch every 8.3 s on a 120.24 Hz panel | frame clock locked to the compositor's real refresh |
| Tearing at 120 fps on a 120 Hz VRR display | V-Sync off: **23 % of the presents** came sooner than a refresh, each one a tear | V-Sync on by default: every frame shown 8.2–8.5 ms after the previous, none twice, same latency (11.8 ms frame start → present) |

### Lower input latency

Timestamps travel with every frame from the game's `VdSwap` to the screen.

| From the game handing over a frame to the screen | Original pacing | This fork |
|---|---|---|
| 60 fps | **49.8 ms** (the game queued ~3 frames) | **19.1 ms** |
| uncapped, GPU emulation as the bottleneck (frame start → screen) | 6.0 ms | **3.1 ms** with the low-latency mode |

The game now waits for its turn where it hands over a frame, as on the console, and a
Reflex-like mode lets it start the next frame (and read the controller) only once the
previous one is out.

### Correct graphics, fewer hitches (GPU emulation)

These apply when the frames are drawn by the GPU emulation (`--native_renderer=false`);
the launcher now always starts the native renderer, which has its own answers to them.

| | Original | This fork |
|---|---|---|
| Sun shining through buildings (D3D12) | every occlusion query returned "fully visible" | Xenos occlusion queries emulated as the console's running sample counter; the sun disappears behind buildings |
| Texture streaming hitches | a new texture cost **0.30–0.37 ms**; 50–60 arrive in one frame while driving (**11 ms**) | textures placed in shared heaps, **0.04–0.05 ms** each |
| Texture cache limits | 384 / 768 MB (sized for small GPUs) | configurable, 2 / 4 GB by default |

### A launcher that is pleasant to use

- New **Qt 6 launcher** (`launcher-windows/`): DPI-aware, English, dark theme; the old
  WinForms one broke with display scaling. It stays as a fallback.
- Picks an ISO or an extracted folder and extracts the ISO itself.
- Always starts the **native renderer** (Direct3D 12). Resolution, render scale
  (supersampling, the anti-aliasing), fullscreen, monitor, frame rate target, V-Sync,
  anisotropic filtering, output filter and sharpness.
- **Advanced** tab: latency and frame pacing (pace in the game thread, low-latency
  mode, adaptive pacing, smoothing, display lock, one present per frame) and
  diagnostics, each with what it does and what was measured, plus a reset.
- Black Edition content and *Unlock everything* (the game's own `UnlockAllThings`
  debug flag; experimental — what it unlocks is up to the game).

### Readable diagnostics

- **English log.** The original's log messages were Spanish.
- Every 10 s: frames the game really presents, frame-time range, present rate, latency,
  and the native renderer's statistics (frames drawn, time per frame, draws, uploads,
  occlusion reports, new shaders and pipelines); one line for every late frame.
- Host crashes write a symbolized stack to `nfsmw_crash.log` next to the game.

---

## Status

| Area | State |
|---|---|
| Boot, menus, career, free roam, races | Working |
| Audio | Working, 5.1 output. A decoder deadlock that killed sound and froze the game on returning to the menu is fixed — see [docs/diario/audio-cuelgue.md](docs/diario/audio-cuelgue.md); sound effects cut after driving are fixed too |
| Graphics: native renderer (D3D12) | Working, the launcher's default. Tested in menus and free roam; see [docs/NATIVE_RENDERER.md](docs/NATIVE_RENDERER.md) |
| Graphics: GPU emulation (D3D12) | Working (`--native_renderer=false`) |
| Graphics: GPU emulation (Vulkan) | Compiles and loads, renders black on Intel. Untested elsewhere; the fork's occlusion and texture fixes are D3D12 only |
| Controller (with rumble) and keyboard | Working |
| Render scale (supersampling) | Working, up to 4× (native renderer) |
| Free camera, photo mode | Working (F6, F8), with zoom |
| Field of view, MSAA, post-processing switch | Working (launcher and Esc menu) |
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

The README is in English; the technical documentation and the source comments are in
Spanish.

| Document | What it covers |
|---|---|
| [docs/arquitectura.md](docs/arquitectura.md) | How the pieces fit: SDK, app, patches, launcher |
| [docs/compilar.md](docs/compilar.md) | Building from a clean checkout |
| [docs/parches.md](docs/parches.md) | Every patch: what it changes, why, and how it was verified |
| [docs/lanzador.md](docs/lanzador.md) | The original launcher, its settings and how it is built |
| [docs/rendimiento.md](docs/rendimiento.md) | Measured findings: EDRAM paths, resolution scaling, frame pacing |
| [docs/problemas-conocidos.md](docs/problemas-conocidos.md) | What is broken and how far each one was traced |
| [docs/NATIVE_RENDERER.md](docs/NATIVE_RENDERER.md) | The native Direct3D 12 renderer: how it works, every stage, every measurement (English) |
| [docs/FREECAM.md](docs/FREECAM.md) | Free camera and photo mode: controls and the game code behind them (English) |
| [docs/diario/](docs/diario/) | Long-form write-ups of the harder diagnoses |

The fork's own changes are documented where they live: the header of
[tools/parche_ff.py](tools/parche_ff.py) lists every SDK change with its measurements,
and each change carries a `PARCHE LOCAL` comment in the code.

## Layout

```
NFSMW Recompiled/
├── app/                 the game application: CMake, codegen config, app subclass
│   ├── src/             main.cpp and the ReXApp subclass with the game's quirks,
│   │   │                the free camera (freecam.cpp)
│   │   └── native/      the native Direct3D 12 renderer (this fork)
│   ├── nfsmw_manifest.toml   what the code generator reads
│   ├── overrides.toml   hand-written codegen fixes, each with its reason
│   └── huecos.toml      generated gap list (774 entries), see HUECOS.bat
├── launcher-windows/    the Qt 6 launcher (this fork)
├── tools/
│   ├── parche_*.py      the patches, applied to the SDK before building it
│   ├── parche_ff.py     this fork's SDK changes
│   ├── sdk_nuevos/      new SDK files the patches add
│   ├── lanzador/        the original launcher (C#, WinForms), kept as a fallback
│   └── diagnostico/     instrumentation, not part of a normal build
├── docs/
└── CONSTRUIR.bat        the build
```

### Why the fixes are patches against the SDK

Most of what this project fixes lives outside the game application (the native
renderer and the free camera are the exceptions: they are in `app/`). The audio deadlock,
the frame pacing, the occlusion queries, the texture heaps, the graphics API selector,
the Xbox Live privilege gate — all of them are in ReXGlue, and they end up compiled
into `rexruntime.dll` and `rexgpu-xenos.dll`, not into the game executable. So the
build patches the SDK source, rebuilds it, and only then builds the game.

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
