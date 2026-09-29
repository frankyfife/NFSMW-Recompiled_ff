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
    Occlusion queries diferidas: el resultado se escribe cuando la GPU del
    host lo tiene, no se sustituye por "1000 muestras visibles" si aun no
    estaba; y un desajuste ya no apaga las consultas reales para siempre.
    Con log_guest_fps, cuentas de consultas cada 10 s ([occlusion]).

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
     '\n#if defined(_WIN32)\n#ifndef NOMINMAX\n#define NOMINMAX\n#endif\n#ifndef WIN32_LEAN_AND_MEAN\n#define WIN32_LEAN_AND_MEAN\n#endif\n#include <windows.h>\n#include <dwmapi.h>  // solo el tipo DWM_TIMING_INFO; la funcion se busca en tiempo de ejecucion\n#endif\n\nREXCVAR_DEFINE_BOOL(vsync, true, "GPU", "Enable vertical sync");\n\nREXCVAR_DEFINE_BOOL(log_guest_fps, false, "GPU",\n                    "Log how many frames per second the game presents, every 10 seconds");\n\n// PARCHE LOCAL - ritmo de los flips\nREXCVAR_DEFINE_INT32(frame_pacing_fps, 0, "GPU",\n                     "Present guest frames at an exact, even rate (e.g. 30 or 60). Combine with "\n                     "a fast guest_vblank_rate so the game never misses a vblank slot. 0 = off.")\n    .range(0, 1000)\n    .lifecycle(rex::cvar::Lifecycle::kHotReload);\n\n'),
    ('src/graphics/command_processor.cpp',
     'graphics/command_processor.cpp #2',
     '\n  IssueSwap(frontbuffer_ptr, frontbuffer_width, frontbuffer_height);\n\n',
     '\n  // PARCHE LOCAL - ritmo de los flips (frame_pacing_fps)\n  //\n  // Con vblank de 60 Hz el juego coloca cada flip en una "ranura" de vblank, y\n  // si llega tarde por un pelo pierde la ranura entera: los flips salian cada\n  // 16,6, 33 o 50 ms en vez de cada 33 -el tiron que se veia a "30 fps"-. La\n  // forma buena es darle vblanks rapidos (guest_vblank_rate alto, no pierde\n  // ninguna) y marcar el ritmo aqui con un reloj preciso: un flip cada\n  // 1/frame_pacing_fps segundos, contando desde el flip PREVISTO y no desde el\n  // real, para que un retraso suelto no desplace a todos los demas.\n  //\n  // MEDIDO en el menu (vblank 1000 Hz, sin vsync): 30 fps con flips cada\n  // 33,1-33,5 ms, 60 fps con 16,4-16,9 ms. Con vsync del host encima vuelve a\n  // haber dos relojes y temblor de un refresco del monitor.\n  //\n  // Y el reloj va enganchado al del monitor. Un monitor "de 120 Hz" midio\n  // 120,2429 Hz: con un reloj propio de 60,000 fps cada 8,3 s sobraba o\n  // faltaba un refresco, y se veia como un tiron periodico. Asi que, en\n  // Windows, el periodo es un multiplo exacto del refresco real que da el\n  // compositor (DwmGetCompositionTimingInfo, en unidades de QPC como nuestro\n  // reloj) y la fase queda a un cuarto de refresco despues de un vblank, lejos\n  // del borde entre dos refrescos.\n  if (const int32_t pacing_fps = REXCVAR_GET(frame_pacing_fps); pacing_fps > 0) {\n    static uint64_t next_flip = 0;\n    static uint32_t flips_since_sync = 0;\n    const uint64_t freq = rex::chrono::Clock::QueryHostTickFrequency();\n    uint64_t period = freq / uint64_t(pacing_fps);\n    uint64_t now = rex::chrono::Clock::QueryHostTickCount();\n#if defined(_WIN32)\n    using DwmTimingFn = HRESULT(WINAPI*)(HWND, DWM_TIMING_INFO*);\n    static DwmTimingFn dwm_timing = [] {\n      HMODULE dwm = LoadLibraryW(L"dwmapi.dll");\n      auto fn = dwm ? reinterpret_cast<DwmTimingFn>(GetProcAddress(dwm, "DwmGetCompositionTimingInfo"))\n                    : nullptr;\n      if (!fn) {\n        REXGPU_WARN("[pacing] sin DwmGetCompositionTimingInfo: ritmo con reloj propio");\n      }\n      return fn;\n    }();\n    static uint64_t refresh = 0, vblank = 0;\n    if (dwm_timing && (refresh == 0 || ++flips_since_sync >= 60)) {\n      flips_since_sync = 0;\n      DWM_TIMING_INFO ti = {};\n      ti.cbSize = sizeof(ti);\n      const HRESULT hr = dwm_timing(nullptr, &ti);\n      // Se apunta cada vez que el refresco cambia mas de un 0,05 %: con VRR o al\n      // pasar a pantalla completa cambia, y es lo primero que mirar si vuelve\n      // un tiron periodico.\n      if (SUCCEEDED(hr) && ti.qpcRefreshPeriod > 0 &&\n          (refresh == 0 || std::abs(double(ti.qpcRefreshPeriod) - double(refresh)) >\n                               double(refresh) * 0.0005)) {\n        REXGPU_INFO("[pacing] refresco del monitor {:.4f} Hz",\n                    double(freq) / double(ti.qpcRefreshPeriod));\n      }\n      if (SUCCEEDED(hr) && ti.qpcRefreshPeriod > 0) {\n        refresh = ti.qpcRefreshPeriod;\n        vblank = ti.qpcVBlank;\n      }\n    }\n    if (refresh > 0) {\n      const uint64_t refreshes = std::max<uint64_t>(1, (period + refresh / 2) / refresh);\n      period = refreshes * refresh;\n      if (next_flip) {\n        // Recolocar la fase: vblank + n refrescos + 1/4 de refresco.\n        const uint64_t phase = vblank + refresh / 4;\n        const int64_t n = int64_t(std::llround(double(int64_t(next_flip - phase)) / double(refresh)));\n        next_flip = uint64_t(int64_t(phase) + n * int64_t(refresh));\n      }\n    }\n#endif\n    // Si vamos mas de un periodo tarde (carga, pausa), se reengancha el reloj.\n    if (!next_flip || now > next_flip + period) {\n      next_flip = now;\n    }\n    if (now < next_flip) {\n      const uint64_t target = next_flip;\n      const double left_ms = double(target - now) * 1000.0 / double(freq);\n      if (left_ms > 1.5) {\n        rex::thread::Sleep(std::chrono::milliseconds(int(left_ms - 1.0)));\n      }\n      while (rex::chrono::Clock::QueryHostTickCount() < target) {\n        rex::thread::MaybeYield();\n      }\n    }\n    next_flip += period;\n  }\n\n  IssueSwap(frontbuffer_ptr, frontbuffer_width, frontbuffer_height);\n\n  // PARCHE LOCAL - fps del guest en el log\n  //\n  // Cuantas veces presenta el juego por segundo, medido aqui y no en el host:\n  // es lo unico que dice si guest_vblank_rate cambio de verdad el ritmo del\n  // juego. Una linea cada 10 s, solo con log_guest_fps.\n  if (REXCVAR_GET(log_guest_fps)) {\n    static uint64_t window_start = 0;\n    static uint64_t last_swap = 0;\n    static uint32_t swaps = 0;\n    // La media dice poco de un tiron: se apuntan tambien el abstand minimo y\n    // maximo entre fotogramas y cuantos pasaron de 25 y de 50 ms.\n    static double min_ms = 1e9, max_ms = 0.0;\n    static uint32_t over25 = 0, over50 = 0;\n    const uint64_t now = rex::chrono::Clock::QueryHostTickCount();\n    const uint64_t freq = rex::chrono::Clock::QueryHostTickFrequency();\n    if (!window_start) {\n      window_start = now;\n    }\n    if (last_swap) {\n      const double ms = double(now - last_swap) * 1000.0 / double(freq);\n      min_ms = std::min(min_ms, ms);\n      max_ms = std::max(max_ms, ms);\n      over25 += ms > 25.0;\n      over50 += ms > 50.0;\n    }\n    last_swap = now;\n    ++swaps;\n    if (now - window_start >= freq * 10) {\n      REXGPU_INFO("[guest fps] {:.1f} swaps/s | frame {:.1f}-{:.1f} ms | >25ms {} | >50ms {}",\n                  swaps * double(freq) / double(now - window_start), min_ms, max_ms, over25,\n                  over50);\n      window_start = now;\n      swaps = 0;\n      min_ms = 1e9;\n      max_ms = 0.0;\n      over25 = over50 = 0;\n    }\n  }\n\n'),
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
     '  void DisableHostOcclusionQueries();\n  uint64_t NormalizeOcclusionSamples(uint64_t samples) const;\n',
     '  void DisableHostOcclusionQueries();\n  // PARCHE LOCAL - occlusion queries diferidas.\n  void AbandonActiveOcclusionQuery();\n  void ProcessPendingOcclusionQueries(bool wait);\n  uint64_t NormalizeOcclusionSamples(uint64_t samples) const;\n'),
    ('include/rex/graphics/d3d12/command_processor.h',
     'd3d12/command_processor.h #3',
     '  } active_occlusion_query_;\n  struct VertexBufferState {\n',
     '  } active_occlusion_query_;\n  // PARCHE LOCAL - occlusion queries diferidas: terminadas en la GPU del host\n  // pero con el resultado todavia sin escribir en la memoria del guest, que\n  // mientras tanto conserva la marca 0xFFFFFEED ("aun no esta").\n  struct PendingOcclusionQuery {\n    uint32_t sample_count_address = 0;\n    uint32_t host_index = 0;\n    uint64_t submission = 0;\n  };\n  std::deque<PendingOcclusionQuery> pending_occlusion_queries_;\n  struct VertexBufferState {\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #1',
     '\nbool D3D12CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader,\n                                                               uint32_t packet, uint32_t count) {\n  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_query_resources_available_) {\n    return CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(reader, packet, count);\n',
     '\n// PARCHE LOCAL - cuentas de occlusion queries para el log (con log_guest_fps).\nnamespace {\nstruct OcclusionStats {\n  uint32_t events = 0, no_resources = 0, begins = 0, ends = 0, fallbacks = 0, delivered = 0,\n           stale = 0;\n  uint64_t samples_max = 0;\n  std::chrono::steady_clock::time_point window{};\n} g_occlusion_stats;\n\nvoid LogOcclusionStats(bool resources) {\n  auto& s = g_occlusion_stats;\n  const auto now = std::chrono::steady_clock::now();\n  if (s.window == std::chrono::steady_clock::time_point{}) {\n    s.window = now;\n  }\n  if (now - s.window < std::chrono::seconds(10)) {\n    return;\n  }\n  if (rex::cvar::GetFlagInfo("log_guest_fps") && rex::cvar::Query<bool>("log_guest_fps")) {\n    REXGPU_INFO(\n        "[occlusion] eventos {} | sin recursos {} | inicios {} | finales {} | entregados {} | "\n        "caducados {} | falsos {} | max muestras {} | recursos {}",\n        s.events, s.no_resources, s.begins, s.ends, s.delivered, s.stale, s.fallbacks,\n        s.samples_max, resources ? "si" : "no");\n  }\n  s = {};\n  s.window = now;\n}\n}  // namespace\n\nbool D3D12CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader,\n                                                               uint32_t packet, uint32_t count) {\n  ++g_occlusion_stats.events;\n  LogOcclusionStats(occlusion_query_resources_available_);\n  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_query_resources_available_) {\n    ++g_occlusion_stats.no_resources;\n    return CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(reader, packet, count);\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #2',
     '  WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);\n\n',
     '  WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);\n\n  // Entregar lo que la GPU del host ya haya terminado.\n  ProcessPendingOcclusionQueries(false);\n\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #3',
     '  auto write_fallback_result = [sample_counts, kQueryFinished]() -> bool {\n    auto fake_sample_count = REXCVAR_GET(query_occlusion_fake_sample_count);\n',
     '  auto write_fallback_result = [sample_counts, kQueryFinished]() -> bool {\n    ++g_occlusion_stats.fallbacks;\n    auto fake_sample_count = REXCVAR_GET(query_occlusion_fake_sample_count);\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #4',
     '\n  if (!is_end) {\n    if (active_occlusion_query_.valid &&\n        active_occlusion_query_.sample_count_address != sample_count_addr) {\n      DisableHostOcclusionQueries();\n      return write_fallback_result();\n    }\n',
     '\n  // PARCHE LOCAL - un inicio o un final que no casa con la consulta activa ya\n  // no apaga las consultas de verdad para siempre (DisableHostOcclusionQueries):\n  // solo se pierde esa consulta. Antes, el primer desajuste dejaba el resto de\n  // la partida con el valor falso de "1000 muestras visibles", y el sol se veia\n  // a traves de los objetos.\n  if (!is_end) {\n    ++g_occlusion_stats.begins;\n    if (active_occlusion_query_.valid) {\n      AbandonActiveOcclusionQuery();\n    }\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #5',
     '\n  if (!active_occlusion_query_.valid ||\n      active_occlusion_query_.sample_count_address != sample_count_addr) {\n    DisableHostOcclusionQueries();\n    return write_fallback_result();\n',
     '\n  ++g_occlusion_stats.ends;\n  if (!active_occlusion_query_.valid ||\n      active_occlusion_query_.sample_count_address != sample_count_addr) {\n    AbandonActiveOcclusionQuery();\n    return write_fallback_result();\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #6',
     '  DisableHostOcclusionQueries();\n\n',
     '  DisableHostOcclusionQueries();\n  pending_occlusion_queries_.clear();\n\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #7',
     '  if (active_occlusion_query_.valid) {\n    REXGPU_WARN(\n        "D3D12CommandProcessor: Occlusion query begin issued while another query is active");\n    DisableHostOcclusionQueries();\n    return false;\n  }\n',
     '  if (active_occlusion_query_.valid) {\n    AbandonActiveOcclusionQuery();\n  }\n'),
    ('src/graphics/d3d12/command_processor.cpp',
     'd3d12/command_processor.cpp #8',
     '\n  uint64_t query_submission = submission_current_ ? submission_current_ - 1 : 0;\n  CheckSubmissionFence(query_submission);\n  if (submission_completed_ < query_submission) {\n    return false;\n  }\n  if (!occlusion_query_readback_mapping_) {\n    return false;\n  }\n\n  uint64_t samples = occlusion_query_readback_mapping_[host_index];\n  samples = NormalizeOcclusionSamples(samples);\n  WriteGuestOcclusionResult(sample_counts, samples);\n  return true;\n}\n',
     '\n  // PARCHE LOCAL - resultado diferido.\n  //\n  // Antes se miraba la valla justo despues de enviar el trabajo, y como la GPU\n  // va por detras casi nunca estaba terminado: se caia al valor falso de\n  // "visible" practicamente siempre. En la Xbox el resultado tampoco es\n  // inmediato: la memoria conserva la marca 0xFFFFFEED hasta que la GPU\n  // escribe la cuenta, y el juego vuelve a preguntar. Asi que aqui se hace lo\n  // mismo: se deja la marca y se escribe cuando la GPU del host termina.\n  (void)sample_counts;\n  PendingOcclusionQuery pending;\n  pending.sample_count_address = sample_count_address;\n  pending.host_index = host_index;\n  pending.submission = submission_current_ ? submission_current_ - 1 : 0;\n  pending_occlusion_queries_.push_back(pending);\n  ProcessPendingOcclusionQueries(false);\n  return true;\n}\n\nvoid D3D12CommandProcessor::AbandonActiveOcclusionQuery() {\n  if (!active_occlusion_query_.valid) {\n    return;\n  }\n  uint32_t host_index = active_occlusion_query_.host_index;\n  active_occlusion_query_ = {};\n  if (occlusion_query_heap_ && BeginSubmission(true)) {\n    deferred_command_list_.D3DEndQuery(occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,\n                                       host_index);\n  }\n}\n\nvoid D3D12CommandProcessor::ProcessPendingOcclusionQueries(bool wait) {\n  if (pending_occlusion_queries_.empty()) {\n    return;\n  }\n  const uint32_t kQueryFinished = rex::byte_swap(0xFFFFFEED);\n  CheckSubmissionFence(0);\n  while (!pending_occlusion_queries_.empty()) {\n    const PendingOcclusionQuery p = pending_occlusion_queries_.front();\n    if (submission_completed_ < p.submission) {\n      if (!wait) {\n        break;\n      }\n      CheckSubmissionFence(p.submission);\n      if (submission_completed_ < p.submission) {\n        break;\n      }\n    }\n    pending_occlusion_queries_.pop_front();\n    auto* sample_counts =\n        memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(p.sample_count_address);\n    if (!sample_counts || !occlusion_query_readback_mapping_) {\n      continue;\n    }\n    // Solo si el guest sigue esperando este resultado (la marca sigue ahi). Si\n    // ya reutilizo la memoria para otra consulta, no se pisa.\n    bool waiting = sample_counts->ZPass_A == kQueryFinished ||\n                   sample_counts->ZPass_B == kQueryFinished ||\n                   sample_counts->ZFail_A == kQueryFinished ||\n                   sample_counts->ZFail_B == kQueryFinished;\n    if (!waiting) {\n      ++g_occlusion_stats.stale;\n      continue;\n    }\n    uint64_t samples = NormalizeOcclusionSamples(occlusion_query_readback_mapping_[p.host_index]);\n    ++g_occlusion_stats.delivered;\n    g_occlusion_stats.samples_max = std::max(g_occlusion_stats.samples_max, samples);\n    WriteGuestOcclusionResult(sample_counts, samples);\n  }\n}\n\nvoid D3D12CommandProcessor::PrepareForWait() {\n  CommandProcessor::PrepareForWait();\n  // El guest se queda esperando -sin comandos nuevos o en un WAIT_REG_MEM-:\n  // puede que este esperando justo uno de estos resultados, asi que se espera\n  // a la GPU y se entregan todos.\n  ProcessPendingOcclusionQueries(true);\n}\n'),
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
