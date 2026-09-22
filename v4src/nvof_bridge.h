#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <functional>
#include <string>

class NvofBridge {
public:
    using Log = std::function<void(const std::wstring&)>;
    ~NvofBridge();
    bool Init(ID3D12Device* device, ID3D12Resource* liveSource, UINT width, UINT height, Log log);
    void Shutdown();
    bool Available() const { return ready_; }
    bool FlowReady() const { return flowValid_; }
    UINT Grid() const { return grid_; }
    UINT SourceWidth() const { return width_; }
    UINT SourceHeight() const { return height_; }
    UINT FlowWidth() const { return flowW_; }
    UINT FlowHeight() const { return flowH_; }
    bool HasCost() const { return cost_ != nullptr; }
    bool HasBackward() const { return backward_ != nullptr; }
    ID3D12Resource* Flow() const { return flow_.Get(); }
    ID3D12Resource* Cost() const { return cost_.Get(); }
    ID3D12Resource* Backward() const { return backward_.Get(); }
    ID3D12Resource* Previous() const { return previous_.Get(); }
    DXGI_FORMAT CostFormat() const { return costFormat_; }

    bool SubmitFrame(ID3D12Fence* captureFence, uint64_t captureValue);
    bool WaitOnFlow(ID3D12CommandQueue* queue) const;
    void BeginRead(ID3D12GraphicsCommandList* list);
    void EndRead(ID3D12GraphicsCommandList* list);
    void RecordHistoryCopy(ID3D12GraphicsCommandList* list, ID3D12Resource* source,
                           D3D12_RESOURCE_STATES sourceState);
    void ResetHistory() { hasPrevious_ = false; flowValid_ = false; disableTemporalOnce_ = true; }

private:
    struct Api;
    Api* api_{};
    void* session_{};
    HMODULE library_{};
    Log log_;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12Fence> ofaFence_;
    uint64_t ofaValue_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> source_, previous_, flow_, cost_, backward_, backwardCost_;
    void* sourceHandle_{};
    void* previousHandle_{};
    void* flowHandle_{};
    void* costHandle_{};
    void* backwardHandle_{};
    void* backwardCostHandle_{};
    UINT width_{}, height_{}, flowW_{}, flowH_{}, grid_{};
    DXGI_FORMAT costFormat_{DXGI_FORMAT_UNKNOWN};
    bool ready_{}, hasPrevious_{}, flowValid_{}, disableTemporalOnce_{true};

    Microsoft::WRL::ComPtr<ID3D12Resource> MakeTexture(DXGI_FORMAT fmt, UINT w, UINT h, const wchar_t* name);
    bool Register(ID3D12Resource* resource, void** handle);
    bool WaitFenceCPU(uint64_t value);
    void UnregisterAll();
};
