#!/usr/bin/env python3
"""
Parches de este fork sobre el SDK (ReXGlue v0.10.0).

    python tools/parche_ff.py            aplicar
    python tools/parche_ff.py --estado
    python tools/parche_ff.py --revertir

Como los demas parche_*.py: sustitucion de texto exacta, bloque a bloque.
Un bloque solo se aplica si su anclaje aparece UNA vez; si ya esta puesto lo
dice y no toca nada; --revertir deja el texto original.

QUE LLEVA
=========

1. include/rex/platform/fpscr.h - excepciones de coma flotante SIEMPRE
   enmascaradas.

   Un contexto del guest cuyo csr nunca paso por InitHost vale 0. El primer
   enableFlushMode() hacia entonces setcsr(FlushMask): MXCSR con TODAS las
   excepciones desenmascaradas, y el siguiente divss inexacto mataba el
   proceso con STATUS_FLOAT_INEXACT_RESULT (0xC000008F). Pasaba al arrancar,
   antes del menu. Ahora setcsr fuerza siempre los bits de mascara.

   OJO: es una cabecera que el codigo generado incluye inline, asi que tras
   aplicarlo hay que recompilar el SDK Y el juego.

2. src/graphics/graphics_system.cpp - cvar guest_vblank_rate.

   Fija cada cuanto recibe el juego un vblank (0 = la frecuencia del modo de
   video), igual que framerate_limit en Xenia Canary. MEDIDO: NFS Most Wanted
   se limita el solo a 30 fps en intro y menus aunque reciba 120 o 1000
   vblanks por segundo, asi que en esas pantallas no cambia nada. Se deja
   porque es inocuo con 0 y sirve para probar otros tramos del juego.

3. src/graphics/command_processor.cpp - cvar log_guest_fps.

   Una linea cada 10 s en el log con cuantas veces presenta el juego por
   segundo. Es la unica medida que dice el ritmo real del guest; el contador
   de F3 mide el del host.
"""

import os
import sys

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

BLOQUES = [
    (
        "include/rex/platform/fpscr.h",
        "fpscr: excepciones enmascaradas",
        "  static inline void setcsr(u32 csr) noexcept { simde_mm_setcsr(csr); }\n",
        "  // PARCHE LOCAL - FP exceptions always masked. A guest context whose csr was\n"
        "  // never seeded by InitHost (csr == 0) would otherwise unmask every x87/SSE\n"
        "  // exception on the first enableFlushMode(), and the next inexact divss\n"
        "  // kills the process with STATUS_FLOAT_INEXACT_RESULT (0xC000008F).\n"
        "  static inline void setcsr(u32 csr) noexcept { simde_mm_setcsr(csr | ExceptionMask); }\n",
    ),
    (
        "src/graphics/graphics_system.cpp",
        "guest_vblank_rate: el cvar",
        'REXCVAR_DEFINE_BOOL(store_shaders, true, "GPU",\n',
        "// PARCHE LOCAL - ritmo del vblank del guest\n"
        "//\n"
        "// Lo mismo que framerate_limit en Xenia Canary: alli ese cvar no limita los\n"
        "// fps del host, fija cada cuanto se le da un vblank al juego. Un juego que\n"
        "// presenta cada 2 vblanks -30 fps en una Xbox a 60 Hz- va a 60 fps con 120.\n"
        "// 0 = la frecuencia del modo de video, como siempre.\n"
        'REXCVAR_DEFINE_INT32(guest_vblank_rate, 0, "GPU",\n'
        '                     "Guest vblank rate in Hz while vsync is on (0 = video mode refresh rate). "\n'
        '                     "A game locked to 30 fps waits two vblanks per frame, so 120 makes it "\n'
        '                     "run at 60 fps. Same as framerate_limit in Xenia Canary.")\n'
        "    .range(0, 1000)\n"
        "    .lifecycle(rex::cvar::Lifecycle::kHotReload);\n"
        "\n"
        'REXCVAR_DEFINE_BOOL(store_shaders, true, "GPU",\n',
    ),
    (
        "src/graphics/graphics_system.cpp",
        "guest_vblank_rate: el hilo del vblank",
        "        uint64_t vsync_interval_ticks =\n"
        "            std::max(uint64_t(1), uint64_t(double(guest_tick_frequency) / refresh_rate_hz));\n"
        "        uint64_t no_vsync_interval_ticks = std::max(uint64_t(1), guest_tick_frequency / 1000);\n"
        "        uint64_t last_frame_time = chrono::Clock::QueryGuestTickCount();\n"
        "        while (vsync_worker_running_) {\n"
        "          uint64_t current_time = chrono::Clock::QueryGuestTickCount();\n"
        "          uint64_t interval_ticks =\n",
        "        uint64_t no_vsync_interval_ticks = std::max(uint64_t(1), guest_tick_frequency / 1000);\n"
        "        uint64_t last_frame_time = chrono::Clock::QueryGuestTickCount();\n"
        "        while (vsync_worker_running_) {\n"
        "          uint64_t current_time = chrono::Clock::QueryGuestTickCount();\n"
        "          // Se relee en cada vuelta: guest_vblank_rate se puede cambiar en marcha.\n"
        "          int32_t vblank_rate = REXCVAR_GET(guest_vblank_rate);\n"
        "          double vblank_hz = vblank_rate > 0 ? double(vblank_rate) : refresh_rate_hz;\n"
        "          uint64_t vsync_interval_ticks =\n"
        "              std::max(uint64_t(1), uint64_t(double(guest_tick_frequency) / vblank_hz));\n"
        "          uint64_t interval_ticks =\n",
    ),
    (
        "src/graphics/command_processor.cpp",
        "log_guest_fps: el cvar",
        'REXCVAR_DEFINE_BOOL(vsync, true, "GPU", "Enable vertical sync");\n',
        'REXCVAR_DEFINE_BOOL(vsync, true, "GPU", "Enable vertical sync");\n'
        "\n"
        'REXCVAR_DEFINE_BOOL(log_guest_fps, false, "GPU",\n'
        '                    "Log how many frames per second the game presents, every 10 seconds");\n',
    ),
    (
        "src/graphics/command_processor.cpp",
        "log_guest_fps: la medida",
        "  IssueSwap(frontbuffer_ptr, frontbuffer_width, frontbuffer_height);\n"
        "\n"
        "  ++counter_;\n",
        "  IssueSwap(frontbuffer_ptr, frontbuffer_width, frontbuffer_height);\n"
        "\n"
        "  // PARCHE LOCAL - fps del guest en el log\n"
        "  //\n"
        "  // Cuantas veces presenta el juego por segundo, medido aqui y no en el host:\n"
        "  // es lo unico que dice si guest_vblank_rate cambio de verdad el ritmo del\n"
        "  // juego. Una linea cada 10 s, solo con log_guest_fps.\n"
        "  if (REXCVAR_GET(log_guest_fps)) {\n"
        "    static uint64_t window_start = 0;\n"
        "    static uint32_t swaps = 0;\n"
        "    const uint64_t now = rex::chrono::Clock::QueryHostTickCount();\n"
        "    const uint64_t freq = rex::chrono::Clock::QueryHostTickFrequency();\n"
        "    if (!window_start) {\n"
        "      window_start = now;\n"
        "    }\n"
        "    ++swaps;\n"
        "    if (now - window_start >= freq * 10) {\n"
        '      REXGPU_INFO("[guest fps] {:.1f} swaps/s", swaps * double(freq) / double(now - window_start));\n'
        "      window_start = now;\n"
        "      swaps = 0;\n"
        "    }\n"
        "  }\n"
        "\n"
        "  ++counter_;\n",
    ),
]


def buscar_sdk():
    for c in (os.path.join(os.path.dirname(RAIZ), "rexglue-sdk"), os.path.join(RAIZ, "sdk")):
        if os.path.isdir(os.path.join(c, "src")):
            return c
    return None


def leer(ruta):
    with open(ruta, "r", encoding="utf-8", newline="") as fh:
        return fh.read()


def escribir(ruta, texto):
    with open(ruta, "w", encoding="utf-8", newline="") as fh:
        fh.write(texto)


def normalizar(texto, muestra):
    # Los fuentes del SDK pueden venir con CRLF segun core.autocrlf.
    return texto.replace("\n", "\r\n") if "\r\n" in muestra else texto


def main():
    modo = sys.argv[1] if len(sys.argv) > 1 else "--aplicar"
    sdk = buscar_sdk()
    if not sdk:
        print("[ERROR] No encuentro el SDK en ..\\rexglue-sdk ni en .\\sdk")
        return 1

    cambios = {}
    fallos = 0
    for rel, nombre, viejo, nuevo in BLOQUES:
        ruta = os.path.join(sdk, rel)
        texto = cambios.get(ruta) or leer(ruta)
        v, n = normalizar(viejo, texto), normalizar(nuevo, texto)
        puesto = texto.count(n) == 1
        limpio = texto.count(v) == 1 and not puesto

        if modo == "--estado":
            print(("[ok] aplicado    " if puesto else "[--] sin aplicar ") + nombre)
            continue
        if modo == "--revertir":
            if puesto:
                cambios[ruta] = texto.replace(n, v, 1)
                print("[ok] Revertido: " + nombre)
            else:
                print("[--] No estaba: " + nombre)
            continue
        if puesto:
            print("[ok] Ya estaba: " + nombre)
        elif limpio:
            cambios[ruta] = texto.replace(v, n, 1)
            print("[ok] Aplicado: " + nombre)
        else:
            print("[ERROR] El anclaje no aparece una sola vez: %s (%s)" % (nombre, rel))
            fallos += 1

    if fallos:
        print("No se ha tocado nada.")
        return 1
    for ruta, texto in cambios.items():
        escribir(ruta, texto)
    if cambios and modo != "--estado":
        print()
        print("  HAY QUE RECOMPILAR EL SDK Y EL JUEGO (fpscr.h va inline en el codigo generado).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
