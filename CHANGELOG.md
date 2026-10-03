# Changelog

Formato basado en [Keep a Changelog](https://keepachangelog.com/es-ES/1.1.0/).

## [ff fork] - 2026-09-29 to 2026-10-03

Changes of the [frankyfife fork](https://github.com/frankyfife/NFSMW-Recompiled_ff) on top
of 0.0.2 (entries in English; the details and measurements are in the README,
`docs/NATIVE_RENDERER.md`, `docs/FREECAM.md` and the header of `tools/parche_ff.py`).

### Added

- **Native Direct3D 12 renderer** (`app/src/native/`, 2026-10-01): the game's draw calls
  are recorded where its Direct3D library issues them and drawn by a renderer of its own
  on a second thread; the GPU emulation no longer draws. The Qt launcher always starts it
  (`nfsmw.exe` on its own still uses the emulation). Free roam uncapped: 148 fps on this
  fork's emulation path, 263-271 with the native renderer in the same run; 298-326 now
  (2160p, render scale 2).
- Native renderer settings, in the launcher and the Esc menu: render scale 1-4×
  (supersampling), MSAA, anisotropic filtering, mipmaps (game / sharper / off).
- Game options for either renderer: post-processing switch, cars at full detail at any
  distance (car LOD).
- Native renderer: dynamic shadows, occlusion queries (sun glare behind buildings),
  pipelines created in the background, pipeline cache on disk (`native_pipelines.bin`),
  textures loaded in parallel into placed heaps (area entry 76 ms -> 5.9 ms).
- **Free camera** (F6, controller L3 + R3): the game's own debug world camera, with
  zoom (LB / RB). **Photo mode** (F8, controller Y while the free camera is on): the world
  stands still while the camera flies.
- Field of view of the driving camera, 50-160 % (`fov_scale`).
- **G-Sync / FreeSync mode** (`frame_pacing_vrr`, off by default): the frame rate stays just
  under the refresh rate (116 fps at 120 Hz, also at Unlimited), presented with V-Sync.
- New **Qt 6 launcher** (`launcher-windows/`) with General and Advanced tabs; the C#
  launcher stays as the fallback when Qt is missing.
- Even frame pacing at any target (30 / 60 / unlimited / custom) instead of the old
  `max_fps` limiter, paced at the game's `VdSwap` (60 fps: 49.8 -> 19.1 ms from frame
  handoff to the screen), low-latency mode, adaptive pacing, smoothing, lock to the
  display's real refresh, one present per game frame, frame time recording (F10).
- *Unlock everything* option (the game's `UnlockAllThings` flag, experimental).
- English log messages, statistics every 10 s, a line for every late frame, symbolized
  host crashes in `nfsmw_crash.log`.
- Tools: scripted test runs with screen recording and glitch detection
  (`tools/audiodiag/`), a sampling profiler (`tools/cpuprof`), frame replay
  (`tools/replay`), frame time measurement (`tools/measure_frametimes.bat`,
  `tools/analyze_frametimes.py`), the SDK patch generator and its end-to-end check
  (`tools/generar_parche_ff.py`, `tools/dev/e2e_check.py`).

### Changed

- The original's Esc menu is in English now (tabs VIDEO / GAME / SYSTEM / DEBUG), can be
  opened and used with the controller (Back + Start), so the game can be quit without a
  keyboard, and has this fork's options.
- V-Sync is on by default (measured: no tearing at 120 fps on a 120 Hz display, same
  latency).
- The launcher only offers what the native renderer uses (no video engine or graphics
  API choice).
- Next to the native renderer the GPU emulation no longer loads shaders, fetches its own
  front buffer or locks a mutex per draw packet (GPU thread busy 25.7 -> 22.0 %), and it
  only stops drawing once its swap can show the native frame (Direct3D 12 on the same
  GPU).
- *Restore defaults* in the Esc menu resets a fixed list (V-Sync, G-Sync / FreeSync, the
  fields of view, the free camera, game speed, anisotropic / MSAA / mipmaps,
  post-processing, car detail) instead of every setting, which switched the running game
  back to the GPU emulation. Display, frame rate and content options keep the launcher's
  values.
- In the Esc menu, B first closes an open list (or ends editing a value), then the menu;
  the left stick moves like the D-pad; Start alone closes on release.
- Launcher Advanced options are greyed out where they do nothing.

### Fixed

- Crash before the menu with `0xC000008F` (floating-point exceptions masked).
- *New game* crash with the Black Edition flag on (now the single byte `0x82A2CE06`).
- Sound effects cut or garbled after driving a while (XMA output reuse).
- Crash at start without an audio device (silent output instead).
- Window larger than the screen at high display scaling, HUD cut off.
- Crash when the police car loads (native renderer).
- Sun shining through buildings (occlusion queries, GPU emulation and native renderer).
- G-Sync tearing and V-Sync judder from presenting on every monitor refresh.
- Texture streaming hitches on the GPU emulation path (textures in shared heaps).
- One-frame white flashes and stripes, half-loaded textures (native renderer).
- Only the largest mip level of every texture was loaded (distant surfaces shimmered).
- Double images while zooming the free camera, and with a field of view other than
  100 %, at more than 60 fps.
- Memory growing all session from unread occlusion query segments while the native
  renderer draws.

### Not solved

- Multiplayer (the network layer below the solved privilege gate).
- The native renderer does not draw points, lines or rectangle lists; a frame whose HUD
  vertex shader was patched for another layout is held back (about one per second with
  sparks and skid marks).
- Fences are still written by the emulator when it parses them, not when the native
  renderer has drawn (covered by measured workarounds).
- Esc menu changes to settings the launcher also has are overridden by the launcher on
  the next start.

## [0.0.2] - 2026-09-17

### Añadido

- El lanzador acepta un `.iso` directamente: lo extrae solo la primera vez a
  `game_root_cache\` dentro de la carpeta portable y reutiliza esa copia después. Antes
  solo servía apuntar a una carpeta ya extraída (`--game_data_root` exige un directorio,
  el SDK no sabe montar `.iso`).
- Ventana del lanzador redimensionable y con scroll: la banda de portada se estrecha en
  pantallas pequeñas en vez de forzar scroll horizontal, y los ajustes se centran en
  pantallas anchas en vez de quedarse pegados a un lado con un hueco enorme.
- Ajustes del lanzador en dos columnas en vez de una lista larga.
- Tema oscuro para el lanzador.

### Cambiado

- El lanzador arranca por defecto a 1080p + escala x2 en vez de 720p + x1 en una
  instalación nueva (sin `lanzador.json` todavía) — coincide con lo que `nfsmw.toml` ya
  trae configurado de fábrica, en vez de arrancar más bajo que eso sin que nadie lo pida.
- La portada del lanzador cubre el panel entero ("cover", no "fit"): antes dejaba un
  tramo negro vacío debajo en proporciones de ventana altas.
- El juego se lanza con prioridad de proceso más alta.

### Arreglado

- Ventana del lanzador marcada DPI-aware: en monitores con escala de Windows (125%,
  150%...) salía borrosa por el bitmap-stretch de Windows; ahora nítida.
- "Banner duplicado" al agrandar la ventana del lanzador: faltaba
  `ControlStyles.ResizeRedraw` en el panel de la portada, así que al crecer el control
  solo se invalidaba la franja nueva expuesta y quedaba el recorte antiguo debajo.
- `nfsmw.toml` de la carpeta portable había perdido la sección de resolución
  (`video_mode_width`/`video_mode_height`/`resolution_scale`) al restaurar una copia de
  seguridad anterior; repuesta para que coincida con `app/nfsmw.toml` del repositorio.

## [0.0.1] - 2026-09-10

Primera versión ordenada del proyecto. Todo lo de abajo se hizo antes de que existiera
este repositorio; queda registrado aquí porque es el estado del que parte.

### Añadido

- Recompilación estática completa de NFS Most Wanted (2005, Xbox 360, `454107D9`) que
  arranca, pasa el prólogo y llega a mundo abierto.
- `parche_desatasco.py`: arregla el cuelgue del descodificador XMA que mataba el audio al
  salir del garaje y congelaba el juego al volver al menú.
- `parche_presentador.py`: vsync real y limitador de fps. Ninguno de los dos existía en
  el SDK.
- `parche_backend.py`: selector de API gráfica (D3D12 / Vulkan) desde el menú de F4, con
  respaldo automático si la elegida no está compilada.
- `parche_velocidad.py`: velocidad del juego ajustable en porcentaje, 0–200%.
- `parche_restaurar.py`: mejoras del menú de F4 — aviso de reinicio pendiente con botón
  para reiniciar, botón de restaurar la configuración de arranque, deslizadores con
  límites para los ajustes decimales, y la API gráfica en uso a la vista.
- `parche_gpu_fallback.py`: respaldo a WARP si no se puede crear el dispositivo D3D12.
- `parche_privilegios.py`: ajuste `grant_user_privileges` para pasar la puerta de
  privilegios de Xbox Live. Apagado por defecto.
- Lanzador nativo en C#/WinForms con la portada al lado, compilado con el `csc.exe` que
  ya trae Windows. Comparte los ajustes con el lanzador antiguo de PowerShell.
- Escalado de resolución interna hasta x4 desde el lanzador.

### Cambiado

- En la carpeta portable, `NFS_Most_Wanted.exe` pasa a ser **el lanzador** y el juego se
  llama `nfsmw.exe`, para que el icono del juego abra la ventana de opciones.
- El camino RTV de la EDRAM es el que viene elegido: casi duplica los fps en gráficas
  integradas frente a ROV.
- Los ajustes del lanzador se llaman "Tamaño de la ventana" y "Resolución interna", que
  es lo que hacen. Antes eran "Resolución de salida" y "Escala de renderizado" y se
  confundían.

### Arreglado

- El menú de F4 ya no abre siempre con un aviso falso de "hace falta reiniciar".
- Los parches ya no se duplican al ejecutarlos dos veces.
- `comprobar_dist.ps1` reconoce `mscoree.dll` como DLL del sistema, y ya no da por rota
  una carpeta que lleva el lanzador de .NET dentro.

### Sin resolver

- Vulkan renderiza en negro en Intel.
- Franja horizontal con el camino RTV en algunas integradas.
- Multijugador: faltan 114 de 158 funciones de red del SDK, incluidas las del System
  Link, y los manejadores de sesión son stubs. Ver
  [docs/diario/red-y-privilegios.md](docs/diario/red-y-privilegios.md).
