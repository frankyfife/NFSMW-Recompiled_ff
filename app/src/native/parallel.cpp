// Native renderer in the running game, in parallel to the emulated GPU (stage 4
// of docs/NATIVE_RENDERER.md, development mode).
//
// With --native_renderer=true the D3D hooks (render_census.cpp ->
// render_capture.cpp) record every draw and resolve of a frame: the device's
// register shadow, the microcode that runs and the resolve arguments. At the
// Swap the frame goes to a thread with its own Direct3D 12 device, which draws
// it with the native renderer (app/src/native/renderer.cpp). Its front buffer
// goes into a texture shared with the emulator's Direct3D 12 device, which
// shows it in the game's window instead of its own (NfsmwNativeFrame below,
// tools/parche_ff.py: IssueSwap); with native_renderer_window also in a second
// window. The emulation still draws every frame; if the renderer is busy, a
// frame is dropped (the game's window shows the previous one).
//
// The renderer reads vertices, indices and textures straight from guest
// memory (a D3D12 heap opened on it), while the game goes on: data the game
// changes in between can show up as glitches in the second window.
#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <windows.h>
#include <emmintrin.h>

#include <rex/cvar.h>
#include <rex/graphics/format/ucode.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>

#include "guest.h"
#include "renderer.h"

REXCVAR_DEFINE_BOOL(native_renderer, false, "Debug",
                    "Draw every frame with the native Direct3D 12 renderer and show its picture in "
                    "the game's window instead of the emulation's (development; the emulation "
                    "still draws every frame too)");
REXCVAR_DEFINE_BOOL(native_renderer_skip_emulation, true, "Debug",
                    "With native_renderer: the emulation skips its draws and resolves once the "
                    "native renderer delivers frames (false = both draw, for comparing)");
REXCVAR_DEFINE_BOOL(native_renderer_window, false, "Debug",
                    "With native_renderer: also show the native picture in a second window");
REXCVAR_DEFINE_INT32(native_renderer_scale, 1, "Debug",
                     "With native_renderer: draw the frames at this multiple of 1280x720 "
                     "(1-4, supersampling)");
REXCVAR_DEFINE_INT32(native_renderer_pipeline_threads, 3, "Debug",
                     "With native_renderer: background threads that create pipelines (0 = "
                     "when first needed, the renderer waits)");
REXCVAR_DEFINE_BOOL(native_renderer_copy_draw_data, true, "Debug",
                    "With native_renderer: draw UP and non-indexed draws from their vertex data "
                    "as it was when the game drew them (false: only count where it differs)");
REXCVAR_DEFINE_BOOL(native_renderer_pipeline_cache, true, "Debug",
                    "With native_renderer: keep created pipelines on disk "
                    "(native_pipelines.bin) and load them in later runs");
REXCVAR_DEFINE_BOOL(native_renderer_hold_incomplete, true, "Debug",
                    "With native_renderer: a frame with draws still waiting for their pipeline "
                    "is not shown (the previous one stays, at most 30 frames)");
REXCVAR_DEFINE_BOOL(native_renderer_bound_lead, true, "Debug",
                    "With native_renderer: the game starts a frame only once the renderer has "
                    "finished the previous one (50 ms at most), so memory it refills is never "
                    "older than what the renderer draws");
REXCVAR_DEFINE_INT32(native_renderer_mipmaps, 0, "Debug",
                     "With native_renderer: texture mipmaps (0 = the game's, 1 = one level "
                     "sharper, 2 = off: only the largest level)");
REXCVAR_DEFINE_INT32(native_renderer_msaa, -1, "Debug",
                     "With native_renderer: samples of the targets the game draws with MSAA "
                     "(-1 = the game's 4, 0 = off, 1 = 2x, 2 = 4x, 3 = 8x)");
REXCVAR_DEFINE_INT32(native_renderer_anisotropic, -1, "Debug",
                     "With native_renderer: anisotropic filtering forced on textures with "
                     "linear filtering and mips (0 off, 1-5 = 1x-16x; -1 = the game's)");

using namespace rex::graphics;
using replay::GuestToPhysical;
using replay::LoadBE32;

namespace {

// Register shadow of the D3D device (as in render_capture.cpp).
struct ShadowGroup {
  uint32_t first_register, count, device_offset;
};
constexpr ShadowGroup kShadowGroups[] = {
    {0x2000, 16, 11456},  {0x2100, 21, 11532},   {0x2180, 5, 11616},
    {0x2200, 12, 11636},  {0x2280, 21, 11684},   {0x2300, 38, 11768},
    {0x2380, 8, 11920},   {0x4000, 1024, 1920},  {0x4400, 1024, 6016},
    {0x4800, 192, 1152},  {0x4900, 40, 10112},
};
constexpr uint32_t kShadowCount = 16 + 21 + 5 + 12 + 21 + 38 + 8 + 1024 + 1024 + 192 + 40;
// The shadow is recorded as changes: chunks of up to kChunk registers (one
// cache line) that differ from the previous draw, as a header (offset in the
// flat shadow | count << 16) and the values.
constexpr uint32_t kChunk = 16;

// Whether two chunks of the shadow are equal: inline SSE2, a library memcmp
// call per 64 bytes cost the game thread more than copying everything.
inline bool ChunkEqual(const uint8_t* a, const uint8_t* b, uint32_t bytes) {
  __m128i diff = _mm_setzero_si128();
  uint32_t i = 0;
  for (; i + 16 <= bytes; i += 16) {
    diff = _mm_or_si128(diff, _mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(a + i)),
                                            _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + i))));
  }
  bool equal = _mm_movemask_epi8(_mm_cmpeq_epi8(diff, _mm_setzero_si128())) == 0xFFFF;
  for (; i < bytes; i += 4) {
    uint32_t x, y;
    std::memcpy(&x, a + i, 4);
    std::memcpy(&y, b + i, 4);
    equal &= x == y;
  }
  return equal;
}

// Register index of each dword of the flat shadow (kShadowGroups in order).
const std::array<uint16_t, kShadowCount>& ShadowRegisters() {
  static const std::array<uint16_t, kShadowCount> table = [] {
    std::array<uint16_t, kShadowCount> t = {};
    uint32_t flat = 0;
    for (const ShadowGroup& g : kShadowGroups) {
      for (uint32_t i = 0; i < g.count; ++i) {
        t[flat++] = uint16_t(g.first_register + i);
      }
    }
    return t;
  }();
  return table;
}
constexpr uint32_t kDeviceVertexShader = 12948, kDevicePixelShader = 12944,
                   kDeviceIndexBuffer = 12532;
// Vertex streams (SetStreamSource sub_8258D968: offset and buffer at +12556
// + 8 s, stride / 4 as a byte at +12688 + s), the vertex declaration (its
// stream count - 1 at +12) and the shadow of fetch constant 95 (stream 0;
// the flush sub_825A2D80 writes stream s to fetch constant 95 - s).
constexpr uint32_t kDeviceStreams = 12556, kDeviceStreamStrides = 12688,
                   kDeviceVertexDeclaration = 11408, kDeviceFetch95 = 1912;
constexpr uint32_t kIndirectLoad = 0xC0012700;   // PM4 IM_LOAD, 2 dwords
constexpr uint32_t kImmediateLoad = 0xC0002B00;  // PM4 IM_LOAD_IMMEDIATE (count in 16:29)

// Entry indices (tools/renderprobe/d3d_layer.json).
enum : int {
  kEntryFlush = 82,  // sub_825A40C0: writes the dirty state out before a draw
  kEntrySegment = 35,  // sub_82597268: a new command buffer segment (returns its write pointer)
  kEntryResolve = 10,
  kEntryBeginVertices = 11,
  kEntryDrawVerticesUP = 12,
  kEntryDrawVertices = 13,
  kEntryDrawIndexed = 14,
};

struct Item {
  bool resolve = false;
  // Occlusion query event (ZPD): report addresses, one per predicated tile.
  bool occlusion = false;
  uint32_t reports[4] = {};
  uint32_t report_count = 0;
  // Register changes since the previous draw or resolve: [begin, end) in
  // Frame::registers.
  uint32_t registers_begin = 0, registers_end = 0;
  // Draw.
  uint32_t primitive_type = 0, count = 0;
  bool indexed = false;
  uint32_t index_address = 0;
  bool up = false;
  uint32_t up_address = 0, up_dwords = 0;
  uint32_t vs_code = 0, vs_dwords = 0, ps_code = 0, ps_dwords = 0;  // offsets in Frame::code
  // The vertex data copied for the draw: [begin, end) in Frame::copies.
  uint32_t copies_begin = 0, copies_end = 0;
  uint32_t stream0_stride_words = 0;  // the draw's stride of fetch constant 95
  // Resolve.
  int32_t rect[4] = {};
  bool has_rect = false;
  uint32_t dest_fetch[6] = {};
  int32_t point[2] = {};
  bool has_point = false;
  uint32_t slice = 0;
};

// Limits of the vertex data copied on the game thread (larger draws are
// drawn from memory as before, and counted).
constexpr uint32_t kMaxCopyBytesPerRange = 1u << 20;
constexpr uint32_t kMaxCopyBytesPerFrame = 4u << 20;

// Copies guest memory that may not be committed: false if it faults.
bool GuardedCopy(uint8_t* dest, const uint8_t* source, size_t size) {
  __try {
    std::memcpy(dest, source, size);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

struct Frame {
  std::vector<uint32_t> registers;  // register changes (see kChunk)
  uint32_t registers_pending = 0;   // start of changes no item took yet
  std::vector<uint8_t> code;
  std::vector<Item> items;
  // Microcode already in `code`: (physical address, hash) -> offset.
  std::map<std::pair<uint32_t, uint64_t>, uint32_t> code_offsets;
  // Microcode of shader objects by address: offset, dwords.
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> code_by_address;
  // Hash of the microcode at each offset in `code`.
  std::unordered_map<uint32_t, uint64_t> code_hashes;
  uint32_t front_buffer = 0;
  uint64_t serial = 0;  // the game's swap count when the frame was complete
  // Vertex data of UP and non-indexed draws as it was at the draw (offset in
  // copy_bytes; size 0: not copied, drawn from memory as before).
  struct Copy {
    uint32_t address, offset, size;
  };
  std::vector<Copy> copies;
  std::vector<uint8_t> copy_bytes;
  // A BeginVertices range: filled by the game after the call returns, copied
  // at the next call into the library on that thread (or at the swap).
  int32_t pending_copy = -1;
  // Draws recorded after a shader load scan that failed (which vertex shader
  // the library had loaded is not known): the frame is not shown.
  uint32_t uncertain_draws = 0;
  // The last patched shader copy (AddCodeCopy).
  bool last_copy_valid = false;
  uint32_t last_copy_address = 0, last_copy_offset = 0;
  uint64_t last_copy_hash = 0;
  void Clear() {
    // Keep the allocations from frame to frame (about 3000 draws).
    if (registers.capacity() < (1u << 20)) {
      registers.reserve(1u << 20);
    }
    if (items.capacity() < 3200) {
      items.reserve(3200);
    }
    registers.clear();
    registers_pending = 0;
    code.clear();
    items.clear();
    code_offsets.clear();
    code_by_address.clear();
    code_hashes.clear();
    front_buffer = 0;
    if (copies.capacity() < 4096) {
      copies.reserve(4096);
    }
    if (copy_bytes.capacity() < kMaxCopyBytesPerFrame) {
      copy_bytes.reserve(kMaxCopyBytesPerFrame);
    }
    copies.clear();
    copy_bytes.clear();
    pending_copy = -1;
    uncertain_draws = 0;
    last_copy_valid = false;
  }
};

// Microcode hash (identity within a run). Eight bytes per step: the byte
// FNV went over every shader of every frame on the game thread.
uint64_t Hash(const uint8_t* p, size_t n) {
  uint64_t h = 1469598103934665603ull ^ n;
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t w;
    std::memcpy(&w, p + i, 8);
    h ^= w;
    h *= 0x9E3779B97F4A7C15ull;
    h ^= h >> 29;
  }
  if (i < n) {
    uint64_t w = 0;
    std::memcpy(&w, p + i, n - i);
    h ^= w;
    h *= 0x9E3779B97F4A7C15ull;
  }
  h ^= h >> 33;
  h *= 0xFF51AFD7ED558CCDull;
  return h ^ (h >> 33);
}

// Length of the microcode at a physical address (the control flow's exec
// instructions address all instructions after it).
uint32_t MicrocodeLength(const uint8_t* memory, uint32_t address) {
  constexpr uint32_t kMaxDwords = 0x4000;
  if (address >= 0x20000000 - kMaxDwords * 4) {
    return 0;
  }
  uint32_t cf_bound = kMaxDwords / 3, end = 0;
  for (uint32_t i = 0; i < cf_bound; ++i) {
    uint32_t dwords[3];
    for (uint32_t j = 0; j < 3; ++j) {
      dwords[j] = LoadBE32(memory + address + 12 * i + 4 * j);
    }
    ucode::ControlFlowInstruction cf[2];
    ucode::UnpackControlFlowInstructions(dwords, cf);
    for (const auto& c : cf) {
      if (ucode::IsControlFlowOpcodeExec(c.opcode())) {
        cf_bound = std::min(cf_bound, c.exec.address());
        end = std::max(end, c.exec.address() + c.exec.count());
      }
    }
  }
  return 3 * std::max(end, cf_bound);
}

class Parallel {
 public:
  static Parallel& Get() {
    static Parallel instance;
    return instance;
  }

  void Record(int entry, const uint32_t* args, uint32_t result, uint8_t* base);
  void Before(int entry, const uint32_t* args, uint8_t* base);
  void SwapDone();
  void ShaderLoadsWritten(uint8_t* base, uint32_t device, uint32_t before, uint32_t after);
  void RecordOcclusion(const uint32_t* addresses, uint32_t count);

 private:
  void EnsureThread();
  void Thread();
  void Render(replay::Renderer& renderer, const Frame& frame);
  uint32_t AddCode(const uint8_t* physical, uint32_t address, uint32_t& dwords_out,
                   bool may_change);
  uint32_t AddCodeCopy(const std::vector<uint8_t>& code, uint32_t address, uint64_t hash);
  uint32_t AddCodeCopyLookup(const std::vector<uint8_t>& code, uint32_t address, uint64_t hash);
  // Vertex data copies (mutex_ held).
  void AddCopy(const uint8_t* physical, uint32_t address, uint32_t size, bool now);
  void TakePendingCopy(const uint8_t* physical);
  void CopyPoint();

  std::mutex mutex_;  // guards recording_ (game threads)
  // The shadow as last recorded (big-endian); invalid at the start of a frame,
  // which then records all of it (the renderer may skip frames).
  std::array<uint32_t, kShadowCount> last_shadow_ = {};
  bool last_shadow_valid_ = false;
  // Dirty masks of the vertex and pixel shader float constants (device +16,
  // +24; one bit per 16 registers, the highest for the first) that the D3D
  // flush wrote out since the last recorded item: only those chunks of the
  // two 1024-register groups can differ from what the GPU had.
  uint64_t pending_constants_dirty_[2] = {};
  // NATIVE_VERIFY_DIRTY: also compare all of them, counting changes outside
  // the masks (should stay 0).
  bool verify_dirty_ = std::getenv("NATIVE_VERIFY_DIRTY") != nullptr;
  std::atomic<uint64_t> dirty_missed_{0};
  std::unique_ptr<Frame> recording_ = std::make_unique<Frame>();
  // Last vertex shader IM_LOAD seen, and the device's vertex shader object
  // at that time: only valid while that object is bound. The microcode is
  // copied at the first draw after it (the library fills an inline copy in
  // after writing the load) and kept: a shader is loaded once while it stays
  // bound, and the command buffer is overwritten long before the object
  // changes.
  uint32_t scanned_vs_ = 0, scanned_vs_object_ = 0, scanned_vs_dwords_ = 0;
  bool scanned_vs_copied_ = false;
  // Shader load scans whose packets did not end where the library stopped
  // writing (their loads are not used).
  std::atomic<uint64_t> walks_misaligned_{0};
  // Since the last scan that failed, until the next one that found a vertex
  // shader load (or the swap): the vertex shader in use is not known.
  bool vs_uncertain_ = false;
  // The thread that records draws (BeginVertices ranges are only copied on
  // it: another thread could get there before the game has filled them).
  std::atomic<uint32_t> record_thread_{0};
  // Vertex data: bytes copied, ranges over the limits or unreadable.
  std::atomic<uint64_t> copied_bytes_{0}, copies_refused_{0};
  // Swaps at which the game waited for the renderer to finish the previous
  // frame (native_renderer_bound_lead), and waits that ran out.
  std::atomic<uint64_t> lead_waits_{0}, lead_wait_timeouts_{0};
  // Frames not shown because draws waited for their pipeline.
  std::atomic<uint64_t> frames_held_{0}, frames_held_uncertain_{0}, frames_held_stride_{0};
  std::vector<uint8_t> scanned_vs_code_;
  uint64_t scanned_vs_hash_ = 0;

  std::mutex handoff_mutex_;
  std::condition_variable handoff_cv_;
  std::unique_ptr<Frame> pending_;        // complete frame for the thread
  // Frames to record into again (they keep their allocations, ~30 MB each).
  std::vector<std::unique_ptr<Frame>> free_frames_;
  std::thread thread_;
  std::atomic<bool> started_{false};
  std::atomic<uint64_t> frames_recorded_{0}, frames_dropped_{0}, frames_rendered_{0};
 public:
  // Swaps the game made, and the swap whose frame the shared output shows.
  std::atomic<uint64_t> swap_serial_total_{0}, shown_serial_{0};
  std::mutex shown_mutex_;
  std::condition_variable shown_cv_;
  // Swaps that showed an older frame than the game's newest (1, 2, 3+ behind).
  std::atomic<uint64_t> late_swaps_[3] = {};
  // Swaps whose wait for the renderer (50 ms) ran out.
  std::atomic<uint64_t> swap_wait_timeouts_{0};
 private:

 public:
  std::atomic<replay::Renderer*> renderer_{nullptr};
};

// This thread recorded a BeginVertices range that may still need copying
// (CopyPoint then takes the mutex; otherwise it returns right away).
thread_local bool t_copy_pending = false;

void Parallel::AddCopy(const uint8_t* physical, uint32_t address, uint32_t size, bool now) {
  Frame& f = *recording_;
  Frame::Copy c = {address, 0, 0};
  f.copies.push_back(c);
  const int32_t index = int32_t(f.copies.size() - 1);
  if (now) {
    // Size stays 0 until the bytes are in.
    const uint32_t offset = uint32_t(f.copy_bytes.size());
    if (!size || size > kMaxCopyBytesPerRange || offset + size > kMaxCopyBytesPerFrame ||
        uint64_t(address) + size > 0x20000000) {
      ++copies_refused_;
      return;
    }
    f.copy_bytes.resize(offset + size);
    if (!GuardedCopy(f.copy_bytes.data() + offset, physical + address, size)) {
      f.copy_bytes.resize(offset);
      ++copies_refused_;
      return;
    }
    f.copies[index].offset = offset;
    f.copies[index].size = size;
    copied_bytes_ += size;
  } else {
    // Filled after the call returns: the size waits in the copy until then.
    f.copies[index].offset = size;
    f.pending_copy = index;
    t_copy_pending = true;
  }
}

void Parallel::TakePendingCopy(const uint8_t* physical) {
  Frame& f = *recording_;
  if (f.pending_copy < 0) {
    return;
  }
  Frame::Copy& c = f.copies[f.pending_copy];
  f.pending_copy = -1;
  const uint32_t size = c.offset;
  c.offset = 0;
  const uint32_t offset = uint32_t(f.copy_bytes.size());
  if (!size || size > kMaxCopyBytesPerRange || offset + size > kMaxCopyBytesPerFrame ||
      uint64_t(c.address) + size > 0x20000000) {
    ++copies_refused_;
    return;
  }
  f.copy_bytes.resize(offset + size);
  if (!GuardedCopy(f.copy_bytes.data() + offset, physical + c.address, size)) {
    f.copy_bytes.resize(offset);
    ++copies_refused_;
    return;
  }
  c.offset = offset;
  c.size = size;
  copied_bytes_ += size;
}

// A call into the library that is not recorded: the last BeginVertices
// range is filled if the call is on the recording thread.
void Parallel::CopyPoint() {
  if (!t_copy_pending) {
    return;
  }
  if (GetCurrentThreadId() != record_thread_.load(std::memory_order_relaxed)) {
    return;
  }
  t_copy_pending = false;
  auto* kernel = rex::system::kernel_state();
  const uint8_t* physical =
      kernel && kernel->memory() ? kernel->memory()->physical_membase() : nullptr;
  if (!physical) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  TakePendingCopy(physical);
}

uint32_t Parallel::AddCode(const uint8_t* physical, uint32_t address, uint32_t& dwords_out,
                           bool may_change) {
  // Shader objects' microcode does not change within a frame: by address.
  // Scratch memory the library patches shaders into does: by content.
  if (!may_change) {
    auto known = recording_->code_by_address.find(address);
    if (known != recording_->code_by_address.end()) {
      dwords_out = known->second.second;
      return known->second.first;
    }
  }
  dwords_out = MicrocodeLength(physical, address);
  if (!dwords_out) {
    return 0;
  }
  const uint8_t* code = physical + address;
  const uint64_t hash = Hash(code, size_t(dwords_out) * 4);
  auto key = std::make_pair(address, hash);
  auto it = recording_->code_offsets.find(key);
  if (it != recording_->code_offsets.end()) {
    return it->second;
  }
  const uint32_t offset = uint32_t(recording_->code.size());
  recording_->code.insert(recording_->code.end(), code, code + size_t(dwords_out) * 4);
  recording_->code_offsets.emplace(key, offset);
  recording_->code_hashes.emplace(offset, hash);
  if (!may_change) {
    recording_->code_by_address.emplace(address, std::make_pair(offset, dwords_out));
  }
  return offset;
}

void Parallel::Before(int entry, const uint32_t* args, uint8_t* base) {
  if (entry != kEntryFlush) {
    return;
  }
  const uint8_t* device = base + args[0];
  const uint64_t vs = (uint64_t(LoadBE32(device + 16)) << 32) | LoadBE32(device + 20);
  const uint64_t ps = (uint64_t(LoadBE32(device + 24)) << 32) | LoadBE32(device + 28);
  std::lock_guard<std::mutex> lock(mutex_);
  pending_constants_dirty_[0] |= vs;
  pending_constants_dirty_[1] |= ps;
}

uint32_t Parallel::AddCodeCopy(const std::vector<uint8_t>& code, uint32_t address,
                               uint64_t hash) {
  // Consecutive draws mostly use the same patched shader: the last one is
  // kept (the ordered map lookup was 1.7 % of the game thread).
  Frame& f = *recording_;
  if (f.last_copy_valid && f.last_copy_address == address && f.last_copy_hash == hash) {
    return f.last_copy_offset;
  }
  const uint32_t offset_found = AddCodeCopyLookup(code, address, hash);
  f.last_copy_valid = true;
  f.last_copy_address = address;
  f.last_copy_hash = hash;
  f.last_copy_offset = offset_found;
  return offset_found;
}

uint32_t Parallel::AddCodeCopyLookup(const std::vector<uint8_t>& code, uint32_t address,
                                     uint64_t hash) {
  auto key = std::make_pair(address, hash);
  auto it = recording_->code_offsets.find(key);
  if (it != recording_->code_offsets.end()) {
    return it->second;
  }
  const uint32_t offset = uint32_t(recording_->code.size());
  recording_->code.insert(recording_->code.end(), code.begin(), code.end());
  recording_->code_offsets.emplace(key, offset);
  recording_->code_hashes.emplace(offset, hash);
  return offset;
}

// The command buffer write pointer of the last segment the D3D library
// started on this thread (0: none since the shader load scan began).
thread_local uint32_t t_segment_start = 0;

void Parallel::Record(int entry, const uint32_t* args, uint32_t result, uint8_t* base) {
  if (entry == kEntrySegment) {
    t_segment_start = result;
    CopyPoint();
    return;
  }
  auto* kernel = rex::system::kernel_state();
  uint8_t* physical = kernel && kernel->memory() ? kernel->memory()->physical_membase() : nullptr;
  if (!physical) {
    return;
  }
  const bool draw = entry == kEntryDrawIndexed || entry == kEntryDrawVertices ||
                    entry == kEntryBeginVertices;
  if (!draw && entry != kEntryResolve) {
    // Any other call into the library (lock, unlock, kickoff, range flush...)
    // comes after the game filled the last BeginVertices range.
    CopyPoint();
    return;
  }
  // BeginVertices that got no memory draws nothing.
  if (entry == kEntryBeginVertices && !result) {
    CopyPoint();
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  Frame& f = *recording_;
  if (draw) {
    record_thread_.store(GetCurrentThreadId(), std::memory_order_relaxed);
    if (vs_uncertain_) {
      ++f.uncertain_draws;
    }
  }
  // The previous BeginVertices range is complete now.
  TakePendingCopy(physical);
  Item item;
  // The shadow as it is in guest memory (big-endian; the renderer thread
  // swaps it), only what changed. Changes of an item that is not recorded
  // stay pending for the next one.
  const uint8_t* device = base + args[0];
  uint32_t flat = 0;
  auto take = [&](const uint8_t* group, uint32_t flat_chunk, uint32_t i, uint32_t n) {
    uint32_t* last = last_shadow_.data() + flat_chunk;
    if (!last_shadow_valid_ ||
        !ChunkEqual(reinterpret_cast<const uint8_t*>(last), group + 4 * i, 4 * n)) {
      std::memcpy(last, group + 4 * i, 4 * n);
      f.registers.push_back(flat_chunk | (n << 16));
      f.registers.insert(f.registers.end(), last, last + n);
      return true;
    }
    return false;
  };
  for (const ShadowGroup& g : kShadowGroups) {
    const uint8_t* group = device + g.device_offset;
    const int constants = g.first_register == 0x4000 ? 0 : (g.first_register == 0x4400 ? 1 : -1);
    // Resolves (about 25 a frame) write the last chunk of both groups
    // themselves, outside the masks (NATIVE_VERIFY_DIRTY): compared whole.
    if (constants >= 0 && last_shadow_valid_ && draw && !verify_dirty_) {
      // Only the chunks the flush wrote out.
      for (uint64_t mask = pending_constants_dirty_[constants]; mask;) {
        const uint32_t chunk = uint32_t(std::countl_zero(mask));
        mask &= ~(uint64_t(1) << (63 - chunk));
        take(group, flat + chunk * kChunk, chunk * kChunk, kChunk);
      }
    } else {
      for (uint32_t i = 0; i < g.count; i += kChunk) {
        const uint32_t n = std::min(kChunk, g.count - i);
        if (take(group, flat + i, i, n) && constants >= 0 && last_shadow_valid_ &&
            !(pending_constants_dirty_[constants] & (uint64_t(1) << (63 - i / kChunk)))) {
          if (draw) {
            ++dirty_missed_;
          }
        }
      }
    }
    flat += g.count;
  }
  pending_constants_dirty_[0] = pending_constants_dirty_[1] = 0;
  last_shadow_valid_ = true;
  if (draw) {
    item.primitive_type = args[1];
    if (entry == kEntryDrawIndexed) {
      // (dev, prim, base, start, count); 16-bit indices at index buffer +12.
      const uint32_t index_buffer = LoadBE32(device + kDeviceIndexBuffer);
      if (!index_buffer) {
        return;
      }
      item.indexed = true;
      item.count = args[4];
      item.index_address = GuestToPhysical(LoadBE32(base + index_buffer + 12)) + 2 * args[3];
    } else if (entry == kEntryDrawVertices) {
      item.count = args[3];  // (dev, prim, start, count)
    } else {
      // BeginVertices (dev, prim, count, stride) -> data, fetched through
      // constant 95.
      item.count = args[2];
      item.up = true;
      item.up_address = GuestToPhysical(result);
      item.up_dwords = args[2] * args[3] / 4;
    }
    // The stride the vertex shader must read stream 0 with: a UP draw's from
    // its call, the others' from the device (SetStreamSource). Normal frames
    // agree; a few frames per 10 s did not, and the HUD was drawn with a
    // shader patched for another layout.
    if (item.up) {
      item.stream0_stride_words = args[3] / 4;
    } else if (LoadBE32(device + kDeviceStreams + 4)) {
      item.stream0_stride_words = device[kDeviceStreamStrides];
    }
    // The vertex data as it is now (see Renderer::ApplyDataCopies).
    item.copies_begin = uint32_t(f.copies.size());
    if (item.up) {
      AddCopy(physical, item.up_address, item.up_dwords * 4, false);
    } else if (entry == kEntryDrawVertices) {
      // (dev, prim, start, count): every stream of the vertex declaration,
      // stream s through fetch constant 95 - s (its shadow at device +1912 -
      // 8 s, written by the flush of this draw), stride at device +12688 + s.
      const uint32_t declaration = LoadBE32(device + kDeviceVertexDeclaration);
      const uint32_t streams =
          declaration ? std::min<uint32_t>(LoadBE32(base + declaration + 12) + 1, 16) : 0;
      const uint64_t start = args[2], count = args[3];
      for (uint32_t s = 0; s < streams; ++s) {
        if (!LoadBE32(device + kDeviceStreams + 8 * s + 4)) {
          continue;
        }
        const uint32_t d0 = LoadBE32(device + kDeviceFetch95 - 8 * s);
        const uint32_t d1 = LoadBE32(device + kDeviceFetch95 - 8 * s + 4);
        const uint64_t stride = uint64_t(device[kDeviceStreamStrides + s]) * 4;
        if ((d0 & 3) != 3 || !stride) {
          continue;
        }
        const uint64_t address = d0 & 0x1FFFFFFC, size = d1 & 0x03FFFFFC;
        const uint64_t first = start * stride;
        const uint64_t last = std::min<uint64_t>((start + count) * stride, size);
        if (first < last && address + last <= 0x20000000) {
          AddCopy(physical, uint32_t(address + first), uint32_t(last - first), true);
        }
      }
    }
    item.copies_end = uint32_t(f.copies.size());
    // The vertex shader the library loads (it patches some into scratch
    // memory), else the object's; the pixel shader object's.
    const uint32_t vs_object = LoadBE32(device + kDeviceVertexShader);
    const uint32_t ps_object = LoadBE32(device + kDevicePixelShader);
    if (scanned_vs_ && scanned_vs_object_ == vs_object && !scanned_vs_copied_) {
      uint32_t dwords = scanned_vs_dwords_;
      if (!dwords) {
        dwords = MicrocodeLength(physical, scanned_vs_);
      }
      scanned_vs_code_.assign(physical + scanned_vs_,
                              physical + scanned_vs_ + size_t(dwords) * 4);
      scanned_vs_hash_ = Hash(scanned_vs_code_.data(), scanned_vs_code_.size());
      scanned_vs_copied_ = true;
    }
    if (scanned_vs_ && scanned_vs_object_ == vs_object && !scanned_vs_code_.empty()) {
      item.vs_code = AddCodeCopy(scanned_vs_code_, scanned_vs_, scanned_vs_hash_);
      item.vs_dwords = uint32_t(scanned_vs_code_.size() / 4);
    } else {
      const uint32_t vs_address =
          vs_object ? GuestToPhysical(LoadBE32(base + vs_object + 40)) : 0;
      if (!vs_address) {
        return;
      }
      item.vs_code = AddCode(physical, vs_address, item.vs_dwords, false);
    }
    if (ps_object) {
      item.ps_code = AddCode(physical, GuestToPhysical(LoadBE32(base + ps_object + 12)),
                             item.ps_dwords, false);
    }
  } else {
    // Resolve (dev, flags, rect*, texture*, point*, level, slice, ...).
    item.resolve = true;
    if (!args[3]) {
      return;
    }
    for (int i = 0; i < 6; ++i) {
      item.dest_fetch[i] = LoadBE32(base + args[3] + 16 + 4 * i);
    }
    if (args[2]) {
      item.has_rect = true;
      for (int i = 0; i < 4; ++i) {
        item.rect[i] = int32_t(LoadBE32(base + args[2] + 4 * i));
      }
    }
    if (args[4]) {
      item.has_point = true;
      item.point[0] = int32_t(LoadBE32(base + args[4]));
      item.point[1] = int32_t(LoadBE32(base + args[4] + 4));
    }
    item.slice = args[6];
    f.front_buffer = GuestToPhysical(item.dest_fetch[1] & 0xFFFFF000);
  }
  item.registers_begin = f.registers_pending;
  item.registers_end = f.registers_pending = uint32_t(f.registers.size());
  f.items.push_back(item);
}

void Parallel::ShaderLoadsWritten(uint8_t* base, uint32_t device, uint32_t before,
                                  uint32_t after) {
  std::lock_guard<std::mutex> lock(mutex_);
  // dwords 0: measured from the control flow at the copy.
  auto keep = [&](uint32_t address, uint32_t dwords) {
    if (uint64_t(address) + dwords * 4ull > 0x20000000) {
      return;
    }
    scanned_vs_ = address;
    scanned_vs_dwords_ = dwords;
    scanned_vs_copied_ = false;
    vs_uncertain_ = false;
    scanned_vs_object_ = LoadBE32(base + device + kDeviceVertexShader);
  };
  const uint32_t vs_object = LoadBE32(base + device + kDeviceVertexShader);
  if (vs_object != scanned_vs_object_) {
    scanned_vs_ = 0;
  }
  if (after <= before || after - before > 0x10000) {
    return;
  }
  // Packet by packet (what the function writes starts after the last dword
  // written before): payloads like constants or vertex data would otherwise
  // be taken for headers.
  // The last vertex shader load the packets have (address, inline dwords or
  // 0), taken only if the walk ends where the library stopped writing.
  uint32_t load_address = 0, load_dwords = 0;
  uint32_t walk_end = before + 4;
  for (uint32_t p = before + 4, next; p + 8 <= after; p = next) {
    const uint32_t header = LoadBE32(base + p);
    switch (header >> 30) {
      case 0:  // registers: count values follow
      case 3:  // count dwords follow
        next = p + 4 * (((header >> 16) & 0x3FFF) + 2);
        break;
      case 1:  // two registers
        next = p + 12;
        break;
      default:  // no-op
        next = p + 4;
        break;
    }
    walk_end = next;
    if (header >> 30 != 3) {
      continue;
    }
    if (header == kIndirectLoad) {
      // IM_LOAD: address | type, start | size.
      const uint32_t address_type = LoadBE32(base + p + 4);
      if ((address_type & 3) == 0) {
        const uint32_t address = address_type & ~3u;
        load_address = address;
        load_dwords = 0;
      }
    } else if ((header & 0xC000FF00) == kImmediateLoad) {
      // IM_LOAD_IMMEDIATE: type, start | size, then the microcode inline. The
      // library copies a vertex shader into the command buffer like this when
      // it patches it for the vertex declaration and links it to the pixel
      // shader (sub_825A37D8): that copy is what runs.
      const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
      if ((LoadBE32(base + p + 4) & 3) == 0 && count > 2) {
        load_address = GuestToPhysical(p + 12);
        load_dwords = count - 2;
      }
    }
  }
  // The last packet ends after the last dword written (short trailing
  // packets are not walked).
  if (walk_end > after + 4 || walk_end + 4 < after) {
    ++walks_misaligned_;
    // The previous patch stays for the same object, but the library may just
    // have loaded another one (BeginVertices patches for its own stride): HUD
    // quads came out as white stripes and triangles for a frame.
    vs_uncertain_ = true;
    return;
  }
  if (load_address) {
    keep(load_address, load_dwords);
  }
}

void Parallel::RecordOcclusion(const uint32_t* addresses, uint32_t count) {
  std::lock_guard<std::mutex> lock(mutex_);
  Item item;
  item.occlusion = true;
  item.report_count = std::min<uint32_t>(count, 4);
  std::memcpy(item.reports, addresses, item.report_count * sizeof(uint32_t));
  recording_->items.push_back(item);
}

void Parallel::SwapDone() {
  EnsureThread();
  std::unique_ptr<Frame> done;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    {
      auto* kernel = rex::system::kernel_state();
      if (const uint8_t* physical =
              kernel && kernel->memory() ? kernel->memory()->physical_membase() : nullptr) {
        TakePendingCopy(physical);
      }
    }
    done = std::move(recording_);
    {
      std::lock_guard<std::mutex> handoff(handoff_mutex_);
      if (free_frames_.empty()) {
        recording_ = std::make_unique<Frame>();
      } else {
        recording_ = std::move(free_frames_.back());
        free_frames_.pop_back();
      }
    }
    recording_->Clear();
    last_shadow_valid_ = false;
    vs_uncertain_ = false;
  }
  ++frames_recorded_;
  done->serial = ++swap_serial_total_;
  const uint64_t serial = done->serial;
  {
    std::lock_guard<std::mutex> handoff(handoff_mutex_);
    if (pending_) {
      ++frames_dropped_;  // the thread has not taken the previous one yet
      free_frames_.push_back(std::move(pending_));
    }
    pending_ = std::move(done);
    handoff_cv_.notify_one();
  }
  // The game may not get more than a frame ahead of the renderer: it starts
  // the next frame only once the renderer has finished the previous one.
  // Memory the game refills (the command ring, buffers in rotation, locked
  // buffers whose fence only the emulated GPU waits for) is then never older
  // than what the renderer is drawing. 50 ms at most.
  // (Only once the renderer runs: if it could not start, or is starting,
  // the waits would only cost 50 ms each.)
  if (REXCVAR_GET(native_renderer_bound_lead) && serial > 1 && renderer_.load()) {
    std::unique_lock<std::mutex> lock(shown_mutex_);
    if (shown_serial_.load() + 1 < serial) {
      ++lead_waits_;
      if (!shown_cv_.wait_for(lock, std::chrono::milliseconds(50),
                              [&]() { return shown_serial_.load() + 1 >= serial; })) {
        ++lead_wait_timeouts_;
      }
    }
  }
}

void Parallel::EnsureThread() {
  bool expected = false;
  if (started_.compare_exchange_strong(expected, true)) {
    thread_ = std::thread([this]() { Thread(); });
    thread_.detach();
  }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_CLOSE) {
    ShowWindow(hwnd, SW_HIDE);
    return 0;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

void Parallel::Thread() {
  SetThreadDescription(GetCurrentThread(), L"Native Renderer");
  HWND window = nullptr;
  if (REXCVAR_GET(native_renderer_window)) {
  WNDCLASSW wc = {};
  wc.lpfnWndProc = WindowProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"NfsmwNativeRenderer";
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  RegisterClassW(&wc);
  RECT rect = {0, 0, 1280, 720};
  AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
  // Never takes the keyboard focus from the game's window.
  window = CreateWindowExW(WS_EX_NOACTIVATE, wc.lpszClassName, L"NFSMW - native renderer (parallel)",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
                                wc.hInstance, nullptr);
  ShowWindow(window, SW_SHOWNOACTIVATE);
  }

  const uint32_t scale = uint32_t(std::clamp(REXCVAR_GET(native_renderer_scale), 1, 4));
  replay::Renderer renderer(scale);
  renderer.SetAnisotropicOverride(REXCVAR_GET(native_renderer_anisotropic));
  renderer.SetMsaaOverride(std::clamp(REXCVAR_GET(native_renderer_msaa), -1, 3));
  renderer.SetAsyncPipelineThreads(
      uint32_t(std::clamp(REXCVAR_GET(native_renderer_pipeline_threads), 0, 8)));
  auto* kernel = rex::system::kernel_state();
  const uint8_t* physical = kernel->memory()->physical_membase();
  if (!renderer.Initialize()) {
    REXLOG_ERROR("[native renderer] Direct3D 12 initialization failed");
    return;
  }
  if (!renderer.UseLiveGuestMemory(physical)) {
    REXLOG_ERROR("[native renderer] no memory for the guest memory copy");
    return;
  }
  // Pipelines of earlier runs, next to the game (native_pipelines.bin).
  if (REXCVAR_GET(native_renderer_pipeline_cache)) {
    renderer.OpenPipelineCache("native_pipelines.bin");
  }
  if (window && !renderer.CreateWindowOutput(window)) {
    REXLOG_ERROR("[native renderer] window output failed");
  }
  if (!renderer.CreateSharedOutput(1280 * scale, 720 * scale)) {
    REXLOG_ERROR("[native renderer] shared output for the game's window failed");
  }
  renderer_ = &renderer;
  REXLOG_INFO("[native renderer] running at {}x{} ({}x)", 1280 * scale, 720 * scale, scale);

  auto last_log = std::chrono::steady_clock::now();
  double render_ms = 0;
  uint64_t rendered_since_log = 0;
  uint32_t shader_failures_logged = 0;
  for (;;) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    std::unique_ptr<Frame> frame;
    {
      std::unique_lock<std::mutex> lock(handoff_mutex_);
      handoff_cv_.wait_for(lock, std::chrono::milliseconds(20), [this]() { return !!pending_; });
      frame = std::move(pending_);
    }
    if (frame) {
      const auto start = std::chrono::steady_clock::now();
      const double translate_before = renderer.stats().translate_ms;
      Render(renderer, *frame);
      const double frame_ms =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
              .count();
      render_ms += frame_ms;
      // A frame that held the renderer up (over 30 ms the game's swap waits run
      // out and the game can overtake it): where the time went.
      if (frame_ms > 30.0) {
        const replay::RendererStats& fs = renderer.stats();
        REXLOG_INFO("[native renderer] slow frame: {:.1f} ms | textures {:.1f} ms ({} loaded, {} "
                    "again) | shader translation {:.1f} ms | sync {:.1f} ms | GPU wait {:.1f} ms "
                    "| {} draws, {} waited for their pipeline",
                    frame_ms, fs.texture_ms, fs.textures_loaded, fs.textures_reloaded,
                    fs.translate_ms - translate_before, fs.sync_ms, fs.flush_ms, fs.draws,
                    fs.draws_waiting_for_pipelines);
      }
      ++rendered_since_log;
      ++frames_rendered_;
      {
        std::lock_guard<std::mutex> lock(shown_mutex_);
        shown_serial_.store(frame->serial);
      }
      shown_cv_.notify_all();
      std::lock_guard<std::mutex> lock(handoff_mutex_);
      free_frames_.push_back(std::move(frame));
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - last_log > std::chrono::seconds(10)) {
      const replay::RendererStats& s = renderer.stats();
      REXLOG_INFO(
          "[native renderer] 10 s: {} frames recorded, {} drawn ({:.1f} ms each), {} dropped | "
          "last frame: {} draws ({} with unpatched vertex shaders), {} skipped ({} shaders unusable so far), "
          "{} resolves, {} texture tables and {} index ranges reused, {} KB "
          "uploaded, {} occlusion reports ({} samples) | textures loaded {}, "
          "reloaded {}, unsupported {} | pipelines {}, failed {} | ms: textures {:.1f}, "
          "GPU wait {:.1f}",
          frames_recorded_.exchange(0), rendered_since_log,
          rendered_since_log ? render_ms / double(rendered_since_log) : 0.0,
          frames_dropped_.exchange(0), s.draws, s.unpatched_vertex_shaders, s.draws_skipped, s.shader_failures,
          s.resolves, s.texture_tables_reused, s.index_ranges_reused, s.bytes_uploaded >> 10, s.occlusion_reports, s.occlusion_samples, s.textures_loaded,
          s.textures_reloaded, s.textures_unsupported, s.pipelines, s.pipeline_failures,
          s.texture_ms, s.flush_ms);
      if (const uint64_t misaligned = walks_misaligned_.exchange(0)) {
        REXLOG_WARN("[native renderer] {} shader load scans did not end at the write pointer",
                    misaligned);
      }
      renderer.SavePipelineCache();
      if (const uint32_t cached = renderer.pipelines_from_cache()) {
        REXLOG_INFO("[native renderer] pipelines loaded from the cache so far: {}", cached);
      }
      {
        static uint64_t logged_differed = 0, logged_differed_bytes = 0;
        const uint64_t copied = copied_bytes_.exchange(0), refused = copies_refused_.exchange(0);
        const uint64_t waits = lead_waits_.exchange(0), timeouts = lead_wait_timeouts_.exchange(0);
        const uint64_t swap_timeouts = swap_wait_timeouts_.exchange(0);
        const uint64_t held = frames_held_.exchange(0);
        const uint64_t held_uncertain = frames_held_uncertain_.exchange(0);
        const uint64_t held_stride = frames_held_stride_.exchange(0);
        REXLOG_INFO("[native renderer] vertex data in 10 s: {} KB copied at the draws ({} ranges "
                    "over the limits), {} draws drawn from the copy where the game had rewritten "
                    "the memory ({} KB) | game waited for the renderer at {} swaps ({} ran out), "
                    "swap waits that ran out {} | frames held back: draws waiting for pipelines {}, "
                    "after a shader load scan that failed {}, vertex shader for another stride {}",
                    copied >> 10, refused, s.draw_data_differed_total - logged_differed,
                    (s.draw_data_differed_bytes_total - logged_differed_bytes) >> 10, waits,
                    timeouts, swap_timeouts, held, held_uncertain, held_stride);
        logged_differed = s.draw_data_differed_total;
        logged_differed_bytes = s.draw_data_differed_bytes_total;
      }
      {
        static uint64_t logged_deferred = 0, logged_reloaded = 0;
        if (s.textures_changes_deferred != logged_deferred ||
            s.textures_reloaded_total != logged_reloaded) {
          REXLOG_INFO("[native renderer] texture memory in 10 s: {} changes seen, {} textures "
                      "loaded again (a change is only taken once it is seen again a frame "
                      "later)",
                      s.textures_changes_deferred - logged_deferred,
                      s.textures_reloaded_total - logged_reloaded);
          logged_deferred = s.textures_changes_deferred;
          logged_reloaded = s.textures_reloaded_total;
        }
      }
      {
        static uint32_t logged_msaa = 0;
        if (s.msaa_samples && s.msaa_samples != logged_msaa) {
          logged_msaa = s.msaa_samples;
          REXLOG_INFO("[native renderer] MSAA setting: the game's MSAA targets drawn with {} "
                      "samples",
                      s.msaa_samples);
        }
      }
      {
        static double logged_translate = 0, logged_pipeline = 0;
        static uint32_t logged_translations = 0, logged_pipelines = 0;
        if (s.translations != logged_translations || s.pipelines != logged_pipelines) {
          REXLOG_INFO("[native renderer] new in 10 s: {} shader translations ({:.0f} ms), {} "
                      "pipelines ({:.0f} ms in the background); last frame {} draws waited "
                      "for theirs",
                      s.translations - logged_translations, s.translate_ms - logged_translate,
                      s.pipelines - logged_pipelines, s.pipeline_ms - logged_pipeline,
                      s.draws_waiting_for_pipelines);
          logged_translate = s.translate_ms;
          logged_pipeline = s.pipeline_ms;
          logged_translations = s.translations;
          logged_pipelines = s.pipelines;
        }
      }
      {
        const uint64_t late[3] = {late_swaps_[0].exchange(0), late_swaps_[1].exchange(0),
                                  late_swaps_[2].exchange(0)};
        if (late[0] || late[1] || late[2]) {
          REXLOG_INFO("[native renderer] swaps that showed an older frame (1/2/3+ behind): {} {} "
                      "{}",
                      late[0], late[1], late[2]);
        }
      }
      if (verify_dirty_) {
        REXLOG_INFO("[native renderer] constant chunks changed outside the dirty masks: {}",
                    dirty_missed_.exchange(0));
      }
      if (s.shader_failures != shader_failures_logged) {
        shader_failures_logged = s.shader_failures;
        const replay::Renderer::ShaderFailure& bad = renderer.last_shader_failure();
        REXLOG_WARN(
            "[native renderer] unusable {} shader microcode at {:08X} ({} dwords): "
            "{:08X} {:08X} {:08X} {:08X}",
            bad.pixel ? "pixel" : "vertex", bad.address, bad.dwords,
            replay::LoadBE32(reinterpret_cast<const uint8_t*>(&bad.first[0])),
            replay::LoadBE32(reinterpret_cast<const uint8_t*>(&bad.first[1])),
            replay::LoadBE32(reinterpret_cast<const uint8_t*>(&bad.first[2])),
            replay::LoadBE32(reinterpret_cast<const uint8_t*>(&bad.first[3])));
      }
      render_ms = 0;
      rendered_since_log = 0;
      last_log = now;
    }
  }
}

void Parallel::Render(replay::Renderer& renderer, const Frame& frame) {
  static std::vector<replay::DrawDataCopy> data_copies;  // renderer thread only
  renderer.SetMipMode(std::clamp(REXCVAR_GET(native_renderer_mipmaps), 0, 2));
  renderer.SetApplyDataCopies(REXCVAR_GET(native_renderer_copy_draw_data));
  renderer.BeginFrame();
  renderer.ResetFrameStats();
  // Registers outside the shadow stay zero (only the defaults below are set);
  // the shadow groups are overwritten for every item.
  static std::unique_ptr<RegisterFile> regs = [] {
    auto file = std::make_unique<RegisterFile>();
    std::memset(file->values, 0, sizeof(file->values));
    return file;
  }();
  const auto& shadow_registers = ShadowRegisters();
  auto* kernel = rex::system::kernel_state();
  uint8_t* guest_memory = kernel->memory()->physical_membase();
  // NATIVE_RENDER_STALL_MS=<ms> (diagnostics): every 120th frame the renderer
  // stops that long before the last tenth of its items (where the HUD is),
  // as when it loads textures or shaders in the middle of a frame.
  static const int stall_ms = [] {
    const char* v = std::getenv("NATIVE_RENDER_STALL_MS");
    return v ? std::atoi(v) : 0;
  }();
  static uint64_t stall_frames = 0;
  const size_t stall_at =
      (stall_ms > 0 && ++stall_frames % 120 == 0) ? frame.items.size() * 9 / 10 : SIZE_MAX;
  size_t item_index = 0;
  for (const Item& item : frame.items) {
    if (item_index++ == stall_at) {
      std::this_thread::sleep_for(std::chrono::milliseconds(stall_ms));
    }
    if (item.occlusion) {
      renderer.OcclusionEvent(item.reports, item.report_count, guest_memory);
      continue;
    }
    // The registers that changed since the previous item (the first item of
    // a frame has all of them).
    const uint32_t* changes = frame.registers.data();
    for (uint32_t p = item.registers_begin; p < item.registers_end;) {
      const uint32_t header = changes[p++];
      const uint16_t* targets = shadow_registers.data() + (header & 0xFFFF);
      const uint32_t n = header >> 16;
      for (uint32_t i = 0; i < n; ++i) {
        regs->values[targets[i]] = _byteswap_ulong(changes[p + i]);
      }
      p += n;
    }
    regs->values[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL] = 0x80000000;
    regs->values[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR] = 0x3FFF3FFF;
    if (item.resolve) {
      replay::ResolveCall c = {};
      c.regs = regs.get();
      std::memcpy(c.dest_fetch, item.dest_fetch, sizeof(c.dest_fetch));
      if (item.has_rect) {
        std::memcpy(c.rect, item.rect, sizeof(c.rect));
      } else {
        c.rect[2] = int32_t((c.dest_fetch[2] & 0x1FFF) + 1);
        c.rect[3] = int32_t(((c.dest_fetch[2] >> 13) & 0x1FFF) + 1);
      }
      c.dest_point[0] = item.has_point ? item.point[0] : c.rect[0];
      c.dest_point[1] = item.has_point ? item.point[1] : c.rect[1];
      c.dest_slice = item.slice;
      renderer.Resolve(c);
      continue;
    }
    replay::DrawCall d = {};
    d.regs = regs.get();
    d.primitive_type = item.primitive_type;
    d.vertex_count = item.count;
    d.indexed = item.indexed;
    d.index_address = item.index_address;
    // A UP draw's data is fetched through constant 95, which it overwrites
    // here; the shadow's values come back after the draw.
    uint32_t* up_fetch = &regs->values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 0xBE];
    const uint32_t up_fetch_saved[2] = {up_fetch[0], up_fetch[1]};
    if (item.up) {
      up_fetch[0] = item.up_address | 3;
      up_fetch[1] = 0x10000002 | (item.up_dwords << 2);
    }
    reg::VGT_DRAW_INITIATOR initiator = {};
    initiator.prim_type = xenos::PrimitiveType(item.primitive_type);
    initiator.num_indices = item.count;
    regs->values[XE_GPU_REG_VGT_DRAW_INITIATOR] = initiator.value;
    auto vs_hash = frame.code_hashes.find(item.vs_code);
    auto ps_hash = frame.code_hashes.find(item.ps_code);
    d.vertex_shader_hash = vs_hash != frame.code_hashes.end() ? vs_hash->second : 0;
    d.pixel_shader_hash = ps_hash != frame.code_hashes.end() ? ps_hash->second : 0;
    d.vertex_shader_code = frame.code.data() + item.vs_code;
    d.vertex_shader_dwords = item.vs_dwords;
    d.pixel_shader_code = frame.code.data() + item.ps_code;
    d.pixel_shader_dwords = item.ps_dwords;
    data_copies.clear();
    for (uint32_t c = item.copies_begin; c < item.copies_end; ++c) {
      const Frame::Copy& copy = frame.copies[c];
      if (copy.size) {
        data_copies.push_back({copy.address, copy.size, frame.copy_bytes.data() + copy.offset});
      }
    }
    d.stream0_stride_words = item.stream0_stride_words;
    d.data_copies = data_copies.data();
    d.data_copy_count = uint32_t(data_copies.size());
    renderer.Draw(d);
    up_fetch[0] = up_fetch_saved[0];
    up_fetch[1] = up_fetch_saved[1];
  }
  // NATIVE_SAVE_FRONT=<png path>: the front buffer every 300 frames (diagnostics).
  static const char* const save_front = std::getenv("NATIVE_SAVE_FRONT");
  if (const char* save_path = save_front) {
    static uint32_t save_count = 0;
    if (++save_count % 300 == 0) {
      renderer.SaveResolved(frame.front_buffer, save_path, false);
    }
  }
  // A frame some of whose draws waited for their pipeline (still created in
  // the background) is missing them: when that is the visual treatment, the
  // whole picture is shown once without the game's colour grading (a white
  // flash). Such a frame is not shown; the previous one stays, at most 30
  // frames in a row.
  // The same for a frame recorded after a shader load scan that failed.
  // The count of frames held in a row starts again only after a complete
  // frame: during a long run of incomplete ones the picture stops once, for
  // 30 frames at most, and then shows them as before.
  static uint32_t held_in_row = 0;
  const bool waited = renderer.stats().draws_waiting_for_pipelines != 0;
  bool mismatched = renderer.stats().draws_stride_mismatch != 0;
  if (mismatched && renderer.mark_mismatch()) {
    renderer.MarkNextPresent();
    mismatched = false;
  }
  const bool incomplete = waited || mismatched || frame.uncertain_draws;
  if (REXCVAR_GET(native_renderer_hold_incomplete) && incomplete && held_in_row < 30) {
    ++held_in_row;
    ++(waited ? frames_held_ : mismatched ? frames_held_stride_ : frames_held_uncertain_);
    // Not shown, but submitted like any frame: its GPU work, upload memory and
    // completion callbacks (occlusion, exposure read back) go on as usual.
    renderer.Submit();
  } else {
    if (!incomplete) {
      held_in_row = 0;
    }
    renderer.PresentToShared(frame.front_buffer);
  }
  if (REXCVAR_GET(native_renderer_skip_emulation)) {
    renderer.WriteBackSmallResolves(guest_memory, 64 * 1024);
  }
  if (REXCVAR_GET(native_renderer_window)) {
    renderer.Present(frame.front_buffer);
  }
}

}  // namespace

bool NativeRendererEnabled() { return REXCVAR_GET(native_renderer); }

void NativeRendererRecord(int entry, const uint32_t* args, uint32_t result, uint8_t* base) {
  Parallel::Get().Record(entry, args, result, base);
}

void NativeRendererSwapDone() { Parallel::Get().SwapDone(); }

void NativeRendererBefore(int entry, const uint32_t* args, uint8_t* base) {
  Parallel::Get().Before(entry, args, base);
}

// The function of the D3D library that writes the shader loads (IM_LOAD) of a
// draw (device +0 is the command buffer write pointer, at the last dword
// written). The vertex shader address it loads is what runs: the library
// patches some vertex shaders into scratch memory.
extern "C" REX_FUNC(__imp__sub_825A3AF0);
extern "C" REX_FUNC(sub_825A3AF0) {
  if (!REXCVAR_GET(native_renderer)) {
    __imp__sub_825A3AF0(ctx, base);
    return;
  }
  // NFSMW_DUMP_IMAGE=<file>: the loaded game image (0x82000000, 13 MB) once,
  // for reverse engineering.
  // NFSMW_DUMP_IMAGE_EVERY=<seconds>: <file>.<n> every so often (to find
  // variables by how they change).
  // (Read once: getenv on every call -thousands per frame- was 30 % of the
  // game thread, in strchr.)
  static bool image_dumped = false;
  static auto last_dump = std::chrono::steady_clock::now();
  static int dump_index = 0;
  static const char* const dump_path = std::getenv("NFSMW_DUMP_IMAGE");
  static const char* const dump_every = std::getenv("NFSMW_DUMP_IMAGE_EVERY");
  if (const char* path = dump_path) {
    const char* every = dump_every;
    const auto now = std::chrono::steady_clock::now();
    if (!image_dumped ||
        (every && now - last_dump > std::chrono::milliseconds(int(std::atof(every) * 1000)))) {
      image_dumped = true;
      last_dump = now;
      const std::string name =
          every ? std::string(path) + "." + std::to_string(dump_index++) : std::string(path);
      if (FILE* out = std::fopen(name.c_str(), "wb")) {
        std::fwrite(base + 0x82000000u, 1, 13434880, out);
        std::fclose(out);
      }
    }
  }
  const uint32_t device = ctx.r3.u32;
  uint32_t before = LoadBE32(base + device);
  t_segment_start = 0;
  __imp__sub_825A3AF0(ctx, base);
  // When the library ran out of room it continued in a new segment: what it
  // wrote is from there on, the memory after `before` is older commands.
  if (t_segment_start) {
    before = t_segment_start;
  }
  Parallel::Get().ShaderLoadsWritten(base, device, before, LoadBE32(base + device));
}

// For the emulator's swap (tools/parche_ff.py, D3D12CommandProcessor::
// IssueSwap): the latest native frame, in a texture its device can open.
struct NfsmwNativeFrameInfo {
  void* texture;  // NT handle of a shared ID3D12Resource, R8G8B8A8, simultaneous access
  void* fence;    // NT handle of a shared ID3D12Fence
  uint64_t fence_value;
  uint32_t width, height;
};
extern "C" __declspec(dllexport) bool NfsmwNativeFrame(NfsmwNativeFrameInfo* info) {
  if (!REXCVAR_GET(native_renderer)) {
    return false;
  }
  replay::Renderer* renderer = Parallel::Get().renderer_.load();
  if (!renderer) {
    return false;
  }
  // The swap shows the frame the game just finished: the GPU thread reaches
  // the swap a few ms before the renderer thread is done with that frame, and
  // without waiting every swap showed the previous one (measured in free
  // roam: 599 of 599). The renderer only needs to have submitted it (the swap
  // waits for its fence on the GPU); 50 ms at most.
  Parallel& p = Parallel::Get();
  {
    const uint64_t wanted = p.swap_serial_total_.load();
    std::unique_lock<std::mutex> lock(p.shown_mutex_);
    if (!p.shown_cv_.wait_for(lock, std::chrono::milliseconds(50),
                              [&]() { return p.shown_serial_.load() >= wanted; })) {
      ++p.swap_wait_timeouts_;
    }
  }
  replay::SharedFrame frame;
  if (!renderer->GetSharedFrame(frame)) {
    return false;
  }
  if (const uint64_t lag = p.swap_serial_total_.load() - p.shown_serial_.load()) {
    ++p.late_swaps_[std::min<uint64_t>(lag, 3) - 1];
  }
  info->texture = frame.texture;
  info->fence = frame.fence;
  info->fence_value = frame.fence_value;
  info->width = frame.width;
  info->height = frame.height;
  return true;
}

// For the emulator's command processor (tools/parche_ff.py,
// ExecutePacketType3Draw): skip the emulated draws and resolves while the
// native renderer delivers the frames.
extern "C" __declspec(dllexport) bool NfsmwNativeSkipEmulation() {
  if (!REXCVAR_GET(native_renderer) || !REXCVAR_GET(native_renderer_skip_emulation)) {
    return false;
  }
  replay::Renderer* renderer = Parallel::Get().renderer_.load();
  replay::SharedFrame frame;
  return renderer && renderer->GetSharedFrame(frame);
}

// The D3D library's occlusion query event (query type 9, sub_8258F810):
// writes RB_SAMPLE_COUNT_ADDR and EVENT_WRITE_ZPD; r4 = query, r5 = which of
// the two reports. With predicated tiling there is one report buffer per
// tile (query +144 = count, +24 = addresses). The native renderer measures
// the samples itself; the emulation no longer draws them.
extern "C" REX_FUNC(__imp__sub_8258EA28);
extern "C" REX_FUNC(sub_8258EA28) {
  const uint32_t query = ctx.r4.u32, second = ctx.r5.u32;
  __imp__sub_8258EA28(ctx, base);
  if (!REXCVAR_GET(native_renderer)) {
    return;
  }
  const uint32_t tiles = LoadBE32(base + query + 144);
  uint32_t addresses[4];
  uint32_t count = 0;
  if (tiles <= 1) {
    addresses[count++] = GuestToPhysical(LoadBE32(base + query + 24) + 32 * second);
  } else {
    for (uint32_t i = 0; i < tiles && count < 4; ++i) {
      addresses[count++] = GuestToPhysical(LoadBE32(base + query + 24 + 4 * i) + 32 * second);
    }
  }
  Parallel::Get().RecordOcclusion(addresses, count);
}
