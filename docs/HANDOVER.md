# Übergabe: NFSMW-Recompiled_ff (Stand 2026-10-01)

Für eine neue Claude-Sitzung (anderes Konto). Zuerst diese Datei lesen, dann `tools/parche_ff.py` (Kopf-Doku), `tools/audiodiag/README.txt` und `README.md`.

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
  - Letzter Commit: 6f560eb.

## Arbeitsweise und Wünsche des Nutzers

- Der Nutzer schreibt Deutsch, also auf Deutsch antworten. Log-Meldungen, ESC-Menü und `nfsmw.toml` sind **Englisch**, Code-Kommentare in den Patches Spanisch (Altbestand) oder Englisch.
- **Messen statt raten:** CapFrameX, eigener Profiler, Log-Statistiken.
- **Keine UAC-Elevation selbst auslösen.** Downloads nur nach ausdrücklicher Erlaubnis. CapFrameX ist erlaubt und liegt unter `D:\NFSMW\_tools\CapFrameX\Portable`, Aufnahmen in `...\Captures`.
- Der Nutzer testet am Fernseher (LG TV über HDMI, 5.1). Spielfenster nicht ungefragt öffnen, solange er spielt. Vorher Bescheid sagen, wenn ein Test mehrere Spielstarts braucht.
- Der Launcher (`NFS_Most_Wanted.exe`) lässt sich nur neu installieren, wenn er geschlossen ist.

## Bauen und Einspielen

Die Hilfsskripte liegen in `tools/dev/`. Sie setzen `tools\_entorno_vs.bat` voraus (clang++ mit MSVC-ABI, Ninja Multi-Config).

- **SDK und Spiel normal:** `tools\dev\sdkbuild.bat` (SDK `--target install`, danach das Spiel).
- **SDK mit Debug-Symbolen** (für den Profiler, aktuell eingespielt): `tools\dev\profbuild.bat`. Baut nur `rexgpu-xenos` und `rexruntime` nach `rexglue-sdk\out\win-amd64\Release\`.
- **Einspielen:** `rexgpu-xenos.dll/.pdb` und `rexruntime.dll/.pdb` von dort nach `build\` kopieren. Nur wenn kein `nfsmw.exe` läuft.
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
- **Cvars:**
  - Cvars der GPU-DLL gehen als `--name=wert` auf die Kommandozeile oder in die `nfsmw.toml`. Toml-Werte für spät registrierte Cvars werden nachgereicht.
  - Der Launcher übergibt fast alles per Kommandozeile, und Kommandozeile schlägt toml.

## Was dieser Fork schon kann (alles committet)

- **Bild und Pacing:**
  - Sonne hinter Gebäuden: ZPD-Occlusion als laufender Zähler.
  - Ein Present pro Frame: G-Sync ohne Tearing.
  - Pacing in VdSwap mit Low-Latency-Modus (ähnlich Reflex) und adaptivem Pacing.
  - Frametime-Aufnahme mit F10.
  - Texturen in D3D12-Heaps: keine Hänger mehr beim Erzeugen.
- **Grafik-Thread schneller** (alles per Profil gemessen):
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

## OFFEN 1: Toneffekte nach dem Fahren verzerrt oder abgehackt (aktuelle Aufgabe)

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

## OFFEN 2: kleinere Punkte

- **Unlock-all** (Byte 0x82A2CE00 = 1, im Log bestätigt): Laut Nutzer sind im Tuning-Shop und Autohaus trotzdem keine Teile oder Autos frei. Ungeklärt, vermutlich bauen die Frontend-Listen aus dem Profil oder Spielstand auf.
- **Launcher:** Der aktuelle Build ist installiert.
- **`nfsmw.toml`:** `build\nfsmw.toml` ist sauber, ohne Testreste. Die Quelle ist `app/nfsmw.toml`.
