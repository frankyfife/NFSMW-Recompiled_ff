// nfsmw - menu de ajustes ingame (estilo GoldenEye, abierto con ESC)
//
// Se apoya en el dialogo de ImGui del SDK: al construirse se registra solo
// (ImGuiDrawer::AddDialog) y al llamar a Close() se cierra y se borra solo
// (ImGuiDialog::Draw). La app no necesita poseerlo: guarda un puntero puro que
// se anula con on_closed.

#pragma once

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/overlay/debug_overlay.h>

#include <cstdint>
#include <functional>

// Escribe la bandera Black Edition (byte 0x82A2CE06) en la memoria del guest.
// Vale tanto al cargar el XEX como en vivo desde el menu.
bool AplicarBlackEdition(bool activo);
// Igual con UnlockAllThings (byte 0x82A2CE00), ver nfsmw_menu.cpp.
bool AplicarUnlockAll(bool activo);

// Controller: every state of user 0 passes through here (from
// NfsmwInputFilter, src/freecam.cpp, on the thread that polls the input).
// Back + Start opens and closes the menu (through the toggle the app sets);
// returns true while the game must get a neutral state (menu open, or the
// buttons that opened or closed it still held).
bool NfsmwMenuPadFilter(uint16_t buttons, uint8_t left_trigger, uint8_t right_trigger,
                        int16_t left_x, int16_t left_y);
void NfsmwMenuSetPadToggle(std::function<void()> toggle);

class NfsmwMenuDialog : public rex::ui::ImGuiDialog {
 public:
  struct Callbacks {
    std::function<void()> persist_config;      // SaveConfig en nfsmw.toml
    std::function<void()> request_restart;     // guardar + relanzar el .exe
    std::function<void()> request_quit;        // cerrar la ventana
    std::function<void()> on_closed;           // avisar a la app (anula su puntero)
    std::function<rex::ui::FrameStats()> sample_fps;  // medidor del overlay F3
  };

  NfsmwMenuDialog(rex::ui::ImGuiDrawer* drawer, Callbacks callbacks);
  ~NfsmwMenuDialog() override;

  void RequestClose();

 protected:
  void OnDraw(ImGuiIO& io) override;
  void OnClose() override;

 private:
  void Persistir();
  static void MarcaVivo(const char* texto);
  static void MarcaReinicio(const char* texto = nullptr);
  static void MarcaReinicioConAviso(const char* texto);
  static bool BotonAplicar();

  Callbacks callbacks_;
  int selected_tab_ = 0;
  bool quit_requested_ = false;
  bool first_draw_ = true;
  // A list open or an item being edited at the end of the previous frame:
  // ImGui takes B for those inside NewFrame, before OnDraw, so by the time
  // OnDraw looks the list is already closed.
  bool busy_before_ = false;
  bool start_alone_ = false;  // Start went down without Back: closes on release

  char gamertag_[16];  // 15 caracteres + nulo, como un gamertag de Xbox Live
  bool gamertag_sync_ = false;
};