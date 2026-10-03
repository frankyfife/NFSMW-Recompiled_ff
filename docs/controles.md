# Controles

## This fork (2026-10-03)

> **Fork note:** this English section lists the controls of this fork
> (branch `windows-qt-launcher-and-fixes`). The Spanish text after it is the
> upstream document. Three passages in it are outdated and are marked where they
> stand: `FASE3_RUN.bat` does not exist in this repository, the log line of a
> controller has no GUID, and the keyboard (`mnk_mode`) is no longer off by
> default.

### Keys

| Key | Action | Defined by |
|---|---|---|
| Esc | Settings menu on / off (tabs VIDEO, GAME, SYSTEM, DEBUG) | game |
| F3 | Debug overlay | SDK |
| F4 | Settings overlay (the SDK's settings, key binds included) | SDK |
| Key left of 1 | Console overlay (see the note below the table) | SDK |
| F6 | Free camera on / off | game |
| F7 | Achievements overlay | SDK |
| F8 | Photo mode on / off: the world stands still. Switching it on also switches the free camera on | game |
| F10 | Frame time recording start / stop: one CSV per recording in the folder `frame_times_dir` (the launcher sets it to `logs\frametimes`) | game |

- Console key: its bind is named `Backtick`, which is the Windows key code
  `VK_OEM_3`. On US layouts that is the key left of 1; on other layouts it is
  whichever key Windows reports as `VK_OEM_3`.
- The game's keys are registered in `app/src/nfsmw_app.h` (`OnCreateDialogs`),
  the SDK's in `src/ui/rex_app.cpp`. Each key is a setting (game:
  `bind_nfsmw_menu`, `bind_freecam`, `bind_photo_mode`,
  `bind_record_frametimes`; SDK: `bind_debug_overlay`, `bind_settings`,
  `bind_console`, `bind_achievements`) and can be rebound in the F4 overlay
  under Input > Keybinds > System.

### Settings menu (Esc) with a controller

| Controller | Action |
|---|---|
| Back + Start | Opens / closes the menu |
| D-pad or left stick | Moves between items |
| A | Selects the item |
| LB / RB | Previous / next tab (while a slider is being edited they are ImGui's slow / fast modifiers instead) |
| B | Closes an open list or ends editing a value first; with nothing open, closes the menu |
| Start (alone) | Closes the menu when it is released |

- While the menu is open, and until every button that opened or closed it is
  released, the game and the free camera get a neutral controller state.
- Keyboard: Esc opens and closes it. Up / Down arrows switch tab while no
  list is open. The keyboard controller works in it too (see
  [Keyboard as a controller](#keyboard-as-a-controller)): Shift + arrows are
  the D-pad, Space is A, Backspace is B, 1 / 3 are LB / RB.

### Free camera

The game's own debug world camera, for the player's view
(`app/src/freecam.cpp`). Switch it on with F6, with L3 + R3 (both stick clicks
together) on the controller, or in the Esc menu (GAME > Content > Free camera).
While it is on, the game gets no controller input (the car does not respond):
the controller and the keyboard drive the camera.

| Controller | Keyboard | Action |
|---|---|---|
| L3 + R3 | F6 (or F + K) | Free camera on / off. The stick clicks of that press reach neither the game nor the camera until both are released |
| Left stick | W A S D | Move along the view direction / sideways |
| Right stick | Arrows | Look |
| RT / LT | E / Q | Up / down |
| A (held) | Space | 4x faster |
| B (held) | Backspace | 16x faster (A and B together: 64x) |
| LB / RB | 1 / 3 | Zoom: LB widens, RB narrows the field of view, 30 degrees per second, within 10-150 degrees |
| R3 tap | K tap | Resets the zoom to 71.5 degrees (the game's). Counts when R3 is released without L3 having been pressed |
| Y | P | Photo mode on / off (only while the free camera is on) |
| - | F8 | Photo mode on / off; switching it on also switches the free camera on |

- The zoom is the setting `freecam_fov` (10-150, default 71.5), also in the
  Esc menu (GAME > Content > Free camera field of view). The field of view
  while driving is a different setting, `fov_scale` (0.5-1.6; launcher:
  Field of view 50-160 %; Esc menu: GAME > Game > Field of view (driving)).
- Photo mode (`freecam_photo_mode`, also in the Esc menu: GAME > Content >
  Photo mode): the world stands still once the free camera has run for 60
  frames (by then the game has hidden its HUD). Switching the free camera off
  also ends photo mode.
- The keyboard column is the keyboard controller (below): Space / Semicolon
  are both A, Backspace / Quote both B, and so on.

### Keyboard as a controller

- `mnk_mode` is **on by default** in this fork. The SDK's own default is off
  (`src/input/mnk/mnk_input_driver.cpp`), but the game switches it on at
  startup (`app/src/nfsmw_app.h`, `OnPostInitLogging`), so the keyboard also
  works when `nfsmw.exe` is started directly or through the C# fallback
  launcher. The Qt launcher passes `--mnk_mode=true` as well.
- The keyboard and a controller work at the same time.
- Default keys (SDK `src/input/mnk/mnk_input_driver.cpp`; the Spanish table
  below matches them):

| Controller | Keys |
|---|---|
| A | Space or Semicolon |
| B | Backspace or Quote |
| X | L |
| Y | P |
| LT / RT | Q or I / E or O |
| LB / RB | 1 / 3 |
| Left stick | W A S D |
| Right stick | Arrows (plus the mouse with `--mnk_mouse=true`; off by default) |
| L3 / R3 | F / K |
| D-pad | Shift + arrows |
| Back | Z or Tab |
| Start | X or Return |

`Semicolon` and `Quote` are bind names for the Windows key codes `VK_OEM_1`
and `VK_OEM_7` (`;` and `'` on US layouts). Note that the X key is Start;
the X button is L. These keys can be rebound in the F4 overlay under
Input > Keybinds > Controller.

### Logs: controller not detected

`FASE3_RUN.bat`, which the Spanish section "Si el mando no responde" below
refers to, does not exist in this repository. The Qt launcher has the game
write its log to `logs\launcher.log`, in the same folder as `launcher.ini`
(it passes `--log_file` and `--log_level=info`). A controller that SDL has
seen shows up there as a line `SDL OnControllerDeviceAdded` with its name,
joystick and controller type, vendor ID and product ID (no GUID).

## Mando

**No hay que mapear nada.** El backend por defecto es SDL, que trae mapeos
nativos para los mandos habituales. Y las fuentes de entrada se **mezclan**:
`MergeInto` hace un OR de los botones de teclado y mando, así que los dos
funcionan a la vez y activar `--mnk_mode` no desactiva el mando.

### DualShock 4 / DualSense (PlayStation)

Funciona por USB y por Bluetooth. Este SDL está compilado con el driver
`hidapi`, que es el que maneja los mandos de PlayStation.

| Botón de 360 | Botón de PlayStation |
|---|---|
| **Start** | **Options** |
| Back | Share / Create |
| A | **Cruz** |
| B | Círculo |
| X | Cuadrado |
| Y | Triángulo |
| LB / RB | L1 / R1 |
| LT / RT | L2 / R2 |
| Stick izq. / der. | Stick izq. / der. |
| Pulsar sticks | L3 / R3 |
| Cruceta | Cruceta |
| Guide | PS (hay que activarlo con `--guide_button`) |

Para conducir: acelerar **R2**, frenar **L2**, girar con el stick izquierdo.

### Si el mando no responde

~~`FASE3_RUN.bat` imprime al final una sección **MANDOS DETECTADOS**.~~
*(Fork note: `FASE3_RUN.bat` does not exist in this repository; the line
below is in `logs\launcher.log`, see
[Logs: controller not detected](#logs-controller-not-detected).)* Si SDL lo
ha visto, aparece una línea `SDL OnControllerDeviceAdded` con su nombre ~~y GUID~~.
*(Fork note: the line has no GUID; it has the joystick and controller type,
vendor ID and product ID.)*
Si no aparece nada:

1. **Steam abierto**: Steam Input secuestra los mandos de PlayStation y puede
   ocultarlos o presentarlos como un mando de Xbox. Cierra Steam o desactiva
   la compatibilidad con PlayStation en sus ajustes de mando.
2. **DS4Windows / DSX**: mismo problema, hacen de intermediario. Ciérralos.
3. **Bluetooth dormido**: pulsa el botón PS para despertarlo antes de arrancar
   el juego. SDL enumera al iniciar y también en caliente, pero es más fiable
   tenerlo despierto antes.
4. **Mapeo manual**: si SDL lo ve como joystick pero no como gamepad, hace falta
   un `gamecontrollerdb.txt` junto al ejecutable. El log avisa de que no existe
   (`SDL GameControllerDB: file does not exist`), pero es solo un aviso: los
   mapeos internos de SDL3 cubren el DS4. Solo hace falta el archivo para
   mandos raros. Se descarga del proyecto SDL_GameControllerDB y se apunta con
   `--hid_mappings_file`.

Alternativa para mandos de Xbox: `--input_backend xinput`.

## Teclado

~~**Hay que activarlo explícitamente con `--mnk_mode`.** Viene apagado de fábrica,
y por eso ninguna tecla hace nada aunque las asignaciones ya existan.~~
*(Fork note: wrong for this fork. `mnk_mode` is on by default: the game
switches it on at startup, also when `nfsmw.exe` is started directly; see
[Keyboard as a controller](#keyboard-as-a-controller).)* Los
lanzadores del proyecto ya lo pasan.

| Botón de 360 | Tecla |
|---|---|
| **Start** | **X** o **Enter** |
| Back | Z o Tab |
| A | `;` o Espacio |
| B | `'` o Retroceso |
| X | L |
| Y | P |
| Gatillo izquierdo (LT) | Q o I |
| Gatillo derecho (RT) | E o O |
| Bumper izquierdo (LB) | 1 |
| Bumper derecho (RB) | 3 |
| Stick izquierdo | W A S D |
| Pulsar stick izquierdo | F |
| Stick derecho | Flechas |
| Pulsar stick derecho | K |
| Cruceta | Shift + flechas |
| Guide | sin asignar |

Ojo con la trampa: la tecla **X es Start**, no el botón X. El botón X es la **L**.

### Para conducir

- Acelerar: **E** o **O** (gatillo derecho)
- Frenar: **Q** o **I** (gatillo izquierdo)
- Girar: **A** / **D**
- Freno de mano: `;` o Espacio (botón A)

### Cambiar las asignaciones

Cada botón es un CVar y acepta varias teclas separadas por comas:

```
--keybind_start "Return,Space"
--keybind_right_trigger "Up"
```

`--mnk_mouse` usa el ratón para el stick derecho (la cámara).
`--mnk_sensitivity` ajusta su sensibilidad.
