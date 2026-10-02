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
// NfsmwInputFilter). The camera is moved here, in real time: position
// (0x82906A20) and target (0x82A2DA90) of the debug camera, with the mover's
// own fields at zero.
//
// Photo mode (F8, with the free camera on): the world stands still. The
// frame's fixed-step accumulator (sub_823A2320) runs the simulation steps
// that are due through sub_823A2098 and returns their time as the world's
// time step; in photo mode no step runs and the time step is 0, while the
// accumulator keeps counting real time (no catch-up afterwards). With a time
// step of 0 the world update (sub_823AFBF8) only runs the cameras in one game
// state (paused, with 0.01 s); in photo mode they run like that every frame,
// with the real time.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>

#include "nfsmw_menu.h"
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

REXCVAR_DEFINE_BOOL(freecam, false, "NFSMW",
                    "Free camera (the game's debug world camera) for the player's view; F6 or "
                    "L3 + R3 (both stick clicks) switch it");
REXCVAR_DEFINE_DOUBLE(fov_scale, 1.0, "NFSMW",
                      "Field of view of the driving camera, times the game's (it still widens "
                      "with speed)")
    .range(0.5, 1.6);
REXCVAR_DEFINE_DOUBLE(freecam_fov, 71.5, "NFSMW",
                      "Field of view of the free camera in degrees (the game's: 71.5); LB/RB "
                      "(keys 1/3) change it, a right stick click (K) resets it")
    .range(10.0, 150.0);
REXCVAR_DEFINE_BOOL(freecam_photo_mode, false, "NFSMW",
                    "Photo mode: with the free camera on, the world stands still");

extern "C" REX_FUNC(__imp__sub_82167640);  // camera director update
extern "C" REX_FUNC(sub_8216E550);         // key of "CDActionDebug" (static)
extern "C" REX_FUNC(sub_82144CA0);         // (key* out, const char* name)
extern "C" REX_FUNC(sub_821832B0);         // (uint32_t* hash, director*) -> new action
extern "C" REX_FUNC(sub_82167330);         // (director*): deletes the running action
extern "C" REX_FUNC(sub_821E4130);         // (key*, view): the view's action changed
extern "C" REX_FUNC(__imp__sub_821751E0);  // debug world camera: its actions to fields
extern "C" REX_FUNC(__imp__sub_823A2320);  // frame: run the due simulation steps -> dt
extern "C" REX_FUNC(__imp__sub_823A2098);  // (stepper*, steps): the simulation steps
extern "C" REX_FUNC(__imp__sub_823AFBF8);
extern "C" REX_FUNC(__imp__sub_82161000);  // (camera, matrix, f1): a camera's new frame  // (dt): world update after the steps
extern "C" REX_FUNC(sub_82168028);         // (dt): cameras (every director)
extern "C" REX_FUNC(sub_82165270);         // (dt): camera movers
extern "C" REX_FUNC(sub_823B6660);         // (dt)

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
extern std::atomic<uint32_t> g_debug_frames;
// Directors switched to the debug action, and the action they got.
std::unordered_map<uint32_t, uint32_t> g_debug_actions;
// The view the free camera is for: the first director seen (the player's).
uint32_t g_player_view = UINT32_MAX;
// The player's camera (static: 0x82C42230 in the German build), from its drive
// action: action +40 is the mover, mover +28 its camera. Whether the player's
// view runs the drive camera (its wanted key's name, +12 of the key).
std::atomic<uint32_t> g_player_camera{0};
std::atomic<bool> g_drive_camera{false};
// Every camera handed a frame (sub_82161000): while a director switches, its
// running action is still the old one, whose +28 is no mover.
std::mutex g_cameras_mutex;
std::vector<uint32_t> g_cameras;
bool IsCamera(uint32_t address) {
  std::lock_guard<std::mutex> lock(g_cameras_mutex);
  return std::find(g_cameras.begin(), g_cameras.end(), address) != g_cameras.end();
}

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
  if (view == g_player_view) {
    const bool drive = Load32(base, director + kDirectorWantedKey + 12) == kDriveActionName;
    g_drive_camera.store(drive);
    const uint32_t action = Load32(base, director + kDirectorAction);
    if (drive && action) {
      // Another action may still run (its +40 is no pointer): only a heap or
      // image address is followed.
      const uint32_t mover = Load32(base, action + 40);
      const uint32_t camera =
          (mover >= 0x40000000u && mover < 0xC0000000u) ? Load32(base, mover + 28) : 0;
      if (camera && camera != g_player_camera.load() && IsCamera(camera)) {
        g_player_camera.store(camera);
        REXLOG_INFO("[freecam] player camera {:08X}", camera);
      }
    }
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
    ++g_debug_frames;
    return;
  }
  if (view == g_player_view) {
    g_debug_frames = 0;
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
    // Photo mode ends with the free camera.
    rex::cvar::SetFlagByName("freecam_photo_mode", "false");
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
constexpr uint32_t kCameraPosition = 0x82906A20, kCameraTarget = 0x82A2DA90;
constexpr uint16_t kButtonA = 0x1000, kButtonB = 0x2000;
constexpr uint16_t kButtonLeftThumb = 0x0040;
constexpr uint16_t kButtonRightThumb = 0x0080, kButtonLeftShoulder = 0x0100,
                   kButtonRightShoulder = 0x0200;
constexpr double kFreecamDefaultFov = 71.5;

// Frames the player's view has run the debug camera: the world stops only
// after a few, once the game has hidden its HUD for the free camera.
std::atomic<uint32_t> g_debug_frames{0};
bool PhotoMode() {
  return REXCVAR_GET(freecam) && REXCVAR_GET(freecam_photo_mode) && g_debug_frames.load() >= 60;
}
std::atomic<bool> g_photo_frame{false};

float LoadFloat(const uint8_t* base, uint32_t address) {
  const uint32_t bits = Load32(base, address);
  float v;
  std::memcpy(&v, &bits, 4);
  return v;
}

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
  // The settings menu (Back + Start) first: while it is open, or the buttons
  // that opened or closed it are held, the game and the free camera get a
  // neutral state.
  const bool menu = NfsmwMenuPadFilter(uint16_t(values[0]), uint8_t(values[1]),
                                       uint8_t(values[2]), values[3], values[4]);
  if (menu) {
    std::fill(values, values + 7, int16_t(0));
  }
  // L3 + R3 (both stick clicks): free camera on / off, like F6. The clicks of
  // that press reach neither the game nor the camera until both are up; R3
  // alone still resets the free camera's zoom, now when released (it would
  // reset it at every chord otherwise).
  static bool thumbs_before = false, thumbs_hold = false, r3_tap = false;
  const uint16_t buttons = uint16_t(values[0]);
  const bool l3 = (buttons & kButtonLeftThumb) != 0, r3 = (buttons & kButtonRightThumb) != 0;
  if (l3 && r3 && !thumbs_before) {
    thumbs_hold = true;
    REXCVAR_SET(freecam, !REXCVAR_GET(freecam));
  }
  thumbs_before = l3 && r3;
  if (l3 || thumbs_hold || menu) {
    r3_tap = false;
  } else if (r3) {
    r3_tap = true;
  } else if (r3_tap) {
    r3_tap = false;
    if (REXCVAR_GET(freecam)) {
      REXCVAR_SET(freecam_fov, kFreecamDefaultFov);
    }
  }
  if (thumbs_hold) {
    if (!l3 && !r3) {
      thumbs_hold = false;
    }
    values[0] = int16_t(buttons & ~(kButtonLeftThumb | kButtonRightThumb));
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
  // The mover adds nothing itself (its time step is 0 in photo mode anyway).
  StoreFloat(base, mover + kMoverForward, 0.0f);
  StoreFloat(base, mover + kMoverSideways, 0.0f);
  StoreFloat(base, mover + kMoverRise, 0.0f);
  Store16(base, mover + kMoverYawRate, 0);
  Store16(base, mover + kMoverPitchRate, 0);
  Store32(base, kTurbo, 0);
  Store32(base, kSuperTurbo, 0);

  static auto last = std::chrono::steady_clock::now();
  const auto now = std::chrono::steady_clock::now();
  const float dt = std::min(std::chrono::duration<float>(now - last).count(), 0.1f);
  last = now;

  // Zoom: LB wider, RB narrower, 30 degrees per second (a right stick click
  // goes back to the game's 71.5, see NfsmwInputFilter).
  if (buttons & (kButtonLeftShoulder | kButtonRightShoulder)) {
    const double step = ((buttons & kButtonLeftShoulder) ? 30.0 : -30.0) * dt;
    REXCVAR_SET(freecam_fov, std::clamp(REXCVAR_GET(freecam_fov) + step, 10.0, 150.0));
  }

  float eye[3], target[3];
  for (int i = 0; i < 3; ++i) {
    eye[i] = LoadFloat(base, kCameraPosition + 4 * i);
    target[i] = LoadFloat(base, kCameraTarget + 4 * i);
  }
  float d[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
  float distance = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
  if (!(distance > 1e-3f)) {
    return;
  }
  // World z is up; the debug camera turns clockwise (seen from above) for
  // "right". About 66 degrees per second at full deflection.
  constexpr float kTurn = 1.15f, kPitchLimit = 1.5f;
  float yaw = std::atan2(d[1], d[0]) - rx * kTurn * dt;
  float pitch = std::clamp(std::asin(std::clamp(d[2] / distance, -1.0f, 1.0f)) + ry * kTurn * dt,
                           -kPitchLimit, kPitchLimit);
  const float forward[3] = {std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw),
                            std::sin(pitch)};
  const float right[3] = {std::sin(yaw), -std::cos(yaw), 0.0f};
  // 20 units per second (like the debug camera's own speed), Space 4x,
  // Backspace 16x.
  const float speed =
      20.0f * ((buttons & kButtonA) ? 4.0f : 1.0f) * ((buttons & kButtonB) ? 16.0f : 1.0f);
  for (int i = 0; i < 3; ++i) {
    eye[i] += (forward[i] * ly + right[i] * lx + (i == 2 ? rt - lt : 0.0f)) * speed * dt;
    target[i] = eye[i] + forward[i] * distance;
    StoreFloat(base, kCameraPosition + 4 * i, eye[i]);
    StoreFloat(base, kCameraTarget + 4 * i, target[i]);
  }
}

// Field of view. The camera keeps it at +196 (uint16, 65536 = a full turn;
// measured on the player's camera: 14196 = 78 degrees driving at rest, the
// debug camera writes 13020 = 71.5 every frame). The movers write it before
// they hand the camera its new frame (sub_82161000, which also keeps the
// previous frame's at +420 for the difference); the projection is built from
// it later, when the view is drawn (sub_8211D510: half the angle, sin/cos).
// So the free camera's or the scaled one is written after sub_82161000, and
// the game's own put back before the next world update, so that a camera
// reading it back never sees the scaled one (and it never compounds).
namespace {
std::atomic<uint32_t> g_fov_written{0};  // 0x10000 | written, 0: none
std::atomic<uint16_t> g_fov_game{0};

void RestoreGameFov(uint8_t* base) {
  const uint32_t written = g_fov_written.exchange(0);
  const uint32_t camera = g_player_camera.load();
  if (!written || !camera) {
    return;
  }
  uint16_t now;
  std::memcpy(&now, base + camera + 196, 2);
  if (__builtin_bswap16(now) == uint16_t(written)) {
    Store16(base, camera + 196, int16_t(g_fov_game.load()));
  }
}
}  // namespace

// The frame's simulation: in photo mode no step and a time step of 0.
extern "C" REX_FUNC(sub_823A2320) {
  const bool photo = PhotoMode();
  g_photo_frame.store(photo);
  __imp__sub_823A2320(ctx, base);
  if (photo) {
    ctx.f1.f64 = 0.0;
  }
}

extern "C" REX_FUNC(sub_823AFBF8) {
  RestoreGameFov(base);
  if (!PhotoMode() || ctx.f1.f64 > 0.0) {
    __imp__sub_823AFBF8(ctx, base);
    return;
  }
  static auto last = std::chrono::steady_clock::now();
  const auto now = std::chrono::steady_clock::now();
  const double dt = std::clamp(std::chrono::duration<double>(now - last).count(), 0.001, 0.1);
  last = now;
  ctx.f1.f64 = dt;
  sub_82168028(ctx, base);
  ctx.f1.f64 = dt;
  sub_82165270(ctx, base);
  ctx.f1.f64 = dt;
  sub_823B6660(ctx, base);
  ctx.f1.f64 = 0.0;
}

extern "C" REX_FUNC(sub_823A2098) {
  if (g_photo_frame.load()) {
    ctx.r4.u64 = 0;
  }
  __imp__sub_823A2098(ctx, base);
}


extern "C" REX_FUNC(sub_82161000) {
  const uint32_t camera = ctx.r3.u32;
  {
    std::lock_guard<std::mutex> lock(g_cameras_mutex);
    if (std::find(g_cameras.begin(), g_cameras.end(), camera) == g_cameras.end() &&
        g_cameras.size() < 64) {
      g_cameras.push_back(camera);
    }
  }
  const bool player = camera && camera == g_player_camera.load();
  if (player) {
    RestoreGameFov(base);
  }
  __imp__sub_82161000(ctx, base);
  if (!player) {
    return;
  }
  uint16_t game;
  std::memcpy(&game, base + camera + 196, 2);
  game = __builtin_bswap16(game);
  double fov = game;
  if (REXCVAR_GET(freecam) && g_debug_frames.load() > 0) {
    if (REXCVAR_GET(freecam_fov) == kFreecamDefaultFov) {
      return;  // the debug camera's own 13020
    }
    fov = REXCVAR_GET(freecam_fov) * (65536.0 / 360.0);
  } else if (g_drive_camera.load() && REXCVAR_GET(fov_scale) != 1.0) {
    fov = game * REXCVAR_GET(fov_scale);
  } else {
    return;
  }
  // Below 170 degrees (the projection's tangent runs away near 180).
  const uint16_t written = uint16_t(std::clamp(fov, 182.0, 170.0 * 65536.0 / 360.0));
  if (written == game) {
    return;
  }
  g_fov_game.store(game);
  Store16(base, camera + 196, int16_t(written));
  g_fov_written.store(0x10000u | written);
}
