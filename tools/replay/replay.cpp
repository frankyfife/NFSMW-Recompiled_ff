// nfsmw_replay <build dir> <frame> [output dir]
//
// Draws a frame captured with --render_capture_frame natively with Direct3D
// 12 (stage 3 of the native renderer, docs/NATIVE_RENDERER.md): every draw
// once, real render targets instead of EDRAM, resolves as copies. Writes
// every resolve destination of the frame as PNG (the last one is the front
// buffer).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <rex/graphics/format/ucode.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/logging.h>

#include "capture.h"
#include "renderer.h"

using namespace rex::graphics;

namespace replay {

// Length of the microcode at a physical address: the exec instructions of the
// control flow address all ALU / fetch instructions after it.
uint32_t MicrocodeLength(const uint8_t* memory, uint32_t address) {
  constexpr uint32_t kMaxDwords = 0x4000;
  std::vector<uint32_t> host(kMaxDwords);
  for (uint32_t i = 0; i < kMaxDwords; ++i) {
    host[i] = LoadBE32(memory + address + 4 * i);
  }
  uint32_t cf_bound = kMaxDwords / 3, end = 0;
  for (uint32_t i = 0; i < cf_bound; ++i) {
    ucode::ControlFlowInstruction cf[2];
    ucode::UnpackControlFlowInstructions(host.data() + 3 * i, cf);
    for (const auto& c : cf) {
      if (ucode::IsControlFlowOpcodeExec(c.opcode())) {
        cf_bound = std::min(cf_bound, c.exec.address());
        end = std::max(end, c.exec.address() + c.exec.count());
      }
    }
  }
  return 3 * std::max(end, cf_bound);
}

// Shader object -> microcode: vertex shaders at +40, pixel shaders at +12.
bool GetMicrocode(const Capture& capture, uint32_t object, bool pixel, uint32_t& address,
                  uint32_t& dwords) {
  address = dwords = 0;
  const std::vector<uint8_t>* bytes = object ? capture.FindObject(object) : nullptr;
  if (!bytes) {
    return false;
  }
  address = GuestToPhysical(LoadBE32(bytes->data() + (pixel ? 12 : 40)));
  if (address >= Capture::kPhysicalSize - 0x10000) {
    return false;
  }
  dwords = MicrocodeLength(capture.memory(), address);
  return dwords != 0;
}

// The shaders the GPU ran per draw, if tools/replay/assign_shaders.py made
// capture_shaders_N.bin (from the GPU side's capture_ucode_N.bin).
struct GpuShaders {
  std::map<uint64_t, std::vector<uint8_t>> microcode;          // hash -> big-endian
  std::map<uint32_t, std::pair<uint64_t, uint64_t>> by_draw;  // sequence -> VS, PS
  bool Load(const std::string& directory, uint32_t frame) {
    std::vector<uint8_t> raw;
    FILE* f = std::fopen((directory + "/capture_ucode_" + std::to_string(frame) + ".bin").c_str(),
                         "rb");
    if (!f) {
      return false;
    }
    uint32_t header[4];
    while (std::fread(header, sizeof(header), 1, f) == 1) {
      std::vector<uint8_t> code(4 * size_t(header[3]));
      if (std::fread(code.data(), 1, code.size(), f) != code.size()) {
        break;
      }
      microcode[uint64_t(header[1]) | (uint64_t(header[2]) << 32)] = std::move(code);
    }
    std::fclose(f);
    f = std::fopen((directory + "/capture_shaders_" + std::to_string(frame) + ".bin").c_str(),
                   "rb");
    if (!f) {
      return false;
    }
    uint8_t entry[20];
    while (std::fread(entry, sizeof(entry), 1, f) == 1) {
      uint32_t sequence;
      uint64_t vs, ps;
      std::memcpy(&sequence, entry, 4);
      std::memcpy(&vs, entry + 4, 8);
      std::memcpy(&ps, entry + 12, 8);
      by_draw[sequence] = {vs, ps};
    }
    std::fclose(f);
    return true;
  }
};

// Registers not in the device shadow that the draw state needs.
void SetDefaultRegisters(RegisterFile& regs) {
  regs.values[XE_GPU_REG_PA_SC_WINDOW_OFFSET] = 0;
  regs.values[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL] = 0x80000000;  // window offset disabled
  regs.values[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR] = 0x3FFF3FFF;
}

int Main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: nfsmw_replay <build dir> <frame> [output dir]\n");
    return 1;
  }
  rex::InitLogging();
  const uint32_t frame = uint32_t(std::atoi(argv[2]));
  const std::string out_dir =
      argc > 3 ? argv[3] : std::string(argv[1]) + "/replay_" + std::to_string(frame);
  std::filesystem::create_directories(out_dir);
  Capture capture;
  if (!capture.Load(argv[1], frame)) {
    return 1;
  }
  GpuShaders gpu_shaders;
  if (gpu_shaders.Load(argv[1], frame)) {
    std::printf("shaders from the GPU side: %zu programs, %zu draws\n",
                gpu_shaders.microcode.size(), gpu_shaders.by_draw.size());
  }
  Renderer renderer;
  if (!renderer.Initialize() || !renderer.UploadMemory(capture.memory())) {
    std::fprintf(stderr, "renderer initialization failed\n");
    return 1;
  }
  auto regs = std::make_unique<RegisterFile>();
  uint32_t draws = 0, resolves = 0, up_draws = 0, unknown_shaders = 0, gpu_side_shaders = 0;
  // Some resolves of the frame are only sampled in the next one (and some
  // textures are sampled before this frame's resolve writes them): the first
  // pass produces them, the second pass is the frame as the game sees it.
  const int passes = std::getenv("REPLAY_PASSES") ? std::atoi(std::getenv("REPLAY_PASSES")) : 2;
  for (int pass = 0; pass < passes; ++pass) {
    if (pass) {
      renderer.ResetStats();
      draws = resolves = up_draws = unknown_shaders = gpu_side_shaders = 0;
    }
    for (const Record& r : capture.records()) {
      if (!r.memory.empty()) {
        capture.ApplyMemory(r);
        for (const MemoryUpdate& u : r.memory) {
          renderer.UpdateMemory(u.address, capture.memory() + u.address, u.size);
        }
      }
      if (r.is_draw) {
        if (r.entry == kEntryDrawIndexedUP) {
          continue;  // its BeginVertices (sub_825932D8) is the draw
        }
        capture.GetRegisters(r, regs->values);
        SetDefaultRegisters(*regs);
        DrawCall d = {};
        d.regs = regs.get();
        d.sequence = r.sequence;
        d.primitive_type = r.args[1];
        if (r.entry == kEntryDrawIndexed) {
          // (dev, prim, base, start, count); 16-bit indices at index buffer +12.
          const std::vector<uint8_t>* ib = capture.FindObject(r.objects[3]);
          if (!ib) {
            continue;
          }
          d.indexed = true;
          d.vertex_count = r.args[4];
          d.index_address = GuestToPhysical(LoadBE32(ib->data() + 12)) + 2 * r.args[3];
        } else if (r.entry == kEntryDrawVertices) {
          // (dev, prim, start, count)
          d.vertex_count = r.args[3];
        } else if (r.entry == kEntryBeginVertices) {
          // (dev, prim, count, stride) -> vertices, fetched through constant 95.
          d.vertex_count = r.args[2];
          const uint32_t dwords = r.args[2] * r.args[3] / 4;
          regs->values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 0xBE] = GuestToPhysical(r.result) | 3;
          regs->values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 0xBF] = 0x10000002 | (dwords << 2);
          ++up_draws;
        } else {
          continue;
        }
        reg::VGT_DRAW_INITIATOR initiator = {};
        initiator.prim_type = xenos::PrimitiveType(d.primitive_type);
        initiator.num_indices = d.vertex_count;
        regs->values[XE_GPU_REG_VGT_DRAW_INITIATOR] = initiator.value;
        GetMicrocode(capture, r.objects[0], false, d.vertex_shader_address,
                     d.vertex_shader_dwords);
        GetMicrocode(capture, r.objects[1], true, d.pixel_shader_address, d.pixel_shader_dwords);
        d.vertex_shader_code = capture.memory() + d.vertex_shader_address;
        d.pixel_shader_code = capture.memory() + d.pixel_shader_address;
        // The shaders the GPU ran (from the GPU side) where known, else the
        // shader objects' microcode. They only differ where the library
        // patched a vertex shader into scratch memory.
        static const bool object_shaders = std::getenv("REPLAY_OBJECT_SHADERS") != nullptr;
        auto assigned = gpu_shaders.by_draw.find(r.sequence);
        if (assigned != gpu_shaders.by_draw.end() && !object_shaders) {
          auto vs = gpu_shaders.microcode.find(assigned->second.first);
          if (vs != gpu_shaders.microcode.end()) {
            d.vertex_shader_code = vs->second.data();
            d.vertex_shader_dwords = uint32_t(vs->second.size() / 4);
            ++gpu_side_shaders;
          }
          auto ps = gpu_shaders.microcode.find(assigned->second.second);
          if (ps != gpu_shaders.microcode.end() && d.pixel_shader_dwords) {
            d.pixel_shader_code = ps->second.data();
            d.pixel_shader_dwords = uint32_t(ps->second.size() / 4);
          }
        }
        if (!d.vertex_shader_dwords) {
          ++unknown_shaders;
          continue;
        }
        // REPLAY_SKIP=first-last: leave out these draws (for finding what a
        // draw contributes).
        static int skip_first = -1, skip_last = -1;
        if (skip_first < 0) {
          skip_first = 0;
          if (const char* skip = std::getenv("REPLAY_SKIP")) {
            std::sscanf(skip, "%d-%d", &skip_first, &skip_last);
          }
        }
        if (int(r.sequence) >= skip_first && int(r.sequence) <= skip_last) {
          continue;
        }
        renderer.Draw(d);
        ++draws;
      } else if (r.entry == kEntryResolve && r.has_registers) {
        // (dev, flags, rect*, texture*, point*, level, slice, clear color*)
        auto texture = r.pointed.find(3);
        if (texture == r.pointed.end()) {
          continue;
        }
        capture.GetRegisters(r, regs->values);
        SetDefaultRegisters(*regs);
        ResolveCall c = {};
        c.regs = regs.get();
        for (int i = 0; i < 6; ++i) {
          c.dest_fetch[i] = LoadBE32(texture->second.data() + 16 + 4 * i);
        }
        const uint32_t width = (c.dest_fetch[2] & 0x1FFF) + 1;
        const uint32_t height = ((c.dest_fetch[2] >> 13) & 0x1FFF) + 1;
        auto rect = r.pointed.find(2);
        if (rect != r.pointed.end()) {
          for (int i = 0; i < 4; ++i) {
            c.rect[i] = int32_t(LoadBE32(rect->second.data() + 4 * i));
          }
        } else {
          c.rect[0] = c.rect[1] = 0;
          c.rect[2] = int32_t(width);
          c.rect[3] = int32_t(height);
        }
        auto point = r.pointed.find(4);
        if (point != r.pointed.end()) {
          c.dest_point[0] = int32_t(LoadBE32(point->second.data()));
          c.dest_point[1] = int32_t(LoadBE32(point->second.data() + 4));
        } else {
          c.dest_point[0] = c.rect[0];
          c.dest_point[1] = c.rect[1];
        }
        c.dest_slice = r.args[6];
        renderer.Resolve(c);
        ++resolves;
      }
    }
  }
  if (!renderer.Flush()) {
    return 1;
  }
  const RendererStats& s = renderer.stats();
  std::printf(
      "draws %u (UP %u, no vertex shader %u, from the GPU side %u) -> drawn %u, skipped %u | resolves %u | textures "
      "loaded %u, from resolves %u, unsupported %u | pipelines %u, failed %u\n",
      draws, up_draws, unknown_shaders, gpu_side_shaders, s.draws, s.draws_skipped, s.resolves, s.textures_loaded,
      s.textures_from_resolves, s.textures_unsupported, s.pipelines, s.pipeline_failures);
  // The resolves write what the guest would see in memory: with
  // RB_COPY_DEST_INFO swap, blue first. The front buffer (last resolve) is
  // shown swapped back, like the display does.
  uint32_t index = 0;
  const std::vector<uint32_t>& order = renderer.resolve_order();
  for (uint32_t base : order) {
    char name[64];
    std::snprintf(name, sizeof(name), "/resolve_%02u_%08X.png", index++, base);
    if (renderer.SaveResolved(base, out_dir + name, base == order.back())) {
      std::printf("wrote %s%s\n", out_dir.c_str(), name);
    }
  }
  return 0;
}

}  // namespace replay

int main(int argc, char** argv) { return replay::Main(argc, argv); }
