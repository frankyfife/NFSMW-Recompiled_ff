// nfsmw - menu de ajustes ingame (estilo GoldenEye, abierto con ESC)
//
// Manejado por cvars del recomp. Hay dos grupos, y cada control avisa de cual
// es:
//   * "se aplica al instante"  -> el SDK o un parche del proyecto refresca el
//                                 valor en vivo (fullscreen, vsync, max_fps,
//                                 anisotropic_override, game_speed).
//   * "se aplica al reiniciar" -> el SDK los marca como pendientes de reinicio
//                                 (lifecycle kRequiresRestart), asi que el
//                                 menu muestra el boton "Aplicar y reiniciar",
//                                 que guarda el toml y relanza el .exe.

#include "nfsmw_menu.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// ---------------------------------------------------------------------------
//  Cvar del proyecto: contenido Black Edition.
//
//  Se aplica al cargar el XEX (OnPostLoadXexImage en nfsmw_app.h) y tambien en
//  vivo desde el menu: la bandera es un byte que el juego relee cada vez que
//  la consulta, asi que basta con escribirlo.
// ---------------------------------------------------------------------------
REXCVAR_DEFINE_BOOL(black_edition, true, "Content",
                    "Black Edition content: the paid cars as downloadable content")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// La patch de Xenia es  be32 0x82A2CE04 = 0x00000100, es decir, en la memoria
// del guest (big-endian) los bytes 00 00 01 00: la bandera es el BYTE
// 0x82A2CE06. El getter del juego es sub_822C62E0: lbz r3,-12794(0x82A30000).
//
// La version anterior escribia un uint32_t 0x00000100 del host sobre 0x82A2CE04.
// El host es little-endian, asi que quedaban 00 01 00 00: ponia a 1 el byte
// 0x82A2CE05 -otra bandera, la que lee sub_822CBC08- y no la de Black Edition.
// Con eso "Nueva partida" reventaba en sub_822CC308 (coche que no esta en
// FEPlayerCarDB -> lectura de la direccion 8). Por eso aqui se toca un byte.
constexpr uint32_t kBlackEditionByte = 0x82A2CE06u;

bool AplicarBlackEdition(bool activo) {
  auto* kernel = rex::system::kernel_state();
  if (kernel == nullptr || kernel->memory() == nullptr) {
    REXLOG_WARN("[black-edition] no kernel memory; cannot patch.");
    return false;
  }
  auto* bandera = kernel->memory()->TranslateVirtual<uint8_t*>(kBlackEditionByte);
  if (bandera == nullptr) {
    REXLOG_WARN("[black-edition] could not translate 0x{:08X}.", kBlackEditionByte);
    return false;
  }
  *bandera = activo ? 1 : 0;
  REXLOG_INFO("[black-edition] byte 0x{:08X} = {} ({}).", kBlackEditionByte, *bandera,
              activo ? "content unlocked" : "content hidden");
  return true;
}

// ---------------------------------------------------------------------------
//  Cvar del proyecto: desbloquearlo todo (UnlockAllThings).
//
//  Es la variable de depuracion del propio juego. En la version de PC es el
//  byte 0x926124: es el que pone NFSMWExtraOptions ("UnlockAllThings ...
//  Everything is now unlocked") y el primero que escribe el manejador de
//  trucos (0x926124, luego 0x926126 y 0x926125). El manejador de trucos de la
//  Xbox, sub_823C4CF0, hace lo mismo en el mismo orden: caso 0 -> 0x82A2CE00,
//  caso 1 -> 0x82A2CE02 y 0x82A2CE01. Y como en PC (25 lecturas) aqui hay
//  una treintena de funciones que lo leen, varias junto a la bandera Black
//  Edition. No toca la partida guardada: con la opcion apagada todo vuelve a
//  estar como estaba.
// ---------------------------------------------------------------------------
REXCVAR_DEFINE_BOOL(unlock_all, false, "Content",
                    "Unlock everything: cars, parts, events and hidden tracks")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

constexpr uint32_t kUnlockAllByte = 0x82A2CE00u;

bool AplicarUnlockAll(bool activo) {
  auto* kernel = rex::system::kernel_state();
  if (kernel == nullptr || kernel->memory() == nullptr) {
    REXLOG_WARN("[unlock-all] no kernel memory; cannot patch.");
    return false;
  }
  auto* bandera = kernel->memory()->TranslateVirtual<uint8_t*>(kUnlockAllByte);
  if (bandera == nullptr) {
    REXLOG_WARN("[unlock-all] could not translate 0x{:08X}.", kUnlockAllByte);
    return false;
  }
  *bandera = activo ? 1 : 0;
  REXLOG_INFO("[unlock-all] byte 0x{:08X} = {} ({}).", kUnlockAllByte, *bandera,
              activo ? "everything unlocked" : "normal progress");
  return true;
}

namespace {

// Paleta del menu: inspirada en la interfaz del juego (fondo oscuro, acento
// naranja).
constexpr ImU32 kBg = IM_COL32(22, 26, 32, 252);
constexpr ImU32 kBorde = IM_COL32(232, 161, 60, 255);
constexpr ImU32 kMarco = IM_COL32(64, 72, 84, 255);
constexpr ImU32 kTexto = IM_COL32(220, 226, 232, 255);
constexpr ImU32 kTextoAtenuado = IM_COL32(148, 156, 168, 255);
constexpr ImU32 kAcento = IM_COL32(232, 161, 60, 255);
constexpr ImU32 kTabSel = IM_COL32(58, 66, 78, 255);
constexpr ImU32 kFondoWidget = IM_COL32(38, 44, 52, 255);
constexpr ImU32 kVivo = IM_COL32(96, 200, 86, 255);
constexpr ImU32 kAviso = IM_COL32(232, 161, 60, 255);

const char* kTitulosPestana[] = {"VIDEO", "GAME", "SYSTEM", "DEBUG"};
constexpr int kNumPestanas = 4;

// Acceso a cvars como strings (como hace el menu de GoldenEye).
bool CvarB(const char* nombre) { return rex::cvar::GetFlagByName(nombre) == "true"; }
void SetCvarB(const char* nombre, bool valor) {
  rex::cvar::SetFlagByName(nombre, valor ? "true" : "false");
}
std::string CvarS(const char* nombre) { return rex::cvar::GetFlagByName(nombre); }
void SetCvarS(const char* nombre, const std::string& valor) {
  rex::cvar::SetFlagByName(nombre, valor);
}
float CvarF(const char* nombre) { return static_cast<float>(std::atof(CvarS(nombre).c_str())); }
void SetCvarF(const char* nombre, float valor) {
  rex::cvar::SetFlagByName(nombre, std::to_string(valor));
}
bool ExisteCvar(const char* nombre) { return rex::cvar::GetFlagInfo(nombre) != nullptr; }

struct Opcion {
  const char* etiqueta;
  const char* valor;
};

// Fila nombre/valor para la pestana DEBUG. "-" cuando el cvar no existe o esta
// vacio.
void FilaDebug(const char* nombre, const std::string& valor) {
  const char* texto = valor.empty() ? "—" : valor.c_str();
  ImGui::Text("  %-26s %s", nombre, texto);
}

std::string JuntarLista(const std::vector<std::string>& items) {
  std::string lista;
  for (const auto& item : items) {
    if (!lista.empty()) lista += ", ";
    lista += item;
  }
  return lista;
}

// Combo con opciones fijas. Si el valor actual no esta en la lista (lo puso la
// linea de comandos, o el toml), muestra "texto_si_otro" o el valor crudo.
void ComboSimple(const char* etiqueta, const std::string& actual, const Opcion* opciones, int cuenta,
                 const char* texto_si_otro, const std::function<void(const char*)>& al_cambiar) {
  int idx = 0;
  for (int i = 0; i < cuenta; ++i) {
    if (opciones[i].valor == actual) {
      idx = i;
      break;
    }
  }
  const char* mostrar = opciones[idx].etiqueta;
  if (actual != opciones[idx].valor) {
    mostrar = (texto_si_otro && *texto_si_otro) ? texto_si_otro : actual.c_str();
  }
  ImGui::PushID(etiqueta);
  ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f),
                                      ImVec2(FLT_MAX, ImGui::GetFrameHeightWithSpacing() * 7.0f));
  if (ImGui::BeginCombo("##combo", mostrar)) {
    for (int i = 0; i < cuenta; ++i) {
      const bool sel = (i == idx);
      if (ImGui::Selectable(opciones[i].etiqueta, sel)) {
        al_cambiar(opciones[i].valor);
      }
      if (sel) {
        ImGui::SetItemDefaultFocus();
      }
    }
    ImGui::EndCombo();
  }
  ImGui::PopID();
  ImGui::SameLine();
  ImGui::TextUnformatted(etiqueta);
}

}  // namespace

// Controller --------------------------------------------------------------
// The SDK's ImGui gets no controller input: the menu feeds it itself, from
// the states the game polls (the game keeps polling while the menu is open;
// it gets neutral states meanwhile).
namespace {
constexpr uint16_t kPadUp = 0x0001, kPadDown = 0x0002, kPadLeft = 0x0004, kPadRight = 0x0008;
constexpr uint16_t kPadStart = 0x0010, kPadBack = 0x0020, kPadLb = 0x0100, kPadRb = 0x0200;
constexpr uint16_t kPadA = 0x1000, kPadB = 0x2000, kPadX = 0x4000, kPadY = 0x8000;
std::atomic<uint32_t> g_pad_buttons{0};
std::atomic<uint32_t> g_pad_left_stick{0};  // x | y << 16
std::atomic<bool> g_menu_open{false};
std::atomic<bool> g_pad_hold{false};  // neutral until every button is released
std::mutex g_pad_toggle_mutex;
std::function<void()> g_pad_toggle;

void FeedPad(ImGuiIO& io) {
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
  io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
  const uint32_t b = g_pad_buttons.load();
  const struct {
    ImGuiKey key;
    uint16_t mask;
  } keys[] = {{ImGuiKey_GamepadDpadUp, kPadUp},     {ImGuiKey_GamepadDpadDown, kPadDown},
              {ImGuiKey_GamepadDpadLeft, kPadLeft}, {ImGuiKey_GamepadDpadRight, kPadRight},
              {ImGuiKey_GamepadStart, kPadStart},   {ImGuiKey_GamepadBack, kPadBack},
              {ImGuiKey_GamepadL1, kPadLb},         {ImGuiKey_GamepadR1, kPadRb},
              {ImGuiKey_GamepadFaceDown, kPadA},    {ImGuiKey_GamepadFaceRight, kPadB},
              {ImGuiKey_GamepadFaceLeft, kPadX},    {ImGuiKey_GamepadFaceUp, kPadY}};
  for (const auto& k : keys) {
    io.AddKeyEvent(k.key, (b & k.mask) != 0);
  }
  const uint32_t stick = g_pad_left_stick.load();
  const auto axis = [](int16_t v) {
    constexpr float kDeadZone = 7849.0f;
    const float f = std::fabs(float(v));
    return f < kDeadZone ? 0.0f : std::min((f - kDeadZone) / (32767.0f - kDeadZone), 1.0f);
  };
  const int16_t x = int16_t(stick), y = int16_t(stick >> 16);
  io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, x < 0 && axis(x) > 0, x < 0 ? axis(x) : 0);
  io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, x > 0 && axis(x) > 0, x > 0 ? axis(x) : 0);
  io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, y > 0 && axis(y) > 0, y > 0 ? axis(y) : 0);
  io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, y < 0 && axis(y) > 0, y < 0 ? axis(y) : 0);
}
}  // namespace

bool NfsmwMenuPadFilter(uint16_t buttons, uint8_t left_trigger, uint8_t right_trigger,
                        int16_t left_x, int16_t left_y) {
  g_pad_buttons.store(buttons);
  g_pad_left_stick.store(uint32_t(uint16_t(left_x)) | (uint32_t(uint16_t(left_y)) << 16));
  static bool combo_before = false;
  const bool combo = (buttons & (kPadStart | kPadBack)) == (kPadStart | kPadBack);
  if (combo && !combo_before) {
    g_pad_hold.store(true);
    std::lock_guard<std::mutex> lock(g_pad_toggle_mutex);
    if (g_pad_toggle) {
      g_pad_toggle();
    }
  }
  combo_before = combo;
  if (g_pad_hold.load() && buttons == 0 && left_trigger < 30 && right_trigger < 30) {
    g_pad_hold.store(false);
  }
  return g_menu_open.load() || g_pad_hold.load();
}

void NfsmwMenuSetPadToggle(std::function<void()> toggle) {
  std::lock_guard<std::mutex> lock(g_pad_toggle_mutex);
  g_pad_toggle = std::move(toggle);
}

NfsmwMenuDialog::NfsmwMenuDialog(rex::ui::ImGuiDrawer* drawer, Callbacks callbacks)
    : rex::ui::ImGuiDialog(drawer), callbacks_(std::move(callbacks)) {
  g_menu_open.store(true);
}

NfsmwMenuDialog::~NfsmwMenuDialog() = default;

void NfsmwMenuDialog::RequestClose() { Close(); }

void NfsmwMenuDialog::Persistir() {
  if (callbacks_.persist_config) {
    callbacks_.persist_config();
  }
}

void NfsmwMenuDialog::OnClose() {
  // The button that closed it does not reach the game.
  g_pad_hold.store(true);
  g_menu_open.store(false);
  ImGuiIO& io = GetIO();
  io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
  io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
  if (callbacks_.on_closed) {
    callbacks_.on_closed();
  }
  if (quit_requested_ && callbacks_.request_quit) {
    callbacks_.request_quit();
  }
}

void NfsmwMenuDialog::OnDraw(ImGuiIO& io) {
  FeedPad(io);
  // Shift + arrows are the D-pad of the keyboard controller (MnK): navigation.
  // (The SDK sets no ImGui modifier, so Shift is asked from Windows.)
#if defined(_WIN32)
  const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
#else
  const bool shift = false;
#endif
  if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && !shift) {
    selected_tab_ = (selected_tab_ + kNumPestanas - 1) % kNumPestanas;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && !shift) {
    selected_tab_ = (selected_tab_ + 1) % kNumPestanas;
  }
  // Controller: LB / RB switch section, B (nothing being edited, no list open)
  // or Start closes; Back + Start is the app's toggle.
  if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) {
    selected_tab_ = (selected_tab_ + kNumPestanas - 1) % kNumPestanas;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) {
    selected_tab_ = (selected_tab_ + 1) % kNumPestanas;
  }
  if (!first_draw_ && !ImGui::IsAnyItemActive() &&
      !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
      (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
       (ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false) &&
        !ImGui::IsKeyDown(ImGuiKey_GamepadBack)))) {
    Close();
    return;
  }

  const ImVec2 pantalla = io.DisplaySize;
  const float ancho = std::floor(std::min(pantalla.x * 0.90f, 800.0f));
  const float alto = std::floor(std::min(pantalla.y * 0.88f, 600.0f));
  const ImVec2 origen((pantalla.x - ancho) * 0.5f, (pantalla.y - alto) * 0.5f);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  const ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus;
  if (first_draw_) {
    // Focused, so the controller navigates it right away.
    ImGui::SetNextWindowFocus();
    first_draw_ = false;
  }
  ImGui::SetNextWindowPos(origen);
  ImGui::SetNextWindowSize(ImVec2(ancho, alto));
  if (!ImGui::Begin("##nfsmw_menu", nullptr, flags)) {
    ImGui::End();
    ImGui::PopStyleVar(2);
    return;
  }

  const float pad = 18.0f;
  const float borde = 2.0f;
  const float cabecera = 56.0f;
  const float pie = 30.0f;
  const float carril = 130.0f;

  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 x0 = origen;
  const ImVec2 x1(origen.x + ancho, origen.y + alto);

  // Fondo y marco.
  dl->AddRectFilled(x0, x1, kBg, 8.0f);
  dl->AddRect(x0, x1, kBorde, 8.0f, 0, borde);
  dl->AddRectFilled(ImVec2(x0.x + borde, x0.y + borde),
                    ImVec2(x1.x - borde, x0.y + cabecera), IM_COL32(26, 30, 37, 255));

  // Cabecera.
  dl->AddText(ImGui::GetFont(), 26.0f,
              ImVec2(x0.x + pad, x0.y + pad - 6.0f), kTexto, "SETTINGS");
  dl->AddText(ImGui::GetFont(), 26.0f,
              ImVec2(x1.x - pad - std::min(ancho * 0.42f, 360.0f), x0.y + pad - 6.0f), kAcento,
              "NFS MOST WANTED");
  dl->AddLine(ImVec2(x0.x + pad, x0.y + cabecera), ImVec2(x1.x - pad, x0.y + cabecera), kMarco,
              borde);

  // Pie: accesos rapidos, por si los botones de la pestana SISTEMA no se ven.
  const float y_pie = x1.y - pie;
  dl->AddLine(ImVec2(x0.x + pad, y_pie), ImVec2(x1.x - pad, y_pie), kMarco, borde);
  dl->AddText(ImGui::GetFont(), 14.0f, ImVec2(x0.x + pad, y_pie + 6.0f), kTextoAtenuado,
              "Up / Down or LB / RB: section   |   ESC, B or Back + Start closes");

  // Carril de pestanas a la izquierda.
  const float y_carril = x0.y + cabecera;
  for (int i = 0; i < kNumPestanas; ++i) {
    const float y_i = y_carril + static_cast<float>(i) * 46.0f;
    const bool sel = (i == selected_tab_);
    if (sel) {
      dl->AddRectFilled(ImVec2(x0.x + borde, y_i), ImVec2(x0.x + carril, y_i + 40.0f), kTabSel);
      dl->AddRectFilled(ImVec2(x0.x + borde, y_i), ImVec2(x0.x + 6.0f, y_i + 40.0f), kAcento);
    }
    dl->AddText(ImGui::GetFont(), 20.0f,
                ImVec2(x0.x + 16.0f, y_i + 8.0f), sel ? kTexto : kTextoAtenuado,
                kTitulosPestana[i]);
    ImGui::SetCursorScreenPos(ImVec2(x0.x + borde, y_i));
    // Not for the controller's navigation (LB / RB switch section).
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    if (ImGui::InvisibleButton(
            (std::string("##pestana") + std::to_string(i)).c_str(),
            ImVec2(carril - borde, 40.0f))) {
      selected_tab_ = i;
    }
    ImGui::PopItemFlag();
  }

  // Contenido, a la derecha del carril.
  ImGui::SetCursorScreenPos(ImVec2(x0.x + carril + pad, y_carril + 8.0f));
  const ImVec2 tam_contenido(ancho - carril - 2.0f * pad, alto - cabecera - pie - 16.0f);
  ImGui::BeginChild("##nfsmw_contenido", tam_contenido, ImGuiChildFlags_NavFlattened,
                    ImGuiWindowFlags_NoBackground);

  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 6.0f));
  ImGui::PushStyleColor(ImGuiCol_FrameBg, kFondoWidget);
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(48, 56, 66, 255));
  ImGui::PushStyleColor(ImGuiCol_Text, kTexto);
  ImGui::PushStyleColor(ImGuiCol_TextDisabled, kTextoAtenuado);
  ImGui::PushStyleColor(ImGuiCol_CheckMark, kAcento);
  ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(48, 56, 66, 255));
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(58, 66, 78, 255));
  ImGui::PushStyleColor(ImGuiCol_SliderGrab, kAcento);
  ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, kAcento);
  // Long explanations wrap at the edge of the content instead of being cut.
  ImGui::PushTextWrapPos(0.0f);

  if (selected_tab_ == 0) {
    ImGui::TextUnformatted("DISPLAY");
    ImGui::Spacing();

    // Pantalla completa: en vivo (callback de ReXApp::SetupPresentation).
    bool completo = CvarB("fullscreen");
    if (ImGui::Checkbox("Fullscreen", &completo)) {
      SetCvarB("fullscreen", completo);
      Persistir();
    }
    MarcaVivo("applies instantly");

    // V-Sync: el parche del presentador lo lee en cada fotograma.
    bool vsync = CvarB("vsync");
    if (ImGui::Checkbox("V-Sync", &vsync)) {
      SetCvarB("vsync", vsync);
      Persistir();
    }
    MarcaVivo("applies instantly");

    // Frame rate: the pacer (frame_pacing_fps), live. The old presenter limiter
    // (max_fps) would be a second clock fighting it.
    if (ExisteCvar("frame_pacing_fps")) {
      static const Opcion kFps[] = {
          {"30 fps (original)", "30"}, {"60 fps", "60"},   {"120 fps", "120"},
          {"144 fps", "144"},          {"165 fps", "165"}, {"Unlimited", "0"}};
      ComboSimple("Frame rate", CvarS("frame_pacing_fps"), kFps, 6, nullptr,
                  [this](const char* v) {
                    SetCvarS("frame_pacing_fps", v);
                    Persistir();
                  });
      MarcaVivo("applies instantly");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextUnformatted("RESOLUTION");
    ImGui::Spacing();

    static const Opcion kRes[] = {
        {"720p", "720p"}, {"900p", "900p"}, {"1080p", "1080p"},
        {"1440p", "1440p"}, {"1800p", "1800p"}, {"4K (2160p)", "4k"}};
    ComboSimple("Window resolution and video mode", CvarS("resolution"), kRes, 6,
                "Custom", [this](const char* v) {
                  SetCvarS("resolution", v);
                  Persistir();
                });
    MarcaReinicio();

    // The native renderer (the launcher's default) has its own scale; the
    // emulation's options below only apply without it.
    const bool nativo = ExisteCvar("native_renderer") && CvarB("native_renderer");
    if (nativo && ExisteCvar("native_renderer_scale")) {
      static const Opcion kEscala[] = {
          {"1x - 720p (native)", "1"},
          {"2x - 1440p (4x the pixels)", "2"},
          {"3x - 2160p 4K (9x the pixels)", "3"},
          {"4x - 2880p (16x the pixels)", "4"}};
      ComboSimple("Render scale (supersampling)", CvarS("native_renderer_scale"), kEscala, 4,
                  nullptr, [this](const char* v) {
                    SetCvarS("native_renderer_scale", v);
                    Persistir();
                  });
      MarcaReinicioConAviso(
          "The native renderer draws every frame at this multiple of 720p and scales it to "
          "the window: smoother edges, sharper textures.");
    } else {
    static const Opcion kIRes[] = {
        {"1x - 720p (native)", "1"},
        {"2x - 1440p (4x the pixels)", "2"},
        {"3x - 2160p 4K (9x the pixels)", "3"},
        {"4x - 2880p (16x the pixels)", "4"}};
    static const char kEtiquetaIRes[] = "Internal resolution (real supersampling by the engine)";
    const std::string escala = CvarS("resolution_scale");
    ComboSimple(kEtiquetaIRes, escala, kIRes, 4, nullptr, [this](const char* v) {
      SetCvarS("resolution_scale", v);
      Persistir();
    });
    MarcaReinicioConAviso(
        "Integer scale of the game's render targets: more pixels every frame, not a "
        "stretch. On a modern GPU 2x or 3x cost next to nothing.");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextUnformatted("IMAGE QUALITY");
    ImGui::Spacing();

    if (nativo && ExisteCvar("native_renderer_anisotropic")) {
      static const Opcion kAnisoNativo[] = {{"The game's", "-1"}, {"Off (bilinear)", "0"},
                                            {"2x", "2"},          {"4x", "3"},
                                            {"8x", "4"},          {"16x", "5"}};
      ComboSimple("Anisotropic filtering", CvarS("native_renderer_anisotropic"), kAnisoNativo, 6,
                  nullptr, [this](const char* v) {
                    SetCvarS("native_renderer_anisotropic", v);
                    Persistir();
                  });
      MarcaReinicio();
    } else if (ExisteCvar("anisotropic_override")) {
      static const Opcion kAniso[] = {{"Off (bilinear)", "0"}, {"1x", "1"}, {"2x", "2"},
                                      {"4x", "3"}, {"8x", "4"}, {"16x", "5"}};
      ComboSimple("Anisotropic filtering", CvarS("anisotropic_override"), kAniso, 6, nullptr,
                  [this](const char* v) {
                    SetCvarS("anisotropic_override", v);
                    Persistir();
                  });
      MarcaVivo("applies instantly");
    }

    if (nativo && ExisteCvar("native_renderer_msaa")) {
      static const Opcion kMsaa[] = {
          {"4x (the game's)", "-1"}, {"Off", "0"}, {"2x", "1"}, {"8x", "3"}};
      ComboSimple("MSAA", CvarS("native_renderer_msaa"), kMsaa, 4, nullptr,
                  [this](const char* v) {
                    SetCvarS("native_renderer_msaa", v);
                    Persistir();
                  });
      MarcaReinicioConAviso(
          "Samples of the targets the game draws with multisampling (its own: 4). On top of "
          "the render scale.");
    }

    if (ExisteCvar("post_processing")) {
      bool post = CvarB("post_processing");
      if (ImGui::Checkbox("Post-processing (visual treatment)", &post)) {
        SetCvarB("post_processing", post);
        Persistir();
      }
      MarcaVivo("applies instantly");
      ImGui::TextColored(ImColor(kTextoAtenuado),
                         "The game's colour grading (the green-yellow tint), bloom, vignette and "
                         "motion blur. Off: the plain picture.");
    }

    if (!nativo && ExisteCvar("swap_post_effect")) {
      static const Opcion kAA[] = {{"Off", "none"}, {"FXAA", "fxaa"}, {"FXAA Extreme", "fxaa_extreme"}};
      ComboSimple("Anti-aliasing", CvarS("swap_post_effect"), kAA, 3, nullptr, [this](const char* v) {
        SetCvarS("swap_post_effect", v);
        Persistir();
      });
      MarcaReinicio();
    }

    if (!nativo && ExisteCvar("gpu_backend")) {
      static const Opcion kApi[] = {{"Direct3D 12", "d3d12"}, {"Vulkan", "vulkan"}};
      ComboSimple("Graphics API", CvarS("gpu_backend"), kApi, 2, nullptr, [this](const char* v) {
        SetCvarS("gpu_backend", v);
        Persistir();
      });
      MarcaReinicio();
    }

    const auto pendientes = rex::cvar::GetPendingRestartFlags();
    const bool hay_pendientes = !pendientes.empty();
    if (hay_pendientes) {
      ImGui::Spacing();
      ImGui::Separator();
      ImGui::Spacing();
      ImGui::TextColored(ImColor(kAviso),
                         "Changes in this section apply after a restart");
      if (BotonAplicar()) {
        Persistir();
        if (callbacks_.request_restart) {
          callbacks_.request_restart();
        }
      }
    }

  } else if (selected_tab_ == 1) {
    ImGui::TextUnformatted("CONTENT");
    ImGui::Spacing();

    bool black = CvarB("black_edition");
    if (ImGui::Checkbox("Black Edition content", &black)) {
      SetCvarB("black_edition", black);
      AplicarBlackEdition(black);
      Persistir();
    }
    MarcaVivo("(applies instantly)");
    ImGui::TextColored(ImColor(kTextoAtenuado),
                       "The Black Edition cars appear as downloadable content in the car "
                       "lot.");
    ImGui::Spacing();

    bool todo = CvarB("unlock_all");
    if (ImGui::Checkbox("Unlock everything", &todo)) {
      SetCvarB("unlock_all", todo);
      AplicarUnlockAll(todo);
      Persistir();
    }
    MarcaVivo("(applies instantly)");
    ImGui::TextColored(ImColor(kTextoAtenuado),
                       "The game's own UnlockAllThings debug flag. It does not touch the save: "
                       "switch it off and the normal progress is back.");
    ImGui::Spacing();

    if (ExisteCvar("freecam")) {
      bool libre = CvarB("freecam");
      if (ImGui::Checkbox("Free camera (F6)", &libre)) {
        SetCvarB("freecam", libre);
      }
      MarcaVivo("(applies instantly)");
      ImGui::TextColored(ImColor(kTextoAtenuado),
                         "The game's debug world camera. Move: WASD / left stick. Look: arrow "
                         "keys / right stick. Up and down: E and Q / triggers. Faster: Space or "
                         "Backspace / A or B. Zoom: 1 and 3 / LB and RB, K / right stick click "
                         "resets it. The car gets no input meanwhile.");
      ImGui::Spacing();
      if (ExisteCvar("freecam_fov")) {
        float fov = CvarF("freecam_fov");
        if (ImGui::SliderFloat("Free camera field of view", &fov, 10.0f, 150.0f, "%.0f deg")) {
          SetCvarF("freecam_fov", std::clamp(fov, 10.0f, 150.0f));
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
          Persistir();
        }
        MarcaVivo("(applies instantly; the game's: 71.5)");
        ImGui::Spacing();
      }
      if (ExisteCvar("freecam_photo_mode")) {
        bool foto = CvarB("freecam_photo_mode");
        if (ImGui::Checkbox("Photo mode: world paused (F8)", &foto)) {
          SetCvarB("freecam_photo_mode", foto);
          if (foto) {
            SetCvarB("freecam", true);
          }
        }
        MarcaVivo("(applies instantly)");
        ImGui::TextColored(ImColor(kTextoAtenuado),
                           "Traffic, physics and effects stand still while the free camera "
                           "moves; F8 again lets the world run on.");
        ImGui::Spacing();
      }
    }

    if (ExisteCvar("grant_user_privileges")) {
      bool gp = CvarB("grant_user_privileges");
      if (ImGui::Checkbox("User privileges (online access)", &gp)) {
        SetCvarB("grant_user_privileges", gp);
        Persistir();
      }
      MarcaReinicio();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextUnformatted("XBOX LIVE (SIMULATED)");
    ImGui::Spacing();

    if (!gamertag_sync_) {
      const auto actual = CvarS("user_profile_name");
      std::memset(gamertag_, 0, sizeof(gamertag_));
      const size_t copia = std::min<size_t>(actual.size(), sizeof(gamertag_) - 1);
      std::copy(actual.begin(), actual.begin() + copia, gamertag_);
      gamertag_sync_ = true;
    }
    ImGui::SetNextItemWidth(340.0f);
    if (ImGui::InputText("Profile gamertag", gamertag_, sizeof(gamertag_))) {
      rex::cvar::SetFlagByName("user_profile_name", gamertag_);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      Persistir();
    }
    MarcaVivo("live: the game reads it every time it asks for the profile");

    ImGui::TextColored(
        ImColor(kTextoAtenuado),
        "Simulated session: user 0 signed in, Gold membership, XUID 0x00B13EBABEBABEBE, "
        "local and online profile.");
    ImGui::TextColored(
        ImColor(kTextoAtenuado),
        "When the game asks for text (a name, a new profile), the recomp opens its "
        "own on-screen keyboard: type and press OK. The Xbox guide always counts as "
        "closed, so no screen is left waiting for it.");
    ImGui::TextColored(ImColor(kTextoAtenuado),
                       "Online multiplayer: EA's servers are gone; the only way left is "
                       "System Link, which is not implemented yet.");
    ImGui::Spacing();

    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextUnformatted("GAME");
    ImGui::Spacing();

    if (ExisteCvar("game_speed")) {
      float velocidad = CvarF("game_speed");
      if (ImGui::SliderFloat("Game speed", &velocidad, 20.0f, 200.0f, "%.0f%%")) {
        velocidad = std::clamp(velocidad, 20.0f, 200.0f);
        SetCvarF("game_speed", velocidad);
      }
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        Persistir();
      }
      MarcaVivo("live: also with the menu closed, until changed again or the game is closed");
    }

    if (ExisteCvar("fov_scale")) {
      float escala = CvarF("fov_scale") * 100.0f;
      if (ImGui::SliderFloat("Field of view (driving)", &escala, 50.0f, 160.0f, "%.0f%%")) {
        SetCvarF("fov_scale", std::clamp(escala, 50.0f, 160.0f) / 100.0f);
      }
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        Persistir();
      }
      MarcaVivo("(applies instantly)");
      ImGui::TextColored(ImColor(kTextoAtenuado),
                         "Times the game's own: 100%% is 78 degrees at rest, 130%% about 101. It "
                         "still widens with speed as before. Menus and cutscenes keep theirs.");
      ImGui::Spacing();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(ImColor(kTextoAtenuado),
                       "Tip: the recomp's settings (F3 and F4) have every technical option; this "
                       "menu is the everyday summary.");

  } else if (selected_tab_ == 2) {
    ImGui::TextUnformatted("APPLICATION");
    ImGui::Spacing();

    const auto pendientes = rex::cvar::GetPendingRestartFlags();
    if (pendientes.empty()) {
      ImGui::TextColored(ImColor(kVivo), "No changes waiting for a restart.");
    } else {
      ImGui::TextColored(ImColor(kAviso), "Changes waiting for a restart:");
      ImGui::TextWrapped("%s", JuntarLista(pendientes).c_str());
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("RESUME", ImVec2(240.0f, 0.0f))) {
      Close();
    }
    if (ImGui::Button("SAVE SETTINGS", ImVec2(240.0f, 0.0f))) {
      Persistir();
    }
    if (ImGui::Button("RESTORE DEFAULTS", ImVec2(240.0f, 0.0f))) {
      rex::cvar::ResetAllToDefaults();
      Persistir();
    }
    if (BotonAplicar()) {
      Persistir();
      if (callbacks_.request_restart) {
        callbacks_.request_restart();
      }
    }
    if (ImGui::Button("QUIT TO DESKTOP", ImVec2(240.0f, 0.0f))) {
      quit_requested_ = true;
      Persistir();
      Close();
    }
  } else {
    // DEBUG -----------------------------------------------------------------
    ImGui::TextUnformatted("PERFORMANCE");
    ImGui::Spacing();

    if (callbacks_.sample_fps) {
      const auto stats = callbacks_.sample_fps();
      ImGui::Text("  Game fps: %.1f", stats.fps);
      ImGui::Text("  Frame time: %.2f ms", stats.frame_time_ms);
      ImGui::TextColored(
          ImColor(kTextoAtenuado),
          "The meter runs while this menu is open: give it a moment.");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextUnformatted("GRAPHICS");
    ImGui::Spacing();
    FilaDebug("Graphics backend", CvarS("gpu_backend"));
    FilaDebug("GPU plugin", CvarS("gpu_plugin"));
    FilaDebug("EDRAM path", CvarS("render_target_path_d3d12"));
    FilaDebug("Anti-aliasing", CvarS("swap_post_effect"));
    FilaDebug("Resolve readback", CvarS("readback_resolve"));

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextUnformatted("VIDEO");
    ImGui::Spacing();
    FilaDebug("Video mode", CvarS("video_mode_width") + "x" + CvarS("video_mode_height"));
    FilaDebug("Internal scale", CvarS("resolution_scale"));
    FilaDebug("Window", CvarS("window_width") + "x" + CvarS("window_height"));
    FilaDebug("Fullscreen", CvarS("fullscreen"));
    FilaDebug("Anisotropic", CvarS("anisotropic_override"));

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextUnformatted("SYSTEM");
    ImGui::Spacing();
    FilaDebug("V-Sync", CvarS("vsync"));
    if (ExisteCvar("frame_pacing_fps")) {
      FilaDebug("Frame rate (0 = unlimited)", CvarS("frame_pacing_fps"));
    }
    if (ExisteCvar("game_speed")) {
      FilaDebug("Game speed", CvarS("game_speed"));
    }
    FilaDebug("Gamertag", CvarS("user_profile_name"));
    FilaDebug("Black Edition", CvarS("black_edition"));
    FilaDebug("Unlock everything", CvarS("unlock_all"));
    if (ExisteCvar("grant_user_privileges")) {
      FilaDebug("Online privileges", CvarS("grant_user_privileges"));
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const auto pendientes = rex::cvar::GetPendingRestartFlags();
    if (pendientes.empty()) {
      ImGui::TextColored(ImColor(kVivo), "No changes waiting for a restart.");
    } else {
      ImGui::TextColored(ImColor(kAviso), "Restart needed for:");
      ImGui::TextWrapped("%s", JuntarLista(pendientes).c_str());
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(
        ImColor(kTextoAtenuado),
        "F3 opens the runtime debug panel and F4 every technical setting (cvars). "
        "This menu is the everyday summary.");
  }

  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor(9);
  ImGui::PopStyleVar(2);
  ImGui::EndChild();

  ImGui::End();
  ImGui::PopStyleVar(2);
}

// ---------------------------------------------------------------------------
//  Helpers de dibujo (deben verse llamados dentro de OnDraw).
// ---------------------------------------------------------------------------
void NfsmwMenuDialog::MarcaVivo(const char* texto) {
  ImGui::TextColored(ImColor(kVivo), texto);
  ImGui::Spacing();
}

void NfsmwMenuDialog::MarcaReinicio(const char* texto) {
  ImGui::TextColored(ImColor(kAviso), (texto && *texto) ? texto : "(applies after a restart)");
  ImGui::Spacing();
}

void NfsmwMenuDialog::MarcaReinicioConAviso(const char* texto) {
  ImGui::TextColored(ImColor(kAviso), "Restart for this to take effect.");
  ImGui::TextColored(ImColor(kTextoAtenuado), texto);
  ImGui::Spacing();
}

bool NfsmwMenuDialog::BotonAplicar() {
  return ImGui::Button("APPLY AND RESTART", ImVec2(240.0f, 0.0f));
}