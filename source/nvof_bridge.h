#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <functional>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <atomic>

class NvofBridge {
public:
    using Log = std::function<void(const std::wstring&)>;
    static constexpr UINT kFlowSlots = 3;
    ~NvofBridge();
    bool Init(ID3D12Device* device, ID3D12Resource* liveSource0, ID3D12Resource* liveSource1,
              ID3D12Resource* liveSource2, UINT width, UINT height, UINT requestedGrid, bool enableCost, Log log);
    void Shutdown();
    bool Available() const { return ready_; }
    bool FlowReady() const { return activeFlowSlot_.load(std::memory_order_acquire) >= 0; }
    bool FlowPending() const;
    UINT Grid() const { return grid_; }
    UINT SourceWidth() const { return width_; }
    UINT SourceHeight() const { return height_; }
    UINT FlowWidth() const { return flowW_; }
    UINT FlowHeight() const { return flowH_; }
    bool HasCost() const { return cost_[0] != nullptr; }
    DXGI_FORMAT CostFormat() const { return costFormat_; }
    ID3D12Resource* Flow(UINT slot) const { return flow_[slot % kFlowSlots].Get(); }
    ID3D12Resource* Cost(UINT slot) const { return cost_[slot % kFlowSlots].Get(); }
    UINT ActiveFlowSlot() const { int s=activeFlowSlot_.load(std::memory_order_acquire); return s < 0 ? 0u : UINT(s); }

    // V22 producer path: submit flow after capture Signal is enqueued but before D3D11 Flush.
    // The explicit reference index is the last actually-presented source at submit time.
    bool SubmitFrame(UINT sourceIndex, UINT referenceIndex, uint64_t sequence,
                     ID3D12Fence* captureFence, uint64_t captureValue,
                     ID3D12Fence* historyFence, uint64_t historyValue,
                     bool producerSubmit);
    bool SelectPrepared(uint64_t sequence, int expectedReferenceIndex);
    void ClearActive();
    void DiscardPrepared(uint64_t sequence);
    bool CanReuseSource(UINT sourceIndex) const;
    bool WaitOnFlow(ID3D12CommandQueue* queue) const;
    void MarkActiveConsumed(ID3D12Fence* fence, uint64_t value);
    void ConsumeTimingStats(uint64_t& count,double& avgMs,double& maxMs);
    void ConsumeOverlapStats(uint64_t& producerSubmits,uint64_t& preparedHits,uint64_t& fallbackSubmits,
                             uint64_t& discarded,uint64_t& noOutput,double& avgLeadMs,double& maxLeadMs);
    void BeginRead(ID3D12GraphicsCommandList* list);
    void EndRead(ID3D12GraphicsCommandList* list);
    void ResetHistory();

private:
    struct Api;
    Api* api_{};
    void* session_{};
    HMODULE library_{};
    Log log_;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12Fence> ofaFence_;
    uint64_t ofaValue_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> sources_[3], flow_[kFlowSlots], cost_[kFlowSlots];
    void* sourceHandles_[3]{};
    void* flowHandles_[kFlowSlots]{};
    void* costHandles_[kFlowSlots]{};
    UINT width_{}, height_{}, flowW_{}, flowH_{}, grid_{};
    DXGI_FORMAT costFormat_{DXGI_FORMAT_UNKNOWN};
    bool ready_{};
    uint64_t sourceUseValue_[3]{};

    enum : uint8_t { kFlowFree=0, kFlowPrepared=1, kFlowActive=2, kFlowAbandoned=3 };
    struct FlowSlot {
        uint8_t state{kFlowFree};
        uint64_t sequence{};
        UINT sourceIndex{};
        UINT referenceIndex{};
        uint64_t ofaDoneValue{};
        Microsoft::WRL::ComPtr<ID3D12Fence> readFence;
        uint64_t readFenceValue{};
        LARGE_INTEGER submitQpc{};
    };
    mutable std::mutex flowMutex_;
    FlowSlot flowSlots_[kFlowSlots];
    UINT nextFlowSlot_{};
    std::atomic<int> activeFlowSlot_{-1};
    std::atomic<uint64_t> activeFlowValue_{0};

    struct TimingItem { uint64_t value{}; LARGE_INTEGER submit{}; };
    std::thread timingThread_;
    std::mutex timingMutex_;
    std::condition_variable timingCv_;
    std::deque<TimingItem> timingQueue_;
    bool timingStop_{};
    HANDLE timingEvent_{};
    LARGE_INTEGER timingFrequency_{};
    uint64_t timingCount_{};
    double timingSumMs_{}, timingMaxMs_{};

    std::atomic<uint64_t> producerSubmits_{0},preparedHits_{0},fallbackSubmits_{0},discarded_{0},noOutput_{0};
    std::mutex leadMutex_;
    uint64_t leadCount_{};
    double leadSumMs_{},leadMaxMs_{};

    void TimingLoop();
    void StartTiming();
    void StopTiming();
    int FindReusableFlowSlotLocked();
    Microsoft::WRL::ComPtr<ID3D12Resource> MakeTexture(DXGI_FORMAT fmt, UINT w, UINT h, const wchar_t* name);
    bool Register(ID3D12Resource* resource, void** handle);
    bool WaitFenceCPU(uint64_t value);
    void UnregisterAll();
};
