#include "nvof_bridge.h"
#include "nvOpticalFlowCommon.h"
#include "nvOpticalFlowD3D12.h"
#include <algorithm>
#include <vector>

using Microsoft::WRL::ComPtr;
struct NvofBridge::Api { NV_OF_D3D12_API_FUNCTION_LIST fn{}; };
using PfnCreateInstanceD3D12 = NV_OF_STATUS (NVOFAPI*)(uint32_t, NV_OF_D3D12_API_FUNCTION_LIST*);

static const wchar_t* NvofStatus(NV_OF_STATUS s) {
    switch (s) {
    case NV_OF_SUCCESS: return L"success";
    case NV_OF_ERR_OF_NOT_AVAILABLE: return L"not available";
    case NV_OF_ERR_UNSUPPORTED_DEVICE: return L"unsupported device";
    case NV_OF_ERR_INVALID_PARAM: return L"invalid parameter";
    case NV_OF_ERR_INVALID_VERSION: return L"invalid version";
    case NV_OF_ERR_UNSUPPORTED_FEATURE: return L"unsupported feature";
    default: return L"driver error";
    }
}

NvofBridge::~NvofBridge() { Shutdown(); }

ComPtr<ID3D12Resource> NvofBridge::MakeTexture(DXGI_FORMAT fmt, UINT w, UINT h, const wchar_t* name) {
    D3D12_HEAP_PROPERTIES hp{}; hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width=w; d.Height=h;
    d.DepthOrArraySize=1; d.MipLevels=1; d.Format=fmt; d.SampleDesc.Count=1;
    d.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN; d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> r;
    if (FAILED(device_->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&r)))) return nullptr;
    r->SetName(name); return r;
}

bool NvofBridge::WaitFenceCPU(uint64_t value) {
    if (!ofaFence_ || ofaFence_->GetCompletedValue() >= value) return true;
    HANDLE e=CreateEventW(nullptr,FALSE,FALSE,nullptr); if(!e) return false;
    bool ok=SUCCEEDED(ofaFence_->SetEventOnCompletion(value,e)) && WaitForSingleObject(e,3000)==WAIT_OBJECT_0;
    CloseHandle(e); return ok;
}

bool NvofBridge::Register(ID3D12Resource* resource, void** handle) {
    NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 p{};
    p.resource=resource;
    p.inputFencePoint.fence=ofaFence_.Get(); p.inputFencePoint.value=ofaFence_->GetCompletedValue();
    p.hOFGpuBuffer=reinterpret_cast<NvOFGPUBufferHandle*>(handle);
    p.outputFencePoint.fence=ofaFence_.Get(); p.outputFencePoint.value=++ofaValue_;
    auto s=api_->fn.nvOFRegisterResourceD3D12(static_cast<NvOFHandle>(session_),&p);
    return s==NV_OF_SUCCESS && *handle && WaitFenceCPU(ofaValue_);
}

bool NvofBridge::Init(ID3D12Device* device, ID3D12Resource* liveSource0, ID3D12Resource* liveSource1,
                      ID3D12Resource* liveSource2, UINT width, UINT height, UINT requestedGrid, bool enableCost, Log log) {
    Shutdown(); log_=std::move(log); device_=device;
    sources_[0]=liveSource0; sources_[1]=liveSource1; sources_[2]=liveSource2; width_=width; height_=height;
    if(!sources_[0] || !sources_[1] || !sources_[2]) { if(log_)log_(L"NVOFA requires all three capture mailbox resources."); return false; }
    library_=LoadLibraryExW(L"nvofapi64.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!library_){ if(log_)log_(L"NVOFA unavailable: nvofapi64.dll not found; compute fallback active."); return false; }
    auto create=reinterpret_cast<PfnCreateInstanceD3D12>(GetProcAddress(library_,"NvOFAPICreateInstanceD3D12"));
    if(!create){ if(log_)log_(L"NVOFA unavailable: D3D12 entry point missing."); Shutdown(); return false; }
    api_=new Api(); auto st=create(NV_OF_API_VERSION,&api_->fn);
    if(st!=NV_OF_SUCCESS || !api_->fn.nvCreateOpticalFlowD3D12 || !api_->fn.nvOFInit || !api_->fn.nvOFExecuteD3D12 ||
       !api_->fn.nvOFRegisterResourceD3D12 || !api_->fn.nvOFGetCaps || !api_->fn.nvOFGetSurfaceFormatCountD3D12 || !api_->fn.nvOFGetSurfaceFormatD3D12){
        if(log_)log_(L"NVOFA unavailable: incomplete driver API."); Shutdown(); return false;
    }
    st=api_->fn.nvCreateOpticalFlowD3D12(device,reinterpret_cast<NvOFHandle*>(&session_));
    if(st!=NV_OF_SUCCESS||!session_){ if(log_)log_(std::wstring(L"NVOFA unavailable: ")+NvofStatus(st)); Shutdown(); return false; }
    auto h=static_cast<NvOFHandle>(session_);

    uint32_t count=0; if(api_->fn.nvOFGetCaps(h,NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES,nullptr,&count)!=NV_OF_SUCCESS||!count){Shutdown();return false;}
    std::vector<uint32_t> grids(count); api_->fn.nvOFGetCaps(h,NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES,grids.data(),&count);
    auto supports=[&](uint32_t g){return std::find(grids.begin(),grids.end(),g)!=grids.end();};
    const UINT requested = requestedGrid == 4 ? 4u : 2u;
    if(supports(requested)) grid_=requested;
    else if(supports(2)) grid_=2;
    else if(supports(4)) grid_=4;
    else if(supports(1)) grid_=1;
    else {Shutdown();return false;}
    if(grid_ != requested && log_) {
        log_(L"Requested NVOFA "+std::to_wstring(requested)+L"x"+std::to_wstring(requested)+
             L" grid is unsupported by this driver/GPU; using "+std::to_wstring(grid_)+L"x"+std::to_wstring(grid_)+L".");
    }

    uint32_t fmtCount=0; api_->fn.nvOFGetSurfaceFormatCountD3D12(h,NV_OF_BUFFER_USAGE_INPUT,NV_OF_MODE_OPTICALFLOW,&fmtCount);
    std::vector<DXGI_FORMAT> inputs(fmtCount); if(fmtCount) api_->fn.nvOFGetSurfaceFormatD3D12(h,NV_OF_BUFFER_USAGE_INPUT,NV_OF_MODE_OPTICALFLOW,inputs.data());
    if(std::find(inputs.begin(),inputs.end(),DXGI_FORMAT_B8G8R8A8_UNORM)==inputs.end()){ if(log_)log_(L"NVOFA BGRA8 input unsupported."); Shutdown(); return false; }

    fmtCount=0; api_->fn.nvOFGetSurfaceFormatCountD3D12(h,NV_OF_BUFFER_USAGE_OUTPUT,NV_OF_MODE_OPTICALFLOW,&fmtCount);
    std::vector<DXGI_FORMAT> outputs(fmtCount); if(fmtCount) api_->fn.nvOFGetSurfaceFormatD3D12(h,NV_OF_BUFFER_USAGE_OUTPUT,NV_OF_MODE_OPTICALFLOW,outputs.data());
    if(std::find(outputs.begin(),outputs.end(),DXGI_FORMAT_R16G16_SINT)==outputs.end()){Shutdown();return false;}

    fmtCount=0; if(api_->fn.nvOFGetSurfaceFormatCountD3D12(h,NV_OF_BUFFER_USAGE_COST,NV_OF_MODE_OPTICALFLOW,&fmtCount)==NV_OF_SUCCESS&&fmtCount){
        std::vector<DXGI_FORMAT> costs(fmtCount); api_->fn.nvOFGetSurfaceFormatD3D12(h,NV_OF_BUFFER_USAGE_COST,NV_OF_MODE_OPTICALFLOW,costs.data());
        if(std::find(costs.begin(),costs.end(),DXGI_FORMAT_R8_UINT)!=costs.end()) costFormat_=DXGI_FORMAT_R8_UINT;
    }

    flowW_=(width+grid_-1)/grid_; flowH_=(height+grid_-1)/grid_;
    for(UINT i=0;i<kFlowSlots;++i) {
        wchar_t flowName[64]{},costName[64]{};
        swprintf_s(flowName,L"NVOFA forward flow ring %u",i);
        flow_[i]=MakeTexture(DXGI_FORMAT_R16G16_SINT,flowW_,flowH_,flowName);
        if(enableCost && costFormat_!=DXGI_FORMAT_UNKNOWN) {
            swprintf_s(costName,L"NVOFA cost ring %u",i);
            cost_[i]=MakeTexture(costFormat_,flowW_,flowH_,costName);
        }
        if(!flow_[i]){Shutdown();return false;}
    }
    if(FAILED(device_->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&ofaFence_)))){Shutdown();return false;}

    NV_OF_INIT_PARAMS init{}; init.width=width; init.height=height; init.outGridSize=static_cast<NV_OF_OUTPUT_VECTOR_GRID_SIZE>(grid_);
    init.hintGridSize=NV_OF_HINT_VECTOR_GRID_SIZE_UNDEFINED; init.mode=NV_OF_MODE_OPTICALFLOW;
    init.perfLevel=NV_OF_PERF_LEVEL_FAST;
    init.enableExternalHints=NV_OF_FALSE; init.enableOutputCost=cost_[0]?NV_OF_TRUE:NV_OF_FALSE;
    init.disparityRange=NV_OF_STEREO_DISPARITY_RANGE_UNDEFINED; init.enableRoi=NV_OF_FALSE;
    init.predDirection=NV_OF_PRED_DIRECTION_FORWARD;
    init.enableGlobalFlow=NV_OF_FALSE; init.inputBufferFormat=NV_OF_BUFFER_FORMAT_ABGR8;
    st=api_->fn.nvOFInit(h,&init);
    if(st!=NV_OF_SUCCESS){ if(log_)log_(std::wstring(L"NVOFA init failed: ")+NvofStatus(st)); Shutdown(); return false; }

    if(!Register(sources_[0].Get(),&sourceHandles_[0])||!Register(sources_[1].Get(),&sourceHandles_[1])||
       !Register(sources_[2].Get(),&sourceHandles_[2])) { if(log_)log_(L"NVOFA source registration failed."); Shutdown(); return false; }
    for(UINT i=0;i<kFlowSlots;++i) {
        if(!Register(flow_[i].Get(),&flowHandles_[i]) || (cost_[i]&&!Register(cost_[i].Get(),&costHandles_[i]))) {
            if(log_)log_(L"NVOFA flow-ring registration failed."); Shutdown(); return false;
        }
    }

    ready_=true; nextFlowSlot_=0; activeFlowSlot_.store(-1); activeFlowValue_.store(0);
    std::fill(std::begin(sourceUseValue_),std::end(sourceUseValue_),0ull);
    for(auto& s:flowSlots_) s=FlowSlot{};
    producerSubmits_=0; preparedHits_=0; fallbackSubmits_=0; discarded_=0; noOutput_=0;
    { std::lock_guard lock(leadMutex_); leadCount_=0;leadSumMs_=leadMaxMs_=0.0; }
    StartTiming();
    if(log_)log_(L"NVOFA motion-only path: "+std::to_wstring(grid_)+L"x"+std::to_wstring(grid_)+
        L" grid, FAST preset, forward-only, triple-source + triple-flow rings, confidence cost="+
        (cost_[0]?std::wstring(L"on"):std::wstring(L"off"))+L".");
    return true;
}

int NvofBridge::FindReusableFlowSlotLocked() {
    for(UINT n=0;n<kFlowSlots;++n) {
        UINT i=(nextFlowSlot_+n)%kFlowSlots;
        if(flowSlots_[i].state==kFlowFree || flowSlots_[i].state==kFlowAbandoned) {
            nextFlowSlot_=(i+1)%kFlowSlots;
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool NvofBridge::SubmitFrame(UINT sourceIndex,UINT referenceIndex,uint64_t sequence,
                             ID3D12Fence* captureFence,uint64_t captureValue,
                             ID3D12Fence* historyFence,uint64_t historyValue,
                             bool producerSubmit){
    if(!ready_||!captureFence||sourceIndex>=3||referenceIndex>=3||sourceIndex==referenceIndex) return false;
    std::lock_guard flowLock(flowMutex_);
    const int slotIndex=FindReusableFlowSlotLocked();
    if(slotIndex<0){++noOutput_;return false;}
    FlowSlot& slot=flowSlots_[slotIndex];

    NV_OF_FENCE_POINT waits[4]{}; uint32_t waitCount=0;
    auto addWait=[&](ID3D12Fence* fence,uint64_t value){
        if(!fence||!value)return;
        for(uint32_t i=0;i<waitCount;++i) if(waits[i].fence==fence){waits[i].value=std::max<uint64_t>(waits[i].value,value);return;}
        if(waitCount<4) waits[waitCount++]={fence,value};
    };
    addWait(captureFence,captureValue);
    addWait(historyFence,historyValue);
    const uint64_t sourceDependency=std::max(sourceUseValue_[sourceIndex],sourceUseValue_[referenceIndex]);
    const uint64_t ofaDependency=std::max(sourceDependency,slot.ofaDoneValue);
    addWait(ofaFence_.Get(),ofaDependency);
    addWait(slot.readFence.Get(),slot.readFenceValue);

    NV_OF_FENCE_POINT done{ofaFence_.Get(),++ofaValue_};
    NV_OF_EXECUTE_INPUT_PARAMS_D3D12 in{};
    in.inputFrame=static_cast<NvOFGPUBufferHandle>(sourceHandles_[sourceIndex]);
    in.referenceFrame=static_cast<NvOFGPUBufferHandle>(sourceHandles_[referenceIndex]);
    // Producer submissions may legitimately skip captures or reuse the same presented
    // reference. Do not feed temporal hints from an unrelated previous OFA pair.
    in.disableTemporalHints=NV_OF_TRUE;
    in.numFencePoints=waitCount; in.fencePoint=waits;
    NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 out{};
    out.outputBuffer=static_cast<NvOFGPUBufferHandle>(flowHandles_[slotIndex]);
    out.outputCostBuffer=static_cast<NvOFGPUBufferHandle>(costHandles_[slotIndex]);
    out.fencePoint=&done;
    LARGE_INTEGER submitQpc{}; QueryPerformanceCounter(&submitQpc);
    auto st=api_->fn.nvOFExecuteD3D12(static_cast<NvOFHandle>(session_),&in,&out);
    if(st!=NV_OF_SUCCESS) {
        slot.state=kFlowAbandoned;
        if(log_)log_(std::wstring(L"NVOFA execute failed: ")+NvofStatus(st)+L"; compute fallback used for this frame.");
        return false;
    }

    slot.state=kFlowPrepared; slot.sequence=sequence; slot.sourceIndex=sourceIndex; slot.referenceIndex=referenceIndex;
    slot.ofaDoneValue=done.value; slot.submitQpc=submitQpc;
    sourceUseValue_[sourceIndex]=std::max(sourceUseValue_[sourceIndex],done.value);
    sourceUseValue_[referenceIndex]=std::max(sourceUseValue_[referenceIndex],done.value);
    if(producerSubmit) ++producerSubmits_; else ++fallbackSubmits_;
    { std::lock_guard lock(timingMutex_); timingQueue_.push_back({done.value,submitQpc}); }
    timingCv_.notify_one();
    return true;
}

bool NvofBridge::SelectPrepared(uint64_t sequence,int expectedReferenceIndex) {
    activeFlowSlot_.store(-1,std::memory_order_release); activeFlowValue_.store(0,std::memory_order_release);
    if(!ready_||expectedReferenceIndex<0||expectedReferenceIndex>=3) return false;
    LARGE_INTEGER now{};QueryPerformanceCounter(&now);
    std::lock_guard lock(flowMutex_);
    for(UINT i=0;i<kFlowSlots;++i) {
        auto& s=flowSlots_[i];
        if(s.state==kFlowPrepared && s.sequence==sequence && int(s.referenceIndex)==expectedReferenceIndex) {
            s.state=kFlowActive;
            activeFlowValue_.store(s.ofaDoneValue,std::memory_order_release);
            activeFlowSlot_.store(static_cast<int>(i),std::memory_order_release);
            ++preparedHits_;
            if(timingFrequency_.QuadPart>0 && now.QuadPart>=s.submitQpc.QuadPart) {
                double ms=1000.0*double(now.QuadPart-s.submitQpc.QuadPart)/double(timingFrequency_.QuadPart);
                std::lock_guard lead(leadMutex_);++leadCount_;leadSumMs_+=ms;leadMaxMs_=std::max(leadMaxMs_,ms);
            }
            return true;
        }
    }
    return false;
}

void NvofBridge::ClearActive(){ activeFlowSlot_.store(-1,std::memory_order_release);activeFlowValue_.store(0,std::memory_order_release); }

void NvofBridge::DiscardPrepared(uint64_t sequence) {
    if(!ready_||!sequence)return;
    std::lock_guard lock(flowMutex_);
    for(auto& s:flowSlots_) if(s.state==kFlowPrepared && s.sequence==sequence){s.state=kFlowAbandoned;++discarded_;}
}

bool NvofBridge::CanReuseSource(UINT sourceIndex) const {
    if(!ready_||sourceIndex>=3||!ofaFence_) return true;
    std::lock_guard lock(flowMutex_);
    const uint64_t v=sourceUseValue_[sourceIndex];
    return !v || ofaFence_->GetCompletedValue()>=v;
}

bool NvofBridge::FlowPending() const {
    const uint64_t v=activeFlowValue_.load(std::memory_order_acquire);
    return v && ofaFence_ && ofaFence_->GetCompletedValue()<v;
}

bool NvofBridge::WaitOnFlow(ID3D12CommandQueue* queue) const {
    const uint64_t v=activeFlowValue_.load(std::memory_order_acquire);
    return !v || (queue&&SUCCEEDED(queue->Wait(ofaFence_.Get(),v)));
}

void NvofBridge::MarkActiveConsumed(ID3D12Fence* fence,uint64_t value) {
    const int active=activeFlowSlot_.load(std::memory_order_acquire);
    if(active<0||active>=int(kFlowSlots)) return;
    std::lock_guard lock(flowMutex_);
    auto& s=flowSlots_[active];
    if(s.state==kFlowActive){s.state=kFlowAbandoned;s.readFence=fence;s.readFenceValue=value;}
    activeFlowSlot_.store(-1,std::memory_order_release);activeFlowValue_.store(0,std::memory_order_release);
}

void NvofBridge::StartTiming(){
    StopTiming();
    QueryPerformanceFrequency(&timingFrequency_);
    timingEvent_=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    timingStop_=false;
    timingCount_=0; timingSumMs_=timingMaxMs_=0.0;
    if(timingEvent_) timingThread_=std::thread([this]{TimingLoop();});
}
void NvofBridge::StopTiming(){
    if(ofaFence_ && ofaValue_) WaitFenceCPU(ofaValue_);
    {
        std::lock_guard lock(timingMutex_);
        timingStop_=true;
        timingQueue_.clear();
    }
    timingCv_.notify_all();
    if(timingEvent_) SetEvent(timingEvent_);
    if(timingThread_.joinable()) timingThread_.join();
    if(timingEvent_){CloseHandle(timingEvent_);timingEvent_=nullptr;}
}
void NvofBridge::TimingLoop(){
    for(;;){
        TimingItem item{};
        {
            std::unique_lock lock(timingMutex_);
            timingCv_.wait(lock,[&]{return timingStop_||!timingQueue_.empty();});
            if(timingStop_) return;
            item=timingQueue_.front(); timingQueue_.pop_front();
        }
        if(!ofaFence_) continue;
        if(ofaFence_->GetCompletedValue()<item.value){
            ResetEvent(timingEvent_);
            if(FAILED(ofaFence_->SetEventOnCompletion(item.value,timingEvent_))) continue;
            for(;;){
                DWORD w=WaitForSingleObject(timingEvent_,25);
                { std::lock_guard lock(timingMutex_); if(timingStop_) return; }
                if(w==WAIT_OBJECT_0) break;
                if(w==WAIT_FAILED) break;
            }
        }
        LARGE_INTEGER end{}; QueryPerformanceCounter(&end);
        if(timingFrequency_.QuadPart>0 && end.QuadPart>=item.submit.QuadPart){
            const double ms=1000.0*double(end.QuadPart-item.submit.QuadPart)/double(timingFrequency_.QuadPart);
            std::lock_guard lock(timingMutex_);
            ++timingCount_; timingSumMs_+=ms; timingMaxMs_=std::max(timingMaxMs_,ms);
        }
    }
}
void NvofBridge::ConsumeTimingStats(uint64_t& count,double& avgMs,double& maxMs){
    std::lock_guard lock(timingMutex_);
    count=timingCount_; avgMs=count?timingSumMs_/double(count):0.0; maxMs=timingMaxMs_;
    timingCount_=0; timingSumMs_=timingMaxMs_=0.0;
}

void NvofBridge::ConsumeOverlapStats(uint64_t& producerSubmits,uint64_t& preparedHits,uint64_t& fallbackSubmits,
                                     uint64_t& discarded,uint64_t& noOutput,double& avgLeadMs,double& maxLeadMs) {
    producerSubmits=producerSubmits_.exchange(0); preparedHits=preparedHits_.exchange(0); fallbackSubmits=fallbackSubmits_.exchange(0);
    discarded=discarded_.exchange(0); noOutput=noOutput_.exchange(0);
    std::lock_guard lead(leadMutex_); avgLeadMs=leadCount_?leadSumMs_/double(leadCount_):0.0;maxLeadMs=leadMaxMs_;
    leadCount_=0;leadSumMs_=leadMaxMs_=0.0;
}

void NvofBridge::BeginRead(ID3D12GraphicsCommandList* list){
    const int active=activeFlowSlot_.load(std::memory_order_acquire); if(active<0||active>=int(kFlowSlots))return;
    ID3D12Resource* r[]={flow_[active].Get(),cost_[active].Get()};
    for(auto* x:r)if(x){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={x,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};list->ResourceBarrier(1,&b);}
}
void NvofBridge::EndRead(ID3D12GraphicsCommandList* list){
    const int active=activeFlowSlot_.load(std::memory_order_acquire); if(active<0||active>=int(kFlowSlots))return;
    ID3D12Resource* r[]={flow_[active].Get(),cost_[active].Get()};
    for(auto* x:r)if(x){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={x,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON};list->ResourceBarrier(1,&b);}
}

void NvofBridge::ResetHistory(){
    std::lock_guard lock(flowMutex_);
    for(auto& s:flowSlots_) if(s.state==kFlowPrepared||s.state==kFlowActive){s.state=kFlowAbandoned;++discarded_;}
    activeFlowSlot_.store(-1,std::memory_order_release);activeFlowValue_.store(0,std::memory_order_release);
}

void NvofBridge::UnregisterAll(){
    if(!api_||!api_->fn.nvOFUnregisterResourceD3D12)return;
    for(auto& h:sourceHandles_) if(h){NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12 u{};u.hOFGpuBuffer=static_cast<NvOFGPUBufferHandle>(h);api_->fn.nvOFUnregisterResourceD3D12(&u);h=nullptr;}
    for(UINT i=0;i<kFlowSlots;++i) {
        if(flowHandles_[i]){NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12 u{};u.hOFGpuBuffer=static_cast<NvOFGPUBufferHandle>(flowHandles_[i]);api_->fn.nvOFUnregisterResourceD3D12(&u);flowHandles_[i]=nullptr;}
        if(costHandles_[i]){NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12 u{};u.hOFGpuBuffer=static_cast<NvOFGPUBufferHandle>(costHandles_[i]);api_->fn.nvOFUnregisterResourceD3D12(&u);costHandles_[i]=nullptr;}
    }
}
void NvofBridge::Shutdown(){
    StopTiming();
    ready_=false;ClearActive();UnregisterAll();
    for(auto& s:sources_)s.Reset();for(auto& f:flow_)f.Reset();for(auto& c:cost_)c.Reset();
    if(api_&&session_&&api_->fn.nvOFDestroy)api_->fn.nvOFDestroy(static_cast<NvOFHandle>(session_));session_=nullptr;ofaFence_.Reset();device_.Reset();delete api_;api_=nullptr;
    library_=nullptr;
}
