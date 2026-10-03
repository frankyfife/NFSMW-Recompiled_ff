# Compilar desde cero

De un clon limpio a una carpeta jugable.

## This fork (2026-10-03)

The steps below still work, but these parts are outdated in this fork:

- **Requirements: Qt 6 for MSVC 2022 x64 (optional).** Only for the Qt launcher in
  `launcher-windows/`. `launcher-windows\build.bat` uses `QT_DIR`, or else looks for
  `D:\Qt\6.*\msvc2022_64` and `C:\Qt\6.*\msvc2022_64`; its header shows how to install
  it with `aqtinstall`. Without Qt the build still finishes, with the C# launcher.
- **Where the SDK has to be.** `app/CMakeLists.txt` looks for the SDK source only in
  `..\rexglue-sdk` (cache variable `REXSDK_SOURCE_DIR`). The `.\sdk` fallback mentioned
  below is only for the patch scripts. Without the SDK source there (or on a non-Windows
  build) the game is built with `src/native/parallel_stub.cpp`: CMake prints
  `Native renderer: off` and the native renderer does nothing.
- **Ten patches, not nine** (phase 1). `CONSTRUIR.bat` applies, in this order:
  `parche_diagnostico`, `parche_anillo`, `parche_desatasco`, `parche_presentador`,
  `parche_gpu_fallback`, `parche_restaurar`, `parche_velocidad`, `parche_backend`,
  `parche_privilegios` and, last, `parche_ff`, the fork's own (see
  [parches.md](parches.md)). The fixes end up in `rexruntime.dll` **and**
  `rexgpu-xenos.dll` (phase 2), and `parche_ff.py` also changes a header the game
  includes inline.
- **The launcher step** (phase 4) first runs `launcher-windows\build.bat /silencioso`
  (Qt launcher, plus its Qt DLLs through `windeployqt`). If that fails, because Qt is
  not found or the build fails, it falls back to `CONSTRUIR_LANZADOR.bat`, the C#
  launcher in `tools/lanzador`. Either one becomes `build\NFS_Most_Wanted.exe` and the
  game is renamed to `nfsmw.exe`.
- **Building only the launcher.** `launcher-windows\build.bat` builds the Qt launcher;
  `CONSTRUIR_LANZADOR.bat` (section "Compilar solo el lanzador") is the C# one.
- **Playing.** In the Qt launcher the game data (ISO or extracted folder) is chosen in
  the General tab; an ISO is extracted once (about 7 GB) into
  `build\game_root_cache\<name>`. See [lanzador.md](lanzador.md).
- **`build\nfsmw.toml`.** The Esc menu saves its settings there as well, but the Qt
  launcher passes its own values on every start, so they win.
- **Rebuilding after changing `parche_ff.py`.** The advice at the end (rebuild only the
  SDK and copy only `rexruntime.dll`) is wrong for it. After applying it:
  rebuild and install the SDK, rebuild the game (`fpscr.h` is inline in the generated
  code), and copy `rexruntime.dll` **and** `rexgpu-xenos.dll` into `build\`, along with
  the rebuilt `nfsmw.exe`. `CONSTRUIR.bat` does all of this.

### Developer scripts in `tools/dev`

Small helpers used while working on the fork. Their paths are fixed to
`D:\NFSMW\NFSMW-Recompiled_ff` and `D:\NFSMW\rexglue-sdk`. `profbuild.bat`,
`replaybuild.bat` and `buildsampler.bat` write their logs and outputs to
`%TEMP%\claude` (the sampler as `%TEMP%\claude\sampler.exe`); `sdkbuild.bat`,
`gamebuild.bat` and `e2e_check.py` print to the console, and `menutest.ps1` logs to
`%TEMP%\nfsmw_menu_<name>.log`.

| Script | What it does |
|---|---|
| `sdkbuild.bat` | Builds and installs the SDK (`out/build/win-amd64`, Release, target `install`). Its second step runs `cmake --build --preset win-amd64-release` from the repository root, where there is no `CMakePresets.json` (only `app\` has one), so that step does not build the game: use `gamebuild.bat` for that |
| `gamebuild.bat` | Builds the game in `app\` with the `win-amd64-release` preset |
| `profbuild.bat` | Separate SDK build with debug info (`out/build/win-amd64-prof`, `rexgpu-xenos` and `rexruntime`), for the sampling profiler |
| `buildsampler.bat` | Builds the sampling profiler, `tools/cpuprof/sampler.cpp` |
| `replaybuild.bat` | Builds `tools/replay` (`nfsmw_replay.exe`) against the installed SDK |
| `e2e_check.py` | Checks `parche_ff.py`: must print `clean rebuild matches working SDK: True` |
| `menutest.ps1` | Starts `build\nfsmw.exe`, skips the intro movies with Enter and logs frame pacing in the menu |

## Lo que hace falta

| Cosa | Por qué |
|---|---|
| Windows 10 u 11 x64 | El backend gráfico es Direct3D 12 |
| Visual Studio 2022 Build Tools | MSVC y el SDK de Windows. No hace falta el IDE |
| CMake 3.28+ y Ninja | El SDK y la aplicación usan presets |
| Clang 20+ | El C++ generado no compila con MSVC |
| Python 3.10+ | Los parches y las herramientas |
| El SDK ReXGlue | Se clona al lado, en `..\rexglue-sdk` |
| Tu propia ISO o dump GOD | El juego. No está aquí ni lo va a estar |

Detalle de la instalación del entorno en [00-entorno.md](00-entorno.md).

## La estructura que se espera

Los scripts buscan el SDK **al lado** del proyecto, no dentro:

```
Documents\
├── NFSMW Recompiled\     ← este repositorio
└── rexglue-sdk\          ← el SDK, clonado aparte
```

Si lo tienes en otro sitio, los parches también miran en `.\sdk`.

## Los pasos

### 1. El SDK

```powershell
.\tools\bootstrap.ps1
```

Comprueba los prerrequisitos, clona el SDK en `..\rexglue-sdk` y lo compila e instala.

### 2. Tu `default.xex`

```bat
EXTRAER_XEX.bat
```

Saca el `default.xex` de tu ISO y lo deja en `assets\`. Esa carpeta está en
`.gitignore` y ahí se queda.

Detalle y alternativas (GOD, XContent) en
[01-extraccion-xex.md](01-extraccion-xex.md).

### 3. Compilar

```bat
CONSTRUIR.bat
```

Esto es todo. Por dentro hace cinco fases:

1. **Parches del SDK.** Aplica los nueve parches del proyecto sobre `..\rexglue-sdk`.
   El orden importa: `parche_anillo` va antes que `parche_desatasco`.
2. **Recompilar el SDK.** Aquí es donde acaban los arreglos, dentro de
   `rexruntime.dll`. Se configura con Vulkan encendido para que el selector de API
   tenga dos opciones de verdad.
3. **Generar y compilar el juego.** Dos pasadas de ninja, y no es capricho: ver abajo.
4. **Armar `build\`.** Copia el ejecutable, las DLL y los ficheros de apoyo, y borra
   los restos de ejecuciones anteriores. Después construye el lanzador y le cambia el
   nombre al juego.
5. **Comprobar que la carpeta es autónoma.** Lee la tabla de importaciones PE de cada
   binario y sigue las dependencias en cadena, para asegurarse de que no falta ninguna
   DLL.

Y al terminar arma las carpetas de reparto en `..\build release\`, para que no haya
que acordarse de un paso a mano. Antes esto era "comprime `build\` sin la ISO", y ese
paso tenía una trampa: la ISO son varios GB y es fácil mandarla sin querer.

- `NFSMW Windows x64\` — jugable, con el ejecutable dentro. Se comprime y se manda a
  alguien que tenga su propia copia. **No se publica.**
- `NFSMW Windows x64 - Portable\` — todo menos el juego. Esta sí.

Tarda bastante la primera vez: son 131 ficheros de C++ generado, más de un millón de
líneas.

### 4. Jugar

Copia tu ISO dentro de `build\` y abre `build\NFS_Most_Wanted.exe`.

Ese es **el lanzador**, con el icono del juego. El juego de verdad es `nfsmw.exe`.
El intercambio de nombres es para que al hacer doble clic en el icono salga la ventana
de opciones; ver [lanzador.md](lanzador.md).

Si llamas a tu ISO `nfsmw.iso` será la preferida cuando haya varias.

## Por qué dos pasadas de compilación

El generador reescribe `generated\default\nfsmw_pch.h`, y de esa cabecera sale la
precompilada que usan los 131 ficheros generados.

En una sola pasada, ninja decide al arrancar qué ficheros están sucios. En ese momento
`nfsmw_pch.h` todavía no ha cambiado, así que da la precompilada por buena. Luego, ya
dentro de la misma pasada, el generador la cambia. Cuando le toca el turno a los `.cpp`,
clang compara y aborta:

```
fatal error: file 'nfsmw_pch.h' has been modified since the precompiled header was
built: size changed (was 18553, now 18522)
```

Lanzando el generador primero y por separado, la segunda pasada arranca con las
cabeceras ya definitivas.

## Cuando algo falla

### El SDK no enlaza

Lo más probable es que un parche esté a medias. Mira el estado:

```bat
for %f in (tools\parche_*.py) do python %f --estado
```

Y si hace falta, revierte todos y vuelve a empezar:

```bat
for %f in (tools\parche_*.py) do python %f --revertir
```

### Un parche dice que el anclaje no aparece una sola vez

El SDK ha cambiado respecto a lo que el parche espera. El script no ha tocado nada. Hay
que mirar el bloque a mano y actualizar el parche; ver [parches.md](parches.md).

### `RC` y `vcvars64`

En los `.bat` de este proyecto los códigos de retorno van siempre en una variable
llamada `SALIDA`, **nunca** `RC`. `vcvars64` pone `RC` con la ruta del compilador de
recursos y CMake la lee al detectar el toolchain. Usar `RC` para otra cosa rompe la
configuración de forma difícil de ver.

### El juego arranca pero se ve mal

Antes de sospechar del código, mira `build\nfsmw.toml`. Los ajustes que se guardan desde
el menú de F4 acaban ahí, y hay dos de depuración que destrozan el render sin dar
errores. Ver [problemas-conocidos.md](problemas-conocidos.md).

### Sin espacio o sin paciencia

El árbol generado ocupa varios GB. `app\generated\` se puede borrar entero: se rehace.

## Compilar solo el lanzador

```bat
CONSTRUIR_LANZADOR.bat
```

Usa el `csc.exe` que ya trae Windows dentro de `C:\Windows\Microsoft.NET\`. No hace
falta instalar nada. Ver [lanzador.md](lanzador.md).

## Recompilar tras tocar un parche

~~No hace falta rehacer el juego: los parches solo tocan el SDK.~~ *(Fork: wrong for
`parche_ff.py`, which needs the SDK and the game rebuilt and both `rexruntime.dll` and
`rexgpu-xenos.dll` copied; see "This fork" at the top.)*

```bat
python tools\parche_loquesea.py
cd ..\rexglue-sdk
cmake --build out/build/win-amd64 --config Release --target install
```

Y copiar la `rexruntime.dll` nueva a `build\`. `CONSTRUIR.bat` hace todo eso, pero si
solo cambiaste un parche, esto es mucho más rápido.
