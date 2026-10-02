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

#include <condition_variable>
#include <cstdint>
#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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
// Bytes of guest memory a draw reads, as they were when the game drew it
// (copied on the game thread): drawn from instead of the memory as it is when
// the renderer gets there (see Renderer::ApplyDataCopies).
struct DrawDataCopy {
  uint32_t address;  // physical
  uint32_t size;
  const uint8_t* bytes;
};

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
  // Hashes of the microcode if the caller knows them (0 = computed here).
  uint64_t vertex_shader_hash, pixel_shader_hash;
  // Vertex data as it was at the draw (UP and non-indexed draws), if any.
  const DrawDataCopy* data_copies;
  uint32_t data_copy_count;
  // The draw's stride of stream 0 (fetch constant 95) in dwords, 0 unknown:
  // the vertex shader must read it with that stride.
  uint32_t stream0_stride_words;
};

struct ResolveCall {
  const rex::graphics::RegisterFile* regs;  // state after the resolve call
  int32_t rect[4];        // source rectangle, left top right bottom
  int32_t dest_point[2];  // destination offset
  uint32_t dest_fetch[6];  // texture fetch constant of the destination texture
  uint32_t dest_slice;     // cube face / array slice
};

// The latest frame in a texture another Direct3D 12 device can open (the
// emulator's, to show it in the game's window): NT handles, the fence value
// to wait for on the shared fence, size.
struct SharedFrame {
  HANDLE texture = nullptr;
  HANDLE fence = nullptr;
  uint64_t fence_value = 0;
  uint32_t width = 0, height = 0;
};

// One part of a texture's data to untile into upload memory: block rows
// [y_begin, y_end) of all z slices of one subresource (see FillTexture).
struct TextureFill {
  uint8_t* dest = nullptr;  // block row 0 of z slice 0 in the upload memory
  uint32_t dest_row_pitch = 0, dest_slice_pitch = 0;
  uint32_t source = 0;  // the array slice's guest address
  uint32_t offset_x = 0, offset_y = 0, offset_z = 0;  // position in packed mips
  uint32_t blocks_x = 0, y_begin = 0, y_end = 0, depth = 1;
  uint32_t pitch_blocks = 0, z_stride_rows = 0, row_pitch_bytes = 0;
  uint32_t bytes_per_block = 0, bpb_log2 = 0, endian = 0;
  bool tiled = false, is_3d = false;
};

struct RendererStats {
  double sync_ms = 0, texture_ms = 0, flush_ms = 0;
  // Of texture_ms: creating the resources; and the time the renderer thread
  // waited for (and helped) the untiling threads before submitting.
  double texture_create_ms = 0, texture_wait_ms = 0;
  uint32_t texture_fills_queued = 0, texture_faults = 0;
  double translate_ms = 0, pipeline_ms = 0;  // shader translation, pipeline creation
  uint32_t translations = 0;
  uint32_t draws_waiting_for_pipelines = 0;
  uint32_t msaa_samples = 0;
  // Since the start (not reset per frame): texture memory changes seen and
  // not taken yet (see GetTexture), textures loaded again after a change.
  uint64_t textures_changes_deferred = 0, textures_reloaded_total = 0;
  // Since the start: draws whose recorded vertex data differed from memory
  // when the renderer got to them (each one a glitch before), and the bytes.
  uint64_t draw_data_differed_total = 0, draw_data_differed_bytes_total = 0;
  // Draws skipped because their vertex shader reads stream 0 with another
  // stride than the draw has (this frame, and since the start).
  uint32_t draws_stride_mismatch = 0;
  uint64_t draws_stride_mismatch_total = 0;  // the MSAA setting's samples (0: the game's)
  uint32_t unpatched_vertex_shaders = 0;  // vertex fetches without stride
  uint32_t texture_tables_reused = 0;
  uint32_t index_ranges_reused = 0;
  uint32_t shader_failures = 0;  // microcode the analysis could not take
  uint32_t occlusion_reports = 0;
  uint64_t occlusion_samples = 0;
  uint32_t draws = 0, draws_skipped = 0, resolves = 0, textures_loaded = 0,
           textures_from_resolves = 0, textures_unsupported = 0, textures_reloaded = 0,
           pipelines = 0, bytes_uploaded = 0,
           pipeline_failures = 0;
};

class Renderer {
 public:
  // scale: the frame is drawn at this multiple of the guest's resolution
  // (render targets, viewports, resolves and the output; 1-4).
  explicit Renderer(uint32_t scale = 1);
  ~Renderer();
  uint32_t scale() const { return scale_; }
  // Anisotropic filtering forced on textures with linear filtering and mips,
  // as xenos::AnisoFilter (0 off, 1-5 = 1x-16x); -1 leaves the game's.
  void SetAnisotropicOverride(int32_t value) { anisotropic_override_ = value; }
  // Samples of the targets the game draws with MSAA (its own: 4): -1 the
  // game's, 0 none, 1 two, 2 four, 3 eight (fewer if the GPU cannot).
  // Before the first frame.
  void SetMsaaOverride(int32_t value) { msaa_override_ = value; }
  // Texture mipmaps: 0 the game's, 1 one level sharper, 2 off (largest
  // level only). Any time (samplers are keyed by their description).
  void SetMipMode(int32_t value) { mip_mode_ = value; }
  // Draw from the recorded vertex data (DrawCall::data_copies) where it
  // differs from memory; false only counts the differences.
  void SetApplyDataCopies(bool value) { apply_data_copies_ = value; }
  // NATIVE_MARK_MISMATCH (diagnostics): draws whose vertex shader reads
  // another stride are drawn anyway, and the frame gets a magenta square in
  // the top left corner (to see in a recording whether they are wrong).
  bool mark_mismatch() const { return mark_mismatch_; }
  void MarkNextPresent() { mark_next_present_ = true; }
  // Pipelines kept on disk between runs (ID3D12PipelineLibrary): created
  // pipelines are stored, and loaded instead of compiled the next time.
  // Before the first frame; SavePipelineCache writes it if it changed.
  void OpenPipelineCache(const std::string& path);
  void SavePipelineCache();
  uint32_t pipelines_from_cache() const { return pipelines_from_cache_.load(); }
  // Pipelines created by this many background threads (0: when first needed,
  // on the calling thread); draws are skipped until theirs is ready.
  void SetAsyncPipelineThreads(uint32_t threads) { async_pipeline_threads_ = threads; }
  // Threads that untile texture data into upload memory (0: on the renderer
  // thread, as tools/replay does). The copies are recorded at once; Submit
  // waits for the data before the list runs.
  void SetTextureThreads(uint32_t threads) { texture_threads_ = threads; }

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
  // Output for another device: copies of the front buffer in shared textures.
  bool CreateSharedOutput(uint32_t width, uint32_t height);
  bool PresentToShared(uint32_t front_buffer_base);
  // Thread-safe: the latest complete frame.
  bool GetSharedFrame(SharedFrame& frame);
  // Thread-safe and without the lock: whether a frame was shared yet (the
  // emulator asks at every draw packet and shader load).
  bool HasSharedFrame() const { return shared_frame_ready_.load(std::memory_order_acquire); }
  // Resolves of this frame up to max_bytes go into guest memory the way the
  // GPU would write them (when the emulation no longer draws, the CPU still
  // reads some, like the brightness for the exposure). Returns how many.
  uint32_t WriteBackSmallResolves(uint8_t* guest_memory, uint32_t max_bytes);
  // A guest occlusion query event (ZPD): the samples drawn since the previous
  // one are added to a continuous counter, which goes to the report
  // addresses (one per predicated tile; only the first gets the counter)
  // once the GPU is done, like the SDK's ZPD handling.
  void OcclusionEvent(const uint32_t* addresses, uint32_t count, uint8_t* guest_memory);
  // Copies changed guest memory into the GPU copy before the next draw.
  void UpdateMemory(uint32_t address, const uint8_t* data, uint32_t size);

  void Draw(const DrawCall& draw);
  void Resolve(const ResolveCall& resolve);

  // Executes everything recorded so far and waits for the GPU.
  bool Flush();
  // Executes without waiting: the next commands go to the other of two
  // resource sets (command allocator, upload memory, descriptor ranges); the
  // CPU only waits when that set's previous frame is not done yet.
  bool Submit();
  bool Submit(bool wait_for_all);
  // Writes a resolve destination (by guest base address) as PNG; the last
  // resolve of the frame (the front buffer) after Flush.
  bool SaveResolved(uint32_t base_address, const std::string& path, bool swap_red_blue);
  // Resolve destinations in the order they were first written.
  const std::vector<uint32_t>& resolve_order() const { return resolve_order_; }

  const RendererStats& stats() const { return stats_; }
  struct ShaderFailure {
    uint32_t address = 0, dwords = 0;
    bool pixel = false;
    uint32_t first[4] = {};  // the first microcode dwords, as stored (big-endian)
  };
  const ShaderFailure& last_shader_failure() const { return last_shader_failure_; }
  void ResetStats() { stats_ = {}; }
  // Everything but the pipeline and shader failure counts (which only grow).
  void ResetFrameStats() {
    const RendererStats kept = stats_;
    stats_ = {};
    stats_.pipelines = kept.pipelines;
    stats_.pipeline_failures = kept.pipeline_failures;
    stats_.shader_failures = kept.shader_failures;
    stats_.translate_ms = kept.translate_ms;
    stats_.pipeline_ms = kept.pipeline_ms;
    stats_.translations = kept.translations;
    stats_.textures_changes_deferred = kept.textures_changes_deferred;
    stats_.textures_reloaded_total = kept.textures_reloaded_total;
    stats_.draw_data_differed_total = kept.draw_data_differed_total;
    stats_.draw_data_differed_bytes_total = kept.draw_data_differed_bytes_total;
    stats_.draws_stride_mismatch_total = kept.draws_stride_mismatch_total;
  }
  // Guest memory as the renderer sees it (for texture loading on the CPU).
  void SetGuestMemory(const uint8_t* memory) { guest_memory_ = memory; }

 private:
  struct Shader;
  struct RenderTarget;
  struct HostTexture;
  struct Pipeline;

  Shader* GetShader(const uint8_t* code, uint32_t dwords, uint32_t address, bool pixel,
                    uint64_t hash = 0);
  RenderTarget* GetRenderTarget(uint32_t edram_base, uint32_t pitch, uint32_t msaa,
                                uint32_t format, bool depth);
  HostTexture* GetTexture(const uint32_t* fetch, bool for_cube);
  HostTexture* LoadGuestTexture(const uint32_t* fetch);
  // Guest textures are placed in large heaps: a committed resource each took
  // about 0.2 ms (60 of the 65 ms an area's 301 textures took to load).
  struct TextureBlock {
    uint32_t heap = UINT32_MAX;
    uint64_t offset = 0, size = 0;
  };
  struct TextureHeap {
    ComPtr<ID3D12Heap> heap;
    std::map<uint64_t, uint64_t> free;  // offset -> size
  };
  bool PlaceTexture(D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES state,
                    ComPtr<ID3D12Resource>& resource, TextureBlock& block);
  bool AllocateTextureBlock(uint64_t size, uint64_t alignment, TextureBlock& block);
  void FreeTextureBlock(const TextureBlock& block);
  std::vector<TextureHeap> texture_heaps_;
  ID3D12RootSignature* GetRootSignature(uint32_t vs_textures, uint32_t vs_samplers,
                                        uint32_t ps_textures, uint32_t ps_samplers);
  void Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES& current,
                  D3D12_RESOURCE_STATES next);
  // Linear allocation in the upload buffer; returns the CPU pointer.
  uint8_t* AllocateUpload(uint32_t size, uint32_t alignment, D3D12_GPU_VIRTUAL_ADDRESS& gpu,
                          ID3D12Resource** buffer = nullptr, uint64_t* offset = nullptr);
  D3D12_GPU_DESCRIPTOR_HANDLE AllocateViews(uint32_t count, D3D12_CPU_DESCRIPTOR_HANDLE& cpu);
  bool CreateResolvePipelines();
  void ActivateSet(uint32_t index);
  // Runs once the GPU finished the commands recorded so far.
  void AfterCompletion(std::function<void()> work) {
    sets_[set_].after_completion.push_back(std::move(work));
  }
  bool CreatePresentPipeline();
  void CreatePipeline(Pipeline& pipeline, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc);
  bool TryLoadCachedPipeline(Pipeline& pipeline, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc);
  void StartPipelineWorkers();
  bool EnsureOcclusionQueries();
  void OcclusionBegin();
  void OcclusionEnd();
  bool BeginList();

  ComPtr<IDXGIFactory4> factory_;
  ComPtr<ID3D12Device> device_;
  ComPtr<ID3D12CommandQueue> queue_;
  ComPtr<ID3D12CommandAllocator> allocator_;
  struct FrameSet {
    ComPtr<ID3D12CommandAllocator> allocator;
    std::vector<ComPtr<ID3D12Resource>> upload_buffers;
    uint8_t* upload_mapping = nullptr;
    uint64_t upload_size = 0;
    uint64_t fence_value = 0;
    std::vector<ComPtr<ID3D12Resource>> release;
    std::vector<std::function<void()>> after_completion;
  };
  FrameSet sets_[2];
  uint32_t set_ = 0;
  uint32_t view_heap_begin_ = 2, view_heap_end_ = 0, sampler_heap_begin_ = 0,
           sampler_heap_end_ = 0;
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
  std::unordered_map<uint64_t, D3D12_GPU_DESCRIPTOR_HANDLE> sampler_ranges_;
  std::unordered_map<uint64_t, D3D12_GPU_DESCRIPTOR_HANDLE> texture_ranges_;

  uint32_t scale_ = 1;
  uint32_t async_pipeline_threads_ = 0;
  struct PipelineJob {
    Pipeline* pipeline = nullptr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
  };
  std::mutex pipeline_jobs_mutex_;
  std::condition_variable pipeline_jobs_cv_;
  std::deque<PipelineJob> pipeline_jobs_;
  std::vector<std::thread> pipeline_workers_;
  bool pipeline_workers_stop_ = false;
  void StartTextureWorkers();
  void QueueTextureFill(const TextureFill& fill);
  void WaitTextureFills();
  uint32_t texture_threads_ = 0;
  std::vector<std::thread> texture_workers_;
  std::mutex texture_jobs_mutex_;
  std::condition_variable texture_jobs_cv_, texture_done_cv_;
  std::deque<TextureFill> texture_jobs_;
  uint32_t texture_jobs_pending_ = 0;  // queued or running (under the mutex)
  bool texture_workers_stop_ = false;
  std::atomic<uint32_t> texture_faults_{0};
  std::vector<TextureFill> texture_fills_;  // LoadGuestTexture's, reused
  int32_t anisotropic_override_ = -1;
  int32_t msaa_override_ = -1;
  int32_t mip_mode_ = 0;
  bool apply_data_copies_ = true;
  bool mark_mismatch_ = std::getenv("NATIVE_MARK_MISMATCH") != nullptr;
  bool mark_next_present_ = false;
  ComPtr<ID3D12PipelineLibrary> pipeline_library_;
  std::vector<uint8_t> pipeline_library_blob_;  // must outlive the library
  std::string pipeline_library_path_;
  std::mutex pipeline_library_mutex_;
  std::atomic<uint32_t> pipeline_library_stored_{0}, pipelines_from_cache_{0};
  // Pages the last draw's recorded data was written into: synced again from
  // memory before the next draw (other draws see the memory as today).
  std::vector<uint32_t> overlay_pages_;
  void ApplyDataCopies(const DrawCall& d);
  uint32_t msaa_supported_ = 0;  // highest count all formats support, 0: not asked yet
  uint32_t HostSamples(uint32_t guest_msaa);
  rex::graphics::DxbcShaderTranslator translator_;
  rex::string::StringBuffer disasm_;
  std::unordered_map<uint64_t, std::unique_ptr<Shader>> shaders_;
  std::map<uint32_t, ComPtr<ID3D12RootSignature>> root_signatures_;
  std::unordered_map<uint64_t, std::unique_ptr<Pipeline>> pipelines_;
  std::map<uint64_t, std::unique_ptr<RenderTarget>> render_targets_;
  // By a hash of the fetch constant words that define the data.
  std::unordered_map<uint64_t, std::unique_ptr<HostTexture>> guest_textures_;
  std::map<uint32_t, std::unique_ptr<HostTexture>> resolved_;
  std::vector<uint32_t> resolve_order_;

  ComPtr<ID3D12RootSignature> resolve_root_signature_;
  ComPtr<ID3D12PipelineState> resolve_color_[2][2];  // [msaa][format r8g8b8a8 / r32f]
  ComPtr<ID3D12PipelineState> resolve_depth_[2];      // [msaa] -> r32f

  bool Readable(uint32_t address, uint32_t size);
  void SyncGuestRange(uint32_t address, uint32_t size);
  void SyncDrawData(const DrawCall& d, const rex::graphics::DxbcShader& vertex_shader);
  static constexpr uint32_t kLivePage = 4096;
  bool live_ = false;
  std::unique_ptr<uint8_t[]> live_copy_;
  std::vector<uint64_t> page_synced_frame_;
  std::vector<uint64_t> page_changed_frame_;  // last frame a page's data changed
  struct IndexRange {
    uint32_t min = 0, max = 0;
    uint64_t frame = 0;  // when it was measured
  };
  std::unordered_map<uint64_t, IndexRange> index_ranges_;
  std::vector<uint64_t> page_readable_frame_;
  uint64_t frame_ = 0;
  // Resources replaced while the GPU may still use them; freed at Flush.
  std::vector<ComPtr<ID3D12Resource>> release_after_flush_;

  ComPtr<IDXGISwapChain3> swap_chain_;
  ComPtr<ID3D12Resource> back_buffers_[2];
  D3D12_CPU_DESCRIPTOR_HANDLE back_buffer_rtvs_[2] = {};
  ComPtr<ID3D12RootSignature> present_root_signature_;
  ComPtr<ID3D12PipelineState> present_pipeline_;
  // Graphics state set on the open list by the last draw; reset by a new
  // list and by anything else that records graphics state.
  struct BoundState {
    bool valid = false;
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pipeline = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4] = {};
    uint32_t rtv_count = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = {};
    D3D12_GPU_VIRTUAL_ADDRESS cbvs[5] = {};
    D3D12_GPU_DESCRIPTOR_HANDLE shared_memory_table = {};
    D3D12_GPU_DESCRIPTOR_HANDLE tables[4] = {};
    D3D12_VIEWPORT viewport = {};
    D3D12_RECT scissor = {};
    float blend_factor[4] = {};
    uint32_t stencil_ref = 0;
    D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    D3D12_INDEX_BUFFER_VIEW index_view = {};
  };
  BoundState bound_;
  void InvalidateBound() { bound_ = BoundState{}; }
  HWND window_ = nullptr;
  ShaderFailure last_shader_failure_;

  static constexpr uint32_t kOcclusionSlots = 16384;
  struct OcclusionReport {
    std::vector<uint32_t> slots;
    std::vector<float> ratios;  // guest samples per host sample, per slot
    std::vector<uint32_t> addresses;
  };
  float occlusion_ratio_[kOcclusionSlots] = {};
  ComPtr<ID3D12QueryHeap> occlusion_heap_;
  ComPtr<ID3D12Resource> occlusion_readback_;
  const uint64_t* occlusion_mapping_ = nullptr;
  uint64_t occlusion_next_slot_ = 0;
  uint32_t occlusion_slot_ = 0;
  bool occlusion_open_ = false;
  std::vector<uint32_t> occlusion_interval_;
  uint64_t occlusion_counter_ = 0;

  static constexpr uint32_t kSharedOutputs = 3;
  struct SharedOutput {
    ComPtr<ID3D12Resource> texture;
    HANDLE handle = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = {};
  };
  SharedOutput shared_outputs_[kSharedOutputs];
  ComPtr<ID3D12Fence> shared_fence_;
  HANDLE shared_fence_handle_ = nullptr;
  uint64_t shared_fence_value_ = 0;
  uint32_t shared_next_ = 0, shared_width_ = 0, shared_height_ = 0;
  std::mutex shared_mutex_;
  uint32_t shared_latest_ = 0;
  uint64_t shared_latest_value_ = 0;
  std::atomic<bool> shared_frame_ready_{false};
  uint32_t window_width_ = 0, window_height_ = 0;

  RendererStats stats_;
};

}  // namespace replay
