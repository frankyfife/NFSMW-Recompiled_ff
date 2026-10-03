# Los parches

Catálogo de lo que este proyecto cambia en el SDK, por qué, y cómo se comprobó.

Todos se aplican sobre el fuente de `..\rexglue-sdk` antes de compilarlo. ~~Ninguno toca
el código del juego.~~ *(Fork: no longer true, see "This fork" below.)*

## This fork (2026-10-03)

The Spanish catalog below describes the nine upstream patches and is still correct for
them. This fork adds a tenth, `tools/parche_ff.py`, and two statements in this page are
outdated:

- **"No patch touches the game code" is no longer true.** `parche_ff.py` changes
  `include/rex/platform/fpscr.h`, a header the generated game code includes inline, so
  after applying or reverting it the game has to be rebuilt as well, not only the SDK.
  And the native renderer (`app/src/native`) compiles SDK sources (the Xenos shader
  translator under `src/graphics/pipeline/shader/` and a few others, taken from
  `..\rexglue-sdk`) into `nfsmw.exe`, so an SDK patch to those files ends up in the game
  executable too (`parche_ff.py` patches `translator_disasm.cpp`, one of them).
- **The red warning about the presenter patch** ("El lanzador avisa en rojo", in the
  `parche_presentador.py` section) is shown by the C# launcher (`tools/lanzador`), which
  looks for the patch marker in the SDK source. The Qt launcher (`launcher-windows/`) has
  no such check: it always passes `--max_fps=0` and sets the frame rate with
  `frame_pacing_fps`, which comes from `parche_ff.py`.

### `parche_ff.py`: the fork's SDK changes

By far the largest patch (about 166 KB: 126 text blocks in 23 SDK files, plus one new
file, `include/rex/graphics/frame_pacer.h`, copied from `tools/sdk_nuevos/`). Its header
lists every change with the reason and the measurements. In short: floating-point
exceptions always masked (without it the game dies with 0xC000008F before the menu);
frame pacing (`guest_vblank_rate`, `frame_pacing_fps`, pacing in `VdSwap`, low-latency
mode, adaptive pacing, smoothing, display lock, the G-Sync / FreeSync mode
`frame_pacing_vrr`, one present per game frame) and frame time recording
(`frame_times_dir`, F10 in game); the SDK side of the native renderer (`IssueSwap` shows
the native frame; while it does, the emulation skips draws, resolves, shader loads, ZPD
sample counting and the host occlusion query chain) and ZPD occlusion queries as a
continuous counter; GPU-thread performance (texture heaps, hot pages, register block
writes, `readback_resolve_max_kb`); controller input passed through the free camera
(`NfsmwInputFilter`, exported by `nfsmw.exe`); the XMA fix for cut sound effects; running
without an audio device; a window larger than the screen; and diagnostics
(`log_guest_fps`, `gpu_capture_frame`, `audio_dump_file`).

How it is maintained:

- **Order.** `CONSTRUIR.bat` applies it last, after the other nine. It has to come after
  `parche_presentador.py`: its `d3d12_presenter.cpp` block is anchored in that patch's
  `Present`.
- **Generated, not edited by hand.** The SDK in `..\rexglue-sdk` is changed directly,
  then `python tools\generar_parche_ff.py` rewrites the `BLOQUES` list of
  `parche_ff.py`. The baseline is not the SDK's git HEAD but HEAD with the other nine
  patches applied in `CONSTRUIR.bat`'s order, so files that other patches touch too are
  diffed correctly; new files are copied into `tools/sdk_nuevos/`. The files it diffs
  are listed in `FICHEROS` / `NUEVOS` at the top of the generator (a newly touched SDK
  file has to be added there). The descriptive header of `parche_ff.py` is not
  regenerated.
- **Check.** `python tools\dev\e2e_check.py` applies the new `parche_ff.py` to a fresh
  baseline and compares it with the working SDK. It must print
  `clean rebuild matches working SDK: True`.
- **Updating an SDK that has an older `parche_ff.py` applied.** Run that older version
  with `--revertir` first. `parche_ff.py` keeps no old versions of its blocks (the
  migration rule further down does not apply to it), so the new version refuses to
  apply over the old one: the anchors of the changed blocks are not found exactly once
  and it writes none of its blocks. (The new file from `tools/sdk_nuevos/` is copied
  before the blocks are checked, so `frame_pacer.h` is already the new one then.)
- **After applying or reverting**, rebuild the SDK **and** the game (the script prints
  that reminder), and copy both `rexruntime.dll` and `rexgpu-xenos.dll` into `build\`.

## Cómo se usan

```bat
python tools\parche_velocidad.py              aplicar
python tools\parche_velocidad.py --estado     ver qué hay puesto
python tools\parche_velocidad.py --revertir   deshacer
```

`CONSTRUIR.bat` los aplica todos en el orden correcto. El orden importa en un caso:
`parche_anillo.py` va antes que `parche_desatasco.py`, porque el segundo se apoya en las
cabeceras (`<atomic>`, `<chrono>`) que mete el primero. El script de desatasco se niega
a aplicarse si el otro no ha pasado.

## El catálogo

### `parche_desatasco.py` — el cuelgue del audio

**El importante.** Sin él, después del prólogo, al salir del garaje el audio se muere y
al volver al menú el juego se congela.

Toca `xboxkrnl_audio_xma.cpp`. Cuando un contexto XMA lleva más de 250 ms girando sin
entrada y con la lectura pegada a la escritura, le da la señal de "buffer terminado" que
el propio juego sabe interpretar.

Es un apaño deliberado: rompe el atasco en vez de evitarlo, y puede costar un tropiezo
de audio en esa voz. La historia completa —y por qué el arreglo "obvio" era el
equivocado— está en [diario/audio-cuelgue.md](diario/audio-cuelgue.md).

**Comprobado:** el usuario jugó la zona que lo reproducía sin que se colgara.

### `parche_anillo.py` — instrumentación del XMA

Prerrequisito del anterior. Añade trazas a las funciones del kernel del XMA. Con el
nivel de log normal no imprime nada.

Un detalle que costó una tarde: los *getters* van limitados a una traza por segundo,
pero los *setters* no. Limitar los dos por igual escondía justo lo que había que ver
—las entregas de entrada— y el diagnóstico se fue por el camino equivocado.

### `parche_presentador.py` — vsync y límite de fps

De fábrica **ninguno de los dos funciona**:

- `vsync` existe como cvar pero no sincroniza nada. Se lee en un solo sitio y solo
  decide si el procesador de comandos duerme o gira en las esperas del guest. El
  `Present` del presentador de D3D12 llevaba el `SyncInterval` clavado a 0.
- No había ningún limitador de fps. Ninguno.

Este parche arregla las dos cosas. El lanzador avisa en rojo si no está aplicado.

> **Fork note:** the warning is only in the C# launcher (`tools/lanzador`). The Qt
> launcher passes `--max_fps=0` and paces with `frame_pacing_fps` from `parche_ff.py`.

### `parche_gpu_fallback.py` — no morir sin GPU

Si el dispositivo D3D12 no se puede crear, cae a WARP en vez de dar una pantalla de
error. Útil en máquinas sin drivers decentes.

### `parche_backend.py` — selector de API gráfica

Añade el cvar `gpu_backend` (`d3d12` / `vulkan`) y se lo pasa al cargador del plugin.

El plugin ya sabía elegir por nombre; lo único que faltaba era que alguien se lo dijera.
`rex_app.cpp` llamaba a `LoadGpuPlugin` con un solo argumento, así que siempre salía
`any`, que en la práctica es D3D12 por ser el primero del `if`.

Tres cosas que este parche aprendió por las malas:

- Es `kRequiresRestart`, no `kInitOnly`. Con `kInitOnly` el menú lo pintaba en rojo y
  no se podía tocar.
- **No hay opción `any`.** Con `any` no se sabía cuál estaba puesta de verdad, que era
  justo lo que había que enseñar.
- Limpia la lista de reinicios pendientes al terminar el arranque, porque si no el menú
  abría con un aviso falso permanente. Ver [arquitectura.md](arquitectura.md).

Si la API elegida no está compilada, cae a la otra y lo dice en el log en vez de no
arrancar.

**Comprobado:** tres pasadas seguidas idénticas, revertir y volver a aplicar devuelve el
mismo resultado, sin restos de versiones anteriores.

### `parche_restaurar.py` — el menú de F4

Cinco bloques en `settings_overlay.cpp`:

- Aviso de reinicio pendiente, con botón para guardar y reiniciar. El SDK ya llevaba la
  cuenta (`GetPendingRestartFlags`) pero no la enseñaba en ninguna parte, así que
  cambiar la API gráfica parecía no hacer nada.
- Foto de la configuración de arranque, para poder volver a ella.
- Botón "Restore defaults" que restaura **esa** foto, no los valores de fábrica del SDK.
- Deslizador para los ajustes decimales con límites, en vez de una caja de texto.
- La API gráfica en uso, leída del registro de cvars.

Ese último punto tiene una lección cara detrás. La primera versión usaba una variable
global compartida con `rex_app.cpp` y **no enlazaba**:

```
lld-link: error: undefined symbol: rex::ui::g_gpu_backend_en_uso
```

`rex_app.cpp` no se compila dentro del SDK: se **instala como fuente** en
`share/rexglue/` y lo compila cada aplicación. Así que la definición acababa dentro de
`nfsmw.exe` y la referencia dentro de `rexruntime.dll`. Para hablar entre módulos está
el registro de cvars.

### `parche_velocidad.py` — velocidad del juego

Añade `game_speed`, en porcentaje, de 0 a 200. No es un límite de fps: cambia a qué
ritmo pasa el tiempo dentro del juego.

En porcentaje y no en multiplicador porque en la ventana de F4 sale un número pelado y
"1.0" no dice de qué. Con un suelo en 0.1% porque un cero literal no cuelga el juego: lo
tumba, por la división de `RecomputeGuestTickScalar`.

### `parche_privilegios.py` — la puerta del multijugador

De fábrica `XamUserCheckPrivilege` deniega **todos** los privilegios, siempre. El
comentario original lo dice: *"If we deny everything, games should hopefully not try to
do stuff"*. En Most Wanted el efecto es el cartel "Los privilegios que tienes en Xbox
Live no te permiten acceder a esta función".

Añade el ajuste `grant_user_privileges`, **apagado por defecto**. Encendido, contesta
que sí a todo.

Abre la puerta del menú y nada más. Lo que hay detrás no funciona; ver
[diario/red-y-privilegios.md](diario/red-y-privilegios.md).

### `parche_diagnostico.py` — trazas del arranque

Instrumentación general que se quedó porque es barata y útil. Entre otras cosas es lo
que puso nombre y hora al cuelgue del audio.

### `tools/diagnostico/parche_xma.py` — instrumentación pesada del XMA

**Fuera del build por defecto.** Traza por segundo del hilo de audio, cada envío y cada
silencio. Se aplica a mano para investigar y se revierte después.

---

## Por qué los parches están escritos así

No es capricho. Cada regla viene de un fallo concreto.

### Sustitución de texto exacta, sin `.original`

La primera versión guardaba una copia del fichero antes de tocarlo. Deja de funcionar en
cuanto **dos parches tocan el mismo fichero**: el segundo guarda como "original" un
fichero que ya estaba parcheado, y revertir deja el árbol en un estado que no es ni el
de antes ni el de después.

Ahora cada parche aplica y deshace por texto, y no guarda nada.

### Bloque a bloque, no un marcador por fichero

Hubo una versión con un solo marcador por fichero: si estaba, el parche se daba por
aplicado. El día que se le añadió un bloque nuevo a un parche ya aplicado, **no hizo
nada** y no dijo nada. El síntoma fue *"abrí el exe y no tenía la barra para cambiar la
velocidad"*.

Ahora cada bloque se comprueba y se aplica por separado.

### Se niegan a escribir si un anclaje no aparece exactamente una vez

Un parche a medias es peor que uno que falla. Si el SDK cambió y el anclaje ya no está,
o está dos veces, el script sale sin tocar nada.

### La regla de migración, que costó tres intentos

Cuando cambias un parche que ya estaba aplicado en algún sitio, hay que quitar la
versión vieja antes de poner la nueva. Y ahí hay dos trampas simétricas:

- **El bloque viejo es un trozo del nuevo** (se le añadió código). Buscar el viejo lo
  encuentra *dentro* del bueno, y sustituirlo por el anclaje le corta la cabeza al
  bloque recién puesto. Luego se vuelve a aplicar y la cola queda **duplicada**. El
  fichero crecía en cada pasada.
- **El bloque nuevo es un trozo del viejo** (se le quitó código). Entonces "el bloque
  bueno está" da que sí aunque lo que hay siga siendo el viejo entero, y el script se da
  por aplicado dejando dentro código muerto.

Intenté resolverlo con una *huella* por versión: un trozo que solo estuviera en esa
versión. No siempre existe: cuando el viejo es prefijo exacto del nuevo, todo lo que hay
en el viejo está también en el nuevo.

La regla que sí vale no necesita huellas:

```python
es_de_verdad_vieja = (viejo in txt) and (viejo not in nuevo or nuevo not in txt)
```

Los dos casos salen bien con eso, y se comprueba solo con los textos.

**Y se prueba corriendo el parche dos veces seguidas y comparando.** El fallo del
duplicado no se ve en la primera pasada, que es la única que se suele mirar.

### Idempotencia

Consecuencia de lo anterior, pero merece decirse aparte: ejecutar un parche N veces
tiene que dar el mismo fichero que ejecutarlo una. Si no, `CONSTRUIR.bat` corrompe el
árbol un poco más en cada reconstrucción.

### Un detalle de Windows

Los parches leen y escriben en modo texto. En Windows eso conserva los CRLF; en Linux
los convierte a LF en la primera escritura. Si comparas resultados entre plataformas,
normaliza los finales de línea antes de gritar.
