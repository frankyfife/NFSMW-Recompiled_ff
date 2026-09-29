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
     '\nREXCVAR_DEFINE_BOOL(vsync, true, "GPU", "Enable vertical sync");\n\n',
     '\n#if defined(_WIN32)\n#ifndef NOMINMAX\n#define NOMINMAX\n#endif\n#ifndef WIN32_LEAN_AND_MEAN\n#define WIN32_LEAN_AND_MEAN\n#endif\n#include <windows.h>\n#include <dwmapi.h>  // solo el tipo DWM_TIMING_INFO; la funcion se busca en tiempo de ejecucion\n#endif\n\nREXCVAR_DEFINE_BOOL(vsync, true, "GPU", "Enable vertical sync");\n\nREXCVAR_DEFINE_BOOL(log_frame_breakdown, false, "GPU",\n                    "With log_guest_fps: split every late frame by stage (shaders, textures, ...). "\n                    "Costs a few ms per frame in scenes with thousands of draws.")\n    .lifecycle(rex::cvar::Lifecycle::kHotReload);\n\nREXCVAR_DEFINE_BOOL(log_guest_fps, false, "GPU",\n                    "Log how many frames per second the game presents, every 10 seconds");\n\n// PARCHE LOCAL - ritmo de los flips\nREXCVAR_DEFINE_INT32(frame_pacing_fps, 0, "GPU",\n                     "Present guest frames at an exact, even rate (e.g. 30 or 60). Combine with "\n                     "a fast guest_vblank_rate so the game never misses a vblank slot. 0 = off.")\n    .range(0, 1000)\n    .lifecycle(rex::cvar::Lifecycle::kHotReload);\nREXCVAR_DEFINE_INT32(frame_pacing_phase, 25, "GPU",\n                     "Where in the display refresh the paced flips land, in percent after the "\n                     "vblank.")\n    .range(0, 99)\n    .lifecycle(rex::cvar::Lifecycle::kHotReload);\nREXCVAR_DEFINE_INT32(frame_pacing_display_lock, 1, "GPU",\n                     "Lock the paced flips to the display refresh reported by the compositor: "\n                     "0 = never, 1 = only with vsync, 2 = always. With G-Sync/FreeSync the "\n                     "reported refresh follows our own presents, so locking to it makes jitter.")\n    .range(0, 2)\n    .lifecycle(rex::cvar::Lifecycle::kHotReload);\n\n'),
    ('src/graphics/command_processor.cpp',
     'graphics/command_processor.cpp #2',
     '\n}  // namespace\n\n',
     '\n// PARCHE LOCAL - desglose de los fotogramas lentos (con log_guest_fps)\n//\n// Todo en ticks del reloj de host y solo desde el hilo del procesador de\n// comandos, asi que no hace falta sincronizar. Se pone a cero en cada flip.\n// kFrameDiagBuckets tiene que coincidir con el backend (d3d12/command_processor.cpp)\n// y con pipeline/texture/cache.cpp (10 y 11, dentro de "texturas").\nconstexpr int kFrameDiagBuckets = 12;\nstruct FrameDiag {\n  uint64_t idle = 0;         // sin comandos del juego (el juego no llega)\n  uint64_t wait_reg = 0;     // WAIT_REG_MEM sin cumplirse\n  uint32_t wait_reg_n = 0;\n  uint64_t gpu_wait = 0;     // esperando a la GPU del host (occlusion)\n  uint32_t gpu_wait_n = 0;\n  // Apartados del trabajo del procesador de comandos (backend D3D12).\n  uint64_t bucket[kFrameDiagBuckets] = {};\n  uint32_t bucket_n[kFrameDiagBuckets] = {};\n} g_frame_diag;\nbool g_frame_diag_enabled = false;  // apartados (log_frame_breakdown)\nbool g_frame_stats_enabled = false;  // estadisticas baratas (log_guest_fps)\n\nconst char* const kFrameDiagBucketNames[kFrameDiagBuckets] = {\n    "shader", "primitivas", "render targets", "pipeline", "texturas",\n    "bindings", "vertex buffers", "resolve", "espera GPU", "submit",\n    "de ellas crear textura", "de ellas subir memoria"};\n\n}  // namespace\n\nvoid FrameDiagAddGpuWait(uint64_t ticks) {\n  g_frame_diag.gpu_wait += ticks;\n  ++g_frame_diag.gpu_wait_n;\n}\n\nbool FrameDiagEnabled() { return g_frame_diag_enabled; }\nbool FrameStatsEnabled() { return g_frame_stats_enabled; }\n\nvoid FrameDiagAddBucket(int bucket, uint64_t ticks) {\n  g_frame_diag.bucket[bucket] += ticks;\n  ++g_frame_diag.bucket_n[bucket];\n}\n\n'),
    ('src/graphics/command_processor.cpp',
     'graphics/command_processor.cpp #3',
     '      PrepareForWait();\n      uint32_t loop_count = 0;\n',
     '      PrepareForWait();\n      const uint64_t idle_start = rex::chrono::Clock::QueryHostTickCount();\n      uint32_t loop_count = 0;\n'),
    ('src/graphics/command_processor.cpp',
     'graphics/command_processor.cpp #4',
     '               (write_ptr_index == 0xBAADF00D || read_ptr_index_ == write_ptr_index));\n      ReturnFromWait();\n',
     '               (write_ptr_index == 0xBAADF00D || read_ptr_index_ == write_ptr_index));\n      g_frame_diag.idle += rex::chrono::Clock::QueryHostTickCount() - idle_start;\n      ReturnFromWait();\n'),
    ('src/graphics/command_processor.cpp',
     'graphics/command_processor.cpp #5',
     '\n  IssueSwap(frontbuffer_ptr, frontbuffer_width, frontbuffer_height);\n\n',
     '\n  // PARCHE LOCAL - ritmo de los flips (frame_pacing_fps)\n  //\n  // Con vblank de 60 Hz el juego coloca cada flip en una "ranura" de vblank, y\n  // si llega tarde por un pelo pierde la ranura entera: los flips salian cada\n  // 16,6, 33 o 50 ms en vez de cada 33 -el tiron que se veia a "30 fps"-. La\n  // forma buena es darle vblanks rapidos (guest_vblank_rate alto, no pierde\n  // ninguna) y marcar el ritmo aqui con un reloj preciso: un flip cada\n  // 1/frame_pacing_fps segundos, contando desde el flip PREVISTO y no desde el\n  // real, para que un retraso suelto no desplace a todos los demas.\n  //\n  // MEDIDO en el menu (vblank 1000 Hz, sin vsync): 30 fps con flips cada\n  // 33,1-33,5 ms, 60 fps con 16,4-16,9 ms. Con vsync del host encima vuelve a\n  // haber dos relojes y temblor de un refresco del monitor.\n  //\n  // Y el reloj va enganchado al del monitor. Un monitor "de 120 Hz" midio\n  // 120,2429 Hz: con un reloj propio de 60,000 fps cada 8,3 s sobraba o\n  // faltaba un refresco, y se veia como un tiron periodico. Asi que, en\n  // Windows, el periodo es un multiplo exacto del refresco real que da el\n  // compositor (DwmGetCompositionTimingInfo, en unidades de QPC como nuestro\n  // reloj) y la fase queda a un cuarto de refresco despues de un vblank, lejos\n  // del borde entre dos refrescos.\n  //\n  // PERO solo con vsync (frame_pacing_display_lock = 1). Con G-Sync/FreeSync\n  // el "vblank" que da el compositor es el de nuestro propio present, y\n  // recolocar la fase sobre el cada segundo desplazaba el reloj hasta medio\n  // refresco. MEDIDO conduciendo sin vsync con G-Sync: 59,8 fps y flips de\n  // 12,4 a 20,8 ms (16,7 +- 4,2 = medio refresco a 120 Hz).\n  double diag_late_ms = 0.0, diag_sleep_ms = 0.0, diag_over_ms = 0.0, diag_period_ms = 0.0;\n  bool diag_reset = false;\n  if (const int32_t pacing_fps = REXCVAR_GET(frame_pacing_fps); pacing_fps > 0) {\n    static uint64_t next_flip = 0;\n    static uint32_t flips_since_sync = 0;\n    const uint64_t freq = rex::chrono::Clock::QueryHostTickFrequency();\n    uint64_t period = freq / uint64_t(pacing_fps);\n    uint64_t now = rex::chrono::Clock::QueryHostTickCount();\n#if defined(_WIN32)\n    using DwmTimingFn = HRESULT(WINAPI*)(HWND, DWM_TIMING_INFO*);\n    static DwmTimingFn dwm_timing = [] {\n      HMODULE dwm = LoadLibraryW(L"dwmapi.dll");\n      auto fn = dwm ? reinterpret_cast<DwmTimingFn>(GetProcAddress(dwm, "DwmGetCompositionTimingInfo"))\n                    : nullptr;\n      if (!fn) {\n        REXGPU_WARN("[pacing] sin DwmGetCompositionTimingInfo: ritmo con reloj propio");\n      }\n      return fn;\n    }();\n    static uint64_t refresh = 0, vblank = 0;\n    const int32_t display_lock = REXCVAR_GET(frame_pacing_display_lock);\n    if (display_lock == 0 || (display_lock == 1 && !REXCVAR_GET(vsync))) {\n      refresh = 0;  // reloj propio; al volver el enganche se vuelve a medir\n    } else if (dwm_timing && (refresh == 0 || ++flips_since_sync >= 60)) {\n      flips_since_sync = 0;\n      DWM_TIMING_INFO ti = {};\n      ti.cbSize = sizeof(ti);\n      const HRESULT hr = dwm_timing(nullptr, &ti);\n      // Se apunta cada vez que el refresco cambia mas de un 0,05 %: con VRR o al\n      // pasar a pantalla completa cambia, y es lo primero que mirar si vuelve\n      // un tiron periodico.\n      if (SUCCEEDED(hr) && ti.qpcRefreshPeriod > 0 &&\n          (refresh == 0 || std::abs(double(ti.qpcRefreshPeriod) - double(refresh)) >\n                               double(refresh) * 0.0005)) {\n        REXGPU_INFO("[pacing] refresco del monitor {:.4f} Hz",\n                    double(freq) / double(ti.qpcRefreshPeriod));\n      }\n      if (SUCCEEDED(hr) && ti.qpcRefreshPeriod > 0) {\n        refresh = ti.qpcRefreshPeriod;\n        vblank = ti.qpcVBlank;\n      }\n    }\n    if (refresh > 0) {\n      const uint64_t refreshes = std::max<uint64_t>(1, (period + refresh / 2) / refresh);\n      period = refreshes * refresh;\n      if (next_flip) {\n        // Recolocar la fase: vblank + n refrescos + 1/4 de refresco.\n        const uint64_t phase = vblank + refresh * uint64_t(REXCVAR_GET(frame_pacing_phase)) / 100;\n        const int64_t n = int64_t(std::llround(double(int64_t(next_flip - phase)) / double(refresh)));\n        next_flip = uint64_t(int64_t(phase) + n * int64_t(refresh));\n      }\n    }\n#endif\n    diag_period_ms = double(period) * 1000.0 / double(freq);\n    if (next_flip && now > next_flip) {\n      diag_late_ms = double(now - next_flip) * 1000.0 / double(freq);\n    }\n    // Si vamos mas de un periodo tarde (carga, pausa), se reengancha el reloj.\n    if (!next_flip || now > next_flip + period) {\n      diag_reset = next_flip != 0;\n      next_flip = now;\n    }\n    if (now < next_flip) {\n      const uint64_t target = next_flip;\n      const double left_ms = double(target - now) * 1000.0 / double(freq);\n      diag_sleep_ms = left_ms;\n      if (left_ms > 1.5) {\n        rex::thread::Sleep(std::chrono::milliseconds(int(left_ms - 1.0)));\n      }\n      uint64_t woke = rex::chrono::Clock::QueryHostTickCount();\n      if (woke > target) {\n        diag_over_ms = double(woke - target) * 1000.0 / double(freq);\n      }\n      while (woke < target) {\n        rex::thread::MaybeYield();\n        woke = rex::chrono::Clock::QueryHostTickCount();\n      }\n    }\n    next_flip += period;\n  }\n\n  const uint64_t swap_start = rex::chrono::Clock::QueryHostTickCount();\n  IssueSwap(frontbuffer_ptr, frontbuffer_width, frontbuffer_height);\n  const uint64_t swap_end = rex::chrono::Clock::QueryHostTickCount();\n\n  // PARCHE LOCAL - desglose de los fotogramas lentos\n  //\n  // Cada fotograma que llega tarde al ritmo (o, sin ritmo, que tarda mas de\n  // 25 ms) deja una linea con en que se fue el tiempo desde el flip anterior.\n  // Las esperas a la GPU de occlusion dentro de un WAIT_REG_MEM cuentan en los\n  // dos apartados.\n  if (REXCVAR_GET(log_guest_fps)) {\n    static uint64_t prev_swap_end = 0;\n    static uint64_t window_start = 0;\n    static uint32_t lines = 0, suppressed = 0;\n    const double f = 1000.0 / double(rex::chrono::Clock::QueryHostTickFrequency());\n    if (prev_swap_end) {\n      const double frame_ms = double(swap_start - prev_swap_end) * f;\n      const bool slow = diag_period_ms > 0.0 ? diag_late_ms > 2.0 : frame_ms > 25.0;\n      if (!window_start || swap_end - window_start >\n                               rex::chrono::Clock::QueryHostTickFrequency() * 10) {\n        if (suppressed) {\n          REXGPU_INFO("[frame lento] ... y {} mas sin apuntar", suppressed);\n        }\n        window_start = swap_end;\n        lines = suppressed = 0;\n      }\n      if (slow && !diag_reset) {\n        if (lines < 30) {\n          ++lines;\n          const double idle = double(g_frame_diag.idle) * f;\n          const double wait_reg = double(g_frame_diag.wait_reg) * f;\n          const double gpu_wait = double(g_frame_diag.gpu_wait) * f;\n          const double work = frame_ms - diag_sleep_ms - idle - wait_reg;\n          REXGPU_INFO(\n              "[frame lento] {:.1f} ms, tarde {:.1f} | juego sin comandos {:.1f} | "\n              "WAIT_REG_MEM {:.1f} ({}x) | GPU occlusion {:.1f} ({}x) | swap {:.1f} | "\n              "pacer durmio {:.1f} +{:.1f} | resto (CP trabajando) {:.1f}",\n              frame_ms, diag_late_ms, idle, g_frame_diag.wait_reg_n ? wait_reg : 0.0,\n              g_frame_diag.wait_reg_n, gpu_wait, g_frame_diag.gpu_wait_n,\n              double(swap_end - swap_start) * f, diag_sleep_ms, diag_over_ms, work);\n          // Y el trabajo, por apartados (solo los de mas de 0,3 ms).\n          std::string partes;\n          for (int i = 0; i < kFrameDiagBuckets; ++i) {\n            const double ms = double(g_frame_diag.bucket[i]) * f;\n            if (ms >= 0.3) {\n              partes += fmt::format(" | {} {:.1f} ({}x)", kFrameDiagBucketNames[i], ms,\n                                    g_frame_diag.bucket_n[i]);\n            }\n          }\n          if (g_frame_diag_enabled) {\n            REXGPU_INFO("[frame lento]   trabajo:{}", partes.empty() ? " (nada medible)" : partes);\n          }\n        } else {\n          ++suppressed;\n        }\n      }\n    }\n    prev_swap_end = swap_end;\n  }\n  g_frame_diag = FrameDiag{};\n  g_frame_stats_enabled = REXCVAR_GET(log_guest_fps);\n  g_frame_diag_enabled = g_frame_stats_enabled && REXCVAR_GET(log_frame_breakdown);\n\n  // PARCHE LOCAL - fps del guest en el log\n  //\n  // Cuantas veces presenta el juego por segundo, medido aqui y no en el host:\n  // es lo unico que dice si guest_vblank_rate cambio de verdad el ritmo del\n  // juego. Una linea cada 10 s, solo con log_guest_fps.\n  if (REXCVAR_GET(log_guest_fps)) {\n    static uint64_t window_start = 0;\n    static uint64_t last_swap = 0;\n    static uint32_t swaps = 0;\n    // La media dice poco de un tiron: se apuntan tambien el abstand minimo y\n    // maximo entre fotogramas y cuantos pasaron de 25 y de 50 ms.\n    static double min_ms = 1e9, max_ms = 0.0;\n    static uint32_t over25 = 0, over50 = 0;\n    const uint64_t now = rex::chrono::Clock::QueryHostTickCount();\n    const uint64_t freq = rex::chrono::Clock::QueryHostTickFrequency();\n    if (!window_start) {\n      window_start = now;\n    }\n    if (last_swap) {\n      const double ms = double(now - last_swap) * 1000.0 / double(freq);\n      min_ms = std::min(min_ms, ms);\n      max_ms = std::max(max_ms, ms);\n      over25 += ms > 25.0;\n      over50 += ms > 50.0;\n    }\n    last_swap = now;\n    ++swaps;\n    if (now - window_start >= freq * 10) {\n      REXGPU_INFO("[guest fps] {:.1f} swaps/s | frame {:.1f}-{:.1f} ms | >25ms {} | >50ms {}",\n                  swaps * double(freq) / double(now - window_start), min_ms, max_ms, over25,\n                  over50);\n      window_start = now;\n      swaps = 0;\n      min_ms = 1e9;\n      max_ms = 0.0;\n      over25 = over50 = 0;\n    }\n  }\n\n'),
    ('src/graphics/command_processor.cpp',
     'graphics/command_processor.cpp #6',
     '  bool is_memory = (wait_info & 0x10) != 0;\n\n  bool matched = false;\n  do {\n',
     '  bool is_memory = (wait_info & 0x10) != 0;\n\n  const uint64_t wait_start = rex::chrono::Clock::QueryHostTickCount();\n  bool matched = false;\n  do {\n'),
    ('src/graphics/command_processor.cpp',
     'graphics/command_processor.cpp #7',
     '        PrepareForWait();\n        if (!REXCVAR_GET(vsync)) {\n          // User wants it fast and dangerous.\n          rex::thread::MaybeYield();\n        } else {\n          rex::thread::Sleep(std::chrono::milliseconds(wait / 0x100));\n        }\n',
     '        PrepareForWait();\n        // PARCHE LOCAL - espera activa los 2 primeros ms, y solo despues\n        // dormir 1 ms por vuelta.\n        //\n        // Antes, con vsync, cada vuelta dormia wait/0x100 ms (y el Sleep de\n        // Windows redondea a 15,6 ms si nadie sube la resolucion del reloj).\n        // NFS Most Wanted hace muchas de estas esperas por fotograma y casi\n        // todas se resuelven en microsegundos: la suma de dormidas le costaba\n        // mas de un periodo de vblank por fotograma. MEDIDO: con vsync, 30 fps\n        // y flips cada 16/33/50 ms; sin vsync -que aqui solo cedia el hilo-,\n        // cientos de fps y flips regulares.\n        const double waited_ms =\n            double(rex::chrono::Clock::QueryHostTickCount() - wait_start) * 1000.0 /\n            double(rex::chrono::Clock::QueryHostTickFrequency());\n        if (!REXCVAR_GET(vsync) || waited_ms < 2.0) {\n          rex::thread::MaybeYield();\n        } else {\n          rex::thread::Sleep(std::chrono::milliseconds(1));\n        }\n'),
    ('src/graphics/command_processor.cpp',
     'graphics/command_processor.cpp #8',
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
     '\n// PARCHE LOCAL - cuentas de occlusion queries para el log (con log_guest_fps).\nnamespace {\nstruct OcclusionStats {\n  uint32_t events = 0, no_resources = 0, reports = 0, delivered = 0, waited = 0;\n  uint64_t delta_max = 0;\n  std::chrono::steady_clock::time_point window{};\n} g_occlusion_stats;\n\nvoid LogOcclusionStats(bool resources, size_t pending) {\n  auto& s = g_occlusion_stats;\n  const auto now = std::chrono::steady_clock::now();\n  if (s.window == std::chrono::steady_clock::time_point{}) {\n    s.window = now;\n  }\n  if (now - s.window < std::chrono::seconds(10)) {\n    return;\n  }\n  if (rex::cvar::GetFlagInfo("log_guest_fps") && rex::cvar::Query<bool>("log_guest_fps")) {\n    REXGPU_INFO(\n        "[occlusion] eventos {} | sin recursos {} | informes {} | entregados {} | con espera {} "\n        "| en cola {} | max muestras por intervalo {} | recursos {}",\n        s.events, s.no_resources, s.reports, s.delivered, s.waited, pending, s.delta_max,\n        resources ? "si" : "no");\n  }\n  s = {};\n  s.window = now;\n}\n}  // namespace\n\n// PARCHE LOCAL - ZPD como contador continuo (ver ZpdReport en la cabecera).\n//\n// MEDIDO en NFS Most Wanted, conduciendo: ~370 eventos por segundo, y NINGUNO\n// casaba con el modelo anterior de "consulta con principio y fin en la misma\n// direccion": el volcado de antes y el de despues van a direcciones distintas.\n// Todos acababan en el valor falso de "1000 muestras visibles", y el sol y sus\n// destellos se veian a traves de edificios y puentes.\nbool D3D12CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader,\n                                                               uint32_t packet, uint32_t count) {\n  ++g_occlusion_stats.events;\n  LogOcclusionStats(occlusion_query_resources_available_, zpd_reports_.size());\n  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_query_resources_available_) {\n    ++g_occlusion_stats.no_resources;\n    return CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(reader, packet, count);\n  }\n\n  assert_true(count == 1);\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #3',
     '\n  uint32_t sample_count_addr = register_file_->values[XE_GPU_REG_RB_SAMPLE_COUNT_ADDR];\n  auto* sample_counts =\n      memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(sample_count_addr);\n  if (!sample_counts) {\n    DisableHostOcclusionQueries();\n    return true;\n  }\n\n  auto write_fallback_result = [sample_counts, kQueryFinished]() -> bool {\n    auto fake_sample_count = REXCVAR_GET(query_occlusion_fake_sample_count);\n    if (fake_sample_count < 0) {\n      return true;\n    }\n    bool is_end_via_z_pass =\n        sample_counts->ZPass_A == kQueryFinished || sample_counts->ZPass_B == kQueryFinished;\n    bool is_end_via_z_fail =\n        sample_counts->ZFail_A == kQueryFinished || sample_counts->ZFail_B == kQueryFinished;\n    std::memset(sample_counts, 0, sizeof(xenos::xe_gpu_depth_sample_counts));\n    if (is_end_via_z_pass || is_end_via_z_fail) {\n      sample_counts->ZPass_A = fake_sample_count;\n      sample_counts->Total_A = fake_sample_count;\n    }\n    return true;\n  };\n\n  bool is_end_via_z_pass =\n      sample_counts->ZPass_A == kQueryFinished || sample_counts->ZPass_B == kQueryFinished;\n  bool is_end_via_z_fail =\n      sample_counts->ZFail_A == kQueryFinished || sample_counts->ZFail_B == kQueryFinished;\n  bool is_end = is_end_via_z_pass || is_end_via_z_fail;\n\n  if (!is_end) {\n    if (active_occlusion_query_.valid &&\n        active_occlusion_query_.sample_count_address != sample_count_addr) {\n      DisableHostOcclusionQueries();\n      return write_fallback_result();\n    }\n    if (!BeginGuestOcclusionQuery(sample_count_addr)) {\n      return write_fallback_result();\n    }\n    return true;\n  }\n\n  if (!active_occlusion_query_.valid ||\n      active_occlusion_query_.sample_count_address != sample_count_addr) {\n    DisableHostOcclusionQueries();\n    return write_fallback_result();\n  }\n\n  if (!EndGuestOcclusionQuery(sample_count_addr, sample_counts)) {\n    return write_fallback_result();\n  }\n\n  return true;\n',
     '\n  // Cerrar el intervalo medido desde el evento anterior, encolar su informe y\n  // abrir el siguiente.\n  ZpdCloseSegment();\n  ZpdReport report;\n  report.address = register_file_->values[XE_GPU_REG_RB_SAMPLE_COUNT_ADDR];\n  report.segments = std::move(zpd_interval_segments_);\n  zpd_interval_segments_.clear();\n  zpd_reports_.push_back(std::move(report));\n  ++g_occlusion_stats.reports;\n  ZpdOpenSegment();\n\n  ProcessPendingOcclusionQueries(false);\n  return true;\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #4',
     '    // Special copy handling.\n    return IssueCopy();\n',
     '    // Special copy handling.\n    DiagScope diag_scope(kDiagResolve);\n    return IssueCopy();\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #5',
     '  }\n  pipeline_cache_->AnalyzeShaderUcode(*vertex_shader);\n  bool memexport_used_vertex = vertex_shader->memexport_eM_written() != 0;\n',
     '  }\n  {\n    DiagScope diag_scope(kDiagShader);\n    pipeline_cache_->AnalyzeShaderUcode(*vertex_shader);\n  }\n  bool memexport_used_vertex = vertex_shader->memexport_eM_written() != 0;\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #6',
     '      if (pixel_shader) {\n        pipeline_cache_->AnalyzeShaderUcode(*pixel_shader);\n        if (!draw_util::IsPixelShaderNeededWithRasterization(*pixel_shader, regs)) {\n',
     '      if (pixel_shader) {\n        {\n          DiagScope diag_scope(kDiagShader);\n          pipeline_cache_->AnalyzeShaderUcode(*pixel_shader);\n        }\n        if (!draw_util::IsPixelShaderNeededWithRasterization(*pixel_shader, regs)) {\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #7',
     '  PrimitiveProcessor::ProcessingResult primitive_processing_result;\n  if (!primitive_processor_->Process(primitive_processing_result)) {\n    return false;\n  }\n',
     '  PrimitiveProcessor::ProcessingResult primitive_processing_result;\n  {\n    DiagScope diag_scope(kDiagPrimitive);\n    if (!primitive_processor_->Process(primitive_processing_result)) {\n      return false;\n    }\n  }\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #8',
     '                   : 0;\n  if (!render_target_cache_->Update(is_rasterization_done, normalized_depth_control,\n                                    normalized_color_mask, *vertex_shader)) {\n    return false;\n  }\n',
     '                   : 0;\n  {\n    DiagScope diag_scope(kDiagRenderTargets);\n    if (!render_target_cache_->Update(is_rasterization_done, normalized_depth_control,\n                                      normalized_color_mask, *vertex_shader)) {\n      return false;\n    }\n  }\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #9',
     '  ID3D12RootSignature* root_signature;\n  if (!pipeline_cache_->ConfigurePipeline(\n          vertex_shader_translation, pixel_shader_translation, primitive_processing_result,\n          normalized_depth_control, normalized_color_mask, bound_depth_and_color_render_target_bits,\n          bound_depth_and_color_render_target_formats, &pipeline_handle, &root_signature)) {\n    return false;\n  }\n',
     '  ID3D12RootSignature* root_signature;\n  {\n    DiagScope diag_scope(kDiagPipeline);\n    if (!pipeline_cache_->ConfigurePipeline(\n            vertex_shader_translation, pixel_shader_translation, primitive_processing_result,\n            normalized_depth_control, normalized_color_mask, bound_depth_and_color_render_target_bits,\n            bound_depth_and_color_render_target_formats, &pipeline_handle, &root_signature)) {\n      return false;\n    }\n  }\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #10',
     '      (pixel_shader != nullptr ? pixel_shader->GetUsedTextureMaskAfterTranslation() : 0);\n  texture_cache_->RequestTextures(used_texture_mask);\n\n',
     '      (pixel_shader != nullptr ? pixel_shader->GetUsedTextureMaskAfterTranslation() : 0);\n  {\n    DiagScope diag_scope(kDiagTextures);\n    texture_cache_->RequestTextures(used_texture_mask);\n  }\n\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #11',
     '  // Update constant buffers, descriptors and root parameters.\n  if (!UpdateBindings(vertex_shader, pixel_shader, root_signature, memexport_used)) {\n    return false;\n  }\n',
     '  // Update constant buffers, descriptors and root parameters.\n  {\n    DiagScope diag_scope(kDiagBindings);\n    if (!UpdateBindings(vertex_shader, pixel_shader, root_signature, memexport_used)) {\n      return false;\n    }\n  }\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #12',
     '      }\n      if (!shared_memory_->RequestRange(vfetch_constant.address << 2, vfetch_constant.size << 2)) {\n',
     '      }\n      DiagScope diag_scope(kDiagVertexBuffers);\n      if (!shared_memory_->RequestRange(vfetch_constant.address << 2, vfetch_constant.size << 2)) {\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #13',
     '      PROFILE_CMD_BUFFER_STALL();\n      WaitForSingleObject(fence_completion_event_, INFINITE);\n',
     '      PROFILE_CMD_BUFFER_STALL();\n      DiagScope diag_scope(kDiagGpuWait);\n      WaitForSingleObject(fence_completion_event_, INFINITE);\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #14',
     '    texture_cache_->BeginSubmission(submission_current_);\n  }\n',
     '    texture_cache_->BeginSubmission(submission_current_);\n\n    // PARCHE LOCAL - ZPD como contador continuo: seguir midiendo el intervalo\n    // que la submission anterior dejo partido.\n    if (zpd_segment_reopen_ && !active_occlusion_query_.valid && occlusion_query_heap_ &&\n        occlusion_query_resources_available_) {\n      zpd_segment_reopen_ = false;\n      uint32_t host_index = 0;\n      if (AcquireOcclusionQueryIndex(host_index)) {\n        deferred_command_list_.D3DBeginQuery(occlusion_query_heap_.Get(),\n                                             D3D12_QUERY_TYPE_OCCLUSION, host_index);\n        active_occlusion_query_.host_index = host_index;\n        active_occlusion_query_.valid = true;\n      }\n    }\n  }\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #15',
     'bool D3D12CommandProcessor::EndSubmission(bool is_swap) {\n  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();\n',
     'bool D3D12CommandProcessor::EndSubmission(bool is_swap) {\n  DiagScope diag_scope(kDiagSubmit);\n  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #16',
     '\n    if (active_occlusion_query_.valid && occlusion_query_heap_) {\n      deferred_command_list_.D3DEndQuery(occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,\n                                         active_occlusion_query_.host_index);\n      active_occlusion_query_ = {};\n    }\n',
     '\n    // PARCHE LOCAL - ZPD como contador continuo: el intervalo en curso se\n    // parte aqui y sigue midiendo en la proxima submission.\n    if (active_occlusion_query_.valid) {\n      ZpdCloseSegment();\n      zpd_segment_reopen_ = true;\n    }\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #17',
     '  DisableHostOcclusionQueries();\n\n',
     '  DisableHostOcclusionQueries();\n  zpd_reports_.clear();\n  zpd_interval_segments_.clear();\n\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #18',
     '  }\n  occlusion_query_cursor_ = 0;\n',
     '  }\n  zpd_segment_reopen_ = false;\n  zpd_interval_segments_.clear();\n  occlusion_query_cursor_ = 0;\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #19',
     '\nbool D3D12CommandProcessor::BeginGuestOcclusionQuery(uint32_t sample_count_address) {\n  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_query_resources_available_) {\n    return false;\n  }\n  if (active_occlusion_query_.valid) {\n    REXGPU_WARN(\n        "D3D12CommandProcessor: Occlusion query begin issued while another query is active");\n    DisableHostOcclusionQueries();\n    return false;\n  }\n\n  uint32_t host_index = 0;\n  if (!AcquireOcclusionQueryIndex(host_index)) {\n    return false;\n  }\n  if (!BeginSubmission(true)) {\n    return false;\n  }\n\n  deferred_command_list_.D3DBeginQuery(occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,\n                                       host_index);\n  active_occlusion_query_.sample_count_address = sample_count_address;\n  active_occlusion_query_.host_index = host_index;\n  active_occlusion_query_.valid = true;\n  return true;\n}\n\nbool D3D12CommandProcessor::EndGuestOcclusionQuery(\n    uint32_t sample_count_address, xenos::xe_gpu_depth_sample_counts* sample_counts) {\n  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_query_resources_available_ ||\n      !active_occlusion_query_.valid || !occlusion_query_heap_ || !occlusion_query_readback_) {\n    return false;\n  }\n\n  uint32_t host_index = active_occlusion_query_.host_index;\n  active_occlusion_query_ = {};\n\n  if (!BeginSubmission(true)) {\n    return false;\n  }\n\n  deferred_command_list_.D3DEndQuery(occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,\n',
     '\n// PARCHE LOCAL - ZPD como contador continuo: abre un segmento de medida en la\n// submission actual (una consulta de D3D12 no puede cruzar listas de comandos).\nvoid D3D12CommandProcessor::ZpdOpenSegment() {\n  zpd_segment_reopen_ = false;\n  if (active_occlusion_query_.valid || !occlusion_query_resources_available_ ||\n      !occlusion_query_heap_) {\n    return;\n  }\n  uint32_t host_index = 0;\n  if (!AcquireOcclusionQueryIndex(host_index) || !BeginSubmission(true)) {\n    return;\n  }\n  deferred_command_list_.D3DBeginQuery(occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,\n                                       host_index);\n  active_occlusion_query_.host_index = host_index;\n  active_occlusion_query_.valid = true;\n}\n\n// Cierra el segmento abierto y lo apunta en el intervalo en curso. La\n// resolucion va en la misma submission, asi que el resultado esta en el\n// readback en cuanto esa submission termina en la GPU.\nvoid D3D12CommandProcessor::ZpdCloseSegment() {\n  if (!active_occlusion_query_.valid || !occlusion_query_heap_ || !occlusion_query_readback_) {\n    active_occlusion_query_ = {};\n    return;\n  }\n  const uint32_t host_index = active_occlusion_query_.host_index;\n  active_occlusion_query_ = {};\n  deferred_command_list_.D3DEndQuery(occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #20',
     '      occlusion_query_readback_.Get(), sizeof(uint64_t) * host_index);\n\n  if (!EndSubmission(false)) {\n    return false;\n  }\n\n  uint64_t query_submission = submission_current_ ? submission_current_ - 1 : 0;\n  CheckSubmissionFence(query_submission);\n  if (submission_completed_ < query_submission) {\n    return false;\n  }\n  if (!occlusion_query_readback_mapping_) {\n    return false;\n  }\n\n  uint64_t samples = occlusion_query_readback_mapping_[host_index];\n  samples = NormalizeOcclusionSamples(samples);\n  WriteGuestOcclusionResult(sample_counts, samples);\n  return true;\n}\n',
     '      occlusion_query_readback_.Get(), sizeof(uint64_t) * host_index);\n  ZpdSegment segment;\n  segment.host_index = host_index;\n  segment.submission = submission_current_;\n  zpd_interval_segments_.push_back(segment);\n}\n\n// Entrega los informes en orden: cada uno lleva el valor del contador continuo\n// tras sumar las muestras de su intervalo. El D3D del juego resta dos\n// informes; aqui solo hace falta que el contador avance como el de la Xenos.\nvoid D3D12CommandProcessor::ProcessPendingOcclusionQueries(bool wait) {\n  if (zpd_reports_.empty()) {\n    return;\n  }\n  CheckSubmissionFence(0);\n  while (!zpd_reports_.empty()) {\n    ZpdReport& report = zpd_reports_.front();\n    uint64_t needed = 0;\n    for (const ZpdSegment& segment : report.segments) {\n      needed = std::max(needed, segment.submission);\n    }\n    if (!report.segments.empty() && submission_completed_ < needed) {\n      if (!wait) {\n        break;\n      }\n      const uint64_t wait_start = rex::chrono::Clock::QueryHostTickCount();\n      CheckSubmissionFence(needed);\n      FrameDiagAddGpuWait(rex::chrono::Clock::QueryHostTickCount() - wait_start);\n      if (submission_completed_ < needed) {\n        break;\n      }\n      ++g_occlusion_stats.waited;\n    }\n    uint64_t delta = 0;\n    if (occlusion_query_readback_mapping_) {\n      for (const ZpdSegment& segment : report.segments) {\n        delta += occlusion_query_readback_mapping_[segment.host_index];\n      }\n    }\n    delta = NormalizeOcclusionSamples(delta);\n    zpd_counter_ += uint32_t(delta);\n    g_occlusion_stats.delta_max = std::max(g_occlusion_stats.delta_max, delta);\n    ++g_occlusion_stats.delivered;\n    auto* sample_counts =\n        memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(report.address);\n    WriteGuestOcclusionResult(sample_counts, zpd_counter_);\n    zpd_reports_.pop_front();\n  }\n}\n\nvoid D3D12CommandProcessor::PrepareForWait() {\n  CommandProcessor::PrepareForWait();\n  // El guest se queda esperando -sin comandos nuevos o en un WAIT_REG_MEM-:\n  // puede que este esperando justo uno de estos resultados, asi que se espera\n  // a la GPU y se entregan todos.\n  ProcessPendingOcclusionQueries(true);\n}\n'),
    ('src/kernel/xboxkrnl/xboxkrnl_video.cpp',
     'xboxkrnl/xboxkrnl_video.cpp #1',
     '                  mapped_u32 height) {\n  // All of these parameters are REQUIRED.\n',
     '                  mapped_u32 height) {\n  // PARCHE LOCAL - ritmo de VdSwap visto desde el guest (diagnostico, con\n  // log_guest_fps). Compararlo con el del procesador de comandos dice si los\n  // tirones los mete el juego o los mete la emulacion de la GPU.\n  static bool log_fps = rex::cvar::GetFlagInfo("log_guest_fps") != nullptr;\n  if (log_fps && rex::cvar::Query<bool>("log_guest_fps")) {\n    using clk = std::chrono::steady_clock;\n    static clk::time_point window_start{}, last{};\n    static uint32_t swaps = 0, over25 = 0, over50 = 0;\n    static double min_ms = 1e9, max_ms = 0.0;\n    const auto now = clk::now();\n    if (window_start == clk::time_point{}) window_start = now;\n    if (last != clk::time_point{}) {\n      const double ms = std::chrono::duration<double, std::milli>(now - last).count();\n      min_ms = std::min(min_ms, ms);\n      max_ms = std::max(max_ms, ms);\n      over25 += ms > 25.0;\n      over50 += ms > 50.0;\n    }\n    last = now;\n    ++swaps;\n    const double win = std::chrono::duration<double>(now - window_start).count();\n    if (win >= 10.0) {\n      REXKRNL_INFO("[VdSwap fps] {:.1f} swaps/s | frame {:.1f}-{:.1f} ms | >25ms {} | >50ms {}",\n                   swaps / win, min_ms, max_ms, over25, over50);\n      window_start = now;\n      swaps = over25 = over50 = 0;\n      min_ms = 1e9;\n      max_ms = 0.0;\n    }\n  }\n\n  // All of these parameters are REQUIRED.\n'),
    ('include/rex/ui/presenter.h',
     'ui/presenter.h #1',
     '  bool guest_output_active_last_refresh_ = false;\n\n',
     '  bool guest_output_active_last_refresh_ = false;\n  // PARCHE LOCAL - un present por fotograma del guest. Momento (steady_clock,\n  // en ms) del ultimo RefreshGuestOutput; mientras el guest siga entregando\n  // imagenes, la UI se repinta con ellas y no por su cuenta en cada vblank.\n  std::atomic<int64_t> guest_output_last_refresh_ms_{0};\n  // La UI queria repintarse pero se dejo para el siguiente fotograma del guest.\n  std::atomic<bool> ui_paint_deferred_{false};\n  bool IsGuestOutputFlowing() const;\n\n'),
    ('src/ui/presenter.cpp',
     'ui/presenter.cpp #1',
     '#include <cctype>\n#include <utility>\n',
     '#include <cctype>\n#include <chrono>\n#include <utility>\n'),
    ('src/ui/presenter.cpp',
     'ui/presenter.cpp #2',
     '                    "Allow presentation from non-UI thread");\n\n',
     '                    "Allow presentation from non-UI thread");\n\n// PARCHE LOCAL - un present por fotograma del guest\n// Con cualquier dialogo de ImGui registrado (y la notificacion de logros lo\n// esta siempre) el hilo de UI repintaba en cada vblank del monitor ademas de en\n// cada fotograma del guest: 120-300 presents por segundo para 60 imagenes. Eso\n// saca a G-Sync/FreeSync de su rango (tearing) y, con vsync, reparte las\n// imagenes del guest entre refrescos de forma irregular (tirones).\nREXCVAR_DEFINE_BOOL(present_ui_with_guest_frames, true, "UI/Presenter",\n                    "Mientras el juego entregue imagenes, repintar la UI solo "\n                    "con ellas (un present por fotograma del juego)");\n\n'),
    ('src/ui/presenter.cpp',
     'ui/presenter.cpp #3',
     '    if (request_ui_paint_after_current_ui_thread_paint_ && !ui_drawers_.empty()) {\n      request_repaint_at_tick = true;\n    }\n',
     '    if (request_ui_paint_after_current_ui_thread_paint_ && !ui_drawers_.empty()) {\n      if (IsGuestOutputFlowing()) {\n        // El siguiente fotograma del guest repinta la UI; si no llega, el hilo\n        // de ticks lo pide en cuanto el guest deje de entregar.\n        ui_paint_deferred_.store(true, std::memory_order_relaxed);\n      } else {\n        request_repaint_at_tick = true;\n      }\n    }\n'),
    ('src/ui/presenter.cpp',
     'ui/presenter.cpp #4',
     '    guest_output_mailbox_writable_ = (3 - last_acquired - guest_output_mailbox_writable_) % 3;\n  }\n\n  // Trigger the presentation on the host.\n',
     '    guest_output_mailbox_writable_ = (3 - last_acquired - guest_output_mailbox_writable_) % 3;\n  }\n\n  guest_output_last_refresh_ms_.store(\n      std::chrono::duration_cast<std::chrono::milliseconds>(\n          std::chrono::steady_clock::now().time_since_epoch())\n          .count(),\n      std::memory_order_relaxed);\n\n  // Trigger the presentation on the host.\n'),
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
     '    COUNT_profile_set("gpu/texture_cache/textures", textures_.size());\n  }\n\n  // PARCHE LOCAL - estado del cache cada 10 s (con log_guest_fps): si\n  // "descartadas" sube mientras se conduce, las texturas se tiran por los\n  // limites de memoria y luego hay que volver a crearlas (tirones).\n  if (rex::graphics::FrameStatsEnabled()) {\n    static uint64_t last_log = 0;\n    if (!last_log) {\n      last_log = current_time;\n    } else if (current_time - last_log >= 10000) {\n      last_log = current_time;\n      const double create_ms = double(g_textures_create_ticks) * 1000.0 /\n                               double(rex::chrono::Clock::QueryHostTickFrequency());\n      REXGPU_INFO(\n          "[texturas] {} en cache, {} MB (limites {}/{} MB) | creadas {} en {:.1f} ms ({:.3f} ms c/u) | "\n          "descartadas {}",\n          textures_.size(), textures_total_host_memory_usage_ >> 20, limit_soft_mb, limit_hard_mb,\n          g_textures_created, create_ms, g_textures_created ? create_ms / g_textures_created : 0.0,\n          g_textures_destroyed);\n      g_textures_create_ticks = 0;\n      g_textures_created = 0;\n      g_textures_destroyed = 0;\n    }\n  }\n'),
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
     '    d3d12_texture_cache.ReleaseTextureDescriptor(descriptor_pair.second);\n  }\n  // PARCHE LOCAL - devolver el hueco del heap compartido. Las texturas solo se\n  // destruyen cuando la GPU ya no las usa (CompletedSubmissionUpdated) o al\n  // vaciar el cache, asi que el hueco se puede reutilizar.\n  if (heap_index_ != UINT32_MAX) {\n    resource_.Reset();\n    d3d12_texture_cache.FreeTextureHeapBlock(heap_index_, heap_offset_, heap_size_);\n  }\n}\n\nbool D3D12TextureCache::AddTextureHeap() {\n  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();\n  D3D12_HEAP_DESC heap_desc = {};\n  heap_desc.SizeInBytes = kTextureHeapSize;\n  heap_desc.Properties = ui::d3d12::util::kHeapPropertiesDefault;\n  heap_desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;\n  heap_desc.Flags =\n      D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES | provider.GetHeapFlagCreateNotZeroed();\n  TextureHeap texture_heap;\n  if (FAILED(provider.GetDevice()->CreateHeap(&heap_desc, IID_PPV_ARGS(&texture_heap.heap)))) {\n    REXGPU_WARN("[texturas] no se pudo crear un heap de {} MB; se sigue con recursos propios",\n                kTextureHeapSize >> 20);\n    texture_heaps_failed_ = true;\n    return false;\n  }\n  texture_heap.free_blocks.emplace(0, kTextureHeapSize);\n  texture_heaps_.push_back(std::move(texture_heap));\n  return true;\n}\n\nbool D3D12TextureCache::AllocateTextureHeapBlock(uint64_t size, uint64_t alignment,\n                                                 uint32_t& heap_index_out, uint64_t& offset_out) {\n  if (texture_heaps_failed_ || size == 0 || size > kTextureHeapSize / 2) {\n    return false;\n  }\n  if (texture_heaps_.empty()) {\n    // Unos cuantos de golpe, que la primera textura llega en la pantalla de\n    // carga y asi no hay que crear heaps mientras se conduce.\n    for (int i = 0; i < 4; ++i) {\n      if (!AddTextureHeap()) {\n        break;\n      }\n    }\n  }\n  for (int attempt = 0; attempt < 2; ++attempt) {\n    for (uint32_t i = 0; i < uint32_t(texture_heaps_.size()); ++i) {\n      auto& free_blocks = texture_heaps_[i].free_blocks;\n      for (auto it = free_blocks.begin(); it != free_blocks.end(); ++it) {\n        const uint64_t block_start = it->first;\n        const uint64_t block_end = it->first + it->second;\n        const uint64_t aligned = (block_start + alignment - 1) / alignment * alignment;\n        if (aligned + size > block_end) {\n          continue;\n        }\n        free_blocks.erase(it);\n        if (aligned > block_start) {\n          free_blocks.emplace(block_start, aligned - block_start);\n        }\n        if (aligned + size < block_end) {\n          free_blocks.emplace(aligned + size, block_end - (aligned + size));\n        }\n        heap_index_out = i;\n        offset_out = aligned;\n        return true;\n      }\n    }\n    if (attempt == 0 && !AddTextureHeap()) {\n      return false;\n    }\n  }\n  return false;\n}\n\nvoid D3D12TextureCache::FreeTextureHeapBlock(uint32_t heap_index, uint64_t offset,\n                                             uint64_t size) {\n  if (heap_index >= texture_heaps_.size()) {\n    return;\n  }\n  auto& free_blocks = texture_heaps_[heap_index].free_blocks;\n  auto it = free_blocks.emplace(offset, size).first;\n  // Juntar con el hueco siguiente y con el anterior.\n  auto next = std::next(it);\n  if (next != free_blocks.end() && it->first + it->second == next->first) {\n    it->second += next->second;\n    free_blocks.erase(next);\n  }\n  if (it != free_blocks.begin()) {\n    auto prev = std::prev(it);\n    if (prev->first + prev->second == it->first) {\n      prev->second += it->second;\n      free_blocks.erase(it);\n    }\n  }\n}\n\n'),
    ('src/graphics/d3d12/texture_cache.cpp',
     'd3d12/texture_cache.cpp #3',
     '  Microsoft::WRL::ComPtr<ID3D12Resource> resource;\n  if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesDefault,\n',
     '  Microsoft::WRL::ComPtr<ID3D12Resource> resource;\n  // PARCHE LOCAL - primero en un heap compartido; si no cabe o falla, como\n  // antes. Alineacion pequena (4 KB) cuando el driver la acepta.\n  if (REXCVAR_GET(d3d12_texture_heaps)) {\n    D3D12_RESOURCE_DESC placed_desc = desc;\n    placed_desc.Alignment = D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT;\n    D3D12_RESOURCE_ALLOCATION_INFO info = device->GetResourceAllocationInfo(0, 1, &placed_desc);\n    if (info.Alignment != D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT) {\n      placed_desc.Alignment = 0;\n      info = device->GetResourceAllocationInfo(0, 1, &placed_desc);\n    }\n    uint32_t heap_index;\n    uint64_t heap_offset;\n    if (info.SizeInBytes != UINT64_MAX &&\n        AllocateTextureHeapBlock(info.SizeInBytes, info.Alignment, heap_index, heap_offset)) {\n      if (SUCCEEDED(device->CreatePlacedResource(texture_heaps_[heap_index].heap.Get(),\n                                                 heap_offset, &placed_desc, resource_state,\n                                                 nullptr, IID_PPV_ARGS(&resource)))) {\n        auto texture = std::unique_ptr<D3D12Texture>(\n            new D3D12Texture(*this, key, resource.Get(), resource_state));\n        texture->SetHeapPlacement(heap_index, heap_offset, info.SizeInBytes);\n        return texture;\n      }\n      FreeTextureHeapBlock(heap_index, heap_offset, info.SizeInBytes);\n    }\n  }\n  if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesDefault,\n'),
    ('src/ui/d3d12/d3d12_presenter.cpp',
     'd3d12/d3d12_presenter.cpp #1',
     '  HRESULT present_result = paint_context_.swap_chain->Present(sync_interval, present_flags);\n',
     '  HRESULT present_result = paint_context_.swap_chain->Present(sync_interval, present_flags);\n  // PARCHE LOCAL - presents por segundo en el log (con log_guest_fps), para\n  // comprobar que hay uno por fotograma del guest.\n  {\n    static bool consultado = false;\n    static bool registrar = false;\n    if (!consultado) {\n      consultado = true;\n      registrar = rex::cvar::GetFlagInfo("log_guest_fps") != nullptr &&\n                  rex::cvar::Query<bool>("log_guest_fps");\n    }\n    if (registrar) {\n      using Reloj = std::chrono::steady_clock;\n      static Reloj::time_point inicio = Reloj::now();\n      static Reloj::time_point previo = inicio;\n      static uint32_t cuenta = 0;\n      static double min_ms = 1e9, max_ms = 0.0;\n      const auto ahora = Reloj::now();\n      if (cuenta != 0) {\n        const double ms = std::chrono::duration<double, std::milli>(ahora - previo).count();\n        min_ms = std::min(min_ms, ms);\n        max_ms = std::max(max_ms, ms);\n      }\n      previo = ahora;\n      ++cuenta;\n      const double transcurrido = std::chrono::duration<double>(ahora - inicio).count();\n      if (transcurrido >= 10.0) {\n        REXLOG_INFO("[present] {:.1f}/s | intervalo {:.1f}-{:.1f} ms | vsync {}",\n                    cuenta / transcurrido, min_ms, max_ms, con_vsync);\n        inicio = ahora;\n        cuenta = 0;\n        min_ms = 1e9;\n        max_ms = 0.0;\n      }\n    }\n  }\n'),
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
