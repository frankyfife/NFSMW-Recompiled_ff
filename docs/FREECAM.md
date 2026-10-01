# Free camera

F6 (or the ESC menu, "Free camera") switches the player's view to the game's
own debug world camera (`DebugWorldCameraMover`, camera action
`CDActionDebug`). The retail game has it but no button reaches it.

| | Keyboard | Controller |
|---|---|---|
| Move | W A S D | left stick |
| Look | arrow keys | right stick |
| Up / down | E (or O) / Q (or I) | right / left trigger |
| Faster | Space, Backspace (even faster) | A, B |

The car gets no input while the free camera is on. F6 again gives the
driving camera back. There is no collision: the camera flies through
buildings and under the ground.

**Photo mode (F8):** the world stands still while the camera keeps
flying: the simulation runs no steps (traffic, physics, sparks freeze). F8
switches the free camera on if it is off; the world stops after about a
second, once the game has hidden its HUD. F8 again lets the world run on;
switching the free camera off (F6) ends the photo mode too.

## How it works (app/src/freecam.cpp)

Found in the game image (dumped with `NFSMW_DUMP_IMAGE=<file>` and the
native renderer on: 13 MB from 0x82000000):

- **Camera director** `sub_82167640`, one per view (view index at +8, the
  player's view is 1): every frame it writes the key of the camera action it
  wants to +16 (16 bytes: 64-bit hash, 32-bit hash, name; built by
  `sub_82144CA0(out, name)`). When the running action (+32) has another key,
  the factory `sub_821832B0(&hash, director)` creates the wanted one,
  `sub_82167330` deletes the old one and `sub_821E4130(&key, view)` reports
  the change. The free camera does the same with the key of
  `CDActionDebug` (static, `sub_8216E550`) and leaves the director alone
  while it is on; off, the wanted action is `CDActionDrive` again.
- **Debug world camera**: position at 0x82906A20, target at 0x82A2DA90.
  Its input step `sub_821751E0` turns the debug actions (ids 53-73,
  `DEBUGACTION_MOVE_*`, `DEBUGACTION_LOOK_*`, turbo) of its action queue
  (mover +180) into fields of the mover; measured in free roam (z is up):
  +144 moves up, +152 along the view, +156 level to the right, +160/+162
  are yaw and pitch rates (int16, 65536 = a full turn per second), turbo
  flags at 0x82A2BFBC/0x82A2BFC0. No button sends these actions in the
  retail game, so with the free camera on the fields come from the
  controller (`sub_821751E0` is wrapped).
- **Camera movement**: the free camera moves the debug camera's position
  and target itself, in real time (20 units per second, Space 4x, Backspace
  16x; about 66 degrees per second); the mover's own fields stay at zero.
- **Photo mode**: the frame's fixed-step accumulator `sub_823A2320` runs
  the simulation steps that are due through `sub_823A2098(stepper, steps)`
  and returns their time as the world's time step. In photo mode no step
  runs and the time step is 0 (the accumulator keeps counting real time, so
  there is no catch-up afterwards). With a time step of 0 the world update
  `sub_823AFBF8` runs the cameras only in one game state (paused, with
  0.01 s: `sub_82168028`, `sub_82165270`, `sub_823B6660`); in photo mode
  they run like that every frame, with the real time. Found by dumping the
  game image every 2 s (`NFSMW_DUMP_IMAGE_EVERY`) while driving and in the
  pause menu: the world timer at 0x82A3967C (4000 ticks per second) stands
  still in the pause menu.
- **Input**: the SDK's `XamInputGetState` passes every controller state
  (keyboard through the MnK bindings) through `NfsmwInputFilter`, exported
  by the game's executable (patch in tools/parche_ff.py); it keeps the
  latest state for the camera and gives the game a neutral one while the
  free camera is on.
