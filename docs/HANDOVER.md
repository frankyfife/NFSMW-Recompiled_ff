# Ãœbergabe: NFSMW-Recompiled_ff (Stand 2026-10-01)

FÃ¼r eine neue Claude-Sitzung (anderes Konto). Zuerst diese Datei lesen, dann `tools/parche_ff.py` (Kopf-Doku), `tools/audiodiag/README.txt` und `README.md`.

## Projekt

- **Was:** Windows-Build von frankyfife/NFSMW-Recompiled_ff, statische Recompilation von Need for Speed Most Wanted (Xbox 360) auf dem ReXGlue SDK v0.10.0 (Xenia-basiert). Deutsche ISO.
- **Pfade:**
  - Fork: `D:\NFSMW\NFSMW-Recompiled_ff`
  - SDK: `D:\NFSMW\rexglue-sdk` (git-Clone, Stand v0.10.0, wird nur Ã¼ber Patch-Skripte verÃ¤ndert)
  - Spielordner zum Starten: `D:\NFSMW\NFSMW-Recompiled_ff\build\` mit `nfsmw.exe`, `NFS_Most_Wanted.exe` (Qt-Launcher), DLLs, `nfsmw.toml` und `logs\`
- **Git:**
  - Arbeits-Branch `windows-qt-launcher-and-fixes`. Es wird auf den Branch **und** auf `main` gepusht, das hat der Nutzer freigegeben.
  - Autor: `-c user.name=frankyfife -c user.email=16177808+frankyfife@users.noreply.github.com`
  - Die Commit-Nachricht endet mit `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
  - Commit-Texte als Datei Ã¼bergeben (`git commit -F datei`), weil PowerShell bei AnfÃ¼hrungszeichen streikt.
  - Letzter Commit: 6f560eb.

## Arbeitsweise und WÃ¼nsche des Nutzers

- Der Nutzer schreibt Deutsch, also auf Deutsch antworten. Log-Meldungen, ESC-MenÃ¼ und `nfsmw.toml` sind **Englisch**, Code-Kommentare in den Patches Spanisch (Altbestand) oder Englisch.
- **Messen statt raten:** CapFrameX, eigener Profiler, Log-Statistiken.
- **Keine UAC-Elevation selbst auslÃ¶sen.** Downloads nur nach ausdrÃ¼cklicher Erlaubnis. CapFrameX ist erlaubt und liegt unter `D:\NFSMW\_tools\CapFrameX\Portable`, Aufnahmen in `...\Captures`.
- Der Nutzer testet am Fernseher (LG TV Ã¼ber HDMI, 5.1). Spielfenster nicht ungefragt Ã¶ffnen, solange er spielt. Vorher Bescheid sagen, wenn ein Test mehrere Spielstarts braucht.
- Der Launcher (`NFS_Most_Wanted.exe`) lÃ¤sst sich nur neu installieren, wenn er geschlossen ist.

## Bauen und Einspielen

Die Hilfsskripte liegen in `tools/dev/`. Sie setzen `tools\_entorno_vs.bat` voraus (clang++ mit MSVC-ABI, Ninja Multi-Config).

- **SDK und Spiel normal:** `tools\dev\sdkbuild.bat` (SDK `--target install`, danach das Spiel).
- **SDK mit Debug-Symbolen** (fÃ¼r den Profiler, aktuell eingespielt): `tools\dev\profbuild.bat`. Baut nur `rexgpu-xenos` und `rexruntime` nach `rexglue-sdk\out\win-amd64\Release\`.
- **Einspielen:** `rexgpu-xenos.dll/.pdb` und `rexruntime.dll/.pdb` von dort nach `build\` kopieren. Nur wenn kein `nfsmw.exe` lÃ¤uft.
- **Spiel-EXE:** `tools\dev\gamebuild.bat`, danach `app\out\build\win-amd64-release\nfsmw.exe` nach `build\` kopieren.
  - **Nicht** das Target `dist` verwenden, es lÃ¶scht `build\logs`.
- **Launcher:** `launcher-windows\build.bat /silencioso`.

## Patch-System (wichtig)

- Das SDK wird nie direkt committet. Ã„nderungen sind exakte Textersetzungen in `tools/parche_*.py`.
- `CONSTRUIR.bat` wendet sie in dieser Reihenfolge an: diagnostico, anillo, desatasco, presentador, gpu_fallback, restaurar, velocidad, backend, privilegios, **ff**.
- **`tools/parche_ff.py` wird generiert**, nicht von Hand gepflegt:
  1. SDK-Dateien im Arbeitsbaum Ã¤ndern.
  2. Neue Dateien in die Liste `FICHEROS` in `tools/generar_parche_ff.py` eintragen. Ganz neue SDK-Dateien kommen nach `NUEVOS` beziehungsweise `tools/sdk_nuevos/`.
  3. `python tools/generar_parche_ff.py` ausfÃ¼hren.
  4. `python tools/dev/e2e_check.py` muss `clean rebuild matches working SDK: True` melden.
  5. Die Beschreibung im Kopf von `parche_ff.py` pflegen (Spanisch, mit MEDIDO-Werten), dann committen.
- **Cvars:**
  - Cvars der GPU-DLL gehen als `--name=wert` auf die Kommandozeile oder in die `nfsmw.toml`. Toml-Werte fÃ¼r spÃ¤t registrierte Cvars werden nachgereicht.
  - Der Launcher Ã¼bergibt fast alles per Kommandozeile, und Kommandozeile schlÃ¤gt toml.

## Was dieser Fork schon kann (alles committet)

- **Bild und Pacing:**
  - Sonne hinter GebÃ¤uden: ZPD-Occlusion als laufender ZÃ¤hler.
  - Ein Present pro Frame: G-Sync ohne Tearing.
  - Pacing in VdSwap mit Low-Latency-Modus (Ã¤hnlich Reflex) und adaptivem Pacing.
  - Frametime-Aufnahme mit F10.
  - Texturen in D3D12-Heaps: keine HÃ¤nger mehr beim Erzeugen.
- **Grafik-Thread schneller** (alles per Profil gemessen):
  - Hot Pages (`gpu_hot_pages`).
  - Register blockweise schreiben (`gpu_register_block_writes`).
  - Resolve-RÃ¼ckkopie nur â‰¤ 64 KB (`readback_resolve_max_kb = 64` in der toml): spart 3,6 GB/s.
  - `clear_memory_page_state = false` (toml).
  - Ergebnis bei Ziel 120 fps in der Stadt: ~90 â†’ 95â€“118 fps. Der Nutzer ist zufrieden, ein weiterer fps-Vergleich ist **nicht** gewÃ¼nscht.
- **Launcher, Texte, Doku:**
  - Launcher-Tab â€žAdvancedâ€œ mit allen Schaltern.
  - Unlock-all-Schalter.
  - Englische Logs, MenÃ¼s und toml.
  - README mit Fork-Hinweis.
- **Werkzeuge:**
  - `tools/cpuprof/sampler.cpp`: Sampling-Profiler mit Aufrufstapeln fÃ¼r einen Thread, ohne Admin. Bauen mit `tools\dev\buildsampler.bat`, Aufruf: `sampler.exe nfsmw.exe "GPU Commands" 45 out.txt`.
  - `tools/analyze_frametimes.py`: CapFrameX-JSON, PresentMon-CSV, F10-CSV.

## OFFEN 1: Toneffekte nach dem Fahren verzerrt oder abgehackt (aktuelle Aufgabe)

**Symptom laut Nutzer:**
- Direkt nach Spielstart klingt alles normal.
- Nach ein paar Metern Fahrt klingen MenÃ¼-Klicks im PausenmenÃ¼ (Icons wechseln) verkÃ¼rzt oder abgehackt, Polizeifunk teils â€ždistortedâ€œ.
- Gleich bei 60 und 120 fps, auch mit allen Performance-Schaltern aus. Low Latency und Adaptive sind ebenfalls ausgeschlossen.

**Reproduziert, stumm und automatisch** (Details und Tastenskripte in `tools/audiodiag/README.txt`):

```powershell
# PausenmenÃ¼ 2x, 20 s Gas, PausenmenÃ¼ 2x; Ton landet in %TEMP%\claude\audio_drv1.raw(.ts)
$pm = 'E@2500,J@1500,H@1500,J@1500,H@1500,B@3000'
tools\audiodiag\autonav.ps1 -Name drv1 -Keys ("E*10,S@3000,W*8,S*2@2500,W*4,S@2500,H@1000,S@3000,W*30,$pm,$pm,G*20,W*2,$pm,$pm,P")
python tools\audiodiag\clicks.py drv1      # LÃ¤nge und Ende jedes Klicks
python tools\audiodiag\envelope.py drv1 <Tastenindex> 220
```

- Die Skripte erwarten `%TEMP%\claude\` als Arbeitsordner. `autonav.ps1` startet `build\nfsmw.exe` mit `--audio_mute=true --audio_dump_file=...`.
- Tastenbelegung im Skript: E = Start/Enter, S = A/Leertaste, B = B/Backspace, H/J = Stick links/rechts, G*n = n Sekunden Gas (Taste O), P = Screenshot.

**Messergebnisse:**
- **Vor der Fahrt:** Jeder PausenmenÃ¼-Klick ist sauber, 40 XMA-BlÃ¶cke â‰ˆ 53 ms, ~45 ms hÃ¶rbar, natÃ¼rliches Ausklingen.
- **Nach 20 s Gas:**
  - Vor fast jedem Klick spielt ein **~55 ms langes Fragment**: ~83 Hz, Spitzen exakt bei Â±0,0240 abgeschnitten (Ã¼bersteuert, flache DÃ¤cher), vorne links und hinten links.
  - Dann folgt harte Stille, dann der saubere Klick.
  - Ein Klick fehlte ganz.
- **XMA-Kontexte:** Das Spiel reserviert beim Start 256 Kontexte und nutzt pro Ton reihum einen frischen. Im MenÃ¼ sind das ~20 Kontexte pro 10 s, beim Fahren bis zu 237. Der Pool lÃ¤uft also durch, und MenÃ¼-Klicks landen danach auf Kontexten, auf denen vorher FahrgerÃ¤usche liefen.
- **Ausgeschlossen:**
  - UnterlÃ¤ufe am AusgabegerÃ¤t (`[audio]`).
  - Langsame Guest-Callbacks (< 0,5 ms).
  - Wartezeiten auf den globalen Lock.
  - XMA-Decoderfehler.
  - Ãœbersteuerung in der Ausgabestufe (Gain 1,0; Spielausgabe < 0,47).
  - HÃ¤ngender Fehlerstatus 4 (tritt nie auf).
  - Voller Ausgabering, der als leer gilt (ein Block frei lassen Ã¤ndert nichts).

**Verdacht:** Beim Wiederverwenden eines Kontexts bleibt Zustand vom vorherigen Ton Ã¼brig: Format, Decoder oder Ausgabe-Offsets. Der Klick wird dann zuerst falsch dekodiert ausgegeben, danach noch einmal richtig. Relevanter Code:
- `rexglue-sdk/src/audio/xma_context.cpp`: `Work`, `Decode`, `Consume`, `ClearLocked`, `ResetDecoderState`, `PrepareDecoder` (Codec-Neustart Ã¼ber `sample_rate = 0`) und die VerzÃ¶gerung um einen Frame Ã¼ber `carry_frame_`.
- `src/audio/xma_decoder.cpp`: `WriteRegister` mit Kick, Lock und Clear. Kicks werden dort direkt dekodiert.

**NÃ¤chster Schritt:**
- FÃ¼r Kontexte, die schon einmal benutzt wurden, die ersten Kicks nach `Clear` loggen:
  - `sample_rate` und `is_stereo` alt und neu
  - ob `PrepareDecoder` den Codec neu Ã¶ffnet
  - Ein- und Ausgabe-Offsets vor und nach `Work`
  - `carry_valid_`
  - Spitzenwert des ersten dekodierten Frames
- Dann mit `drv1` testen. Hilfreich ist auch, die dekodierte Ausgabe pro Kontext in eine Datei zu schreiben, um das Fragment einem Kontext zuzuordnen.

**Diagnose, die schon im SDK-Patch steckt** (bleibt vorerst drin):
- `[audio]` meldet sich nur bei Problemen.
- `[xma]` schreibt alle 10 s eine Zeile: Kontexte mit `id:kicks/blocks/clears`.
- Cvar `audio_dump_file` speichert die Rohdaten (6 Ã— 256 Big-Endian-Floats pro Frame) plus `.ts` mit dem QPC-Zeitstempel jedes Frames.

## OFFEN 2: kleinere Punkte

- **Unlock-all** (Byte 0x82A2CE00 = 1, im Log bestÃ¤tigt): Laut Nutzer sind im Tuning-Shop und Autohaus trotzdem keine Teile oder Autos frei. UngeklÃ¤rt, vermutlich bauen die Frontend-Listen aus dem Profil oder Spielstand auf.
- **Launcher:** Der aktuelle Build ist installiert.
- **`nfsmw.toml`:** `build\nfsmw.toml` ist sauber, ohne Testreste. Die Quelle ist `app/nfsmw.toml`.
