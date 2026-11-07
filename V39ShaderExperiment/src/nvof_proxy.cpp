// Experimental app-local NVOF driver proxy for the user's V39 executable.
// Original code: this file does not contain or derive from V39 sources.
// Only the mode selected through V39_MOTION=SHADER redirects NvOFExecute.
// The native NvOFA path is forwarded unchanged, through its official ABI.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;
using NvOFHandle = void*;
using NvOFGPUBufferHandle = void*;
using NV_OF_STATUS = int32_t;
static constexpr NV_OF_STATUS kSuccess = 0;
static constexpr NV_OF_STATUS kFailure = 1;

struct NV_OF_FENCE_POINT { ID3D12Fence* fence; uint64_t value; };
struct NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 {
    ID3D12Resource* resource;
    NV_OF_FENCE_POINT inputFencePoint;
    NvOFGPUBufferHandle* hOFGpuBuffer;
    NV_OF_FENCE_POINT outputFencePoint;
};
struct NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12 { NvOFGPUBufferHandle hOFGpuBuffer; };
struct NV_OF_EXECUTE_INPUT_PARAMS_D3D12 {
    NvOFGPUBufferHandle inputFrame;
    NvOFGPUBufferHandle referenceFrame;
    NvOFGPUBufferHandle externalHints;
    uint32_t disableTemporalHints;
    uint32_t padding;
    void* hPrivData;
    uint32_t padding2;
    uint32_t numRois;
    void* roiData;
    uint32_t padding3;
    uint32_t numFencePoints;
    NV_OF_FENCE_POINT* fencePoint;
};
struct NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 {
    NvOFGPUBufferHandle outputBuffer;
    NvOFGPUBufferHandle outputCostBuffer;
    void* hPrivData;
    NvOFGPUBufferHandle bwdOutputBuffer;
    NvOFGPUBufferHandle bwdOutputCostBuffer;
    NvOFGPUBufferHandle globalFlowBuffer;
    uint32_t padding[2];
    NV_OF_FENCE_POINT* fencePoint;
};
using CreateProc = NV_OF_STATUS(__stdcall*)(ID3D12Device*, NvOFHandle*);
using RegisterProc = NV_OF_STATUS(__stdcall*)(NvOFHandle, NV_OF_REGISTER_RESOURCE_PARAMS_D3D12*);
using UnregisterProc = NV_OF_STATUS(__stdcall*)(NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12*);
using ExecuteProc = NV_OF_STATUS(__stdcall*)(NvOFHandle, const NV_OF_EXECUTE_INPUT_PARAMS_D3D12*, NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12*);
using DestroyProc = NV_OF_STATUS(__stdcall*)(NvOFHandle);

struct NvofFunctionList {
    CreateProc create;
    void* init;
    void* surfaceFormatCount;
    void* surfaceFormat;
    RegisterProc registerResource;
    UnregisterProc unregisterResource;
    ExecuteProc execute;
    DestroyProc destroy;
    void* getLastError;
    void* getCaps;
};

using FactoryProc = NV_OF_STATUS(__stdcall*)(uint32_t, NvofFunctionList*);

struct Pending {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12DescriptorHeap> heap;
    uint64_t fenceValue{};
};

struct ShaderDevice {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> ownFence;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    uint64_t nextFence = 1;
    std::vector<Pending> pending;
};

static std::mutex gMutex;
static HMODULE gNative;
static NvofFunctionList gOriginal{};
static std::unordered_map<NvOFHandle, ShaderDevice> gDevices;
static std::unordered_map<NvOFGPUBufferHandle, ComPtr<ID3D12Resource>> gResources;
static bool gShaderMode = false;

static std::filesystem::path ApplicationDirectory() {
    std::array<wchar_t, MAX_PATH> path{};
    GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    return std::filesystem::path(path.data()).parent_path();
}

static void Diagnostic(const wchar_t* s) {
    OutputDebugStringW(L"[V39 Shader Proxy] ");
    OutputDebugStringW(s);
    OutputDebugStringW(L"\n");
}

static bool SetupCompute(ShaderDevice& state) {
    D3D12_COMMAND_QUEUE_DESC queue{};
    queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    if (FAILED(state.device->CreateCommandQueue(&queue, IID_PPV_ARGS(&state.queue)))) return false;
    if (FAILED(state.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&state.ownFence)))) return false;

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 2;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 2;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &ranges[0];
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &ranges[1];
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants.Num32BitValues = 4;
    params[2].Constants.ShaderRegister = 0;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 3;
    rs.pParameters = params;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ComPtr<ID3DBlob> blob, errors;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1,
                                           &blob, &errors))) return false;
    if (FAILED(state.device->CreateRootSignature(0, blob->GetBufferPointer(),
                                                blob->GetBufferSize(), IID_PPV_ARGS(&state.root)))) return false;

    const auto shaderPath = ApplicationDirectory() / L"block_motion.hlsl";
    blob.Reset();
    errors.Reset();
    HRESULT hr = D3DCompileFromFile(shaderPath.c_str(), nullptr,
                                    D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                    "CSMain", "cs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3,
                                    0, &blob, &errors);
    if (FAILED(hr)) {
        if (errors) OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));
        Diagnostic(L"Motion shader compilation failed. Check block_motion.hlsl.");
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = state.root.Get();
    desc.CS = {blob->GetBufferPointer(), blob->GetBufferSize()};
    return SUCCEEDED(state.device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&state.pipeline)));
}

static NV_OF_STATUS __stdcall CreateShim(ID3D12Device* device, NvOFHandle* out) {
    NV_OF_STATUS status = gOriginal.create(device, out);
    if (status != kSuccess || !out || !*out) return status;
    std::lock_guard<std::mutex> lock(gMutex);
    ShaderDevice state;
    state.device = device;
    if (!SetupCompute(state)) {
        Diagnostic(L"Cannot initialize D3D12 shader backend.");
        // Let native NVOFA cleanup the handle; do not silently fall back.
        gOriginal.destroy(*out);
        *out = nullptr;
        return kFailure;
    }
    gDevices.emplace(*out, std::move(state));
    return status;
}

static NV_OF_STATUS __stdcall RegisterShim(NvOFHandle handle, NV_OF_REGISTER_RESOURCE_PARAMS_D3D12* p) {
    NV_OF_STATUS status = gOriginal.registerResource(handle, p);
    if (status != kSuccess || !p || !p->resource || !p->hOFGpuBuffer || !*p->hOFGpuBuffer) return status;
    std::lock_guard<std::mutex> lock(gMutex);
    gResources[*p->hOFGpuBuffer] = p->resource;
    return status;
}

static NV_OF_STATUS __stdcall UnregisterShim(NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12* p) {
    if (p) {
        std::lock_guard<std::mutex> lock(gMutex);
        gResources.erase(p->hOFGpuBuffer);
    }
    return gOriginal.unregisterResource(p);
}

static void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* res,
                       D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &b);
}

static NV_OF_STATUS __stdcall ExecuteShim(NvOFHandle h,
                                          const NV_OF_EXECUTE_INPUT_PARAMS_D3D12* in,
                                          NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12* out) {
    if (!in || !out || !out->outputBuffer || !out->outputCostBuffer) return kFailure;
    std::lock_guard<std::mutex> lock(gMutex);
    auto d = gDevices.find(h);
    if (d == gDevices.end()) return kFailure;
    auto lookup = [](NvOFGPUBufferHandle key) -> ID3D12Resource* {
        auto i = gResources.find(key);
        return i == gResources.end() ? nullptr : i->second.Get();
    };
    ID3D12Resource* current = lookup(in->inputFrame);
    ID3D12Resource* previous = lookup(in->referenceFrame);
    ID3D12Resource* flow = lookup(out->outputBuffer);
    ID3D12Resource* cost = lookup(out->outputCostBuffer);
    if (!current || !previous || !flow || !cost) {
        Diagnostic(L"Unregistered input/output resource; no native fallback.");
        return kFailure;
    }
    const auto src = current->GetDesc();
    const auto ref = previous->GetDesc();
    const auto dst = flow->GetDesc();
    if (src.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        ref.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        dst.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        src.Width != ref.Width || src.Height != ref.Height ||
        flow->GetDesc().Format != DXGI_FORMAT_R16G16_SINT ||
        cost->GetDesc().Format != DXGI_FORMAT_R8_UINT ||
        dst.Width == 0 || dst.Height == 0) {
        Diagnostic(L"Unsupported NVOFA resource shape/format in shader mode.");
        return kFailure;
    }
    ShaderDevice& s = d->second;
    auto& pending = s.pending;
    pending.erase(std::remove_if(pending.begin(), pending.end(), [&](const Pending& p) {
        return s.ownFence->GetCompletedValue() >= p.fenceValue;
    }), pending.end());
    // Avoid CPU blocking on GPU and accidental use of allocator/heap in flight.
    if (pending.size() >= 4) {
        Diagnostic(L"Shader queue exceeded four outstanding frames.");
        return kFailure;
    }
    Pending p;
    if (FAILED(s.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                IID_PPV_ARGS(&p.allocator)))) return kFailure;
    if (FAILED(s.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           p.allocator.Get(), s.pipeline.Get(),
                                           IID_PPV_ARGS(&p.list)))) return kFailure;
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.NumDescriptors = 4;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(s.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&p.heap)))) return kFailure;
    const UINT increment = s.device->GetDescriptorHandleIncrementSize(hd.Type);
    const auto cpu = p.heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = src.Format;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    s.device->CreateShaderResourceView(current, &view, cpu);
    view.Format = ref.Format;
    D3D12_CPU_DESCRIPTOR_HANDLE at{cpu.ptr + increment};
    s.device->CreateShaderResourceView(previous, &view, at);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav.Format = DXGI_FORMAT_R16G16_SINT;
    at.ptr += increment;
    s.device->CreateUnorderedAccessView(flow, nullptr, &uav, at);
    uav.Format = DXGI_FORMAT_R8_UINT;
    at.ptr += increment;
    s.device->CreateUnorderedAccessView(cost, nullptr, &uav, at);

    auto* cmd = p.list.Get();
    Transition(cmd, current, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (previous != current)
        Transition(cmd, previous, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(cmd, flow, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(cmd, cost, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmd->SetPipelineState(s.pipeline.Get());
    cmd->SetComputeRootSignature(s.root.Get());
    ID3D12DescriptorHeap* heaps[] = {p.heap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    auto gpu = p.heap->GetGPUDescriptorHandleForHeapStart();
    cmd->SetComputeRootDescriptorTable(0, gpu);
    gpu.ptr += 2 * increment;
    cmd->SetComputeRootDescriptorTable(1, gpu);
    const uint32_t dims[4] = {uint32_t(src.Width), src.Height,
                              uint32_t(dst.Width), dst.Height};
    cmd->SetComputeRoot32BitConstants(2, 4, dims, 0);
    cmd->Dispatch((uint32_t(dst.Width) + 7) / 8, (dst.Height + 7) / 8, 1);
    D3D12_RESOURCE_BARRIER uavBarrier{};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = flow;
    cmd->ResourceBarrier(1, &uavBarrier);
    Transition(cmd, flow, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    Transition(cmd, cost, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    Transition(cmd, current, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    if (previous != current)
        Transition(cmd, previous, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    if (FAILED(cmd->Close())) return kFailure;

    for (uint32_t k = 0; k < in->numFencePoints; ++k) {
        if (in->fencePoint && in->fencePoint[k].fence &&
            FAILED(s.queue->Wait(in->fencePoint[k].fence, in->fencePoint[k].value))) return kFailure;
    }
    ID3D12CommandList* lists[] = {cmd};
    s.queue->ExecuteCommandLists(1, lists);
    if (out->fencePoint && out->fencePoint->fence &&
        FAILED(s.queue->Signal(out->fencePoint->fence, out->fencePoint->value))) return kFailure;
    p.fenceValue = s.nextFence++;
    if (FAILED(s.queue->Signal(s.ownFence.Get(), p.fenceValue))) return kFailure;
    pending.push_back(std::move(p));
    return kSuccess;
}

static NV_OF_STATUS __stdcall DestroyShim(NvOFHandle h) {
    {
        std::lock_guard<std::mutex> lock(gMutex);
        auto it = gDevices.find(h);
        if (it != gDevices.end()) {
            // Avoid releasing in-flight command allocators before the GPU has finished.
            const uint64_t last = it->second.nextFence - 1;
            if (last && it->second.ownFence->GetCompletedValue() < last) {
                HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (event) {
                    it->second.ownFence->SetEventOnCompletion(last, event);
                    WaitForSingleObject(event, 1000);
                    CloseHandle(event);
                }
            }
            gDevices.erase(it);
        }
    }
    return gOriginal.destroy(h);
}

extern "C" __declspec(dllexport) NV_OF_STATUS __stdcall
NvOFAPICreateInstanceD3D12(uint32_t version, NvofFunctionList* funcs) {
    if (!funcs) return kFailure;
    if (!gNative) {
        std::array<wchar_t, MAX_PATH> system{};
        if (!GetSystemDirectoryW(system.data(), DWORD(system.size()))) return kFailure;
        std::filesystem::path native = std::filesystem::path(system.data()) / L"nvofapi64.dll";
        gNative = LoadLibraryW(native.c_str());
        if (!gNative) {
            Diagnostic(L"NVIDIA system32 nvofapi64.dll not found (no fallback).");
            return kFailure;
        }
    }
    auto native = reinterpret_cast<FactoryProc>(GetProcAddress(gNative, "NvOFAPICreateInstanceD3D12"));
    if (!native) return kFailure;
    NV_OF_STATUS status = native(version, funcs);
    if (status != kSuccess) return status;
    wchar_t mode[20]{};
    GetEnvironmentVariableW(L"V39_MOTION", mode, DWORD(std::size(mode)));
    gShaderMode = (_wcsicmp(mode, L"SHADER") == 0);
    if (!gShaderMode) return status;
    gOriginal = *funcs;
    if (!gOriginal.create || !gOriginal.registerResource || !gOriginal.unregisterResource ||
        !gOriginal.execute || !gOriginal.destroy) return kFailure;
    funcs->create = &CreateShim;
    funcs->registerResource = &RegisterShim;
    funcs->unregisterResource = &UnregisterShim;
    funcs->execute = &ExecuteShim;
    funcs->destroy = &DestroyShim;
    Diagnostic(L"Experimental SHADER motion selected; NvOFExecute redirected.");
    return status;
}
