#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <sl.h>
#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <sl_pcl.h>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <chrono>
#include "nvof_bridge.h"
using Microsoft::WRL::ComPtr;

class DlssExperiment {
public:
    using Log = std::function<void(const std::wstring&)>;
    using FactoryFn = HRESULT(WINAPI*)(REFIID,void**);
    using DeviceFn = HRESULT(WINAPI*)(IUnknown*,D3D_FEATURE_LEVEL,REFIID,void**);
    FactoryFn factory{};
    DeviceFn createDevice{};
    Log log;
    UINT rw{},rh{},ow{},oh{};
    static constexpr UINT kInputSets = 3;
    struct InputSet {
        ComPtr<ID3D12Resource> current,motion,depth;
        ComPtr<ID3D12Fence> completionFence;
        uint64_t completionValue{};
    };
    InputSet inputSets[kInputSets];
    ComPtr<ID3D12Resource> previous;
    bool sr{},fg{},initialized{},history{};
    bool srEvaluated{};
    sl::FrameToken* token{};
    sl::ViewportHandle viewport{0};
    std::wstring error;
    void Load();
    void BindDevice(ID3D12Device* d) {Check(setDevice(d),"slSetD3DDevice");}
    void Init(ID3D12Device*,IDXGIAdapter1*,ID3D12Resource*,ID3D12Resource*,ID3D12Resource*,UINT,UINT,bool,bool,UINT,bool,UINT,UINT,bool);
    void PrepareFrame(bool sleepNow);
    void SleepPreparedFrame();
    bool ProducerPrepareMotion(UINT sourceIndex,UINT referenceIndex,uint64_t sequence,ID3D12Fence* captureFence,uint64_t captureValue,ID3D12Fence* historyFence,uint64_t historyValue);
    bool PrepareMotion(UINT sourceIndex,UINT referenceIndex,uint64_t sequence,ID3D12Fence* captureFence,uint64_t captureValue,ID3D12Fence* historyFence,uint64_t historyValue);
    bool SelectPreparedMotion(uint64_t sequence,int expectedReferenceIndex);
    void DiscardPreparedMotion(uint64_t sequence) { nvof.DiscardPrepared(sequence); }
    bool CanReuseMotionSource(UINT sourceIndex) const { return nvof.CanReuseSource(sourceIndex); }
    bool WaitMotion(ID3D12CommandQueue*);
    bool MotionWouldBlockNow() const { return nvof.Available() && nvof.FlowPending(); }
    void MarkMotionConsumed(ID3D12Fence* fence,uint64_t value) { nvof.MarkActiveConsumed(fence,value); }
    bool HasHardwareMotion() const { return nvof.Available() && nvof.FlowReady(); }
    void RecordPreprocess(ID3D12GraphicsCommandList*,ID3D12Resource*,UINT,bool sharedCopyRead);
    void TagInputs(ID3D12GraphicsCommandList*);
    void TagBackbuffer(ID3D12GraphicsCommandList*,ID3D12Resource*,D3D12_RESOURCE_STATES);
    void BeforeFrame(ID3D12CommandQueue*);
    void DrainInputs(ID3D12CommandQueue*);
    ID3D12Resource* InputColor(UINT i) const { return inputSets[i % kInputSets].current.Get(); }
    UINT ActiveInputSet() const { return activeInputSet; }
    void AfterPresent();
    uint64_t ConsumeInputRecycleWaits() { auto v=inputRecycleWaits; inputRecycleWaits=0; return v; }
    void ConsumeNvofTimingStats(uint64_t& count,double& avgMs,double& maxMs) { nvof.ConsumeTimingStats(count,avgMs,maxMs); }
    void ConsumeNvofOverlapStats(uint64_t& producerSubmits,uint64_t& preparedHits,uint64_t& fallbackSubmits,uint64_t& discarded,uint64_t& noOutput,double& avgLeadMs,double& maxLeadMs) {
        nvof.ConsumeOverlapStats(producerSubmits,preparedHits,fallbackSubmits,discarded,noOutput,avgLeadMs,maxLeadMs);
    }
    void ConsumeReflexSleepStats(uint64_t& count,double& avgMs,double& maxMs) {
        count=reflexSleepCount; avgMs=count?reflexSleepSumMs/double(count):0.0; maxMs=reflexSleepMaxMs;
        reflexSleepCount=0; reflexSleepSumMs=reflexSleepMaxMs=0.0;
    }
    void Suspend(bool);
    void Marker(sl::PCLMarker m);
    std::wstring Status();
    void ResetHistory() { history=false; nvof.ResetHistory(); }
    void Release();
    void Shutdown();
    void EndRuntime() {if(initialized&&shutdown)shutdown();initialized=false;}
private:
    HMODULE module{};
    uint32_t index{};
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> resamplePSO,motionPSO,nvofResolvePSO;
    ComPtr<ID3D12Device> device;
    NvofBridge nvof;
    UINT stride{};
    sl::DLSSGOptions activeOptions{};
    bool suspended{};
    bool framePrepared{};
    bool frameSlept{};
    UINT reflexTiming{};
    UINT activeInputSet{}, nextInputSet{};
    uint64_t inputRecycleWaits{};
    uint64_t reflexSleepCount{};
    double reflexSleepSumMs{}, reflexSleepMaxMs{};
    uint64_t reportCalls{}, reportPresents{};
    sl::Result reportResult{sl::Result::eOk};
    sl::DLSSGStatus reportStatus{};
    PFun_slInit* init{};
    PFun_slShutdown* shutdown{};
    PFun_slSetD3DDevice* setDevice{};
    PFun_slGetFeatureFunction* feature{};
    PFun_slIsFeatureSupported* supported{};
    PFun_slGetNewFrameToken* newToken{};
    PFun_slSetConstants* constants{};
    PFun_slSetTagForFrame* tags{};
    PFun_slEvaluateFeature* evaluate{};
    PFun_slFreeResources* freeResources{};
    PFun_slDLSSGSetOptions* fgOptions{};
    PFun_slDLSSGGetState* fgState{};
    PFun_slReflexSetOptions* reflexOptions{};
    PFun_slReflexSleep* reflexSleep{};
    PFun_slPCLSetMarker* marker{};
    void Check(sl::Result,const char*);
    template<class T> void Export(T& dst,const char* name) {
        dst=reinterpret_cast<T>(GetProcAddress(module,name));
        if(!dst) throw std::runtime_error(std::string("Missing SDK export: ")+name);
    }
    template<class T> void Feature(T& dst,sl::Feature f,const char* name) {
        void* ptr{}; Check(feature(f,name,ptr),name); dst=reinterpret_cast<T>(ptr);
        if(!dst) throw std::runtime_error(std::string("Missing feature function: ")+name);
    }
    void Texture(ComPtr<ID3D12Resource>&,UINT,UINT,DXGI_FORMAT,D3D12_RESOURCE_STATES);
    void SRV(ID3D12Resource*,DXGI_FORMAT,UINT);
    void UAV(ID3D12Resource*,DXGI_FORMAT,UINT);
};
inline void HR(HRESULT h,const char* where) {
    if(FAILED(h)) { char s[200]; snprintf(s,sizeof(s),"%s HRESULT 0x%08X",where,(unsigned)h); throw std::runtime_error(s); }
}
inline void Transition(ID3D12GraphicsCommandList* list,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b) {
    if(a==b) return;
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};list->ResourceBarrier(1,&barrier);
}
