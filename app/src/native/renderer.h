// Native Direct3D 12 renderer for captured NFSMW frames (stage 3 of the native
// renderer, docs/NATIVE_RENDERER.md).
//
// Differences to the SDK's GPU emulation it is meant to replace:
// - Render targets are real host textures per EDRAM surface (base, pitch,
//   MSAA, format), drawn once, without predicated tiling.
// - Resolves copy (and MSAA-resolve) a render target region into a host
//   texture keyed by the destination address; draws that sample that address
//   later get the host texture. Nothing goes through guest memory.
// - Guest memory is one buffer, uploaded from the snapshot plus the data each
//   draw changed; the translated shaders fetch vertices from it directly.
// Shaders are translated with the SDK's DXBC translator, constants are laid
// out the way it expects (see D3D12CommandProcessor::UpdateBindings).
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <rex/graphics/pipeline/shader/dxbc.h>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>
#include <rex/graphics/register_file.h>
#include <rex/string/buffer.h>

namespace replay {

using Microsoft::WRL::ComPtr;

// One draw of the frame, already decoded from the capture.
struct DrawCall {
  const rex::graphics::RegisterFile* regs;
  uint32_t sequence;           // record sequence in the capture (for tracing)
  uint32_t primitive_type;     // xenos::PrimitiveType
  uint32_t vertex_count;       // indices for indexed draws
  bool indexed;
  uint32_t index_address;      // physical, first index
  bool index_32bit;
  // Big-endian microcode; the address only names it in traces.
  const uint8_t* vertex_shader_code;
  uint32_t vertex_shader_address, vertex_shader_dwords;
  const uint8_t* pixel_shader_code;
  uint32_t pixel_shader_address, pixel_shader_dwords;  // 0 dwords if none
};

struct ResolveCall {
  const rex::graphics::RegisterFile* regs;  // state after the resolve call
  int32_t rect[4];        // source rectangle, left top right bottom
  int32_t dest_point[2];  // destination offset
  uint32_t dest_fetch[6];  // texture fetch constant of the destination texture
  uint32_t dest_slice;     // cube face / array slice
};

struct RendererStats {
  double sync_ms = 0, texture_ms = 0, flush_ms = 0;
  uint32_t draws = 0, draws_skipped = 0, resolves = 0, textures_loaded = 0,
           textures_from_resolves = 0, textures_unsupported = 0, textures_reloaded = 0,
           pipelines = 0, bytes_uploaded = 0,
           pipeline_failures = 0;
};

class Renderer {
 public:
  Renderer();
  ~Renderer();

  bool Initialize();
  // Uploads the whole guest physical memory (512 MB).
  bool UploadMemory(const uint8_t* memory);
  // In the running game: guest memory is read live. Each draw uploads the
  // pages it reads (indices, the vertices they cover) that changed since the
  // last upload, compared once per frame against a copy. Only committed pages
  // are read (the SDK reserves guest memory and commits it as the game
  // allocates).
  bool UseLiveGuestMemory(const uint8_t* physical_memory);
  // Start of a frame of the running game: guest textures are checked against
  // their memory again (the game streams textures into the same memory).
  void BeginFrame();

  // Output to a window: the front buffer (a resolve destination) scaled to it.
  bool CreateWindowOutput(HWND window);
  bool Present(uint32_t front_buffer_base);
  // Copies changed guest memory into the GPU copy before the next draw.
  void UpdateMemory(uint32_t address, const uint8_t* data, uint32_t size);

  void Draw(const DrawCall& draw);
  void Resolve(const ResolveCall& resolve);

  // Executes everything recorded so far and waits for the GPU.
  bool Flush();
  // Writes a resolve destination (by guest base address) as PNG; the last
  // resolve of the frame (the front buffer) after Flush.
  bool SaveResolved(uint32_t base_address, const std::string& path, bool swap_red_blue);
  // Resolve destinations in the order they were first written.
  const std::vector<uint32_t>& resolve_order() const { return resolve_order_; }

  const RendererStats& stats() const { return stats_; }
  void ResetStats() { stats_ = {}; }
  // Everything but the pipeline counts (which only grow).
  void ResetFrameStats() {
    const RendererStats kept = stats_;
    stats_ = {};
    stats_.pipelines = kept.pipelines;
    stats_.pipeline_failures = kept.pipeline_failures;
  }
  // Guest memory as the renderer sees it (for texture loading on the CPU).
  void SetGuestMemory(const uint8_t* memory) { guest_memory_ = memory; }

 private:
  struct Shader;
  struct RenderTarget;
  struct HostTexture;
  struct Pipeline;

  Shader* GetShader(const uint8_t* code, uint32_t dwords, uint32_t address, bool pixel);
  RenderTarget* GetRenderTarget(uint32_t edram_base, uint32_t pitch, uint32_t msaa,
                                uint32_t format, bool depth);
  HostTexture* GetTexture(const uint32_t* fetch, bool for_cube);
  HostTexture* LoadGuestTexture(const uint32_t* fetch);
  ID3D12RootSignature* GetRootSignature(uint32_t vs_textures, uint32_t vs_samplers,
                                        uint32_t ps_textures, uint32_t ps_samplers);
  void Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES& current,
                  D3D12_RESOURCE_STATES next);
  // Linear allocation in the upload buffer; returns the CPU pointer.
  uint8_t* AllocateUpload(uint32_t size, uint32_t alignment, D3D12_GPU_VIRTUAL_ADDRESS& gpu,
                          ID3D12Resource** buffer = nullptr, uint64_t* offset = nullptr);
  D3D12_GPU_DESCRIPTOR_HANDLE AllocateViews(uint32_t count, D3D12_CPU_DESCRIPTOR_HANDLE& cpu);
  bool CreateResolvePipelines();
  bool CreatePresentPipeline();
  bool BeginList();

  ComPtr<IDXGIFactory4> factory_;
  ComPtr<ID3D12Device> device_;
  ComPtr<ID3D12CommandQueue> queue_;
  ComPtr<ID3D12CommandAllocator> allocator_;
  ComPtr<ID3D12GraphicsCommandList> list_;
  ComPtr<ID3D12Fence> fence_;
  uint64_t fence_value_ = 0;
  HANDLE fence_event_ = nullptr;
  bool list_open_ = false;

  // Guest memory on the GPU.
  ComPtr<ID3D12Resource> shared_memory_;
  D3D12_RESOURCE_STATES shared_memory_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
  const uint8_t* guest_memory_ = nullptr;

  // Upload buffers (constants, textures, memory updates, generated indices);
  // a new one when full, all kept until Flush.
  std::vector<ComPtr<ID3D12Resource>> upload_buffers_;
  uint8_t* upload_mapping_ = nullptr;
  uint64_t upload_offset_ = 0, upload_size_ = 0;

  // Shader-visible heaps, allocated linearly; CPU-only heaps for SRVs/RTVs/DSVs.
  ComPtr<ID3D12DescriptorHeap> view_heap_, sampler_heap_;
  uint32_t view_heap_used_ = 0, sampler_heap_used_ = 0;
  uint32_t view_increment_ = 0, sampler_increment_ = 0, rtv_increment_ = 0, dsv_increment_ = 0;
  ComPtr<ID3D12DescriptorHeap> staging_heap_, rtv_heap_, dsv_heap_;
  uint32_t staging_used_ = 0, rtv_used_ = 0, dsv_used_ = 0;
  D3D12_CPU_DESCRIPTOR_HANDLE null_srv_2d_array_{}, null_srv_3d_{}, null_srv_cube_{};
  D3D12_GPU_DESCRIPTOR_HANDLE shared_memory_table_{};
  // Sampler descriptor ranges by their parameters.
  std::map<std::vector<uint64_t>, D3D12_GPU_DESCRIPTOR_HANDLE> sampler_ranges_;

  rex::graphics::DxbcShaderTranslator translator_;
  rex::string::StringBuffer disasm_;
  std::unordered_map<uint64_t, std::unique_ptr<Shader>> shaders_;
  std::map<uint32_t, ComPtr<ID3D12RootSignature>> root_signatures_;
  std::unordered_map<uint64_t, std::unique_ptr<Pipeline>> pipelines_;
  std::map<uint64_t, std::unique_ptr<RenderTarget>> render_targets_;
  std::map<std::vector<uint32_t>, std::unique_ptr<HostTexture>> guest_textures_;
  std::map<uint32_t, std::unique_ptr<HostTexture>> resolved_;
  std::vector<uint32_t> resolve_order_;

  ComPtr<ID3D12RootSignature> resolve_root_signature_;
  ComPtr<ID3D12PipelineState> resolve_color_[2][2];  // [msaa][format r8g8b8a8 / r32f]
  ComPtr<ID3D12PipelineState> resolve_depth_[2];      // [msaa] -> r32f

  bool Readable(uint32_t address, uint32_t size) const;
  void SyncGuestRange(uint32_t address, uint32_t size);
  void SyncDrawData(const DrawCall& d, const rex::graphics::DxbcShader& vertex_shader);
  static constexpr uint32_t kLivePage = 4096;
  bool live_ = false;
  std::unique_ptr<uint8_t[]> live_copy_;
  std::vector<uint64_t> page_synced_frame_;
  uint64_t frame_ = 0;
  // Resources replaced while the GPU may still use them; freed at Flush.
  std::vector<ComPtr<ID3D12Resource>> release_after_flush_;

  ComPtr<IDXGISwapChain3> swap_chain_;
  ComPtr<ID3D12Resource> back_buffers_[2];
  D3D12_CPU_DESCRIPTOR_HANDLE back_buffer_rtvs_[2] = {};
  ComPtr<ID3D12RootSignature> present_root_signature_;
  ComPtr<ID3D12PipelineState> present_pipeline_;
  HWND window_ = nullptr;
  uint32_t window_width_ = 0, window_height_ = 0;

  RendererStats stats_;
};

}  // namespace replay
