#pragma once
#include "common.h"
#include "resolve.h"
#include "../vendor/nvof/nvOpticalFlowD3D12.h"
#include <functional>
#include <initializer_list>

// Optional, experimental external motion-vector source for DLSS-G.
// NVOFA reports signed S10.5 vectors on a 4x4 grid. A compute pass expands them
// to pixel-space R16G16_FLOAT motion, while the existing depth stays flat.
class NvOfMotion {
    HMODULE module{};
    NV_OF_D3D12_API_FUNCTION_LIST api{};
    NvOFHandle session{};
    UINT sourceWidth{}, sourceHeight{}, width{}, height{}, gridWidth{}, gridHeight{};
    UINT stagedPitch{}, resolutionPercent{};
    DXGI_FORMAT inputFormat{DXGI_FORMAT_UNKNOWN}, outputFormat{DXGI_FORMAT_UNKNOWN};
    std::array<ComPtr<ID3D12Resource>, 2> images;
    // Small linear intermediate buffers avoid requiring UAV flags on NVOFA-registered textures.
    std::array<ComPtr<ID3D12Resource>, 2> scaledBuffers;
    std::array<NvOFGPUBufferHandle, 2> imageHandles{};
    std::array<ComPtr<ID3D12Resource>, FrameCount> vectors;
    std::array<NvOFGPUBufferHandle, FrameCount> vectorHandles{};
    ComPtr<ID3D12Fence> copied, processed, registered;
    uint64_t copyValue{}, processValue{}, registerValue{};
    std::array<Commands, FrameCount> compute;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    ComPtr<ID3D12RootSignature> signature;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12RootSignature> scaleSignature;
    ComPtr<ID3D12PipelineState> scalePipeline;
    UINT descriptorStep{};
    int previous = -1;

    void require(NV_OF_STATUS, const char *);
    DXGI_FORMAT selectFormat(NV_OF_BUFFER_USAGE, std::initializer_list<DXGI_FORMAT>);
    void registerResource(ID3D12Resource *, NvOFGPUBufferHandle &);
    void initShader(ID3D12Device *, MotionResolve &);
    void initScaler(ID3D12Device *);
public:
    NvOfMotion() = default;
    ~NvOfMotion();
    NvOfMotion(const NvOfMotion &) = delete;
    NvOfMotion &operator=(const NvOfMotion &) = delete;
    void init(ID3D12Device *, UINT, UINT, UINT resolutionPercent, MotionResolve &);
    void upload(ID3D12GraphicsCommandList *, ID3D12Resource *cudaSource, UINT rowPitch,
                unsigned frameNumber);
    // Must run immediately after submission of the command list containing upload().
    // Returns true after queuing the NVOFA engine; false on the initial frame.
    bool execute(ID3D12CommandQueue *, unsigned frameNumber);
    // Execute when execute() returned true; tags the resulting full-res motion.
    // Device queue waits asynchronously on the NVOFA completion fence.
    void convertAndTag(ID3D12CommandQueue *, unsigned context, MotionResolve &,
                       const std::function<void(ID3D12GraphicsCommandList *)> &tag);
    void tagWithoutFlow(ID3D12CommandQueue *, unsigned context, MotionResolve &,
                        const std::function<void(ID3D12GraphicsCommandList *)> &tag);
    uint64_t numberProcessed() const { return processValue; }
    void resetHistory() { previous = -1; }
};
