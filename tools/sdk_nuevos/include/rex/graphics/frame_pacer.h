/**
 * @file        rex/graphics/frame_pacer.h
 * @brief       PARCHE LOCAL - ritmo de fotogramas (frame_pacing_fps).
 *
 * Lo usan dos sitios, segun frame_pacing_at_guest:
 *  - VdSwap (kernel, hilo del juego): el juego espera su turno al entregar el
 *    fotograma, como en la Xbox, y no puede adelantarse. Es lo que se usa.
 *  - XE_SWAP (procesador de comandos): la version anterior. El juego se
 *    adelantaba y el anillo guardaba ~3 fotogramas: MEDIDO 49,8 ms desde el
 *    VdSwap hasta la salida, de los que la espera del ritmo eran 14,8.
 *
 * Solo cabecera: el estado vive en variables estaticas de la funcion inline,
 * una copia por DLL, y en cada momento solo la usa uno de los dos sitios.
 */
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

#include <rex/chrono/clock.h>
#include <rex/logging.h>
#include <rex/thread.h>

#if defined(_WIN32)
#include <windows.h>
#include <dwmapi.h>  // solo el tipo DWM_TIMING_INFO; la funcion se busca en tiempo de ejecucion
#endif

namespace rex::graphics {

struct FramePacerTiming {
  double late_ms = 0.0;    // cuanto llego tarde el fotograma a su turno
  double sleep_ms = 0.0;   // cuanto se espero
  double over_ms = 0.0;    // cuanto se paso el Sleep
  double period_ms = 0.0;  // periodo usado
  bool reset = false;      // el reloj se reengancho (carga, pausa)
};

// Un fotograma cada 1/fps segundos, contando desde el turno PREVISTO y no desde
// el real, para que un retraso suelto no desplace a todos los demas.
//
// display_lock: el periodo es un multiplo exacto del refresco real que da el
// compositor (DwmGetCompositionTimingInfo, en unidades de QPC como nuestro
// reloj) y la fase queda phase_percent de refresco despues de un vblank. Hace
// falta con vsync: un monitor "de 120 Hz" midio 120,2429 Hz y con un reloj
// propio de 60,000 fps habia un tiron cada 8,3 s. Sin vsync y con
// G-Sync/FreeSync NO: el "vblank" del compositor es nuestro propio present y
// recolocar la fase sobre el desplazaba el reloj hasta medio refresco.
inline FramePacerTiming PaceFrame(int32_t fps, bool display_lock, int32_t phase_percent) {
  FramePacerTiming t;
  if (fps <= 0) {
    return t;
  }
  static uint64_t next_flip = 0;
  static uint32_t flips_since_sync = 0;
  const uint64_t freq = rex::chrono::Clock::QueryHostTickFrequency();
  uint64_t period = freq / uint64_t(fps);
  const uint64_t now = rex::chrono::Clock::QueryHostTickCount();
#if defined(_WIN32)
  using DwmTimingFn = HRESULT(WINAPI*)(HWND, DWM_TIMING_INFO*);
  static DwmTimingFn dwm_timing = [] {
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    auto fn = dwm ? reinterpret_cast<DwmTimingFn>(
                        GetProcAddress(dwm, "DwmGetCompositionTimingInfo"))
                  : nullptr;
    if (!fn) {
      REXLOG_WARN("[pacing] sin DwmGetCompositionTimingInfo: ritmo con reloj propio");
    }
    return fn;
  }();
  static uint64_t refresh = 0, vblank = 0;
  if (!display_lock) {
    refresh = 0;  // reloj propio; al volver el enganche se vuelve a medir
  } else if (dwm_timing && (refresh == 0 || ++flips_since_sync >= 60)) {
    flips_since_sync = 0;
    DWM_TIMING_INFO ti = {};
    ti.cbSize = sizeof(ti);
    const HRESULT hr = dwm_timing(nullptr, &ti);
    // Se apunta cada vez que el refresco cambia mas de un 0,05 %.
    if (SUCCEEDED(hr) && ti.qpcRefreshPeriod > 0 &&
        (refresh == 0 ||
         std::abs(double(ti.qpcRefreshPeriod) - double(refresh)) > double(refresh) * 0.0005)) {
      REXLOG_INFO("[pacing] refresco del monitor {:.4f} Hz",
                  double(freq) / double(ti.qpcRefreshPeriod));
    }
    if (SUCCEEDED(hr) && ti.qpcRefreshPeriod > 0) {
      refresh = ti.qpcRefreshPeriod;
      vblank = ti.qpcVBlank;
    }
  }
  if (refresh > 0) {
    const uint64_t refreshes = std::max<uint64_t>(1, (period + refresh / 2) / refresh);
    period = refreshes * refresh;
    if (next_flip) {
      // Recolocar la fase: vblank + n refrescos + phase_percent de refresco.
      const uint64_t phase = vblank + refresh * uint64_t(phase_percent) / 100;
      const int64_t n =
          int64_t(std::llround(double(int64_t(next_flip - phase)) / double(refresh)));
      next_flip = uint64_t(int64_t(phase) + n * int64_t(refresh));
    }
  }
#endif
  t.period_ms = double(period) * 1000.0 / double(freq);
  if (next_flip && now > next_flip) {
    t.late_ms = double(now - next_flip) * 1000.0 / double(freq);
  }
  // Si vamos mas de un periodo tarde (carga, pausa), se reengancha el reloj.
  if (!next_flip || now > next_flip + period) {
    t.reset = next_flip != 0;
    next_flip = now;
  }
  if (now < next_flip) {
    const uint64_t target = next_flip;
    const double left_ms = double(target - now) * 1000.0 / double(freq);
    t.sleep_ms = left_ms;
    // Dormir casi todo y rematar cediendo el hilo.
    if (left_ms > 1.5) {
      rex::thread::Sleep(std::chrono::milliseconds(int(left_ms - 1.0)));
    }
    uint64_t woke = rex::chrono::Clock::QueryHostTickCount();
    if (woke > target) {
      t.over_ms = double(woke - target) * 1000.0 / double(freq);
    }
    while (woke < target) {
      rex::thread::MaybeYield();
      woke = rex::chrono::Clock::QueryHostTickCount();
    }
  }
  next_flip += period;
  return t;
}

}  // namespace rex::graphics
