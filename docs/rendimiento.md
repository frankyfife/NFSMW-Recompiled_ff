# Rendimiento

## This fork (2026-10-03)

> **Fork note:** the Spanish text after this block is the upstream state, measured with
> the GPU emulation on an Intel Iris 540, and is kept as it was. In this fork the Qt
> launcher always starts the native Direct3D 12 renderer (`app/src/native`, see
> [NATIVE_RENDERER.md](NATIVE_RENDERER.md)): it records the game's D3D library calls on
> the game thread, draws them on its own thread, and its picture is shown through the
> emulator's swap. The upstream sections on the EDRAM path, `--resolution_scale`, the
> frame limit and the shader cache describe the GPU emulation path only, which is used
> when `nfsmw.exe` is started directly or through the C# fallback launcher (the cvar
> `native_renderer` defaults to false), or when the native renderer cannot take over
> (it needs Direct3D 12 on the same GPU, otherwise the emulation keeps drawing).

### What no longer matters with the native renderer

- **EDRAM path (ROV / RTV) and `resolution_scale`:** the emulation skips its draws and
  resolves, so neither affects the picture or the cost. The Qt launcher passes
  `--resolution_scale=1` and does not pass `render_target_path_d3d12`.
- **Render scale** is `native_renderer_scale`, 1-4, a multiple of the game's 1280x720
  (supersampling). In the launcher it is *Render scale (AA)*, default 2x; *Resolution*
  (480p-2160p or custom, default 1080p) only sets the window and video mode.
- Other native renderer settings: `native_renderer_msaa`, `native_renderer_anisotropic`,
  `native_renderer_mipmaps` (0 game, 1 sharper, 2 off). `post_processing`,
  `car_lod_highest` and `fov_scale` are patches of the game code (`app/src`), so they
  work with either path.

What the GPU emulation still does next to the native renderer: it reads the whole PM4
command stream (registers, fences, `WAIT_REG_MEM`, interrupts, swaps, tile replays). It
skips draws, resolves, ZPD sample counting (and since 2026-10-03 the host occlusion
query chain), shader loads (deferred) and its own front buffer.

### Frame rate

Set by the SDK frame pacer (`tools/parche_ff.py`), not by the upstream 60 Hz guest vblank
described under *Fotogramas* below. The launcher always passes:

- `frame_pacing_fps`: the target (launcher: 30, 60 default, Unlimited = 0, Custom
  10-240), paced where the game hands over a frame (`VdSwap`).
- `guest_vblank_rate=1000`: the game gets a vblank every millisecond, so it never misses
  a vblank slot.
- `max_fps=0`: the presenter's own frame limit is off.

V-Sync is on by default. The G-Sync / FreeSync mode (`frame_pacing_vrr`, off by default)
paces at `min(target, floor(refresh - refresh^2/3600))`, 116 fps at 120 Hz, also at
Unlimited, and presents with V-Sync without the display lock. The launcher's Advanced
tab holds the rest (pacing in the game thread, low-latency mode, adaptive pacing,
smoothing 0-16 ms, lock to display, one present per game frame). Game speed is still a
separate setting (`game_speed`, *Game speed* 20-200 % in the Esc menu).

### Native pipeline cache

Created pipelines are kept in `native_pipelines.bin` next to the game
(`native_renderer_pipeline_cache`, on by default) and loaded in later runs; a cache from
another driver is refused and started again. The `dist` target clears the build folder
(apart from the game data, ISOs, launcher settings and cover image), so the first run
after packaging builds it again. Measured: on a first run 2-6 frames per 10 s held back
for pipelines, on the next run none. The shader cache in `Documents\nfsmw\cache`
described below belongs to the GPU emulation.

Textures: all mip levels since 2026-10-02 (before only level 0), placed in 64 MB heaps
and untiled by `native_renderer_texture_threads` workers (default 6). Loading an area's
textures went from 76 ms to 5.9 ms.

### Measurements

| Measurement | Value |
|---|---|
| Free roam, uncapped, 2160p, render scale 2, driving | 298-326 fps |
| Same, standing | about 366-376 fps |
| Renderer thread per frame in free roam | 1.8-2.3 ms |
| GPU emulation thread ("GPU Commands") busy, before / after step 1 (2026-10-03, uncapped, 2160p, scale 2) | 25.7 % / 22.0 % |
| Fork's own emulation path vs native renderer, same run (2026-10-01) | 148 fps vs 263-271 fps |
| 60 fps in the menu, from the game handing over a frame (`VdSwap`) to the output, pacer in the command processor / pacing in `VdSwap` (GPU emulation, 2026-09-29) | 49.8 ms / 19.1 ms |
| Uncapped in the menu (652 fps), frame start to output, without / with low-latency mode (GPU emulation, 2026-09-29) | 6.0 ms / 3.1 ms |

Uncapped, the game thread sets the frame rate, not the renderer (see
[NATIVE_RENDERER.md](NATIVE_RENDERER.md) for the profiles and the GPU used).

### Measuring tools

- **F10** in the game: frame time recording. One CSV per recording
  (`frametimes_<date>_<time>.csv`: present time and, from DXGI, when the frame reached
  the display) into `frame_times_dir`, which the launcher sets to `logs\frametimes`.
- **`log_guest_fps`** (launcher, Advanced, *Performance statistics in the log*, on by
  default): every 10 s the frames the game presents, pacing and latency. The native
  renderer logs its own `[native renderer] 10 s:` line regardless, and every frame over
  30 ms on its thread with a breakdown (`slow frame`).
- **`tools/analyze_frametimes.py`**: reads the F10 CSVs and PresentMon CSVs (by default
  every `build/logs/frametimes/*.csv`) and writes `report.html` with percentiles,
  hitches and a chart per recording.
- **`tools/measure_frametimes.bat`**: records with PresentMon, independent of the game's
  own log; needs administrator rights (it asks through UAC).
- **`tools/cpuprof`**: sampling profiler for one thread of the running game (for example
  "GPU Commands"), no administrator rights; built with `tools/dev/buildsampler.bat`.
- **Esc menu, DEBUG tab:** game fps and frame time (the meter runs while the menu is
  open) and the current values of the main settings. Its *EDRAM path* and *Internal
  scale* rows are the emulation's settings and say nothing about the native renderer.

---

## Upstream text (GPU emulation)

Lo medido, no lo supuesto. La máquina de referencia es modesta a propósito: si algo se
nota ahí, se nota en cualquier sitio.

**Equipo de referencia:** Intel Iris 540 (integrada, vendor 0x8086, device 0x1926),
pocos núcleos. El SDK avisa en el arranque: `Too few processor cores - scheduling will
be wonky`.

Lo que la GPU reporta y que condiciona lo demás:

```
Max GPU virtual address bits per resource: 38
Rasterizer-ordered views: yes
Resource binding: tier 3
Tiled resources: tier 3
Pixel-shader-specified stencil reference: yes
```

## Lo que más cambia: el camino de la EDRAM

| Camino | Qué hace | FPS medidos |
|---|---|---|
| ROV | Rasterizer ordered views. Exacto | ~10 |
| RTV | Render targets del host. Aproximado | ~18 |

Casi el doble. El SDK elige ROV en Intel por defecto, que es la decisión conservadora;
`render_target_path_d3d12 = "rtv"` se la salta.

**El coste:** en algunas integradas el camino rápido deja una franja horizontal rara.
Está sin cerrar del todo — ver [problemas-conocidos.md](problemas-conocidos.md).

Este ajuste es `kInitOnly`: se decide al arrancar y no se puede tocar desde F4. El
lanzador lo pasa por línea de comandos.

## Resolución interna

`--resolution_scale` es el "x2" de los emuladores: multiplica el tamaño de los render
targets y de la EDRAM emulada, así que el juego dibuja de verdad más píxeles.

Crece con el cuadrado: x2 son **cuatro** veces los píxeles. Partiendo de 18–38 fps a x1,
en esta máquina x2 no es jugable.

Lo que sí conviene saber es que **no se va a recortar por falta de capacidad**. El SDK
lo limita en dos casos:

- si la GPU está por debajo de *tiled resources tier 1* — aquí es tier 3
- si el espacio de direcciones no llega: `kBufferSize × escala²` tiene que caber en los
  bits por recurso. Con 38 bits y un buffer de 512 MB, a x2 hacen falta 31. Sobra
  muchísimo.

Si el SDK llega a recortarla, lo dice:

```
The requested draw resolution scale is not supported by the device or the emulator,
reducing to NxN
```

No confundir con `--resolution`, que solo cambia el tamaño de la ventana y del modo de
vídeo del guest. Ver [arquitectura.md](arquitectura.md).

## Ajustes que cuestan y a veces no se ven

### `readback_resolve`

Por defecto `none`. El valor `fast` copia de GPU a CPU **en cada fotograma**.

Lo pone la propia aplicación (`nfsmw_app.h`) porque sin él la imagen sale lavada y el sol
reventado. O sea: es un arreglo visual, no una preferencia, y su coste es el precio de
que se vea bien.

### `native_stencil_value_output_d3d12_intel`

**Dejar apagado en Intel.** El SDK excluye esta ruta en Intel a propósito:

```cpp
use_stencil_reference_output_ =
    REXCVAR_GET(native_stencil_value_output) &&
    provider.IsPSSpecifiedStencilReferenceSupported() &&
    (REXCVAR_GET(native_stencil_value_output_d3d12_intel) ||
     provider.GetAdapterVendorID() != kIntel);
```

Encenderlo fuerza esa salida en Intel, que es justo el caso excluido. Con el camino RTV
destroza el render **sin dar un solo error de GPU**, lo cual lo hace especialmente
difícil de diagnosticar.

### `d3d12_tessellation_wireframe`

Interruptor de depuración: dibuja en alambre toda la geometría teselada. No tocarlo salvo
para depurar.

Estos dos últimos se colaron una vez en el `nfsmw.toml` desde el menú de F4 y produjeron
una pantalla verde rota que parecía un fallo del backend gráfico. **Si de repente se ve
mal, mira el toml antes de sospechar del código.**

## Fotogramas

Ni el vsync ni el límite de fps funcionan de fábrica: `parche_presentador.py` los
implementa. Ver [parches.md](parches.md).

Un detalle que sorprende: **limitar los fps no ralentiza el juego.** El parpadeo vertical
del guest lo genera un hilo aparte contra el reloj de pared, a `video_mode.refresh_rate`
(60 Hz), independiente del ritmo de dibujado. Si quieres cambiar la velocidad del juego,
eso es `game_speed`, otro ajuste distinto.

## La caché de shaders

Vive en `%USERPROFILE%\Documents\nfsmw\cache`, **fuera** de la carpeta portable. Sale de
`GetUserFolder() / GetName()`, y `GetName()` está en el código, así que renombrar el
ejecutable no la mueve.

Dos consecuencias:

- Rehacer `build\` no la borra. El primer arranque tras una build nueva sigue siendo
  rápido.
- Si sospechas que la caché está corrupta tras cambiar de backend, hay que borrarla a
  mano.

En un arranque normal se ven líneas como `Translated 85 shaders from the storage` y
`Created 258 graphics pipelines ... from the storage`.

## Método

Los números de arriba salieron de comparar ejecuciones cambiando **una** cosa cada vez y
mirando el contador de F3. Es tosco pero suficiente para diferencias del 80%.

Para diferencias pequeñas no vale: la variabilidad entre ejecuciones en una integrada con
pocos núcleos se come cualquier mejora de un dígito.
