# The Qt launcher

The launcher is a Qt 6 Widgets program in `launcher-windows/`. In the portable folder
`build\` it takes the game's name, `NFS_Most_Wanted.exe` (its icon is its own, an amber
square with speed chevrons drawn by `launcher-windows/res/make_icon.py`, nothing from the
game); the game itself is `nfsmw.exe` next to it. It is the window where the game is
picked and configured, and it starts `nfsmw.exe` with every setting on the command line.

The launcher always starts the game with the native Direct3D 12 renderer
(`--native_renderer=true --gpu_backend=d3d12`, see [NATIVE_RENDERER.md](NATIVE_RENDERER.md)).
`nfsmw.exe` started directly, or through the C# fallback launcher, uses the GPU emulation
instead, because the cvar `native_renderer` defaults to false
(`app/src/native/parallel.cpp`), unless `nfsmw.toml` sets it. An Esc-menu change in a game
started from this launcher does write it there (see [Precedence](#arguments-always-passed)).
Even with the launcher, the native renderer only takes over once the emulator's swap can
open its frame (Direct3D 12, same GPU); otherwise the emulation keeps drawing.

The older C# WinForms launcher is only built when the Qt build fails; it is described in
[lanzador.md](lanzador.md).

Source: `launcher-windows/src/launcher_window.cpp` (window, options, command line),
`iso_extractor.cpp` (ISO extraction), `widgets.cpp` (cards, segmented controls, toggles,
banner), `theme.cpp`, `main.cpp`.

## Contents

- [What it does](#what-it-does)
- [The window](#the-window)
- [General tab](#general-tab)
- [Advanced tab](#advanced-tab)
- [Arguments always passed](#arguments-always-passed)
- [Files and folders](#files-and-folders)
- [Running the game](#running-the-game)
- [Building it](#building-it)

## What it does

1. **Finds the game.** It looks for `nfsmw.exe` next to itself. If it is not there, it
   walks up to five folders up and takes `build\nfsmw.exe` (portable folder) or
   `app\out\build\win-amd64-release\nfsmw.exe` (source tree). The folder it settles on,
   called the *launcher root* below, holds `launcher.ini`, `logs\`, `game_root_cache\`
   and `portada.jpg`: the folder of `nfsmw.exe` in the portable case, the repository root
   in the source-tree case. If no `nfsmw.exe` is found, PLAY is disabled and the footer
   says `nfsmw.exe was not found next to the launcher.`
2. **Finds the game data.** If the game path in `launcher.ini` is empty, it uses
   `game_root\` in the launcher root when that folder contains `default.xex`, otherwise the
   first `*.iso` there (alphabetical order).
3. **On PLAY:** saves the settings to `launcher.ini`, extracts the ISO if needed (once),
   starts `nfsmw.exe` with the arguments built from the options, and minimizes itself.
   When the game exits it comes back and reports errors from the log.

Settings are written only when PLAY is pressed. Closing the launcher with Quit does not
save changes.

## The window

- **Banner** at the top: the title, a game status chip (`No game selected`,
  `ISO found`, `Game ready` or `Game not found`) and the cover image (`portada.jpg`,
  see [Files and folders](#files-and-folders)); without the image a drawn speed-streak
  pattern is shown.
- **Tabs** `General` and `Advanced`.
- **Footer:** `Command line…` (shows the exact command line that PLAY would run, with a
  Copy button), a status line, `Quit` and `PLAY`.

Many options have a note under them that changes with the selection. The notes quoted
below are the launcher's own text.

## General tab

Left column: Game data, Display, Game. Right column: Effects & detail, Frame rate,
Image quality.

### Game data

| Control | What it does |
|---|---|
| Path field | An ISO file or an extracted folder (one that contains `default.xex`). |
| `ISO…` | File dialog for an Xbox 360 disc image (`*.iso`). |
| `Folder…` | Folder dialog for the extracted game. |

The note under the field says what was found:

| Path | Note |
|---|---|
| empty | `Pick your ISO or an extracted folder.` |
| folder with `default.xex` | `Extracted folder · ready.` |
| ISO, already extracted | `ISO · already extracted, starts right away.` |
| ISO, not yet extracted | `ISO · extracted once on the first PLAY (~7 GB).` |
| anything else | `Not found, or not an ISO / folder with default.xex.` |

### Display

| Option | Choices | Default | Argument |
|---|---|---|---|
| Resolution | 480p (640 × 480), 540p (960 × 540), 720p (1280 × 720), 900p (1600 × 900), 1080p (1920 × 1080), 1440p (2560 × 1440), 1800p (3200 × 1800), 2160p (3840 × 2160), Custom | 1080p | `--resolution=1080p` (preset name) or `--resolution=<width>x<height>` |
| Custom width × height | 320-7680 × 240-4320, enabled only with Custom | 1920 × 1080 | part of `--resolution` |
| Render scale (AA) | 1×, 2×, 3×, 4× | 2× | `--native_renderer_scale=1..4` |
| Mode | Fullscreen, Windowed | Fullscreen | `--fullscreen=true/false` |
| Monitor | Automatic, then `Monitor N · W × H` for each screen (`(primary)` on the primary one) | Automatic | `--monitor=<index>` (0 = Automatic) |

- **Resolution** is the window resolution and video mode.
- **Render scale (AA)** is the native renderer's: it draws the game at that multiple of
  the Xbox 360's 1280 × 720 (supersampling). The note shows the result: at 1×
  `Xbox 360 rendering resolution, 1280 × 720.`, at 2× to 4× for example
  `Drawn at 2560 × 1440 (4× the pixels): sharper, much smoother edges (supersampling on
  top of the game's 4× MSAA).` The emulation's own scale, `--resolution_scale`, is always
  1 (see [Arguments always passed](#arguments-always-passed)).

### Game

| Option | Choices | Default | Argument |
|---|---|---|---|
| Language | Auto (from nfsmw.toml), English, German, French, Spanish, Italian | Auto | `--user_language=1/3/4/5/6`; nothing is passed for Auto |
| Black Edition cars in the car lot | on / off | on | `--black_edition=true/false` |
| Unlock everything (cars, parts, events) | on / off | off | `--unlock_all=true/false` |
| Field of view | 50 % to 160 % (slider) | 100 % | `--fov_scale=0.50..1.60` |

- **Language** values are Xbox XLanguage ids. Only languages that are on the disc work
  (the German PAL disc was verified with German, id 3).
- **Black Edition / Unlock everything:** `Both can also be switched live in the in-game
  ESC menu. Unlocking does not touch your save: switch it off and your normal progress is
  back.`
- **Field of view** is the driving camera's, as a factor of the game's own. The value
  label shows the percentage and the angle at rest (`100% · 78°`). Note: `The driving
  camera's, times the game's own (78° at rest); it still widens with speed. Live in the
  ESC menu too, as is the free camera's (F6; zoom with LB / RB).`

### Effects & detail

| Option | Choices | Default | Argument |
|---|---|---|---|
| Post-processing (colour grading, bloom, motion blur) | on / off | on | `--post_processing=true/false` |
| Cars at full detail at any distance | on / off | on | `--car_lod_highest=true/false` |
| Mipmaps | Game, Sharper, Off | Game | `--native_renderer_mipmaps=0/1/2` |

Note: `Post-processing off shows the plain picture, without the game's green-yellow
tint. Cars at full detail: the game switches a car to simpler models below 120 pixels of
720p. Mipmaps: Sharper uses one level finer textures, Off only the full-size ones
(sharpest, distant surfaces shimmer). All three also live in the ESC menu.`

### Frame rate

| Option | Choices | Default | Argument |
|---|---|---|---|
| Target | 30 · Original, 60, Unlimited, Custom | 60 | `--frame_pacing_fps=30/60/0/<custom>` |
| Custom fps | 10-240, enabled only with Custom | 60 | used as `--frame_pacing_fps` |
| V-Sync (no tearing) | on / off | on | `--vsync=true/false` |
| G-Sync / FreeSync (variable refresh) | on / off | off | `--frame_pacing_vrr=true/false` |

The game gets a guest vblank every millisecond (`--guest_vblank_rate=1000`), so it never
misses a vblank slot, and the pace is set by `frame_pacing_fps`, a clock that runs in the
game thread where it hands over a frame (VdSwap) while Pace frames in the game thread is on
(the default, see [Advanced tab](#advanced-tab)), and in the GPU thread before presenting
when it is off. Physics run on real time, so game speed is the same at any frame rate.

**G-Sync / FreeSync cap.** With G-Sync / FreeSync on, the game keeps the frame rate at
`floor(refresh - refresh² / 3600)`: 116 fps at 120 Hz. This also applies at Unlimited.
The launcher computes the same number for its notes from the current display mode
(whole Hz) of the monitor picked, or of the primary monitor with Automatic.

The note next to Custom fps depends on the combination:

| Target | G-Sync / FreeSync | Note |
|---|---|---|
| Unlimited | on | `As fast as the display shows: 116 fps at 120 Hz (G-Sync / FreeSync).` (numbers from the monitor) |
| Unlimited | off | `As fast as the PC allows: above the refresh rate it tears without V-Sync, even with G-Sync. Physics run on real time, so game speed is unaffected.` (warning) |
| above the cap | on | `Paced at 116 fps: G-Sync / FreeSync keeps it under the display's 120 Hz.` |
| any other | either | `Evenly paced at N fps. Physics run on real time, so game speed is the same at any frame rate.` |

**V-Sync** is greyed out while G-Sync / FreeSync is on: in that mode the game presents
with V-Sync anyway, and the note reads `On with G-Sync / FreeSync: it only holds back a
frame that comes a hair before the display is ready.` With V-Sync on: `Every frame waits
for the monitor's refresh: no tearing. Measured: no extra latency.` With V-Sync off
(warning): `Frames faster than the monitor's refresh tear (at 120 fps on 120 Hz: about a
quarter of them).` V-Sync has been on by default since 2026-10-01; the setting is stored
under a new key (`frame/vsync3`), so older V-Sync keys in `launcher.ini` are ignored.

**G-Sync / FreeSync** is off by default: on a display without variable refresh the cap
just under the refresh rate would repeat a frame every so often. When on, the game is
presented with V-Sync and without the display lock. Note when on: `The frame rate stays
just under the refresh rate (116 fps at 120 Hz), with V-Sync: no tearing and no V-Sync
lag. Only for displays with G-Sync or FreeSync (on others a frame repeats now and then).`

### Image quality

| Option | Choices | Default | Argument |
|---|---|---|---|
| Anisotropic | Off, 2×, 4×, 8×, 16× | 8× | `--anisotropic_override` and `--native_renderer_anisotropic`, both `0/2/3/4/5` |
| MSAA | Off, 2×, 4× · Game, 8× | 4× · Game | `--native_renderer_msaa=0/1/-1/3` |
| Output filter | Bilinear, CAS, FSR | Bilinear | `--present_effect=cas/fsr`; nothing is passed for Bilinear |
| Sharpness | 0 % to 100 %, enabled only with CAS or FSR | 50 % | `--present_cas_additional_sharpness=0.00..1.00` (always passed) |

- **Anisotropic:** for `native_renderer_anisotropic`, 0 is off and 1-5 are 1x-16x, so the
  launcher's values 2/3/4/5 are 2x/4x/8x/16x.
- **MSAA:** `MSAA: the samples of the targets the game draws with multisampling, on top of
  the render scale.` `-1` keeps the game's own 4 samples.
- **Output filter** is the filter used when the finished picture is scaled to the
  window.

## Advanced tab

The switches added while working on latency and stutter. Every switch is on by default
(the measured best), with smoothing at 6 ms and the display lock at With V-Sync;
`Reset advanced settings` puts every option on this tab back to its default.

### Latency & frame pacing

| Option | Default | Argument | Greyed out when |
|---|---|---|---|
| Pace frames in the game thread | on | `--frame_pacing_at_guest` | there is no target (Unlimited) and G-Sync / FreeSync is off |
| Low-latency mode (like NVIDIA Reflex) | on | `--frame_pacing_low_latency` | never |
| Adaptive pacing | on | `--frame_pacing_adaptive` | as above, or Pace frames in the game thread is off |
| Smoothing (ms), 0-16 | 6 | `--frame_pacing_smooth_max_ms` | as above, or Pace frames in the game thread is off |
| Lock to display: Never, With V-Sync, Always | With V-Sync | `--frame_pacing_display_lock=0/1/2` | there is no target and G-Sync / FreeSync is off, or G-Sync / FreeSync is on |
| One present per game frame | on | `--present_ui_with_guest_frames` | never |

A greyed-out option is still passed with its value; it just has no effect in that
combination.

- **Pace frames in the game thread.** The pacing is applied where the game hands over a
  frame (VdSwap), as on the console, so the game cannot queue frames ahead. Measured
  50 → 19 ms from frame to screen at 60 fps. It needs a frame rate target or
  G-Sync / FreeSync: at Unlimited nothing is paced.
- **Low-latency mode.** The game starts the next frame, and reads the controller, only
  once the previous one is drawn and on its way to the screen. Works at every frame rate.
- **Adaptive pacing.** When a scene cannot hold the target, the game is paced at what it
  holds evenly (the 90th percentile of recent frames) instead of alternating fast and
  slow frames. The launcher's note: the native renderer holds about 270 fps (210 at 4×
  render scale), so this only matters for higher targets.
- **Smoothing (ms).** Every frame is shown the same time after the game releases it, for
  even frame times. It uses the slowest recent frame, up to this limit. 0 = show as soon
  as possible.
- **Lock to display.** Aligns the frame clock with the monitor's real refresh (as the
  compositor reports it). Needed with V-Sync on a fixed refresh display. Never used with
  G-Sync / FreeSync on: there the reported refresh follows the game's own presents, and
  the lock would round 116 fps back up to 120.
- **One present per game frame.** While the game delivers frames, the overlay is only
  repainted with them. Without it the overlay repaints on every monitor refresh: up to
  180 presents for 60 frames, which knocks G-Sync / FreeSync out of range.

### Diagnostics

| Option | Default | Argument |
|---|---|---|
| Performance statistics in the log | on | `--log_guest_fps` |

Every 10 s the log gets fps, frame times, latency and pacing, and a line for every late
frame. The native renderer logs its own statistics either way.

`Reset advanced settings` restores: pacing in the game thread on, low-latency mode on,
adaptive pacing on, smoothing 6 ms, lock to display With V-Sync, one present per game
frame on, performance statistics on. Like every other change, it is saved on the next
PLAY.

## Arguments always passed

These do not depend on any option:

| Argument | Why |
|---|---|
| `--log_level=info` | |
| `--log_file=<root>\logs\launcher.log` | The game's log for this run; the launcher reads it after the game exits. |
| `--game_data_root=<folder>` | The extracted folder, or the ISO's extraction cache. The SDK only accepts a folder. |
| `--gpu_plugin=xenos` | The GPU plugin (`rexgpu-xenos.dll`). |
| `--mnk_mode=true` | Keyboard as controller (see [controles.md](controles.md)). |
| `--readback_resolve=fast` | Without it the picture comes out washed out. |
| `--gpu_backend=d3d12` | The native renderer is Direct3D 12. The command line wins over `nfsmw.toml`, so an API picked in the F4 menu cannot lock it out. |
| `--native_renderer=true` | The game's frames are drawn by the native renderer; the emulated draws are skipped. |
| `--resolution_scale=1` | The emulation draws nothing, so its render targets stay at 1x; the scale goes to `--native_renderer_scale`. |
| `--guest_vblank_rate=1000` | A guest vblank every millisecond; the pace comes from `--frame_pacing_fps`. |
| `--max_fps=0` | No extra limiter in the presenter. |
| `--swap_post_effect=none` | |
| `--log_frame_breakdown=false` | |
| `--frame_times_dir=<root>\logs\frametimes` | F10 in the game records frame times there, one CSV per recording. |

Every argument is passed as `--name=value`. Measured: with `--name value` the cvars that
live in the GPU plugin DLL (`frame_pacing_fps`, `guest_vblank_rate`) were silently left at
their defaults.

**Precedence.** Command-line values win over `nfsmw.toml`. Settings changed in the
in-game Esc menu are saved to `nfsmw.toml`, but the launcher passes its own values on
every start, so the launcher's win. To keep a change made in the game to a setting the
launcher has, set it in the launcher too.

The Esc menu's save (`rex::cvar::SaveConfig`, called from `app/src/nfsmw_app.h`) rewrites
`nfsmw.toml` with every setting whose value differs from its default, wherever the value
came from. In a game started from the launcher that includes the launcher's arguments:
`native_renderer = true`, `frame_pacing_fps`, `guest_vblank_rate`, `log_file`,
`game_data_root` and the rest. `nfsmw.exe` started directly afterwards reads that file, so
it then also runs the native renderer. The next `CONSTRUIR.bat` (the `dist` target) puts
the original `app/nfsmw.toml` back.

## Files and folders

All paths are relative to the launcher root (`build\` in the portable folder).

| Path | What |
|---|---|
| `launcher.ini` | The settings, INI format, sections `[game]`, `[display]`, `[frame]`, `[image]`, `[advanced]`. Written on PLAY. |
| `logs\launcher.log` | The game's log for the last run (the launcher creates `logs\` if needed). |
| `logs\frametimes\` | F10 frame time recordings (CSV), see `tools/analyze_frametimes.py`. |
| `game_root_cache\<ISO name>\` | The extracted ISO (about 7 GB). |
| `portada.jpg` | Optional cover image for the banner. |

**ISO extraction.** The SDK only accepts a folder as game data, so an ISO is extracted
once, on the first PLAY, into `game_root_cache\<name>`, where `<name>` is the ISO's file
name without its extension (characters not allowed in folder names become `_`). A modal
dialog shows the progress and has a Cancel button; the copy runs on its own thread. The
folder gets a marker file, `.origen_iso.txt`, with the ISO's full path and size; the cache
is reused while `default.xex` is there and both still match, otherwise the folder is
deleted and extracted again. If extraction fails, an `Extraction failed` message shows the
error. An extracted folder picked directly is used in place, without a copy.

**Cover image.** `portada.jpg` is read from the launcher root at start (not built into the
exe). It is the game's cover art and is not in the repository; without it the banner shows
a drawn pattern.

**Kept across builds.** When `CONSTRUIR.bat` rebuilds `build\` (the `dist` target), it
keeps `game_root\`, `game_root_cache\`, `launcher.ini`, `lanzador.json`, `portada.jpg` and
any `*.iso`, and deletes the rest before copying the new build.

### launcher.ini keys

| Key | Default | Option |
|---|---|---|
| `game/path` | empty (auto-detect) | Game data |
| `game/language` | `0` | Language (0 = Auto, else the XLanguage id) |
| `game/black_edition` | `true` | Black Edition |
| `game/unlock_all` | `false` | Unlock everything |
| `game/fov_percent` | `100` | Field of view (50-160) |
| `display/resolution` | `1080p` | Resolution (`480p` ... `2160p`, `custom`) |
| `display/width`, `display/height` | `1920`, `1080` | Custom resolution |
| `display/scale` | `2` | Render scale (1-4) |
| `display/fullscreen` | `true` | Mode |
| `display/monitor` | `0` | Monitor (0 = Automatic) |
| `frame/mode` | `60` | Target (`30`, `60`, `unlimited`, `custom`) |
| `frame/fps` | `60` | Custom fps |
| `frame/vsync3` | `true` | V-Sync |
| `frame/vrr` | `false` | G-Sync / FreeSync |
| `image/anisotropic` | `4` | Anisotropic (0, 2, 3, 4, 5 = Off, 2×, 4×, 8×, 16×) |
| `image/msaa` | `-1` | MSAA (0, 1, -1, 3 = Off, 2×, 4× · Game, 8×) |
| `image/filter` | `bilinear` | Output filter (`bilinear`, `cas`, `fsr`) |
| `image/sharpness` | `50` | Sharpness (0-100) |
| `image/post_processing` | `true` | Post-processing |
| `image/car_lod_highest` | `true` | Cars at full detail |
| `image/mipmaps` | `0` | Mipmaps (0 Game, 1 Sharper, 2 Off) |
| `advanced/pacing_at_guest` | `true` | Pace frames in the game thread |
| `advanced/low_latency` | `true` | Low-latency mode |
| `advanced/adaptive_pacing` | `true` | Adaptive pacing |
| `advanced/smooth_ms` | `6` | Smoothing (ms) |
| `advanced/display_lock` | `1` | Lock to display (0 Never, 1 With V-Sync, 2 Always) |
| `advanced/present_per_frame` | `true` | One present per game frame |
| `advanced/log_stats` | `true` | Performance statistics in the log |

## Running the game

- `nfsmw.exe` is started with its own folder as the working directory and at HIGH
  priority (`HIGH_PRIORITY_CLASS`, never realtime), which helps the audio and GPU threads
  when the CPU is busy.
- Once the process has started, the launcher minimizes itself. PLAY reads `RUNNING` and
  the footer says `Running · ESC in game for settings, F3 for the fps counter.`
- When the game exits, the launcher is restored and brought to the front, then checks
  the log:
  - exit code not 0, or a crash: `Game exited with an error`, with the exit code in hex,
    up to the last eight log lines containing `[critical]`, `FATAL`, `unregistered` or
    `Unhandled guest`, and the log's path;
  - a line `draw resolution scale is not supported`: `Internal scale reduced`, with that
    line.
- If `nfsmw.exe` cannot be started, an error dialog shows the reason and PLAY is enabled
  again.

In the game: Esc opens the settings menu, F3 the debug overlay, F6 the free camera, F8
photo mode, F10 frame time recording. See [controles.md](controles.md) and
[FREECAM.md](FREECAM.md).

## Building it

```bat
launcher-windows\build.bat
```

`/silencioso` skips the pause at the end (`CONSTRUIR.bat` uses it).

**Requirements.** Qt 6 (6.5 or newer, per `CMakeLists.txt`) for MSVC 2022 x64. It is
looked up in `%QT_DIR%` (for example `D:\Qt\6.8.3\msvc2022_64`), then in
`D:\Qt\6.*\msvc2022_64` and `C:\Qt\6.*\msvc2022_64`. To install it:

```bat
pip install aqtinstall
python -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 --outputdir D:\Qt --archives qtbase
```

The Visual Studio environment comes from `tools\_entorno_vs.bat`. It is built with CMake,
Ninja and `cl` (Qt's MSVC binaries need the MSVC compiler, not clang), C++20.

**What the script does.**

1. Configures and builds into `launcher-windows\out\`, which gives
   `launcher-windows\out\NFS_Most_Wanted.exe`.
2. If `build\` has no `nfsmw.exe` but has `NFS_Most_Wanted.exe` (the game, as the `dist`
   target leaves it), renames that to `nfsmw.exe`. If `build\` has no game at all, it
   stops here and the launcher stays in `launcher-windows\out\` (from there it finds the
   game in `build\` or `app\out\build\win-amd64-release\` on its own).
3. Copies the launcher to `build\NFS_Most_Wanted.exe` and runs `windeployqt` to add the
   Qt DLLs and plugin folders it needs.

**In the full build.** `CONSTRUIR.bat` runs `launcher-windows\build.bat /silencioso` in
step 4 (after `build\` is assembled). If it fails, for example because Qt is not
installed, it builds the C# launcher with `CONSTRUIR_LANZADOR.bat` instead
([lanzador.md](lanzador.md)). That one does not pass `--native_renderer` (the game uses
the GPU emulation unless `nfsmw.toml` turns the native renderer on) and has only the basic
options: resolution, render scale of the emulation (`--resolution_scale`), fullscreen or
window, monitor, V-Sync, an fps limit (`--max_fps`), post-process AA
(`--swap_post_effect`), anisotropic filtering, output filter and sharpness, plus its own
video engine and graphics API choices. It has none of the others above.
