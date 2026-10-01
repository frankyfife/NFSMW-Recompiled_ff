// Free camera: the game's own debug world camera (DebugWorldCameraMover,
// camera action "CDActionDebug"), switched on for the player's view.
//
// The camera director (sub_82167640, one per view; view index at +8) decides
// every frame which camera action a view runs: it writes the wanted action's
// key (16 bytes: 64-bit hash, 32-bit hash, name pointer) to +16, and when the
// running action (+32) has another key it creates the wanted one through the
// action factory (sub_821832B0), deletes the old one (sub_82167330) and tells
// the view about the change (sub_821E4130). With the free camera on, the
// director of the player's view is switched to the debug action the same
// way and then left alone (it would switch straight back to the drive
// camera); switched off, its wanted action is the drive camera again and it
// runs as usual.
//
// Control: the debug camera moves on debug actions (ids 53-73) no button
// sends in the retail game. Its input step (sub_821751E0) turns them into
// fields of the mover; with the free camera on those fields come from the
// player's controller instead (keyboard through the MnK bindings: WASD move,
// arrows look, E/Q up/down, Space faster), which the game itself does not get
// meanwhile (the SDK's XamInputGetState passes every state through
// NfsmwInputFilter).

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

REXCVAR_DEFINE_BOOL(freecam, false, "NFSMW",
                    "Free camera (the game's debug world camera) for the player's view");

extern "C" REX_FUNC(__imp__sub_82167640);  // camera director update
extern "C" REX_FUNC(sub_8216E550);         // key of "CDActionDebug" (static)
extern "C" REX_FUNC(sub_82144CA0);         // (key* out, const char* name)
extern "C" REX_FUNC(sub_821832B0);         // (uint32_t* hash, director*) -> new action
extern "C" REX_FUNC(sub_82167330);         // (director*): deletes the running action
extern "C" REX_FUNC(sub_821E4130);         // (key*, view): the view's action changed
extern "C" REX_FUNC(__imp__sub_821751E0);  // debug world camera: its actions to fields

namespace {

constexpr uint32_t kDirectorView = 8;
constexpr uint32_t kDirectorWantedKey = 16;  // 16 bytes
constexpr uint32_t kDirectorAction = 32;
constexpr uint32_t kDriveActionName = 0x8206BA84;  // "CDActionDrive"

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}
void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, 4);
}

std::mutex g_mutex;
// Directors switched to the debug action, and the action they got.
std::unordered_map<uint32_t, uint32_t> g_debug_actions;
// The view the free camera is for: the first director seen (the player's).
uint32_t g_player_view = UINT32_MAX;

// Room on the guest stack for the calls' arguments: above the moved stack
// pointer (callees save registers below theirs).
struct GuestFrame {
  PPCContext& ctx;
  uint32_t saved;
  explicit GuestFrame(PPCContext& c) : ctx(c), saved(c.r1.u32) { ctx.r1.u32 -= 160; }
  ~GuestFrame() { ctx.r1.u32 = saved; }
  uint32_t scratch(uint32_t offset) const { return ctx.r1.u32 + 96 + offset; }
};

// Makes `key` the director's wanted action and switches to it like the
// director does. Returns the new action (0 if the factory gave none).
uint32_t SwitchAction(PPCContext& ctx, uint8_t* base, uint32_t director, uint32_t key) {
  GuestFrame frame(ctx);
  std::memmove(base + director + kDirectorWantedKey, base + key, 16);
  const uint32_t hash = frame.scratch(0);
  Store32(base, hash, Load32(base, director + kDirectorWantedKey + 8));
  ctx.r3.u32 = hash;
  ctx.r4.u32 = director;
  sub_821832B0(ctx, base);
  const uint32_t action = ctx.r3.u32;
  if (!action) {
    return 0;
  }
  Store32(base, director + 704, 0);  // 0.0f, as the director does
  base[director + 700] = 0;
  ctx.r3.u32 = director;
  sub_82167330(ctx, base);
  Store32(base, director + kDirectorAction, action);
  const uint32_t key_copy = frame.scratch(16);
  std::memmove(base + key_copy, base + director + kDirectorWantedKey, 16);
  ctx.r3.u32 = key_copy;
  ctx.r4.u32 = Load32(base, director + kDirectorView);
  sub_821E4130(ctx, base);
  return action;
}

}  // namespace

extern "C" REX_FUNC(sub_82167640) {
  const uint32_t director = ctx.r3.u32;
  const uint32_t view = Load32(base, director + kDirectorView);
  std::unique_lock<std::mutex> lock(g_mutex);
  if (g_player_view == UINT32_MAX) {
    g_player_view = view;
    REXLOG_INFO("[freecam] player camera director {:08X}, view {}", director, view);
  }
  const bool want = REXCVAR_GET(freecam) && view == g_player_view;
  auto it = g_debug_actions.find(director);
  if (want) {
    if (it == g_debug_actions.end() || it->second != Load32(base, director + kDirectorAction)) {
      sub_8216E550(ctx, base);
      const uint32_t key = ctx.r3.u32;
      const uint32_t action = SwitchAction(ctx, base, director, key);
      REXLOG_INFO("[freecam] debug world camera on (action {:08X})", action);
      if (action) {
        g_debug_actions[director] = action;
      }
    }
    // Left alone while the free camera is on.
    return;
  }
  if (it != g_debug_actions.end()) {
    g_debug_actions.erase(it);
    // Wanted: the drive camera again; the director switches to it below.
    GuestFrame frame(ctx);
    const uint32_t key = frame.scratch(0);
    ctx.r3.u32 = key;
    ctx.r4.u32 = kDriveActionName;
    sub_82144CA0(ctx, base);
    std::memmove(base + director + kDirectorWantedKey, base + key, 16);
    REXLOG_INFO("[freecam] debug world camera off");
    ctx.r3.u32 = director;
  }
  lock.unlock();
  __imp__sub_82167640(ctx, base);
}

namespace {

// The latest controller state of user 0 (buttons, LT, RT, LX, LY, RX, RY).
std::atomic<uint64_t> g_pad_buttons_triggers{0};
std::atomic<uint64_t> g_pad_sticks{0};

// Mover fields its debug actions set (sub_821751E0) and turbo flags.
// Measured in free roam (world z is up): +144 moves the camera up, +152
// along the view, +156 level to the right.
constexpr uint32_t kMoverRise = 144;      // action value * 10
constexpr uint32_t kMoverForward = 152;   // * 20
constexpr uint32_t kMoverSideways = 156;  // * 20
constexpr uint32_t kMoverYawRate = 160;   // int16, * 20000
constexpr uint32_t kMoverPitchRate = 162;  // int16, * 20000
constexpr uint32_t kTurbo = 0x82A2BFBC, kSuperTurbo = 0x82A2BFC0;
constexpr uint16_t kButtonA = 0x1000, kButtonB = 0x2000;

float Axis(int16_t value) {
  constexpr float kDeadZone = 7849.0f;  // XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE
  const float v = float(value);
  if (std::fabs(v) < kDeadZone) {
    return 0.0f;
  }
  return std::clamp((v - std::copysign(kDeadZone, v)) / (32767.0f - kDeadZone), -1.0f, 1.0f);
}

void StoreFloat(uint8_t* base, uint32_t address, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  Store32(base, address, bits);
}
void Store16(uint8_t* base, uint32_t address, int16_t value) {
  const uint16_t v = __builtin_bswap16(uint16_t(value));
  std::memcpy(base + address, &v, 2);
}

}  // namespace

extern "C" __declspec(dllexport) void NfsmwInputFilter(uint32_t user_index, int16_t* values) {
  if (user_index != 0) {
    return;
  }
  g_pad_buttons_triggers.store(uint64_t(uint16_t(values[0])) | (uint64_t(uint16_t(values[1])) << 16) |
                               (uint64_t(uint16_t(values[2])) << 32));
  g_pad_sticks.store(uint64_t(uint16_t(values[3])) | (uint64_t(uint16_t(values[4])) << 16) |
                     (uint64_t(uint16_t(values[5])) << 32) | (uint64_t(uint16_t(values[6])) << 48));
  if (REXCVAR_GET(freecam)) {
    std::fill(values, values + 7, int16_t(0));
  }
}

// The debug world camera's input step: its actions as usual, then, with the
// free camera on, the controller.
extern "C" REX_FUNC(sub_821751E0) {
  const uint32_t mover = ctx.r3.u32;
  __imp__sub_821751E0(ctx, base);
  if (!REXCVAR_GET(freecam)) {
    return;
  }
  const uint64_t bt = g_pad_buttons_triggers.load();
  const uint64_t st = g_pad_sticks.load();
  const uint16_t buttons = uint16_t(bt);
  const float lt = float(uint8_t(bt >> 16)) / 255.0f, rt = float(uint8_t(bt >> 32)) / 255.0f;
  const float lx = Axis(int16_t(st)), ly = Axis(int16_t(st >> 16));
  const float rx = Axis(int16_t(st >> 32)), ry = Axis(int16_t(st >> 48));
  StoreFloat(base, mover + kMoverForward, ly * 20.0f);
  StoreFloat(base, mover + kMoverSideways, -lx * 20.0f);
  StoreFloat(base, mover + kMoverRise, (rt - lt) * 10.0f);
  // 12000 of 65536 per second at full deflection: about 66 degrees (the
  // debug actions' 20000 turned too fast to aim).
  Store16(base, mover + kMoverYawRate, int16_t(-rx * 12000.0f));
  Store16(base, mover + kMoverPitchRate, int16_t(ry * 12000.0f));
  Store32(base, kTurbo, (buttons & kButtonA) ? 1 : 0);
  Store32(base, kSuperTurbo, (buttons & kButtonB) ? 1 : 0);
}
