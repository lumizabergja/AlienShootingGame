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
    ComPtr<ID3D12Resource> current,previous,motion,depth,result;
    bool sr{},fg{},initialized{},history{};
    bool srEvaluated{};
    sl::FrameToken* token{};
    sl::ViewportHandle viewport{0};
    std::wstring error;
    void Load();
    void BindDevice(ID3D12Device* d) {Check(setDevice(d),"slSetD3DDevice");}
    void Init(ID3D12Device*,IDXGIAdapter1*,ID3D12Resource*,UINT,UINT,bool,bool,UINT,bool);
    void PrepareFrame();
    bool PrepareMotion(ID3D12Fence*,uint64_t);
    bool WaitMotion(ID3D12CommandQueue*);
    void Record(ID3D12GraphicsCommandList*,ID3D12Resource*);
    void TagBackbuffer(ID3D12GraphicsCommandList*,ID3D12Resource*,D3D12_RESOURCE_STATES);
    void BeforeFrame(ID3D12CommandQueue*);
    void AfterPresent();
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
    uint64_t reportCalls{}, reportPresents{};
    sl::Result reportResult{sl::Result::eOk};
    sl::DLSSGStatus reportStatus{};
    ComPtr<ID3D12Fence> inputFence;
    uint64_t inputFenceValue{};
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
