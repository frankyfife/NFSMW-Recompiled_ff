# El lanzador

> **Fork note (2026-10-03).** This document describes the C# WinForms launcher
> (`tools/lanzador/Lanzador.cs`, built by `CONSTRUIR_LANZADOR.bat`). In this fork it is
> only the fallback: `CONSTRUIR.bat` builds the Qt launcher (`launcher-windows/`) and
> builds this one only when the Qt build fails, for example because Qt is not installed.
> The main launcher is described in [LAUNCHER.md](LAUNCHER.md).
>
> The C# launcher does not pass `--native_renderer`, so the game runs on the GPU
> emulation (the cvar defaults to false), unless `nfsmw.toml` sets it: an Esc-menu change
> in a game started from the Qt launcher writes `native_renderer = true` there, with the
> launcher's other arguments (see [LAUNCHER.md](LAUNCHER.md#arguments-always-passed),
> "Precedence"). It has none of the fork's options: native
> renderer scale, MSAA, mipmaps, frame rate target and frame pacing, G-Sync / FreeSync,
> field of view, post-processing, car detail, Black Edition, Unlock everything, language.
>
> What below is outdated for the main (Qt) launcher:
>
> - **"Es lo que se abre para jugar"** (intro): the window that opens from
>   `NFS_Most_Wanted.exe` is now the Qt launcher. The file names in "Los nombres" are the
>   same for both launchers; the icon is not: the Qt launcher's is its own artwork (an
>   amber square with speed chevrons, `launcher-windows/res/make_icon.py`), not the game's.
> - **"Los ajustes"**: the Qt launcher stores its settings in `launcher.ini` (INI format)
>   in its root folder (next to it in `build\`; the repository root when it runs from
>   `launcher-windows\out\` and finds the game in `app\out\`), not in `lanzador.json`, and
>   shares nothing with this launcher or `lanzador.ps1`.
> - **"Motor de vídeo" and "API gráfica"**: the Qt launcher has neither option. It always
>   passes `--gpu_backend=d3d12` and `--native_renderer=true`, because the native
>   renderer is Direct3D 12. The reason given in "Por qué la API gráfica no tiene
>   automático" still holds: the command line wins over `nfsmw.toml`.
> - **"Los dos ajustes de resolución"**: in the Qt launcher, Resolution goes to
>   `--resolution` (window resolution and video mode) and Render scale (AA), 1×-4×, goes
>   to `--native_renderer_scale`, a multiple of 1280 × 720, so its note does show the
>   size in pixels. `--resolution_scale` is always 1, because the emulation draws
>   nothing.
> - **"Fotogramas"** (vsync and fps limit): the Qt launcher sets a target of 30, 60,
>   Unlimited or Custom 10-240 through `--frame_pacing_fps`, passes `--max_fps=0`, and
>   has V-Sync on by default.
> - **"Argumentos fijos"**: the Qt launcher's fixed list is longer (it adds
>   `--native_renderer=true`, `--resolution_scale=1`, `--guest_vblank_rate=1000` and
>   `--max_fps=0`, among others) and every argument goes as
>   `--name=value`: with `--name value` the cvars that live in the GPU plugin DLL
>   (`frame_pacing_fps`, `guest_vblank_rate`) were silently left at their defaults. The
>   full list is in [LAUNCHER.md](LAUNCHER.md#arguments-always-passed).
> - **"La portada"**: the Qt launcher reads `portada.jpg` from the same root folder as
>   `launcher.ini` at start instead of building it into the exe, and draws a pattern when
>   it is missing.
> - **"Los nombres"**: started directly, `nfsmw.exe` also uses the GPU emulation, unless
>   `nfsmw.toml` sets `native_renderer` (which happens after an Esc-menu change in a game
>   started from the Qt launcher, see above).

Ventana nativa de Windows en C# con WinForms, con la portada del juego al lado en plan
instalador. ~~Es lo que se abre para jugar.~~ (Fork note: only when the Qt launcher could
not be built; see above.)

## Compilarlo

```bat
CONSTRUIR_LANZADOR.bat
```

Usa el `csc.exe` del .NET Framework que **ya viene con Windows**, en
`C:\Windows\Microsoft.NET\Framework64\v4.0.30319\`. No hace falta Visual Studio, ni el
SDK de .NET, ni nada.

Se eligió C# frente a las alternativas por eso:

- **C++ con Win32 a pelo**: sale un exe pequeño, pero montar a mano una ventana con
  veinte controles es muchísimo código para lo que es.
- **Python empaquetado**: hay que instalar Python y PyInstaller, y el exe acaba pesando
  30 MB.
- **C# con el compilador que ya trae Windows**: un solo fichero, los mismos controles
  que ya usaba el lanzador de PowerShell —WinForms es lo que había debajo—, icono y
  portada dentro del exe, y cero instalaciones.

### Se compila con un csc viejo

El que trae Windows es de C# 5 (2012). En `Lanzador.cs` **no** se puede usar nada
moderno: ni cadenas interpoladas `$"..."`, ni `?.`, ni `nameof`, ni miembros con `=>`.
Todo con `string.Format` y sintaxis clásica.

Si algo de eso se cuela, el error que sale no dice "necesitas un compilador más nuevo":
dice cosas raras sobre `;` que faltan, y se pierde media tarde.

## Los nombres, que están intercambiados a propósito

En la carpeta portable:

| Fichero | Qué es |
|---|---|
| `NFS_Most_Wanted.exe` | **El lanzador**, con el icono del juego |
| `nfsmw.exe` | El juego de verdad |

El motivo es solo que al hacer doble clic en el icono del juego salga la ventana de
opciones, como en cualquier juego con lanzador.

**El juego sigue sabiendo arrancar solo.** `nfsmw.exe` a pelo funciona: `nfsmw_app.h` le
pone `gpu_plugin`, `mnk_mode` y `readback_resolve` si nadie los pidió, y busca una ISO en
su carpeta. Queda como salida si el lanzador diera guerra.

Dos consecuencias del cambio de nombre:

- El buscador de ISO prefiere la que se llame **igual que el ejecutable**. Al renombrar,
  una `NFS_Most_Wanted.iso` deja de ser la preferida y entra por la segunda regla (la
  primera por orden alfabético). Con una sola ISO da igual. Llámala `nfsmw.iso` si
  quieres que vuelva a ser la preferida.
- **No cambia dónde guarda sus cosas el juego.** Esa carpeta sale de
  `GetUserFolder() / GetName()`, y `GetName()` está en el código, no en el nombre del
  fichero. La caché de shaders sigue en `Documents\nfsmw\cache`.

## Los ajustes

Todo se guarda en `lanzador.json`, en la misma carpeta. El lanzador antiguo de PowerShell
lee y escribe el mismo fichero con los mismos nombres de campo, así que conviven.

| Grupo | Qué |
|---|---|
| Imagen del juego | La ISO |
| Pantalla y resolución | Tamaño de la ventana, resolución interna, ventana o completa |
| Fotogramas | Vsync y límite de fps |
| Motor de vídeo | Automático / Rápido (rtv) / Exacto (rov) |
| API gráfica | DirectX 12 / Vulkan |

### Por qué la API gráfica no tiene "automático"

Es una salida de emergencia, y por eso es distinta del resto.

`gpu_backend` también se puede cambiar desde el menú de F4. El problema: si eliges una
API que en tu equipo da pantalla negra, guardas y reinicias, el valor se queda escrito en
`nfsmw.toml` y **ya no hay forma de volver** — para cambiarlo necesitas el menú, y para
llegar al menú necesitas ver algo. Pasó de verdad.

Lo que lo arregla es el orden de prioridad de los cvars del SDK: la línea de comandos
manda sobre el fichero de configuración. Así que el lanzador pasa **siempre**
`--gpu_backend`, aunque coincida con el toml. La ventana siempre gana.

Un "automático" que no pasara nada devolvería el mando al toml, que es justo el agujero.
En "Motor de vídeo" sí tiene sentido, porque elegir mal ahí no deja el juego invisible.

### Los dos ajustes de resolución

Se llamaban "Resolución de salida" y "Escala de renderizado", y con esos nombres es fácil
tocar el primero esperando lo segundo, ver que no cambia nada y darlo por roto. Ahora:

- **Tamaño de la ventana** → `--resolution`. Solo agranda la imagen.
- **Resolución interna** → `--resolution_scale`. El "x2" de los emuladores.

Debajo del segundo hay una línea que dice qué hace la escala elegida. No pone la
resolución en píxeles a propósito: la escala no multiplica el tamaño de la ventana, sino
los render targets del juego, que son de un tamaño suyo que desde el lanzador no se
conoce. Poner "2560 x 1440" sería inventárselo.

### Argumentos fijos

Van siempre, y no son preferencias:

| Argumento | Por qué |
|---|---|
| `--readback_resolve=fast` | Sin esto la imagen sale lavada y el sol reventado |
| `--gpu_plugin xenos` | Es el único backend construido |
| `--mnk_mode` | Teclado y ratón además del mando |
| `--gpu_backend=...` | Siempre; ver arriba |

## La portada

`tools/lanzador/portada.jpg` y `icono.ico` **no están en el repositorio**: son la
carátula del juego, arte de Electronic Arts.

El lanzador arranca perfectamente sin ellas. `CargarRecurso` devuelve null si el recurso
no está y el panel lateral se dibuja en negro con el título del proyecto.

Si quieres poner una:

- `portada.jpg` — se dibuja entera, pegada arriba del panel, con el hueco de abajo para
  el texto. Relación recomendada parecida a una carátula (algo así como 760×1064).
- `icono.ico` — cuadrado, con los tamaños del 16 al 256.

`CONSTRUIR_LANZADOR.bat` avisa si faltan.

## Detalles de implementación que conviene conocer

**El juego se espera en otro hilo.** El lanzador de PowerShell hacía `WaitForExit` en el
hilo de la ventana, y mientras jugabas la ventana se quedaba colgada: Windows la pintaba
en blanco y la marcaba como "no responde". Aquí se lanza aparte y se vuelve con `Invoke`
al terminar, con una comprobación delante por si cerraste el lanzador mientras jugabas.

**El json se parsea a mano.** Son diez parejas clave/valor sin anidar; no hacía falta
traerse Newtonsoft (que habría que descargar) ni `JavaScriptSerializer` (que obliga a
referenciar `System.Web.Extensions`). Lo único delicado son las barras invertidas de las
rutas de Windows, que en json van dobladas.

**El panel de la portada se pinta a mano**, no con un `PictureBox`, para controlar cómo
encaja: la imagen entra entera y pegada arriba en vez de recortarse por los lados, que se
comería parte del título.

## El lanzador antiguo

`tools/lanzador/lanzador.ps1` es la versión en PowerShell. Hace lo mismo, comparte los
ajustes y se abre con `LANZADOR.bat` en la carpeta portable. Está por si el lanzador
compilado diera problemas en alguna máquina.
