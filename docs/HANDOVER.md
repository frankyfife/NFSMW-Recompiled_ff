# Übergabe: NFSMW-Recompiled_ff (Stand 2026-10-03)

Für eine neue Claude-Sitzung (anderes Konto). Zuerst diese Datei lesen, dann `tools/parche_ff.py` (Kopf-Doku), `tools/audiodiag/README.txt` und `README.md`.
For the native renderer also read the status box at the top of `docs/NATIVE_RENDERER.md`; the free camera is in `docs/FREECAM.md`.

## Projekt

- **Was:** Windows-Build von frankyfife/NFSMW-Recompiled_ff, statische Recompilation von Need for Speed Most Wanted (Xbox 360) auf dem ReXGlue SDK v0.10.0 (Xenia-basiert). Deutsche ISO.
- **Pfade:**
  - Fork: `D:\NFSMW\NFSMW-Recompiled_ff`
  - SDK: `D:\NFSMW\rexglue-sdk` (git-Clone, Stand v0.10.0, wird nur über Patch-Skripte verändert)
  - Spielordner zum Starten: `D:\NFSMW\NFSMW-Recompiled_ff\build\` mit `nfsmw.exe`, `NFS_Most_Wanted.exe` (Qt-Launcher), DLLs, `nfsmw.toml` und `logs\`
- **Git:**
  - Arbeits-Branch `windows-qt-launcher-and-fixes`. Es wird auf den Branch **und** auf `main` gepusht, das hat der Nutzer freigegeben.
  - Autor: `-c user.name=frankyfife -c user.email=16177808+frankyfife@users.noreply.github.com`
  - Die Commit-Nachricht endet mit `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
  - Commit-Texte als Datei übergeben (`git commit -F datei`), weil PowerShell bei Anführungszeichen streikt.
  - Letzter Commit: 63684bb (step 1 measured on Windows; review fixes around the native takeover, among them the occlusion query leak). `windows-qt-launcher-and-fixes` = `origin/main`.
  - Fork of madelrandel-blip/NFSMW-Recompiled, first fork commit 2026-09-29.

## Arbeitsweise und Wünsche des Nutzers

- Der Nutzer schreibt Deutsch, also auf Deutsch antworten. Log-Meldungen, ESC-Menü und `nfsmw.toml` sind **Englisch**, Code-Kommentare in den Patches Spanisch (Altbestand) oder Englisch.
- **Messen statt raten:** CapFrameX, eigener Profiler, Log-Statistiken.
- **Keine UAC-Elevation selbst auslösen.** Downloads nur nach ausdrücklicher Erlaubnis. CapFrameX ist erlaubt und liegt unter `D:\NFSMW\_tools\CapFrameX\Portable`, Aufnahmen in `...\Captures`.
- Der Nutzer testet am Fernseher (LG TV über HDMI, 5.1). Spielfenster nicht ungefragt öffnen, solange er spielt. Vorher Bescheid sagen, wenn ein Test mehrere Spielstarts braucht.
- Der Launcher (`NFS_Most_Wanted.exe`) lässt sich nur neu installieren, wenn er geschlossen ist.
- **Test runs must not use the player's saves.** The player's saves are in `Documents\nfsmw\<xuid>\454107D9\00000001\<profile>`. `tools/audiodiag/autonav.ps1` runs with `--user_data_root=%TEMP%\claude\nfsmw_testuser` (parameter `-UserData`), a copy of `Documents\nfsmw` made once at its first run (folders starting with `cache` or `_backup` left out), so key scripts that create or change a profile do not touch the player's. Other test starts should pass their own `--user_data_root` the same way; `tools/dev/menutest.ps1` does not yet (it starts with the player's saves), so give it `-Extra '--user_data_root=...'`.
- `autonav.ps1` starts `build\nfsmw.exe` directly, without `--native_renderer`; which renderer runs then depends on `build\nfsmw.toml` (the local one has `native_renderer = true` at the moment; the shipped `app/nfsmw.toml` does not set it). `-Extra '--native_renderer=false'` forces the GPU emulation. `-Record <mkv>` records the screen (ffmpeg ddagrab, NVENC) for `glitchscan.py` / `markscan.py`; `:<command>` steps type a line into the SDK console.

## Bauen und Einspielen

Die Hilfsskripte liegen in `tools/dev/`. Sie setzen `tools\_entorno_vs.bat` voraus (clang++ mit MSVC-ABI, Ninja Multi-Config).

- **SDK und Spiel normal:** `tools\dev\sdkbuild.bat` (SDK `--target install`, danach das Spiel).
- **SDK mit Debug-Symbolen** (für den Profiler, aktuell eingespielt): `tools\dev\profbuild.bat`. Baut nur `rexgpu-xenos` und `rexruntime` nach `rexglue-sdk\out\win-amd64\Release\`.
- **Einspielen:** `rexgpu-xenos.dll/.pdb` und `rexruntime.dll/.pdb` von dort nach `build\` kopieren. Nur wenn kein `nfsmw.exe` läuft.
- **After applying a new `parche_ff.py`:** rebuild the SDK **and** the game (`fpscr.h` is inline in the generated code, so an SDK-only build is not enough), then copy the new `nfsmw.exe`, `rexruntime.dll` and `rexgpu-xenos.dll` into `build\`.
- **Spiel-EXE:** `tools\dev\gamebuild.bat`, danach `app\out\build\win-amd64-release\nfsmw.exe` nach `build\` kopieren.
  - **Nicht** das Target `dist` verwenden, es löscht `build\logs`.
- **Launcher:** `launcher-windows\build.bat /silencioso`.

## Patch-System (wichtig)

- Das SDK wird nie direkt committet. Änderungen sind exakte Textersetzungen in `tools/parche_*.py`.
- `CONSTRUIR.bat` wendet sie in dieser Reihenfolge an: diagnostico, anillo, desatasco, presentador, gpu_fallback, restaurar, velocidad, backend, privilegios, **ff**.
- **`tools/parche_ff.py` wird generiert**, nicht von Hand gepflegt:
  1. SDK-Dateien im Arbeitsbaum ändern.
  2. Neue Dateien in die Liste `FICHEROS` in `tools/generar_parche_ff.py` eintragen. Ganz neue SDK-Dateien kommen nach `NUEVOS` beziehungsweise `tools/sdk_nuevos/`.
  3. `python tools/generar_parche_ff.py` ausführen.
  4. `python tools/dev/e2e_check.py` muss `clean rebuild matches working SDK: True` melden.
  5. Die Beschreibung im Kopf von `parche_ff.py` pflegen (Spanisch, mit MEDIDO-Werten), dann committen.
- **Rules for `parche_ff.py` (English):**
  - It is the tenth patch and runs last, after `parche_presentador` and the others. `generar_parche_ff.py` diffs the patched SDK against a baseline of SDK HEAD + the other nine patches, plus the new files in `tools/sdk_nuevos/` (e.g. `include/rex/graphics/frame_pacer.h`).
  - `python tools/dev/e2e_check.py` must print `clean rebuild matches working SDK: True` before a commit.
  - **Updating an SDK that has an older `parche_ff.py` applied:** first run that older version with `--revertir`, then apply the new one. Over the old one the new one stops with `[ERROR] El anclaje no aparece una sola vez` and writes none of its text replacements ("No se ha tocado nada"); only the new files from `tools/sdk_nuevos/` are already copied before that check.
  - After applying: rebuild SDK **and** game and copy `nfsmw.exe` and the two DLLs (see "Bauen und Einspielen").
- **Cvars:**
  - Cvars der GPU-DLL gehen als `--name=wert` auf die Kommandozeile oder in die `nfsmw.toml`. Toml-Werte für spät registrierte Cvars werden nachgereicht.
  - Der Launcher übergibt fast alles per Kommandozeile, und Kommandozeile schlägt toml.

## Was dieser Fork schon kann (alles committet, Stand 2026-10-03)

New since 2026-10-01 (English):

- **Native Direct3D 12 renderer** (`app/src/native`, details and status box in `docs/NATIVE_RENDERER.md`):
  - Records the game's D3D library calls on the game thread, draws on its own thread; the picture is shown through the emulator's `IssueSwap`.
  - The Qt launcher always passes `--native_renderer=true --gpu_backend=d3d12`. `nfsmw.exe` started directly (or through the C# fallback launcher) uses the GPU emulation, since the cvar `native_renderer` defaults to false (`app/src/native/parallel.cpp`) and the shipped `nfsmw.toml` does not set it.
  - It only takes over once the emulator's swap can open its frame (Direct3D 12, same GPU; `NfsmwNativeFrameShown`); otherwise the emulation keeps drawing.
  - The GPU emulation still reads the whole PM4 stream (registers, fences, `WAIT_REG_MEM`, interrupts, swaps, tile replays) but skips draws, resolves, ZPD sample counting (since 2026-10-03 also the host occlusion query chain), shader loads (deferred) and its own front buffer.
  - Pipelines cached in `native_pipelines.bin` next to the game (deleted by the dist packaging). Textures with all mip levels since 2026-10-02 (before only level 0), placed in 64 MB heaps, untiled by `native_renderer_texture_threads` workers (default 6): entering an area 76 ms -> 5.9 ms.
  - Settings: `native_renderer_scale` 1-4, `native_renderer_msaa`, `native_renderer_anisotropic`, `native_renderer_mipmaps` (0 game, 1 sharper, 2 off), `post_processing`, `car_lod_highest`, `fov_scale` (0.5-1.6), `freecam_fov` (10-150, default 71.5).
  - Measured uncapped in free roam, 2160p, render scale 2: 298-326 fps driving, about 366-376 standing; renderer thread 1.8-2.3 ms per frame. GPU thread busy 25.7 % -> 22.0 % after step 1 (2026-10-03). Earlier, on 2026-10-01 (uncapped free roam, before supersampling existed; separate runs measured the same way): emulation 148 fps, native 263-271.
- **Frame pacing** (SDK patches in `tools/parche_ff.py`): `frame_pacing_fps` (launcher: 30 / 60 / Unlimited / Custom 10-240), `guest_vblank_rate=1000`, pacing in VdSwap (60 fps: 49.8 -> 19.1 ms frame to screen), low-latency mode (6.0 -> 3.1 ms uncapped), adaptive pacing, smoothing, display lock, one present per game frame. V-Sync on by default since 2026-10-01. G-Sync / FreeSync mode `frame_pacing_vrr` (off by default): paces at min(target, floor(refresh - refresh^2/3600)) = 116 at 120 Hz, also at Unlimited, presented with V-Sync, no display lock; the launcher greys out V-Sync then.
- **Free camera and photo mode** (`docs/FREECAM.md`): F6 or L3 + R3 = the game's debug world camera, F8 (or Y with the free camera on) = photo mode, world paused. Fix 2026-10-02: frames without a simulation step showed the game's 71.5° instead of the zoom at 120 fps (double images; also `fov_scale` while driving).
- **Esc menu** (`app/src/nfsmw_menu.cpp`; controller: Back + Start), tabs VIDEO / GAME / SYSTEM / DEBUG. Changes are saved to `nfsmw.toml`, but the Qt launcher passes its own values on every start, so they win. "Restore defaults" resets only the menu's live settings (since 2026-10-02).
- **Qt launcher** (`launcher-windows/`, built by `launcher-windows\build.bat`; `CONSTRUIR.bat` falls back to the C# launcher `tools/lanzador` only when `launcher-windows\build.bat` fails, e.g. no Qt found): `launcher.ini` next to it, the game's log of a launcher start in `logs\launcher.log` (passed as `--log_file`), an ISO is extracted once (~7 GB) into `build\game_root_cache\<name>`, the game runs at HIGH priority and the launcher is minimized meanwhile. Tabs General and Advanced; it always passes `mnk_mode=true`, `readback_resolve=fast`, `gpu_backend=d3d12`, `native_renderer=true`, `resolution_scale=1`, `native_renderer_scale`, `guest_vblank_rate=1000`, `max_fps=0`, `swap_post_effect=none` (all as `--name=value`).
- **Keys:** Esc = settings menu, F6 = free camera, F8 = photo mode, F10 = frame time recording (CSV in `logs\frametimes`); SDK: F3 debug overlay, F4 settings overlay, the key left of 1 = console. Keyboard as controller (`mnk_mode`): see `src/input/mnk/mnk_input_driver.cpp` in the SDK.
- **Fixes vs the original:** 0xC000008F crash (fpscr), Black Edition byte 0x82A2CE06, German disc, cut/garbled sound effects after driving (XMA, 2f919cb), silent audio output when there is no audio device (e1edcc3), window larger than the screen at 225 % scaling cutting the HUD (df2f763), crash when the police car loads + `nfsmw_crash.log` (a397920), white flashes/stripes in the native renderer (099586c), occlusion query leak (63684bb).
- **More tools:** `tools/audiodiag/autonav.ps1` (scripted runs, own saves, `-Record`, console steps), `glitchscan.py`, `markscan.py`; `tools/replay`; `tools/renderprobe`; `tools/measure_frametimes.bat`; `tools/dev` (sdkbuild / gamebuild / replaybuild / e2e_check).

Older entries (German):

- **Bild und Pacing:**
  - Sonne hinter Gebäuden: ZPD-Occlusion als laufender Zähler.
  - Ein Present pro Frame: G-Sync ohne Tearing.
  - Pacing in VdSwap mit Low-Latency-Modus (ähnlich Reflex) und adaptivem Pacing.
  - Frametime-Aufnahme mit F10.
  - Texturen in D3D12-Heaps: keine Hänger mehr beim Erzeugen.
- **Grafik-Thread schneller** (alles per Profil gemessen; these speed up the GPU emulation path):
  - Hot Pages (`gpu_hot_pages`).
  - Register blockweise schreiben (`gpu_register_block_writes`).
  - Resolve-Rückkopie nur ≤ 64 KB (`readback_resolve_max_kb = 64` in der toml): spart 3,6 GB/s.
  - `clear_memory_page_state = false` (toml).
  - Ergebnis bei Ziel 120 fps in der Stadt: ~90 → 95–118 fps. Der Nutzer ist zufrieden, ein weiterer fps-Vergleich ist **nicht** gewünscht.
- **Launcher, Texte, Doku:**
  - Launcher-Tab „Advanced“ mit allen Schaltern.
  - Unlock-all-Schalter.
  - Englische Logs, Menüs und toml.
  - README mit Fork-Hinweis.
- **Werkzeuge:**
  - `tools/cpuprof/sampler.cpp`: Sampling-Profiler mit Aufrufstapeln für einen Thread, ohne Admin. Bauen mit `tools\dev\buildsampler.bat`, Aufruf: `sampler.exe nfsmw.exe "GPU Commands" 45 out.txt`.
  - `tools/analyze_frametimes.py`: CapFrameX-JSON, PresentMon-CSV, F10-CSV.

## OFFEN 1: Toneffekte nach dem Fahren verzerrt oder abgehackt (fixed in 2f919cb)

> **Fixed in 2f919cb (2026-10-01).** Cause (header of `tools/parche_ff.py`, `src/audio/xma_context.cpp`): the game reuses its 256 XMA contexts round robin and starts a sound before giving it input buffers; that pass writes nothing and invalidates the output, and the game read blocks the decoder had not written yet. The first time round they were zeros, after the pool wrapped they were the previous sound (engine) in front of the menu clicks. Now the output buffer of a new stream (first pass after the context's clear) starts silent. Measured: 20 of 20 clicks clean, 45 ms, after the wrap. Everything below is kept as history; the "Verdacht" and "Nächster Schritt" parts are obsolete. Details: `tools/audiodiag/README.txt`.

**Symptom laut Nutzer:**
- Direkt nach Spielstart klingt alles normal.
- Nach ein paar Metern Fahrt klingen Menü-Klicks im Pausenmenü (Icons wechseln) verkürzt oder abgehackt, Polizeifunk teils „distorted“.
- Gleich bei 60 und 120 fps, auch mit allen Performance-Schaltern aus. Low Latency und Adaptive sind ebenfalls ausgeschlossen.

**Reproduziert, stumm und automatisch** (Details und Tastenskripte in `tools/audiodiag/README.txt`):

```powershell
# Pausenmenü 2x, 20 s Gas, Pausenmenü 2x; Ton landet in %TEMP%\claude\audio_drv1.raw(.ts)
$pm = 'E@2500,J@1500,H@1500,J@1500,H@1500,B@3000'
tools\audiodiag\autonav.ps1 -Name drv1 -Keys ("E*10,S@3000,W*8,S*2@2500,W*4,S@2500,H@1000,S@3000,W*30,$pm,$pm,G*20,W*2,$pm,$pm,P")
python tools\audiodiag\clicks.py drv1      # Länge und Ende jedes Klicks
python tools\audiodiag\envelope.py drv1 <Tastenindex> 220
```

- Die Skripte erwarten `%TEMP%\claude\` als Arbeitsordner. `autonav.ps1` startet `build\nfsmw.exe` mit `--audio_mute=true --audio_dump_file=...`.
- Tastenbelegung im Skript: E = Start/Enter, S = A/Leertaste, B = B/Backspace, H/J = Stick links/rechts, G*n = n Sekunden Gas (Taste O), P = Screenshot.

**Messergebnisse:**
- **Vor der Fahrt:** Jeder Pausenmenü-Klick ist sauber, 40 XMA-Blöcke ≈ 53 ms, ~45 ms hörbar, natürliches Ausklingen.
- **Nach 20 s Gas:**
  - Vor fast jedem Klick spielt ein **~55 ms langes Fragment**: ~83 Hz, Spitzen exakt bei ±0,0240 abgeschnitten (übersteuert, flache Dächer), vorne links und hinten links.
  - Dann folgt harte Stille, dann der saubere Klick.
  - Ein Klick fehlte ganz.
- **XMA-Kontexte:** Das Spiel reserviert beim Start 256 Kontexte und nutzt pro Ton reihum einen frischen. Im Menü sind das ~20 Kontexte pro 10 s, beim Fahren bis zu 237. Der Pool läuft also durch, und Menü-Klicks landen danach auf Kontexten, auf denen vorher Fahrgeräusche liefen.
- **Ausgeschlossen:**
  - Unterläufe am Ausgabegerät (`[audio]`).
  - Langsame Guest-Callbacks (< 0,5 ms).
  - Wartezeiten auf den globalen Lock.
  - XMA-Decoderfehler.
  - Übersteuerung in der Ausgabestufe (Gain 1,0; Spielausgabe < 0,47).
  - Hängender Fehlerstatus 4 (tritt nie auf).
  - Voller Ausgabering, der als leer gilt (ein Block frei lassen ändert nichts).

**Verdacht:** Beim Wiederverwenden eines Kontexts bleibt Zustand vom vorherigen Ton übrig: Format, Decoder oder Ausgabe-Offsets. Der Klick wird dann zuerst falsch dekodiert ausgegeben, danach noch einmal richtig. Relevanter Code:
- `rexglue-sdk/src/audio/xma_context.cpp`: `Work`, `Decode`, `Consume`, `ClearLocked`, `ResetDecoderState`, `PrepareDecoder` (Codec-Neustart über `sample_rate = 0`) und die Verzögerung um einen Frame über `carry_frame_`.
- `src/audio/xma_decoder.cpp`: `WriteRegister` mit Kick, Lock und Clear. Kicks werden dort direkt dekodiert.

**Nächster Schritt:**
- Für Kontexte, die schon einmal benutzt wurden, die ersten Kicks nach `Clear` loggen:
  - `sample_rate` und `is_stereo` alt und neu
  - ob `PrepareDecoder` den Codec neu öffnet
  - Ein- und Ausgabe-Offsets vor und nach `Work`
  - `carry_valid_`
  - Spitzenwert des ersten dekodierten Frames
- Dann mit `drv1` testen. Hilfreich ist auch, die dekodierte Ausgabe pro Kontext in eine Datei zu schreiben, um das Fragment einem Kontext zuzuordnen.

**Diagnose, die schon im SDK-Patch steckt** (bleibt vorerst drin):
- `[audio]` meldet sich nur bei Problemen.
- `[xma]` schreibt alle 10 s eine Zeile: Kontexte mit `id:kicks/blocks/clears`.
- Cvar `audio_dump_file` speichert die Rohdaten (6 × 256 Big-Endian-Floats pro Frame) plus `.ts` mit dem QPC-Zeitstempel jedes Frames.

## Next for the native renderer (proposed)

From "Order proposed" in `docs/NATIVE_RENDERER.md` (step 1 is done and measured):

1. Diagnostics for the fences: which, how many per frame, who waits and how long.
2. Native fences and read pointer with a time-out fallback; then try `native_renderer_copy_draw_data` and `native_renderer_bound_lead` off.
3. Only then option A (direct submission, bypassing PM4).

## OFFEN 2: kleinere Punkte

- **Unlock-all** (Byte 0x82A2CE00 = 1, im Log bestätigt): Laut Nutzer sind im Tuning-Shop und Autohaus trotzdem keine Teile oder Autos frei. Ungeklärt, vermutlich bauen die Frontend-Listen aus dem Profil oder Spielstand auf.
- **Launcher:** Der aktuelle Build ist installiert.
- **`nfsmw.toml`:** Die Quelle ist `app/nfsmw.toml`. `build\nfsmw.toml` ist zurzeit (2026-10-03) keine Kopie davon, sondern eine vom ESC-Menü oder vom F4-Overlay gespeicherte Fassung (`rex::cvar::SaveConfig`, `# Auto-generated cvar configuration`, 18 Zeilen mit Werten eines Launcher-Starts wie `native_renderer = true`, `frame_pacing_fps = 120`, `log_guest_fps = true`; ohne `readback_resolve_max_kb` und `clear_memory_page_state`).
