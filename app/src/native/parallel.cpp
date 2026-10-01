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
#include <vector>

#include <windows.h>

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
REXCVAR_DEFINE_BOOL(native_renderer_window, false, "Debug",
                    "With native_renderer: also show the native picture in a second window");

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
constexpr uint32_t kDeviceVertexShader = 12948, kDevicePixelShader = 12944,
                   kDeviceIndexBuffer = 12532;
constexpr uint32_t kIndirectLoad = 0xC0012700;  // PM4 IM_LOAD, 2 dwords

// Entry indices (tools/renderprobe/d3d_layer.json).
enum : int {
  kEntryResolve = 10,
  kEntryBeginVertices = 11,
  kEntryDrawVerticesUP = 12,
  kEntryDrawVertices = 13,
  kEntryDrawIndexed = 14,
};

struct Item {
  bool resolve = false;
  uint32_t registers = 0;  // offset of the shadow in Frame::registers
  // Draw.
  uint32_t primitive_type = 0, count = 0;
  bool indexed = false;
  uint32_t index_address = 0;
  bool up = false;
  uint32_t up_address = 0, up_dwords = 0;
  uint32_t vs_code = 0, vs_dwords = 0, ps_code = 0, ps_dwords = 0;  // offsets in Frame::code
  // Resolve.
  int32_t rect[4] = {};
  bool has_rect = false;
  uint32_t dest_fetch[6] = {};
  int32_t point[2] = {};
  bool has_point = false;
  uint32_t slice = 0;
};

struct Frame {
  std::vector<uint32_t> registers;
  std::vector<uint8_t> code;
  std::vector<Item> items;
  // Microcode already in `code`: (physical address, hash) -> offset.
  std::map<std::pair<uint32_t, uint64_t>, uint32_t> code_offsets;
  uint32_t front_buffer = 0;
  void Clear() {
    registers.clear();
    code.clear();
    items.clear();
    code_offsets.clear();
    front_buffer = 0;
  }
};

uint64_t Hash(const uint8_t* p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; ++i) {
    h = (h ^ p[i]) * 1099511628211ull;
  }
  return h;
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
  void SwapDone();
  void ShaderLoadsWritten(uint8_t* base, uint32_t device, uint32_t before, uint32_t after);

 private:
  void EnsureThread();
  void Thread();
  void Render(replay::Renderer& renderer, const Frame& frame);
  uint32_t AddCode(const uint8_t* physical, uint32_t address, uint32_t& dwords_out);

  std::mutex mutex_;  // guards recording_ (game threads)
  std::unique_ptr<Frame> recording_ = std::make_unique<Frame>();
  // Last vertex shader IM_LOAD seen, and the device's vertex shader object
  // at that time: only valid while that object is bound.
  uint32_t scanned_vs_ = 0, scanned_vs_object_ = 0;

  std::mutex handoff_mutex_;
  std::condition_variable handoff_cv_;
  std::unique_ptr<Frame> pending_;        // complete frame for the thread
  std::unique_ptr<Frame> spare_;          // reused allocation
  std::thread thread_;
  std::atomic<bool> started_{false};
  std::atomic<uint64_t> frames_recorded_{0}, frames_dropped_{0}, frames_rendered_{0};

 public:
  std::atomic<replay::Renderer*> renderer_{nullptr};
};

uint32_t Parallel::AddCode(const uint8_t* physical, uint32_t address, uint32_t& dwords_out) {
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
  return offset;
}

void Parallel::Record(int entry, const uint32_t* args, uint32_t result, uint8_t* base) {
  auto* kernel = rex::system::kernel_state();
  uint8_t* physical = kernel && kernel->memory() ? kernel->memory()->physical_membase() : nullptr;
  if (!physical) {
    return;
  }
  const bool draw = entry == kEntryDrawIndexed || entry == kEntryDrawVertices ||
                    entry == kEntryBeginVertices;
  if (!draw && entry != kEntryResolve) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  Frame& f = *recording_;
  Item item;
  item.registers = uint32_t(f.registers.size());
  const uint8_t* device = base + args[0];
  for (const ShadowGroup& g : kShadowGroups) {
    for (uint32_t i = 0; i < g.count; ++i) {
      f.registers.push_back(LoadBE32(device + g.device_offset + 4 * i));
    }
  }
  if (draw) {
    item.primitive_type = args[1];
    if (entry == kEntryDrawIndexed) {
      // (dev, prim, base, start, count); 16-bit indices at index buffer +12.
      const uint32_t index_buffer = LoadBE32(device + kDeviceIndexBuffer);
      if (!index_buffer) {
        f.registers.resize(item.registers);
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
    // The vertex shader the library loads (it patches some into scratch
    // memory), else the object's; the pixel shader object's.
    const uint32_t vs_object = LoadBE32(device + kDeviceVertexShader);
    const uint32_t ps_object = LoadBE32(device + kDevicePixelShader);
    uint32_t vs_address = scanned_vs_object_ == vs_object ? scanned_vs_ : 0;
    if (!vs_address && vs_object) {
      vs_address = GuestToPhysical(LoadBE32(base + vs_object + 40));
    }
    if (!vs_address) {
      f.registers.resize(item.registers);
      return;
    }
    item.vs_code = AddCode(physical, vs_address, item.vs_dwords);
    if (ps_object) {
      item.ps_code = AddCode(physical, GuestToPhysical(LoadBE32(base + ps_object + 12)),
                             item.ps_dwords);
    }
  } else {
    // Resolve (dev, flags, rect*, texture*, point*, level, slice, ...).
    item.resolve = true;
    if (!args[3]) {
      f.registers.resize(item.registers);
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
  f.items.push_back(item);
}

void Parallel::ShaderLoadsWritten(uint8_t* base, uint32_t device, uint32_t before,
                                  uint32_t after) {
  std::lock_guard<std::mutex> lock(mutex_);
  const uint32_t vs_object = LoadBE32(base + device + kDeviceVertexShader);
  if (vs_object != scanned_vs_object_) {
    scanned_vs_ = 0;
  }
  if (after <= before || after - before > 0x1000) {
    return;
  }
  for (uint32_t p = before + 4; p + 8 <= after; p += 4) {
    if (LoadBE32(base + p) == kIndirectLoad) {
      const uint32_t address_type = LoadBE32(base + p + 4);
      if ((address_type & 3) == 0) {
        scanned_vs_ = address_type & ~3u;
        scanned_vs_object_ = vs_object;
      }
      p += 8;
    }
  }
}

void Parallel::SwapDone() {
  EnsureThread();
  std::unique_ptr<Frame> done;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    done = std::move(recording_);
    {
      std::lock_guard<std::mutex> handoff(handoff_mutex_);
      recording_ = spare_ ? std::move(spare_) : std::make_unique<Frame>();
    }
    recording_->Clear();
  }
  ++frames_recorded_;
  std::lock_guard<std::mutex> handoff(handoff_mutex_);
  if (pending_) {
    ++frames_dropped_;  // the thread has not taken the previous one yet
    spare_ = std::move(pending_);
  }
  pending_ = std::move(done);
  handoff_cv_.notify_one();
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

  replay::Renderer renderer;
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
  if (window && !renderer.CreateWindowOutput(window)) {
    REXLOG_ERROR("[native renderer] window output failed");
  }
  if (!renderer.CreateSharedOutput(1280, 720)) {
    REXLOG_ERROR("[native renderer] shared output for the game's window failed");
  }
  renderer_ = &renderer;
  REXLOG_INFO("[native renderer] running");

  auto last_log = std::chrono::steady_clock::now();
  double render_ms = 0;
  uint64_t rendered_since_log = 0;
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
      Render(renderer, *frame);
      render_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                             start)
                       .count();
      ++rendered_since_log;
      ++frames_rendered_;
      std::lock_guard<std::mutex> lock(handoff_mutex_);
      if (!spare_) {
        spare_ = std::move(frame);
      }
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - last_log > std::chrono::seconds(10)) {
      const replay::RendererStats& s = renderer.stats();
      REXLOG_INFO(
          "[native renderer] 10 s: {} frames recorded, {} drawn ({:.1f} ms each), {} dropped | "
          "last frame: {} draws, {} skipped, {} resolves, {} KB uploaded | textures loaded {}, "
          "reloaded {}, unsupported {} | pipelines {}, failed {} | ms: sync {:.1f}, textures {:.1f}, "
          "GPU wait {:.1f}",
          frames_recorded_.exchange(0), rendered_since_log,
          rendered_since_log ? render_ms / double(rendered_since_log) : 0.0,
          frames_dropped_.exchange(0), s.draws, s.draws_skipped, s.resolves,
          s.bytes_uploaded >> 10, s.textures_loaded,
          s.textures_reloaded, s.textures_unsupported, s.pipelines, s.pipeline_failures, s.sync_ms,
          s.texture_ms, s.flush_ms);
      render_ms = 0;
      rendered_since_log = 0;
      last_log = now;
    }
  }
}

void Parallel::Render(replay::Renderer& renderer, const Frame& frame) {
  renderer.BeginFrame();
  renderer.ResetFrameStats();
  auto regs = std::make_unique<RegisterFile>();
  for (const Item& item : frame.items) {
    std::memset(regs->values, 0, sizeof(regs->values));
    const uint32_t* shadow = frame.registers.data() + item.registers;
    for (const ShadowGroup& g : kShadowGroups) {
      std::memcpy(&regs->values[g.first_register], shadow, g.count * 4);
      shadow += g.count;
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
    if (item.up) {
      regs->values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 0xBE] = item.up_address | 3;
      regs->values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 0xBF] =
          0x10000002 | (item.up_dwords << 2);
    }
    reg::VGT_DRAW_INITIATOR initiator = {};
    initiator.prim_type = xenos::PrimitiveType(item.primitive_type);
    initiator.num_indices = item.count;
    regs->values[XE_GPU_REG_VGT_DRAW_INITIATOR] = initiator.value;
    d.vertex_shader_code = frame.code.data() + item.vs_code;
    d.vertex_shader_dwords = item.vs_dwords;
    d.pixel_shader_code = frame.code.data() + item.ps_code;
    d.pixel_shader_dwords = item.ps_dwords;
    renderer.Draw(d);
  }
  renderer.PresentToShared(frame.front_buffer);
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
  const uint32_t device = ctx.r3.u32;
  const uint32_t before = LoadBE32(base + device);
  __imp__sub_825A3AF0(ctx, base);
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
  replay::SharedFrame frame;
  if (!renderer || !renderer->GetSharedFrame(frame)) {
    return false;
  }
  info->texture = frame.texture;
  info->fence = frame.fence;
  info->fence_value = frame.fence_value;
  info->width = frame.width;
  info->height = frame.height;
  return true;
}
