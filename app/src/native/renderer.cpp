#include "renderer.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <thread>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <d3dcompiler.h>
#include <wincodec.h>

#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>

#include "guest.h"

using namespace rex::graphics;

namespace replay {

namespace {

constexpr uint64_t kSharedMemorySize = 0x20000000;
constexpr uint64_t kUploadBufferSize = 256ull << 20;
constexpr uint32_t kViewHeapSize = 500000;
constexpr uint32_t kSamplerHeapSize = 2048;
constexpr uint32_t kStagingHeapSize = 65536;
constexpr uint32_t kRenderTargetHeight = 2048;

// Root parameters, as in D3D12CommandProcessor (bindful).
enum : uint32_t {
  kRootFetchConstants,
  kRootFloatConstantsVertex,
  kRootFloatConstantsPixel,
  kRootSystemConstants,
  kRootBoolLoopConstants,
  kRootSharedMemory,
  kRootBaseCount,
};

D3D12_BLEND BlendFactor(uint32_t f, bool alpha) {
  switch (xenos::BlendFactor(f)) {
    case xenos::BlendFactor::kZero:
      return D3D12_BLEND_ZERO;
    case xenos::BlendFactor::kOne:
      return D3D12_BLEND_ONE;
    case xenos::BlendFactor::kSrcColor:
      return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
    case xenos::BlendFactor::kOneMinusSrcColor:
      return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
    case xenos::BlendFactor::kSrcAlpha:
      return D3D12_BLEND_SRC_ALPHA;
    case xenos::BlendFactor::kOneMinusSrcAlpha:
      return D3D12_BLEND_INV_SRC_ALPHA;
    case xenos::BlendFactor::kDstColor:
      return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
    case xenos::BlendFactor::kOneMinusDstColor:
      return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
    case xenos::BlendFactor::kDstAlpha:
      return D3D12_BLEND_DEST_ALPHA;
    case xenos::BlendFactor::kOneMinusDstAlpha:
      return D3D12_BLEND_INV_DEST_ALPHA;
    case xenos::BlendFactor::kConstantColor:
    case xenos::BlendFactor::kConstantAlpha:
      return D3D12_BLEND_BLEND_FACTOR;
    case xenos::BlendFactor::kOneMinusConstantColor:
    case xenos::BlendFactor::kOneMinusConstantAlpha:
      return D3D12_BLEND_INV_BLEND_FACTOR;
    case xenos::BlendFactor::kSrcAlphaSaturate:
      return D3D12_BLEND_SRC_ALPHA_SAT;
    default:
      return D3D12_BLEND_ZERO;
  }
}

D3D12_BLEND_OP BlendOp(uint32_t op) {
  static const D3D12_BLEND_OP kOps[8] = {D3D12_BLEND_OP_ADD, D3D12_BLEND_OP_SUBTRACT,
                                         D3D12_BLEND_OP_MIN, D3D12_BLEND_OP_MAX,
                                         D3D12_BLEND_OP_REV_SUBTRACT, D3D12_BLEND_OP_ADD,
                                         D3D12_BLEND_OP_ADD, D3D12_BLEND_OP_ADD};
  return kOps[op & 7];
}

DXGI_FORMAT ColorFormat(uint32_t format) {
  switch (xenos::ColorRenderTargetFormat(format)) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      return DXGI_FORMAT_R10G10B10A2_UNORM;
    case xenos::ColorRenderTargetFormat::k_16_16:
      return DXGI_FORMAT_R16G16_SNORM;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      return DXGI_FORMAT_R16G16B16A16_SNORM;
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return DXGI_FORMAT_R16G16_FLOAT;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return DXGI_FORMAT_R32_FLOAT;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return DXGI_FORMAT_R32G32_FLOAT;
    default:
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
  }
}

// Host format for a guest texture format (UNKNOWN = unsupported), and which
// host component each guest component comes from (the SDK's host swizzles:
// single-channel formats replicate red, two-channel ones green).
constexpr uint32_t kSwizzleRGBA = 0 | (1 << 3) | (2 << 6) | (3 << 9);
constexpr uint32_t kSwizzleRRRR = 0;
constexpr uint32_t kSwizzleRGGG = 0 | (1 << 3) | (1 << 6) | (1 << 9);
constexpr uint32_t kSwizzleRGBB = 0 | (1 << 3) | (2 << 6) | (2 << 9);
struct TextureFormatInfo {
  DXGI_FORMAT format;
  bool block_compressed;
  uint32_t host_swizzle = kSwizzleRGBA;
};
TextureFormatInfo TextureFormat(xenos::TextureFormat f) {
  switch (f) {
    case xenos::TextureFormat::k_DXT1:
      return {DXGI_FORMAT_BC1_UNORM, true};
    case xenos::TextureFormat::k_DXT2_3:
      return {DXGI_FORMAT_BC2_UNORM, true};
    case xenos::TextureFormat::k_DXT4_5:
      return {DXGI_FORMAT_BC3_UNORM, true};
    case xenos::TextureFormat::k_8_8_8_8:
      return {DXGI_FORMAT_R8G8B8A8_UNORM, false};
    case xenos::TextureFormat::k_8:
      return {DXGI_FORMAT_R8_UNORM, false, kSwizzleRRRR};
    case xenos::TextureFormat::k_8_8:
      return {DXGI_FORMAT_R8G8_UNORM, false, kSwizzleRGGG};
    case xenos::TextureFormat::k_5_6_5:
      return {DXGI_FORMAT_B5G6R5_UNORM, false, kSwizzleRGBB};
    case xenos::TextureFormat::k_1_5_5_5:
      return {DXGI_FORMAT_B5G5R5A1_UNORM, false};
    case xenos::TextureFormat::k_4_4_4_4:
      return {DXGI_FORMAT_B4G4R4A4_UNORM, false};
    case xenos::TextureFormat::k_2_10_10_10:
      return {DXGI_FORMAT_R10G10B10A2_UNORM, false};
    case xenos::TextureFormat::k_16_16:
      return {DXGI_FORMAT_R16G16_UNORM, false, kSwizzleRGGG};
    case xenos::TextureFormat::k_16_16_16_16:
      return {DXGI_FORMAT_R16G16B16A16_UNORM, false};
    case xenos::TextureFormat::k_16_FLOAT:
      return {DXGI_FORMAT_R16_FLOAT, false, kSwizzleRRRR};
    case xenos::TextureFormat::k_16_16_FLOAT:
      return {DXGI_FORMAT_R16G16_FLOAT, false, kSwizzleRGGG};
    case xenos::TextureFormat::k_16_16_16_16_FLOAT:
      return {DXGI_FORMAT_R16G16B16A16_FLOAT, false};
    case xenos::TextureFormat::k_32_FLOAT:
      return {DXGI_FORMAT_R32_FLOAT, false, kSwizzleRRRR};
    default:
      return {DXGI_FORMAT_UNKNOWN, false};
  }
}

// Eight bytes per step (a byte per step, FNV, was the renderer thread's
// biggest cost: the pipeline key of every draw, the views of every texture,
// the memory samples of every texture; measured 14 + 7 + 7 %).
inline uint64_t HashMix(uint64_t h, uint64_t w) {
  h ^= w;
  h *= 0x9E3779B97F4A7C15ull;
  return h ^ (h >> 29);
}
inline uint64_t HashFinish(uint64_t h) {
  h ^= h >> 33;
  h *= 0xFF51AFD7ED558CCDull;
  return h ^ (h >> 33);
}

uint64_t HashBytes(const void* data, size_t size) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint64_t h = 1469598103934665603ull ^ size;
  size_t i = 0;
  for (; i + 8 <= size; i += 8) {
    uint64_t w;
    std::memcpy(&w, p + i, 8);
    h = HashMix(h, w);
  }
  if (i < size) {
    uint64_t w = 0;
    std::memcpy(&w, p + i, size - i);
    h = HashMix(h, w);
  }
  return HashFinish(h);
}

// Hash of 16 bytes of every 512 of a guest memory range (cheap enough to check
// every texture of a frame), at most 256 samples: each one is a cache miss,
// and a large texture (up to 8192 samples) cost the most. Another texture
// streamed into the memory changes nearly every byte, so 256 still see it.
uint64_t SampledHash(const uint8_t* memory, uint32_t address, uint32_t size) {
  uint64_t h = 1469598103934665603ull;
  const uint64_t end = std::min<uint64_t>(uint64_t(address) + size, 0x20000000);
  uint64_t stride = 512;
  while (size / stride > 256) {
    stride *= 2;
  }
  for (uint64_t at = address; at + 16 <= end; at += stride) {
    uint64_t w[2];
    std::memcpy(w, memory + at, 16);
    h = HashMix(HashMix(h, w[0]), w[1]);
  }
  return HashFinish(h);
}

void SwapCopy(uint8_t* dest, const uint8_t* src, uint32_t size, xenos::Endian endian) {
  switch (endian) {
    case xenos::Endian::k8in16:
      for (uint32_t i = 0; i + 1 < size; i += 2) {
        dest[i] = src[i + 1];
        dest[i + 1] = src[i];
      }
      break;
    case xenos::Endian::k8in32:
      for (uint32_t i = 0; i + 3 < size; i += 4) {
        dest[i] = src[i + 3];
        dest[i + 1] = src[i + 2];
        dest[i + 2] = src[i + 1];
        dest[i + 3] = src[i];
      }
      break;
    case xenos::Endian::k16in32:
      for (uint32_t i = 0; i + 3 < size; i += 4) {
        dest[i] = src[i + 2];
        dest[i + 1] = src[i + 3];
        dest[i + 2] = src[i];
        dest[i + 3] = src[i + 1];
      }
      break;
    default:
      std::memcpy(dest, src, size);
  }
}

D3D12_TEXTURE_ADDRESS_MODE AddressMode(xenos::ClampMode mode) {
  switch (mode) {
    case xenos::ClampMode::kRepeat:
      return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case xenos::ClampMode::kMirroredRepeat:
      return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case xenos::ClampMode::kClampToEdge:
    case xenos::ClampMode::kClampToHalfway:
      return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case xenos::ClampMode::kMirrorClampToEdge:
    case xenos::ClampMode::kMirrorClampToHalfway:
      return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
    default:
      return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
  }
}

const char kResolveShaders[] = R"(
#if MSAA
Texture2DMS<float4> source : register(t0);
#else
Texture2D<float4> source : register(t0);
#endif
cbuffer Constants : register(b0) {
  int2 source_offset;  // source pixel = dest pixel + source_offset
  uint sample_select;  // xenos::CopySampleSelect
  uint flags;          // 1 = swap red and blue; bits 8-15: the source's samples
};
float4 Load(int2 p, int s) {
#if MSAA
  return source.Load(p, s);
#else
  return source.Load(int3(p, 0));
#endif
}
float4 VSMain(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
float4 PSMain(float4 position : SV_Position) : SV_Target {
  int2 p = int2(position.xy) + source_offset;
  float4 c;
#if MSAA
  // The game's 4 samples, or the sample count chosen instead (MSAA setting):
  // then a single sample is the nearest one, a pair or all four the average.
  uint n = max((flags >> 8) & 0xFF, 1u);
  if (sample_select <= 3) {
    c = Load(p, min(sample_select, n - 1));
  } else if (n == 4 && sample_select == 4) {
    c = 0.5 * (Load(p, 0) + Load(p, 1));
  } else if (n == 4 && sample_select == 5) {
    c = 0.5 * (Load(p, 2) + Load(p, 3));
  } else {
    c = 0;
    for (uint i = 0; i < n; ++i) {
      c += Load(p, i);
    }
    c /= n;
  }
#else
  c = Load(p, 0);
#endif
  if (flags & 1) {
    c = c.bgra;
  }
  return c;
}
)";

}  // namespace


struct Renderer::Shader {
  std::unique_ptr<DxbcShader> shader;
};

struct Renderer::RenderTarget {
  ComPtr<ID3D12Resource> resource;
  D3D12_RESOURCE_STATES state;
  D3D12_CPU_DESCRIPTOR_HANDLE view;  // RTV or DSV
  DXGI_FORMAT view_format;           // RTV / DSV format
  DXGI_FORMAT srv_format;            // for resolving
  uint32_t width, height, samples;
  bool depth;
};

struct Renderer::HostTexture {
  ComPtr<ID3D12Resource> resource;
  D3D12_RESOURCE_STATES state;
  DXGI_FORMAT format;
  uint32_t width, height, array_size, mips;
  bool is_3d = false;  // array_size is the depth then
  uint32_t host_swizzle = kSwizzleRGBA;
  std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtvs;  // resolve destinations, per slice
  // Guest textures in the running game: the memory they were loaded from and
  // a sampled hash of it, compared once per frame.
  uint32_t check_address = 0, check_size = 0;
  uint64_t check_hash = 0, checked_frame = 0;
  // A changed memory content seen at the last check, not taken yet: only once
  // the same content is seen at a later frame (see GetTexture).
  uint64_t changed_hash = 0;
  // Drawn at the renderer's scale (resolve destinations): shaders get their
  // unnormalized coordinates and sizes scaled.
  bool scaled = false;
  // Resolve destinations: their fetch constant (guest layout) and the frame
  // they were last written in.
  uint32_t guest_fetch[6] = {};
  uint64_t resolved_frame = 0;
  ComPtr<ID3D12Resource> readback;  // for WriteBackSmallResolves
};

struct Renderer::Pipeline {
  ComPtr<ID3D12PipelineState> state;
  // 0 being created (in the background), 1 ready, 2 failed.
  std::atomic<int> status{0};
  double compile_ms = 0;
  bool counted = false;  // in the stats yet
};

Renderer::Renderer(uint32_t scale)
    : scale_(std::clamp<uint32_t>(scale, 1, 4)),
      translator_(rex::ui::GraphicsProvider::GpuVendorID::kNvidia, false, false, false, true,
                  std::clamp<uint32_t>(scale, 1, 4), std::clamp<uint32_t>(scale, 1, 4)) {}

Renderer::~Renderer() {
  {
    std::lock_guard<std::mutex> lock(pipeline_jobs_mutex_);
    pipeline_workers_stop_ = true;
  }
  pipeline_jobs_cv_.notify_all();
  for (std::thread& worker : pipeline_workers_) {
    worker.join();
  }
  if (fence_event_) {
    CloseHandle(fence_event_);
  }
}

bool Renderer::Initialize() {
  if (std::getenv("REPLAY_DEBUG")) {
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
      debug->EnableDebugLayer();
    }
  }
  if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_)))) {
    return false;
  }
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT i = 0; factory_->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 desc;
    adapter->GetDesc1(&desc);
    if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
      continue;
    }
    if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)))) {
      std::wprintf(L"GPU: %s\n", desc.Description);
      break;
    }
  }
  if (!device_) {
    std::fprintf(stderr, "no Direct3D 12 device\n");
    return false;
  }
  D3D12_COMMAND_QUEUE_DESC queue_desc = {};
  queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue_));
  for (FrameSet& set : sets_) {
    device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                    IID_PPV_ARGS(&set.allocator));
  }
  allocator_ = sets_[0].allocator;
  device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(), nullptr,
                             IID_PPV_ARGS(&list_));
  list_open_ = true;
  device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
  fence_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);

  auto heap = [&](D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count, bool visible,
                  ComPtr<ID3D12DescriptorHeap>& out) {
    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.Type = type;
    desc.NumDescriptors = count;
    desc.Flags = visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    return SUCCEEDED(device_->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&out)));
  };
  if (!heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kViewHeapSize, true, view_heap_) ||
      !heap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerHeapSize, true, sampler_heap_) ||
      !heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kStagingHeapSize, false, staging_heap_) ||
      !heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 4096, false, rtv_heap_) ||
      !heap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1024, false, dsv_heap_)) {
    return false;
  }
  view_increment_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  sampler_increment_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  rtv_increment_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  dsv_increment_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

  // Guest memory buffer.
  D3D12_HEAP_PROPERTIES default_heap = {D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC buffer_desc = {};
  buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer_desc.Width = kSharedMemorySize;
  buffer_desc.Height = 1;
  buffer_desc.DepthOrArraySize = 1;
  buffer_desc.MipLevels = 1;
  buffer_desc.SampleDesc.Count = 1;
  buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(device_->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&shared_memory_)))) {
    std::fprintf(stderr, "cannot create the 512 MB guest memory buffer\n");
    return false;
  }
  shared_memory_state_ = D3D12_RESOURCE_STATE_COPY_DEST;

  // Descriptors 0 and 1 of the shader-visible heap: guest memory as a raw SRV
  // (t0) and a null raw UAV (u0), always.
  {
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = view_heap_->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = DXGI_FORMAT_R32_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements = UINT(kSharedMemorySize / 4);
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    device_->CreateShaderResourceView(shared_memory_.Get(), &srv, cpu);
    cpu.ptr += view_increment_;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = 1;
    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device_->CreateUnorderedAccessView(nullptr, nullptr, &uav, cpu);
    shared_memory_table_ = view_heap_->GetGPUDescriptorHandleForHeapStart();
    view_heap_used_ = 2;
  }
  // Each resource set gets half of the shader-visible descriptors.
  view_heap_begin_ = 2;
  view_heap_end_ = 2 + (kViewHeapSize - 2) / 2;
  sampler_heap_begin_ = 0;
  sampler_heap_end_ = kSamplerHeapSize / 2;
  // Null texture views.
  {
    auto staging = [&]() {
      D3D12_CPU_DESCRIPTOR_HANDLE h = staging_heap_->GetCPUDescriptorHandleForHeapStart();
      h.ptr += size_t(staging_used_++) * view_increment_;
      return h;
    };
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srv.Texture2DArray.MipLevels = 1;
    srv.Texture2DArray.ArraySize = 1;
    null_srv_2d_array_ = staging();
    device_->CreateShaderResourceView(nullptr, &srv, null_srv_2d_array_);
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    srv.Texture3D.MipLevels = 1;
    null_srv_3d_ = staging();
    device_->CreateShaderResourceView(nullptr, &srv, null_srv_3d_);
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srv.TextureCube.MipLevels = 1;
    null_srv_cube_ = staging();
    device_->CreateShaderResourceView(nullptr, &srv, null_srv_cube_);
  }
  BeginList();
  return CreateResolvePipelines();
}

bool Renderer::CreateResolvePipelines() {
  // Root signature: b0 = 4 constants, t0 = source.
  D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants.Num32BitValues = 4;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable.NumDescriptorRanges = 1;
  params[1].DescriptorTable.pDescriptorRanges = &range;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC desc = {2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ComPtr<ID3DBlob> blob, error;
  if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
      FAILED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                          IID_PPV_ARGS(&resolve_root_signature_)))) {
    return false;
  }
  for (int msaa = 0; msaa < 2; ++msaa) {
    D3D_SHADER_MACRO defines[] = {{"MSAA", msaa ? "1" : "0"}, {nullptr, nullptr}};
    ComPtr<ID3DBlob> vs, ps;
    if (FAILED(D3DCompile(kResolveShaders, sizeof(kResolveShaders) - 1, "resolve", defines,
                          nullptr, "VSMain", "vs_5_0", 0, 0, &vs, &error)) ||
        FAILED(D3DCompile(kResolveShaders, sizeof(kResolveShaders) - 1, "resolve", defines,
                          nullptr, "PSMain", "ps_5_0", 0, 0, &ps, &error))) {
      std::fprintf(stderr, "resolve shader: %s\n",
                   error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
      return false;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = resolve_root_signature_.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.SampleDesc.Count = 1;
    const DXGI_FORMAT formats[2] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R32_FLOAT};
    for (int f = 0; f < 2; ++f) {
      pso.RTVFormats[0] = formats[f];
      if (FAILED(device_->CreateGraphicsPipelineState(&pso,
                                                      IID_PPV_ARGS(&resolve_color_[msaa][f])))) {
        return false;
      }
    }
  }
  return true;
}

void Renderer::Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES& current,
                          D3D12_RESOURCE_STATES next) {
  if (current == next) {
    return;
  }
  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = current;
  barrier.Transition.StateAfter = next;
  list_->ResourceBarrier(1, &barrier);
  current = next;
}

uint8_t* Renderer::AllocateUpload(uint32_t size, uint32_t alignment,
                                  D3D12_GPU_VIRTUAL_ADDRESS& gpu, ID3D12Resource** buffer,
                                  uint64_t* offset) {
  uint64_t aligned = (upload_offset_ + alignment - 1) & ~uint64_t(alignment - 1);
  if (upload_buffers_.empty() || aligned + size > upload_size_) {
    D3D12_HEAP_PROPERTIES upload_heap = {D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = std::max<uint64_t>(kUploadBufferSize, size);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> resource;
    if (FAILED(device_->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                IID_PPV_ARGS(&resource)))) {
      std::fprintf(stderr, "upload buffer allocation failed\n");
      std::exit(1);
    }
    D3D12_RANGE none = {0, 0};
    void* mapping;
    resource->Map(0, &none, &mapping);
    upload_mapping_ = static_cast<uint8_t*>(mapping);
    upload_size_ = desc.Width;
    upload_buffers_.push_back(resource);
    aligned = 0;
  }
  upload_offset_ = aligned + size;
  gpu = upload_buffers_.back()->GetGPUVirtualAddress() + aligned;
  if (buffer) {
    *buffer = upload_buffers_.back().Get();
  }
  if (offset) {
    *offset = aligned;
  }
  return upload_mapping_ + aligned;
}

D3D12_GPU_DESCRIPTOR_HANDLE Renderer::AllocateViews(uint32_t count,
                                                    D3D12_CPU_DESCRIPTOR_HANDLE& cpu) {
  cpu = view_heap_->GetCPUDescriptorHandleForHeapStart();
  cpu.ptr += size_t(view_heap_used_) * view_increment_;
  D3D12_GPU_DESCRIPTOR_HANDLE gpu = view_heap_->GetGPUDescriptorHandleForHeapStart();
  gpu.ptr += uint64_t(view_heap_used_) * view_increment_;
  view_heap_used_ += count;
  return gpu;
}

bool Renderer::BeginList() {
  InvalidateBound();
  if (!list_open_) {
    allocator_->Reset();
    list_->Reset(allocator_.Get(), nullptr);
    list_open_ = true;
  }
  ID3D12DescriptorHeap* heaps[] = {view_heap_.Get(), sampler_heap_.Get()};
  list_->SetDescriptorHeaps(2, heaps);
  return true;
}

void Renderer::ActivateSet(uint32_t index) {
  set_ = index;
  FrameSet& set = sets_[index];
  if (set.fence_value && fence_->GetCompletedValue() < set.fence_value) {
    fence_->SetEventOnCompletion(set.fence_value, fence_event_);
    WaitForSingleObject(fence_event_, INFINITE);
  }
  for (auto& work : set.after_completion) {
    work();
  }
  set.after_completion.clear();
  set.release.clear();
  // Its upload memory is free again (one buffer kept).
  upload_buffers_ = std::move(set.upload_buffers);
  if (upload_buffers_.size() > 1) {
    upload_buffers_.erase(upload_buffers_.begin(), upload_buffers_.end() - 1);
  }
  upload_mapping_ = set.upload_mapping;
  upload_size_ = set.upload_size;
  upload_offset_ = 0;
  allocator_ = set.allocator;
  const uint32_t view_half = (kViewHeapSize - 2) / 2;
  view_heap_begin_ = 2 + index * view_half;
  view_heap_end_ = view_heap_begin_ + view_half;
  view_heap_used_ = view_heap_begin_;
  sampler_heap_begin_ = index * (kSamplerHeapSize / 2);
  sampler_heap_end_ = sampler_heap_begin_ + kSamplerHeapSize / 2;
  sampler_heap_used_ = sampler_heap_begin_;
  sampler_ranges_.clear();
  texture_ranges_.clear();
}

bool Renderer::Submit() { return Submit(false); }

bool Renderer::Submit(bool wait_for_all) {
  // An occlusion query cannot span command lists: end it here, begin a new
  // one for the same interval in the next list.
  const bool reopen_occlusion = occlusion_open_;
  OcclusionEnd();
  if (list_open_) {
    list_->Close();
    ID3D12CommandList* lists[] = {list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    list_open_ = false;
  }
  queue_->Signal(fence_.Get(), ++fence_value_);
  FrameSet& done = sets_[set_];
  done.fence_value = fence_value_;
  done.upload_buffers = std::move(upload_buffers_);
  done.upload_mapping = upload_mapping_;
  done.upload_size = upload_size_;
  for (auto& resource : release_after_flush_) {
    done.release.push_back(std::move(resource));
  }
  release_after_flush_.clear();
  upload_buffers_.clear();
  const auto wait_start = std::chrono::steady_clock::now();
  if (wait_for_all && fence_->GetCompletedValue() < fence_value_) {
    fence_->SetEventOnCompletion(fence_value_, fence_event_);
    WaitForSingleObject(fence_event_, INFINITE);
  }
  ActivateSet(set_ ^ 1);
  stats_.flush_ms += std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - wait_start).count();
  HRESULT removed = device_->GetDeviceRemovedReason();
  if (FAILED(removed)) {
    std::fprintf(stderr, "device removed: %08X\n", unsigned(removed));
    return false;
  }
  BeginList();
  if (reopen_occlusion) {
    OcclusionBegin();
  }
  return true;
}

bool Renderer::Flush() { return Submit(true); }

bool Renderer::UploadMemory(const uint8_t* memory) {
  guest_memory_ = memory;
  // In 128 MB pieces through the upload buffer.
  constexpr uint32_t kPiece = 128u << 20;
  for (uint64_t offset = 0; offset < kSharedMemorySize; offset += kPiece) {
    D3D12_GPU_VIRTUAL_ADDRESS gpu;
    ID3D12Resource* buffer;
    uint64_t buffer_offset;
    uint8_t* data = AllocateUpload(kPiece, 256, gpu, &buffer, &buffer_offset);
    std::memcpy(data, memory + offset, kPiece);
    Transition(shared_memory_.Get(), shared_memory_state_, D3D12_RESOURCE_STATE_COPY_DEST);
    list_->CopyBufferRegion(shared_memory_.Get(), offset, buffer, buffer_offset, kPiece);
    if (!Flush()) {
      return false;
    }
  }
  return true;
}

void Renderer::UpdateMemory(uint32_t address, const uint8_t* data, uint32_t size) {
  if (!size || address >= kSharedMemorySize) {
    return;
  }
  size = uint32_t(std::min<uint64_t>(size, kSharedMemorySize - address));
  D3D12_GPU_VIRTUAL_ADDRESS gpu;
  ID3D12Resource* buffer;
  uint64_t buffer_offset;
  uint8_t* upload = AllocateUpload(size, 16, gpu, &buffer, &buffer_offset);
  std::memcpy(upload, data, size);
  Transition(shared_memory_.Get(), shared_memory_state_, D3D12_RESOURCE_STATE_COPY_DEST);
  list_->CopyBufferRegion(shared_memory_.Get(), address, buffer, buffer_offset, size);
}

namespace {
// The analysis of microcode that is not a valid program can also fault (its
// disassembly reads null names); no C++ objects here, so SEH can guard it.
bool AnalyzeUcodeGuarded(DxbcShader* shader, rex::string::StringBuffer& disasm) {
  __try {
    shader->AnalyzeUcode(disasm);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
}  // namespace

Renderer::Shader* Renderer::GetShader(const uint8_t* code, uint32_t dwords, uint32_t address,
                                      bool pixel, uint64_t hash) {
  // By content: the same address holds different (patched) programs.
  if (!hash) {
    hash = HashBytes(code, size_t(dwords) * 4);
  }
  const uint64_t key = (hash << 1) | uint64_t(pixel);
  auto it = shaders_.find(key);
  if (it != shaders_.end()) {
    return it->second.get();
  }
  auto shader = std::make_unique<Shader>();
  shader->shader = std::make_unique<DxbcShader>(
      pixel ? xenos::ShaderType::kPixel : xenos::ShaderType::kVertex, key,
      reinterpret_cast<const uint32_t*>(code), dwords);
  // Microcode the analysis cannot take (its disassembly throws or faults on
  // fields it does not know) is remembered as unusable; those draws are
  // skipped.
  if (!AnalyzeUcodeGuarded(shader->shader.get(), disasm_)) {
    ++stats_.shader_failures;
    last_shader_failure_ = {address, dwords, pixel};
    std::memcpy(last_shader_failure_.first, code, std::min<size_t>(sizeof(last_shader_failure_.first), size_t(dwords) * 4));
    // REPLAY_BAD_SHADERS=<directory>: keep their microcode for a look.
    if (const char* directory = std::getenv("REPLAY_BAD_SHADERS")) {
      char path[512];
      std::snprintf(path, sizeof(path), "%s/bad_%s_%016llx.bin", directory, pixel ? "ps" : "vs",
                    (unsigned long long)hash);
      if (FILE* out = std::fopen(path, "wb")) {
        std::fwrite(code, 4, dwords, out);
        std::fclose(out);
      }
    }
    shaders_.emplace(key, nullptr);
    return nullptr;
  }
  // REPLAY_DISASM=<hex address>: print that shader's microcode.
  if (const char* want = std::getenv("REPLAY_DISASM")) {
    if (std::strtoul(want, nullptr, 16) == address) {
      std::printf("%s", shader->shader->ucode_disassembly().c_str());
    }
  }
  Shader* result = shader.get();
  shaders_.emplace(key, std::move(shader));
  return result;
}

uint32_t Renderer::HostSamples(uint32_t guest_msaa) {
  if (!guest_msaa || msaa_override_ < 0) {
    return 1u << guest_msaa;
  }
  const uint32_t wanted = 1u << std::min<int32_t>(msaa_override_, 3);
  if (!msaa_supported_) {
    // The highest count up to the wanted one that every format the game
    // draws to supports (the pipelines need one count for all targets).
    const DXGI_FORMAT formats[] = {
        DXGI_FORMAT_R8G8B8A8_UNORM,     DXGI_FORMAT_R10G10B10A2_UNORM,
        DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16_FLOAT,
        DXGI_FORMAT_R32_FLOAT,          DXGI_FORMAT_D24_UNORM_S8_UINT,
        DXGI_FORMAT_D32_FLOAT_S8X24_UINT};
    msaa_supported_ = 1;
    for (uint32_t count = 2; count <= 8; count *= 2) {
      bool all = true;
      for (DXGI_FORMAT format : formats) {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels = {};
        levels.Format = format;
        levels.SampleCount = count;
        if (FAILED(device_->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &levels,
                                                sizeof(levels))) ||
            !levels.NumQualityLevels) {
          all = false;
          break;
        }
      }
      if (all) {
        msaa_supported_ = count;
      }
    }
  }
  stats_.msaa_samples = std::min(wanted, msaa_supported_);
  return stats_.msaa_samples;
}

Renderer::RenderTarget* Renderer::GetRenderTarget(uint32_t edram_base, uint32_t pitch,
                                                  uint32_t msaa, uint32_t format, bool depth) {
  const uint64_t key = (uint64_t(edram_base) << 32) | (uint64_t(pitch) << 8) |
                       (uint64_t(msaa) << 6) | (uint64_t(format) << 1) | uint64_t(depth);
  auto it = render_targets_.find(key);
  if (it != render_targets_.end()) {
    return it->second.get();
  }
  auto rt = std::make_unique<RenderTarget>();
  rt->width = std::max<uint32_t>(pitch, 1) * scale_;
  rt->height = kRenderTargetHeight * scale_;
  rt->samples = HostSamples(msaa);
  rt->depth = depth;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = rt->width;
  desc.Height = rt->height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = rt->samples;
  D3D12_CLEAR_VALUE clear = {};
  if (depth) {
    const bool float24 = format == uint32_t(xenos::DepthRenderTargetFormat::kD24FS8);
    desc.Format = float24 ? DXGI_FORMAT_R32G8X24_TYPELESS : DXGI_FORMAT_R24G8_TYPELESS;
    rt->view_format = float24 ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D24_UNORM_S8_UINT;
    rt->srv_format = float24 ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS
                             : DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    clear.Format = rt->view_format;
    clear.DepthStencil.Depth = 1.0f;
    rt->state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
  } else {
    desc.Format = ColorFormat(format);
    rt->view_format = rt->srv_format = desc.Format;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    clear.Format = desc.Format;
    rt->state = D3D12_RESOURCE_STATE_RENDER_TARGET;
  }
  D3D12_HEAP_PROPERTIES default_heap = {D3D12_HEAP_TYPE_DEFAULT};
  if (FAILED(device_->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                              rt->state, &clear, IID_PPV_ARGS(&rt->resource)))) {
    std::fprintf(stderr, "render target %ux%u x%u failed\n", rt->width, rt->height, rt->samples);
    return nullptr;
  }
  if (depth) {
    rt->view = dsv_heap_->GetCPUDescriptorHandleForHeapStart();
    rt->view.ptr += size_t(dsv_used_++) * dsv_increment_;
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
    dsv.Format = rt->view_format;
    dsv.ViewDimension = rt->samples > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS
                                        : D3D12_DSV_DIMENSION_TEXTURE2D;
    device_->CreateDepthStencilView(rt->resource.Get(), &dsv, rt->view);
    list_->ClearDepthStencilView(rt->view, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                 1.0f, 0, 0, nullptr);
  } else {
    rt->view = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    rt->view.ptr += size_t(rtv_used_++) * rtv_increment_;
    device_->CreateRenderTargetView(rt->resource.Get(), nullptr, rt->view);
    const float zero[4] = {};
    list_->ClearRenderTargetView(rt->view, zero, 0, nullptr);
  }
  RenderTarget* result = rt.get();
  render_targets_.emplace(key, std::move(rt));
  return result;
}

ID3D12RootSignature* Renderer::GetRootSignature(uint32_t vs_textures, uint32_t vs_samplers,
                                                uint32_t ps_textures, uint32_t ps_samplers) {
  const uint32_t key = ps_textures | (ps_samplers << 8) | (vs_textures << 16) | (vs_samplers << 24);
  auto it = root_signatures_.find(key);
  if (it != root_signatures_.end()) {
    return it->second.Get();
  }
  D3D12_ROOT_PARAMETER params[kRootBaseCount + 4] = {};
  auto cbv = [&](uint32_t index, DxbcShaderTranslator::CbufferRegister reg,
                 D3D12_SHADER_VISIBILITY visibility) {
    params[index].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[index].Descriptor.ShaderRegister = uint32_t(reg);
    params[index].ShaderVisibility = visibility;
  };
  cbv(kRootFetchConstants, DxbcShaderTranslator::CbufferRegister::kFetchConstants,
      D3D12_SHADER_VISIBILITY_ALL);
  cbv(kRootFloatConstantsVertex, DxbcShaderTranslator::CbufferRegister::kFloatConstants,
      D3D12_SHADER_VISIBILITY_VERTEX);
  cbv(kRootFloatConstantsPixel, DxbcShaderTranslator::CbufferRegister::kFloatConstants,
      D3D12_SHADER_VISIBILITY_PIXEL);
  cbv(kRootSystemConstants, DxbcShaderTranslator::CbufferRegister::kSystemConstants,
      D3D12_SHADER_VISIBILITY_ALL);
  cbv(kRootBoolLoopConstants, DxbcShaderTranslator::CbufferRegister::kBoolLoopConstants,
      D3D12_SHADER_VISIBILITY_ALL);
  D3D12_DESCRIPTOR_RANGE shared_ranges[2] = {
      {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
      {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1},
  };
  params[kRootSharedMemory].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[kRootSharedMemory].DescriptorTable.NumDescriptorRanges = 2;
  params[kRootSharedMemory].DescriptorTable.pDescriptorRanges = shared_ranges;
  params[kRootSharedMemory].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  uint32_t count = kRootBaseCount;
  D3D12_DESCRIPTOR_RANGE ranges[4];
  auto table = [&](D3D12_DESCRIPTOR_RANGE_TYPE type, uint32_t n, uint32_t base,
                   D3D12_SHADER_VISIBILITY visibility) {
    if (!n) {
      return;
    }
    D3D12_DESCRIPTOR_RANGE& r = ranges[count - kRootBaseCount];
    r = {type, n, base, 0, 0};
    params[count].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[count].DescriptorTable.NumDescriptorRanges = 1;
    params[count].DescriptorTable.pDescriptorRanges = &r;
    params[count].ShaderVisibility = visibility;
    ++count;
  };
  const uint32_t texture_base =
      uint32_t(DxbcShaderTranslator::SRVMainRegister::kBindfulTexturesStart);
  table(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, ps_textures, texture_base, D3D12_SHADER_VISIBILITY_PIXEL);
  table(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, ps_samplers, 0, D3D12_SHADER_VISIBILITY_PIXEL);
  table(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, vs_textures, texture_base, D3D12_SHADER_VISIBILITY_VERTEX);
  table(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, vs_samplers, 0, D3D12_SHADER_VISIBILITY_VERTEX);
  D3D12_ROOT_SIGNATURE_DESC desc = {count, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ComPtr<ID3DBlob> blob, error;
  ComPtr<ID3D12RootSignature> root;
  if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
      FAILED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                          IID_PPV_ARGS(&root)))) {
    std::fprintf(stderr, "root signature failed\n");
    return nullptr;
  }
  root_signatures_.emplace(key, root);
  return root.Get();
}

Renderer::HostTexture* Renderer::LoadGuestTexture(const uint32_t* words) {
  xenos::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, words, sizeof(fetch));
  uint32_t width_minus_1, height_minus_1, depth_minus_1, base_page, mip_page, mip_min, mip_max;
  texture_util::GetSubresourcesFromFetchConstant(fetch, &width_minus_1, &height_minus_1,
                                                 &depth_minus_1, &base_page, &mip_page, &mip_min,
                                                 &mip_max);
  const uint32_t width = width_minus_1 + 1, height = height_minus_1 + 1;
  const uint32_t depth_or_array = depth_minus_1 + 1;
  const xenos::DataDimension dimension = fetch.dimension;
  const xenos::TextureFormat format = fetch.format;
  const FormatInfo* info = FormatInfo::Get(format);
  const TextureFormatInfo host = TextureFormat(format);
  if (host.format == DXGI_FORMAT_UNKNOWN || !info ||
      (host.block_compressed && ((width & 3) || (height & 3))) || !base_page) {
    ++stats_.textures_unsupported;
    std::printf("unsupported texture: format %u, %ux%ux%u, dimension %u, base %08X\n",
                uint32_t(format), width, height, depth_or_array, uint32_t(dimension),
                base_page << 12);
    return nullptr;
  }
  const uint32_t block_width = info->block_width, block_height = info->block_height;
  const uint32_t bytes_per_block = info->bytes_per_block();
  uint32_t bpb_log2 = 0;
  while ((1u << bpb_log2) < bytes_per_block) {
    ++bpb_log2;
  }
  const texture_util::TextureGuestLayout layout = texture_util::GetGuestTextureLayout(
      dimension, fetch.pitch, width, height, depth_or_array, fetch.tiled, format,
      fetch.packed_mips, base_page != 0, mip_max);
  if (live_ && (!Readable(base_page << 12, std::max<uint32_t>(layout.base.level_data_extent_bytes, 1)) ||
                (mip_max && mip_page &&
                 !Readable(mip_page << 12, std::max<uint32_t>(layout.mips_total_extent_bytes, 1))))) {
    ++stats_.textures_unsupported;
    return nullptr;
  }
  const bool is_3d = dimension == xenos::DataDimension::k3D;
  const uint32_t array_size = is_3d ? 1 : depth_or_array;
  const uint32_t depth = is_3d ? depth_or_array : 1;
  const uint32_t levels = mip_max + 1;

  auto texture = std::make_unique<HostTexture>();
  texture->format = host.format;
  texture->width = width;
  texture->height = height;
  texture->array_size = depth_or_array;
  texture->mips = levels;
  texture->is_3d = is_3d;
  texture->host_swizzle = host.host_swizzle;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = is_3d ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = uint16_t(depth_or_array);
  desc.MipLevels = uint16_t(levels);
  desc.Format = host.format;
  desc.SampleDesc.Count = 1;
  D3D12_HEAP_PROPERTIES default_heap = {D3D12_HEAP_TYPE_DEFAULT};
  texture->state = D3D12_RESOURCE_STATE_COPY_DEST;
  if (FAILED(device_->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                              texture->state, nullptr,
                                              IID_PPV_ARGS(&texture->resource)))) {
    ++stats_.textures_unsupported;
    return nullptr;
  }
  const uint32_t subresources = levels * array_size;
  std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresources);
  std::vector<UINT> rows(subresources);
  std::vector<UINT64> row_sizes(subresources);
  UINT64 total = 0;
  device_->GetCopyableFootprints(&desc, 0, subresources, 0, footprints.data(), rows.data(),
                                 row_sizes.data(), &total);
  D3D12_GPU_VIRTUAL_ADDRESS gpu;
  ID3D12Resource* upload_buffer;
  uint64_t upload_offset;
  uint8_t* upload = AllocateUpload(uint32_t(total), D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, gpu,
                                   &upload_buffer, &upload_offset);
  std::memset(upload, 0, size_t(total));
  for (uint32_t level = 0; level < levels; ++level) {
    const uint32_t guest_level = std::min(level, layout.packed_level);
    const texture_util::TextureGuestLayout::Level& level_layout =
        level ? layout.mips[guest_level] : layout.base;
    const uint32_t level_address =
        level ? (mip_page << 12) + layout.mip_offsets_bytes[guest_level] : (base_page << 12);
    uint32_t offset_x = 0, offset_y = 0, offset_z = 0;
    if (level >= layout.packed_level && fetch.packed_mips) {
      texture_util::GetPackedMipOffset(width, height, depth, format, level, offset_x, offset_y,
                                       offset_z);
    }
    const uint32_t level_width = std::max(width >> level, 1u);
    const uint32_t level_height = std::max(height >> level, 1u);
    const uint32_t blocks_x = (level_width + block_width - 1) / block_width;
    const uint32_t blocks_y = (level_height + block_height - 1) / block_height;
    const uint32_t level_depth = std::max(depth >> level, 1u);
    const uint32_t pitch_blocks = level_layout.row_pitch_bytes / bytes_per_block;
    const uint32_t z_stride_rows = level_layout.z_slice_stride_block_rows;
    for (uint32_t slice = 0; slice < array_size; ++slice) {
      const uint32_t slice_address = level_address + slice * level_layout.array_slice_stride_bytes;
      const uint32_t sub = slice * levels + level;
      const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp = footprints[sub];
      for (uint32_t z = 0; z < level_depth; ++z) {
      uint8_t* dest = upload + fp.Offset + size_t(z) * fp.Footprint.RowPitch * rows[sub];
      for (uint32_t y = 0; y < blocks_y && y < rows[sub]; ++y) {
        for (uint32_t x = 0; x < blocks_x; ++x) {
          uint32_t source;
          if (fetch.tiled) {
            source = slice_address +
                     uint32_t(is_3d ? texture_util::GetTiledOffset3D(
                                          int32_t(x + offset_x), int32_t(y + offset_y),
                                          int32_t(z + offset_z), pitch_blocks, z_stride_rows,
                                          bpb_log2)
                                    : texture_util::GetTiledOffset2D(int32_t(x + offset_x),
                                                                     int32_t(y + offset_y),
                                                                     pitch_blocks, bpb_log2));
          } else {
            source = slice_address + (z + offset_z) * z_stride_rows * level_layout.row_pitch_bytes +
                     (y + offset_y) * level_layout.row_pitch_bytes +
                     (x + offset_x) * bytes_per_block;
          }
          if (uint64_t(source) + bytes_per_block > kSharedMemorySize) {
            continue;
          }
          SwapCopy(dest + y * fp.Footprint.RowPitch + x * bytes_per_block, guest_memory_ + source,
                   bytes_per_block, fetch.endianness);
        }
      }
      }
      D3D12_TEXTURE_COPY_LOCATION dst = {};
      dst.pResource = texture->resource.Get();
      dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dst.SubresourceIndex = sub;
      D3D12_TEXTURE_COPY_LOCATION src = {};
      src.pResource = upload_buffer;
      src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      src.PlacedFootprint = fp;
      src.PlacedFootprint.Offset += upload_offset;
      list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
  }
  ++stats_.textures_loaded;
  texture->check_address = base_page << 12;
  texture->check_size = std::max<uint32_t>(layout.base.level_data_extent_bytes, 4096);
  texture->check_hash = SampledHash(guest_memory_, texture->check_address, texture->check_size);
  texture->checked_frame = frame_;
  HostTexture* result = texture.get();
  guest_textures_[HashBytes(words, 6 * sizeof(uint32_t))] = std::move(texture);
  return result;
}

Renderer::HostTexture* Renderer::GetTexture(const uint32_t* words, bool for_cube) {
  (void)for_cube;
  if ((words[0] & 3) != 2) {
    return nullptr;
  }
  const uint32_t base = words[1] & 0xFFFFF000;
  auto resolved = resolved_.find(base);
  if (resolved != resolved_.end()) {
    ++stats_.textures_from_resolves;
    return resolved->second.get();
  }
  // Cache by the fields that define the data (not the sampler fields).
  uint32_t masked[6];
  std::memcpy(masked, words, sizeof(masked));
  masked[0] &= ~(uint32_t(0x1FF) << 10);  // clamp modes
  masked[3] = 0;                          // swizzle, filters
  masked[4] = 0;                          // mip filters, LOD bias
  const uint64_t key = HashBytes(masked, sizeof(masked));
  auto it = guest_textures_.find(key);
  if (it != guest_textures_.end()) {
    HostTexture* texture = it->second.get();
    if (!texture || !live_ || texture->checked_frame == frame_) {
      return texture;
    }
    // The game may have streamed another texture into this memory. The
    // renderer draws a frame or two after the game recorded it, so a change
    // can also be the game already writing the next texture into memory it
    // freed after this frame: loading it then gave a half-written texture for
    // a frame (white or garbage flashes). So a change is only taken once the
    // same content is seen again at a later frame; until then the texture as
    // it was (right for the frame being drawn in the freed-memory case, one
    // frame late in the other).
    texture->checked_frame = frame_;
    const uint64_t hash =
        SampledHash(guest_memory_, texture->check_address, texture->check_size);
    if (hash == texture->check_hash) {
      texture->changed_hash = 0;
      return texture;
    }
    if (hash != texture->changed_hash) {
      texture->changed_hash = hash;
      ++stats_.textures_changes_deferred;
      return texture;
    }
    release_after_flush_.push_back(texture->resource);
    guest_textures_.erase(it);
    ++stats_.textures_reloaded;
    ++stats_.textures_reloaded_total;
  }
  const auto start = std::chrono::steady_clock::now();
  HostTexture* texture = LoadGuestTexture(masked);
  stats_.texture_ms +=
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  if (!texture) {
    guest_textures_.emplace(key, nullptr);
  }
  return texture;
}

namespace {

// Pipeline state that differs between draws, hashed as raw bytes.
struct PipelineKey {
  const void* vertex_translation;
  const void* pixel_translation;
  ID3D12RootSignature* root_signature;
  uint32_t topology_type;
  DXGI_FORMAT rtv_formats[4];
  DXGI_FORMAT dsv_format;
  uint32_t samples;
  uint32_t blend[4];
  uint32_t write_mask[4];
  uint32_t depth_enable, depth_write, depth_func;
  uint32_t stencil_enable, stencil_read_mask, stencil_write_mask, stencil_front, stencil_back;
  uint32_t cull, front_counter_clockwise, depth_clip;
  int32_t depth_bias;
  float depth_bias_slope;
};

// Guest swizzle (fetch constant) through the host swizzle of the format.
UINT ComponentMapping(uint32_t swizzle, uint32_t host_swizzle) {
  uint32_t m[4];
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t s = (swizzle >> (3 * i)) & 7;
    m[i] = s < 4 ? (host_swizzle >> (3 * s)) & 7 : (s == 5 ? 5 : 4);
  }
  return D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(m[0], m[1], m[2], m[3]);
}

}  // namespace

void Renderer::CreatePipeline(Pipeline& pipeline, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc) {
  const auto start = std::chrono::steady_clock::now();
  const HRESULT created =
      device_->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline.state));
  pipeline.compile_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  pipeline.status.store(SUCCEEDED(created) ? 1 : 2, std::memory_order_release);
}

void Renderer::StartPipelineWorkers() {
  if (!pipeline_workers_.empty()) {
    return;
  }
  for (uint32_t i = 0; i < async_pipeline_threads_; ++i) {
    pipeline_workers_.emplace_back([this]() {
      for (;;) {
        PipelineJob job;
        {
          std::unique_lock<std::mutex> lock(pipeline_jobs_mutex_);
          pipeline_jobs_cv_.wait(
              lock, [this]() { return pipeline_workers_stop_ || !pipeline_jobs_.empty(); });
          if (pipeline_workers_stop_) {
            return;
          }
          job = pipeline_jobs_.front();
          pipeline_jobs_.pop_front();
        }
        CreatePipeline(*job.pipeline, job.desc);
      }
    });
  }
}

void Renderer::Draw(const DrawCall& d) {
  using rex::graphics::RegisterFile;
  const RegisterFile& regs = *d.regs;
  // Start a new list when the shader-visible heaps run low (state is set per
  // draw anyway).
  if (view_heap_used_ + 512 > view_heap_end_ || sampler_heap_used_ + 64 > sampler_heap_end_) {
    Flush();
  }
  const xenos::EdramMode mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (mode != xenos::EdramMode::kColorDepth && mode != xenos::EdramMode::kDepthOnly) {
    ++stats_.draws_skipped;
    return;
  }
  const xenos::PrimitiveType prim = xenos::PrimitiveType(d.primitive_type);
  if (prim != xenos::PrimitiveType::kTriangleList && prim != xenos::PrimitiveType::kTriangleFan &&
      prim != xenos::PrimitiveType::kTriangleStrip && prim != xenos::PrimitiveType::kQuadList) {
    ++stats_.draws_skipped;
    return;
  }
  if ((d.indexed && prim != xenos::PrimitiveType::kTriangleList &&
       prim != xenos::PrimitiveType::kTriangleStrip) ||
      !d.vertex_shader_dwords) {
    ++stats_.draws_skipped;
    return;
  }
  Shader* vs =
      GetShader(d.vertex_shader_code, d.vertex_shader_dwords, d.vertex_shader_address, false,
                d.vertex_shader_hash);
  if (!vs) {
    ++stats_.draws_skipped;
    return;
  }
  const bool polygonal = draw_util::IsPrimitivePolygonal(regs);
  if (!draw_util::IsRasterizationPotentiallyDone(regs, polygonal)) {
    ++stats_.draws_skipped;
    return;
  }
  Shader* ps = nullptr;
  if (mode == xenos::EdramMode::kColorDepth && d.pixel_shader_dwords) {
    ps = GetShader(d.pixel_shader_code, d.pixel_shader_dwords, d.pixel_shader_address, true,
                   d.pixel_shader_hash);
    if (!ps) {
      ++stats_.draws_skipped;
      return;
    }
    if (!draw_util::IsPixelShaderNeededWithRasterization(*ps->shader, regs)) {
      ps = nullptr;
    }
  }
  const reg::RB_DEPTHCONTROL depth_control = draw_util::GetNormalizedDepthControl(regs);
  const auto sq_program_cntl = regs.Get<reg::SQ_PROGRAM_CNTL>();
  const auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();

  // Shader modifications (PipelineCache::GetCurrent*ShaderModification).
  uint32_t param_gen_pos = UINT32_MAX;
  const uint32_t interpolator_mask =
      ps ? vs->shader->writes_interpolators() &
               ps->shader->GetInterpolatorInputMask(sq_program_cntl,
                                                    regs.Get<reg::SQ_CONTEXT_MISC>(), param_gen_pos)
         : 0;
  DxbcShaderTranslator::Modification vertex_mod(translator_.GetDefaultVertexShaderModification(
      vs->shader->GetDynamicAddressableRegisterCount(sq_program_cntl.vs_num_reg),
      rex::graphics::Shader::HostVertexShaderType::kVertex));
  vertex_mod.vertex.interpolator_mask = interpolator_mask;
  const auto clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  const uint32_t user_clip_planes = clip_cntl.clip_disable ? 0 : clip_cntl.ucp_ena;
  vertex_mod.vertex.user_clip_plane_count = uint32_t(__builtin_popcount(user_clip_planes));
  vertex_mod.vertex.user_clip_plane_cull = uint32_t(user_clip_planes && clip_cntl.ucp_cull_only_ena);
  vertex_mod.vertex.point_ps_ucp_mode = clip_cntl.ps_ucp_mode;
  vertex_mod.vertex.vertex_kill_and =
      uint32_t((vs->shader->writes_point_size_edge_flag_kill_vertex() & 0b100) &&
               !clip_cntl.vtx_kill_or);
  vertex_mod.vertex.output_point_size = 0;
  DxbcShaderTranslator::Modification pixel_mod(0);
  if (ps) {
    pixel_mod = DxbcShaderTranslator::Modification(translator_.GetDefaultPixelShaderModification(
        ps->shader->GetDynamicAddressableRegisterCount(sq_program_cntl.ps_num_reg)));
    pixel_mod.pixel.interpolator_mask = interpolator_mask;
    pixel_mod.pixel.interpolators_centroid =
        interpolator_mask &
        ~xenos::GetInterpolatorSamplingPattern(
            surface_info.msaa_samples, regs.Get<reg::SQ_CONTEXT_MISC>().sc_sample_cntl,
            regs.Get<reg::SQ_INTERPOLATOR_CNTL>().sampling_pattern);
    if (param_gen_pos < xenos::kMaxInterpolators) {
      pixel_mod.pixel.param_gen_enable = 1;
      pixel_mod.pixel.param_gen_interpolator = param_gen_pos;
    }
    using DepthStencilMode = DxbcShaderTranslator::Modification::DepthStencilMode;
    pixel_mod.pixel.depth_stencil_mode =
        ps->shader->implicit_early_z_write_allowed() &&
                (!ps->shader->writes_color_target(0) ||
                 !draw_util::DoesCoverageDependOnAlpha(regs.Get<reg::RB_COLORCONTROL>()))
            ? DepthStencilMode::kEarlyHint
            : DepthStencilMode::kNoModifiers;
  }
  auto* vertex_translation = vs->shader->GetOrCreateTranslation(vertex_mod.value);
  if (!vertex_translation->is_translated()) {
    const auto start = std::chrono::steady_clock::now();
    translator_.TranslateAnalyzedShader(*vertex_translation);
    stats_.translate_ms += std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - start).count();
    ++stats_.translations;
  }
  rex::graphics::Shader::Translation* pixel_translation = nullptr;
  if (ps) {
    pixel_translation = ps->shader->GetOrCreateTranslation(pixel_mod.value);
    if (!pixel_translation->is_translated()) {
      const auto start = std::chrono::steady_clock::now();
      translator_.TranslateAnalyzedShader(*pixel_translation);
      stats_.translate_ms += std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - start).count();
      ++stats_.translations;
    }
  }
  if (!vertex_translation->is_valid() || (pixel_translation && !pixel_translation->is_valid())) {
    ++stats_.draws_skipped;
    return;
  }

  // Render targets.
  const uint32_t color_mask =
      ps ? draw_util::GetNormalizedColorMask(regs, ps->shader->writes_color_targets()) : 0;
  const uint32_t msaa = uint32_t(surface_info.msaa_samples);
  const uint32_t pitch = surface_info.surface_pitch;
  RenderTarget* colors[4] = {};
  uint32_t color_count = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    if (!((color_mask >> (4 * i)) & 0xF)) {
      continue;
    }
    const auto color_info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    colors[i] = GetRenderTarget(color_info.color_base, pitch, msaa,
                                uint32_t(color_info.color_format), false);
    color_count = i + 1;
  }
  RenderTarget* depth = nullptr;
  if (depth_control.z_enable || depth_control.stencil_enable) {
    const auto depth_info = regs.Get<reg::RB_DEPTH_INFO>();
    depth = GetRenderTarget(depth_info.depth_base, pitch, msaa, uint32_t(depth_info.depth_format),
                            true);
  }
  if (!color_count && !depth) {
    ++stats_.draws_skipped;
    return;
  }

  // Root signature and pipeline.
  const DxbcShader& vs_shader = *vs->shader;
  const uint32_t vs_textures = uint32_t(vs_shader.GetTextureBindingsAfterTranslation().size());
  const uint32_t vs_samplers = uint32_t(vs_shader.GetSamplerBindingsAfterTranslation().size());
  const uint32_t ps_textures =
      ps ? uint32_t(ps->shader->GetTextureBindingsAfterTranslation().size()) : 0;
  const uint32_t ps_samplers =
      ps ? uint32_t(ps->shader->GetSamplerBindingsAfterTranslation().size()) : 0;
  ID3D12RootSignature* root = GetRootSignature(vs_textures, vs_samplers, ps_textures, ps_samplers);
  if (!root) {
    ++stats_.draws_skipped;
    return;
  }
  PipelineKey key;
  std::memset(&key, 0, sizeof(key));
  key.vertex_translation = vertex_translation;
  key.pixel_translation = pixel_translation;
  key.root_signature = root;
  key.topology_type = prim == xenos::PrimitiveType::kTriangleStrip ? 1 : 0;
  key.samples = HostSamples(msaa);
  // Occlusion queries count host samples: back to the guest's for this draw.
  if (occlusion_open_) {
    occlusion_ratio_[occlusion_slot_] = float(1u << msaa) / float(key.samples);
  }
  for (uint32_t i = 0; i < color_count; ++i) {
    if (!colors[i]) {
      continue;
    }
    key.rtv_formats[i] = colors[i]->view_format;
    key.write_mask[i] = (color_mask >> (4 * i)) & 0xF;
    key.blend[i] = regs[reg::RB_BLENDCONTROL::rt_register_indices[i]] & 0x1FFF1FFF;
  }
  const auto mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  if (polygonal) {
    key.cull = mode_cntl.cull_front ? 1 : (mode_cntl.cull_back ? 2 : 0);
    key.front_counter_clockwise = mode_cntl.face == 0;
  }
  float offset_scale, offset;
  draw_util::GetPreferredFacePolygonOffset(regs, polygonal, offset_scale, offset);
  key.depth_bias = draw_util::GetD3D10IntegerPolygonOffset(
      regs.Get<reg::RB_DEPTH_INFO>().depth_format, offset);
  key.depth_bias_slope = offset_scale * xenos::kPolygonOffsetScaleSubpixelUnit;
  key.depth_clip = !clip_cntl.clip_disable;
  if (depth) {
    key.dsv_format = depth->view_format;
    xenos::CompareFunction func =
        depth_control.z_enable ? depth_control.zfunc : xenos::CompareFunction::kAlways;
    key.depth_write = depth_control.z_enable && depth_control.z_write_enable;
    key.depth_enable = func != xenos::CompareFunction::kAlways || key.depth_write;
    key.depth_func = uint32_t(func);
    if (depth_control.stencil_enable) {
      key.stencil_enable = 1;
      const auto ref_mask = regs.Get<reg::RB_STENCILREFMASK>();
      key.stencil_read_mask = ref_mask.stencilmask;
      key.stencil_write_mask = ref_mask.stencilwritemask;
      key.stencil_front = (depth_control.value >> 8) & 0xFFF;
      key.stencil_back = (polygonal && depth_control.backface_enable)
                             ? (depth_control.value >> 20) & 0xFFF
                             : key.stencil_front;
    }
  }


  auto& pipeline = pipelines_[HashBytes(&key, sizeof(key))];
  if (!pipeline) {
    pipeline = std::make_unique<Pipeline>();
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = root;
    desc.VS = {vertex_translation->translated_binary().data(),
               vertex_translation->translated_binary().size()};
    if (pixel_translation) {
      desc.PS = {pixel_translation->translated_binary().data(),
                 pixel_translation->translated_binary().size()};
    }
    desc.BlendState.IndependentBlendEnable = TRUE;
    for (uint32_t i = 0; i < 4; ++i) {
      D3D12_RENDER_TARGET_BLEND_DESC& b = desc.BlendState.RenderTarget[i];
      b.RenderTargetWriteMask = uint8_t(key.write_mask[i]);
      const uint32_t c = key.blend[i];
      const uint32_t src = c & 0x1F, op = (c >> 5) & 7, dst = (c >> 8) & 0x1F;
      const uint32_t asrc = (c >> 16) & 0x1F, aop = (c >> 21) & 7, adst = (c >> 24) & 0x1F;
      if (key.write_mask[i] &&
          !(src == 1 && dst == 0 && op == 0 && asrc == 1 && adst == 0 && aop == 0)) {
        b.BlendEnable = TRUE;
        b.SrcBlend = BlendFactor(src, false);
        b.DestBlend = BlendFactor(dst, false);
        b.BlendOp = BlendOp(op);
        b.SrcBlendAlpha = BlendFactor(asrc, true);
        b.DestBlendAlpha = BlendFactor(adst, true);
        b.BlendOpAlpha = BlendOp(aop);
      }
      desc.RTVFormats[i] = key.rtv_formats[i];
    }
    desc.NumRenderTargets = color_count;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = key.cull == 1   ? D3D12_CULL_MODE_FRONT
                                    : key.cull == 2 ? D3D12_CULL_MODE_BACK
                                                    : D3D12_CULL_MODE_NONE;
    desc.RasterizerState.FrontCounterClockwise = key.front_counter_clockwise;
    desc.RasterizerState.DepthBias = key.depth_bias;
    desc.RasterizerState.SlopeScaledDepthBias = key.depth_bias_slope;
    desc.RasterizerState.DepthClipEnable = key.depth_clip;
    if (key.depth_enable) {
      desc.DepthStencilState.DepthEnable = TRUE;
      desc.DepthStencilState.DepthWriteMask =
          key.depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
      desc.DepthStencilState.DepthFunc =
          D3D12_COMPARISON_FUNC(uint32_t(D3D12_COMPARISON_FUNC_NEVER) + key.depth_func);
    }
    if (key.stencil_enable) {
      desc.DepthStencilState.StencilEnable = TRUE;
      desc.DepthStencilState.StencilReadMask = uint8_t(key.stencil_read_mask);
      desc.DepthStencilState.StencilWriteMask = uint8_t(key.stencil_write_mask);
      auto face = [](uint32_t v) {
        D3D12_DEPTH_STENCILOP_DESC f;
        f.StencilFunc = D3D12_COMPARISON_FUNC(uint32_t(D3D12_COMPARISON_FUNC_NEVER) + (v & 7));
        f.StencilFailOp = D3D12_STENCIL_OP(uint32_t(D3D12_STENCIL_OP_KEEP) + ((v >> 3) & 7));
        f.StencilPassOp = D3D12_STENCIL_OP(uint32_t(D3D12_STENCIL_OP_KEEP) + ((v >> 6) & 7));
        f.StencilDepthFailOp = D3D12_STENCIL_OP(uint32_t(D3D12_STENCIL_OP_KEEP) + ((v >> 9) & 7));
        return f;
      };
      desc.DepthStencilState.FrontFace = face(key.stencil_front);
      desc.DepthStencilState.BackFace = face(key.stencil_back);
    }
    if (depth) {
      desc.DSVFormat = key.dsv_format;
    }
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.SampleDesc.Count = key.samples;
    if (async_pipeline_threads_) {
      // In the background: a pipeline the driver has not compiled before took
      // up to 350 ms, the whole renderer stood still meanwhile. The draws that
      // need it are skipped until it is there (a few frames).
      StartPipelineWorkers();
      {
        std::lock_guard<std::mutex> lock(pipeline_jobs_mutex_);
        pipeline_jobs_.push_back({pipeline.get(), desc});
      }
      pipeline_jobs_cv_.notify_one();
    } else {
      CreatePipeline(*pipeline, desc);
    }
  }
  const int pipeline_status = pipeline->status.load(std::memory_order_acquire);
  if (pipeline_status != 0 && !pipeline->counted) {
    pipeline->counted = true;
    stats_.pipeline_ms += pipeline->compile_ms;
    if (pipeline_status == 1) {
      ++stats_.pipelines;
    } else {
      ++stats_.pipeline_failures;
    }
  }
  if (pipeline_status != 1) {
    ++stats_.draws_skipped;
    if (pipeline_status == 0) {
      ++stats_.draws_waiting_for_pipelines;
    }
    return;
  }

  // System constants (D3D12CommandProcessor::UpdateSystemConstantValues
  // without the ROV parts).
  DxbcShaderTranslator::SystemConstants system;
  std::memset(&system, 0, sizeof(system));
  const auto vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  uint32_t flags = 0;
  if (vte_cntl.vtx_xy_fmt) flags |= DxbcShaderTranslator::kSysFlag_XYDividedByW;
  if (vte_cntl.vtx_z_fmt) flags |= DxbcShaderTranslator::kSysFlag_ZDividedByW;
  if (vte_cntl.vtx_w0_fmt) flags |= DxbcShaderTranslator::kSysFlag_WNotReciprocal;
  if (polygonal) flags |= DxbcShaderTranslator::kSysFlag_PrimitivePolygonal;
  if (draw_util::IsPrimitiveLine(regs)) flags |= DxbcShaderTranslator::kSysFlag_PrimitiveLine;
  if (regs.Get<reg::RB_DEPTH_INFO>().depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    flags |= DxbcShaderTranslator::kSysFlag_DepthFloat24;
  }
  const auto color_control = regs.Get<reg::RB_COLORCONTROL>();
  const xenos::CompareFunction alpha_func =
      color_control.alpha_test_enable ? color_control.alpha_func : xenos::CompareFunction::kAlways;
  flags |= uint32_t(alpha_func) << DxbcShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
  for (uint32_t i = 0; i < 4; ++i) {
    const auto color_info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    if (color_info.color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
      flags |= DxbcShaderTranslator::kSysFlag_ConvertColor0ToGamma << i;
    }
    system.color_exp_bias[i] =
        std::ldexp(1.0f, int(color_info.color_exp_bias));
  }
  system.flags = flags;
  system.vertex_index_endian =
      d.indexed ? (d.index_32bit ? xenos::Endian::k8in32 : xenos::Endian::k8in16)
                : xenos::Endian::kNone;
  system.vertex_index_offset = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
  system.vertex_index_min = regs.Get<reg::VGT_MIN_VTX_INDX>().min_indx;
  system.vertex_index_max = regs.Get<reg::VGT_MAX_VTX_INDX>().max_indx;
  {
    float* plane = system.user_clip_planes[0];
    for (uint32_t i = 0; i < 6; ++i) {
      if (user_clip_planes & (1u << i)) {
        std::memcpy(plane, &regs.values[XE_GPU_REG_PA_CL_UCP_0_X + 4 * i], 4 * sizeof(float));
        plane += 4;
      }
    }
  }
  draw_util::ViewportInfo viewport;
  draw_util::GetHostViewportInfo(regs, scale_, scale_, true, D3D12_VIEWPORT_BOUNDS_MAX,
                                 D3D12_VIEWPORT_BOUNDS_MAX, false, depth_control, false, true,
                                 ps && ps->shader->writes_depth(), viewport);
  for (uint32_t i = 0; i < 3; ++i) {
    system.ndc_scale[i] = viewport.ndc_scale[i];
    system.ndc_offset[i] = viewport.ndc_offset[i];
  }
  // The host's sample count (8 samples have no Xenos pattern: as 4).
  system.sample_count_log2[0] = key.samples >= 4 ? 1 : 0;
  system.sample_count_log2[1] = key.samples >= 2 ? 1 : 0;
  system.alpha_test_reference = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  system.alpha_to_mask =
      color_control.alpha_to_mask_enable ? (color_control.value >> 24) | (1 << 8) : 0;

  // Textures and samplers of both stages.
  auto fetch_words = [&](uint32_t index) {
    return &regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 6 * index];
  };
  // Tables of the same views (most draws in a row use the same textures) are
  // made once per descriptor heap half: creating views is a driver call.
  auto bind_textures = [&](const DxbcShader& shader) -> D3D12_GPU_DESCRIPTOR_HANDLE {
    const auto& bindings = shader.GetTextureBindingsAfterTranslation();
    struct View {
      ID3D12Resource* resource;  // null: a null view
      D3D12_SHADER_RESOURCE_VIEW_DESC srv;
      D3D12_CPU_DESCRIPTOR_HANDLE null_view;
    };
    View views[64];
    uint32_t view_count = 0;
    uint64_t key = 1469598103934665603ull;
    for (const auto& binding : bindings) {
      const uint32_t* words = fetch_words(binding.fetch_constant);
      xenos::xe_gpu_texture_fetch_t fetch;
      std::memcpy(&fetch, words, sizeof(fetch));
      const uint8_t signs = texture_util::SwizzleSigns(fetch);
      uint32_t& signs_word = system.texture_swizzled_signs[binding.fetch_constant >> 2];
      signs_word |= uint32_t(signs) << ((binding.fetch_constant & 3) * 8);
      HostTexture* texture = GetTexture(words, binding.dimension == xenos::FetchOpDimension::kCube);
      const bool cube = binding.dimension == xenos::FetchOpDimension::kCube;
      const bool volume = binding.dimension == xenos::FetchOpDimension::k3DOrStacked;
      if (texture && (volume != texture->is_3d || (cube && texture->array_size != 6))) {
        texture = nullptr;
      }
      if (texture && texture->scaled) {
        system.textures_resolution_scaled |= uint32_t(1) << binding.fetch_constant;
      }
      if (texture) {
        Transition(texture->resource.Get(), texture->state,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = texture->format;
        srv.Shader4ComponentMapping = ComponentMapping(fetch.swizzle, texture->host_swizzle);
        if (volume) {
          srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
          srv.Texture3D.MipLevels = texture->mips;
        } else if (cube) {
          srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
          srv.TextureCube.MipLevels = texture->mips;
        } else {
          srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
          srv.Texture2DArray.MipLevels = texture->mips;
          srv.Texture2DArray.ArraySize = texture->array_size;
        }
        views[view_count] = {texture->resource.Get(), srv, {}};
      } else {
        views[view_count] = {
            nullptr, {}, cube ? null_srv_cube_ : (volume ? null_srv_3d_ : null_srv_2d_array_)};
      }
      const View& v = views[view_count];
      key = (key ^ reinterpret_cast<uintptr_t>(v.resource)) * 1099511628211ull;
      key = (key ^ HashBytes(&v.srv, sizeof(v.srv))) * 1099511628211ull;
      key = (key ^ v.null_view.ptr) * 1099511628211ull;
      if (++view_count == std::size(views)) {
        break;
      }
    }
    auto known = texture_ranges_.find(key);
    if (known != texture_ranges_.end()) {
      ++stats_.texture_tables_reused;
      return known->second;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = AllocateViews(uint32_t(bindings.size()), cpu);
    for (uint32_t i = 0; i < view_count; ++i) {
      const View& v = views[i];
      if (v.resource) {
        device_->CreateShaderResourceView(v.resource, &v.srv, cpu);
      } else {
        device_->CopyDescriptorsSimple(1, cpu, v.null_view, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      }
      cpu.ptr += view_increment_;
    }
    texture_ranges_.emplace(key, gpu);
    return gpu;
  };
  auto bind_samplers = [&](const DxbcShader& shader) -> D3D12_GPU_DESCRIPTOR_HANDLE {
    const auto& bindings = shader.GetSamplerBindingsAfterTranslation();
    D3D12_SAMPLER_DESC descs[32];
    uint32_t desc_count = 0;
    uint64_t key = 1469598103934665603ull;
    for (const auto& binding : bindings) {
      xenos::xe_gpu_texture_fetch_t fetch;
      std::memcpy(&fetch, fetch_words(binding.fetch_constant), sizeof(fetch));
      auto pick = [](xenos::TextureFilter b, xenos::TextureFilter f) {
        return b == xenos::TextureFilter::kUseFetchConst ? f : b;
      };
      const xenos::TextureFilter mag = pick(binding.mag_filter, fetch.mag_filter);
      const xenos::TextureFilter min = pick(binding.min_filter, fetch.min_filter);
      const xenos::TextureFilter mip = pick(binding.mip_filter, fetch.mip_filter);
      xenos::AnisoFilter aniso = binding.aniso_filter == xenos::AnisoFilter::kUseFetchConst
                                     ? fetch.aniso_filter
                                     : binding.aniso_filter;
      // The launcher's anisotropic filtering, on the textures the SDK's
      // texture cache would force it on (linear, with mips).
      if (anisotropic_override_ >= 0 && anisotropic_override_ < 6 &&
          mag == xenos::TextureFilter::kLinear && min == xenos::TextureFilter::kLinear &&
          (mip == xenos::TextureFilter::kPoint || mip == xenos::TextureFilter::kLinear) &&
          fetch.mip_max_level > fetch.mip_min_level) {
        aniso = xenos::AnisoFilter(anisotropic_override_);
      }
      xenos::ClampMode cx, cy, cz;
      texture_util::GetClampModesForDimension(fetch, cx, cy, cz);
      D3D12_SAMPLER_DESC s = {};
      if (aniso != xenos::AnisoFilter::kDisabled) {
        s.Filter = D3D12_FILTER_ANISOTROPIC;
        s.MaxAnisotropy = 1u << (std::min<uint32_t>(uint32_t(aniso), 5) - 1);
      } else {
        auto t = [](xenos::TextureFilter f) {
          return f == xenos::TextureFilter::kLinear ? D3D12_FILTER_TYPE_LINEAR
                                                    : D3D12_FILTER_TYPE_POINT;
        };
        s.Filter = D3D12_ENCODE_BASIC_FILTER(t(min), t(mag), t(mip),
                                             D3D12_FILTER_REDUCTION_TYPE_STANDARD);
        s.MaxAnisotropy = 1;
      }
      s.AddressU = AddressMode(cx);
      s.AddressV = AddressMode(cy);
      s.AddressW = AddressMode(cz);
      s.MipLODBias = float(fetch.lod_bias) * (1.0f / 32.0f);
      s.MinLOD = float(fetch.mip_min_level);
      s.MaxLOD = mip == xenos::TextureFilter::kBaseMap ? 0.0f : float(fetch.mip_max_level);
      // Mipmap setting: 1 one level sharper, 2 only the largest level (no
      // mipmaps: sharpest, but distant surfaces shimmer).
      if (mip_mode_ == 1) {
        s.MipLODBias -= 1.0f;
      } else if (mip_mode_ == 2) {
        s.MaxLOD = s.MinLOD;
      }
      s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
      if (fetch.border_color == xenos::BorderColor::k_ABGR_White) {
        s.BorderColor[0] = s.BorderColor[1] = s.BorderColor[2] = s.BorderColor[3] = 1.0f;
      }
      if (desc_count < 32) {
        descs[desc_count++] = s;
      }
      key = (key ^ HashBytes(&s, sizeof(s))) * 1099511628211ull;
    }
    auto it = sampler_ranges_.find(key);
    if (it != sampler_ranges_.end()) {
      return it->second;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = sampler_heap_->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += size_t(sampler_heap_used_) * sampler_increment_;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = sampler_heap_->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += uint64_t(sampler_heap_used_) * sampler_increment_;
    for (uint32_t i = 0; i < desc_count; ++i) {
      const D3D12_SAMPLER_DESC& s = descs[i];
      device_->CreateSampler(&s, cpu);
      cpu.ptr += sampler_increment_;
    }
    sampler_heap_used_ += desc_count;
    sampler_ranges_.emplace(key, gpu);
    return gpu;
  };
  D3D12_GPU_DESCRIPTOR_HANDLE tables[4];
  uint32_t table_count = 0;
  if (ps_textures) tables[table_count++] = bind_textures(*ps->shader);
  if (ps_samplers) tables[table_count++] = bind_samplers(*ps->shader);
  if (vs_textures) tables[table_count++] = bind_textures(vs_shader);
  if (vs_samplers) tables[table_count++] = bind_samplers(vs_shader);

  // Constant buffers.
  auto upload_constants = [&](const void* data, uint32_t size) {
    D3D12_GPU_VIRTUAL_ADDRESS gpu;
    uint8_t* p = AllocateUpload(std::max<uint32_t>(size, 16), 256, gpu);
    std::memcpy(p, data, size);
    return gpu;
  };
  // Packed as the translator expects (only the constants the shader reads),
  // straight into the upload buffer.
  auto float_constants = [&](const DxbcShader* shader, uint32_t first_register) {
    const uint32_t count = shader ? shader->constant_register_map().float_count : 0;
    D3D12_GPU_VIRTUAL_ADDRESS gpu;
    uint32_t* out =
        reinterpret_cast<uint32_t*>(AllocateUpload(std::max<uint32_t>(count, 1) * 16, 256, gpu));
    if (!count) {
      std::memset(out, 0, 16);
      return gpu;
    }
    const auto& map = shader->constant_register_map();
    for (uint32_t i = 0; i < 4; ++i) {
      uint64_t bits = map.float_bitmap[i];
      while (bits) {
        const uint32_t index = uint32_t(__builtin_ctzll(bits));
        bits &= bits - 1;
        std::memcpy(out, &regs.values[first_register + (i << 8) + (index << 2)], 16);
        out += 4;
      }
    }
    return gpu;
  };
  const D3D12_GPU_VIRTUAL_ADDRESS cb_system = upload_constants(&system, sizeof(system));
  const D3D12_GPU_VIRTUAL_ADDRESS cb_float_vs =
      float_constants(&vs_shader, XE_GPU_REG_SHADER_CONSTANT_000_X);
  const D3D12_GPU_VIRTUAL_ADDRESS cb_float_ps =
      float_constants(ps ? ps->shader.get() : nullptr, XE_GPU_REG_SHADER_CONSTANT_256_X);
  const D3D12_GPU_VIRTUAL_ADDRESS cb_bool_loop =
      upload_constants(&regs.values[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031], (8 + 32) * 4);
  const D3D12_GPU_VIRTUAL_ADDRESS cb_fetch =
      upload_constants(&regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], 32 * 6 * 4);

  // Index buffer.
  D3D12_INDEX_BUFFER_VIEW index_view = {};
  uint32_t host_count = d.vertex_count;
  bool use_indices = d.indexed;
  if (d.indexed) {
    index_view.BufferLocation = shared_memory_->GetGPUVirtualAddress() + d.index_address;
    index_view.Format = d.index_32bit ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    index_view.SizeInBytes = d.vertex_count * (d.index_32bit ? 4 : 2);
  } else if (prim == xenos::PrimitiveType::kTriangleFan ||
             prim == xenos::PrimitiveType::kQuadList) {
    std::vector<uint32_t> indices;
    if (prim == xenos::PrimitiveType::kTriangleFan) {
      for (uint32_t i = 2; i < d.vertex_count; ++i) {
        indices.insert(indices.end(), {0, i - 1, i});
      }
    } else {
      for (uint32_t q = 0; q + 3 < d.vertex_count; q += 4) {
        indices.insert(indices.end(), {q, q + 1, q + 2, q, q + 2, q + 3});
      }
    }
    if (indices.empty()) {
      ++stats_.draws_skipped;
      return;
    }
    D3D12_GPU_VIRTUAL_ADDRESS gpu;
    uint8_t* p = AllocateUpload(uint32_t(indices.size() * 4), 16, gpu);
    std::memcpy(p, indices.data(), indices.size() * 4);
    index_view.BufferLocation = gpu;
    index_view.Format = DXGI_FORMAT_R32_UINT;
    index_view.SizeInBytes = uint32_t(indices.size() * 4);
    host_count = uint32_t(indices.size());
    use_indices = true;
  }

  for (const auto& binding : vs_shader.vertex_bindings()) {
    if (!binding.stride_words) {
      ++stats_.unpatched_vertex_shaders;
      break;
    }
  }
  // The library patches the vertex shader for the draw's vertex layout. A
  // draw recorded with a shader patched for another stride (measured: a 32
  // byte UP fan with the shader of a previous 24 byte UP draw, HUD quad lists
  // with a 32 byte shader on 24 byte streams; the library loaded the right
  // one where the recorder did not see it) puts every vertex somewhere else:
  // white stripes and triangles over the HUD for a frame. Not drawn (the
  // frame is then not shown either, see Parallel::Render).
  if (d.stream0_stride_words) {
    for (const auto& binding : vs_shader.vertex_bindings()) {
      if (binding.fetch_constant == 95 && binding.stride_words &&
          binding.stride_words != d.stream0_stride_words) {
        ++stats_.draws_stride_mismatch;
        ++stats_.draws_stride_mismatch_total;
        ++stats_.draws_skipped;
        return;
      }
    }
  }
  // In the running game: upload what the draw reads that changed (first the
  // vertex data recorded with the draw, where the game has rewritten it).
  if (live_) {
    ApplyDataCopies(d);
    SyncDrawData(d, vs_shader);
  }

  // Record.
  Transition(shared_memory_.Get(), shared_memory_state_,
             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                 D3D12_RESOURCE_STATE_INDEX_BUFFER);
  D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4];
  for (uint32_t i = 0; i < color_count; ++i) {
    if (colors[i]) {
      Transition(colors[i]->resource.Get(), colors[i]->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
      rtvs[i] = colors[i]->view;
    } else {
      rtvs[i] = colors[0] ? colors[0]->view : rtvs[0];
    }
  }
  if (depth) {
    Transition(depth->resource.Get(), depth->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
  }
  // State the previous draw of this list set already is not set again (the
  // driver calls are most of the renderer thread's time).
  BoundState& b = bound_;
  const D3D12_CPU_DESCRIPTOR_HANDLE dsv = depth ? depth->view : D3D12_CPU_DESCRIPTOR_HANDLE{};
  bool targets_same = b.valid && b.rtv_count == color_count && b.dsv.ptr == dsv.ptr;
  for (uint32_t i = 0; targets_same && i < color_count; ++i) {
    targets_same = b.rtvs[i].ptr == rtvs[i].ptr;
  }
  if (!targets_same) {
    list_->OMSetRenderTargets(color_count, color_count ? rtvs : nullptr, FALSE,
                              depth ? &depth->view : nullptr);
    b.rtv_count = color_count;
    std::copy(rtvs, rtvs + color_count, b.rtvs);
    b.dsv = dsv;
  }
  if (!b.valid || b.root != root) {
    list_->SetGraphicsRootSignature(root);
    b.root = root;
    // A new root signature leaves all root arguments unset.
    std::fill(std::begin(b.cbvs), std::end(b.cbvs), D3D12_GPU_VIRTUAL_ADDRESS(0));
    std::fill(std::begin(b.tables), std::end(b.tables), D3D12_GPU_DESCRIPTOR_HANDLE{});
    b.shared_memory_table = {};
  }
  if (!b.valid || b.pipeline != pipeline->state.Get()) {
    list_->SetPipelineState(pipeline->state.Get());
    b.pipeline = pipeline->state.Get();
  }
  const D3D12_GPU_VIRTUAL_ADDRESS cbvs[5] = {cb_fetch, cb_float_vs, cb_float_ps, cb_system,
                                             cb_bool_loop};
  const UINT cbv_roots[5] = {kRootFetchConstants, kRootFloatConstantsVertex,
                             kRootFloatConstantsPixel, kRootSystemConstants,
                             kRootBoolLoopConstants};
  for (uint32_t i = 0; i < 5; ++i) {
    if (b.cbvs[i] != cbvs[i]) {
      list_->SetGraphicsRootConstantBufferView(cbv_roots[i], cbvs[i]);
      b.cbvs[i] = cbvs[i];
    }
  }
  if (b.shared_memory_table.ptr != shared_memory_table_.ptr) {
    list_->SetGraphicsRootDescriptorTable(kRootSharedMemory, shared_memory_table_);
    b.shared_memory_table = shared_memory_table_;
  }
  for (uint32_t i = 0; i < table_count; ++i) {
    if (b.tables[i].ptr != tables[i].ptr) {
      list_->SetGraphicsRootDescriptorTable(kRootBaseCount + i, tables[i]);
      b.tables[i] = tables[i];
    }
  }
  D3D12_VIEWPORT vp = {float(viewport.xy_offset[0]), float(viewport.xy_offset[1]),
                       float(viewport.xy_extent[0]), float(viewport.xy_extent[1]), viewport.z_min,
                       viewport.z_max};
  if (!b.valid || std::memcmp(&b.viewport, &vp, sizeof(vp))) {
    list_->RSSetViewports(1, &vp);
    b.viewport = vp;
  }
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  D3D12_RECT rect = {LONG(scissor.offset[0] * scale_), LONG(scissor.offset[1] * scale_),
                     LONG((scissor.offset[0] + scissor.extent[0]) * scale_),
                     LONG((scissor.offset[1] + scissor.extent[1]) * scale_)};
  if (!b.valid || std::memcmp(&b.scissor, &rect, sizeof(rect))) {
    list_->RSSetScissorRects(1, &rect);
    b.scissor = rect;
  }
  const float blend_factor[4] = {
      regs.Get<float>(XE_GPU_REG_RB_BLEND_RED), regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
      regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE), regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA)};
  if (!b.valid || std::memcmp(b.blend_factor, blend_factor, sizeof(blend_factor))) {
    list_->OMSetBlendFactor(blend_factor);
    std::memcpy(b.blend_factor, blend_factor, sizeof(blend_factor));
  }
  const uint32_t stencil_ref = regs.Get<reg::RB_STENCILREFMASK>().stencilref;
  if (!b.valid || b.stencil_ref != stencil_ref) {
    list_->OMSetStencilRef(stencil_ref);
    b.stencil_ref = stencil_ref;
  }
  // REPLAY_TRACE=first-last: draw state of these sequence numbers.
  static int trace_first = -1, trace_last = -1;
  if (trace_first < 0) {
    const char* t = std::getenv("REPLAY_TRACE");
    trace_first = 0;
    if (t) {
      std::sscanf(t, "%d-%d", &trace_first, &trace_last);
    }
  }
  if (int(d.sequence) >= trace_first && int(d.sequence) <= trace_last) {
    std::printf(
        "draw %u prim %u count %u%s vs %08X ps %08X | rt %u depth %s | viewport %u,%u %ux%u z "
        "%.3f-%.3f ndc scale %.4f %.4f %.4f offset %.4f %.4f %.4f | scissor %ld,%ld-%ld,%ld | "
        "depth %u write %u func %u cull %u ccw %u bias %d %.3f\n",
        d.sequence, d.primitive_type, d.vertex_count, d.indexed ? " indexed" : "",
        d.vertex_shader_address, ps ? d.pixel_shader_address : 0, color_count,
        depth ? "yes" : "no", viewport.xy_offset[0], viewport.xy_offset[1], viewport.xy_extent[0],
        viewport.xy_extent[1], viewport.z_min, viewport.z_max, viewport.ndc_scale[0],
        viewport.ndc_scale[1], viewport.ndc_scale[2], viewport.ndc_offset[0],
        viewport.ndc_offset[1], viewport.ndc_offset[2], rect.left, rect.top, rect.right,
        rect.bottom, key.depth_enable, key.depth_write, key.depth_func, key.cull,
        key.front_counter_clockwise, key.depth_bias, key.depth_bias_slope);
  }
  if (int(d.sequence) >= trace_first && int(d.sequence) <= trace_last) {
    for (const auto& binding : vs_shader.vertex_bindings()) {
      for (const auto& attribute : binding.attributes) {
        const auto& a = attribute.fetch_instr.attributes;
        std::printf("  binding fetch %u stride %u dwords: format %u offset %d dwords%s%s exp %d\n",
                    binding.fetch_constant, binding.stride_words, uint32_t(a.data_format),
                    a.offset, a.is_signed ? " signed" : "", a.is_integer ? " integer" : "",
                    a.exp_adjust);
      }
    }
    const auto& map = vs_shader.constant_register_map();
    for (uint32_t i = 0; i < 96; ++i) {
      if (map.vertex_fetch_bitmap[i >> 5] & (1u << (i & 31))) {
        const auto vf = regs.GetVertexFetch(i);
        const uint32_t address = vf.address << 2;
        std::printf("  vfetch %u: %08X size %u endian %u, first dwords %08X %08X %08X %08X\n", i,
                    address, vf.size << 2, uint32_t(vf.endian),
                    LoadBE32(guest_memory_ + address), LoadBE32(guest_memory_ + address + 4),
                    LoadBE32(guest_memory_ + address + 8), LoadBE32(guest_memory_ + address + 12));
      }
    }
    if (d.indexed) {
      const uint8_t* ib = guest_memory_ + d.index_address;
      std::printf("  indices at %08X: %u %u %u %u %u %u\n", d.index_address, ib[0] << 8 | ib[1],
                  ib[2] << 8 | ib[3], ib[4] << 8 | ib[5], ib[6] << 8 | ib[7], ib[8] << 8 | ib[9],
                  ib[10] << 8 | ib[11]);
    }
  }
  const D3D_PRIMITIVE_TOPOLOGY topology = prim == xenos::PrimitiveType::kTriangleStrip
                                              ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP
                                              : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  if (!b.valid || b.topology != topology) {
    list_->IASetPrimitiveTopology(topology);
    b.topology = topology;
  }
  b.valid = true;
  if (use_indices) {
    if (b.index_view.BufferLocation != index_view.BufferLocation ||
        b.index_view.Format != index_view.Format ||
        b.index_view.SizeInBytes != index_view.SizeInBytes) {
      list_->IASetIndexBuffer(&index_view);
      b.index_view = index_view;
    }
    list_->DrawIndexedInstanced(host_count, 1, 0, 0, 0);
  } else {
    list_->DrawInstanced(host_count, 1, 0, 0);
  }
  ++stats_.draws;
}

void Renderer::Resolve(const ResolveCall& r) {
  InvalidateBound();
  const rex::graphics::RegisterFile& regs = *r.regs;
  if (view_heap_used_ + 16 > view_heap_end_) {
    Flush();
  }
  const uint32_t control = regs[XE_GPU_REG_RB_COPY_CONTROL];
  const uint32_t source_select = control & 7;
  const uint32_t sample_select = (control >> 4) & 7;
  const bool clear_color = (control >> 8) & 1, clear_depth = (control >> 9) & 1;
  const auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  const uint32_t msaa = uint32_t(surface_info.msaa_samples);
  const uint32_t pitch = surface_info.surface_pitch;
  const auto depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  RenderTarget* source;
  RenderTarget* color_target = nullptr;
  if (source_select == 4) {
    source = GetRenderTarget(depth_info.depth_base, pitch, msaa, uint32_t(depth_info.depth_format),
                             true);
  } else {
    const auto color_info =
        regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[source_select & 3]);
    source = GetRenderTarget(color_info.color_base, pitch, msaa, uint32_t(color_info.color_format),
                             false);
    color_target = source;
  }
  if (!source) {
    return;
  }

  // Destination texture, by its physical base address.
  const uint32_t* f = r.dest_fetch;
  const uint32_t base = GuestToPhysical(f[1] & 0xFFFFF000);
  // At the renderer's scale, like the render targets it is copied from.
  const uint32_t width = ((f[2] & 0x1FFF) + 1) * scale_;
  const uint32_t height = (((f[2] >> 13) & 0x1FFF) + 1) * scale_;
  const bool cube = ((f[5] >> 9) & 3) == 3;
  const uint32_t array_size = cube ? 6 : 1;
  const DXGI_FORMAT format = source->depth ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
  auto& dest = resolved_[base];
  if (!dest || dest->width != width || dest->height != height || dest->format != format ||
      dest->array_size != array_size) {
    dest = std::make_unique<HostTexture>();
    dest->format = format;
    dest->width = width;
    dest->height = height;
    dest->array_size = array_size;
    dest->mips = 1;
    dest->host_swizzle = source->depth ? kSwizzleRRRR : kSwizzleRGBA;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = uint16_t(array_size);
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear = {};
    clear.Format = format;
    D3D12_HEAP_PROPERTIES default_heap = {D3D12_HEAP_TYPE_DEFAULT};
    dest->state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (FAILED(device_->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                dest->state, &clear,
                                                IID_PPV_ARGS(&dest->resource)))) {
      dest.reset();
      return;
    }
    for (uint32_t slice = 0; slice < array_size; ++slice) {
      D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
      rtv.ptr += size_t(rtv_used_++) * rtv_increment_;
      D3D12_RENDER_TARGET_VIEW_DESC rtv_desc = {};
      rtv_desc.Format = format;
      rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
      rtv_desc.Texture2DArray.FirstArraySlice = slice;
      rtv_desc.Texture2DArray.ArraySize = 1;
      device_->CreateRenderTargetView(dest->resource.Get(), &rtv_desc, rtv);
      dest->rtvs.push_back(rtv);
      const float zero[4] = {};
      list_->ClearRenderTargetView(rtv, zero, 0, nullptr);
    }
  }
  std::memcpy(dest->guest_fetch, r.dest_fetch, sizeof(dest->guest_fetch));
  dest->resolved_frame = frame_;
  dest->scaled = scale_ > 1;
  if (std::find(resolve_order_.begin(), resolve_order_.end(), base) == resolve_order_.end()) {
    resolve_order_.push_back(base);
  }

  // Copy (with MSAA resolve) through a full-screen triangle.
  const int32_t s = int32_t(scale_);
  int32_t left = std::max(r.rect[0] * s, 0), top = std::max(r.rect[1] * s, 0);
  int32_t right = std::min<int32_t>(r.rect[2] * s, int32_t(source->width));
  int32_t bottom = std::min<int32_t>(r.rect[3] * s, int32_t(source->height));
  const int32_t dest_x = r.dest_point[0] * s, dest_y = r.dest_point[1] * s;
  Transition(source->resource.Get(), source->state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  Transition(dest->resource.Get(), dest->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
  D3D12_CPU_DESCRIPTOR_HANDLE cpu;
  D3D12_GPU_DESCRIPTOR_HANDLE gpu = AllocateViews(1, cpu);
  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.Format = source->srv_format;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  if (source->samples > 1) {
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
  } else {
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
  }
  device_->CreateShaderResourceView(source->resource.Get(), &srv, cpu);
  const uint32_t dest_info = regs[XE_GPU_REG_RB_COPY_DEST_INFO];
  const uint32_t constants[4] = {uint32_t(left - dest_x), uint32_t(top - dest_y),
                                 source->depth ? 0u : sample_select,
                                 (source->depth ? 0u : ((dest_info >> 24) & 1)) |
                                     (source->samples << 8)};
  list_->SetPipelineState(resolve_color_[source->samples > 1][source->depth ? 1 : 0].Get());
  list_->SetGraphicsRootSignature(resolve_root_signature_.Get());
  list_->SetGraphicsRoot32BitConstants(0, 4, constants, 0);
  list_->SetGraphicsRootDescriptorTable(1, gpu);
  const uint32_t slice = std::min(r.dest_slice, array_size - 1);
  list_->OMSetRenderTargets(1, &dest->rtvs[slice], FALSE, nullptr);
  D3D12_VIEWPORT vp = {0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
  list_->RSSetViewports(1, &vp);
  D3D12_RECT rect = {dest_x, dest_y, dest_x + (right - left), dest_y + (bottom - top)};
  rect.right = std::min<LONG>(rect.right, LONG(width));
  rect.bottom = std::min<LONG>(rect.bottom, LONG(height));
  list_->RSSetScissorRects(1, &rect);
  list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  list_->DrawInstanced(3, 1, 0, 0);
  ++stats_.resolves;

  // Clears of the source region (the guest clears the EDRAM it resolved).
  const D3D12_RECT clear_rect = {left, top, right, bottom};
  if (clear_color && color_target) {
    const uint32_t c = regs[XE_GPU_REG_RB_COLOR_CLEAR];
    const float color[4] = {float((c >> 16) & 0xFF) / 255.0f, float((c >> 8) & 0xFF) / 255.0f,
                            float(c & 0xFF) / 255.0f, float(c >> 24) / 255.0f};
    Transition(color_target->resource.Get(), color_target->state,
               D3D12_RESOURCE_STATE_RENDER_TARGET);
    list_->ClearRenderTargetView(color_target->view, color, 1, &clear_rect);
  }
  if (clear_depth) {
    RenderTarget* depth = GetRenderTarget(depth_info.depth_base, pitch, msaa,
                                          uint32_t(depth_info.depth_format), true);
    if (depth) {
      const uint32_t v = regs[XE_GPU_REG_RB_DEPTH_CLEAR];
      Transition(depth->resource.Get(), depth->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
      list_->ClearDepthStencilView(depth->view, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                   float(v >> 8) / float(0xFFFFFF), uint8_t(v & 0xFF), 1,
                                   &clear_rect);
    }
  }
}

bool Renderer::SaveResolved(uint32_t base_address, const std::string& path, bool swap_red_blue) {
  auto it = resolved_.find(base_address);
  if (it == resolved_.end() || !it->second) {
    return false;
  }
  HostTexture& texture = *it->second;
  D3D12_RESOURCE_DESC desc = texture.resource->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT rows;
  UINT64 row_size, total;
  device_->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &row_size, &total);
  D3D12_HEAP_PROPERTIES readback_heap = {D3D12_HEAP_TYPE_READBACK};
  D3D12_RESOURCE_DESC buffer_desc = {};
  buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer_desc.Width = total;
  buffer_desc.Height = 1;
  buffer_desc.DepthOrArraySize = 1;
  buffer_desc.MipLevels = 1;
  buffer_desc.SampleDesc.Count = 1;
  buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> readback;
  if (FAILED(device_->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&readback)))) {
    return false;
  }
  Transition(texture.resource.Get(), texture.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION src = {};
  src.pResource = texture.resource.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.SubresourceIndex = 0;
  D3D12_TEXTURE_COPY_LOCATION dst = {};
  dst.pResource = readback.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = footprint;
  list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  if (!Flush()) {
    return false;
  }
  const uint8_t* data;
  D3D12_RANGE range = {0, size_t(total)};
  readback->Map(0, &range, reinterpret_cast<void**>(const_cast<uint8_t**>(&data)));
  const uint32_t w = texture.width, h = texture.height;
  std::vector<uint8_t> bgra(size_t(w) * h * 4);
  for (uint32_t y = 0; y < h; ++y) {
    const uint8_t* row = data + size_t(y) * footprint.Footprint.RowPitch;
    for (uint32_t x = 0; x < w; ++x) {
      uint8_t* o = &bgra[(size_t(y) * w + x) * 4];
      if (texture.format == DXGI_FORMAT_R32_FLOAT) {
        float v;
        std::memcpy(&v, row + x * 4, 4);
        const uint8_t g = uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
        o[0] = o[1] = o[2] = g;
      } else {
        const uint8_t* p = row + x * 4;
        o[0] = swap_red_blue ? p[0] : p[2];
        o[1] = p[1];
        o[2] = swap_red_blue ? p[2] : p[0];
      }
      o[3] = 255;
    }
  }
  readback->Unmap(0, nullptr);

  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  ComPtr<IWICImagingFactory> factory;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&factory)))) {
    return false;
  }
  std::wstring wpath(path.begin(), path.end());
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICBitmapFrameEncode> frame;
  if (FAILED(factory->CreateStream(&stream)) ||
      FAILED(stream->InitializeFromFilename(wpath.c_str(), GENERIC_WRITE)) ||
      FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
      FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
      FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr))) {
    return false;
  }
  frame->SetSize(w, h);
  WICPixelFormatGUID pixel_format = GUID_WICPixelFormat32bppBGRA;
  frame->SetPixelFormat(&pixel_format);
  frame->WritePixels(h, w * 4, UINT(bgra.size()), bgra.data());
  frame->Commit();
  encoder->Commit();
  return true;
}

// ---------------------------------------------------------------------------
// In the running game.

bool Renderer::UseLiveGuestMemory(const uint8_t* physical_memory) {
  guest_memory_ = physical_memory;
  live_copy_.reset(new (std::nothrow) uint8_t[kSharedMemorySize]());
  if (!live_copy_) {
    return false;
  }
  page_synced_frame_.assign(kSharedMemorySize / kLivePage, 0);
  page_changed_frame_.assign(kSharedMemorySize / kLivePage, 0);
  page_readable_frame_.assign(kSharedMemorySize / kLivePage, 0);
  live_ = true;
  return true;
}

namespace {

// Guest memory is reserved, and its pages are committed as the game allocates
// them; reading others faults. These read under structured exception handling
// instead of asking the OS about every page (VirtualQuery was too slow).
bool ProbePages(const uint8_t* p, size_t n) {
  __try {
    volatile uint8_t sink = 0;
    for (size_t at = 0; at < n; at += 4096) {
      sink ^= p[at];
    }
    sink ^= p[n - 1];
    (void)sink;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Updates `copy` from `guest` if they differ; false if the guest page faults.
bool CompareAndCopy(uint8_t* copy, const uint8_t* guest, size_t n, bool* changed) {
  __try {
    *changed = std::memcmp(copy, guest, n) != 0;
    if (*changed) {
      std::memcpy(copy, guest, n);
    }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

}  // namespace

bool Renderer::Readable(uint32_t address, uint32_t size) {
  if (!size || uint64_t(address) + size > kSharedMemorySize) {
    return false;
  }
  if (!live_) {
    return true;
  }
  // Pages already found readable in this frame are not probed again.
  for (uint32_t page = address / kLivePage, last = (address + size - 1) / kLivePage;
       page <= last; ++page) {
    if (page_readable_frame_[page] == frame_) {
      continue;
    }
    if (!ProbePages(guest_memory_ + size_t(page) * kLivePage, kLivePage)) {
      return false;
    }
    page_readable_frame_[page] = frame_;
  }
  return true;
}

void Renderer::SyncGuestRange(uint32_t address, uint32_t size) {
  if (!live_ || !size || address >= kSharedMemorySize) {
    return;
  }
  size = uint32_t(std::min<uint64_t>(size, kSharedMemorySize - address));
  uint32_t run_start = 0, run_size = 0;
  auto flush = [&]() {
    if (run_size) {
      UpdateMemory(run_start, live_copy_.get() + run_start, run_size);
      stats_.bytes_uploaded += run_size;
      run_size = 0;
    }
  };
  const uint32_t first_page = address / kLivePage;
  const uint32_t last_page = (address + size - 1) / kLivePage;
  for (uint32_t page = first_page; page <= last_page; ++page) {
    if (page_synced_frame_[page] == frame_) {
      flush();
      continue;
    }
    page_synced_frame_[page] = frame_;
    const uint32_t at = page * kLivePage;
    bool changed = false;
    if (!CompareAndCopy(live_copy_.get() + at, guest_memory_ + at, kLivePage, &changed)) {
      flush();
      continue;
    }
    // Read without a fault: readable this frame (Readable, called right after
    // for the same indices, then needs no probe of its own; the probes were
    // 15 % of the renderer thread).
    page_readable_frame_[page] = frame_;
    if (!changed) {
      flush();
      continue;
    }
    page_changed_frame_[page] = frame_;
    if (!run_size) {
      run_start = at;
    }
    run_size += kLivePage;
  }
  flush();
}

// The draw's recorded vertex data. The renderer draws a frame or two after
// the game: by then the game may have refilled the memory of a UP draw (the
// D3D library's 6 MB command ring, reused as soon as the emulated GPU has
// passed it) or of a front end buffer (three buffers in rotation, rewritten
// three frames later), and the HUD and front end quads were drawn with
// vertices of other quads: giant triangles in HUD colours for a frame, when
// an overlay changed. Where the recorded bytes differ from the memory, they
// go into the live copy and to the GPU for this draw; the pages are synced
// again from memory before the next draw. The pages of all copies are synced
// and uploaded once here (one upload per page per draw: two copies into the
// same bytes without a barrier between them would race).
void Renderer::ApplyDataCopies(const DrawCall& d) {
  for (uint32_t page : overlay_pages_) {
    page_synced_frame_[page] = 0;
  }
  overlay_pages_.clear();
  if (!d.data_copy_count) {
    return;
  }
  std::vector<uint32_t> upload;
  for (uint32_t i = 0; i < d.data_copy_count; ++i) {
    const DrawDataCopy& c = d.data_copies[i];
    if (!c.size || uint64_t(c.address) + c.size > kSharedMemorySize) {
      continue;
    }
    for (uint32_t page = c.address / kLivePage, last = (c.address + c.size - 1) / kLivePage;
         page <= last; ++page) {
      if (page_synced_frame_[page] == frame_) {
        continue;
      }
      page_synced_frame_[page] = frame_;
      const uint32_t at = page * kLivePage;
      bool changed = false;
      if (!CompareAndCopy(live_copy_.get() + at, guest_memory_ + at, kLivePage, &changed)) {
        continue;
      }
      page_readable_frame_[page] = frame_;
      if (changed) {
        page_changed_frame_[page] = frame_;
        upload.push_back(page);
      }
    }
  }
  for (uint32_t i = 0; i < d.data_copy_count; ++i) {
    const DrawDataCopy& c = d.data_copies[i];
    if (!c.size || uint64_t(c.address) + c.size > kSharedMemorySize ||
        std::memcmp(live_copy_.get() + c.address, c.bytes, c.size) == 0) {
      continue;
    }
    ++stats_.draw_data_differed_total;
    stats_.draw_data_differed_bytes_total += c.size;
    if (!apply_data_copies_) {
      continue;
    }
    std::memcpy(live_copy_.get() + c.address, c.bytes, c.size);
    for (uint32_t page = c.address / kLivePage, last = (c.address + c.size - 1) / kLivePage;
         page <= last; ++page) {
      page_changed_frame_[page] = frame_;
      upload.push_back(page);
      overlay_pages_.push_back(page);
    }
  }
  std::sort(upload.begin(), upload.end());
  upload.erase(std::unique(upload.begin(), upload.end()), upload.end());
  for (size_t i = 0; i < upload.size();) {
    size_t j = i + 1;
    while (j < upload.size() && upload[j] == upload[j - 1] + 1) {
      ++j;
    }
    const uint32_t at = upload[i] * kLivePage;
    const uint32_t size = uint32_t(j - i) * kLivePage;
    UpdateMemory(at, live_copy_.get() + at, size);
    stats_.bytes_uploaded += size;
    i = j;
  }
}

// The guest memory a draw reads: its indices and, for the index range they
// cover, the vertices of every vertex fetch of the vertex shader.
void Renderer::SyncDrawData(const DrawCall& d, const DxbcShader& vertex_shader) {
  const rex::graphics::RegisterFile& regs = *d.regs;
  uint32_t min_index = 0, max_index = d.vertex_count ? d.vertex_count - 1 : 0;
  if (d.indexed) {
    const uint32_t index_bytes = d.vertex_count * (d.index_32bit ? 4 : 2);
    SyncGuestRange(d.index_address, index_bytes);
    if (!Readable(d.index_address, index_bytes)) {
      return;
    }
    // The index range of the same indices is kept while their pages do not
    // change (most index buffers are static).
    const uint64_t key = (uint64_t(d.index_address) << 32) | (uint64_t(d.vertex_count) << 1) |
                         uint64_t(d.index_32bit);
    if (index_ranges_.size() > (1u << 18)) {
      index_ranges_.clear();  // dynamic index buffers leave many stale entries
    }
    IndexRange& range = index_ranges_[key];
    bool known = range.frame != 0;
    if (known && !(live_ && range.frame == frame_)) {
      for (uint32_t page = d.index_address / kLivePage,
                    last = (d.index_address + index_bytes - 1) / kLivePage;
           page <= last; ++page) {
        if (!live_ || page_changed_frame_[page] >= range.frame) {
          known = false;
          break;
        }
      }
    }
    if (known) {
      min_index = range.min;
      max_index = range.max;
      ++stats_.index_ranges_reused;
    } else {
      min_index = UINT32_MAX;
      max_index = 0;
      const uint8_t* p = guest_memory_ + d.index_address;
      for (uint32_t i = 0; i < d.vertex_count; ++i) {
        const uint32_t index =
            d.index_32bit ? LoadBE32(p + 4 * i) : (uint32_t(p[2 * i]) << 8) | p[2 * i + 1];
        min_index = std::min(min_index, index);
        max_index = std::max(max_index, index);
      }
      range = {min_index, max_index, frame_};
    }
    if (min_index > max_index) {
      return;
    }
  }
  const uint32_t offset = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
  for (const auto& binding : vertex_shader.vertex_bindings()) {
    const auto fetch = regs.GetVertexFetch(binding.fetch_constant);
    const uint32_t base = fetch.address << 2, size = fetch.size << 2;
    const uint32_t stride = binding.stride_words * 4;
    uint64_t first = 0, last = size;
    if (stride) {
      first = uint64_t(min_index + offset) * stride;
      last = std::min<uint64_t>(uint64_t(max_index + offset + 1) * stride, size);
    }
    if (first < last) {
      SyncGuestRange(base + uint32_t(first), uint32_t(std::min<uint64_t>(last - first, 64u << 20)));
    }
  }
}

void Renderer::BeginFrame() { ++frame_; }

bool Renderer::CreatePresentPipeline() {
  static const char kPresentShaders[] = R"(
Texture2DArray<float4> source : register(t0);
SamplerState linear_sampler : register(s0);
cbuffer Constants : register(b0) {
  float2 inverse_size;  // of the window
  uint flags;           // 1 = swap red and blue
};
float4 VSMain(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
float4 PSMain(float4 position : SV_Position) : SV_Target {
  float4 c = source.SampleLevel(linear_sampler, float3(position.xy * inverse_size, 0.0), 0.0);
  return float4((flags & 1) ? c.bgr : c.rgb, 1.0);
}
)";
  D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants.Num32BitValues = 3;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable.NumDescriptorRanges = 1;
  params[1].DescriptorTable.pDescriptorRanges = &range;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_STATIC_SAMPLER_DESC sampler = {};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC desc = {2, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ComPtr<ID3DBlob> blob, error, vs, ps;
  if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
      FAILED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                          IID_PPV_ARGS(&present_root_signature_))) ||
      FAILED(D3DCompile(kPresentShaders, sizeof(kPresentShaders) - 1, "present", nullptr, nullptr,
                        "VSMain", "vs_5_0", 0, 0, &vs, &error)) ||
      FAILED(D3DCompile(kPresentShaders, sizeof(kPresentShaders) - 1, "present", nullptr, nullptr,
                        "PSMain", "ps_5_0", 0, 0, &ps, &error))) {
    return false;
  }
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
  pso.pRootSignature = present_root_signature_.Get();
  pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pso.SampleMask = UINT_MAX;
  pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso.NumRenderTargets = 1;
  pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  pso.SampleDesc.Count = 1;
  return SUCCEEDED(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&present_pipeline_)));
}

bool Renderer::CreateWindowOutput(HWND window) {
  window_ = window;
  RECT rect;
  GetClientRect(window, &rect);
  window_width_ = std::max<uint32_t>(uint32_t(rect.right - rect.left), 1);
  window_height_ = std::max<uint32_t>(uint32_t(rect.bottom - rect.top), 1);
  DXGI_SWAP_CHAIN_DESC1 desc = {};
  desc.Width = window_width_;
  desc.Height = window_height_;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 2;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  ComPtr<IDXGISwapChain1> swap_chain;
  if (FAILED(factory_->CreateSwapChainForHwnd(queue_.Get(), window, &desc, nullptr, nullptr,
                                              &swap_chain)) ||
      FAILED(swap_chain.As(&swap_chain_))) {
    return false;
  }
  for (uint32_t i = 0; i < 2; ++i) {
    swap_chain_->GetBuffer(i, IID_PPV_ARGS(&back_buffers_[i]));
    back_buffer_rtvs_[i] = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    back_buffer_rtvs_[i].ptr += size_t(rtv_used_++) * rtv_increment_;
    device_->CreateRenderTargetView(back_buffers_[i].Get(), nullptr, back_buffer_rtvs_[i]);
  }
  return CreatePresentPipeline();
}

bool Renderer::Present(uint32_t front_buffer_base) {
  InvalidateBound();
  auto it = resolved_.find(front_buffer_base);
  if (!swap_chain_ || it == resolved_.end() || !it->second) {
    return Flush();
  }
  HostTexture& texture = *it->second;
  const uint32_t index = swap_chain_->GetCurrentBackBufferIndex();
  Transition(texture.resource.Get(), texture.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = back_buffers_[index].Get();
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
  list_->ResourceBarrier(1, &barrier);
  D3D12_CPU_DESCRIPTOR_HANDLE cpu;
  D3D12_GPU_DESCRIPTOR_HANDLE gpu = AllocateViews(1, cpu);
  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.Format = texture.format;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
  srv.Texture2DArray.MipLevels = 1;
  srv.Texture2DArray.ArraySize = 1;
  device_->CreateShaderResourceView(texture.resource.Get(), &srv, cpu);
  struct {
    float inverse_size[2];
    uint32_t flags;
  } constants = {{1.0f / float(window_width_), 1.0f / float(window_height_)}, 1};
  list_->SetPipelineState(present_pipeline_.Get());
  list_->SetGraphicsRootSignature(present_root_signature_.Get());
  list_->SetGraphicsRoot32BitConstants(0, 3, &constants, 0);
  list_->SetGraphicsRootDescriptorTable(1, gpu);
  list_->OMSetRenderTargets(1, &back_buffer_rtvs_[index], FALSE, nullptr);
  D3D12_VIEWPORT viewport = {0.0f, 0.0f, float(window_width_), float(window_height_), 0.0f, 1.0f};
  D3D12_RECT scissor = {0, 0, LONG(window_width_), LONG(window_height_)};
  list_->RSSetViewports(1, &viewport);
  list_->RSSetScissorRects(1, &scissor);
  list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  list_->DrawInstanced(3, 1, 0, 0);
  std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
  list_->ResourceBarrier(1, &barrier);
  if (!Flush()) {
    return false;
  }
  return SUCCEEDED(swap_chain_->Present(0, 0));
}

bool Renderer::CreateSharedOutput(uint32_t width, uint32_t height) {
  if (!present_pipeline_ && !CreatePresentPipeline()) {
    return false;
  }
  if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&shared_fence_))) ||
      FAILED(device_->CreateSharedHandle(shared_fence_.Get(), nullptr, GENERIC_ALL, nullptr,
                                         &shared_fence_handle_))) {
    return false;
  }
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  // Simultaneous access: the other device reads it from the common state
  // without transitions.
  desc.Flags =
      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
  D3D12_HEAP_PROPERTIES default_heap = {D3D12_HEAP_TYPE_DEFAULT};
  for (SharedOutput& output : shared_outputs_) {
    if (FAILED(device_->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_SHARED, &desc,
                                                D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                IID_PPV_ARGS(&output.texture))) ||
        FAILED(device_->CreateSharedHandle(output.texture.Get(), nullptr, GENERIC_ALL, nullptr,
                                           &output.handle))) {
      return false;
    }
    output.rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    output.rtv.ptr += size_t(rtv_used_++) * rtv_increment_;
    device_->CreateRenderTargetView(output.texture.Get(), nullptr, output.rtv);
  }
  shared_width_ = width;
  shared_height_ = height;
  return true;
}

bool Renderer::PresentToShared(uint32_t front_buffer_base) {
  InvalidateBound();
  auto it = resolved_.find(front_buffer_base);
  if (!shared_fence_ || it == resolved_.end() || !it->second) {
    return Flush();
  }
  HostTexture& texture = *it->second;
  const uint32_t index = shared_next_;
  shared_next_ = (shared_next_ + 1) % kSharedOutputs;
  SharedOutput& output = shared_outputs_[index];
  Transition(texture.resource.Get(), texture.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = output.texture.Get();
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
  list_->ResourceBarrier(1, &barrier);
  D3D12_CPU_DESCRIPTOR_HANDLE cpu;
  D3D12_GPU_DESCRIPTOR_HANDLE gpu = AllocateViews(1, cpu);
  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.Format = texture.format;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
  srv.Texture2DArray.MipLevels = 1;
  srv.Texture2DArray.ArraySize = 1;
  device_->CreateShaderResourceView(texture.resource.Get(), &srv, cpu);
  struct {
    float inverse_size[2];
    uint32_t flags;
  } constants = {{1.0f / float(shared_width_), 1.0f / float(shared_height_)}, 1};
  list_->SetPipelineState(present_pipeline_.Get());
  list_->SetGraphicsRootSignature(present_root_signature_.Get());
  list_->SetGraphicsRoot32BitConstants(0, 3, &constants, 0);
  list_->SetGraphicsRootDescriptorTable(1, gpu);
  list_->OMSetRenderTargets(1, &output.rtv, FALSE, nullptr);
  D3D12_VIEWPORT viewport = {0.0f, 0.0f, float(shared_width_), float(shared_height_), 0.0f, 1.0f};
  D3D12_RECT scissor = {0, 0, LONG(shared_width_), LONG(shared_height_)};
  list_->RSSetViewports(1, &viewport);
  list_->RSSetScissorRects(1, &scissor);
  list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  list_->DrawInstanced(3, 1, 0, 0);
  std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
  list_->ResourceBarrier(1, &barrier);
  // Not waiting: the next frame is recorded while the GPU draws this one; the
  // shared fence tells the other device when it is done.
  if (!Submit()) {
    return false;
  }
  queue_->Signal(shared_fence_.Get(), ++shared_fence_value_);
  std::lock_guard<std::mutex> lock(shared_mutex_);
  shared_latest_ = index;
  shared_latest_value_ = shared_fence_value_;
  return true;
}

bool Renderer::GetSharedFrame(SharedFrame& frame) {
  std::lock_guard<std::mutex> lock(shared_mutex_);
  if (!shared_latest_value_) {
    return false;
  }
  frame.texture = shared_outputs_[shared_latest_].handle;
  frame.fence = shared_fence_handle_;
  frame.fence_value = shared_latest_value_;
  frame.width = shared_width_;
  frame.height = shared_height_;
  return true;
}

uint32_t Renderer::WriteBackSmallResolves(uint8_t* guest_memory, uint32_t max_bytes) {
  // Which resolve destinations of this frame are small enough (the CPU reads
  // results like the average brightness for the exposure).
  struct Pending {
    HostTexture* texture;
    ComPtr<ID3D12Resource> readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    uint32_t fetch[6];
    uint32_t width, height;
  };
  std::vector<Pending> pending;
  for (auto& [base, texture] : resolved_) {
    if (!texture || texture->resolved_frame != frame_ ||
        texture->format != DXGI_FORMAT_R8G8B8A8_UNORM || texture->array_size != 1 ||
        (texture->width / scale_) * (texture->height / scale_) * 4 > max_bytes) {
      continue;
    }
    D3D12_RESOURCE_DESC desc = texture->resource->GetDesc();
    Pending p = {texture.get()};
    UINT rows;
    UINT64 row_size, total;
    device_->GetCopyableFootprints(&desc, 0, 1, 0, &p.footprint, &rows, &row_size, &total);
    // One readback buffer per destination, kept (creating them every frame
    // cost more than the rest of the frame).
    if (texture->readback) {
      p.readback = texture->readback;
    }
    D3D12_HEAP_PROPERTIES readback_heap = {D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC buffer_desc = {};
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Width = total;
    buffer_desc.Height = 1;
    buffer_desc.DepthOrArraySize = 1;
    buffer_desc.MipLevels = 1;
    buffer_desc.SampleDesc.Count = 1;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (!p.readback &&
        FAILED(device_->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&p.readback)))) {
      continue;
    }
    texture->readback = p.readback;
    Transition(texture->resource.Get(), texture->state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = texture->resource.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = p.readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = p.footprint;
    list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::memcpy(p.fetch, texture->guest_fetch, sizeof(p.fetch));
    // The guest's size; at a scale, each guest texel is the mean of its block.
    p.width = texture->width / scale_;
    p.height = texture->height / scale_;
    p.texture = nullptr;
    pending.push_back(std::move(p));
  }
  const uint32_t count = uint32_t(pending.size());
  if (!count) {
    return 0;
  }
  // Into guest memory once the GPU is done, the way the GPU writes a
  // resolve: in the destination texture's layout (tiled, 32 texels per row
  // of tiles) and byte order.
  const uint32_t scale = scale_;
  AfterCompletion([this, guest_memory, scale, pending = std::move(pending)]() mutable {
  for (Pending& p : pending) {
    struct {
      uint32_t width, height;
    } t = {p.width, p.height};
    xenos::xe_gpu_texture_fetch_t fetch;
    std::memcpy(&fetch, p.fetch, sizeof(fetch));
    const uint32_t base = GuestToPhysical(p.fetch[1] & 0xFFFFF000);
    const uint32_t pitch_texels = std::max<uint32_t>(fetch.pitch, 1) * 32;
    if (!Readable(base, pitch_texels * ((t.height + 31) & ~31u) * 4)) {
      continue;
    }
    const uint8_t* data;
    D3D12_RANGE range = {0, size_t(p.footprint.Footprint.RowPitch) * t.height * scale};
    if (FAILED(p.readback->Map(0, &range, reinterpret_cast<void**>(const_cast<uint8_t**>(&data))))) {
      continue;
    }
    for (uint32_t y = 0; y < t.height; ++y) {
      for (uint32_t x = 0; x < t.width; ++x) {
        const uint32_t offset =
            fetch.tiled ? uint32_t(texture_util::GetTiledOffset2D(int32_t(x), int32_t(y),
                                                                 pitch_texels, 2))
                        : (y * pitch_texels + x) * 4;
        if (uint64_t(base) + offset + 4 > kSharedMemorySize) {
          continue;
        }
        // Host R8G8B8A8 back into the guest's byte order.
        uint32_t sum[4] = {};
        for (uint32_t sy = 0; sy < scale; ++sy) {
          const uint8_t* row =
              data + size_t(y * scale + sy) * p.footprint.Footprint.RowPitch + 4 * x * scale;
          for (uint32_t sx = 0; sx < scale; ++sx) {
            for (uint32_t c = 0; c < 4; ++c) {
              sum[c] += row[4 * sx + c];
            }
          }
        }
        uint8_t texel[4];
        for (uint32_t c = 0; c < 4; ++c) {
          texel[c] = uint8_t((sum[c] + scale * scale / 2) / (scale * scale));
        }
        uint8_t* out = guest_memory + base + offset;
        switch (fetch.endianness) {
          case xenos::Endian::k8in32:
            out[0] = texel[3], out[1] = texel[2], out[2] = texel[1], out[3] = texel[0];
            break;
          case xenos::Endian::k16in32:
            out[0] = texel[2], out[1] = texel[3], out[2] = texel[0], out[3] = texel[1];
            break;
          case xenos::Endian::k8in16:
            out[0] = texel[1], out[1] = texel[0], out[2] = texel[3], out[3] = texel[2];
            break;
          default:
            std::memcpy(out, texel, 4);
        }
      }
    }
    D3D12_RANGE none = {0, 0};
    p.readback->Unmap(0, &none);
  }
  });
  return count;
}

// ---------------------------------------------------------------------------
// Occlusion queries (the guest's ZPD events), the way the SDK counts them: a
// continuous sample counter, written at every event to the event's report
// address; the game subtracts two reports. Between two events the samples
// are measured with host occlusion queries (one per command list the interval
// spans), resolved into a ring of readback slots.

bool Renderer::EnsureOcclusionQueries() {
  if (occlusion_heap_) {
    return true;
  }
  D3D12_QUERY_HEAP_DESC heap_desc = {};
  heap_desc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
  heap_desc.Count = kOcclusionSlots;
  D3D12_HEAP_PROPERTIES readback_heap = {D3D12_HEAP_TYPE_READBACK};
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = kOcclusionSlots * sizeof(uint64_t);
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(device_->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&occlusion_heap_))) ||
      FAILED(device_->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&occlusion_readback_)))) {
    occlusion_heap_.Reset();
    return false;
  }
  D3D12_RANGE range = {0, size_t(desc.Width)};
  void* mapping;
  occlusion_readback_->Map(0, &range, &mapping);
  occlusion_mapping_ = static_cast<const uint64_t*>(mapping);
  return true;
}

void Renderer::OcclusionBegin() {
  if (!occlusion_heap_ || occlusion_open_) {
    return;
  }
  occlusion_slot_ = uint32_t(occlusion_next_slot_++ % kOcclusionSlots);
  occlusion_ratio_[occlusion_slot_] = 1.0f;
  list_->BeginQuery(occlusion_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, occlusion_slot_);
  occlusion_open_ = true;
}

void Renderer::OcclusionEnd() {
  if (!occlusion_open_) {
    return;
  }
  list_->EndQuery(occlusion_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, occlusion_slot_);
  list_->ResolveQueryData(occlusion_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, occlusion_slot_, 1,
                          occlusion_readback_.Get(), sizeof(uint64_t) * occlusion_slot_);
  occlusion_interval_.push_back(occlusion_slot_);
  occlusion_open_ = false;
}

void Renderer::OcclusionEvent(const uint32_t* addresses, uint32_t count, uint8_t* guest_memory) {
  if (!EnsureOcclusionQueries()) {
    return;
  }
  OcclusionEnd();
  OcclusionReport report;
  report.slots = std::move(occlusion_interval_);
  occlusion_interval_.clear();
  report.ratios.reserve(report.slots.size());
  for (uint32_t slot : report.slots) {
    report.ratios.push_back(occlusion_ratio_[slot]);
  }
  report.addresses.assign(addresses, addresses + count);
  // Delivered in order once the GPU is done with what was recorded so far.
  AfterCompletion([this, guest_memory, report = std::move(report)]() {
    uint64_t samples = 0;
    for (size_t i = 0; i < report.slots.size(); ++i) {
      samples += uint64_t(double(occlusion_mapping_[report.slots[i]]) * report.ratios[i] + 0.5);
    }
    // In the guest's samples: at a scale every guest pixel is scale^2 host ones.
    samples /= uint64_t(scale_) * scale_;
    occlusion_counter_ += samples;
    stats_.occlusion_samples += samples;
    ++stats_.occlusion_reports;
    for (size_t i = 0; i < report.addresses.size(); ++i) {
      // The first report gets the counter; the others (predicated tiling,
      // one per tile) a constant, so their begin and end differ by nothing.
      const uint32_t value = i ? 0u : uint32_t(occlusion_counter_);
      const uint32_t address = report.addresses[i];
      if (!address || uint64_t(address) + 32 > kSharedMemorySize || !Readable(address, 32)) {
        continue;
      }
      // xe_gpu_depth_sample_counts: Total_A/B, ZFail_A/B, ZPass_A/B,
      // StencilFail_A/B, little-endian (the guest's D3D swaps them).
      uint32_t counts[8] = {value, 0, 0, 0, value, 0, 0, 0};
      std::memcpy(guest_memory + address, counts, sizeof(counts));
    }
  });
  OcclusionBegin();
}

}  // namespace replay
