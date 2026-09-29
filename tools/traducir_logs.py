#!/usr/bin/env python3
"""
Translates the Spanish log messages and setting descriptions to English.

    python tools/traducir_logs.py            apply (SDK working tree, patch scripts, app)
    python tools/traducir_logs.py --check    list what is still Spanish

One-off tool: the log is read by people who do not speak Spanish. It replaces
exact fragments of string literals (never code), in:
  - the SDK working tree (already patched),
  - every tools/parche_*.py (the "new" texts, so a clean rebuild gives the
    same result), tools/sdk_nuevos/,
  - app/src.
Comments stay as they are; they never reach the log.
"""

import os
import sys

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SDK = os.path.join(os.path.dirname(RAIZ), "rexglue-sdk")

T = [
    # --- tools/parche_ff.py (frame pacing, occlusion, textures, latency) ---
    ('"shader", "primitivas", "render targets", "pipeline", "texturas",',
     '"shader", "primitives", "render targets", "pipeline", "textures",'),
    ('"bindings", "vertex buffers", "resolve", "espera GPU", "submit",',
     '"bindings", "vertex buffers", "resolve", "GPU wait", "submit",'),
    ('"de ellas crear textura", "de ellas subir memoria"};',
     '"of which texture creation", "of which memory upload"};'),
    ("[latencia] VdSwap -> salida {:.1f} ms media ({:.1f}-{:.1f}) | espera del ritmo ",
     "[latency] VdSwap -> output {:.1f} ms avg ({:.1f}-{:.1f}) | pacing wait "),
    ("aqui {:.1f} ms | suelta -> salida {:.1f} ms (suavizado a {:.1f}) | inicio del ",
     "here {:.1f} ms | release -> output {:.1f} ms (smoothed to {:.1f}) | frame "),
    ("fotograma -> salida {:.1f} ms media, {:.1f} max",
     "start -> output {:.1f} ms avg, {:.1f} max"),
    ("[frame lento] ... y {} mas sin apuntar", "[slow frame] ... and {} more not logged"),
    ("[frame lento] {:.1f} ms, tarde {:.1f} | juego sin comandos {:.1f} | ",
     "[slow frame] {:.1f} ms, late {:.1f} | no game commands {:.1f} | "),
    ("pacer durmio {:.1f} +{:.1f} | resto (CP trabajando) {:.1f}",
     "pacer slept {:.1f} +{:.1f} | rest (CP working) {:.1f}"),
    ("[frame lento]   trabajo:{}", "[slow frame]   work:{}"),
    (" (nada medible)", " (nothing measurable)"),
    ("[occlusion] eventos {} | sin recursos {} | informes {} | entregados {} | con espera {} ",
     "[occlusion] events {} | no resources {} | reports {} | delivered {} | waited {} "),
    ("| en cola {} | max muestras por intervalo {} | recursos {}",
     "| queued {} | max samples per interval {} | resources {}"),
    ('resources ? "si" : "no"', 'resources ? "yes" : "no"'),
    ("[texturas] no se pudo crear un heap de {} MB; se sigue con recursos propios",
     "[textures] could not create a {} MB heap; one resource per texture from now on"),
    ("[texturas] {} en cache, {} MB (limites {}/{} MB) | creadas {} en {:.1f} ms ({:.3f} ms c/u) | ",
     "[textures] {} cached, {} MB (limits {}/{} MB) | created {} in {:.1f} ms ({:.3f} ms each) | "),
    ('"descartadas {}",', '"discarded {}",'),
    ("[pacing] refresco del monitor {:.4f} Hz", "[pacing] display refresh {:.4f} Hz"),
    ("[pacing] sin DwmGetCompositionTimingInfo: ritmo con reloj propio",
     "[pacing] no DwmGetCompositionTimingInfo: pacing with our own clock"),
    ("[present] {:.1f}/s | intervalo {:.1f}-{:.1f} ms | vsync {} | imagen -> Present ",
     "[present] {:.1f}/s | interval {:.1f}-{:.1f} ms | vsync {} | image -> Present "),
    ("{:.2f} ms media, {:.2f} max", "{:.2f} ms avg, {:.2f} max"),
    ("see [latencia] in the log", "see [latency] in the log"),
    ("Mientras el juego entregue imagenes, repintar la UI solo ",
     "While the game delivers frames, repaint the UI only "),
    ("con ellas (un present por fotograma del juego)", "with them (one present per game frame)"),
    # --- other patch scripts ---
    ("Limite de fotogramas por segundo (0 = sin limite)", "Frames per second limit (0 = unlimited)"),
    ("[guest] ENTREGA BUFFER 0: ctx={:08X} datos={:08X} paquetes={}",
     "[guest] SUBMITS BUFFER 0: ctx={:08X} data={:08X} packets={}"),
    ("[guest] ENTREGA BUFFER 1: ctx={:08X} datos={:08X} paquetes={}",
     "[guest] SUBMITS BUFFER 1: ctx={:08X} data={:08X} packets={}"),
    ("[guest] DA ENTRADA 0: ctx={:08X}", "[guest] GIVES INPUT 0: ctx={:08X}"),
    ("[guest] DA ENTRADA 1: ctx={:08X}", "[guest] GIVES INPUT 1: ctx={:08X}"),
    ("[guest] revalida la salida: ctx={:08X}", "[guest] revalidates output: ctx={:08X}"),
    ("[guest] mueve la lectura: ctx={:08X} {} -> {}", "[guest] moves read offset: ctx={:08X} {} -> {}"),
    ("[desatasco] ctx={:08X} lleva {} ms girando sin entrada ",
     "[unstall] ctx={:08X} has been spinning for {} ms without input "),
    ("(escritura={} lectura={}). Le digo que el buffer esta terminado.",
     "(write={} read={}). Telling it the buffer is finished."),
    ("[guest] apaga el contexto: ctx={:08X} esperar={}", "[guest] disables context: ctx={:08X} wait={}"),
    ("[guest] pregunta entrada 0: ctx={:08X} -> {}", "[guest] queries input 0: ctx={:08X} -> {}"),
    ("[guest] pregunta entrada 1: ctx={:08X} -> {}", "[guest] queries input 1: ctx={:08X} -> {}"),
    ("[guest] pregunta si la salida vale: ctx={:08X} -> {}",
     "[guest] asks whether output is valid: ctx={:08X} -> {}"),
    ("[guest] pide lectura: ctx={:08X} lectura={} escritura={} valida={} ent0={} ent1={}",
     "[guest] requests read: ctx={:08X} read={} write={} valid={} in0={} in1={}"),
    ("[guest] pide escritura: ctx={:08X} escritura={} lectura={} valida={} ent0={} ent1={}",
     "[guest] requests write: ctx={:08X} write={} read={} valid={} in0={} in1={}"),
    ("[guest] enciende el contexto: ctx={:08X}", "[guest] enables context: ctx={:08X}"),
    # No-GPU error dialog (d3d12_provider.cpp, parche_gpu_fallback.py)
    ("No se ha encontrado ninguna tarjeta grafica compatible.\\n",
     "No compatible graphics card was found.\\n"),
    ("Hace falta Direct3D 12 con feature level 11_0. Eso lo\\n",
     "Direct3D 12 with feature level 11_0 is required. Almost\\n"),
    ("cumple practicamente cualquier GPU de 2012 en adelante,\\n",
     "any GPU from 2012 onwards has it, so the most likely\\n"),
    ("asi que lo mas probable es que el problema sea el driver.\\n",
     "cause is the graphics driver.\\n"),
    ("Que probar, por orden:\\n", "What to try, in order:\\n"),
    ("  1. Actualizar el driver de la tarjeta grafica.\\n", "  1. Update the graphics card driver.\\n"),
    ("  2. Comprobar que Windows esta al dia.\\n", "  2. Make sure Windows is up to date.\\n"),
    ("  3. Si es un portatil con dos graficas, forzar que el\\n",
     "  3. On a laptop with two GPUs, make the game use\\n"),
    ("     juego use la dedicada.\\n", "     the dedicated one.\\n"),
    ("Hay mas detalle en la carpeta logs, junto al ejecutable.",
     "More details are in the logs folder next to the executable."),
    ("[velocidad] el tiempo del juego pasa al {:.0f}% (escala x{:.3f})",
     "[speed] in-game time runs at {:.0f}% (scale x{:.3f})"),
    ("  [hilo guest] no hay XThread en este hilo: el fallo NO viene de ",
     "  [guest thread] no XThread on this thread: the fault does NOT come from "),
    ("codigo del juego, sino del propio runtime.", "game code but from the runtime itself."),
    ("  [hilo guest] id=0x{:X} entrada=0x{:08X} contexto=0x{:08X} ",
     "  [guest thread] id=0x{:X} entry=0x{:08X} context=0x{:08X} "),
    ("trampolin_xapi=0x{:08X} principal={} creado_por_el_juego={}",
     "xapi_trampoline=0x{:08X} main={} created_by_game={}"),
    ("  [bloques del hilo] pcr=0x{:08X} tls=0x{:08X}", "  [thread blocks] pcr=0x{:08X} tls=0x{:08X}"),
    ("  [contexto ppc] r13=", "  [ppc context] r13="),
    ("ultimo_salto_indirecto=0x{:08X} r1=0x{:08X}", "last_indirect_jump=0x{:08X} r1=0x{:08X}"),
    ("  [registros] r3=", "  [registers] r3="),
    ("  [contenido del pcr] +0x000=", "  [pcr contents] +0x000="),
    ("[hilo guest] arrancando: entrada=0x{:08X} start_address=0x{:08X} ",
     "[guest thread] starting: entry=0x{:08X} start_address=0x{:08X} "),
    ("contexto=0x{:08X} trampolin_xapi=0x{:08X} pila={} bytes | ",
     "context=0x{:08X} xapi_trampoline=0x{:08X} stack={} bytes | "),
    (" (este aviso solo se muestra una vez por ejecucion)", " (this warning is only shown once per run)"),
    ("No hay ninguna GPU fisica con Direct3D 12 feature level 11_0. ",
     "No physical GPU with Direct3D 12 feature level 11_0. "),
    ("Probando el rasterizador por software (WARP).", "Trying the software rasterizer (WARP)."),
    ("Usando WARP: el render lo hace la CPU. Va a ir MUY lento -unos ",
     "Using WARP: the CPU does the rendering. It will be VERY slow - a "),
    ("pocos fotogramas por segundo- pero el juego arranca y se ve. ",
     "few frames per second - but the game starts and shows. "),
    ("Actualiza el driver de la GPU para volver a la aceleracion por ",
     "Update the GPU driver to get hardware "),
    ('"hardware.");', '"acceleration back.");'),
    ("Adaptador elegido: WARP (rasterizador por software)", "Adapter chosen: WARP (software rasterizer)"),
    ("[ajustes] foto de partida: {} valores", "[settings] startup snapshot: {} values"),
    ("[ajustes] reiniciando para aplicar: {}", "[settings] restarting to apply: {}"),
    ("[ajustes] no se pudo relanzar el juego (error {}). Cierralo y abrelo tu.",
     "[settings] could not relaunch the game (error {}). Close it and start it again."),
    ("[ajustes] {} de {} valores devueltos a la configuracion de partida",
     "[settings] {} of {} values restored to the startup configuration"),
    ("API grafica pedida: {}", "Requested graphics API: {}"),
    ("La API grafica '{}' no esta compilada en esta copia. Se prueba con la otra.",
     "Graphics API '{}' is not built into this copy. Trying the other one."),
    ("Arrancando con '{}' en su lugar. Cambia gpu_backend para quitar el aviso.",
     "Starting with '{}' instead. Change gpu_backend to silence this warning."),
    # --- app/src ---
    ("[black-edition] sin kernel de memoria; no se puede parchear.",
     "[black-edition] no kernel memory; cannot patch."),
    ("[black-edition] no se pudo traducir 0x{:08X}.", "[black-edition] could not translate 0x{:08X}."),
    ("[unlock-all] sin kernel de memoria; no se puede parchear.",
     "[unlock-all] no kernel memory; cannot patch."),
    ("[unlock-all] no se pudo traducir 0x{:08X}.", "[unlock-all] could not translate 0x{:08X}."),
    ('"contenido desbloqueado" : "contenido oculto"', '"content unlocked" : "content hidden"'),
    ('"todo desbloqueado" : "progreso normal"', '"everything unlocked" : "normal progress"'),
    ("[black-edition] desactivado (black_edition=false).", "[black-edition] off (black_edition=false)."),
    ("[temporizador] resolucion {:.2f} ms", "[timer] resolution {:.2f} ms"),
    ("[temporizador] no se pudo subir la resolucion; Sleep(1) puede durar 15 ms",
     "[timer] could not raise the resolution; Sleep(1) may take 15 ms"),
    ("Ajuste '{}' no registrado todavia; no lo toco.", "Setting '{}' not registered yet; leaving it alone."),
    ("Ajuste por defecto de la build portable: {} = {}", "Portable build default: {} = {}"),
    ("[vigilante]   hilo id=0x{:X} entrada=0x{:08X} principal={} corriendo={} | ",
     "[watchdog]   thread id=0x{:X} entry=0x{:08X} main={} running={} | "),
    ("ultimo_indirecto=0x{:08X}", "last_indirect=0x{:08X}"),
    ("[vigilante]   hilo id=0x{:X} entrada=0x{:08X} sin contexto",
     "[watchdog]   thread id=0x{:X} entry=0x{:08X} no context"),
    ("[vigilante] instantanea: {} hilos del juego", "[watchdog] snapshot: {} game threads"),
    ("[vigilante] el juego ha vuelto a moverse despues de {} s parado.",
     "[watchdog] the game is moving again after {} s stalled."),
    ("[vigilante] {} s sin que se mueva ni un registro en ninguno de los {} hilos ",
     "[watchdog] {} s without a single register changing in any of the {} game "),
    ("del juego. Esto no es lentitud: esta parado.", "threads. This is not slowness: it is stuck."),
    ("[menu] no se pudo relanzar el juego (ShellExecuteW = {}); sigue con ",
     "[menu] could not relaunch the game (ShellExecuteW = {}); keeping "),
    ("lo aplicado y reinicia a mano.", "what was applied, restart it manually."),
    ("[menu] sin ruta del ejecutable; reinicia el juego a mano.",
     "[menu] no executable path; restart the game manually."),
    ("[menu] reinicia el juego a mano para aplicar los cambios.",
     "[menu] restart the game manually to apply the changes."),
    ("Abrir/cerrar menu de ajustes del juego", "Open/close the game settings menu"),
    ("Contenido Black Edition: coches de pago como descargables",
     "Black Edition content: the paid cars as downloadable content"),
    ("Desbloquearlo todo: coches, piezas, eventos y circuitos ocultos",
     "Unlock everything: cars, parts, events and hidden tracks"),
    ('"Contenido",', '"Content",'),
]


def objetivos():
    for base in (os.path.join(SDK, "src"), os.path.join(SDK, "include"),
                 os.path.join(RAIZ, "app", "src"), os.path.join(RAIZ, "tools", "sdk_nuevos")):
        for d, _, ficheros in os.walk(base):
            for f in ficheros:
                if f.endswith((".cpp", ".h", ".hpp", ".inl")):
                    yield os.path.join(d, f)
    herramientas = os.path.join(RAIZ, "tools")
    for f in sorted(os.listdir(herramientas)):
        if f.startswith("parche_") and f.endswith(".py"):
            yield os.path.join(herramientas, f)


def main():
    comprobar = "--check" in sys.argv
    usados = set()
    for ruta in objetivos():
        with open(ruta, encoding="utf-8", errors="surrogateescape", newline="") as fh:
            texto = fh.read()
        nuevo = texto
        for es, en in T:
            # Inside the patch scripts C++ escapes can be escaped once more
            # ("\\n" for "\n"): try that spelling too.
            variantes = [(es, en)]
            if ruta.endswith(".py") and "\\n" in es:
                variantes.append((es.replace("\\n", "\\\\n"), en.replace("\\n", "\\\\n")))
            for es_v, en_v in variantes:
                if es_v in nuevo:
                    usados.add(es)
                    if not comprobar:
                        nuevo = nuevo.replace(es_v, en_v)
        if nuevo != texto:
            with open(ruta, "w", encoding="utf-8", errors="surrogateescape", newline="") as fh:
                fh.write(nuevo)
            print("translated:", os.path.relpath(ruta, os.path.dirname(RAIZ)))
    for es, _ in T:
        if es not in usados:
            print("[not found]", es)
    return 0


if __name__ == "__main__":
    sys.exit(main())
