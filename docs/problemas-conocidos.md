# Problemas conocidos

Lo que está roto, y hasta dónde se llegó investigando cada cosa. Un problema con el
diagnóstico a medias vale más que uno sin empezar.

## This fork (2026-10-03)

> **Fork note:** this block covers the frankyfife fork (branch
> `windows-qt-launcher-and-fixes`, HEAD `63684bb`). The Spanish text after it is the
> upstream state and is kept as it was. In this fork the Qt launcher always starts the
> game with the native Direct3D 12 renderer (`--native_renderer=true`,
> `--gpu_backend=d3d12`, see [NATIVE_RENDERER.md](NATIVE_RENDERER.md)). The GPU
> emulation is used when `nfsmw.exe` is started directly or through the C# fallback
> launcher (`tools/lanzador`), because the cvar `native_renderer` defaults to false. The
> upstream items below about Vulkan and the RTV stripe concern that GPU emulation path
> only.

### Fixed in this fork

| Problem | Fix | Commit |
|---|---|---|
| Crash before the menu with `0xC000008F` | Floating-point exceptions always masked (`fpscr.h`) | `c8fa2b3` |
| *New game* crash with the Black Edition flag on | The flag is now the single byte `0x82A2CE06` | `c8fa2b3` |
| Upstream required the PAL Spain disc | Built and played with the German PAL disc; the language is picked in the launcher | `c8fa2b3` |
| Sound effects cut or garbled after driving a while | XMA output reuse fixed | `2f919cb` |
| No audio device | Silent audio output instead | `e1edcc3` |
| Window larger than the screen at 225 % display scaling, HUD cut off | The window opens inside the usable area of the display | `df2f763` |
| Crash when the police car loads (native renderer) | Fixed; host crashes are written to `nfsmw_crash.log` | `a397920` |
| One-frame white flashes and stripes (native renderer) | Three causes, three fixes: vertex data copied at the draw and the game at most one frame ahead of the renderer (`native_renderer_copy_draw_data`, `native_renderer_bound_lead`); frames with draws still waiting for their pipeline held back (`native_renderer_hold_incomplete`); draws whose vertex shader stride does not match skipped and their frame held back | `099586c` |
| Memory growing all session from unread occlusion query segments while the native renderer draws | The host occlusion query chain stops when the native renderer takes over | `63684bb` |

### Known limits and open points

- **Native renderer, primitives:** no points, lines or rectangle lists. What it leaves
  out shows in the "skipped" and "unsupported" counts of the `[native renderer] 10 s:`
  log lines.
- **Native renderer, held frames:** a frame with a draw whose vertex shader stride does
  not match the draw's is not shown (the previous one stays). About one per second while
  driving with sparks and skid marks (57 in 60 s of driving in circles, 0 in the minute
  before it; see [NATIVE_RENDERER.md](NATIVE_RENDERER.md)).
- **Fences not native yet:** the emulator still writes fences and the ring's read
  pointer when it parses the packet, not when the native renderer has drawn. Covered by
  measured workarounds (`native_renderer_copy_draw_data`, `native_renderer_bound_lead`).
- **The native renderer needs Direct3D 12 on the same GPU:** it only takes over once
  the emulator's swap can open its frame (`NfsmwNativeFrameShown`). With the Vulkan
  backend or another adapter (`d3d12_adapter`) the GPU emulation keeps drawing.
- **Vulkan:** not re-checked in this fork. The Qt launcher offers no graphics API choice
  and always passes `--gpu_backend=d3d12`; only the C# fallback launcher still offers
  Vulkan.
- **Multiplayer:** still not working (the network layer below the solved privilege
  gate, see the Spanish item below).
- **Settings changed in the Esc menu** are saved to `nfsmw.toml`, but the Qt launcher
  passes its own values as `--name=value` on every start, so the launcher's values win on
  the next start.

## Abiertos

### Vulkan renderiza en negro (Intel)

**Estado:** reproducible, sin diagnosticar.

El backend de Vulkan está entero en el SDK y compila. Se carga bien —tan bien que el
respaldo automático no salta, porque ese solo entra cuando la API ni siquiera existe en
la copia— y la pantalla sale negra.

Lo siguiente sería mirar el log de esa ejecución con `--log_level=debug` a ver qué dice
antes de quedarse en negro. No se ha hecho.

Mientras tanto: el lanzador siempre pasa `--gpu_backend`, así que elegir mal aquí nunca
deja el juego sin poder abrirse. Vuelves al lanzador y marcas DirectX 12.

> **Fork note:** this advice applies to the GPU emulation path and the C# fallback
> launcher only. The fork's Qt launcher has no API choice and always passes
> `--gpu_backend=d3d12`. Vulkan has not been re-checked in the fork.

### Franja horizontal con el camino RTV

**Estado:** visto, no acotado.

> **Fork note:** GPU emulation path only. With the native renderer (the Qt launcher's
> default) the emulation does not draw, so the EDRAM path (ROV or RTV) does not affect
> the picture.

En algunas gráficas integradas el camino rápido de la EDRAM deja una franja horizontal
rara. Como cuesta la mitad de los fps, merece la pena investigarlo antes que renunciar.

Sin comprobar todavía: si depende de la versión del driver de Intel, y si se reproduce en
otras integradas o solo en la Iris 540.

### Multijugador

**Estado:** diagnosticado a fondo, sin implementar.

La puerta de los privilegios está resuelta. Debajo faltan 114 de 158 funciones de red,
incluidas las del System Link, y los manejadores de sesión son stubs que devuelven éxito
sin hacer nada.

El detalle completo, con la tabla de qué falta, está en
[diario/red-y-privilegios.md](diario/red-y-privilegios.md).

### Pocos núcleos

El SDK avisa en el arranque:

```
Too few processor cores - scheduling will be wonky
```

No es decorativo. En máquinas con pocos núcleos el hilo de audio compite peor y el
atasco del XMA es más probable. Si en el log salen muchas líneas `[desatasco]`, es por
aquí.

## Resueltos, documentados por si vuelven

### El audio se moría y el juego se congelaba

Arreglado por `parche_desatasco.py`. La historia completa, incluido el arreglo que
parecía obvio y estaba mal, en [diario/audio-cuelgue.md](diario/audio-cuelgue.md).

### Pantalla verde rota que parecía un fallo del backend

**No era el código.** Eran dos interruptores de depuración que se habían colado en
`nfsmw.toml` desde el menú de F4:

```toml
d3d12_tessellation_wireframe = true
native_stencil_value_output_d3d12_intel = true
```

El primero dibuja en alambre la geometría teselada. El segundo fuerza la salida nativa de
stencil **en Intel**, que es justo el caso que el SDK excluye a propósito. Con el camino
RTV destroza el render sin dar un solo error de GPU.

**Lección: si de repente se ve mal, mira el toml antes de sospechar del código.**

### `NtCreateFile FAILED` en el log

43 avisos de ficheros del juego que no se abren, con `0xc000000f`. **Es normal.** El
juego tantea ficheros que en este disco no existen. Se confirmó comparando con una
ejecución larga que llegó hasta el final: salen exactamente los mismos 43.

No perseguir esto.

### El aviso permanente de "hace falta reiniciar"

El menú de F4 abría siempre diciendo `Restart needed to apply: gpu_backend`, aunque no
hubieras tocado nada.

`SetFlagFromSource` apunta en la lista de pendientes cualquier cvar `kRequiresRestart`
que se toque, sin mirar de dónde viene el valor. Como el lanzador pasa `--gpu_backend`
siempre, entraba en la lista pese a estar ya aplicado. Un aviso que no se puede quitar
deja de leerse, y entonces tampoco se lee cuando es real.

`parche_backend.py` limpia la lista cuando termina el arranque.

### Subir la resolución no cambiaba nada

No era un fallo: eran dos controles distintos con nombres parecidos. `--resolution` solo
agranda la imagen; el que la hace más fina es `--resolution_scale`. El lanzador ahora los
llama "Tamaño de la ventana" y "Resolución interna", y enseña qué hace la escala elegida.

> **Fork note:** in the fork's Qt launcher the two controls are *Resolution* (window and
> video mode) and *Render scale (AA)*, which sets `native_renderer_scale` (1-4, a
> multiple of 1280x720); `resolution_scale` is always passed as 1.

Ver [rendimiento.md](rendimiento.md).

## Cosas que conviene no volver a intentar

**Reservar un bloque en el anillo del XMA** para desambiguar lleno/vacío. Parece el
arreglo de libro y es incorrecto: `output_buffer_valid = 0` con el anillo lleno es la
señal que el juego usa para saber que el buffer terminó. Quitársela le quita su única
salida.

**Buscar un backend de DirectX 11.** No existe en este SDK y no es un olvido: la
emulación de la Xenos se apoya en cosas de la generación de DX12 —los rasterizer ordered
views, los descriptores sin límite, las escrituras tipadas desde shaders para el
memexport—. Un backend de DX11 no es un ajuste, es rehacer el plugin de GPU. Y no
arreglaría nada: el cuello está en la GPU al 100%, y la API no cambia cuántos píxeles hay
que sombrear.

**Perseguir los servidores de EA.** Están apagados. La única vía para el multijugador es
System Link.
