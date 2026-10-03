# Arquitectura

Cómo encajan las piezas, y sobre todo **dónde vive cada cosa**, que es lo que más
cuesta entender al llegar al proyecto.

## This fork (2026-10-03)

The three layers below are still the same files, but each holds more than the Spanish
text says. Outdated below: "almost no fix is in this repository" and "the app is
surprisingly small" (`app/`), the launcher (`tools/lanzador/`), the two resolutions
(there are three), the EDRAM section (idle while the native renderer draws) and the
game speed bar of the launcher (no launcher has one; game speed is set in the Esc
menu).

```
   nfsmw.exe          generated game code + app/src: native renderer, free camera,
                      car LOD, post-processing switch, Esc menu, crash log,
                      render capture / census
      │  links against              ▲  functions nfsmw.exe exports (NfsmwNativeFrame,
      ▼                             │  NfsmwNativeFrameShown, NfsmwNativeSkipEmulation,
   rexruntime.dll     patched SDK   │  NfsmwInputFilter), looked up by the patched SDK
      │  LoadLibrary                │
      ▼                             │
   rexgpu-xenos.dll   patched GPU emulation; still reads the whole command stream
```

**`nfsmw.exe` now carries a lot of the fork's own work.** `app/src` has grown to about
9,700 lines:

| File | What it is |
|---|---|
| `src/native/` | The native Direct3D 12 renderer: records the game's D3D library calls on the game thread, draws them on its own thread with real render targets, and its picture is shown through the emulator's `IssueSwap`. Compiles the SDK's Xenos shader translator sources from `..\rexglue-sdk` into the executable. Cvar `native_renderer`, off by default; the Qt launcher always turns it on. It takes over only once the swap can open its frame (Direct3D 12, same GPU); otherwise the emulation keeps drawing. See [NATIVE_RENDERER.md](NATIVE_RENDERER.md) |
| `src/freecam.cpp` | Free camera (F6), photo mode (F8), `fov_scale`, `freecam_fov`. Takes the controller input through `NfsmwInputFilter`. See [FREECAM.md](FREECAM.md) |
| `src/car_lod.cpp` | `car_lod_highest`: cars at their highest level of detail at any distance |
| `src/post_processing.cpp` | `post_processing`: switch for the game's post-processing pass ("visual treatment") |
| `src/nfsmw_menu.cpp` | The in-game settings menu on Esc (tabs VIDEO / GAME / SYSTEM / DEBUG), including Black Edition and Unlock everything |
| `src/crash_log.cpp` | `nfsmw_crash.log` with the exception and a symbolized stack on host crashes |
| `src/render_capture.cpp`, `src/render_census.cpp` | Debug: one frame's D3D calls written to a file (`render_capture_frame`), and call counts (`render_census`); used with `tools/renderprobe` and `tools/replay` |
| `src/nfsmw_app.h` | Still the `ReXApp` subclass described below, now also with the F6, F8 and F10 keys (the Esc key for the menu was already there upstream) |

**The fixes live in two DLLs, not one.** The fork's SDK changes (`parche_ff.py`) go into
`rexruntime.dll` and into `rexgpu-xenos.dll` (frame pacing, the command processor, the
SDK side of the native renderer). After changing that patch both DLLs are copied, and
the game is rebuilt too, because `fpscr.h` is inline in the generated code. See
[parches.md](parches.md).

**Launchers.** The main launcher is the Qt one in `launcher-windows/` (built by
`launcher-windows\build.bat`, and by `CONSTRUIR.bat` first). The C# launcher in
`tools/lanzador/` is the fallback when Qt is not available. `launcher-linux/` holds a
Qt launcher for Linux, built by `app/CMakeLists.txt` on Linux when Qt 6.4+ is found and
used by `packaging/appimage/`. See [lanzador.md](lanzador.md).

**New tool folders.** `tools/audiodiag` (scripted runs, sound analysis),
`tools/cpuprof` (sampling profiler), `tools/replay` (replays a captured frame without
the game), `tools/renderprobe` (analysis of the game's D3D layer and of captures),
`tools/dev` (build and check helpers, see [compilar.md](compilar.md)) and
`tools/sdk_nuevos` (new SDK files that `parche_ff.py` copies), plus
`tools/generar_parche_ff.py`, `tools/measure_frametimes.bat` and
`tools/analyze_frametimes.py`.

**Three resolution settings.** Besides `--resolution` and `--resolution_scale`
(section "Dos resoluciones" below) there is `native_renderer_scale` (1-4): the native
renderer draws at that multiple of 1280x720 (supersampling). `resolution_scale` only
affects the emulation's render targets and EDRAM, which do not draw while the native
renderer runs; the Qt launcher always passes `resolution_scale=1` and puts its
"Render scale (AA)" (1x-4x, default 2x) into `native_renderer_scale`.

**EDRAM idle with the native renderer.** While the native renderer draws, the
emulation still reads the whole command stream (registers, fences, waits, interrupts,
swaps) but skips draws and resolves, so its EDRAM / render target cache is idle and the
ROV / RTV choice below does not matter. It matters only when the emulation draws:
`nfsmw.exe` started directly or through the C# launcher (`native_renderer` defaults to
false), or when the swap cannot open the native frame.

**Game speed.** `game_speed` (`parche_velocidad.py`) is set in the Esc menu (GAME tab,
20-200 %); neither launcher has a game speed control, so the launcher bar mentioned in
the section on the guest clock does not exist in this fork. The 0.1 % floor described
there still applies to the cvar.

**Settings priority.** The Esc menu saves to `nfsmw.toml`, but the Qt launcher passes
all its values as `--name=value` on every start, so they win (see the cvar section
below). It always passes `gpu_backend=d3d12`.

## Las tres capas

```
   TU ISO
     │  EXTRAER_XEX.bat
     ▼
   default.xex ──────────┐
                         │  rexglue codegen
                         ▼
                   app/generated/          131 ficheros de C++ generado
                   nfsmw_recomp.*.cpp      el código PowerPC del juego, traducido
                   nfsmw_register.cpp      tabla de direcciones → funciones
                         │
                         │  clang
                         ▼
                   ┌───────────────┐
                   │  nfsmw.exe    │  el juego. Aquí dentro está su código.
                   └───────┬───────┘
                           │  enlaza contra
                   ┌───────▼────────────────────────────┐
                   │  rexruntime.dll   (el SDK)         │  ← AQUÍ VIVEN LOS ARREGLOS
                   │  kernel, VFS, audio, input, ventana│
                   └───────┬────────────────────────────┘
                           │  carga en tiempo de ejecución (LoadLibrary)
                   ┌───────▼────────────────────────────┐
                   │  rexgpu-xenos.dll                  │
                   │  Xenos → Direct3D 12 / Vulkan      │
                   └────────────────────────────────────┘
```

## Lo que hay que entender antes de tocar nada

**Casi ningún arreglo de este proyecto está en este repositorio.**

El código del juego no se puede editar: sale del generador y se sobrescribe en cada
`codegen`. Y los fallos que hemos ido arreglando —el cuelgue del audio, el vsync que no
existía, el selector de API, la puerta de los privilegios— no son del juego: son del
SDK. Acaban compilados dentro de `rexruntime.dll`.

Por eso el build hace, en este orden:

1. Aplica los parches al **fuente del SDK**, que está en `..\rexglue-sdk`
2. Recompila el SDK
3. Genera el C++ del juego
4. Compila el juego
5. Arma la carpeta portable

Si te saltas el paso 1 y 2, compilas un juego perfecto contra un runtime sin arreglar,
y el audio se muere igual que el primer día.

## Qué hay en cada sitio

### `app/`

La aplicación. Es sorprendentemente pequeña, y eso es buena señal.

| Fichero | Qué es |
|---|---|
| `nfsmw_manifest.toml` | Lo que lee el generador: dónde está el XEX, dónde escribir, qué TOMLs incluir |
| `overrides.toml` | Correcciones al generador escritas a mano, cada una con su motivo |
| `huecos.toml` | 774 huecos de ≥8 bytes que el análisis automático no reconoció como código. Lo genera `HUECOS.bat` |
| `nfsmw.toml` | Configuración del juego que se copia a la carpeta portable |
| `src/main.cpp` | Cuatro líneas: arranca la app |
| `src/nfsmw_app.h` | La subclase de `ReXApp`. Aquí sí hay lógica propia del juego |

`nfsmw_app.h` merece una lectura. Hace dos cosas que no son obvias:

- **Busca la ISO sola**, para que abrir el ejecutable sin argumentos funcione. Prefiere
  la que se llame igual que el ejecutable, si no la primera por orden alfabético, y si
  no una carpeta `game_root\`. Esto corre **antes** de que se lea `nfsmw.toml`, así que
  poner `game_data_root` en el toml no sirve de nada: manda la línea de comandos y, si
  no hay, esto.
- **Pone ajustes obligatorios** que sin ellos el juego no se ve o no se controla:
  `gpu_plugin`, `mnk_mode` y `readback_resolve`. Solo los pone si nadie los pidió, así
  que la línea de comandos y el toml siguen mandando. Y están en dos sitios distintos a
  propósito: `readback_resolve` lo registra el plugin de GPU, que se carga después, así
  que ponerlo antes sería escribir sobre un flag que aún no existe.

### `tools/parche_*.py`

Los parches. Cada uno es un script que aplica y deshace por sustitución de texto exacta
sobre el fuente del SDK. Ver [parches.md](parches.md) para el catálogo y el porqué del
diseño.

### `tools/lanzador/`

El lanzador, en C# con WinForms. Se compila con el `csc.exe` que ya trae Windows, sin
instalar nada. Ver [lanzador.md](lanzador.md).

### `tools/diagnostico/`

Instrumentación. No entra en un build normal. Se aplica a mano cuando hace falta
investigar algo y se revierte después.

## El sistema de cvars, que es cómo se configura todo

El SDK tiene un registro de variables de configuración. Entender sus dos ejes ahorra
horas.

**Prioridad de origen**, de menos a más:

```
kDefault  <  kConfig  <  kEnvironment  <  kCommandLine  <  kRuntime
```

Un valor puesto por la línea de comandos gana al del `nfsmw.toml`. Eso no es un detalle:
es lo que hace que el lanzador sea una salida de emergencia. Si eliges una API gráfica
que en tu equipo da pantalla negra y se guarda en el toml, el lanzador puede sacarte de
ahí porque pasa `--gpu_backend` siempre.

**Ciclo de vida**, que decide cuándo se puede cambiar:

| Ciclo | Significa |
|---|---|
| `kHotReload` | Se puede cambiar en caliente y tiene efecto ya |
| `kRequiresRestart` | Se puede cambiar y se guarda, pero no se aplica hasta reiniciar |
| `kInitOnly` | **Ni siquiera se puede guardar** después del arranque |

La diferencia entre los dos últimos costó un rato. `kInitOnly` hace que el menú de F4
pinte el ajuste en rojo y deshabilitado: se ve pero no se toca. Si lo que quieres es
"se puede cambiar, pero hace falta reiniciar", es `kRequiresRestart`, que además hace
que el SDK lo apunte en su lista de cambios pendientes.

Un aviso sobre esa lista: `SetFlagFromSource` apunta ahí cualquier `kRequiresRestart`
que se toque, **sin mirar de dónde viene el valor**. Así que un `--gpu_backend` en la
línea de comandos, que ya está aplicado desde el arranque, entraba igualmente y el menú
abría diciendo "hace falta reiniciar" desde el primer segundo. Por eso
`parche_backend.py` limpia la lista una vez terminado el arranque.

## El reloj del guest

`Clock::set_guest_time_scalar()` escala el reloj del guest **entero**: el contador de
ticks, la hora del sistema, los temporizadores y las esperas. Por eso vale como mando
de velocidad del juego sin tocar nada más — el hilo que genera el parpadeo vertical
compara ticks del guest, así que se ajusta solo.

Con un cuidado: `RecomputeGuestTickScalar` hace `frac.second *= uint64_t(10.0 / escala)`
cuando la escala es ≤ 1.0. Con la escala a cero eso es `10.0/0.0` = infinito, y
convertir infinito a `uint64_t` es comportamiento indefinido. Por eso el 0% de la barra
del lanzador se queda en una milésima de la velocidad normal en vez de en cero.

## Dos resoluciones que no son la misma

Esto confunde a todo el mundo, incluido a quien escribe esto.

- `--resolution` cambia el **modo de vídeo del guest** y el tamaño de la ventana. No le
  pide al juego que dibuje más fino. Most Wanted dibuja en sus propios render targets de
  tamaño fijo y deja que el escalador estire el resultado. Subir esto agranda la imagen.
- `--resolution_scale` multiplica el tamaño de esos render targets y de la EDRAM
  emulada. **Este** es el "x2" de los emuladores. Cuesta cara y crece con el cuadrado.

Ver [rendimiento.md](rendimiento.md) para los números medidos.

## La EDRAM, que es de donde sale la mitad del rendimiento

La Xbox 360 no tiene render targets normales: tiene 10 MB de memoria embebida donde el
hardware fijo hace la mezcla y el test de profundidad. Emular eso tiene dos caminos:

| Camino | Cómo | Coste |
|---|---|---|
| ROV | Rasterizer ordered views. Exacto | Lento |
| RTV | Render targets del host. Aproximado | Rápido |

En la Iris 540 medida, pasar de ROV a RTV llevó el juego de ~10 a ~18 fps. El SDK elige
ROV en Intel por defecto, que es la decisión conservadora; `render_target_path_d3d12`
permite saltársela.
