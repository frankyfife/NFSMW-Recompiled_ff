Audio diagnostics (cut or short sound effects), work in progress

- xma/audio stats: [audio] and [xma] lines in the log (see tools/parche_ff.py).
- autonav.ps1: starts build\nfsmw.exe muted with --audio_dump_file, plays a key
  script (E=Start, S=A button, H/J=stick left/right, P=screenshot) and logs the
  QPC time of every key press to %TEMP%\claude\nav_<name>_keys.txt.
  Reaching the hideout menu from a cold start:
    autonav.ps1 -Name x -Keys "E*10,S@3000,W*8,S*2@2500,W*4,J@1500,H@1500"
- press_sfx.py <name>: sound energy after each key press over the background.
  The menu music drowns the clicks; not conclusive yet.
- audio_events.py <dump>: sound events in a dump by loudness.
