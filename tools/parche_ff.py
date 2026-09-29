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
     '\n#if defined(_WIN32)\n#ifndef NOMINMAX\n#define NOMINMAX\n#endif\n#ifndef WIN32_LEAN_AND_MEAN\n#define WIN32_LEAN_AND_MEAN\n#endif\n#include <windows.h>\n#include <dwmapi.h>  // solo el tipo DWM_TIMING_INFO; la funcion se busca en tiempo de ejecucion\n#endif\n\nREXCVAR_DEFINE_BOOL(vsync, true, "GPU", "Enable vertical sync");\n\nREXCVAR_DEFINE_BOOL(log_guest_fps, false, "GPU",\n                    "Log how many frames per second the game presents, every 10 seconds");\n\n// PARCHE LOCAL - ritmo de los flips\nREXCVAR_DEFINE_INT32(frame_pacing_fps, 0, "GPU",\n                     "Present guest frames at an exact, even rate (e.g. 30 or 60). Combine with "\n                     "a fast guest_vblank_rate so the game never misses a vblank slot. 0 = off.")\n    .range(0, 1000)\n    .lifecycle(rex::cvar::Lifecycle::kHotReload);\nREXCVAR_DEFINE_INT32(frame_pacing_phase, 25, "GPU",\n                     "Where in the display refresh the paced flips land, in percent after the "\n                     "vblank.")\n    .range(0, 99)\n    .lifecycle(rex::cvar::Lifecycle::kHotReload);\n\n'),
    ('src/graphics/command_processor.cpp',
     'graphics/command_processor.cpp #2',
     '\n  IssueSwap(frontbuffer_ptr, frontbuffer_width, frontbuffer_height);\n\n',
     '\n  // PARCHE LOCAL - ritmo de los flips (frame_pacing_fps)\n  //\n  // Con vblank de 60 Hz el juego coloca cada flip en una "ranura" de vblank, y\n  // si llega tarde por un pelo pierde la ranura entera: los flips salian cada\n  // 16,6, 33 o 50 ms en vez de cada 33 -el tiron que se veia a "30 fps"-. La\n  // forma buena es darle vblanks rapidos (guest_vblank_rate alto, no pierde\n  // ninguna) y marcar el ritmo aqui con un reloj preciso: un flip cada\n  // 1/frame_pacing_fps segundos, contando desde el flip PREVISTO y no desde el\n  // real, para que un retraso suelto no desplace a todos los demas.\n  //\n  // MEDIDO en el menu (vblank 1000 Hz, sin vsync): 30 fps con flips cada\n  // 33,1-33,5 ms, 60 fps con 16,4-16,9 ms. Con vsync del host encima vuelve a\n  // haber dos relojes y temblor de un refresco del monitor.\n  //\n  // Y el reloj va enganchado al del monitor. Un monitor "de 120 Hz" midio\n  // 120,2429 Hz: con un reloj propio de 60,000 fps cada 8,3 s sobraba o\n  // faltaba un refresco, y se veia como un tiron periodico. Asi que, en\n  // Windows, el periodo es un multiplo exacto del refresco real que da el\n  // compositor (DwmGetCompositionTimingInfo, en unidades de QPC como nuestro\n  // reloj) y la fase queda a un cuarto de refresco despues de un vblank, lejos\n  // del borde entre dos refrescos.\n  if (const int32_t pacing_fps = REXCVAR_GET(frame_pacing_fps); pacing_fps > 0) {\n    static uint64_t next_flip = 0;\n    static uint32_t flips_since_sync = 0;\n    const uint64_t freq = rex::chrono::Clock::QueryHostTickFrequency();\n    uint64_t period = freq / uint64_t(pacing_fps);\n    uint64_t now = rex::chrono::Clock::QueryHostTickCount();\n#if defined(_WIN32)\n    using DwmTimingFn = HRESULT(WINAPI*)(HWND, DWM_TIMING_INFO*);\n    static DwmTimingFn dwm_timing = [] {\n      HMODULE dwm = LoadLibraryW(L"dwmapi.dll");\n      auto fn = dwm ? reinterpret_cast<DwmTimingFn>(GetProcAddress(dwm, "DwmGetCompositionTimingInfo"))\n                    : nullptr;\n      if (!fn) {\n        REXGPU_WARN("[pacing] sin DwmGetCompositionTimingInfo: ritmo con reloj propio");\n      }\n      return fn;\n    }();\n    static uint64_t refresh = 0, vblank = 0;\n    if (dwm_timing && (refresh == 0 || ++flips_since_sync >= 60)) {\n      flips_since_sync = 0;\n      DWM_TIMING_INFO ti = {};\n      ti.cbSize = sizeof(ti);\n      const HRESULT hr = dwm_timing(nullptr, &ti);\n      // Se apunta cada vez que el refresco cambia mas de un 0,05 %: con VRR o al\n      // pasar a pantalla completa cambia, y es lo primero que mirar si vuelve\n      // un tiron periodico.\n      if (SUCCEEDED(hr) && ti.qpcRefreshPeriod > 0 &&\n          (refresh == 0 || std::abs(double(ti.qpcRefreshPeriod) - double(refresh)) >\n                               double(refresh) * 0.0005)) {\n        REXGPU_INFO("[pacing] refresco del monitor {:.4f} Hz",\n                    double(freq) / double(ti.qpcRefreshPeriod));\n      }\n      if (SUCCEEDED(hr) && ti.qpcRefreshPeriod > 0) {\n        refresh = ti.qpcRefreshPeriod;\n        vblank = ti.qpcVBlank;\n      }\n    }\n    if (refresh > 0) {\n      const uint64_t refreshes = std::max<uint64_t>(1, (period + refresh / 2) / refresh);\n      period = refreshes * refresh;\n      if (next_flip) {\n        // Recolocar la fase: vblank + n refrescos + 1/4 de refresco.\n        const uint64_t phase = vblank + refresh * uint64_t(REXCVAR_GET(frame_pacing_phase)) / 100;\n        const int64_t n = int64_t(std::llround(double(int64_t(next_flip - phase)) / double(refresh)));\n        next_flip = uint64_t(int64_t(phase) + n * int64_t(refresh));\n      }\n    }\n#endif\n    // Si vamos mas de un periodo tarde (carga, pausa), se reengancha el reloj.\n    if (!next_flip || now > next_flip + period) {\n      next_flip = now;\n    }\n    if (now < next_flip) {\n      const uint64_t target = next_flip;\n      const double left_ms = double(target - now) * 1000.0 / double(freq);\n      if (left_ms > 1.5) {\n        rex::thread::Sleep(std::chrono::milliseconds(int(left_ms - 1.0)));\n      }\n      while (rex::chrono::Clock::QueryHostTickCount() < target) {\n        rex::thread::MaybeYield();\n      }\n    }\n    next_flip += period;\n  }\n\n  IssueSwap(frontbuffer_ptr, frontbuffer_width, frontbuffer_height);\n\n  // PARCHE LOCAL - fps del guest en el log\n  //\n  // Cuantas veces presenta el juego por segundo, medido aqui y no en el host:\n  // es lo unico que dice si guest_vblank_rate cambio de verdad el ritmo del\n  // juego. Una linea cada 10 s, solo con log_guest_fps.\n  if (REXCVAR_GET(log_guest_fps)) {\n    static uint64_t window_start = 0;\n    static uint64_t last_swap = 0;\n    static uint32_t swaps = 0;\n    // La media dice poco de un tiron: se apuntan tambien el abstand minimo y\n    // maximo entre fotogramas y cuantos pasaron de 25 y de 50 ms.\n    static double min_ms = 1e9, max_ms = 0.0;\n    static uint32_t over25 = 0, over50 = 0;\n    const uint64_t now = rex::chrono::Clock::QueryHostTickCount();\n    const uint64_t freq = rex::chrono::Clock::QueryHostTickFrequency();\n    if (!window_start) {\n      window_start = now;\n    }\n    if (last_swap) {\n      const double ms = double(now - last_swap) * 1000.0 / double(freq);\n      min_ms = std::min(min_ms, ms);\n      max_ms = std::max(max_ms, ms);\n      over25 += ms > 25.0;\n      over50 += ms > 50.0;\n    }\n    last_swap = now;\n    ++swaps;\n    if (now - window_start >= freq * 10) {\n      REXGPU_INFO("[guest fps] {:.1f} swaps/s | frame {:.1f}-{:.1f} ms | >25ms {} | >50ms {}",\n                  swaps * double(freq) / double(now - window_start), min_ms, max_ms, over25,\n                  over50);\n      window_start = now;\n      swaps = 0;\n      min_ms = 1e9;\n      max_ms = 0.0;\n      over25 = over50 = 0;\n    }\n  }\n\n'),
    ('src/graphics/command_processor.cpp',
     'graphics/command_processor.cpp #3',
     '  bool is_memory = (wait_info & 0x10) != 0;\n\n  bool matched = false;\n  do {\n',
     '  bool is_memory = (wait_info & 0x10) != 0;\n\n  const uint64_t wait_start = rex::chrono::Clock::QueryHostTickCount();\n  bool matched = false;\n  do {\n'),
    ('src/graphics/command_processor.cpp',
     'graphics/command_processor.cpp #4',
     '        PrepareForWait();\n        if (!REXCVAR_GET(vsync)) {\n          // User wants it fast and dangerous.\n          rex::thread::MaybeYield();\n        } else {\n          rex::thread::Sleep(std::chrono::milliseconds(wait / 0x100));\n        }\n',
     '        PrepareForWait();\n        // PARCHE LOCAL - espera activa los 2 primeros ms, y solo despues\n        // dormir 1 ms por vuelta.\n        //\n        // Antes, con vsync, cada vuelta dormia wait/0x100 ms (y el Sleep de\n        // Windows redondea a 15,6 ms si nadie sube la resolucion del reloj).\n        // NFS Most Wanted hace muchas de estas esperas por fotograma y casi\n        // todas se resuelven en microsegundos: la suma de dormidas le costaba\n        // mas de un periodo de vblank por fotograma. MEDIDO: con vsync, 30 fps\n        // y flips cada 16/33/50 ms; sin vsync -que aqui solo cedia el hilo-,\n        // cientos de fps y flips regulares.\n        const double waited_ms =\n            double(rex::chrono::Clock::QueryHostTickCount() - wait_start) * 1000.0 /\n            double(rex::chrono::Clock::QueryHostTickFrequency());\n        if (!REXCVAR_GET(vsync) || waited_ms < 2.0) {\n          rex::thread::MaybeYield();\n        } else {\n          rex::thread::Sleep(std::chrono::milliseconds(1));\n        }\n'),
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
     '\nbool D3D12CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader,\n                                                               uint32_t packet, uint32_t count) {\n  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_query_resources_available_) {\n    return CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(reader, packet, count);\n  }\n\n  const uint32_t kQueryFinished = rex::byte_swap(0xFFFFFEED);\n  assert_true(count == 1);\n',
     '\n// PARCHE LOCAL - cuentas de occlusion queries para el log (con log_guest_fps).\nnamespace {\nstruct OcclusionStats {\n  uint32_t events = 0, no_resources = 0, reports = 0, delivered = 0, waited = 0;\n  uint64_t delta_max = 0;\n  std::chrono::steady_clock::time_point window{};\n} g_occlusion_stats;\n\nvoid LogOcclusionStats(bool resources, size_t pending) {\n  auto& s = g_occlusion_stats;\n  const auto now = std::chrono::steady_clock::now();\n  if (s.window == std::chrono::steady_clock::time_point{}) {\n    s.window = now;\n  }\n  if (now - s.window < std::chrono::seconds(10)) {\n    return;\n  }\n  if (rex::cvar::GetFlagInfo("log_guest_fps") && rex::cvar::Query<bool>("log_guest_fps")) {\n    REXGPU_INFO(\n        "[occlusion] eventos {} | sin recursos {} | informes {} | entregados {} | con espera {} "\n        "| en cola {} | max muestras por intervalo {} | recursos {}",\n        s.events, s.no_resources, s.reports, s.delivered, s.waited, pending, s.delta_max,\n        resources ? "si" : "no");\n  }\n  s = {};\n  s.window = now;\n}\n}  // namespace\n\n// PARCHE LOCAL - ZPD como contador continuo (ver ZpdReport en la cabecera).\n//\n// MEDIDO en NFS Most Wanted, conduciendo: ~370 eventos por segundo, y NINGUNO\n// casaba con el modelo anterior de "consulta con principio y fin en la misma\n// direccion": el volcado de antes y el de despues van a direcciones distintas.\n// Todos acababan en el valor falso de "1000 muestras visibles", y el sol y sus\n// destellos se veian a traves de edificios y puentes.\nbool D3D12CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader,\n                                                               uint32_t packet, uint32_t count) {\n  ++g_occlusion_stats.events;\n  LogOcclusionStats(occlusion_query_resources_available_, zpd_reports_.size());\n  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_query_resources_available_) {\n    ++g_occlusion_stats.no_resources;\n    return CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(reader, packet, count);\n  }\n\n  assert_true(count == 1);\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #2',
     '\n  uint32_t sample_count_addr = register_file_->values[XE_GPU_REG_RB_SAMPLE_COUNT_ADDR];\n  auto* sample_counts =\n      memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(sample_count_addr);\n  if (!sample_counts) {\n    DisableHostOcclusionQueries();\n    return true;\n  }\n\n  auto write_fallback_result = [sample_counts, kQueryFinished]() -> bool {\n    auto fake_sample_count = REXCVAR_GET(query_occlusion_fake_sample_count);\n    if (fake_sample_count < 0) {\n      return true;\n    }\n    bool is_end_via_z_pass =\n        sample_counts->ZPass_A == kQueryFinished || sample_counts->ZPass_B == kQueryFinished;\n    bool is_end_via_z_fail =\n        sample_counts->ZFail_A == kQueryFinished || sample_counts->ZFail_B == kQueryFinished;\n    std::memset(sample_counts, 0, sizeof(xenos::xe_gpu_depth_sample_counts));\n    if (is_end_via_z_pass || is_end_via_z_fail) {\n      sample_counts->ZPass_A = fake_sample_count;\n      sample_counts->Total_A = fake_sample_count;\n    }\n    return true;\n  };\n\n  bool is_end_via_z_pass =\n      sample_counts->ZPass_A == kQueryFinished || sample_counts->ZPass_B == kQueryFinished;\n  bool is_end_via_z_fail =\n      sample_counts->ZFail_A == kQueryFinished || sample_counts->ZFail_B == kQueryFinished;\n  bool is_end = is_end_via_z_pass || is_end_via_z_fail;\n\n  if (!is_end) {\n    if (active_occlusion_query_.valid &&\n        active_occlusion_query_.sample_count_address != sample_count_addr) {\n      DisableHostOcclusionQueries();\n      return write_fallback_result();\n    }\n    if (!BeginGuestOcclusionQuery(sample_count_addr)) {\n      return write_fallback_result();\n    }\n    return true;\n  }\n\n  if (!active_occlusion_query_.valid ||\n      active_occlusion_query_.sample_count_address != sample_count_addr) {\n    DisableHostOcclusionQueries();\n    return write_fallback_result();\n  }\n\n  if (!EndGuestOcclusionQuery(sample_count_addr, sample_counts)) {\n    return write_fallback_result();\n  }\n\n  return true;\n',
     '\n  // Cerrar el intervalo medido desde el evento anterior, encolar su informe y\n  // abrir el siguiente.\n  ZpdCloseSegment();\n  ZpdReport report;\n  report.address = register_file_->values[XE_GPU_REG_RB_SAMPLE_COUNT_ADDR];\n  report.segments = std::move(zpd_interval_segments_);\n  zpd_interval_segments_.clear();\n  zpd_reports_.push_back(std::move(report));\n  ++g_occlusion_stats.reports;\n  ZpdOpenSegment();\n\n  ProcessPendingOcclusionQueries(false);\n  return true;\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #3',
     '    texture_cache_->BeginSubmission(submission_current_);\n  }\n',
     '    texture_cache_->BeginSubmission(submission_current_);\n\n    // PARCHE LOCAL - ZPD como contador continuo: seguir midiendo el intervalo\n    // que la submission anterior dejo partido.\n    if (zpd_segment_reopen_ && !active_occlusion_query_.valid && occlusion_query_heap_ &&\n        occlusion_query_resources_available_) {\n      zpd_segment_reopen_ = false;\n      uint32_t host_index = 0;\n      if (AcquireOcclusionQueryIndex(host_index)) {\n        deferred_command_list_.D3DBeginQuery(occlusion_query_heap_.Get(),\n                                             D3D12_QUERY_TYPE_OCCLUSION, host_index);\n        active_occlusion_query_.host_index = host_index;\n        active_occlusion_query_.valid = true;\n      }\n    }\n  }\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #4',
     '\n    if (active_occlusion_query_.valid && occlusion_query_heap_) {\n      deferred_command_list_.D3DEndQuery(occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,\n                                         active_occlusion_query_.host_index);\n      active_occlusion_query_ = {};\n    }\n',
     '\n    // PARCHE LOCAL - ZPD como contador continuo: el intervalo en curso se\n    // parte aqui y sigue midiendo en la proxima submission.\n    if (active_occlusion_query_.valid) {\n      ZpdCloseSegment();\n      zpd_segment_reopen_ = true;\n    }\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #5',
     '  DisableHostOcclusionQueries();\n\n',
     '  DisableHostOcclusionQueries();\n  zpd_reports_.clear();\n  zpd_interval_segments_.clear();\n\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #6',
     '  }\n  occlusion_query_cursor_ = 0;\n',
     '  }\n  zpd_segment_reopen_ = false;\n  zpd_interval_segments_.clear();\n  occlusion_query_cursor_ = 0;\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #7',
     '\nbool D3D12CommandProcessor::BeginGuestOcclusionQuery(uint32_t sample_count_address) {\n  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_query_resources_available_) {\n    return false;\n  }\n  if (active_occlusion_query_.valid) {\n    REXGPU_WARN(\n        "D3D12CommandProcessor: Occlusion query begin issued while another query is active");\n    DisableHostOcclusionQueries();\n    return false;\n  }\n\n  uint32_t host_index = 0;\n  if (!AcquireOcclusionQueryIndex(host_index)) {\n    return false;\n  }\n  if (!BeginSubmission(true)) {\n    return false;\n  }\n\n  deferred_command_list_.D3DBeginQuery(occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,\n                                       host_index);\n  active_occlusion_query_.sample_count_address = sample_count_address;\n  active_occlusion_query_.host_index = host_index;\n  active_occlusion_query_.valid = true;\n  return true;\n}\n\nbool D3D12CommandProcessor::EndGuestOcclusionQuery(\n    uint32_t sample_count_address, xenos::xe_gpu_depth_sample_counts* sample_counts) {\n  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_query_resources_available_ ||\n      !active_occlusion_query_.valid || !occlusion_query_heap_ || !occlusion_query_readback_) {\n    return false;\n  }\n\n  uint32_t host_index = active_occlusion_query_.host_index;\n  active_occlusion_query_ = {};\n\n  if (!BeginSubmission(true)) {\n    return false;\n  }\n\n  deferred_command_list_.D3DEndQuery(occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,\n',
     '\n// PARCHE LOCAL - ZPD como contador continuo: abre un segmento de medida en la\n// submission actual (una consulta de D3D12 no puede cruzar listas de comandos).\nvoid D3D12CommandProcessor::ZpdOpenSegment() {\n  zpd_segment_reopen_ = false;\n  if (active_occlusion_query_.valid || !occlusion_query_resources_available_ ||\n      !occlusion_query_heap_) {\n    return;\n  }\n  uint32_t host_index = 0;\n  if (!AcquireOcclusionQueryIndex(host_index) || !BeginSubmission(true)) {\n    return;\n  }\n  deferred_command_list_.D3DBeginQuery(occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,\n                                       host_index);\n  active_occlusion_query_.host_index = host_index;\n  active_occlusion_query_.valid = true;\n}\n\n// Cierra el segmento abierto y lo apunta en el intervalo en curso. La\n// resolucion va en la misma submission, asi que el resultado esta en el\n// readback en cuanto esa submission termina en la GPU.\nvoid D3D12CommandProcessor::ZpdCloseSegment() {\n  if (!active_occlusion_query_.valid || !occlusion_query_heap_ || !occlusion_query_readback_) {\n    active_occlusion_query_ = {};\n    return;\n  }\n  const uint32_t host_index = active_occlusion_query_.host_index;\n  active_occlusion_query_ = {};\n  deferred_command_list_.D3DEndQuery(occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #8',
     '      occlusion_query_readback_.Get(), sizeof(uint64_t) * host_index);\n\n  if (!EndSubmission(false)) {\n    return false;\n  }\n\n  uint64_t query_submission = submission_current_ ? submission_current_ - 1 : 0;\n  CheckSubmissionFence(query_submission);\n  if (submission_completed_ < query_submission) {\n    return false;\n  }\n  if (!occlusion_query_readback_mapping_) {\n    return false;\n  }\n\n  uint64_t samples = occlusion_query_readback_mapping_[host_index];\n  samples = NormalizeOcclusionSamples(samples);\n  WriteGuestOcclusionResult(sample_counts, samples);\n  return true;\n}\n',
     '      occlusion_query_readback_.Get(), sizeof(uint64_t) * host_index);\n  ZpdSegment segment;\n  segment.host_index = host_index;\n  segment.submission = submission_current_;\n  zpd_interval_segments_.push_back(segment);\n}\n\n// Entrega los informes en orden: cada uno lleva el valor del contador continuo\n// tras sumar las muestras de su intervalo. El D3D del juego resta dos\n// informes; aqui solo hace falta que el contador avance como el de la Xenos.\nvoid D3D12CommandProcessor::ProcessPendingOcclusionQueries(bool wait) {\n  if (zpd_reports_.empty()) {\n    return;\n  }\n  CheckSubmissionFence(0);\n  while (!zpd_reports_.empty()) {\n    ZpdReport& report = zpd_reports_.front();\n    uint64_t needed = 0;\n    for (const ZpdSegment& segment : report.segments) {\n      needed = std::max(needed, segment.submission);\n    }\n    if (!report.segments.empty() && submission_completed_ < needed) {\n      if (!wait) {\n        break;\n      }\n      CheckSubmissionFence(needed);\n      if (submission_completed_ < needed) {\n        break;\n      }\n      ++g_occlusion_stats.waited;\n    }\n    uint64_t delta = 0;\n    if (occlusion_query_readback_mapping_) {\n      for (const ZpdSegment& segment : report.segments) {\n        delta += occlusion_query_readback_mapping_[segment.host_index];\n      }\n    }\n    delta = NormalizeOcclusionSamples(delta);\n    zpd_counter_ += uint32_t(delta);\n    g_occlusion_stats.delta_max = std::max(g_occlusion_stats.delta_max, delta);\n    ++g_occlusion_stats.delivered;\n    auto* sample_counts =\n        memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(report.address);\n    WriteGuestOcclusionResult(sample_counts, zpd_counter_);\n    zpd_reports_.pop_front();\n  }\n}\n\nvoid D3D12CommandProcessor::PrepareForWait() {\n  CommandProcessor::PrepareForWait();\n  // El guest se queda esperando -sin comandos nuevos o en un WAIT_REG_MEM-:\n  // puede que este esperando justo uno de estos resultados, asi que se espera\n  // a la GPU y se entregan todos.\n  ProcessPendingOcclusionQueries(true);\n}\n'),
    ('src/kernel/xboxkrnl/xboxkrnl_video.cpp',
     'xboxkrnl/xboxkrnl_video.cpp #1',
     '                  mapped_u32 height) {\n  // All of these parameters are REQUIRED.\n',
     '                  mapped_u32 height) {\n  // PARCHE LOCAL - ritmo de VdSwap visto desde el guest (diagnostico, con\n  // log_guest_fps). Compararlo con el del procesador de comandos dice si los\n  // tirones los mete el juego o los mete la emulacion de la GPU.\n  static bool log_fps = rex::cvar::GetFlagInfo("log_guest_fps") != nullptr;\n  if (log_fps && rex::cvar::Query<bool>("log_guest_fps")) {\n    using clk = std::chrono::steady_clock;\n    static clk::time_point window_start{}, last{};\n    static uint32_t swaps = 0, over25 = 0, over50 = 0;\n    static double min_ms = 1e9, max_ms = 0.0;\n    const auto now = clk::now();\n    if (window_start == clk::time_point{}) window_start = now;\n    if (last != clk::time_point{}) {\n      const double ms = std::chrono::duration<double, std::milli>(now - last).count();\n      min_ms = std::min(min_ms, ms);\n      max_ms = std::max(max_ms, ms);\n      over25 += ms > 25.0;\n      over50 += ms > 50.0;\n    }\n    last = now;\n    ++swaps;\n    const double win = std::chrono::duration<double>(now - window_start).count();\n    if (win >= 10.0) {\n      REXKRNL_INFO("[VdSwap fps] {:.1f} swaps/s | frame {:.1f}-{:.1f} ms | >25ms {} | >50ms {}",\n                   swaps / win, min_ms, max_ms, over25, over50);\n      window_start = now;\n      swaps = over25 = over50 = 0;\n      min_ms = 1e9;\n      max_ms = 0.0;\n    }\n  }\n\n  // All of these parameters are REQUIRED.\n'),
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
