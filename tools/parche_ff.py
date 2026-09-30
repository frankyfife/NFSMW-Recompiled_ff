#!/usr/bin/env python3
"""
Parches de este fork sobre el SDK (ReXGlue v0.10.0).

    python tools/parche_ff.py            aplicar
    python tools/parche_ff.py --estado
    python tools/parche_ff.py --revertir

Como los demas parche_*.py: sustitucion de texto exacta, bloque a bloque. Un
bloque solo se aplica si su anclaje aparece UNA vez; si ya esta puesto lo dice
y no toca nada; --revertir deja el texto original. Los bloques se generan
desde el diff del SDK parcheado (con contexto hasta que el anclaje es unico),
por eso llevan nombres de fichero y numero y no de funcion.

QUE LLEVA (el porque esta en los comentarios PARCHE LOCAL del propio codigo)
==========

include/rex/platform/fpscr.h
    Excepciones de coma flotante SIEMPRE enmascaradas. Sin esto el juego muere
    con 0xC000008F (STATUS_FLOAT_INEXACT_RESULT) antes del menu. Es una
    cabecera que el codigo generado incluye inline: tras aplicarlo hay que
    recompilar el SDK Y el juego.

src/graphics/graphics_system.cpp
    guest_vblank_rate: cada cuanto recibe el juego un vblank, con o sin vsync
    (como framerate_limit en Xenia Canary).

src/graphics/command_processor.cpp
    frame_pacing_fps: los flips salen a un ritmo exacto (30 o 60) medido con
    un reloj preciso. Con guest_vblank_rate alto el juego no pierde ranuras
    de vblank y el ritmo lo marca esto: 33,1-33,5 ms por fotograma a 30.
    WAIT_REG_MEM: espera activa los 2 primeros ms; antes, con vsync, cada
    vuelta dormia wait/0x100 ms.
    El reloj de ese ritmo va enganchado al refresco real del monitor
    (DwmGetCompositionTimingInfo): un "120 Hz" midio 120,2429 Hz y con un reloj
    propio de 60,000 fps habia un tiron cada 8,3 s.
    Ese enganche solo con vsync (frame_pacing_display_lock = 1): con
    G-Sync/FreeSync el refresco que da el compositor sigue a nuestros propios
    presents y engancharse a el meneaba el reloj medio refresco cada segundo.
    log_guest_fps: fps, abstand minimo/maximo y fotogramas lentos en el log.

include/rex/graphics/d3d12/command_processor.h
src/graphics/d3d12/command_processor.cpp
    ZPD (occlusion queries) como contador continuo, la idea de Xenia Edge
    (3d233a5): la Xenos no tiene consultas con principio y fin, cada
    EVENT_WRITE_ZPD vuelca un contador de muestras que no para y el D3D del
    juego resta dos volcados. MEDIDO: ~370 eventos/s conduciendo y ninguno
    casaba con el modelo anterior (principio y fin en la misma direccion):
    todo acababa en "1000 muestras visibles" y el sol se veia a traves de los
    edificios. Ahora cada evento cierra el intervalo desde el anterior, lo
    mide en la GPU del host (partido en segmentos si cruza submissions) y
    escribe el contador en orden cuando la GPU lo tiene.
    Con log_guest_fps, cuentas cada 10 s ([occlusion]).

    Tambien: cuando el procesador de comandos se queda esperando se entregan
    los informes pendientes esperando a la GPU, para que un guest que espera
    un resultado no se quede colgado.

src/kernel/xboxkrnl/xboxkrnl_video.cpp
    Con log_guest_fps, el ritmo de VdSwap visto desde el guest (diagnostico).

include/rex/ui/presenter.h
src/ui/presenter.cpp
    present_ui_with_guest_frames: un present por fotograma del juego. La
    notificacion de logros es un dialogo de ImGui registrado siempre, y con
    cualquier dialogo el hilo de UI repintaba en cada vblank ADEMAS de en cada
    fotograma: MEDIDO 180 presents/s para 60 imagenes en un monitor de 120 Hz.
    Eso deja a G-Sync/FreeSync fuera de rango (tearing aunque este activo) y
    con vsync reparte las imagenes entre refrescos a trompicones. Ahora la UI
    se repinta con las imagenes del juego y solo vuelve a su ritmo propio si
    el juego deja de entregar durante 100 ms (cargas).
    Va DESPUES de parche_presentador.py: el bloque de d3d12_presenter.cpp se
    ancla en su Present.

src/ui/d3d12/d3d12_presenter.cpp
    Con log_guest_fps, presents por segundo en el log ([present]).

src/graphics/pipeline/texture/cache.cpp
    Con log_guest_fps, estado del cache de texturas cada 10 s ([texturas]:
    cuantas, MB, creadas, descartadas) y dos apartados mas en el desglose de
    los fotogramas lentos (crear textura, subir memoria). MEDIDO conduciendo:
    los tirones que quedaban eran "texturas 9,5-12,8 ms" en un solo
    fotograma. Los limites por defecto (384/768 MB) los sube el lanzador.

include/rex/graphics/d3d12/texture_cache.h
src/graphics/d3d12/texture_cache.cpp
    d3d12_texture_heaps: las texturas se colocan en heaps compartidos de
    64 MB (CreatePlacedResource) en vez de un CreateCommittedResource cada
    una. MEDIDO: 0,30-0,37 ms por textura antes, 0,04-0,05 ms despues;
    conduciendo llegaban 50-60 texturas nuevas en un fotograma (11 ms).
    El cache nunca descartaba nada: el coste era solo crear.
    Y log_frame_breakdown: el desglose por apartados cuesta 2-3 ms por
    fotograma con 7000 draws, asi que ya no va con log_guest_fps solo.

include/rex/graphics/frame_pacer.h  (fichero nuevo, tools/sdk_nuevos/)
src/kernel/xboxkrnl/xboxkrnl_video.cpp
src/graphics/command_processor.cpp
    LATENCIA. frame_pacing_at_guest: el ritmo se marca en VdSwap, en el hilo
    del juego, como en la Xbox, y no justo antes de presentar. Antes el
    juego se adelantaba y el anillo guardaba ~3 fotogramas. VdSwap anade al
    paquete XE_SWAP el momento de la entrega y el de la suelta. MEDIDO en el
    menu, desde el VdSwap hasta la salida: 49,8 ms antes, 19,1 ms ahora.
    frame_pacing_smooth_max_ms: cada fotograma se presenta a la misma
    distancia de su suelta (el maximo reciente, ~2,5 ms), porque sin eso los
    presents iban cada 14,9-19,1 ms; con eso 16,3-17,1 como antes.
    frame_pacing_low_latency (la idea de Reflex): VdSwap no suelta al juego
    hasta que el fotograma anterior ha salido (el plugin de GPU exporta
    rex_gpu_last_output_vdswap_ticks). A 120 fps en escenas cargadas la
    emulacion de la GPU es el cuello de botella y los fotogramas volvian a
    esperar 15-21 ms en el anillo. MEDIDO sin limite de fps en el menu, del
    inicio del fotograma a la salida: 6,0 ms sin esto, 3,1 ms con esto, los
    mismos 652 fps.
    frame_pacing_adaptive: si la escena no llega al objetivo, el ritmo apunta
    al percentil 90 de lo que cuestan de verdad los ultimos 90 fotogramas.
    MEDIDO con CapFrameX a 120 fps en la ciudad: 75-117 fps alternando
    fotogramas de 8 y 15 ms. En el menu con objetivo 1000: 262 fps con
    intervalos de 3,3-4,3 ms.

src/ui/d3d12/d3d12_presenter.cpp
    frame_times_dir / frame_times_recording: una fila por present (momento
    del present y, de las estadisticas de DXGI, cuando llego a la pantalla),
    un CSV por grabacion; la app lo alterna con F10. PresentMon necesita
    permisos de administrador y el de FrameView sale sin decir nada.

include/rex/graphics/shared_memory.h
src/graphics/shared_memory.cpp
    gpu_hot_pages: paginas que el juego reescribe en cada fotograma dejan de
    estar protegidas tras 3 fallos de escritura; cada peticion las compara
    con una copia (memcmp de 4 KB) y solo las sube si cambiaron. Las que
    tienen texturas vigiladas o datos escritos por la GPU nunca pasan a
    calientes. MEDIDO con el perfilador (tools/cpuprof) en el hilo
    "GPU Commands" conduciendo: NtProtectVirtualMemory 15,1 % -> 1,0 %.
    RequestRanges y el filtro reutilizan vectores en vez de reservar uno por
    draw (RtlAllocateHeap 2,1 %).

src/graphics/command_processor.cpp
src/graphics/d3d12/command_processor.cpp
    Paquetes Type0 de varios registros seguidos van por
    WriteRegisterRangeFromRing, y WriteRegistersFromMem parte los rangos
    mezclados en tramos: registros normales con una copia, constantes por su
    camino rapido y solo los especiales (SCRATCH, COHER_STATUS_HOST, DC_LUT,
    ...) uno a uno. MEDIDO: WriteRegister 10,4 % -> 0,6 % del hilo; conduciendo
    a 120 fps ~90 -> 95-110 fps (CapFrameX, sin contar la pantalla de carga).

Este fichero lo genera tools/generar_parche_ff.py: la linea base es el SDK
de git con los demas parche_*.py aplicados en el orden de CONSTRUIR.bat.
"""

import os
import sys

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

BLOQUES = [
    ('include/rex/platform/fpscr.h',
     'platform/fpscr.h #1',
     '\n  static inline void setcsr(u32 csr) noexcept { simde_mm_setcsr(csr); }\n\n',
     '\n  // PARCHE LOCAL - FP exceptions always masked. A guest context whose csr was\n  // never seeded by InitHost (csr == 0) would otherwise unmask every x87/SSE\n  // exception on the first enableFlushMode(), and the next inexact divss\n  // kills the process with STATUS_FLOAT_INEXACT_RESULT (0xC000008F).\n  static inline void setcsr(u32 csr) noexcept { simde_mm_setcsr(csr | ExceptionMask); }\n\n'),
    ('src/graphics/graphics_system.cpp',
     'graphics/graphics_system.cpp #1',
     '    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);\n\n',
     '    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);\n\n// PARCHE LOCAL - ritmo del vblank del guest\n//\n// Lo mismo que framerate_limit en Xenia Canary: alli ese cvar no limita los\n// fps del host, fija cada cuanto se le da un vblank al juego. Un juego que\n// presenta cada 2 vblanks -30 fps en una Xbox a 60 Hz- va a 60 fps con 120.\n// 0 = la frecuencia del modo de video, como siempre.\nREXCVAR_DEFINE_INT32(guest_vblank_rate, 0, "GPU",\n                     "Guest vblank rate in Hz (0 = video mode refresh rate with vsync, "\n                     "1000 without). A game locked to 30 fps waits two vblanks per frame, so "\n                     "120 makes it run at up to 60 fps. Same as framerate_limit in Xenia Canary.")\n    .range(0, 1000)\n    .lifecycle(rex::cvar::Lifecycle::kHotReload);\n\n'),
    ('src/graphics/graphics_system.cpp',
     'graphics/graphics_system.cpp #2',
     '        uint64_t guest_tick_frequency = chrono::Clock::guest_tick_frequency();\n        uint64_t vsync_interval_ticks =\n            std::max(uint64_t(1), uint64_t(double(guest_tick_frequency) / refresh_rate_hz));\n        uint64_t no_vsync_interval_ticks = std::max(uint64_t(1), guest_tick_frequency / 1000);\n',
     '        uint64_t guest_tick_frequency = chrono::Clock::guest_tick_frequency();\n        uint64_t no_vsync_interval_ticks = std::max(uint64_t(1), guest_tick_frequency / 1000);\n'),
    ('src/graphics/graphics_system.cpp',
     'graphics/graphics_system.cpp #3',
     '          uint64_t current_time = chrono::Clock::QueryGuestTickCount();\n          uint64_t interval_ticks =\n              REXCVAR_GET(vsync) ? vsync_interval_ticks : no_vsync_interval_ticks;\n          while (current_time - last_frame_time >= interval_ticks) {\n',
     '          uint64_t current_time = chrono::Clock::QueryGuestTickCount();\n          // Se relee en cada vuelta: guest_vblank_rate se puede cambiar en marcha.\n          int32_t vblank_rate = REXCVAR_GET(guest_vblank_rate);\n          double vblank_hz = vblank_rate > 0 ? double(vblank_rate) : refresh_rate_hz;\n          uint64_t vsync_interval_ticks =\n              std::max(uint64_t(1), uint64_t(double(guest_tick_frequency) / vblank_hz));\n          // Con guest_vblank_rate puesto manda el, haya vsync o no: asi el ritmo del\n          // juego y la sincronizacion con el monitor se eligen por separado.\n          uint64_t interval_ticks = vblank_rate > 0 ? vsync_interval_ticks\n                                    : REXCVAR_GET(vsync) ? vsync_interval_ticks\n                                                         : no_vsync_interval_ticks;\n          while (current_time - last_frame_time >= interval_ticks) {\n'),
    ('src/graphics/command_processor.cpp',
     'graphics/command_processor.cpp #1',
     '#include <rex/graphics/flags.h>\n#include <rex/graphics/graphics_system.h>\n',
     '#include <rex/graphics/flags.h>\n#include <rex/graphics/frame_pacer.h>  // PARCHE LOCAL\n#include <rex/graphics/graphics_system.h>\n'),
    ('src/graphics/command_processor.cpp',
     'graphics/command_processor.cpp #2',
     '\nREXCVAR_DEFINE_BOOL(vsync, true, "GPU", "Enable vertical sync");\n\n',
     '\n#if defined(_WIN32)\n#ifndef NOMINMAX\n#define NOMINMAX\n#endif\n#ifndef WIN32_LEAN_AND_MEAN\n#define WIN32_LEAN_AND_MEAN\n#endif\n#include <windows.h>\n#include <dwmapi.h>  // solo el tipo DWM_TIMING_INFO; la funcion se busca en tiempo de ejecucion\n#endif\n\nREXCVAR_DEFINE_BOOL(vsync, true, "GPU", "Enable vertical sync");\n\nREXCVAR_DEFINE_BOOL(log_frame_breakdown, false, "GPU",\n                    "With log_guest_fps: split every late frame by stage (shaders, textures, ...). "\n                    "Costs a few ms per frame in scenes with thousands of draws.")\n    .lifecycle(rex::cvar::Lifecycle::kHotReload);\n\nREXCVAR_DEFINE_BOOL(log_guest_fps, false, "GPU",\n                    "Log how many frames per second the game presents, every 10 seconds");\n\n// PARCHE LOCAL - ritmo de los flips\nREXCVAR_DEFINE_INT32(frame_pacing_fps, 0, "GPU",\n                     "Present guest frames at an exact, even rate (e.g. 30 or 60). Combine with "\n                     "a fast guest_vblank_rate so the game never misses a vblank slot. 0 = off.")\n    .range(0, 1000)\n    .lifecycle(rex::cvar::Lifecycle::kHotReload);\nREXCVAR_DEFINE_INT32(frame_pacing_phase, 25, "GPU",\n                     "Where in the display refresh the paced flips land, in percent after the "\n                     "vblank.")\n    .range(0, 99)\n    .lifecycle(rex::cvar::Lifecycle::kHotReload);\nREXCVAR_DEFINE_INT32(frame_pacing_smooth_max_ms, 6, "GPU",\n                     "With frame_pacing_at_guest: present every frame the same time after the "\n                     "game releases it (the slowest recent one, up to this many ms), for even "\n                     "frame times. 0 = present as soon as possible.")\n    .range(0, 16)\n    .lifecycle(rex::cvar::Lifecycle::kHotReload);\nREXCVAR_DEFINE_BOOL(frame_pacing_adaptive, true, "GPU",\n                    "With frame_pacing_at_guest: when a scene cannot hold the target, pace at "\n                    "what it can hold evenly (the 90th percentile of recent frames) instead of "\n                    "letting frames alternate between fast and slow.")\n    .lifecycle(rex::cvar::Lifecycle::kHotReload);\nREXCVAR_DEFINE_BOOL(frame_pacing_low_latency, true, "GPU",\n                    "With frame_pacing_at_guest: the game may only start a new frame once the "\n                    "previous one has been output, so it reads the controller as late as "\n                    "possible (like NVIDIA Reflex). Matters when the GPU emulation is the "\n                    "bottleneck, e.g. at 120 fps in busy scenes.")\n    .lifecycle(rex::cvar::Lifecycle::kHotReload);\nREXCVAR_DEFINE_BOOL(frame_pacing_at_guest, true, "GPU",\n                    "Apply frame_pacing_fps where the game hands over a frame (VdSwap), as the "\n                    "console did, instead of before presenting, so the game cannot queue "\n                    "frames ahead (lower input latency, see [latency] in the log).")\n    .lifecycle(rex::cvar::Lifecycle::kHotReload);\nREXCVAR_DEFINE_INT32(frame_pacing_display_lock, 1, "GPU",\n                     "Lock the paced flips to the display refresh reported by the compositor: "\n                     "0 = never, 1 = only with vsync, 2 = always. With G-Sync/FreeSync the "\n                     "reported refresh follows our own presents, so locking to it makes jitter.")\n    .range(0, 2)\n    .lifecycle(rex::cvar::Lifecycle::kHotReload);\n\n'),
    ('src/graphics/command_processor.cpp',
     'graphics/command_processor.cpp #3',
     '\n}  // namespace\n\n',
     '\n// PARCHE LOCAL - desglose de los fotogramas lentos (con log_guest_fps)\n//\n// Todo en ticks del reloj de host y solo desde el hilo del procesador de\n// comandos, asi que no hace falta sincronizar. Se pone a cero en cada flip.\n// kFrameDiagBuckets tiene que coincidir con el backend (d3d12/command_processor.cpp)\n// y con pipeline/texture/cache.cpp (10 y 11, dentro de "texturas").\nconstexpr int kFrameDiagBuckets = 12;\nstruct FrameDiag {\n  uint64_t idle = 0;         // sin comandos del juego (el juego no llega)\n  uint64_t wait_reg = 0;     // WAIT_REG_MEM sin cumplirse\n  uint32_t wait_reg_n = 0;\n  uint64_t gpu_wait = 0;     // esperando a la GPU del host (occlusion)\n  uint32_t gpu_wait_n = 0;\n  // Apartados del trabajo del procesador de comandos (backend D3D12).\n  uint64_t bucket[kFrameDiagBuckets] = {};\n  uint32_t bucket_n[kFrameDiagBuckets] = {};\n} g_frame_diag;\nbool g_frame_diag_enabled = false;  // apartados (log_frame_breakdown)\nbool g_frame_stats_enabled = false;  // estadisticas baratas (log_guest_fps)\n\nconst char* const kFrameDiagBucketNames[kFrameDiagBuckets] = {\n    "shader", "primitives", "render targets", "pipeline", "textures",\n    "bindings", "vertex buffers", "resolve", "GPU wait", "submit",\n    "of which texture creation", "of which memory upload"};\n\n}  // namespace\n\nvoid FrameDiagAddGpuWait(uint64_t ticks) {\n  g_frame_diag.gpu_wait += ticks;\n  ++g_frame_diag.gpu_wait_n;\n}\n\nbool FrameDiagEnabled() { return g_frame_diag_enabled; }\nbool FrameStatsEnabled() { return g_frame_stats_enabled; }\n\nvoid FrameDiagAddBucket(int bucket, uint64_t ticks) {\n  g_frame_diag.bucket[bucket] += ticks;\n  ++g_frame_diag.bucket_n[bucket];\n}\n\n// PARCHE LOCAL - fotogramas en cola (frame_pacing_max_queued). Momento del\n// VdSwap del ultimo fotograma que ya salio hacia el presentador. VdSwap (otro\n// DLL, el runtime) lo lee por GetProcAddress y espera a que el anterior haya\n// salido antes de dejar al juego empezar el siguiente: asi el juego lee el\n// mando lo mas tarde posible, como hace Reflex.\nnamespace {\nstd::atomic<uint64_t> g_last_output_vdswap_ticks{0};\n}  // namespace\n\n}  // namespace rex::graphics\n\nextern "C" __declspec(dllexport) uint64_t rex_gpu_last_output_vdswap_ticks() {\n  return rex::graphics::g_last_output_vdswap_ticks.load(std::memory_order_acquire);\n}\n\nnamespace rex::graphics {\n\n'),
    ('src/graphics/command_processor.cpp',
     'graphics/command_processor.cpp #4',
     '      PrepareForWait();\n      uint32_t loop_count = 0;\n',
     '      PrepareForWait();\n      const uint64_t idle_start = rex::chrono::Clock::QueryHostTickCount();\n      uint32_t loop_count = 0;\n'),
    ('src/graphics/command_processor.cpp',
     'graphics/command_processor.cpp #5',
     '               (write_ptr_index == 0xBAADF00D || read_ptr_index_ == write_ptr_index));\n      ReturnFromWait();\n',
     '               (write_ptr_index == 0xBAADF00D || read_ptr_index_ == write_ptr_index));\n      g_frame_diag.idle += rex::chrono::Clock::QueryHostTickCount() - idle_start;\n      ReturnFromWait();\n'),
    ('src/graphics/command_processor.cpp',
     'graphics/command_processor.cpp #6',
     '  uint32_t write_one_reg = (packet >> 15) & 0x1;\n  for (uint32_t m = 0; m < count; m++) {\n',
     '  uint32_t write_one_reg = (packet >> 15) & 0x1;\n  // PARCHE LOCAL - consecutive registers as one block, so the backend can use\n  // its bulk paths (see D3D12CommandProcessor::WriteRegistersFromMem).\n  if (!write_one_reg && count > 1) {\n    WriteRegisterRangeFromRing(reader, base_index, count);\n    return true;\n  }\n  for (uint32_t m = 0; m < count; m++) {\n'),
    ('src/graphics/command_processor.cpp',
     'graphics/command_processor.cpp #7',
     '  uint32_t frontbuffer_height = reader->ReadAndSwap<uint32_t>();\n  reader->AdvanceRead((count - 4) * sizeof(uint32_t));\n\n  IssueSwap(frontbuffer_ptr, frontbuffer_width, frontbuffer_height);\n\n',
     '  uint32_t frontbuffer_height = reader->ReadAndSwap<uint32_t>();\n  // PARCHE LOCAL - latencia: VdSwap anade el momento en que el juego entrego\n  // el fotograma (xboxkrnl_video.cpp).\n  // Y, con 8 palabras, el momento en que se solto tras la espera del ritmo.\n  uint64_t vdswap_ticks = 0, release_ticks = 0;\n  uint32_t extra_read = 4;\n  if (count >= 6) {\n    const uint32_t ticks_lo = reader->ReadAndSwap<uint32_t>();\n    const uint32_t ticks_hi = reader->ReadAndSwap<uint32_t>();\n    vdswap_ticks = uint64_t(ticks_lo) | (uint64_t(ticks_hi) << 32);\n    extra_read = 6;\n  }\n  if (count >= 8) {\n    const uint32_t ticks_lo = reader->ReadAndSwap<uint32_t>();\n    const uint32_t ticks_hi = reader->ReadAndSwap<uint32_t>();\n    release_ticks = uint64_t(ticks_lo) | (uint64_t(ticks_hi) << 32);\n    extra_read = 8;\n  }\n  reader->AdvanceRead((count - extra_read) * sizeof(uint32_t));\n\n  // PARCHE LOCAL - ritmo de los flips (frame_pacing_fps)\n  //\n  // Con vblank de 60 Hz el juego coloca cada flip en una "ranura" de vblank, y\n  // si llega tarde por un pelo pierde la ranura entera: los flips salian cada\n  // 16,6, 33 o 50 ms en vez de cada 33 -el tiron que se veia a "30 fps"-. La\n  // forma buena es darle vblanks rapidos (guest_vblank_rate alto, no pierde\n  // ninguna) y marcar el ritmo aqui con un reloj preciso: un flip cada\n  // 1/frame_pacing_fps segundos, contando desde el flip PREVISTO y no desde el\n  // real, para que un retraso suelto no desplace a todos los demas.\n  //\n  // MEDIDO en el menu (vblank 1000 Hz, sin vsync): 30 fps con flips cada\n  // 33,1-33,5 ms, 60 fps con 16,4-16,9 ms. Con vsync del host encima vuelve a\n  // haber dos relojes y temblor de un refresco del monitor.\n  //\n  // Y el reloj va enganchado al del monitor. Un monitor "de 120 Hz" midio\n  // 120,2429 Hz: con un reloj propio de 60,000 fps cada 8,3 s sobraba o\n  // faltaba un refresco, y se veia como un tiron periodico. Asi que, en\n  // Windows, el periodo es un multiplo exacto del refresco real que da el\n  // compositor (DwmGetCompositionTimingInfo, en unidades de QPC como nuestro\n  // reloj) y la fase queda a un cuarto de refresco despues de un vblank, lejos\n  // del borde entre dos refrescos.\n  //\n  // PERO solo con vsync (frame_pacing_display_lock = 1). Con G-Sync/FreeSync\n  // el "vblank" que da el compositor es el de nuestro propio present, y\n  // recolocar la fase sobre el cada segundo desplazaba el reloj hasta medio\n  // refresco. MEDIDO conduciendo sin vsync con G-Sync: 59,8 fps y flips de\n  // 12,4 a 20,8 ms (16,7 +- 4,2 = medio refresco a 120 Hz).\n  //\n  // Desde que el ritmo va en el hilo del juego (frame_pacing_at_guest, en\n  // VdSwap) aqui solo se aplica si ese ajuste esta apagado. El codigo del\n  // ritmo vive en rex/graphics/frame_pacer.h para los dos sitios.\n  double diag_late_ms = 0.0, diag_sleep_ms = 0.0, diag_over_ms = 0.0, diag_period_ms = 0.0;\n  bool diag_reset = false;\n  if (const int32_t pacing_fps = REXCVAR_GET(frame_pacing_fps);\n      pacing_fps > 0 && !REXCVAR_GET(frame_pacing_at_guest)) {\n    const int32_t display_lock = REXCVAR_GET(frame_pacing_display_lock);\n    const FramePacerTiming t =\n        PaceFrame(pacing_fps, display_lock == 2 || (display_lock == 1 && REXCVAR_GET(vsync)),\n                  REXCVAR_GET(frame_pacing_phase));\n    diag_late_ms = t.late_ms;\n    diag_sleep_ms = t.sleep_ms;\n    diag_over_ms = t.over_ms;\n    diag_period_ms = t.period_ms;\n    diag_reset = t.reset;\n  }\n\n  // PARCHE LOCAL - suavizado con el ritmo en el juego. El fotograma sale del\n  // VdSwap a un ritmo exacto, pero al procesador de comandos todavia le queda\n  // el final del fotograma, y eso varia: MEDIDO en el menu, presents cada\n  // 14,9-18,6 ms en vez de 16,2-17,0. Asi que cada fotograma se presenta a la\n  // misma distancia de su suelta: el maximo de ese resto en los ultimos 120\n  // fotogramas (+0,3 ms), con un tope de frame_pacing_smooth_max_ms. Se\n  // cambia algo de latencia (la diferencia entre ese maximo y el resto de\n  // cada fotograma) por intervalos regulares.\n  double smooth_target_ms = 0.0, smooth_wait_ms = 0.0;\n  if (const int32_t smooth_max = REXCVAR_GET(frame_pacing_smooth_max_ms);\n      smooth_max > 0 && release_ticks && REXCVAR_GET(frame_pacing_fps) > 0 &&\n      REXCVAR_GET(frame_pacing_at_guest)) {\n    const uint64_t freq = rex::chrono::Clock::QueryHostTickFrequency();\n    const uint64_t now = rex::chrono::Clock::QueryHostTickCount();\n    static uint64_t tails[120] = {};\n    static uint32_t tail_index = 0;\n    const uint64_t tail = now > release_ticks ? now - release_ticks : 0;\n    if (tail < freq / 10) {  // ignorar cargas y pausas\n      tails[tail_index++ % 120] = tail;\n    }\n    uint64_t target_tail = 0;\n    for (uint64_t v : tails) {\n      target_tail = std::max(target_tail, v);\n    }\n    target_tail += freq * 3 / 10000;  // +0,3 ms\n    target_tail = std::min(target_tail, freq * uint64_t(smooth_max) / 1000);\n    smooth_target_ms = double(target_tail) * 1000.0 / double(freq);\n    const uint64_t target = release_ticks + target_tail;\n    if (now < target) {\n      smooth_wait_ms = double(target - now) * 1000.0 / double(freq);\n      if (smooth_wait_ms > 1.5) {\n        rex::thread::Sleep(std::chrono::milliseconds(int(smooth_wait_ms - 1.0)));\n      }\n      while (rex::chrono::Clock::QueryHostTickCount() < target) {\n        rex::thread::MaybeYield();\n      }\n    }\n  }\n\n  const uint64_t swap_start = rex::chrono::Clock::QueryHostTickCount();\n  IssueSwap(frontbuffer_ptr, frontbuffer_width, frontbuffer_height);\n  const uint64_t swap_end = rex::chrono::Clock::QueryHostTickCount();\n  if (vdswap_ticks) {\n    g_last_output_vdswap_ticks.store(vdswap_ticks, std::memory_order_release);\n  }\n\n  // PARCHE LOCAL - desglose de los fotogramas lentos\n  //\n  // Cada fotograma que llega tarde al ritmo (o, sin ritmo, que tarda mas de\n  // 25 ms) deja una linea con en que se fue el tiempo desde el flip anterior.\n  // Las esperas a la GPU de occlusion dentro de un WAIT_REG_MEM cuentan en los\n  // dos apartados.\n  if (REXCVAR_GET(log_guest_fps)) {\n    static uint64_t prev_swap_end = 0;\n    static uint64_t window_start = 0;\n    static uint32_t lines = 0, suppressed = 0;\n    const double f = 1000.0 / double(rex::chrono::Clock::QueryHostTickFrequency());\n\n    // PARCHE LOCAL - latencia: desde que el juego entrega el fotograma\n    // (VdSwap) hasta que sale hacia el presentador (IssueSwap), y cuanto de\n    // eso es la espera del ritmo. Cada 10 s: minimo, media y maximo.\n    {\n      static uint64_t lat_window = 0;\n      static uint32_t lat_n = 0;\n      static double lat_sum = 0.0, lat_min = 1e9, lat_max = 0.0, pace_sum = 0.0;\n      static double rel_sum = 0.0, smooth_sum = 0.0;\n      static uint32_t rel_n = 0;\n      // Desde que el juego empezo este fotograma (la suelta del anterior, que\n      // es cuando lee el mando) hasta que sale: la latencia de entrada que\n      // depende de nosotros.\n      static uint64_t prev_release = 0;\n      static double input_sum = 0.0, input_max = 0.0;\n      static uint32_t input_n = 0;\n      if (prev_release && release_ticks && swap_end > prev_release) {\n        const double input_ms = double(swap_end - prev_release) * f;\n        if (input_ms < 1000.0) {\n          input_sum += input_ms;\n          input_max = std::max(input_max, input_ms);\n          ++input_n;\n        }\n      }\n      prev_release = release_ticks;\n      if (release_ticks && swap_end > release_ticks) {\n        const double rel_ms = double(swap_end - release_ticks) * f;\n        if (rel_ms < 1000.0) {\n          rel_sum += rel_ms;\n          smooth_sum += smooth_target_ms;\n          ++rel_n;\n        }\n      }\n      if (vdswap_ticks && swap_end > vdswap_ticks) {\n        const double lat_ms = double(swap_end - vdswap_ticks) * f;\n        if (lat_ms < 1000.0) {\n          ++lat_n;\n          lat_sum += lat_ms;\n          lat_min = std::min(lat_min, lat_ms);\n          lat_max = std::max(lat_max, lat_ms);\n          pace_sum += diag_sleep_ms;\n        }\n      }\n      if (!lat_window) {\n        lat_window = swap_end;\n      } else if (swap_end - lat_window >= rex::chrono::Clock::QueryHostTickFrequency() * 10) {\n        if (lat_n) {\n          REXGPU_INFO(\n              "[latency] VdSwap -> output {:.1f} ms avg ({:.1f}-{:.1f}) | pacing wait "\n              "here {:.1f} ms | release -> output {:.1f} ms (smoothed to {:.1f}) | frame "\n              "start -> output {:.1f} ms avg, {:.1f} max",\n              lat_sum / lat_n, lat_min, lat_max, pace_sum / lat_n,\n              rel_n ? rel_sum / rel_n : 0.0, rel_n ? smooth_sum / rel_n : 0.0,\n              input_n ? input_sum / input_n : 0.0, input_max);\n        }\n        lat_window = swap_end;\n        lat_n = 0;\n        rel_n = 0;\n        input_n = 0;\n        input_sum = input_max = 0.0;\n        rel_sum = smooth_sum = 0.0;\n        lat_sum = pace_sum = 0.0;\n        lat_min = 1e9;\n        lat_max = 0.0;\n      }\n    }\n    if (prev_swap_end) {\n      const double frame_ms = double(swap_start - prev_swap_end) * f;\n      const bool slow = diag_period_ms > 0.0 ? diag_late_ms > 2.0 : frame_ms > 25.0;\n      if (!window_start || swap_end - window_start >\n                               rex::chrono::Clock::QueryHostTickFrequency() * 10) {\n        if (suppressed) {\n          REXGPU_INFO("[slow frame] ... and {} more not logged", suppressed);\n        }\n        window_start = swap_end;\n        lines = suppressed = 0;\n      }\n      if (slow && !diag_reset) {\n        if (lines < 30) {\n          ++lines;\n          const double idle = double(g_frame_diag.idle) * f;\n          const double wait_reg = double(g_frame_diag.wait_reg) * f;\n          const double gpu_wait = double(g_frame_diag.gpu_wait) * f;\n          const double work = frame_ms - diag_sleep_ms - idle - wait_reg;\n          REXGPU_INFO(\n              "[slow frame] {:.1f} ms, late {:.1f} | no game commands {:.1f} | "\n              "WAIT_REG_MEM {:.1f} ({}x) | GPU occlusion {:.1f} ({}x) | swap {:.1f} | "\n              "pacer slept {:.1f} +{:.1f} | rest (CP working) {:.1f}",\n              frame_ms, diag_late_ms, idle, g_frame_diag.wait_reg_n ? wait_reg : 0.0,\n              g_frame_diag.wait_reg_n, gpu_wait, g_frame_diag.gpu_wait_n,\n              double(swap_end - swap_start) * f, diag_sleep_ms, diag_over_ms, work);\n          // Y el trabajo, por apartados (solo los de mas de 0,3 ms).\n          std::string partes;\n          for (int i = 0; i < kFrameDiagBuckets; ++i) {\n            const double ms = double(g_frame_diag.bucket[i]) * f;\n            if (ms >= 0.3) {\n              partes += fmt::format(" | {} {:.1f} ({}x)", kFrameDiagBucketNames[i], ms,\n                                    g_frame_diag.bucket_n[i]);\n            }\n          }\n          if (g_frame_diag_enabled) {\n            REXGPU_INFO("[slow frame]   work:{}", partes.empty() ? " (nothing measurable)" : partes);\n          }\n        } else {\n          ++suppressed;\n        }\n      }\n    }\n    prev_swap_end = swap_end;\n  }\n  g_frame_diag = FrameDiag{};\n  g_frame_stats_enabled = REXCVAR_GET(log_guest_fps);\n  g_frame_diag_enabled = g_frame_stats_enabled && REXCVAR_GET(log_frame_breakdown);\n\n  // PARCHE LOCAL - fps del guest en el log\n  //\n  // Cuantas veces presenta el juego por segundo, medido aqui y no en el host:\n  // es lo unico que dice si guest_vblank_rate cambio de verdad el ritmo del\n  // juego. Una linea cada 10 s, solo con log_guest_fps.\n  if (REXCVAR_GET(log_guest_fps)) {\n    static uint64_t window_start = 0;\n    static uint64_t last_swap = 0;\n    static uint32_t swaps = 0;\n    // La media dice poco de un tiron: se apuntan tambien el abstand minimo y\n    // maximo entre fotogramas y cuantos pasaron de 25 y de 50 ms.\n    static double min_ms = 1e9, max_ms = 0.0;\n    static uint32_t over25 = 0, over50 = 0;\n    const uint64_t now = rex::chrono::Clock::QueryHostTickCount();\n    const uint64_t freq = rex::chrono::Clock::QueryHostTickFrequency();\n    if (!window_start) {\n      window_start = now;\n    }\n    if (last_swap) {\n      const double ms = double(now - last_swap) * 1000.0 / double(freq);\n      min_ms = std::min(min_ms, ms);\n      max_ms = std::max(max_ms, ms);\n      over25 += ms > 25.0;\n      over50 += ms > 50.0;\n    }\n    last_swap = now;\n    ++swaps;\n    if (now - window_start >= freq * 10) {\n      REXGPU_INFO("[guest fps] {:.1f} swaps/s | frame {:.1f}-{:.1f} ms | >25ms {} | >50ms {}",\n                  swaps * double(freq) / double(now - window_start), min_ms, max_ms, over25,\n                  over50);\n      window_start = now;\n      swaps = 0;\n      min_ms = 1e9;\n      max_ms = 0.0;\n      over25 = over50 = 0;\n    }\n  }\n\n'),
    ('src/graphics/command_processor.cpp',
     'graphics/command_processor.cpp #8',
     '  bool is_memory = (wait_info & 0x10) != 0;\n\n  bool matched = false;\n  do {\n',
     '  bool is_memory = (wait_info & 0x10) != 0;\n\n  const uint64_t wait_start = rex::chrono::Clock::QueryHostTickCount();\n  bool matched = false;\n  do {\n'),
    ('src/graphics/command_processor.cpp',
     'graphics/command_processor.cpp #9',
     '        PrepareForWait();\n        if (!REXCVAR_GET(vsync)) {\n          // User wants it fast and dangerous.\n          rex::thread::MaybeYield();\n        } else {\n          rex::thread::Sleep(std::chrono::milliseconds(wait / 0x100));\n        }\n',
     '        PrepareForWait();\n        // PARCHE LOCAL - espera activa los 2 primeros ms, y solo despues\n        // dormir 1 ms por vuelta.\n        //\n        // Antes, con vsync, cada vuelta dormia wait/0x100 ms (y el Sleep de\n        // Windows redondea a 15,6 ms si nadie sube la resolucion del reloj).\n        // NFS Most Wanted hace muchas de estas esperas por fotograma y casi\n        // todas se resuelven en microsegundos: la suma de dormidas le costaba\n        // mas de un periodo de vblank por fotograma. MEDIDO: con vsync, 30 fps\n        // y flips cada 16/33/50 ms; sin vsync -que aqui solo cedia el hilo-,\n        // cientos de fps y flips regulares.\n        const double waited_ms =\n            double(rex::chrono::Clock::QueryHostTickCount() - wait_start) * 1000.0 /\n            double(rex::chrono::Clock::QueryHostTickFrequency());\n        if (!REXCVAR_GET(vsync) || waited_ms < 2.0) {\n          rex::thread::MaybeYield();\n        } else {\n          rex::thread::Sleep(std::chrono::milliseconds(1));\n        }\n'),
    ('src/graphics/command_processor.cpp',
     'graphics/command_processor.cpp #10',
     '  } while (!matched);\n\n',
     '  } while (!matched);\n\n  if (const uint64_t waited = rex::chrono::Clock::QueryHostTickCount() - wait_start;\n      waited > rex::chrono::Clock::QueryHostTickFrequency() / 10000) {  // > 0,1 ms\n    g_frame_diag.wait_reg += waited;\n    ++g_frame_diag.wait_reg_n;\n  }\n\n'),
    ('include/rex/graphics/d3d12/command_processor.h',
     'd3d12/command_processor.h #1',
     '\n  Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,\n',
     '\n  // PARCHE LOCAL - occlusion queries diferidas: al quedarse sin comandos, o\n  // cuando el guest espera un registro, se entregan los resultados pendientes.\n  void PrepareForWait() override;\n\n  Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,\n'),
    ('include/rex/graphics/d3d12/command_processor.h',
     'd3d12/command_processor.h #2',
     '  void ShutdownOcclusionQueryResources();\n  bool BeginGuestOcclusionQuery(uint32_t sample_count_address);\n  bool EndGuestOcclusionQuery(uint32_t sample_count_address,\n                              xenos::xe_gpu_depth_sample_counts* sample_counts);\n  bool AcquireOcclusionQueryIndex(uint32_t& host_index_out);\n  void DisableHostOcclusionQueries();\n  uint64_t NormalizeOcclusionSamples(uint64_t samples) const;\n',
     '  void ShutdownOcclusionQueryResources();\n  bool AcquireOcclusionQueryIndex(uint32_t& host_index_out);\n  void DisableHostOcclusionQueries();\n  // PARCHE LOCAL - ZPD como contador continuo.\n  void ZpdOpenSegment();\n  void ZpdCloseSegment();\n  void ProcessPendingOcclusionQueries(bool wait);\n  uint64_t NormalizeOcclusionSamples(uint64_t samples) const;\n'),
    ('include/rex/graphics/d3d12/command_processor.h',
     'd3d12/command_processor.h #3',
     '  } active_occlusion_query_;\n  struct VertexBufferState {\n',
     '  } active_occlusion_query_;\n  // PARCHE LOCAL - ZPD como contador continuo (la idea de Xenia Edge,\n  // 3d233a5 "Rewrite ZPD as a running sample counter").\n  //\n  // La Xenos no tiene consultas con principio y fin: tiene un contador de\n  // muestras que no para, y cada EVENT_WRITE_ZPD vuelca su valor en\n  // RB_SAMPLE_COUNT_ADDR; el D3D del juego resta dos volcados. Aqui cada\n  // evento cierra el intervalo medido desde el anterior, pone en cola su\n  // informe y abre el siguiente. Un intervalo puede partirse en varios\n  // segmentos si termina una submission por medio (una consulta de D3D12 no\n  // puede cruzar listas de comandos).\n  struct ZpdSegment {\n    uint32_t host_index = 0;\n    uint64_t submission = 0;\n  };\n  struct ZpdReport {\n    uint32_t address = 0;\n    std::vector<ZpdSegment> segments;\n  };\n  std::vector<ZpdSegment> zpd_interval_segments_;\n  std::deque<ZpdReport> zpd_reports_;\n  bool zpd_segment_reopen_ = false;\n  uint32_t zpd_counter_ = 0;\n  struct VertexBufferState {\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #1',
     '                    "Submit command list when PM4 primary buffer ends")\n    .lifecycle(rex::cvar::Lifecycle::kHotReload);\n\nnamespace rex::graphics::d3d12 {\n',
     '                    "Submit command list when PM4 primary buffer ends")\n    .lifecycle(rex::cvar::Lifecycle::kHotReload);\n\nnamespace rex::graphics {\n// PARCHE LOCAL - desglose de los fotogramas lentos (graphics/command_processor.cpp).\nvoid FrameDiagAddGpuWait(uint64_t ticks);\nbool FrameDiagEnabled();\nvoid FrameDiagAddBucket(int bucket, uint64_t ticks);\n}  // namespace rex::graphics\n\nnamespace {\n// Mismo orden que kFrameDiagBucketNames en graphics/command_processor.cpp.\nenum DiagBucket {\n  kDiagShader,\n  kDiagPrimitive,\n  kDiagRenderTargets,\n  kDiagPipeline,\n  kDiagTextures,\n  kDiagBindings,\n  kDiagVertexBuffers,\n  kDiagResolve,\n  kDiagGpuWait,\n  kDiagSubmit,\n};\n// Mide el ambito y lo suma al apartado; sin log_guest_fps no cuesta nada.\nclass DiagScope {\n public:\n  explicit DiagScope(DiagBucket bucket)\n      : bucket_(bucket),\n        start_(rex::graphics::FrameDiagEnabled() ? rex::chrono::Clock::QueryHostTickCount() : 0) {}\n  ~DiagScope() {\n    if (start_) {\n      rex::graphics::FrameDiagAddBucket(bucket_,\n                                        rex::chrono::Clock::QueryHostTickCount() - start_);\n    }\n  }\n  DiagScope(const DiagScope&) = delete;\n  DiagScope& operator=(const DiagScope&) = delete;\n\n private:\n  DiagBucket bucket_;\n  uint64_t start_;\n};\n}  // namespace\n\nnamespace rex::graphics::d3d12 {\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #2',
     '\nbool D3D12CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader,\n                                                               uint32_t packet, uint32_t count) {\n  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_query_resources_available_) {\n    return CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(reader, packet, count);\n  }\n\n  const uint32_t kQueryFinished = rex::byte_swap(0xFFFFFEED);\n  assert_true(count == 1);\n',
     '\n// PARCHE LOCAL - cuentas de occlusion queries para el log (con log_guest_fps).\nnamespace {\nstruct OcclusionStats {\n  uint32_t events = 0, no_resources = 0, reports = 0, delivered = 0, waited = 0;\n  uint64_t delta_max = 0;\n  std::chrono::steady_clock::time_point window{};\n} g_occlusion_stats;\n\nvoid LogOcclusionStats(bool resources, size_t pending) {\n  auto& s = g_occlusion_stats;\n  const auto now = std::chrono::steady_clock::now();\n  if (s.window == std::chrono::steady_clock::time_point{}) {\n    s.window = now;\n  }\n  if (now - s.window < std::chrono::seconds(10)) {\n    return;\n  }\n  if (rex::cvar::GetFlagInfo("log_guest_fps") && rex::cvar::Query<bool>("log_guest_fps")) {\n    REXGPU_INFO(\n        "[occlusion] events {} | no resources {} | reports {} | delivered {} | waited {} "\n        "| queued {} | max samples per interval {} | resources {}",\n        s.events, s.no_resources, s.reports, s.delivered, s.waited, pending, s.delta_max,\n        resources ? "yes" : "no");\n  }\n  s = {};\n  s.window = now;\n}\n}  // namespace\n\n// PARCHE LOCAL - ZPD como contador continuo (ver ZpdReport en la cabecera).\n//\n// MEDIDO en NFS Most Wanted, conduciendo: ~370 eventos por segundo, y NINGUNO\n// casaba con el modelo anterior de "consulta con principio y fin en la misma\n// direccion": el volcado de antes y el de despues van a direcciones distintas.\n// Todos acababan en el valor falso de "1000 muestras visibles", y el sol y sus\n// destellos se veian a traves de edificios y puentes.\nbool D3D12CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader,\n                                                               uint32_t packet, uint32_t count) {\n  ++g_occlusion_stats.events;\n  LogOcclusionStats(occlusion_query_resources_available_, zpd_reports_.size());\n  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_query_resources_available_) {\n    ++g_occlusion_stats.no_resources;\n    return CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(reader, packet, count);\n  }\n\n  assert_true(count == 1);\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #3',
     '\n  uint32_t sample_count_addr = register_file_->values[XE_GPU_REG_RB_SAMPLE_COUNT_ADDR];\n  auto* sample_counts =\n      memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(sample_count_addr);\n  if (!sample_counts) {\n    DisableHostOcclusionQueries();\n    return true;\n  }\n\n  auto write_fallback_result = [sample_counts, kQueryFinished]() -> bool {\n    auto fake_sample_count = REXCVAR_GET(query_occlusion_fake_sample_count);\n    if (fake_sample_count < 0) {\n      return true;\n    }\n    bool is_end_via_z_pass =\n        sample_counts->ZPass_A == kQueryFinished || sample_counts->ZPass_B == kQueryFinished;\n    bool is_end_via_z_fail =\n        sample_counts->ZFail_A == kQueryFinished || sample_counts->ZFail_B == kQueryFinished;\n    std::memset(sample_counts, 0, sizeof(xenos::xe_gpu_depth_sample_counts));\n    if (is_end_via_z_pass || is_end_via_z_fail) {\n      sample_counts->ZPass_A = fake_sample_count;\n      sample_counts->Total_A = fake_sample_count;\n    }\n    return true;\n  };\n\n  bool is_end_via_z_pass =\n      sample_counts->ZPass_A == kQueryFinished || sample_counts->ZPass_B == kQueryFinished;\n  bool is_end_via_z_fail =\n      sample_counts->ZFail_A == kQueryFinished || sample_counts->ZFail_B == kQueryFinished;\n  bool is_end = is_end_via_z_pass || is_end_via_z_fail;\n\n  if (!is_end) {\n    if (active_occlusion_query_.valid &&\n        active_occlusion_query_.sample_count_address != sample_count_addr) {\n      DisableHostOcclusionQueries();\n      return write_fallback_result();\n    }\n    if (!BeginGuestOcclusionQuery(sample_count_addr)) {\n      return write_fallback_result();\n    }\n    return true;\n  }\n\n  if (!active_occlusion_query_.valid ||\n      active_occlusion_query_.sample_count_address != sample_count_addr) {\n    DisableHostOcclusionQueries();\n    return write_fallback_result();\n  }\n\n  if (!EndGuestOcclusionQuery(sample_count_addr, sample_counts)) {\n    return write_fallback_result();\n  }\n\n  return true;\n',
     '\n  // Cerrar el intervalo medido desde el evento anterior, encolar su informe y\n  // abrir el siguiente.\n  ZpdCloseSegment();\n  ZpdReport report;\n  report.address = register_file_->values[XE_GPU_REG_RB_SAMPLE_COUNT_ADDR];\n  report.segments = std::move(zpd_interval_segments_);\n  zpd_interval_segments_.clear();\n  zpd_reports_.push_back(std::move(report));\n  ++g_occlusion_stats.reports;\n  ZpdOpenSegment();\n\n  ProcessPendingOcclusionQueries(false);\n  return true;\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #4',
     '\n  CommandProcessor::WriteRegistersFromMem(start_index, base, num_registers);\n}\n',
     '\n  // PARCHE LOCAL - mixed ranges in pieces. Type-0 packets (ExecutePacketType0)\n  // now come here as a whole block too; before, every register went through\n  // the virtual WriteRegister (MEASURED: ~13 % of the GPU thread in the city).\n  // Each piece gets exactly the handling a single write would: constant\n  // ranges their fast paths above, registers with side effects in\n  // CommandProcessor::WriteRegister (scratch writeback, COHER, gamma LUT,\n  // out of range) one by one, and every other register a plain copy.\n  enum class Kind { kPlain, kFloat, kBoolLoop, kFetch, kSingle };\n  auto kind_of = [](uint32_t i) {\n    if (i >= RegisterFile::kRegisterCount) return Kind::kSingle;\n    if (i >= XE_GPU_REG_SHADER_CONSTANT_000_X && i <= XE_GPU_REG_SHADER_CONSTANT_511_W)\n      return Kind::kFloat;\n    if (i >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 && i <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5)\n      return Kind::kFetch;\n    if (i >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 && i <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31)\n      return Kind::kBoolLoop;\n    if (i >= XE_GPU_REG_SCRATCH_REG0 && i <= XE_GPU_REG_SCRATCH_REG7) return Kind::kSingle;\n    switch (i) {\n      case XE_GPU_REG_COHER_STATUS_HOST:\n      case XE_GPU_REG_DC_LUT_RW_INDEX:\n      case XE_GPU_REG_DC_LUT_SEQ_COLOR:\n      case XE_GPU_REG_DC_LUT_PWL_DATA:\n      case XE_GPU_REG_DC_LUT_30_COLOR:\n        return Kind::kSingle;\n      default:\n        return Kind::kPlain;\n    }\n  };\n  uint32_t offset = 0;\n  while (offset < num_registers) {\n    const uint32_t first = start_index + offset;\n    const Kind kind = kind_of(first);\n    uint32_t count = 1;\n    if (kind != Kind::kSingle) {\n      while (offset + count < num_registers && kind_of(first + count) == kind) {\n        ++count;\n      }\n    }\n    switch (kind) {\n      case Kind::kPlain:\n        memory::copy_and_swap(register_file_->values + first, base + offset, count);\n        break;\n      case Kind::kSingle:\n        WriteRegister(first, memory::load_and_swap<uint32_t>(base + offset));\n        break;\n      default:\n        // A pure constant range: one of the fast paths above.\n        WriteRegistersFromMem(first, base + offset, count);\n        break;\n    }\n    offset += count;\n  }\n}\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #5',
     '    // Special copy handling.\n    return IssueCopy();\n',
     '    // Special copy handling.\n    DiagScope diag_scope(kDiagResolve);\n    return IssueCopy();\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #6',
     '  }\n  pipeline_cache_->AnalyzeShaderUcode(*vertex_shader);\n  bool memexport_used_vertex = vertex_shader->memexport_eM_written() != 0;\n',
     '  }\n  {\n    DiagScope diag_scope(kDiagShader);\n    pipeline_cache_->AnalyzeShaderUcode(*vertex_shader);\n  }\n  bool memexport_used_vertex = vertex_shader->memexport_eM_written() != 0;\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #7',
     '      if (pixel_shader) {\n        pipeline_cache_->AnalyzeShaderUcode(*pixel_shader);\n        if (!draw_util::IsPixelShaderNeededWithRasterization(*pixel_shader, regs)) {\n',
     '      if (pixel_shader) {\n        {\n          DiagScope diag_scope(kDiagShader);\n          pipeline_cache_->AnalyzeShaderUcode(*pixel_shader);\n        }\n        if (!draw_util::IsPixelShaderNeededWithRasterization(*pixel_shader, regs)) {\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #8',
     '  PrimitiveProcessor::ProcessingResult primitive_processing_result;\n  if (!primitive_processor_->Process(primitive_processing_result)) {\n    return false;\n  }\n',
     '  PrimitiveProcessor::ProcessingResult primitive_processing_result;\n  {\n    DiagScope diag_scope(kDiagPrimitive);\n    if (!primitive_processor_->Process(primitive_processing_result)) {\n      return false;\n    }\n  }\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #9',
     '                   : 0;\n  if (!render_target_cache_->Update(is_rasterization_done, normalized_depth_control,\n                                    normalized_color_mask, *vertex_shader)) {\n    return false;\n  }\n',
     '                   : 0;\n  {\n    DiagScope diag_scope(kDiagRenderTargets);\n    if (!render_target_cache_->Update(is_rasterization_done, normalized_depth_control,\n                                      normalized_color_mask, *vertex_shader)) {\n      return false;\n    }\n  }\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #10',
     '  ID3D12RootSignature* root_signature;\n  if (!pipeline_cache_->ConfigurePipeline(\n          vertex_shader_translation, pixel_shader_translation, primitive_processing_result,\n          normalized_depth_control, normalized_color_mask, bound_depth_and_color_render_target_bits,\n          bound_depth_and_color_render_target_formats, &pipeline_handle, &root_signature)) {\n    return false;\n  }\n',
     '  ID3D12RootSignature* root_signature;\n  {\n    DiagScope diag_scope(kDiagPipeline);\n    if (!pipeline_cache_->ConfigurePipeline(\n            vertex_shader_translation, pixel_shader_translation, primitive_processing_result,\n            normalized_depth_control, normalized_color_mask, bound_depth_and_color_render_target_bits,\n            bound_depth_and_color_render_target_formats, &pipeline_handle, &root_signature)) {\n      return false;\n    }\n  }\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #11',
     '      (pixel_shader != nullptr ? pixel_shader->GetUsedTextureMaskAfterTranslation() : 0);\n  texture_cache_->RequestTextures(used_texture_mask);\n\n',
     '      (pixel_shader != nullptr ? pixel_shader->GetUsedTextureMaskAfterTranslation() : 0);\n  {\n    DiagScope diag_scope(kDiagTextures);\n    texture_cache_->RequestTextures(used_texture_mask);\n  }\n\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #12',
     '  // Update constant buffers, descriptors and root parameters.\n  if (!UpdateBindings(vertex_shader, pixel_shader, root_signature, memexport_used)) {\n    return false;\n  }\n',
     '  // Update constant buffers, descriptors and root parameters.\n  {\n    DiagScope diag_scope(kDiagBindings);\n    if (!UpdateBindings(vertex_shader, pixel_shader, root_signature, memexport_used)) {\n      return false;\n    }\n  }\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #13',
     '      }\n      if (!shared_memory_->RequestRange(vfetch_constant.address << 2, vfetch_constant.size << 2)) {\n',
     '      }\n      DiagScope diag_scope(kDiagVertexBuffers);\n      if (!shared_memory_->RequestRange(vfetch_constant.address << 2, vfetch_constant.size << 2)) {\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #14',
     '      PROFILE_CMD_BUFFER_STALL();\n      WaitForSingleObject(fence_completion_event_, INFINITE);\n',
     '      PROFILE_CMD_BUFFER_STALL();\n      DiagScope diag_scope(kDiagGpuWait);\n      WaitForSingleObject(fence_completion_event_, INFINITE);\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #15',
     '    texture_cache_->BeginSubmission(submission_current_);\n  }\n',
     '    texture_cache_->BeginSubmission(submission_current_);\n\n    // PARCHE LOCAL - ZPD como contador continuo: seguir midiendo el intervalo\n    // que la submission anterior dejo partido.\n    if (zpd_segment_reopen_ && !active_occlusion_query_.valid && occlusion_query_heap_ &&\n        occlusion_query_resources_available_) {\n      zpd_segment_reopen_ = false;\n      uint32_t host_index = 0;\n      if (AcquireOcclusionQueryIndex(host_index)) {\n        deferred_command_list_.D3DBeginQuery(occlusion_query_heap_.Get(),\n                                             D3D12_QUERY_TYPE_OCCLUSION, host_index);\n        active_occlusion_query_.host_index = host_index;\n        active_occlusion_query_.valid = true;\n      }\n    }\n  }\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #16',
     'bool D3D12CommandProcessor::EndSubmission(bool is_swap) {\n  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();\n',
     'bool D3D12CommandProcessor::EndSubmission(bool is_swap) {\n  DiagScope diag_scope(kDiagSubmit);\n  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #17',
     '\n    if (active_occlusion_query_.valid && occlusion_query_heap_) {\n      deferred_command_list_.D3DEndQuery(occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,\n                                         active_occlusion_query_.host_index);\n      active_occlusion_query_ = {};\n    }\n',
     '\n    // PARCHE LOCAL - ZPD como contador continuo: el intervalo en curso se\n    // parte aqui y sigue midiendo en la proxima submission.\n    if (active_occlusion_query_.valid) {\n      ZpdCloseSegment();\n      zpd_segment_reopen_ = true;\n    }\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #18',
     '  DisableHostOcclusionQueries();\n\n',
     '  DisableHostOcclusionQueries();\n  zpd_reports_.clear();\n  zpd_interval_segments_.clear();\n\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #19',
     '  }\n  occlusion_query_cursor_ = 0;\n',
     '  }\n  zpd_segment_reopen_ = false;\n  zpd_interval_segments_.clear();\n  occlusion_query_cursor_ = 0;\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #20',
     '\nbool D3D12CommandProcessor::BeginGuestOcclusionQuery(uint32_t sample_count_address) {\n  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_query_resources_available_) {\n    return false;\n  }\n  if (active_occlusion_query_.valid) {\n    REXGPU_WARN(\n        "D3D12CommandProcessor: Occlusion query begin issued while another query is active");\n    DisableHostOcclusionQueries();\n    return false;\n  }\n\n  uint32_t host_index = 0;\n  if (!AcquireOcclusionQueryIndex(host_index)) {\n    return false;\n  }\n  if (!BeginSubmission(true)) {\n    return false;\n  }\n\n  deferred_command_list_.D3DBeginQuery(occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,\n                                       host_index);\n  active_occlusion_query_.sample_count_address = sample_count_address;\n  active_occlusion_query_.host_index = host_index;\n  active_occlusion_query_.valid = true;\n  return true;\n}\n\nbool D3D12CommandProcessor::EndGuestOcclusionQuery(\n    uint32_t sample_count_address, xenos::xe_gpu_depth_sample_counts* sample_counts) {\n  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_query_resources_available_ ||\n      !active_occlusion_query_.valid || !occlusion_query_heap_ || !occlusion_query_readback_) {\n    return false;\n  }\n\n  uint32_t host_index = active_occlusion_query_.host_index;\n  active_occlusion_query_ = {};\n\n  if (!BeginSubmission(true)) {\n    return false;\n  }\n\n  deferred_command_list_.D3DEndQuery(occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,\n',
     '\n// PARCHE LOCAL - ZPD como contador continuo: abre un segmento de medida en la\n// submission actual (una consulta de D3D12 no puede cruzar listas de comandos).\nvoid D3D12CommandProcessor::ZpdOpenSegment() {\n  zpd_segment_reopen_ = false;\n  if (active_occlusion_query_.valid || !occlusion_query_resources_available_ ||\n      !occlusion_query_heap_) {\n    return;\n  }\n  uint32_t host_index = 0;\n  if (!AcquireOcclusionQueryIndex(host_index) || !BeginSubmission(true)) {\n    return;\n  }\n  deferred_command_list_.D3DBeginQuery(occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,\n                                       host_index);\n  active_occlusion_query_.host_index = host_index;\n  active_occlusion_query_.valid = true;\n}\n\n// Cierra el segmento abierto y lo apunta en el intervalo en curso. La\n// resolucion va en la misma submission, asi que el resultado esta en el\n// readback en cuanto esa submission termina en la GPU.\nvoid D3D12CommandProcessor::ZpdCloseSegment() {\n  if (!active_occlusion_query_.valid || !occlusion_query_heap_ || !occlusion_query_readback_) {\n    active_occlusion_query_ = {};\n    return;\n  }\n  const uint32_t host_index = active_occlusion_query_.host_index;\n  active_occlusion_query_ = {};\n  deferred_command_list_.D3DEndQuery(occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #21',
     '      occlusion_query_readback_.Get(), sizeof(uint64_t) * host_index);\n\n  if (!EndSubmission(false)) {\n    return false;\n  }\n\n  uint64_t query_submission = submission_current_ ? submission_current_ - 1 : 0;\n  CheckSubmissionFence(query_submission);\n  if (submission_completed_ < query_submission) {\n    return false;\n  }\n  if (!occlusion_query_readback_mapping_) {\n    return false;\n  }\n\n  uint64_t samples = occlusion_query_readback_mapping_[host_index];\n  samples = NormalizeOcclusionSamples(samples);\n  WriteGuestOcclusionResult(sample_counts, samples);\n  return true;\n}\n',
     '      occlusion_query_readback_.Get(), sizeof(uint64_t) * host_index);\n  ZpdSegment segment;\n  segment.host_index = host_index;\n  segment.submission = submission_current_;\n  zpd_interval_segments_.push_back(segment);\n}\n\n// Entrega los informes en orden: cada uno lleva el valor del contador continuo\n// tras sumar las muestras de su intervalo. El D3D del juego resta dos\n// informes; aqui solo hace falta que el contador avance como el de la Xenos.\nvoid D3D12CommandProcessor::ProcessPendingOcclusionQueries(bool wait) {\n  if (zpd_reports_.empty()) {\n    return;\n  }\n  CheckSubmissionFence(0);\n  while (!zpd_reports_.empty()) {\n    ZpdReport& report = zpd_reports_.front();\n    uint64_t needed = 0;\n    for (const ZpdSegment& segment : report.segments) {\n      needed = std::max(needed, segment.submission);\n    }\n    if (!report.segments.empty() && submission_completed_ < needed) {\n      if (!wait) {\n        break;\n      }\n      const uint64_t wait_start = rex::chrono::Clock::QueryHostTickCount();\n      CheckSubmissionFence(needed);\n      FrameDiagAddGpuWait(rex::chrono::Clock::QueryHostTickCount() - wait_start);\n      if (submission_completed_ < needed) {\n        break;\n      }\n      ++g_occlusion_stats.waited;\n    }\n    uint64_t delta = 0;\n    if (occlusion_query_readback_mapping_) {\n      for (const ZpdSegment& segment : report.segments) {\n        delta += occlusion_query_readback_mapping_[segment.host_index];\n      }\n    }\n    delta = NormalizeOcclusionSamples(delta);\n    zpd_counter_ += uint32_t(delta);\n    g_occlusion_stats.delta_max = std::max(g_occlusion_stats.delta_max, delta);\n    ++g_occlusion_stats.delivered;\n    auto* sample_counts =\n        memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(report.address);\n    WriteGuestOcclusionResult(sample_counts, zpd_counter_);\n    zpd_reports_.pop_front();\n  }\n}\n\nvoid D3D12CommandProcessor::PrepareForWait() {\n  CommandProcessor::PrepareForWait();\n  // El guest se queda esperando -sin comandos nuevos o en un WAIT_REG_MEM-:\n  // puede que este esperando justo uno de estos resultados, asi que se espera\n  // a la GPU y se entregan todos.\n  ProcessPendingOcclusionQueries(true);\n}\n'),
    ('src/kernel/xboxkrnl/xboxkrnl_video.cpp',
     'xboxkrnl/xboxkrnl_video.cpp #1',
     '\n#include <rex/cvar.h>\n#include <rex/graphics/pipeline/texture/info.h>\n',
     '\n#include <rex/chrono/clock.h>\n#include <rex/cvar.h>\n#include <rex/graphics/frame_pacer.h>  // PARCHE LOCAL\n#include <rex/graphics/pipeline/texture/info.h>\n'),
    ('src/kernel/xboxkrnl/xboxkrnl_video.cpp',
     'xboxkrnl/xboxkrnl_video.cpp #2',
     '                  mapped_u32 height) {\n  // All of these parameters are REQUIRED.\n',
     '                  mapped_u32 height) {\n  // PARCHE LOCAL - latencia: momento en que el juego entrega el fotograma,\n  // antes de cualquier espera del ritmo.\n  const uint64_t swap_ticks = rex::chrono::Clock::QueryHostTickCount();\n\n  // PARCHE LOCAL - ritmo de VdSwap visto desde el guest (diagnostico, con\n  // log_guest_fps). Compararlo con el del procesador de comandos dice si los\n  // tirones los mete el juego o los mete la emulacion de la GPU.\n  static bool log_fps = rex::cvar::GetFlagInfo("log_guest_fps") != nullptr;\n  if (log_fps && rex::cvar::Query<bool>("log_guest_fps")) {\n    using clk = std::chrono::steady_clock;\n    static clk::time_point window_start{}, last{};\n    static uint32_t swaps = 0, over25 = 0, over50 = 0;\n    static double min_ms = 1e9, max_ms = 0.0;\n    const auto now = clk::now();\n    if (window_start == clk::time_point{}) window_start = now;\n    if (last != clk::time_point{}) {\n      const double ms = std::chrono::duration<double, std::milli>(now - last).count();\n      min_ms = std::min(min_ms, ms);\n      max_ms = std::max(max_ms, ms);\n      over25 += ms > 25.0;\n      over50 += ms > 50.0;\n    }\n    last = now;\n    ++swaps;\n    const double win = std::chrono::duration<double>(now - window_start).count();\n    if (win >= 10.0) {\n      REXKRNL_INFO("[VdSwap fps] {:.1f} swaps/s | frame {:.1f}-{:.1f} ms | >25ms {} | >50ms {}",\n                   swaps / win, min_ms, max_ms, over25, over50);\n      window_start = now;\n      swaps = over25 = over50 = 0;\n      min_ms = 1e9;\n      max_ms = 0.0;\n    }\n  }\n\n  // All of these parameters are REQUIRED.\n'),
    ('src/kernel/xboxkrnl/xboxkrnl_video.cpp',
     'xboxkrnl/xboxkrnl_video.cpp #3',
     '\n  dwords[offset++] = xenos::MakePacketType3(xenos::PM4_XE_SWAP, 4);\n  dwords[offset++] = rex::graphics::xenos::kSwapSignature;\n',
     '\n  // PARCHE LOCAL - latencia: cuatro palabras mas (reloj de host): el momento\n  // del VdSwap y el momento en que se suelta el fotograma tras la espera del\n  // ritmo (se rellena abajo; el procesador de comandos no lee el paquete hasta\n  // que el juego avanza el puntero de escritura, despues de volver de aqui).\n  // Con ellas el log dice cuanto tarda un fotograma ya entregado en llegar a\n  // la pantalla ([latencia]) y el procesador de comandos presenta cada\n  // fotograma a la misma distancia de su suelta.\n  dwords[offset++] = xenos::MakePacketType3(xenos::PM4_XE_SWAP, 8);\n  dwords[offset++] = rex::graphics::xenos::kSwapSignature;\n'),
    ('src/kernel/xboxkrnl/xboxkrnl_video.cpp',
     'xboxkrnl/xboxkrnl_video.cpp #4',
     '  dwords[offset++] = *height;\n\n',
     '  dwords[offset++] = *height;\n  dwords[offset++] = uint32_t(swap_ticks);\n  dwords[offset++] = uint32_t(swap_ticks >> 32);\n  const uint32_t release_offset = offset;\n  dwords[offset++] = uint32_t(swap_ticks);\n  dwords[offset++] = uint32_t(swap_ticks >> 32);\n\n'),
    ('src/kernel/xboxkrnl/xboxkrnl_video.cpp',
     'xboxkrnl/xboxkrnl_video.cpp #5',
     '    dwords[i] = xenos::MakePacketType2();\n  }\n}\n\n',
     '    dwords[i] = xenos::MakePacketType2();\n  }\n\n  // PARCHE LOCAL - ritmo en el hilo del juego (frame_pacing_at_guest).\n  //\n  // El juego espera aqui su turno, como en la Xbox, donde el D3D del juego\n  // se bloqueaba al entregar el fotograma. Si el ritmo se marca en el\n  // procesador de comandos, el juego se adelanta y el anillo guarda ~3\n  // fotogramas: MEDIDO 49,8 ms desde este VdSwap hasta la salida. Los\n  // ajustes son del plugin de GPU (otro DLL): se leen por nombre.\n  static const bool pacing_cvars = rex::cvar::GetFlagInfo("frame_pacing_at_guest") != nullptr &&\n                                   rex::cvar::GetFlagInfo("frame_pacing_fps") != nullptr;\n  const bool pace_here = pacing_cvars && rex::cvar::Query<bool>("frame_pacing_at_guest");\n\n  // PARCHE LOCAL - baja latencia (frame_pacing_low_latency), la idea de\n  // Reflex: el juego no empieza el fotograma siguiente -y con el, la lectura\n  // del mando- hasta que el anterior ha salido hacia el presentador. Cuando\n  // el cuello de botella es la emulacion de la GPU (120 fps en escenas\n  // cargadas) el juego si no se adelanta y los fotogramas esperan en el\n  // anillo: MEDIDO 15-21 ms desde la suelta hasta la salida en vez de ~3.\n  // El plugin de GPU publica el VdSwap del ultimo fotograma que salio\n  // (rex_gpu_last_output_vdswap_ticks); tope de 100 ms por si acaso.\n  static uint64_t prev_swap_ticks = 0;\n#if defined(_WIN32)\n  static const bool low_latency_cvar =\n      rex::cvar::GetFlagInfo("frame_pacing_low_latency") != nullptr;\n  if (low_latency_cvar && prev_swap_ticks && rex::cvar::Query<bool>("frame_pacing_low_latency")) {\n    using LastOutputFn = uint64_t (*)();\n    static LastOutputFn last_output = [] {\n      HMODULE gpu = GetModuleHandleW(L"rexgpu-xenos.dll");\n      return gpu ? reinterpret_cast<LastOutputFn>(\n                       GetProcAddress(gpu, "rex_gpu_last_output_vdswap_ticks"))\n                 : nullptr;\n    }();\n    if (last_output && last_output() < prev_swap_ticks) {\n      const uint64_t freq = rex::chrono::Clock::QueryHostTickFrequency();\n      const uint64_t wait_start = rex::chrono::Clock::QueryHostTickCount();\n      const uint64_t deadline = wait_start + freq / 10;\n      uint64_t now = wait_start;\n      while (last_output() < prev_swap_ticks && now < deadline) {\n        // Los primeros 2 ms cediendo el hilo; despues, a trozos de 1 ms.\n        if (now - wait_start < freq / 500) {\n          rex::thread::MaybeYield();\n        } else {\n          rex::thread::Sleep(std::chrono::milliseconds(1));\n        }\n        now = rex::chrono::Clock::QueryHostTickCount();\n      }\n    }\n  }\n#endif\n  prev_swap_ticks = swap_ticks;\n\n  // PARCHE LOCAL - ritmo adaptativo (frame_pacing_adaptive).\n  //\n  // Si la escena no llega al objetivo, el ritmo no tiene nada que esperar y\n  // cada fotograma sale cuando esta: MEDIDO con CapFrameX a 120 fps en la\n  // ciudad, 75-117 fps con fotogramas de 8 a 15 ms alternando (desviacion\n  // 1,7 ms), porque la emulacion de la GPU no da mas. Aqui se mide lo que\n  // cuesta de verdad cada fotograma -desde la suelta del anterior hasta que\n  // este esta listo, incluida la espera de baja latencia, que es la que\n  // refleja a la emulacion de la GPU- y el ritmo apunta al percentil 90 de\n  // los ultimos 90 (+2 %), nunca por debajo del objetivo. Sube enseguida si\n  // la escena se complica y baja despacio (0,05 ms por fotograma) para no\n  // oscilar.\n  static uint64_t prev_release_ticks = 0;\n  static double demand_ms[90] = {};\n  static uint32_t demand_n = 0;\n  static double adaptive_ms = 0.0;\n  static double paced_sum_ms = 0.0;\n  static uint32_t paced_n = 0;\n  if (pace_here) {\n    const int32_t fps = rex::cvar::Query<int32_t>("frame_pacing_fps");\n    if (fps > 0) {\n      const uint64_t freq = rex::chrono::Clock::QueryHostTickFrequency();\n      const double target_ms = 1000.0 / double(fps);\n      double period_ms = target_ms;\n      static const bool adaptive_cvar = rex::cvar::GetFlagInfo("frame_pacing_adaptive") != nullptr;\n      if (adaptive_cvar && rex::cvar::Query<bool>("frame_pacing_adaptive")) {\n        const uint64_t ready = rex::chrono::Clock::QueryHostTickCount();\n        if (prev_release_ticks) {\n          const double demand = double(ready - prev_release_ticks) * 1000.0 / double(freq);\n          if (demand < 100.0) {  // loads and pauses do not count\n            demand_ms[demand_n++ % 90] = demand;\n          }\n        }\n        const uint32_t n = std::min<uint32_t>(demand_n, 90);\n        if (n >= 30) {\n          double sorted[90];\n          std::copy(demand_ms, demand_ms + n, sorted);\n          std::sort(sorted, sorted + n);\n          const double wanted = std::max(target_ms, sorted[n * 9 / 10] * 1.02);\n          adaptive_ms = (adaptive_ms <= 0.0 || wanted > adaptive_ms)\n                            ? wanted\n                            : std::max(wanted, adaptive_ms - 0.05);\n          period_ms = std::min(adaptive_ms, 50.0);\n        }\n      }\n      paced_sum_ms += period_ms;\n      ++paced_n;\n      // Every 10 s with log_guest_fps: the rate actually paced at.\n      static uint64_t pace_log_ticks = 0;\n      const uint64_t now_ticks = rex::chrono::Clock::QueryHostTickCount();\n      if (!pace_log_ticks) {\n        pace_log_ticks = now_ticks;\n      } else if (now_ticks - pace_log_ticks >= freq * 10) {\n        if (rex::cvar::Query<bool>("log_guest_fps")) {\n          REXKRNL_INFO("[pacing] paced at {:.1f} fps on average (target {} fps{})",\n                       1000.0 * paced_n / paced_sum_ms, fps,\n                       adaptive_ms > target_ms + 0.01 ? ", adaptive" : "");\n        }\n        pace_log_ticks = now_ticks;\n        paced_sum_ms = 0.0;\n        paced_n = 0;\n      }\n      const int32_t lock = rex::cvar::Query<int32_t>("frame_pacing_display_lock");\n      const bool vsync = rex::cvar::Query<bool>("vsync");\n      rex::graphics::PaceFrame(1000.0 / period_ms, lock == 2 || (lock == 1 && vsync),\n                               rex::cvar::Query<int32_t>("frame_pacing_phase"));\n    }\n  }\n\n  const uint64_t release_ticks = rex::chrono::Clock::QueryHostTickCount();\n  prev_release_ticks = release_ticks;\n  dwords[release_offset] = uint32_t(release_ticks);\n  dwords[release_offset + 1] = uint32_t(release_ticks >> 32);\n}\n\n'),
    ('include/rex/ui/presenter.h',
     'ui/presenter.h #1',
     ' protected:\n  enum class PaintResult {\n',
     ' protected:\n  // PARCHE LOCAL - momento (steady_clock, ns) de la ultima imagen del guest.\n  int64_t GetGuestOutputLastRefreshNs() const {\n    return guest_output_last_refresh_ns_.load(std::memory_order_relaxed);\n  }\n\n  enum class PaintResult {\n'),
    ('include/rex/ui/presenter.h',
     'ui/presenter.h #2',
     '  bool guest_output_active_last_refresh_ = false;\n\n',
     '  bool guest_output_active_last_refresh_ = false;\n  // PARCHE LOCAL - un present por fotograma del guest. Momento (steady_clock,\n  // en ms) del ultimo RefreshGuestOutput; mientras el guest siga entregando\n  // imagenes, la UI se repinta con ellas y no por su cuenta en cada vblank.\n  std::atomic<int64_t> guest_output_last_refresh_ms_{0};\n  // Lo mismo en ns, para medir la latencia hasta el Present ([present]).\n  std::atomic<int64_t> guest_output_last_refresh_ns_{0};\n  // La UI queria repintarse pero se dejo para el siguiente fotograma del guest.\n  std::atomic<bool> ui_paint_deferred_{false};\n  bool IsGuestOutputFlowing() const;\n\n'),
    ('src/ui/presenter.cpp',
     'ui/presenter.cpp #1',
     '#include <cctype>\n#include <utility>\n',
     '#include <cctype>\n#include <chrono>\n#include <utility>\n'),
    ('src/ui/presenter.cpp',
     'ui/presenter.cpp #2',
     '                    "Allow presentation from non-UI thread");\n\n',
     '                    "Allow presentation from non-UI thread");\n\n// PARCHE LOCAL - un present por fotograma del guest\n// Con cualquier dialogo de ImGui registrado (y la notificacion de logros lo\n// esta siempre) el hilo de UI repintaba en cada vblank del monitor ademas de en\n// cada fotograma del guest: 120-300 presents por segundo para 60 imagenes. Eso\n// saca a G-Sync/FreeSync de su rango (tearing) y, con vsync, reparte las\n// imagenes del guest entre refrescos de forma irregular (tirones).\nREXCVAR_DEFINE_BOOL(present_ui_with_guest_frames, true, "UI/Presenter",\n                    "While the game delivers frames, repaint the UI only "\n                    "with them (one present per game frame)");\n\n'),
    ('src/ui/presenter.cpp',
     'ui/presenter.cpp #3',
     '    if (request_ui_paint_after_current_ui_thread_paint_ && !ui_drawers_.empty()) {\n      request_repaint_at_tick = true;\n    }\n',
     '    if (request_ui_paint_after_current_ui_thread_paint_ && !ui_drawers_.empty()) {\n      if (IsGuestOutputFlowing()) {\n        // El siguiente fotograma del guest repinta la UI; si no llega, el hilo\n        // de ticks lo pide en cuanto el guest deje de entregar.\n        ui_paint_deferred_.store(true, std::memory_order_relaxed);\n      } else {\n        request_repaint_at_tick = true;\n      }\n    }\n'),
    ('src/ui/presenter.cpp',
     'ui/presenter.cpp #4',
     '    guest_output_mailbox_writable_ = (3 - last_acquired - guest_output_mailbox_writable_) % 3;\n  }\n',
     '    guest_output_mailbox_writable_ = (3 - last_acquired - guest_output_mailbox_writable_) % 3;\n  }\n\n  {\n    const auto refresh_now = std::chrono::steady_clock::now().time_since_epoch();\n    guest_output_last_refresh_ms_.store(\n        std::chrono::duration_cast<std::chrono::milliseconds>(refresh_now).count(),\n        std::memory_order_relaxed);\n    guest_output_last_refresh_ns_.store(\n        std::chrono::duration_cast<std::chrono::nanoseconds>(refresh_now).count(),\n        std::memory_order_relaxed);\n  }\n'),
    ('src/ui/presenter.cpp',
     'ui/presenter.cpp #5',
     "  if (!ui_drawers_.empty() && paint_mode_ != PaintMode::kNone) {\n    // The window must be present, otherwise the conditions wouldn't have been\n",
     "  if (!ui_drawers_.empty() && paint_mode_ != PaintMode::kNone) {\n    if (IsGuestOutputFlowing()) {\n      ui_paint_deferred_.store(true, std::memory_order_relaxed);\n      return;\n    }\n    // The window must be present, otherwise the conditions wouldn't have been\n"),
    ('src/ui/presenter.cpp',
     'ui/presenter.cpp #6',
     '    window_->RequestPaint();\n  }\n}\n\n',
     '    window_->RequestPaint();\n  }\n}\n\nbool Presenter::IsGuestOutputFlowing() const {\n  if (!REXCVAR_GET(present_ui_with_guest_frames)) {\n    return false;\n  }\n  // Hasta 100 ms sin imagen nueva se sigue considerando que el guest entrega\n  // (cubre 10 fps); pasado eso la UI vuelve a repintarse sola en cada vblank\n  // para que un menu siga vivo aunque el juego este cargando.\n  const int64_t ultimo = guest_output_last_refresh_ms_.load(std::memory_order_relaxed);\n  if (ultimo == 0) {\n    return false;\n  }\n  const int64_t ahora = std::chrono::duration_cast<std::chrono::milliseconds>(\n                            std::chrono::steady_clock::now().time_since_epoch())\n                            .count();\n  return ahora - ultimo < 100;\n}\n\n'),
    ('src/ui/presenter.cpp',
     'ui/presenter.cpp #7',
     '    dxgi_ui_tick_signal_condition_.notify_all();\n  }\n',
     '    dxgi_ui_tick_signal_condition_.notify_all();\n    // PARCHE LOCAL - un present por fotograma del guest: si la UI aplazo su\n    // repintado esperando al guest y este ha dejado de entregar imagenes, se\n    // pide aqui. Sin el mutex de ticks tomado, porque RefreshGuestOutput toma\n    // primero paint_mode_mutex_ y luego el de ticks.\n    if (ui_paint_deferred_.load(std::memory_order_relaxed) && !IsGuestOutputFlowing()) {\n      dxgi_ui_tick_lock.unlock();\n      {\n        std::lock_guard<std::mutex> paint_mode_lock(paint_mode_mutex_);\n        if (paint_mode_ == PaintMode::kUIThreadOnRequest &&\n            ui_paint_deferred_.exchange(false, std::memory_order_relaxed)) {\n          RequestPaintOrConnectionRecoveryViaWindow(false);\n        }\n      }\n      dxgi_ui_tick_lock.lock();\n    }\n  }\n'),
    ('src/ui/d3d12/d3d12_presenter.cpp',
     'd3d12/d3d12_presenter.cpp #1',
     '#include <climits>\n#include <cmath>\n',
     '#include <climits>\n#include <cstdio>      // PARCHE LOCAL - frame times\n#include <ctime>       // PARCHE LOCAL - frame times\n#include <filesystem>  // PARCHE LOCAL - frame times\n#include <string>      // PARCHE LOCAL - frame times\n#include <cmath>\n'),
    ('src/ui/d3d12/d3d12_presenter.cpp',
     'd3d12/d3d12_presenter.cpp #2',
     '// tocaba el SyncInterval del Present. Este es nuevo.\nREXCVAR_DEFINE_INT32(max_fps, 0, "UI/Present",\n',
     '// tocaba el SyncInterval del Present. Este es nuevo.\n// PARCHE LOCAL - frame time recording. With frame_times_dir set, every\n// recording (frame_times_recording, toggled with F10 by the app) becomes\n// frametimes_<date>_<time>.csv there: one row per present with the present time\n// and, from DXGI\'s frame statistics, when the frame actually reached the\n// display. tools/analyze_frametimes.py turns it into numbers and a chart.\n// Measured this way because PresentMon needs admin rights and the build that\n// ships with FrameView exits silently even with them.\nREXCVAR_DEFINE_STRING(frame_times_dir, "", "UI/Present",\n                      "Folder for frame time recordings (F10 in game). Empty = off.")\n    .lifecycle(rex::cvar::Lifecycle::kHotReload);\nREXCVAR_DEFINE_BOOL(frame_times_recording, false, "UI/Present",\n                    "Record one CSV row per present into frame_times_dir.")\n    .lifecycle(rex::cvar::Lifecycle::kHotReload);\n\nREXCVAR_DEFINE_INT32(max_fps, 0, "UI/Present",\n'),
    ('src/ui/d3d12/d3d12_presenter.cpp',
     'd3d12/d3d12_presenter.cpp #3',
     '}  // namespace shaders\n\n',
     '}  // namespace shaders\n\nnamespace {\n\n// PARCHE LOCAL - frame time recording (see frame_times_dir). Only the present\n// thread calls this, so the state needs no locking.\nvoid RecordFrameTime(IDXGISwapChain3* swap_chain, bool vsync) {\n  static FILE* file = nullptr;\n  static std::string path;\n  static uint32_t rows = 0;\n  static LARGE_INTEGER freq = [] {\n    LARGE_INTEGER f;\n    QueryPerformanceFrequency(&f);\n    return f;\n  }();\n\n  const bool want = rex::cvar::GetFlagInfo("frame_times_recording") != nullptr &&\n                    rex::cvar::Query<bool>("frame_times_recording") &&\n                    !rex::cvar::Query<std::string>("frame_times_dir").empty();\n  if (!want) {\n    if (file) {\n      std::fclose(file);\n      file = nullptr;\n      REXLOG_INFO("[frametimes] recording stopped: {} frames in {}", rows, path);\n    }\n    return;\n  }\n  if (!file) {\n    const std::filesystem::path dir = rex::cvar::Query<std::string>("frame_times_dir");\n    std::error_code ec;\n    std::filesystem::create_directories(dir, ec);\n    const std::time_t now = std::time(nullptr);\n    std::tm local = {};\n    localtime_s(&local, &now);\n    char name[64];\n    std::strftime(name, sizeof(name), "frametimes_%Y%m%d_%H%M%S.csv", &local);\n    path = (dir / name).string();\n    file = std::fopen(path.c_str(), "w");\n    if (!file) {\n      REXLOG_WARN("[frametimes] cannot write {}; recording off", path);\n      rex::cvar::SetFlagByName("frame_times_recording", "false");\n      return;\n    }\n    std::setvbuf(file, nullptr, _IOFBF, 1 << 16);\n    std::fputs("present,present_ms,displayed_present,display_ms,target_fps,vsync\\n", file);\n    rows = 0;\n    REXLOG_INFO("[frametimes] recording to {}", path);\n  }\n\n  LARGE_INTEGER now;\n  QueryPerformanceCounter(&now);\n  UINT present = 0;\n  swap_chain->GetLastPresentCount(&present);\n  // The latest present that reached the display, and when (the vblank or, with\n  // VRR, the refresh it was scanned out on). Not always available: then 0.\n  DXGI_FRAME_STATISTICS stats = {};\n  const bool have_stats = SUCCEEDED(swap_chain->GetFrameStatistics(&stats));\n  const int32_t target =\n      rex::cvar::GetFlagInfo("frame_pacing_fps") ? rex::cvar::Query<int32_t>("frame_pacing_fps")\n                                                 : 0;\n  std::fprintf(file, "%u,%.4f,%u,%.4f,%d,%d\\n", present,\n               double(now.QuadPart) * 1000.0 / double(freq.QuadPart),\n               have_stats ? stats.PresentCount : 0u,\n               have_stats ? double(stats.SyncQPCTime.QuadPart) * 1000.0 / double(freq.QuadPart)\n                          : 0.0,\n               target, vsync ? 1 : 0);\n  if (++rows % 120 == 0) {\n    std::fflush(file);\n  }\n}\n\n}  // namespace\n\n'),
    ('src/ui/d3d12/d3d12_presenter.cpp',
     'd3d12/d3d12_presenter.cpp #4',
     '  HRESULT present_result = paint_context_.swap_chain->Present(sync_interval, present_flags);\n  // Even if presentation has failed, work might have been enqueued anyway\n',
     '  HRESULT present_result = paint_context_.swap_chain->Present(sync_interval, present_flags);\n  // PARCHE LOCAL - frame times, one CSV row per present (frame_times_dir, F10).\n  RecordFrameTime(paint_context_.swap_chain.Get(), con_vsync);\n  // PARCHE LOCAL - presents por segundo en el log (con log_guest_fps), para\n  // comprobar que hay uno por fotograma del guest.\n  {\n    static bool consultado = false;\n    static bool registrar = false;\n    if (!consultado) {\n      consultado = true;\n      registrar = rex::cvar::GetFlagInfo("log_guest_fps") != nullptr &&\n                  rex::cvar::Query<bool>("log_guest_fps");\n    }\n    if (registrar) {\n      using Reloj = std::chrono::steady_clock;\n      static Reloj::time_point inicio = Reloj::now();\n      static Reloj::time_point previo = inicio;\n      static uint32_t cuenta = 0;\n      static double min_ms = 1e9, max_ms = 0.0;\n      static double edad_sum = 0.0, edad_max = 0.0;\n      static uint32_t edad_n = 0;\n      const auto ahora = Reloj::now();\n      // Latencia: cuanto lleva esperando la imagen del guest (desde\n      // RefreshGuestOutput) cuando vuelve el Present.\n      if (const int64_t refresco_ns = GetGuestOutputLastRefreshNs(); refresco_ns > 0) {\n        const double edad =\n            double(std::chrono::duration_cast<std::chrono::nanoseconds>(ahora.time_since_epoch())\n                       .count() -\n                   refresco_ns) /\n            1e6;\n        if (edad >= 0.0 && edad < 1000.0) {\n          edad_sum += edad;\n          edad_max = std::max(edad_max, edad);\n          ++edad_n;\n        }\n      }\n      if (cuenta != 0) {\n        const double ms = std::chrono::duration<double, std::milli>(ahora - previo).count();\n        min_ms = std::min(min_ms, ms);\n        max_ms = std::max(max_ms, ms);\n      }\n      previo = ahora;\n      ++cuenta;\n      const double transcurrido = std::chrono::duration<double>(ahora - inicio).count();\n      if (transcurrido >= 10.0) {\n        REXLOG_INFO(\n            "[present] {:.1f}/s | interval {:.1f}-{:.1f} ms | vsync {} | image -> Present "\n            "{:.2f} ms avg, {:.2f} max",\n            cuenta / transcurrido, min_ms, max_ms, con_vsync, edad_n ? edad_sum / edad_n : 0.0,\n            edad_max);\n        edad_sum = edad_max = 0.0;\n        edad_n = 0;\n        inicio = ahora;\n        cuenta = 0;\n        min_ms = 1e9;\n        max_ms = 0.0;\n      }\n    }\n  }\n  // Even if presentation has failed, work might have been enqueued anyway\n'),
    ('src/graphics/pipeline/texture/cache.cpp',
     'texture/cache.cpp #1',
     '#include <rex/math.h>\n\n',
     '#include <rex/math.h>\n\n// PARCHE LOCAL - desglose de los fotogramas lentos y estado del cache de\n// texturas (graphics/command_processor.cpp).\nnamespace rex::graphics {\nbool FrameDiagEnabled();\nbool FrameStatsEnabled();\nvoid FrameDiagAddBucket(int bucket, uint64_t ticks);\n}  // namespace rex::graphics\n\nnamespace {\nconstexpr int kDiagTextureCreate = 10;\nconstexpr int kDiagTextureUpload = 11;\nstruct TextureDiagScope {\n  explicit TextureDiagScope(int bucket)\n      : bucket_(bucket),\n        start_(rex::graphics::FrameDiagEnabled() ? rex::chrono::Clock::QueryHostTickCount() : 0) {}\n  ~TextureDiagScope() {\n    if (start_) {\n      rex::graphics::FrameDiagAddBucket(bucket_,\n                                        rex::chrono::Clock::QueryHostTickCount() - start_);\n    }\n  }\n  int bucket_;\n  uint64_t start_;\n};\nuint32_t g_textures_created = 0;\nuint32_t g_textures_destroyed = 0;\nuint64_t g_textures_create_ticks = 0;\n}  // namespace\n\n'),
    ('src/graphics/pipeline/texture/cache.cpp',
     'texture/cache.cpp #2',
     '      textures_.erase(found_texture_it);\n      // `texture` is invalid now.\n',
     '      textures_.erase(found_texture_it);\n      ++g_textures_destroyed;\n      // `texture` is invalid now.\n'),
    ('src/graphics/pipeline/texture/cache.cpp',
     'texture/cache.cpp #3',
     '    COUNT_profile_set("gpu/texture_cache/textures", textures_.size());\n  }\n',
     '    COUNT_profile_set("gpu/texture_cache/textures", textures_.size());\n  }\n\n  // PARCHE LOCAL - estado del cache cada 10 s (con log_guest_fps): si\n  // "descartadas" sube mientras se conduce, las texturas se tiran por los\n  // limites de memoria y luego hay que volver a crearlas (tirones).\n  if (rex::graphics::FrameStatsEnabled()) {\n    static uint64_t last_log = 0;\n    if (!last_log) {\n      last_log = current_time;\n    } else if (current_time - last_log >= 10000) {\n      last_log = current_time;\n      const double create_ms = double(g_textures_create_ticks) * 1000.0 /\n                               double(rex::chrono::Clock::QueryHostTickFrequency());\n      REXGPU_INFO(\n          "[textures] {} cached, {} MB (limits {}/{} MB) | created {} in {:.1f} ms ({:.3f} ms each) | "\n          "discarded {}",\n          textures_.size(), textures_total_host_memory_usage_ >> 20, limit_soft_mb, limit_hard_mb,\n          g_textures_created, create_ms, g_textures_created ? create_ms / g_textures_created : 0.0,\n          g_textures_destroyed);\n      g_textures_create_ticks = 0;\n      g_textures_created = 0;\n      g_textures_destroyed = 0;\n    }\n  }\n'),
    ('src/graphics/pipeline/texture/cache.cpp',
     'texture/cache.cpp #4',
     '    }\n    batched_shared_memory_request_succeeded = shared_memory().RequestRanges(\n',
     '    }\n    TextureDiagScope diag_scope(kDiagTextureUpload);\n    batched_shared_memory_request_succeeded = shared_memory().RequestRanges(\n'),
    ('src/graphics/pipeline/texture/cache.cpp',
     'texture/cache.cpp #5',
     '  {\n    std::unique_ptr<Texture> new_texture = CreateTexture(key);\n    if (!new_texture) {\n',
     '  {\n    TextureDiagScope diag_scope(kDiagTextureCreate);\n    const uint64_t create_start = rex::chrono::Clock::QueryHostTickCount();\n    std::unique_ptr<Texture> new_texture = CreateTexture(key);\n    g_textures_create_ticks += rex::chrono::Clock::QueryHostTickCount() - create_start;\n    ++g_textures_created;\n    if (!new_texture) {\n'),
    ('include/rex/graphics/d3d12/texture_cache.h',
     'd3d12/texture_cache.h #1',
     '#include <functional>\n#include <memory>\n',
     '#include <functional>\n#include <map>\n#include <memory>\n'),
    ('include/rex/graphics/d3d12/texture_cache.h',
     'd3d12/texture_cache.h #2',
     '    }\n\n   private:\n    Microsoft::WRL::ComPtr<ID3D12Resource> resource_;\n    D3D12_RESOURCE_STATES resource_state_;\n',
     '    }\n\n    // PARCHE LOCAL - hueco del heap compartido donde vive la textura, si vive\n    // en uno (se devuelve al destruirla).\n    void SetHeapPlacement(uint32_t heap_index, uint64_t offset, uint64_t size) {\n      heap_index_ = heap_index;\n      heap_offset_ = offset;\n      heap_size_ = size;\n    }\n\n   private:\n    uint32_t heap_index_ = UINT32_MAX;\n    uint64_t heap_offset_ = 0;\n    uint64_t heap_size_ = 0;\n    Microsoft::WRL::ComPtr<ID3D12Resource> resource_;\n    D3D12_RESOURCE_STATES resource_state_;\n'),
    ('include/rex/graphics/d3d12/texture_cache.h',
     'd3d12/texture_cache.h #3',
     '  uint64_t scaled_resolve_current_range_length_scaled_;\n};\n',
     '  uint64_t scaled_resolve_current_range_length_scaled_;\n\n  // PARCHE LOCAL - texturas en heaps compartidos (d3d12_texture_heaps).\n  // CreateCommittedResource crea un heap por textura y cuesta ~0,2 ms: con el\n  // streaming de la ciudad llegaban 50-60 texturas nuevas en un fotograma y\n  // eso era un tiron de 10-12 ms. Aqui se reservan heaps grandes y cada\n  // textura se coloca en un hueco (CreatePlacedResource). Declarado al final\n  // para que los heaps se destruyan despues de las texturas.\n  static constexpr uint64_t kTextureHeapSize = uint64_t(64) << 20;\n  struct TextureHeap {\n    Microsoft::WRL::ComPtr<ID3D12Heap> heap;\n    // Huecos libres: desplazamiento -> tamano.\n    std::map<uint64_t, uint64_t> free_blocks;\n  };\n  std::vector<TextureHeap> texture_heaps_;\n  bool texture_heaps_failed_ = false;\n  bool AddTextureHeap();\n  bool AllocateTextureHeapBlock(uint64_t size, uint64_t alignment, uint32_t& heap_index_out,\n                                uint64_t& offset_out);\n  // La llama ~D3D12Texture (clase anidada, tiene acceso).\n  void FreeTextureHeapBlock(uint32_t heap_index, uint64_t offset, uint64_t size);\n};\n'),
    ('src/graphics/d3d12/texture_cache.cpp',
     'd3d12/texture_cache.cpp #1',
     '#include <rex/ui/d3d12/d3d12_util.h>\n\n',
     '#include <rex/ui/d3d12/d3d12_util.h>\n#include <rex/cvar.h>\n\n// PARCHE LOCAL - texturas en heaps compartidos (ver texture_cache.h).\nREXCVAR_DEFINE_BOOL(d3d12_texture_heaps, true, "GPU/D3D12",\n                    "Place textures in shared 64 MB heaps instead of one committed resource "\n                    "each (creating many textures at once no longer hitches)")\n    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);\n\n'),
    ('src/graphics/d3d12/texture_cache.cpp',
     'd3d12/texture_cache.cpp #2',
     '    d3d12_texture_cache.ReleaseTextureDescriptor(descriptor_pair.second);\n  }\n}\n\n',
     '    d3d12_texture_cache.ReleaseTextureDescriptor(descriptor_pair.second);\n  }\n  // PARCHE LOCAL - devolver el hueco del heap compartido. Las texturas solo se\n  // destruyen cuando la GPU ya no las usa (CompletedSubmissionUpdated) o al\n  // vaciar el cache, asi que el hueco se puede reutilizar.\n  if (heap_index_ != UINT32_MAX) {\n    resource_.Reset();\n    d3d12_texture_cache.FreeTextureHeapBlock(heap_index_, heap_offset_, heap_size_);\n  }\n}\n\nbool D3D12TextureCache::AddTextureHeap() {\n  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();\n  D3D12_HEAP_DESC heap_desc = {};\n  heap_desc.SizeInBytes = kTextureHeapSize;\n  heap_desc.Properties = ui::d3d12::util::kHeapPropertiesDefault;\n  heap_desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;\n  heap_desc.Flags =\n      D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES | provider.GetHeapFlagCreateNotZeroed();\n  TextureHeap texture_heap;\n  if (FAILED(provider.GetDevice()->CreateHeap(&heap_desc, IID_PPV_ARGS(&texture_heap.heap)))) {\n    REXGPU_WARN("[textures] could not create a {} MB heap; one resource per texture from now on",\n                kTextureHeapSize >> 20);\n    texture_heaps_failed_ = true;\n    return false;\n  }\n  texture_heap.free_blocks.emplace(0, kTextureHeapSize);\n  texture_heaps_.push_back(std::move(texture_heap));\n  return true;\n}\n\nbool D3D12TextureCache::AllocateTextureHeapBlock(uint64_t size, uint64_t alignment,\n                                                 uint32_t& heap_index_out, uint64_t& offset_out) {\n  if (texture_heaps_failed_ || size == 0 || size > kTextureHeapSize / 2) {\n    return false;\n  }\n  if (texture_heaps_.empty()) {\n    // Unos cuantos de golpe, que la primera textura llega en la pantalla de\n    // carga y asi no hay que crear heaps mientras se conduce.\n    for (int i = 0; i < 4; ++i) {\n      if (!AddTextureHeap()) {\n        break;\n      }\n    }\n  }\n  for (int attempt = 0; attempt < 2; ++attempt) {\n    for (uint32_t i = 0; i < uint32_t(texture_heaps_.size()); ++i) {\n      auto& free_blocks = texture_heaps_[i].free_blocks;\n      for (auto it = free_blocks.begin(); it != free_blocks.end(); ++it) {\n        const uint64_t block_start = it->first;\n        const uint64_t block_end = it->first + it->second;\n        const uint64_t aligned = (block_start + alignment - 1) / alignment * alignment;\n        if (aligned + size > block_end) {\n          continue;\n        }\n        free_blocks.erase(it);\n        if (aligned > block_start) {\n          free_blocks.emplace(block_start, aligned - block_start);\n        }\n        if (aligned + size < block_end) {\n          free_blocks.emplace(aligned + size, block_end - (aligned + size));\n        }\n        heap_index_out = i;\n        offset_out = aligned;\n        return true;\n      }\n    }\n    if (attempt == 0 && !AddTextureHeap()) {\n      return false;\n    }\n  }\n  return false;\n}\n\nvoid D3D12TextureCache::FreeTextureHeapBlock(uint32_t heap_index, uint64_t offset,\n                                             uint64_t size) {\n  if (heap_index >= texture_heaps_.size()) {\n    return;\n  }\n  auto& free_blocks = texture_heaps_[heap_index].free_blocks;\n  auto it = free_blocks.emplace(offset, size).first;\n  // Juntar con el hueco siguiente y con el anterior.\n  auto next = std::next(it);\n  if (next != free_blocks.end() && it->first + it->second == next->first) {\n    it->second += next->second;\n    free_blocks.erase(next);\n  }\n  if (it != free_blocks.begin()) {\n    auto prev = std::prev(it);\n    if (prev->first + prev->second == it->first) {\n      prev->second += it->second;\n      free_blocks.erase(it);\n    }\n  }\n}\n\n'),
    ('src/graphics/d3d12/texture_cache.cpp',
     'd3d12/texture_cache.cpp #3',
     '  Microsoft::WRL::ComPtr<ID3D12Resource> resource;\n  if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesDefault,\n',
     '  Microsoft::WRL::ComPtr<ID3D12Resource> resource;\n  // PARCHE LOCAL - primero en un heap compartido; si no cabe o falla, como\n  // antes. Alineacion pequena (4 KB) cuando el driver la acepta.\n  if (REXCVAR_GET(d3d12_texture_heaps)) {\n    D3D12_RESOURCE_DESC placed_desc = desc;\n    placed_desc.Alignment = D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT;\n    D3D12_RESOURCE_ALLOCATION_INFO info = device->GetResourceAllocationInfo(0, 1, &placed_desc);\n    if (info.Alignment != D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT) {\n      placed_desc.Alignment = 0;\n      info = device->GetResourceAllocationInfo(0, 1, &placed_desc);\n    }\n    uint32_t heap_index;\n    uint64_t heap_offset;\n    if (info.SizeInBytes != UINT64_MAX &&\n        AllocateTextureHeapBlock(info.SizeInBytes, info.Alignment, heap_index, heap_offset)) {\n      if (SUCCEEDED(device->CreatePlacedResource(texture_heaps_[heap_index].heap.Get(),\n                                                 heap_offset, &placed_desc, resource_state,\n                                                 nullptr, IID_PPV_ARGS(&resource)))) {\n        auto texture = std::unique_ptr<D3D12Texture>(\n            new D3D12Texture(*this, key, resource.Get(), resource_state));\n        texture->SetHeapPlacement(heap_index, heap_offset, info.SizeInBytes);\n        return texture;\n      }\n      FreeTextureHeapBlock(heap_index, heap_offset, info.SizeInBytes);\n    }\n  }\n  if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesDefault,\n'),
    ('include/rex/graphics/shared_memory.h',
     'graphics/shared_memory.h #1',
     '  std::vector<std::pair<uint32_t, uint32_t>> upload_ranges_;\n\n',
     '  std::vector<std::pair<uint32_t, uint32_t>> upload_ranges_;\n  // PARCHE LOCAL - reused by RequestRanges and FilterHotUploads instead of a\n  // new vector per call.\n  std::vector<std::pair<uint32_t, uint32_t>> request_ranges_scratch_;\n  std::vector<std::pair<uint32_t, uint32_t>> hot_filtered_scratch_;\n\n'),
    ('include/rex/graphics/shared_memory.h',
     'graphics/shared_memory.h #2',
     '  void UnlinkWatchRange(WatchRange* range);\n};\n',
     '  void UnlinkWatchRange(WatchRange* range);\n\n  // PARCHE LOCAL - hot pages (gpu_hot_pages). Pages the game rewrites every\n  // frame cost a write fault on the game thread, a VirtualProtect and a\n  // re-upload of up to 256 KB on the GPU thread, every frame. After a few\n  // faults a page stops being protected: every request compares it with a\n  // shadow copy (4 KB memcmp) and uploads only when it really changed.\n  // Pages with texture watches or GPU-written data never become hot (their\n  // consumers need the write fault), and go back to normal as soon as either\n  // appears. hot_pages_ and hot_fault_count_ are protected by the global\n  // critical region; the shadow copies and streaks belong to the GPU thread.\n  bool hot_pages_enabled_ = false;\n  std::vector<uint64_t> hot_pages_;\n  std::vector<uint8_t> hot_fault_count_;\n  std::vector<uint8_t*> hot_shadow_;\n  std::vector<uint16_t> hot_equal_streak_;\n  uint32_t hot_page_count_ = 0;\n  uint64_t hot_stat_checked_ = 0, hot_stat_unchanged_ = 0, hot_stat_uploaded_ = 0;\n  uint64_t hot_stat_classic_pages_ = 0;\n  bool IsPageHot(uint32_t page) const {\n    return (hot_pages_[page >> 6] >> (page & 63)) & 1;\n  }\n  bool PageHasRangeWatch(uint32_t page) const;\n  void CoolPages(uint32_t page_first, uint32_t page_last);\n  void FilterHotUploads();\n};\n'),
    ('src/graphics/shared_memory.cpp',
     'graphics/shared_memory.cpp #1',
     '#include <rex/bit.h>\n#include <rex/dbg.h>\n#include <rex/graphics/shared_memory.h>\n#include <rex/math.h>\n#include <rex/memory.h>\n\nnamespace rex::graphics {\n\n',
     '#include <rex/bit.h>\n#include <rex/chrono/clock.h>\n#include <rex/cvar.h>\n#include <rex/dbg.h>\n#include <rex/graphics/shared_memory.h>\n#include <rex/logging.h>\n#include <rex/math.h>\n#include <rex/memory.h>\n\n// PARCHE LOCAL - hot pages (see shared_memory.h).\nREXCVAR_DEFINE_BOOL(gpu_hot_pages, true, "GPU",\n                    "Stop write-protecting guest pages the game rewrites every frame and compare "\n                    "them with a copy instead (fewer write faults, VirtualProtect calls and "\n                    "re-uploads on the GPU thread).")\n    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);\n\nnamespace rex::graphics {\n// graphics/command_processor.cpp: log_guest_fps statistics are enabled.\nbool FrameStatsEnabled();\n\nnamespace {\n// Write faults on a page before it becomes hot, and unchanged checks in a row\n// before it goes back to being protected.\nconstexpr uint8_t kHotFaults = 3;\nconstexpr uint16_t kHotCoolAfter = 240;\n}  // namespace\n\n'),
    ('src/graphics/shared_memory.cpp',
     'graphics/shared_memory.cpp #2',
     '  system_page_flags_valid_and_gpu_written_.assign(num_system_page_flags_, 0);\n\n',
     '  system_page_flags_valid_and_gpu_written_.assign(num_system_page_flags_, 0);\n\n  hot_pages_enabled_ = REXCVAR_GET(gpu_hot_pages) && page_size_log2_ == 12;\n  if (hot_pages_enabled_) {\n    const size_t pages = size_t(kBufferSize) >> page_size_log2_;\n    hot_pages_.assign(num_system_page_flags_, 0);\n    hot_fault_count_.assign(pages, 0);\n    hot_shadow_.assign(pages, nullptr);\n    hot_equal_streak_.assign(pages, 0);\n  }\n\n'),
    ('src/graphics/shared_memory.cpp',
     'graphics/shared_memory.cpp #3',
     '  num_system_page_flags_ = 0;\n}\n',
     '  num_system_page_flags_ = 0;\n\n  for (uint8_t*& shadow : hot_shadow_) {\n    delete[] shadow;\n    shadow = nullptr;\n  }\n  hot_shadow_.clear();\n  hot_pages_.clear();\n  hot_fault_count_.clear();\n  hot_equal_streak_.clear();\n  hot_page_count_ = 0;\n  hot_pages_enabled_ = false;\n}\n\nbool SharedMemory::PageHasRangeWatch(uint32_t page) const {\n  const uint32_t bucket = (page << page_size_log2_) >> kWatchBucketSizeLog2;\n  for (const WatchNode* node = watch_buckets_[bucket]; node != nullptr;\n       node = node->bucket_node_next) {\n    if (page >= node->range->page_first && page <= node->range->page_last) {\n      return true;\n    }\n  }\n  return false;\n}\n\n// In the global critical region. The pages become ordinary again: they stay\n// invalid, so the next request uploads and protects them the classic way.\nvoid SharedMemory::CoolPages(uint32_t page_first, uint32_t page_last) {\n  if (!hot_pages_enabled_ || !hot_page_count_) {\n    return;\n  }\n  for (uint32_t page = page_first; page <= page_last; ++page) {\n    if (IsPageHot(page)) {\n      hot_pages_[page >> 6] &= ~(uint64_t(1) << (page & 63));\n      hot_fault_count_[page] = 0;\n      --hot_page_count_;\n    }\n  }\n}\n'),
    ('src/graphics/shared_memory.cpp',
     'graphics/shared_memory.cpp #4',
     '\n  // Allocate and link the nodes.\n',
     '\n  // PARCHE LOCAL - a watched range needs write faults: its pages stop being hot.\n  CoolPages(watch_page_first, watch_page_last);\n\n  // Allocate and link the nodes.\n'),
    ('src/graphics/shared_memory.cpp',
     'graphics/shared_memory.cpp #5',
     '\n  // Trigger modification callbacks so, for instance, resolved data is loaded to\n',
     '\n  // PARCHE LOCAL - GPU-written data needs the classic path (scaled resolves).\n  if (hot_pages_enabled_ && hot_page_count_) {\n    auto global_lock = global_critical_region_.Acquire();\n    CoolPages(page_first, page_last);\n  }\n\n  // Trigger modification callbacks so, for instance, resolved data is loaded to\n'),
    ('src/graphics/shared_memory.cpp',
     'graphics/shared_memory.cpp #6',
     '  uint32_t valid_block_last = valid_page_last >> 6;\n\n  {\n    auto global_lock = global_critical_region_.Acquire();\n',
     '  uint32_t valid_block_last = valid_page_last >> 6;\n\n  // PARCHE LOCAL - hot pages are uploaded but neither marked valid nor\n  // protected: every request compares them with their copy instead.\n  bool any_hot = false;\n  {\n    auto global_lock = global_critical_region_.Acquire();\n'),
    ('src/graphics/shared_memory.cpp',
     'graphics/shared_memory.cpp #7',
     '      }\n      system_page_flags_valid_[i] |= valid_bits;\n',
     '      }\n      if (hot_page_count_ && (valid_bits & hot_pages_[i])) {\n        valid_bits &= ~hot_pages_[i];\n        any_hot = true;\n      }\n      system_page_flags_valid_[i] |= valid_bits;\n'),
    ('src/graphics/shared_memory.cpp',
     'graphics/shared_memory.cpp #8',
     '\n  if (memory_invalidation_callback_handle_) {\n    memory().EnablePhysicalMemoryAccessCallbacks(\n        valid_page_first << page_size_log2_,\n        (valid_page_last - valid_page_first + 1) << page_size_log2_, true, false);\n  }\n',
     '\n  if (!memory_invalidation_callback_handle_) {\n    return;\n  }\n  if (!any_hot) {\n    memory().EnablePhysicalMemoryAccessCallbacks(\n        valid_page_first << page_size_log2_,\n        (valid_page_last - valid_page_first + 1) << page_size_log2_, true, false);\n    return;\n  }\n  // Protect the runs of pages that are not hot.\n  uint32_t run_first = UINT32_MAX;\n  for (uint32_t page = valid_page_first; page <= valid_page_last + 1; ++page) {\n    const bool protect = page <= valid_page_last && !IsPageHot(page);\n    if (protect && run_first == UINT32_MAX) {\n      run_first = page;\n    } else if (!protect && run_first != UINT32_MAX) {\n      memory().EnablePhysicalMemoryAccessCallbacks(\n          run_first << page_size_log2_, (page - run_first) << page_size_log2_, true, false);\n      run_first = UINT32_MAX;\n    }\n  }\n'),
    ('src/graphics/shared_memory.cpp',
     'graphics/shared_memory.cpp #9',
     '  // Some texture or buffer is empty, for example - safe to draw in this case.\n  std::vector<std::pair<uint32_t, uint32_t>> merged_ranges;\n  merged_ranges.reserve(count);\n  for (size_t i = 0; i < count; ++i) {\n',
     '  // Some texture or buffer is empty, for example - safe to draw in this case.\n  // PARCHE LOCAL - a persistent vector: this runs for nearly every draw, and a\n  // fresh one cost a heap allocation each time (~3 % of the GPU thread with\n  // the other per-call vectors, MEASURED with the sampling profiler).\n  std::vector<std::pair<uint32_t, uint32_t>>& merged_ranges = request_ranges_scratch_;\n  merged_ranges.clear();\n  for (size_t i = 0; i < count; ++i) {\n'),
    ('src/graphics/shared_memory.cpp',
     'graphics/shared_memory.cpp #10',
     '\n  if (upload_ranges_.empty()) {\n',
     '\n  if (hot_page_count_) {\n    FilterHotUploads();\n  }\n  if (hot_pages_enabled_ && FrameStatsEnabled()) {\n    for (const auto& range : upload_ranges_) {\n      hot_stat_classic_pages_ += range.second;\n    }\n    static uint64_t last_log = 0;\n    const uint64_t now = rex::chrono::Clock::QueryHostTickCount();\n    const uint64_t freq = rex::chrono::Clock::QueryHostTickFrequency();\n    if (!last_log) {\n      last_log = now;\n    } else if (now - last_log >= freq * 10) {\n      REXGPU_INFO(\n          "[shared memory] hot pages {} | checked {}, unchanged {}, changed and uploaded {} | "\n          "pages uploaded in total {}",\n          hot_page_count_, hot_stat_checked_, hot_stat_unchanged_, hot_stat_uploaded_,\n          hot_stat_classic_pages_);\n      hot_stat_checked_ = hot_stat_unchanged_ = hot_stat_uploaded_ = hot_stat_classic_pages_ = 0;\n      last_log = now;\n    }\n  }\n\n  if (upload_ranges_.empty()) {\n'),
    ('src/graphics/shared_memory.cpp',
     'graphics/shared_memory.cpp #11',
     '  return UploadRanges(upload_ranges_);\n}\n',
     '  return UploadRanges(upload_ranges_);\n}\n\n// PARCHE LOCAL - hot pages: drop the ones that did not change since they were\n// last uploaded (4 KB memcmp against the copy). A page that keeps being equal\n// goes back to the classic path (protected, invalidated by write faults).\nvoid SharedMemory::FilterHotUploads() {\n  const size_t page_size = size_t(1) << page_size_log2_;\n  std::vector<std::pair<uint32_t, uint32_t>>& filtered = hot_filtered_scratch_;\n  filtered.clear();\n  auto append = [&filtered](uint32_t page) {\n    if (!filtered.empty() && filtered.back().first + filtered.back().second == page) {\n      ++filtered.back().second;\n    } else {\n      filtered.emplace_back(page, 1);\n    }\n  };\n  for (const auto& range : upload_ranges_) {\n    for (uint32_t page = range.first; page < range.first + range.second; ++page) {\n      if (!IsPageHot(page)) {\n        append(page);\n        continue;\n      }\n      ++hot_stat_checked_;\n      const uint8_t* guest = memory().TranslatePhysical<const uint8_t*>(page << page_size_log2_);\n      uint8_t*& shadow = hot_shadow_[page];\n      if (shadow && std::memcmp(shadow, guest, page_size) == 0) {\n        ++hot_stat_unchanged_;\n        if (++hot_equal_streak_[page] >= kHotCoolAfter) {\n          // Quiet page: protect it again. It is uploaded now through the\n          // classic path (MakeRangeValid protects it before the copy).\n          auto global_lock = global_critical_region_.Acquire();\n          CoolPages(page, page);\n          hot_equal_streak_[page] = 0;\n          delete[] shadow;\n          shadow = nullptr;\n          append(page);\n        }\n        continue;\n      }\n      if (!shadow) {\n        shadow = new uint8_t[page_size];\n      }\n      std::memcpy(shadow, guest, page_size);\n      hot_equal_streak_[page] = 0;\n      ++hot_stat_uploaded_;\n      append(page);\n    }\n  }\n  upload_ranges_.swap(filtered);\n}\n'),
    ('src/graphics/shared_memory.cpp',
     'graphics/shared_memory.cpp #12',
     '\n  auto global_lock = global_critical_region_.Acquire();\n\n  if (!exact_range) {\n',
     '\n  auto global_lock = global_critical_region_.Acquire();\n\n  // PARCHE LOCAL - hot pages: count the write faults on uploaded pages (only\n  // the pages really written, before the range is widened below).\n  if (hot_pages_enabled_) {\n    for (uint32_t page = page_first; page <= page_last; ++page) {\n      const uint64_t bit = uint64_t(1) << (page & 63);\n      if (!(system_page_flags_valid_[page >> 6] & bit) ||\n          (system_page_flags_valid_and_gpu_written_[page >> 6] & bit) || IsPageHot(page)) {\n        continue;\n      }\n      if (hot_fault_count_[page] < 255) {\n        ++hot_fault_count_[page];\n      }\n      if (hot_fault_count_[page] >= kHotFaults && !PageHasRangeWatch(page)) {\n        hot_pages_[page >> 6] |= bit;\n        ++hot_page_count_;\n      }\n    }\n  }\n\n  if (!exact_range) {\n'),
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


# Ficheros nuevos del SDK: viven en tools/sdk_nuevos/<ruta en el SDK> y se
# copian tal cual (--revertir los quita si siguen iguales).
NUEVOS = os.path.join(RAIZ, "tools", "sdk_nuevos")


def ficheros_nuevos():
    for base, _, nombres in os.walk(NUEVOS):
        for nombre in nombres:
            origen = os.path.join(base, nombre)
            yield origen, os.path.relpath(origen, NUEVOS)


def main():
    modo = sys.argv[1] if len(sys.argv) > 1 else "--aplicar"
    sdk = buscar_sdk()
    if not sdk:
        print("[ERROR] No encuentro el SDK en ..\\rexglue-sdk ni en .\\sdk")
        return 1

    for origen, rel in ficheros_nuevos():
        destino = os.path.join(sdk, rel)
        contenido = leer(origen)
        igual = os.path.isfile(destino) and leer(destino) == contenido
        if modo == "--estado":
            print(("[ok] aplicado    " if igual else "[--] sin aplicar ") + rel)
        elif modo == "--revertir":
            if igual:
                os.remove(destino)
                print("[ok] Quitado: " + rel)
            else:
                print("[--] No estaba: " + rel)
        elif igual:
            print("[ok] Ya estaba: " + rel)
        else:
            os.makedirs(os.path.dirname(destino), exist_ok=True)
            escribir(destino, contenido)
            print("[ok] Creado: " + rel)

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
