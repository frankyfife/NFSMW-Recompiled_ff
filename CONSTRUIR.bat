@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

rem ===========================================================================
rem  Genera la carpeta portable  build\  y comprueba que sea autonoma.
rem
rem  AHORA EMPIEZA POR LOS ARREGLOS, Y NO ES ADORNO
rem
rem  Los arreglos del juego no viven en el codigo de la app: viven en el SDK,
rem  aplicados por los scripts de tools\ sobre sus fuentes. O sea que acaban
rem  dentro de rexruntime.dll, no del .exe.
rem
rem  Antes esto compilaba solo la app y copiaba lo que hubiera. Si el SDK
rem  estaba sin parchear -recien clonado, revertido a mano, o de otra rama-,
rem  build\ salia con un rexruntime.dll SIN los arreglos y con la misma pinta
rem  que uno bueno. Ese fallo no se ve hasta que el juego se cuelga en casa de
rem  otro, que es el peor sitio para enterarse.
rem
rem  Asi que ahora se aplican los parches -son idempotentes: si ya estan, lo
rem  dicen y no tocan nada- y se recompila el SDK antes de armar la carpeta.
rem  Si todo estaba al dia, esa fase tarda segundos.
rem
rem  QUE ARREGLOS VAN DENTRO
rem    tools\parche_desatasco.py    EL DEL AUDIO Y EL CUELGUE. Cuando el hilo
rem                                 de audio del juego lleva mas de un cuarto
rem                                 de segundo girando sobre una voz que se
rem                                 quedo sin datos, se le da la senal de
rem                                 "buffer terminado" que su propio codigo
rem                                 sabe leer, y sigue.
rem    tools\parche_diagnostico.py  el aviso "Too few processor cores" salia
rem                                 mil veces por segundo y ahogaba la CPU en
rem                                 equipos de pocos nucleos. Ahora sale una
rem                                 vez. Ademas, si algo revienta, el log dice
rem                                 en que hilo del juego y con que registros.
rem    tools\parche_gpu_fallback.py si no hay grafica con Direct3D 12 nivel
rem                                 11_0, intenta WARP antes de rendirse; y si
rem                                 tampoco, sale un cuadro de dialogo en vez
rem                                 de no hacer nada al abrir.
rem    tools\parche_restaurar.py    mejoras del menu de ajustes (F4): un boton
rem                                 "Restore defaults" y deslizadores para los
rem                                 ajustes decimales con limites. No arreglan
rem                                 nada, pero probando cvars de rendimiento se
rem                                 tocan seis o siete y luego no hay forma de
rem                                 volver al punto de partida sin reiniciar.
rem    tools\parche_velocidad.py    un ajuste game_speed que multiplica la
rem                                 velocidad a la que pasa el tiempo dentro del
rem                                 juego, movible en marcha desde F4. No es un
rem                                 limite de fps: los fps son cuantas veces se
rem                                 dibuja, esto es a que ritmo avanza el juego.
rem    tools\parche_backend.py      un ajuste gpu_backend para elegir la API
rem                                 grafica: d3d12 o vulkan. El plugin ya sabia
rem                                 elegir; lo que faltaba era que alguien se lo
rem                                 dijera. Pide reiniciar para que valga.
rem                                 DX11 no esta en la lista porque este SDK no
rem                                 tiene backend de DX11, ni lo tuvo: la
rem                                 emulacion de la Xenos usa cosas de la
rem                                 generacion de DX12 -ROV, descriptores sin
rem                                 limite, escrituras tipadas desde shaders-.
rem    tools\parche_anillo.py       instrumentacion del XMA. No cambia el
rem                                 comportamiento y con el log normal no
rem                                 imprime nada, pero es lo que puso nombre y
rem                                 hora al cuelgue, y lo que hara falta si
rem                                 vuelve. Ademas el desatasco se apoya en
rem                                 sus cabeceras, asi que va por delante.
rem
rem  QUE VA A build\
rem    NFS_Most_Wanted.exe        EL LANZADOR, con el icono del juego. Es lo
rem                               que hay que abrir: saca la ventana de
rem                               opciones y desde ahi se juega.
rem    nfsmw.exe                  el juego de verdad. Se llamaba
rem                               NFS_Most_Wanted.exe hasta que el lanzador le
rem                               quito el nombre. Abrirlo a pelo funciona
rem                               igual que siempre: se busca la ISO al lado.
rem    rexruntime.dll             runtime del SDK: aqui viven los arreglos
rem    rexgpu-xenos.dll           emulacion de la GPU. Se carga con LoadLibrary
rem                               segun el cvar gpu_plugin, asi que NO aparece
rem                               en las dependencias del enlazador: hay que
rem                               copiarla a mano o la pantalla sale en negro.
rem    MSVCP140.dll               \  runtime de Visual C++. Las que el .exe y
rem    MSVCP140_ATOMIC_WAIT.dll    | rexruntime importan y que no vienen con
rem    VCRUNTIME140.dll            | Windows. Se resuelven desde el propio
rem    VCRUNTIME140_1.dll         /  toolchain, sin rutas fijas.
rem    LANZADOR.bat / lanzador.ps1
rem    nfsmw.toml  COMPARAR_VIDEO.bat  PROBAR.bat  matriz.ps1  LEEME.txt
rem
rem  LA ISO NO SE COPIA. Pesa varios GB, es tuya, y el juego la lee al vuelo.
rem  Ponla tu en build\ cuando quieras usar la carpeta.
rem
rem  Se usa SALIDA para los codigos de retorno, NUNCA "RC": esa variable la
rem  pone vcvars64 con la ruta del compilador de recursos y CMake la lee al
rem  detectar el toolchain.
rem ===========================================================================

if /i "%~1"=="__run" goto :run

if not exist "logs" mkdir "logs"

echo ============================================
echo   Build portable
echo ============================================
echo.
echo Comprueba los arreglos, recompila lo que haga falta y arma la
echo carpeta portable  build\
echo.
echo   1. Arreglos del SDK   audio, vsync, GPU, menu de F4, API grafica
echo   2. Recompilar el SDK  ahi es donde viven esos arreglos
echo   3. Compilar la app
echo   4. Armar build\
echo   5. Comprobar que la carpeta sea autonoma
echo.
echo Si ya estaba todo al dia, son unos segundos.
echo Si hay que reenlazar, el paso final son casi 50 MB y no imprime nada
echo durante varios minutos. NO CIERRES LA VENTANA.
echo.
echo Registro en logs\construir.log
echo.
pause

echo.
powershell -NoProfile -ExecutionPolicy Bypass -Command "& { & $env:ComSpec /c 'CONSTRUIR.bat __run 2>&1' | Tee-Object -FilePath 'logs\construir.log' }"

echo.
echo ============================================
pause
exit /b

rem ===========================================================================
:run
rem ===========================================================================

call "%~dp0tools\_entorno_vs.bat"
if not defined ENTORNO_OK goto fin

rem Sin Python no se pueden aplicar los parches, y sin parches la carpeta
rem saldria sin los arreglos. Mejor pararlo aqui que armar una build muda.
if not defined PY (
    echo [ERROR] No encuentro Python. Hace falta para aplicar los arreglos
    echo         del SDK, que es donde vive el del audio.
    echo         Instala Python 3 y vuelve a intentarlo.
    goto fin
)

echo ############################################
echo # 1/5  ARREGLOS DEL SDK
echo ############################################
rem Idempotentes: si ya estan puestos lo dicen y no tocan nada.
rem
rem EL ORDEN IMPORTA. parche_anillo va antes que parche_desatasco porque es
rem quien mete <atomic> y <chrono> en el fichero del kernel, y el desatasco los
rem usa. El desatasco lo comprueba y se niega a aplicarse si falta, asi que
rem como mucho esto se para aqui con un mensaje claro, no a mitad de la build.
%PY% "%~dp0tools\parche_diagnostico.py"
if errorlevel 1 (
    echo [ERROR] No se pudo aplicar el parche de diagnostico. Me detengo.
    goto fin
)
%PY% "%~dp0tools\parche_anillo.py"
if errorlevel 1 (
    echo [ERROR] No se pudo instrumentar el kernel del XMA. Me detengo.
    goto fin
)
%PY% "%~dp0tools\parche_desatasco.py"
if errorlevel 1 (
    echo [ERROR] No se pudo aplicar el desatasco del audio. Me detengo.
    echo.
    echo         Este es EL arreglo del cuelgue. Sin el, la carpeta no vale
    echo         para repartir, asi que no sigo y no toco build\
    goto fin
)
rem Vsync y limitador de fps. De fabrica NINGUNO de los dos funciona: "vsync"
rem existe como cvar pero el Present del presentador llevaba el SyncInterval
rem clavado a 0, y limitador no habia ninguno. Sin esto, esos dos ajustes del
rem lanzador no hacen nada y el propio lanzador lo avisa en rojo.
%PY% "%~dp0tools\parche_presentador.py"
if errorlevel 1 (
    echo [ERROR] No se pudo aplicar el vsync y el limite de fps. Me detengo.
    goto fin
)
%PY% "%~dp0tools\parche_gpu_fallback.py"
if errorlevel 1 (
    echo [ERROR] No se pudo aplicar el parche de GPU. Me detengo.
    goto fin
)
rem Este es de comodidad, no de correccion: el boton de restaurar del menu F4.
%PY% "%~dp0tools\parche_restaurar.py"
if errorlevel 1 (
    echo [ERROR] No se pudo mejorar el menu de ajustes. Me detengo.
    goto fin
)
%PY% "%~dp0tools\parche_velocidad.py"
if errorlevel 1 (
    echo [ERROR] No se pudo anadir el ajuste de velocidad. Me detengo.
    goto fin
)
%PY% "%~dp0tools\parche_backend.py"
if errorlevel 1 (
    echo [ERROR] No se pudo anadir el selector de API grafica. Me detengo.
    goto fin
)
rem La puerta del multijugador. Anade el ajuste grant_user_privileges, APAGADO
rem por defecto, asi que ponerlo aqui no cambia el comportamiento de nadie: solo
rem deja el interruptor disponible en F4.
%PY% "%~dp0tools\parche_privilegios.py"
if errorlevel 1 (
    echo [ERROR] No se pudo anadir el ajuste de privilegios. Me detengo.
    goto fin
)
rem Los de este fork: excepciones de coma flotante enmascaradas (sin esto el
rem juego muere con 0xC000008F antes del menu), guest_vblank_rate y el log de
rem fps del guest. Ver la cabecera de tools\parche_ff.py.
%PY% "%~dp0tools\parche_ff.py"
if errorlevel 1 (
    echo [ERROR] No se pudieron aplicar los parches del fork. Me detengo.
    goto fin
)
echo.

echo ############################################
echo # 2/5  RECOMPILAR EL SDK
echo ############################################
echo Los arreglos viven en rexruntime.dll, no en el .exe. Si el SDK ya
echo estaba compilado y no cambio nada, esto tarda segundos.
echo.
rem ---------------------------------------------------------------------------
rem  CONFIGURAR CON VULKAN ENCENDIDO
rem
rem  En Windows el SDK trae REXGLUE_USE_VULKAN en OFF, asi que el backend de
rem  Vulkan -que esta entero en src/graphics/vulkan- no se compila y el ajuste
rem  gpu_backend=vulkan no tendria nada que cargar.
rem
rem  Esto lo enciende en la cache de CMake. Es idempotente: si ya estaba, la
rem  configuracion no cambia nada y tarda segundos. LA PRIMERA VEZ NO: cambiar
rem  una opcion obliga a recompilar medio SDK, y ademas entran glslang y
rem  spirv-tools. Esa vez tarda un buen rato.
rem
rem  No hace falta instalar el SDK de Vulkan: las cabeceras, el cargador, el
rem  asignador de memoria y glslang ya vienen en thirdparty\
rem ---------------------------------------------------------------------------
pushd "%SDK%"
cmake --preset win-amd64 -DREXGLUE_USE_VULKAN=ON
set "SALIDA=!errorlevel!"
if not "!SALIDA!"=="0" (
    popd
    echo [ERROR] Fallo la configuracion del SDK con codigo !SALIDA!
    echo         Si se queja de Vulkan, se puede seguir sin el:
    echo             cmake --preset win-amd64 -DREXGLUE_USE_VULKAN=OFF
    echo         El resto de arreglos no lo necesitan.
    goto fin
)
cmake --build out/build/win-amd64 --config Release --target install
set "SALIDA=!errorlevel!"
popd
if not "!SALIDA!"=="0" (
    echo [ERROR] Fallo la compilacion del SDK con codigo !SALIDA!
    echo.
    echo Para dejar el SDK como estaba:
    echo     %PY% tools\parche_anillo.py --revertir
    echo     %PY% tools\parche_diagnostico.py --revertir
    echo     %PY% tools\parche_gpu_fallback.py --revertir
    goto fin
)
echo.

echo ############################################
echo # 3/5  COMPILAR LA APP EN RELEASE
echo ############################################
set "DIRREL=%~dp0app\out\build\win-amd64-release"
if exist "%DIRREL%\.ninja_lock" del /q "%DIRREL%\.ninja_lock" >nul 2>&1

pushd "app"

rem En un clon limpio no existe generated\rexglue.cmake, y CMakeLists.txt lo
rem incluye antes de definir el target del codegen: sin este paso la
rem configuracion falla ("include could not find requested file"). El CLI del
rem SDK lo genera junto con el C++.
if not exist "generated\rexglue.cmake" (
    echo -- Primer codegen: generated\rexglue.cmake no existe todavia --
    "%SDKBIN%\rexglue.exe" codegen nfsmw_manifest.toml
    if errorlevel 1 (
        popd
        echo [ERROR] El primer codegen fallo. Esta assets\default.xex en su sitio?
        goto fin
    )
)

cmake --preset win-amd64-release

rem ---------------------------------------------------------------------------
rem  DOS PASADAS, Y NO ES CAPRICHO
rem
rem  El codegen reescribe generated\default\nfsmw_pch.h, y de esa cabecera sale
rem  la precompilada (cmake_pch.hxx.pch) que usan los 131 ficheros generados.
rem
rem  En UNA sola pasada, ninja decide al arrancar que ficheros estan sucios.
rem  En ese momento nfsmw_pch.h todavia no ha cambiado, asi que da la PCH por
rem  buena. Luego, ya dentro de la misma pasada, el codegen la cambia. Cuando
rem  le toca el turno a los .cpp, clang compara y aborta:
rem
rem      fatal error: file 'nfsmw_pch.h' has been modified since the
rem      precompiled header was built: size changed (was 18553, now 18522)
rem
rem  Lanzando el codegen primero y por separado, la segunda pasada arranca con
rem  las cabeceras ya definitivas y recalcula bien que hay que rehacer.
rem ---------------------------------------------------------------------------
echo -- Pasada 1: codegen --
cmake --build --preset win-amd64-release --target nfsmw_codegen
set "SALIDA=!errorlevel!"
if not "!SALIDA!"=="0" (
    popd
    echo [ERROR] El codegen fallo con codigo !SALIDA!
    goto fin
)

echo.
echo -- Pasada 2: compilar --
cmake --build --preset win-amd64-release
set "SALIDA=!errorlevel!"
popd
if not "!SALIDA!"=="0" (
    echo [ERROR] Fallo la compilacion con codigo !SALIDA!
    goto fin
)
echo.

echo ############################################
echo # 4/5  ARMAR build\
echo ############################################
rem Ademas de copiar, borra los restos de ejecuciones anteriores -logs\,
rem matriz\, shaders\, cache\-, que son de ESTA maquina y no deben viajar.
pushd "app"
cmake --build --preset win-amd64-release --target dist
set "SALIDA=!errorlevel!"
popd
if not "!SALIDA!"=="0" (
    echo [ERROR] No se pudo armar build\ con codigo !SALIDA!
    goto fin
)
echo.

rem ---------------------------------------------------------------------------
rem  El lanzador, y el cambio de nombre que lo pone delante
rem
rem  El target dist deja el juego como build\NFS_Most_Wanted.exe. Esto lo
rem  renombra a nfsmw.exe y pone el lanzador en su sitio, para que hacer doble
rem  clic en el icono del juego abra la ventana de opciones. Abrir nfsmw.exe
rem  directamente sigue funcionando igual que antes.
rem
rem  Va ANTES de la comprobacion de autonomia a proposito: asi lo que se
rem  comprueba es la carpeta tal y como va a quedar, lanzador incluido.
rem
rem  Si falla, la carpeta sigue sirviendo: el juego estara como nfsmw.exe o como
rem  NFS_Most_Wanted.exe y LANZADOR.bat funciona igual. Por eso no se aborta.
echo -- Lanzador --
rem Primero el de Qt (launcher-windows\). Si no hay Qt instalado, el de
rem C#/WinForms de siempre, que no necesita nada.
call "%~dp0launcher-windows\build.bat" /silencioso
set "SALIDA=!errorlevel!"
if not "!SALIDA!"=="0" (
    echo [aviso] Sin el lanzador de Qt; construyo el de C#.
    call "%~dp0CONSTRUIR_LANZADOR.bat" /silencioso
    set "SALIDA=!errorlevel!"
)
if not "!SALIDA!"=="0" (
    echo [aviso] No se pudo construir el lanzador ^(codigo !SALIDA!^).
    echo         La carpeta sigue valiendo: se juega con LANZADOR.bat.
)
echo.

echo ############################################
echo # 5/5  COMPROBAR QUE SEA AUTONOMA
echo ############################################
rem Lee la tabla de importaciones PE de cada binario de build\ y sigue las
rem dependencias en cadena. No usa dumpbin a proposito: dumpbin viene con
rem Visual Studio, y la gracia es comprobarlo sin suponer herramientas.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\comprobar_dist.ps1"

echo.

rem ---------------------------------------------------------------------------
rem  Y de paso, las carpetas de reparto
rem
rem  Antes esto era un paso a mano: "comprime build\ sin la ISO". Se hace aqui
rem  porque es justo el momento en que build\ esta recien hecha y limpia, y
rem  porque el paso a mano tenia una trampa: la ISO son varios GB y es facil
rem  mandarla sin querer.
rem
rem  Deja dos carpetas al lado del proyecto, en "build release":
rem
rem    NFSMW Windows x64\              lista para jugar y para mandar a alguien
rem                                    que tenga SU PROPIA copia del juego
rem    NFSMW Windows x64 - Portable\   todo menos el juego; esta si se publica
rem
rem  Si no encuentra el script no pasa nada: build\ ya esta hecha y se puede
rem  comprimir a mano como siempre.
set "RELEASE=%~dp0..\build release\PREPARAR_RELEASE.bat"
if exist "%RELEASE%" (
    echo ############################################
    echo # EXTRA  CARPETAS DE REPARTO
    echo ############################################
    call "%RELEASE%" /silencioso
    if errorlevel 1 (
        echo [aviso] No se pudieron armar las carpetas de reparto.
        echo         build\ esta bien; comprimela a mano si hace falta.
    )
    echo.
)

echo ============================================
echo   LISTO
echo ============================================
echo.
echo build\ esta rehecha, con los arreglos dentro y sin rastro de
echo ejecuciones anteriores.
echo.
echo PARA JUGAR TU
echo   Copia tu ISO en build\ y doble clic en NFS_Most_Wanted.exe
echo.
echo   Ese es el LANZADOR, con el icono del juego: abre la ventana de
echo   opciones y desde ahi se juega. El juego de verdad es nfsmw.exe, y
echo   normalmente no hay que tocarlo. LANZADOR.bat sigue haciendo lo mismo.
echo.
echo PARA MANDARSELA A ALGUIEN
echo   Ya esta hecha, en  ..\build release\NFSMW Windows x64\
echo   Sin la ISO dentro. Comprimela y mandala.
echo.
echo   El tiene que poner SU PROPIA ISO, y del MISMO default.xex. Esto no
echo   es un emulador: el .exe lleva dentro el codigo de esa ISO concreta,
echo   traducido y compilado. Con una ROM de otra region no arranca, y ya
echo   nos costo un dia averiguarlo la primera vez.
echo.

:fin
exit /b
