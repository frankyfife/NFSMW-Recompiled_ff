Audio diagnostics (sound effects that sound cut short), work in progress

Tools
- [audio] and [xma] lines in the log (see tools/parche_ff.py). [xma] lists
  every XMA context used in the last 10 s as id:kicks/blocks/clears.
- --audio_dump_file=<f>: raw dump of every frame the game submits (6 x 256
  big-endian floats, channel after channel) plus <f>.ts with the QPC time of
  each frame. Written before audio_mute, so tests can run silent.
- autonav.ps1: starts build\nfsmw.exe muted with the dump, plays a key script
  and logs the QPC time of every key press (nav_<name>_keys.txt).
  Keys: E=Start(Enter) S=A(Space) B=B(Backspace) H/J=stick left/right
  W*n=wait n s, P=screenshot, <key>*<count>@<gap ms>.
  Cold start to the hideout menu:   E*10,S@3000,W*8,S*2@2500,W*4
  ... then into free roam:          S@2500,H@1000,S@3000,W*30
  ... pause menu, icons, close:     E@2500,J@1500,H@1500,B@4000
- clicks.py <name>: per icon press, how long the sound lasts and how loud it
  is when it ends. envelope.py <name> <press indices>: 5 ms envelope per
  channel. press_sfx.py / audio_events.py: first attempts, drowned by music.

Findings (2026-09-30, all muted, dump + [xma])
- Host output never runs dry, guest audio callbacks take < 0.5 ms, no XMA
  decoder errors, no clipping (game output peaks < 0.47, output gain 1.0).
- The game takes a fresh XMA context per sound, round robin over its 256.
  A pause menu icon click is 13 kicks / 40 output blocks every time, the 4th
  pause menu exactly like the 1st: 40 blocks of 16-bit stereo = 2560 samples
  = 53 ms, and the dump shows 45 ms of it above the floor, same for all 16
  clicks. The decoder does not cut these clicks.
- In the pause menu everything else is paused: the output between clicks is
  digital silence (0.0000). TVs and receivers on HDMI often mute on digital
  silence and need tens of ms to open again, which eats short sounds.
- Disproved: stale XMA error_status 4 (never happens), full output ring
  looking empty (keeping one block free changes nothing).
