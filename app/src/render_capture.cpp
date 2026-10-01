// Render capture, game side (stage 1 of the native renderer, see
// docs/NATIVE_RENDERER.md).
//
// With --render_capture_frame=N, every call into the game's D3D library during
// frame N (frame = completed Swap calls) is written to capture_game_N.bin in
// the working directory, after the original ran. Draw calls also get the
// complete register shadow of the device (2401 Xenos registers), read from the
// layout the D3D flush (sub_825A40C0) uses. The GPU plugin writes the register
// file at every draw of the same frame to capture_cp_N.bin
// (--gpu_capture_frame=N), and tools/renderprobe/compare_capture.py checks
// that both agree.
//
// Record: u32 magic ('EVNT' or 'DRAW'), u32 sequence, u32 entry, u32 args[6]
// (r3..r8 at the call), and for DRAW u32 count + count register values in the
// order of kShadowGroups, then u32 4 + the device's current vertex shader,
// pixel shader, vertex declaration and index buffer objects (+12948, +12944,
// +11408, +12532). The first time an object shows up in the frame, an 'OBJ '
// record follows: u32 magic, u32 sequence, u32 kind (0 VS, 1 PS, 2 decl, 3 IB),
// u32 guest address, u32 size, then size raw (big-endian) bytes of it.
// All host little-endian.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <algorithm>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_INT32(render_capture_frame, 0, "Debug",
                     "Write every D3D call of this frame (and the register shadow at each draw) "
                     "to capture_game_<frame>.bin; 0 = off")
    .range(0, 1 << 30);

namespace {

// Register shadow of the D3D device: Xenos register range -> device offset.
struct ShadowGroup {
  uint32_t first_register;
  uint32_t count;
  uint32_t device_offset;
};
constexpr ShadowGroup kShadowGroups[] = {
    {0x2000, 16, 11456},  {0x2100, 21, 11532},   {0x2180, 5, 11616},
    {0x2200, 12, 11636},  {0x2280, 21, 11684},   {0x2300, 38, 11768},
    {0x2380, 8, 11920},   {0x4000, 1024, 1920},  {0x4400, 1024, 6016},
    {0x4800, 192, 1152},  {0x4900, 40, 10112},
};
constexpr uint32_t kShadowRegisterCount = 16 + 21 + 5 + 12 + 21 + 38 + 8 + 1024 + 1024 + 192 + 40;

constexpr uint32_t kMagicEvent = 0x544E5645;  // 'EVNT'
constexpr uint32_t kMagicDraw = 0x57415244;   // 'DRAW'
constexpr uint32_t kMagicObject = 0x204A424F;  // 'OBJ '
// Device fields with the current objects, and how much of each to dump.
constexpr uint32_t kObjectFields[4] = {12948, 12944, 11408, 12532};
constexpr uint32_t kObjectDumpBytes = 1024;

// Draw entry points (docs/NATIVE_RENDERER.md).
bool IsDrawEntry(const char* name) {
  return !std::strcmp(name, "sub_82593C50") || !std::strcmp(name, "sub_82593A10") ||
         !std::strcmp(name, "sub_825932D8") || !std::strcmp(name, "sub_82593588");
}

std::atomic<uint64_t> g_swaps{0};
std::mutex g_mutex;
FILE* g_file = nullptr;
uint32_t g_sequence = 0;
uint64_t g_file_frame = 0;
std::vector<uint32_t> g_dumped_objects;

inline uint32_t LoadBE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

}  // namespace

bool RenderCaptureActive() {
  const int32_t target = REXCVAR_GET(render_capture_frame);
  return target > 0 && g_swaps.load(std::memory_order_relaxed) == uint64_t(target);
}

void RenderCaptureCall(int entry, const char* name, const uint32_t* args, uint8_t* base) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const uint64_t frame = g_swaps.load(std::memory_order_relaxed);
  if (!g_file) {
    const std::string path = "capture_game_" + std::to_string(frame) + ".bin";
    g_file = std::fopen(path.c_str(), "wb");
    g_file_frame = frame;
    g_sequence = 0;
    g_dumped_objects.clear();
    REXLOG_INFO("[render capture] game side: writing frame {} to {}", frame, path);
    if (!g_file) {
      return;
    }
  }
  const bool draw = IsDrawEntry(name);
  uint32_t header[9] = {draw ? kMagicDraw : kMagicEvent, g_sequence++, uint32_t(entry)};
  std::memcpy(header + 3, args, 6 * sizeof(uint32_t));
  std::fwrite(header, sizeof(header), 1, g_file);
  if (draw) {
    static uint32_t values[kShadowRegisterCount];
    uint32_t n = 0;
    const uint8_t* device = base + args[0];
    for (const ShadowGroup& g : kShadowGroups) {
      for (uint32_t i = 0; i < g.count; ++i) {
        values[n++] = LoadBE32(device + g.device_offset + 4 * i);
      }
    }
    std::fwrite(&n, sizeof(n), 1, g_file);
    std::fwrite(values, sizeof(uint32_t), n, g_file);
    uint32_t objects[5] = {4};
    for (uint32_t i = 0; i < 4; ++i) {
      objects[1 + i] = LoadBE32(device + kObjectFields[i]);
    }
    std::fwrite(objects, sizeof(objects), 1, g_file);
    for (uint32_t i = 0; i < 4; ++i) {
      const uint32_t address = objects[1 + i];
      if (!address || std::find(g_dumped_objects.begin(), g_dumped_objects.end(), address) !=
                          g_dumped_objects.end()) {
        continue;
      }
      g_dumped_objects.push_back(address);
      const uint32_t header_obj[5] = {kMagicObject, g_sequence - 1, i, address, kObjectDumpBytes};
      std::fwrite(header_obj, sizeof(header_obj), 1, g_file);
      std::fwrite(base + address, 1, kObjectDumpBytes, g_file);
    }
  }
}

void RenderCaptureSwapDone() {
  const uint64_t frame = g_swaps.fetch_add(1, std::memory_order_relaxed) + 1;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_file && frame > g_file_frame) {
    std::fclose(g_file);
    g_file = nullptr;
    REXLOG_INFO("[render capture] game side: frame {} done, {} records", g_file_frame,
                g_sequence);
  }
}
