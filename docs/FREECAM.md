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
- **Input**: the SDK's `XamInputGetState` passes every controller state
  (keyboard through the MnK bindings) through `NfsmwInputFilter`, exported
  by the game's executable (patch in tools/parche_ff.py); it keeps the
  latest state for the camera and gives the game a neutral one while the
  free camera is on.
