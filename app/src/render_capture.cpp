// Render capture, game side (native renderer stages 1-3, see
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
// For replaying the frame without the game (tools/replay), the whole guest
// physical memory is written to capture_snap_N.bin at the first call of the
// frame, plus the vertex, index and UP data each draw uses that changed since
// then. The game may fill that data after the draw call (the GPU reads it
// later), so it is compared at the next KickOff (sub_82596C18), when the
// command buffer goes to the GPU; large ranges (geometry pools) once, at the
// end of the frame.
//
// File: u32 'NFSC', u32 version (2). Then records, all host little-endian:
// - 'EVNT' / 'DRAW' / 'EVTR': u32 magic, u32 sequence, u32 entry,
//   u32 args[8] (r3..r10 at the call), u32 result (r3 after it). DRAW and EVTR
//   (resolve, clear) continue with u32 count + count register values in the
//   order of kShadowGroups; DRAW then has u32 4 + the device's current vertex
//   shader, pixel shader, vertex declaration and index buffer objects (+12948,
//   +12944, +11408, +12532).
// - 'OBJ ': u32 magic, u32 sequence, u32 kind (0 VS, 1 PS, 2 decl, 3 IB,
//   0x100 + n: what argument n of the call points to), u32 guest address,
//   u32 size, then size raw (big-endian) bytes. Objects of a draw are written
//   the first time they show up in the frame.
// - 'MEM ': u32 magic, u32 sequence of the draw it belongs to, u32 physical
//   address, u32 size, then the bytes; to be applied before that draw (it is
//   written later in the file).
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <algorithm>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

REXCVAR_DEFINE_INT32(render_capture_frame, 0, "Debug",
                     "Write every D3D call of this frame (and the register shadow at each draw) "
                     "to capture_game_<frame>.bin; 0 = off")
    .range(0, 1 << 30);
REXCVAR_DEFINE_BOOL(render_capture_memory, true, "Debug",
                    "With render_capture_frame: also write the guest physical memory "
                    "(capture_snap_<frame>.bin) and the data each draw uses, for tools/replay");

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
constexpr uint32_t kFetchConstantsDeviceOffset = 1152;  // registers 0x4800..

constexpr uint32_t kMagicFile = 0x4353464E;    // 'NFSC'
constexpr uint32_t kFileVersion = 2;
constexpr uint32_t kMagicEvent = 0x544E5645;   // 'EVNT'
constexpr uint32_t kMagicDraw = 0x57415244;    // 'DRAW'
constexpr uint32_t kMagicEventRegs = 0x52545645;  // 'EVTR'
constexpr uint32_t kMagicObject = 0x204A424F;  // 'OBJ '
constexpr uint32_t kMagicMemory = 0x204D454D;  // 'MEM '
// Device fields with the current objects, and how much of each to dump.
constexpr uint32_t kObjectFields[4] = {12948, 12944, 11408, 12532};
constexpr uint32_t kObjectDumpBytes = 1024;
constexpr uint32_t kPhysicalSize = 0x20000000;

// Entry points (docs/NATIVE_RENDERER.md).
constexpr const char* kDrawIndexed = "sub_82593C50";      // (dev, prim, base, start, count)
constexpr const char* kBeginVertices = "sub_825932D8";    // (dev, prim, count, stride) -> data
constexpr const char* kResolve = "sub_82592538";          // (dev, flags, rect*, tex*, point*, ..)
constexpr const char* kClear = "sub_8259A500";
constexpr const char* kKickOff = "sub_82596C18";

bool IsDrawEntry(const char* name) {
  return !std::strcmp(name, kDrawIndexed) || !std::strcmp(name, "sub_82593A10") ||
         !std::strcmp(name, kBeginVertices) || !std::strcmp(name, "sub_82593588");
}

std::atomic<uint64_t> g_swaps{0};
std::mutex g_mutex;
FILE* g_file = nullptr;
uint32_t g_sequence = 0;
uint64_t g_file_frame = 0;
std::vector<uint32_t> g_dumped_objects;
// Copy of guest physical memory as of the snapshot plus every MEM record since.
uint8_t* g_memory_copy = nullptr;
// Data of a draw still to be compared: sequence, physical address, size.
struct PendingRange {
  uint32_t sequence, address, size;
};
std::vector<PendingRange> g_pending;
// Ranges larger than this (static geometry pools: a fetch constant can span
// hundreds of MB) are compared at the end of the frame, each page once;
// dynamic buffers are small and compared at every KickOff.
constexpr uint32_t kLargeRange = 1u << 20;
std::vector<PendingRange> g_pending_large;
constexpr uint32_t kPage = 4096;
std::vector<bool> g_page_checked;
uint64_t g_memory_bytes_written = 0;

inline uint32_t LoadBE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// Guest virtual address -> physical (0xE0000000 views are offset by 4 KB).
inline uint32_t GuestToPhysical(uint32_t address) {
  return (address & 0x1FFFFFFF) + (address >= 0xE0000000u ? 0x1000u : 0u);
}

uint8_t* PhysicalBase() {
  auto* kernel = rex::system::kernel_state();
  return kernel && kernel->memory() ? kernel->memory()->physical_membase() : nullptr;
}

// Writes the parts of [address, address + size) that differ from the copy,
// in runs of changed 4 KB pages.
void SyncRange(uint32_t sequence, uint32_t address, uint32_t size) {
  uint8_t* physical = PhysicalBase();
  if (!g_memory_copy || !physical || !size || address >= kPhysicalSize) {
    return;
  }
  size = std::min(size, kPhysicalSize - address);
  const bool large = size > kLargeRange;
  uint32_t run_start = 0, run_size = 0;
  auto flush = [&]() {
    if (!run_size) {
      return;
    }
    std::memcpy(g_memory_copy + run_start, physical + run_start, run_size);
    const uint32_t header[4] = {kMagicMemory, sequence, run_start, run_size};
    std::fwrite(header, sizeof(header), 1, g_file);
    std::fwrite(g_memory_copy + run_start, 1, run_size, g_file);
    g_memory_bytes_written += run_size;
    run_size = 0;
  };
  for (uint32_t at = address, end = address + size; at < end;) {
    const uint32_t chunk = std::min(kPage - (at & (kPage - 1)), end - at);
    bool changed = false;
    if (large && g_page_checked[at / kPage]) {
      changed = false;
    } else {
      changed = std::memcmp(g_memory_copy + at, physical + at, chunk) != 0;
      if (large) {
        g_page_checked[at / kPage] = true;
      }
    }
    if (changed) {
      if (!run_size) {
        run_start = at;
      }
      run_size += chunk;
    } else {
      flush();
    }
    at += chunk;
  }
  flush();
}

void WriteSnapshot(uint64_t frame) {
  uint8_t* physical = PhysicalBase();
  if (!physical) {
    REXLOG_WARN("[render capture] no guest memory, no snapshot");
    return;
  }
  g_memory_copy = static_cast<uint8_t*>(std::malloc(kPhysicalSize));
  if (!g_memory_copy) {
    REXLOG_WARN("[render capture] out of memory for the snapshot");
    return;
  }
  std::memcpy(g_memory_copy, physical, kPhysicalSize);
  const std::string path = "capture_snap_" + std::to_string(frame) + ".bin";
  if (FILE* f = std::fopen(path.c_str(), "wb")) {
    std::fwrite(g_memory_copy, 1, kPhysicalSize, f);
    std::fclose(f);
    REXLOG_INFO("[render capture] guest physical memory written to {}", path);
  }
}

void AddPending(uint32_t sequence, uint32_t address, uint32_t size) {
  (size > kLargeRange ? g_pending_large : g_pending).push_back({sequence, address, size});
}

void SyncPending(bool end_of_frame) {
  for (const PendingRange& p : g_pending) {
    SyncRange(p.sequence, p.address, p.size);
  }
  g_pending.clear();
  if (end_of_frame) {
    for (const PendingRange& p : g_pending_large) {
      SyncRange(p.sequence, p.address, p.size);
    }
    g_pending_large.clear();
  }
}

// The data a draw reads: vertex fetch ranges and its index range.
void AddDrawData(uint32_t sequence, const char* name, const uint32_t* args, const uint8_t* base) {
  const uint8_t* device = base + args[0];
  for (uint32_t i = 0; i < 96; ++i) {
    const uint32_t d0 = LoadBE32(device + kFetchConstantsDeviceOffset + 8 * i);
    const uint32_t d1 = LoadBE32(device + kFetchConstantsDeviceOffset + 8 * i + 4);
    if ((d0 & 3) == 3) {
      AddPending(sequence, d0 & 0x1FFFFFFC, ((d1 >> 2) & 0xFFFFFF) * 4);
    }
  }
  if (!std::strcmp(name, kDrawIndexed)) {
    const uint32_t index_buffer = LoadBE32(device + 12532);
    if (index_buffer) {
      const uint32_t data = GuestToPhysical(LoadBE32(base + index_buffer + 12));
      const uint32_t start = args[3], count = args[4];
      // 16- or 32-bit indices: cover both.
      AddPending(sequence, data + 2 * start, 2 * start + 4 * count);
    }
  }
}

bool CaptureFrameActive() {
  const int32_t target = REXCVAR_GET(render_capture_frame);
  return target > 0 && g_swaps.load(std::memory_order_relaxed) == uint64_t(target);
}

}  // namespace

// native/parallel.cpp: the native renderer running next to the emulation.
bool NativeRendererEnabled();
void NativeRendererRecord(int entry, const uint32_t* args, uint32_t result, uint8_t* base);
void NativeRendererSwapDone();

// Whether the D3D hooks hand their calls to RenderCaptureCall.
bool RenderCaptureActive() { return CaptureFrameActive() || NativeRendererEnabled(); }

void RenderCaptureCall(int entry, const char* name, const uint32_t* args, uint32_t result,
                       uint8_t* base) {
  if (NativeRendererEnabled()) {
    NativeRendererRecord(entry, args, result, base);
  }
  if (!CaptureFrameActive()) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  const uint64_t frame = g_swaps.load(std::memory_order_relaxed);
  if (!g_file) {
    const std::string path = "capture_game_" + std::to_string(frame) + ".bin";
    g_file = std::fopen(path.c_str(), "wb");
    g_file_frame = frame;
    g_sequence = 0;
    g_dumped_objects.clear();
    g_pending.clear();
    g_pending_large.clear();
    g_page_checked.assign(kPhysicalSize / kPage, false);
    g_memory_bytes_written = 0;
    REXLOG_INFO("[render capture] game side: writing frame {} to {}", frame, path);
    if (!g_file) {
      return;
    }
    const uint32_t file_header[2] = {kMagicFile, kFileVersion};
    std::fwrite(file_header, sizeof(file_header), 1, g_file);
    if (REXCVAR_GET(render_capture_memory)) {
      WriteSnapshot(frame);
    }
  }
  // The command buffer goes to the GPU: the data of the draws in it is final.
  if (!std::strcmp(name, kKickOff)) {
    SyncPending(false);
  }

  const bool draw = IsDrawEntry(name);
  const bool with_registers = draw || !std::strcmp(name, kResolve) || !std::strcmp(name, kClear);
  const uint32_t sequence = g_sequence++;
  if (draw) {
    AddDrawData(sequence, name, args, base);
  }
  uint32_t header[12] = {draw ? kMagicDraw : (with_registers ? kMagicEventRegs : kMagicEvent),
                         sequence, uint32_t(entry)};
  std::memcpy(header + 3, args, 8 * sizeof(uint32_t));
  header[11] = result;
  std::fwrite(header, sizeof(header), 1, g_file);
  if (!with_registers) {
    return;
  }
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
  if (!draw) {
    // Resolve / clear: what the pointer arguments point to (rect, texture,
    // point, clear color ...), 64 bytes each.
    for (uint32_t i = 2; i < 8; ++i) {
      if (args[i] >= 0x40000000u && args[i] < 0xFFFF0000u) {
        const uint32_t obj[5] = {kMagicObject, sequence, 0x100 + i, args[i], 64};
        std::fwrite(obj, sizeof(obj), 1, g_file);
        std::fwrite(base + args[i], 1, 64, g_file);
      }
    }
    return;
  }
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
    const uint32_t header_obj[5] = {kMagicObject, sequence, i, address, kObjectDumpBytes};
    std::fwrite(header_obj, sizeof(header_obj), 1, g_file);
    std::fwrite(base + address, 1, kObjectDumpBytes, g_file);
  }
  if (!std::strcmp(name, kBeginVertices) && result) {
    // BeginVertices(dev, prim, count, stride) returns where the caller writes
    // count * stride bytes of vertices.
    g_pending.push_back({sequence, GuestToPhysical(result), args[2] * args[3]});
  }
}

void RenderCaptureSwapDone() {
  if (NativeRendererEnabled()) {
    NativeRendererSwapDone();
  }
  const uint64_t frame = g_swaps.fetch_add(1, std::memory_order_relaxed) + 1;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_file && frame > g_file_frame) {
    SyncPending(true);
    std::fclose(g_file);
    g_file = nullptr;
    std::free(g_memory_copy);
    g_memory_copy = nullptr;
    REXLOG_INFO("[render capture] game side: frame {} done, {} records, {} KB of draw data",
                g_file_frame, g_sequence, g_memory_bytes_written >> 10);
  }
}
