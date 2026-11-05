#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers_dlssg.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
constexpr UINT wm_status = WM_APP + 1;
constexpr DXGI_FORMAT rgba = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT bgra = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr DXGI_FORMAT flow = DXGI_FORMAT_R16G16_FLOAT;
constexpr DXGI_FORMAT zfmt = DXGI_FORMAT_R32_FLOAT;
constexpr wchar_t app_class[] = L"ExternalDLSSGControl";
constexpr wchar_t output_class[] = L"ExternalDLSSGOutput";

void check(HRESULT hr, const char* label) {
  if (FAILED(hr)) {
    char buffer[200];
    std::snprintf(buffer, sizeof(buffer), "%s (HRESULT 0x%08X)", label,
                  static_cast<unsigned>(hr));
    throw std::runtime_error(buffer);
  }
}

void checkngx(NVSDK_NGX_Result result, const char* label) {
  if (NVSDK_NGX_FAILED(result)) {
    char buffer[200];
    std::snprintf(buffer, sizeof(buffer), "%s (NGX 0x%08X)", label,
                  static_cast<unsigned>(result));
    throw std::runtime_error(buffer);
  }
}

std::wstring widen(const std::string& message) {
  if (message.empty()) return {};
  const int count = MultiByteToWideChar(CP_UTF8, 0, message.c_str(), -1, nullptr, 0);
  std::wstring result(static_cast<size_t>(count), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, message.c_str(), -1, result.data(), count);
  result.resize(result.size() - 1);
  return result;
}

struct Gpu {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  HANDLE event = nullptr;
  UINT64 value = 0;

  explicit Gpu(IDXGIAdapter1* adapter) {
    check(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0,
                            IID_PPV_ARGS(&device)), "D3D12CreateDevice");
    D3D12_COMMAND_QUEUE_DESC q{};
    q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          IID_PPV_ARGS(&allocator)), "CreateCommandAllocator");
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                    allocator.Get(), nullptr, IID_PPV_ARGS(&list)),
          "CreateCommandList");
    check(list->Close(), "initial list close");
    check(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED,
                              IID_PPV_ARGS(&fence)), "CreateFence");
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) throw std::runtime_error("CreateEvent failed");
  }
  ~Gpu() { if (event) CloseHandle(event); }
  void begin() {
    check(allocator->Reset(), "command allocator reset");
    check(list->Reset(allocator.Get(), nullptr), "command list reset");
  }
  void flush() {
    check(list->Close(), "command list close");
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    check(queue->Signal(fence.Get(), ++value), "signal fence");
    if (fence->GetCompletedValue() < value) {
      check(fence->SetEventOnCompletion(value, event), "fence event");
      WaitForSingleObject(event, INFINITE);
    }
  }
  static void barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res,
                      D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    if (from == to) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    cmd->ResourceBarrier(1, &b);
  }
  ComPtr<ID3D12Resource> texture(UINT width, UINT height, DXGI_FORMAT format,
                                  D3D12_RESOURCE_FLAGS flags,
                                  D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> result;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state,
                                           nullptr, IID_PPV_ARGS(&result)),
          "CreateCommittedResource");
    return result;
  }
};

struct Screen {
  ComPtr<IDXGIAdapter1> adapter;
  ComPtr<IDXGIOutput1> output;
  DXGI_OUTPUT_DESC desc{};
  RECT game{};
};

Screen select_screen(HWND game_hwnd) {
  RECT client{};
  GetClientRect(game_hwnd, &client);
  POINT top_left{0, 0};
  ClientToScreen(game_hwnd, &top_left);
  RECT game{top_left.x, top_left.y, top_left.x + client.right,
            top_left.y + client.bottom};
  const POINT center{(game.left + game.right) / 2, (game.top + game.bottom) / 2};

  ComPtr<IDXGIFactory6> factory;
  check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
  for (UINT a = 0;; ++a) {
    ComPtr<IDXGIAdapter1> adapter;
    const HRESULT hr = factory->EnumAdapters1(a, &adapter);
    if (hr == DXGI_ERROR_NOT_FOUND) break;
    check(hr, "EnumAdapters1");
    DXGI_ADAPTER_DESC1 adapter_desc{};
    adapter->GetDesc1(&adapter_desc);
    if (adapter_desc.VendorId != 0x10DE) continue;
    for (UINT o = 0;; ++o) {
      ComPtr<IDXGIOutput> output;
      const HRESULT found = adapter->EnumOutputs(o, &output);
      if (found == DXGI_ERROR_NOT_FOUND) break;
      check(found, "EnumOutputs");
      DXGI_OUTPUT_DESC desc{};
      check(output->GetDesc(&desc), "output description");
      if (!PtInRect(&desc.DesktopCoordinates, center)) continue;
      ComPtr<IDXGIOutput1> output1;
      check(output.As(&output1), "IDXGIOutput1");
      return {adapter, output1, desc, game};
    }
  }
  throw std::runtime_error("Game is not on an NVIDIA-connected display");
}

struct Duplication {
  ComPtr<ID3D11Device> device11;
  ComPtr<ID3D11Device5> device5;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11DeviceContext4> context4;
  ComPtr<IDXGIOutputDuplication> duplicate;
  ComPtr<ID3D11Texture2D> shared11;
  ComPtr<ID3D12Resource> shared12;
  ComPtr<ID3D12Fence> capture_fence12;
  ComPtr<ID3D11Fence> shared_fence11;
  UINT64 capture_value = 0;
  UINT desktop_w = 0, desktop_h = 0;
  HANDLE shared_handle = nullptr;
  HANDLE fence_handle = nullptr;

  Duplication(const Screen& screen, Gpu& gpu) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL level{};
    check(D3D11CreateDevice(screen.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                            flags, nullptr, 0, D3D11_SDK_VERSION, &device11,
                            &level, &context), "D3D11CreateDevice");
    check(device11.As(&device5), "ID3D11Device5");
    check(context.As(&context4), "ID3D11DeviceContext4");
    check(screen.output->DuplicateOutput(device11.Get(), &duplicate),
          "DuplicateOutput (borderless mode recommended)");
    const auto bounds = screen.desc.DesktopCoordinates;
    desktop_w = static_cast<UINT>(bounds.right - bounds.left);
    desktop_h = static_cast<UINT>(bounds.bottom - bounds.top);
    D3D11_TEXTURE2D_DESC texture{};
    texture.Width = desktop_w;
    texture.Height = desktop_h;
    texture.MipLevels = 1;
    texture.ArraySize = 1;
    texture.Format = bgra;
    texture.SampleDesc.Count = 1;
    texture.Usage = D3D11_USAGE_DEFAULT;
    texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    texture.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    check(device11->CreateTexture2D(&texture, nullptr, &shared11),
          "Create shared D3D11 capture texture");
    ComPtr<IDXGIResource1> dxgi_resource;
    check(shared11.As(&dxgi_resource), "IDXGIResource1");
    check(dxgi_resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ |
                                           DXGI_SHARED_RESOURCE_WRITE, nullptr,
                                           &shared_handle), "CreateSharedHandle(texture)");
    check(gpu.device->OpenSharedHandle(shared_handle, IID_PPV_ARGS(&shared12)),
          "OpenSharedHandle(texture)");
    check(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_SHARED,
                                  IID_PPV_ARGS(&capture_fence12)), "capture fence");
    check(gpu.device->CreateSharedHandle(capture_fence12.Get(), nullptr, GENERIC_ALL,
                                         nullptr, &fence_handle), "CreateSharedHandle(fence)");
    check(device5->OpenSharedFence(fence_handle, IID_PPV_ARGS(&shared_fence11)),
          "OpenSharedFence");
  }
  ~Duplication() {
    if (shared_handle) CloseHandle(shared_handle);
    if (fence_handle) CloseHandle(fence_handle);
  }
  bool next(Gpu& gpu) {
    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> frame;
    const HRESULT hr = duplicate->AcquireNextFrame(5, &info, &frame);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
    check(hr, "AcquireNextFrame");
    // Pointer-only changes should not generate synthetic game frames.
    if (info.LastPresentTime.QuadPart == 0) {
      check(duplicate->ReleaseFrame(), "Release pointer-only frame");
      return false;
    }
    try {
      ComPtr<ID3D11Texture2D> texture;
      check(frame.As(&texture), "captured texture");
      context->CopyResource(shared11.Get(), texture.Get());
      check(context4->Signal(shared_fence11.Get(), ++capture_value),
            "D3D11 Signal(shared fence)");
      check(gpu.queue->Wait(capture_fence12.Get(), capture_value), "D3D12 Wait capture");
    } catch (...) {
      duplicate->ReleaseFrame();
      throw;
    }
    check(duplicate->ReleaseFrame(), "ReleaseFrame");
    return true;
  }
};

// A simple device-local optical-flow estimator. Modes:
// 0 = zero motion; 1 = uniform global search; 2 = 8x8 local search.
// It is deliberately labeled approximate. It is NOT NVIDIA's OFA engine.
constexpr char motion_cs[] = R"HLSL(
Texture2D<float4> frameNow : register(t0);
Texture2D<float4> framePrev : register(t1);
RWTexture2D<float4> colorOut : register(u0);
RWTexture2D<float2> flowOut : register(u1);
RWTexture2D<float> depthOut : register(u2);
cbuffer Settings : register(b0) {
 uint2 size;
 uint2 offset;
 uint motionMode;
 uint firstFrame;
 uint2 desktop;
};
float lum(float4 c) { return dot(c.rgb, float3(.2126, .7152, .0722)); }
float sampleGray(Texture2D<float4> tex, int2 p) {
 p = clamp(p, int2(0,0), int2(desktop) - 1);
 return lum(tex.Load(int3(p,0)));
}
groupshared float2 blockMotion;
[numthreads(8,8,1)]
void main(uint3 tid : SV_DispatchThreadID,
          uint3 local : SV_GroupThreadID,
          uint3 group : SV_GroupID) {
 int2 xy = int2(tid.xy);
 if (local.x == 0 && local.y == 0) {
  blockMotion = float2(0,0);
  if (firstFrame == 0 && motionMode != 0) {
   int2 base = motionMode == 1 ? int2(size / 2) : int2(group.xy)*8 + 4;
   base += int2(offset);
   float best = 1e10;
   int2 chosen = int2(0,0);
   [loop] for (int dy = -8; dy <= 8; dy += 4) {
     [loop] for (int dx = -8; dx <= 8; dx += 4) {
       int2 delta = int2(dx,dy);
       float error = 0.001f * (abs(dx) + abs(dy));
       [unroll] for (int j=0; j<2; ++j) {
         [unroll] for (int i=0; i<2; ++i) {
           int2 q = base + int2(i*3,j*3);
           float a = sampleGray(frameNow, q);
           float b = sampleGray(framePrev, q + delta);
           error += abs(a-b);
         }
       }
       if (error < best) { best=error; chosen=delta; }
     }
   }
   if (best < 0.17f) blockMotion = float2(chosen);
  }
 }
 GroupMemoryBarrierWithGroupSync();
 if (xy.x >= int(size.x) || xy.y >= int(size.y)) return;
 int2 full = xy + int2(offset);
 colorOut[xy] = frameNow.Load(int3(full,0));
 depthOut[xy] = 1.0f;
 flowOut[xy] = blockMotion;
}
)HLSL";

class ShaderInput {
  Gpu& gpu_;
  UINT width_, height_, x_, y_, desktop_w_, desktop_h_;
  ComPtr<ID3D12Resource> prev_, color_, motion_, depth_, output_;
  ComPtr<ID3D12RootSignature> signature_;
  ComPtr<ID3D12PipelineState> pipeline_;
  ComPtr<ID3D12DescriptorHeap> descriptors_;
  bool first_ = true;

  static D3D12_CPU_DESCRIPTOR_HANDLE handle(ID3D12DescriptorHeap* heap,
                                           UINT stride, UINT i) {
    auto result = heap->GetCPUDescriptorHandleForHeapStart();
    result.ptr += static_cast<SIZE_T>(stride) * i;
    return result;
  }
public:
  ShaderInput(Gpu& gpu, const Duplication& capture,
              UINT w, UINT h, UINT x, UINT y)
      : gpu_(gpu), width_(w), height_(h), x_(x), y_(y),
        desktop_w_(capture.desktop_w), desktop_h_(capture.desktop_h) {
    prev_ = gpu.texture(desktop_w_, desktop_h_, bgra, D3D12_RESOURCE_FLAG_NONE,
                        D3D12_RESOURCE_STATE_COPY_DEST);
    color_ = gpu.texture(w, h, rgba, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    motion_ = gpu.texture(w, h, flow, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    depth_ = gpu.texture(w, h, zfmt, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    output_ = gpu.texture(w, h, rgba, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0};
    ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0, 0};
    D3D12_ROOT_PARAMETER roots[3]{};
    roots[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    roots[0].Constants = {0, 0, 8};
    roots[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    roots[1].DescriptorTable = {1, &ranges[0]};
    roots[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    roots[2].DescriptorTable = {1, &ranges[1]};
    for (auto& root : roots) root.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rs{3, roots, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> signature, errors;
    check(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &signature,
                                       &errors), "D3D12SerializeRootSignature");
    check(gpu.device->CreateRootSignature(0, signature->GetBufferPointer(),
                                          signature->GetBufferSize(),
                                          IID_PPV_ARGS(&signature_)),
          "CreateRootSignature");
    ComPtr<ID3DBlob> cs;
    const HRESULT hr = D3DCompile(motion_cs, sizeof(motion_cs) - 1, nullptr, nullptr,
                                  nullptr, "main", "cs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3,
                                  0, &cs, &errors);
    if (FAILED(hr)) {
      const char* details = errors ? static_cast<const char*>(errors->GetBufferPointer())
                                   : "unknown HLSL error";
      throw std::runtime_error(std::string("motion shader compile: ") + details);
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = signature_.Get();
    pso.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
    check(gpu.device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipeline_)),
          "CreateComputePipelineState");
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = 5;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    check(gpu.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&descriptors_)),
          "CreateDescriptorHeap");
    const UINT stride = gpu.device->GetDescriptorHandleIncrementSize(heap.Type);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = bgra;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    gpu.device->CreateShaderResourceView(capture.shared12.Get(), &srv,
                                          handle(descriptors_.Get(), stride, 0));
    gpu.device->CreateShaderResourceView(prev_.Get(), &srv,
                                          handle(descriptors_.Get(), stride, 1));
    for (UINT i = 0; i < 3; ++i) {
      auto* resource = i == 0 ? color_.Get() : (i == 1 ? motion_.Get() : depth_.Get());
      D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
      uav.Format = i == 0 ? rgba : (i == 1 ? flow : zfmt);
      uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
      gpu.device->CreateUnorderedAccessView(resource, nullptr, &uav,
                                             handle(descriptors_.Get(), stride, i+2));
    }
  }

  bool first_frame() const { return first_; }
  UINT width() const { return width_; }
  UINT height() const { return height_; }
  ID3D12Resource* color() const { return color_.Get(); }
  ID3D12Resource* motion() const { return motion_.Get(); }
  ID3D12Resource* depth() const { return depth_.Get(); }
  ID3D12Resource* output() const { return output_.Get(); }

  void compute(ID3D12Resource* captured, unsigned mode) {
    gpu_.begin();
    auto* cmd = gpu_.list.Get();
    Gpu::barrier(cmd, captured, D3D12_RESOURCE_STATE_COMMON,
                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (first_) Gpu::barrier(cmd, prev_.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12DescriptorHeap* heaps[] = {descriptors_.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(signature_.Get());
    cmd->SetPipelineState(pipeline_.Get());
    const UINT stride = gpu_.device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const UINT parameters[] = {width_, height_, x_, y_, mode, first_ ? 1U : 0U,
                               desktop_w_, desktop_h_};
    cmd->SetComputeRoot32BitConstants(0, 8, parameters, 0);
    auto address = descriptors_->GetGPUDescriptorHandleForHeapStart();
    cmd->SetComputeRootDescriptorTable(1, address);
    address.ptr += static_cast<UINT64>(stride) * 2;
    cmd->SetComputeRootDescriptorTable(2, address);
    cmd->Dispatch((width_+7)/8, (height_+7)/8, 1);
    Gpu::barrier(cmd, captured, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                 D3D12_RESOURCE_STATE_COPY_SOURCE);
    Gpu::barrier(cmd, prev_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                 D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyResource(prev_.Get(), captured);
    Gpu::barrier(cmd, captured, D3D12_RESOURCE_STATE_COPY_SOURCE,
                 D3D12_RESOURCE_STATE_COMMON);
    Gpu::barrier(cmd, prev_.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    for (ID3D12Resource* resource : {color_.Get(), motion_.Get(), depth_.Get()})
      Gpu::barrier(cmd, resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    gpu_.flush();
    first_ = false;
  }
  void back_to_compute() {
    gpu_.begin();
    for (ID3D12Resource* resource : {color_.Get(), motion_.Get(), depth_.Get()})
      Gpu::barrier(gpu_.list.Get(), resource,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    gpu_.flush();
  }
};

class Ngx {
  Gpu& gpu_;
  ShaderInput& frames_;
  NVSDK_NGX_Parameter* capabilities_ = nullptr;
  NVSDK_NGX_Parameter* params_ = nullptr;
  NVSDK_NGX_Handle* feature_ = nullptr;
  bool initialized_ = false;
  unsigned count_ = 1;
public:
  Ngx(Gpu& gpu, ShaderInput& input, unsigned requested)
      : gpu_(gpu), frames_(input) {
    const wchar_t* path = L".";
    checkngx(NVSDK_NGX_D3D12_Init_with_ProjectID(
                 "dd3e1976-fd7a-4cc8-9cef-9d45f8d13c71",
                 NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0.0",
                 path, gpu_.device.Get(), nullptr), "NGX initialization");
    initialized_ = true;
    checkngx(NVSDK_NGX_D3D12_GetCapabilityParameters(&capabilities_),
             "NGX capabilities");
    unsigned available = 0, maximum = 1;
    NVSDK_NGX_Parameter_GetUI(capabilities_, NVSDK_NGX_Parameter_FrameGeneration_Available,
                              &available);
    NVSDK_NGX_Parameter_GetUI(capabilities_, NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax,
                              &maximum);
    if (!available) throw std::runtime_error(
        "NVIDIA NGX reports Frame Generation unavailable (GPU, driver or DLL)");
    count_ = std::min(requested, std::max(1u, maximum));
    gpu_.begin();
    checkngx(NVSDK_NGX_D3D12_AllocateParameters(&params_), "NGX allocate parameters");
    NVSDK_NGX_Parameter_SetUI(params_, NVSDK_NGX_Parameter_CreationNodeMask, 1);
    NVSDK_NGX_Parameter_SetUI(params_, NVSDK_NGX_Parameter_VisibilityNodeMask, 1);
    NVSDK_NGX_Parameter_SetUI(params_, NVSDK_NGX_DLSSG_Parameter_Width, frames_.width());
    NVSDK_NGX_Parameter_SetUI(params_, NVSDK_NGX_DLSSG_Parameter_Height, frames_.height());
    NVSDK_NGX_Parameter_SetUI(params_, NVSDK_NGX_DLSSG_Parameter_BackbufferFormat, rgba);
    NVSDK_NGX_Parameter_SetUI(params_, NVSDK_NGX_DLSSG_Parameter_InternalWidth, frames_.width());
    NVSDK_NGX_Parameter_SetUI(params_, NVSDK_NGX_DLSSG_Parameter_InternalHeight, frames_.height());
    NVSDK_NGX_Parameter_SetUI(params_, NVSDK_NGX_DLSSG_Parameter_DynamicResolution, 0);
    checkngx(NVSDK_NGX_D3D12_CreateFeature(gpu_.list.Get(),
                                           NVSDK_NGX_Feature_FrameGeneration,
                                           params_, &feature_), "NGX create FrameGeneration");
    gpu_.flush();
  }
  ~Ngx() {
    if (feature_) NVSDK_NGX_D3D12_ReleaseFeature(feature_);
    if (params_) NVSDK_NGX_D3D12_DestroyParameters(params_);
    if (capabilities_) NVSDK_NGX_D3D12_DestroyParameters(capabilities_);
    if (initialized_) NVSDK_NGX_D3D12_Shutdown1(gpu_.device.Get());
  }
  unsigned generated_count() const { return count_; }
  void evaluate(unsigned index, bool reset) {
    gpu_.begin();
    NVSDK_NGX_D3D12_DLSSG_Eval_Params resources{};
    resources.pBackbuffer = frames_.color();
    resources.pDepth = frames_.depth();
    resources.pMVecs = frames_.motion();
    resources.pHudless = frames_.color();
    resources.pOutputInterpFrame = frames_.output();
    NVSDK_NGX_DLSSG_Opt_Eval_Params options{};
    options.multiFrameCount = count_;
    options.multiFrameIndex = index;
    for (auto* matrix : {&options.cameraViewToClip, &options.clipToCameraView,
                          &options.clipToLensClip, &options.clipToPrevClip,
                          &options.prevClipToClip}) {
      (*matrix)[0][0] = (*matrix)[1][1] = (*matrix)[2][2] = (*matrix)[3][3] = 1.f;
    }
    options.mvecScale[0] = options.mvecScale[1] = 1.f;
    options.cameraUp[1] = options.cameraRight[0] = options.cameraFwd[2] = 1.f;
    options.cameraNear = .1f;
    options.cameraFar = 1000.f;
    options.cameraFOV = 1.0471975512f;
    options.cameraAspectRatio = float(frames_.width()) / float(frames_.height());
    options.cameraMotionIncluded = true;
    options.motionVectorsDilated = true;
    options.reset = reset;
    options.menuDetectionEnabled = false;
    options.mvecsSubrectSize = {frames_.width(), frames_.height()};
    options.depthSubrectSize = {frames_.width(), frames_.height()};
    options.hudLessSubrectSize = {frames_.width(), frames_.height()};
    options.backbufferSubrectSize = {frames_.width(), frames_.height()};
    options.outputInterpSubrectSize = {frames_.width(), frames_.height()};
    checkngx(NGX_D3D12_EVALUATE_DLSSG(gpu_.list.Get(), feature_, params_,
                                       &resources, &options), "NGX evaluate");
    gpu_.flush();
  }
};

LRESULT CALLBACK output_proc(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
  if (message == WM_NCHITTEST) return HTTRANSPARENT;
  if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
  return DefWindowProcW(hwnd, message, w, l);
}

class Presenter {
  Gpu& gpu_;
  HWND hwnd_ = nullptr;
  ComPtr<IDXGISwapChain3> swap_;
  std::array<ComPtr<ID3D12Resource>, 2> buffers_{};
  HANDLE frame_ready_ = nullptr;
  bool excluded_ = false;
public:
  Presenter(Gpu& gpu, HWND game, UINT w, UINT h)
      : gpu_(gpu) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = output_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = output_class;
    RegisterClassExW(&wc);
    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
                           WS_EX_TOOLWINDOW, output_class, L"External DLSS-G output",
                           WS_POPUP, 0, 0, int(w), int(h), nullptr, nullptr,
                           GetModuleHandleW(nullptr), nullptr);
    if (!hwnd_) throw std::runtime_error("Create overlay window failed");
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = w;
    desc.Height = h;
    desc.Format = rgba;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING |
                 DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    ComPtr<IDXGIFactory4> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "swapchain factory");
    ComPtr<IDXGISwapChain1> base;
    check(factory->CreateSwapChainForHwnd(gpu.queue.Get(), hwnd_, &desc,
                                           nullptr, nullptr, &base),
          "CreateSwapChainForHwnd");
    check(base.As(&swap_), "IDXGISwapChain3");
    ComPtr<IDXGISwapChain2> frame_latency_swapchain;
    check(swap_.As(&frame_latency_swapchain), "IDXGISwapChain2");
    check(frame_latency_swapchain->SetMaximumFrameLatency(1),
          "SetMaximumFrameLatency(1)");
    frame_ready_ = frame_latency_swapchain->GetFrameLatencyWaitableObject();
    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    for (UINT i=0; i<2; ++i)
      check(swap_->GetBuffer(i, IID_PPV_ARGS(&buffers_[i])), "swapchain buffer");
    // WDA_EXCLUDEFROMCAPTURE prevents the overlay becoming its own input.
    excluded_ = SetWindowDisplayAffinity(hwnd_, 0x00000011) != 0;
    if (!excluded_) throw std::runtime_error(
        "SetWindowDisplayAffinity(EXCLUDEFROMCAPTURE) failed: refusing recursive capture");
    RECT rc{};
    GetClientRect(game, &rc);
    POINT p{0,0};
    ClientToScreen(game, &p);
    SetWindowPos(hwnd_, HWND_TOPMOST, p.x, p.y, int(w), int(h),
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
  }
  ~Presenter() {
    if (frame_ready_) CloseHandle(frame_ready_);
    if (hwnd_) DestroyWindow(hwnd_);
  }
  void present(ID3D12Resource* source, D3D12_RESOURCE_STATES state) {
    if (frame_ready_) WaitForSingleObject(frame_ready_, 16);
    const UINT index = swap_->GetCurrentBackBufferIndex();
    auto* target = buffers_[index].Get();
    gpu_.begin();
    auto* cmd = gpu_.list.Get();
    Gpu::barrier(cmd, source, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Gpu::barrier(cmd, target, D3D12_RESOURCE_STATE_PRESENT,
                 D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyResource(target, source);
    Gpu::barrier(cmd, target, D3D12_RESOURCE_STATE_COPY_DEST,
                 D3D12_RESOURCE_STATE_PRESENT);
    Gpu::barrier(cmd, source, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    gpu_.flush();
    check(swap_->Present(0, DXGI_PRESENT_ALLOW_TEARING), "swapchain Present");
  }
  void hide() { ShowWindow(hwnd_, SW_HIDE); }
  void show(HWND game, UINT w, UINT h) {
    POINT point{0,0};
    ClientToScreen(game, &point);
    SetWindowPos(hwnd_, HWND_TOPMOST, point.x, point.y, int(w), int(h),
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
  }
  void pump() {
    MSG msg{};
    while (PeekMessageW(&msg, hwnd_, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
};

struct GameChoice {
  HWND hwnd{};
  DWORD pid{};
  std::wstring title;
};

BOOL CALLBACK list_game(HWND hwnd, LPARAM context) {
  if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
  const int length = GetWindowTextLengthW(hwnd);
  if (length < 2) return TRUE;
  wchar_t title[320]{};
  GetWindowTextW(hwnd, title, 320);
  DWORD pid=0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (!pid) return TRUE;
  auto* games = reinterpret_cast<std::vector<GameChoice>*>(context);
  games->push_back({hwnd, pid, title});
  return TRUE;
}

struct App {
  HWND window{}, game_combo{}, mode_combo{}, fg_combo{}, button{}, status{};
  std::vector<GameChoice> games;
  std::thread runner;
  std::atomic<bool> stop{false};
  std::atomic<bool> running{false};

  void refresh() {
    games.clear();
    EnumWindows(list_game, reinterpret_cast<LPARAM>(&games));
    SendMessageW(game_combo, CB_RESETCONTENT, 0, 0);
    for (const auto& item : games) {
      wchar_t name[420]{};
      _snwprintf_s(name, _TRUNCATE, L"%s  [PID %lu]", item.title.c_str(),
                   static_cast<unsigned long>(item.pid));
      SendMessageW(game_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
    }
    if (!games.empty()) SendMessageW(game_combo, CB_SETCURSEL, 0, 0);
  }
  void note(const std::wstring& text) {
    SetWindowTextW(status, text.c_str());
  }
  void run(HWND game, unsigned mode, unsigned generated) {
    try {
      const Screen screen = select_screen(game);
      const UINT w = static_cast<UINT>(screen.game.right-screen.game.left);
      const UINT h = static_cast<UINT>(screen.game.bottom-screen.game.top);
      const UINT x = static_cast<UINT>(screen.game.left-screen.desc.DesktopCoordinates.left);
      const UINT y = static_cast<UINT>(screen.game.top-screen.desc.DesktopCoordinates.top);
      if (!w || !h || w > 8192 || h > 8192 ||
          x+w > static_cast<UINT>(screen.desc.DesktopCoordinates.right-
                                  screen.desc.DesktopCoordinates.left) ||
          y+h > static_cast<UINT>(screen.desc.DesktopCoordinates.bottom-
                                  screen.desc.DesktopCoordinates.top))
        throw std::runtime_error("Game client area is outside the NVIDIA display");
      Gpu gpu(screen.adapter.Get());
      Duplication capture(screen, gpu);
      ShaderInput input(gpu, capture, w, h, x, y);
      Ngx ngx(gpu, input, generated);
      Presenter presenter(gpu, game, w, h);
      bool had_previous = false, overlay_visible = true;
      DEVMODEW mode_info{};
      mode_info.dmSize = sizeof(mode_info);
      double physical_hz = 240.0;
      if (EnumDisplaySettingsW(screen.desc.DeviceName, ENUM_CURRENT_SETTINGS, &mode_info) &&
          mode_info.dmDisplayFrequency >= 30)
        physical_hz = static_cast<double>(mode_info.dmDisplayFrequency);
      using Clock = std::chrono::steady_clock;
      auto previous_capture = Clock::now();
      auto last_present = Clock::now();
      while (!stop.load()) {
        presenter.pump();
        if (!IsWindow(game)) break;
        const bool active = GetForegroundWindow() == game;
        if (!active) {
          if (overlay_visible) { presenter.hide(); overlay_visible=false; }
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
          continue;
        }
        if (!overlay_visible) { presenter.show(game, w, h); overlay_visible=true; }
        if (!capture.next(gpu)) continue;
        const auto captured_at = Clock::now();
        const double source_interval = std::chrono::duration<double>(
            captured_at - previous_capture).count();
        previous_capture = captured_at;
        const double display_interval = std::max(1.0 / physical_hz,
            std::clamp(source_interval, 1.0/500.0, 1.0/15.0) /
            static_cast<double>(ngx.generated_count()+1));
        const auto interval = std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(display_interval));
        const auto paced_present = [&](ID3D12Resource* resource,
                                       D3D12_RESOURCE_STATES state) {
          const auto target = last_present + interval;
          if (target > Clock::now())
            std::this_thread::sleep_until(target);
          presenter.present(resource, state);
          last_present = Clock::now();
        };
        input.compute(capture.shared12.Get(), mode);
        if (!had_previous) {
          // Seed DLSS-G's internal history without displaying an interpolated frame.
          ngx.evaluate(1, true);
        } else {
          for (unsigned i=1; i<=ngx.generated_count(); ++i) {
            ngx.evaluate(i, false);
            paced_present(input.output(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
          }
        }
        paced_present(input.color(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        input.back_to_compute();
        had_previous = true;
      }
    } catch (const std::exception& e) {
      const auto error = widen(e.what());
      PostMessageW(window, wm_status, 1,
                   reinterpret_cast<LPARAM>(new std::wstring(error)));
    }
    running = false;
    PostMessageW(window, wm_status, 0, reinterpret_cast<LPARAM>(
        new std::wstring(L"Stopped")));
  }
  void toggle() {
    if (running.load()) {
      stop = true;
      note(L"Stopping...");
      return;
    }
    if (runner.joinable()) runner.join();
    const LRESULT choice = SendMessageW(game_combo, CB_GETCURSEL, 0, 0);
    if (choice == CB_ERR || size_t(choice) >= games.size()) {
      note(L"Select a game window first");
      return;
    }
    const HWND game = games[size_t(choice)].hwnd;
    const unsigned mode = static_cast<unsigned>(SendMessageW(mode_combo, CB_GETCURSEL,0,0));
    const unsigned generated = 1u + static_cast<unsigned>(
        SendMessageW(fg_combo, CB_GETCURSEL, 0, 0));
    stop = false;
    running = true;
    note(L"Starting NVIDIA NGX...");
    SetWindowTextW(button, L"Stop");
    runner = std::thread([this,game,mode,generated]{run(game,mode,generated);});
  }
  ~App() {
    stop = true;
    if (runner.joinable()) runner.join();
  }
};

LRESULT CALLBACK control_proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
  auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (message == WM_CREATE) {
    const auto* create = reinterpret_cast<LPCREATESTRUCTW>(lp);
    app = static_cast<App*>(create->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    app->window = hwnd;
    CreateWindowW(L"STATIC", L"Game window / PID:", WS_CHILD | WS_VISIBLE,
                  16, 16, 300, 20, hwnd, nullptr, nullptr, nullptr);
    app->game_combo = CreateWindowW(L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        16, 38, 460, 180, hwnd, reinterpret_cast<HMENU>(100), nullptr, nullptr);
    CreateWindowW(L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE,
                  482, 38, 82, 26, hwnd, reinterpret_cast<HMENU>(101), nullptr, nullptr);
    CreateWindowW(L"STATIC", L"Motion input:", WS_CHILD | WS_VISIBLE,
                  16, 85, 210, 20, hwnd, nullptr, nullptr, nullptr);
    app->mode_combo = CreateWindowW(L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
        16, 108, 300, 140, hwnd, reinterpret_cast<HMENU>(102), nullptr, nullptr);
    for (const auto* entry : {L"Zero motion", L"Global motion (GPU approximation)",
                               L"Shader motion (GPU 8x8)"}) {
      SendMessageW(app->mode_combo, CB_ADDSTRING,0,reinterpret_cast<LPARAM>(entry));
    }
    SendMessageW(app->mode_combo,CB_SETCURSEL,2,0);
    CreateWindowW(L"STATIC", L"DLSS-G:", WS_CHILD | WS_VISIBLE,
                  330, 85, 200, 20, hwnd, nullptr, nullptr, nullptr);
    app->fg_combo = CreateWindowW(L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
        330, 108, 180, 140, hwnd, reinterpret_cast<HMENU>(103), nullptr, nullptr);
    for (const auto* entry : {L"x2 (1 generated)", L"x3 (2 generated)", L"x4 (3 generated)"})
      SendMessageW(app->fg_combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(entry));
    SendMessageW(app->fg_combo,CB_SETCURSEL,0,0);
    app->button = CreateWindowW(L"BUTTON", L"Start", WS_CHILD | WS_VISIBLE,
                   16, 151, 120, 32, hwnd, reinterpret_cast<HMENU>(104), nullptr, nullptr);
    app->status = CreateWindowW(L"STATIC", L"Ready. Borderless game mode recommended.",
                  WS_CHILD | WS_VISIBLE, 16, 195, 535, 38,
                  hwnd, nullptr, nullptr, nullptr);
    CreateWindowW(L"STATIC",
      L"Experimental NGX app. NvOFA hardware motion is not integrated in this build.",
      WS_CHILD | WS_VISIBLE, 16, 244, 535, 35, hwnd, nullptr, nullptr, nullptr);
    app->refresh();
    return 0;
  }
  if (message == WM_COMMAND && app) {
    switch (LOWORD(wp)) {
      case 101: if (!app->running.load()) app->refresh(); break;
      case 104: app->toggle(); break;
    }
  } else if (message == wm_status && app) {
    std::unique_ptr<std::wstring> message_text(reinterpret_cast<std::wstring*>(lp));
    app->note(*message_text);
    if (!app->running.load()) SetWindowTextW(app->button, L"Start");
    if (wp == 1) MessageBoxW(hwnd, message_text->c_str(), L"External DLSS-G",
                              MB_OK | MB_ICONERROR);
    return 0;
  } else if (message == WM_DESTROY && app) {
    app->stop = true;
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(hwnd,message,wp,lp);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  try {
    if (wcsstr(GetCommandLineW(), L"--shader-test")) {
      ComPtr<ID3DBlob> compiled, errors;
      return SUCCEEDED(D3DCompile(motion_cs, sizeof(motion_cs) - 1, nullptr,
                                   nullptr, nullptr, "main", "cs_5_1",
                                   D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                   &compiled, &errors)) ? 0 : 2;
    }
    App app;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = control_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
    wc.lpszClassName = app_class;
    if (!RegisterClassExW(&wc)) throw std::runtime_error("RegisterClassEx");
    HWND main = CreateWindowExW(0,app_class,L"External DLSS-G (Experimental)",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT,CW_USEDEFAULT,595,335,
        nullptr,nullptr,instance,&app);
    if (!main) throw std::runtime_error("CreateWindowEx");
    ShowWindow(main,SW_SHOW);
    MSG msg{};
    while (GetMessageW(&msg,nullptr,0,0)>0) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    return 0;
  } catch (const std::exception& e) {
    MessageBoxW(nullptr,widen(e.what()).c_str(),L"External DLSS-G startup",
                MB_OK | MB_ICONERROR);
    return 1;
  }
}
