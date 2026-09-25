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

bool NvofBridge::Init(ID3D12Device* device, ID3D12Resource* const* liveSources, UINT sourceCount, UINT width, UINT height, Log log) {
    Shutdown(); log_=std::move(log); device_=device; width_=width; height_=height;
    if(!liveSources || sourceCount==0) return false;
    sources_.reserve(sourceCount); sourceHandles_.assign(sourceCount,nullptr);
    for(UINT i=0;i<sourceCount;++i) {
        if(!liveSources[i]) return false;
        sources_.emplace_back(liveSources[i]);
    }
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
    const uint64_t pixels=uint64_t(width)*height;
    if(supports(2)) grid_=2; else if(supports(1)) grid_=1; else if(supports(4)) grid_=4; else {Shutdown();return false;}

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
    previous_=MakeTexture(DXGI_FORMAT_B8G8R8A8_UNORM,width,height,L"NVOFA previous capture");
    flow_=MakeTexture(DXGI_FORMAT_R16G16_SINT,flowW_,flowH_,L"NVOFA forward flow");
    backward_.Reset();
    if(costFormat_!=DXGI_FORMAT_UNKNOWN) cost_=MakeTexture(costFormat_,flowW_,flowH_,L"NVOFA cost");
    if(!previous_||!flow_){Shutdown();return false;}
    if(FAILED(device_->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&ofaFence_)))){Shutdown();return false;}

    NV_OF_INIT_PARAMS init{}; init.width=width; init.height=height; init.outGridSize=static_cast<NV_OF_OUTPUT_VECTOR_GRID_SIZE>(grid_);
    init.hintGridSize=NV_OF_HINT_VECTOR_GRID_SIZE_UNDEFINED; init.mode=NV_OF_MODE_OPTICALFLOW;
    // Medium at <=1440p gives visibly cleaner edges while NVOFA stays off the graphics cores.
    // Above 1440p use FAST to protect the latency budget.
    init.perfLevel=NV_OF_PERF_LEVEL_FAST;
    init.enableExternalHints=NV_OF_FALSE; init.enableOutputCost=cost_?NV_OF_TRUE:NV_OF_FALSE;
    init.disparityRange=NV_OF_STEREO_DISPARITY_RANGE_UNDEFINED; init.enableRoi=NV_OF_FALSE;
    init.predDirection=NV_OF_PRED_DIRECTION_FORWARD;
    init.enableGlobalFlow=NV_OF_FALSE; init.inputBufferFormat=NV_OF_BUFFER_FORMAT_ABGR8;
    st=api_->fn.nvOFInit(h,&init);
    if(st!=NV_OF_SUCCESS){ if(log_)log_(std::wstring(L"NVOFA init failed: ")+NvofStatus(st)); Shutdown(); return false; }

    for(UINT i=0;i<sourceCount;++i) {
        if(!Register(sources_[i].Get(),&sourceHandles_[i])) {
            if(log_)log_(L"NVOFA source registration failed."); Shutdown(); return false;
        }
    }
    if(!Register(previous_.Get(),&previousHandle_)||!Register(flow_.Get(),&flowHandle_)||
       (cost_&&!Register(cost_.Get(),&costHandle_))){ if(log_)log_(L"NVOFA resource registration failed."); Shutdown(); return false; }

    ready_=true; hasPrevious_=false; flowValid_=false; disableTemporalOnce_=true;
    if(log_)log_(L"NVOFA motion-only path: "+std::to_wstring(grid_)+L"x"+std::to_wstring(grid_)+
        L" grid, FAST preset, forward-only, confidence cost="+(cost_?std::wstring(L"on"):std::wstring(L"off"))+L".");
    return true;
}

bool NvofBridge::SubmitFrame(ID3D12Fence* captureFence,uint64_t captureValue,UINT sourceIndex,ID3D12Fence* historyFence,uint64_t historyValue){
    if(!ready_||!captureFence||sourceIndex>=sourceHandles_.size()){flowValid_=false;return false;}
    if(!hasPrevious_){flowValid_=false;return false;}
    NV_OF_FENCE_POINT waits[2]={{captureFence,captureValue},{historyFence,historyValue}}; NV_OF_FENCE_POINT done{ofaFence_.Get(),++ofaValue_};
    NV_OF_EXECUTE_INPUT_PARAMS_D3D12 in{}; in.inputFrame=static_cast<NvOFGPUBufferHandle>(sourceHandles_[sourceIndex]); in.referenceFrame=static_cast<NvOFGPUBufferHandle>(previousHandle_);
    in.disableTemporalHints=disableTemporalOnce_?NV_OF_TRUE:NV_OF_FALSE; disableTemporalOnce_=false; in.numFencePoints=(historyFence && historyValue)?2u:1u; in.fencePoint=waits;
    NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 out{}; out.outputBuffer=static_cast<NvOFGPUBufferHandle>(flowHandle_); out.outputCostBuffer=static_cast<NvOFGPUBufferHandle>(costHandle_);
    out.fencePoint=&done;
    auto st=api_->fn.nvOFExecuteD3D12(static_cast<NvOFHandle>(session_),&in,&out); flowValid_=st==NV_OF_SUCCESS;
    if(!flowValid_&&log_)log_(std::wstring(L"NVOFA execute failed: ")+NvofStatus(st)+L"; compute fallback used for this frame.");
    return flowValid_;
}

bool NvofBridge::WaitOnFlow(ID3D12CommandQueue* queue) const { return !flowValid_ || (queue&&SUCCEEDED(queue->Wait(ofaFence_.Get(),ofaValue_))); }

void NvofBridge::BeginRead(ID3D12GraphicsCommandList* list){
    ID3D12Resource* r[]={flow_.Get(),cost_.Get(),backward_.Get()};
    for(auto* x:r)if(x){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={x,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};list->ResourceBarrier(1,&b);}
}
void NvofBridge::EndRead(ID3D12GraphicsCommandList* list){
    ID3D12Resource* r[]={flow_.Get(),cost_.Get(),backward_.Get()};
    for(auto* x:r)if(x){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={x,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON};list->ResourceBarrier(1,&b);}
}
void NvofBridge::RecordHistoryCopy(ID3D12GraphicsCommandList* list,ID3D12Resource* source,D3D12_RESOURCE_STATES sourceState){
    if(!ready_||!list||!source||!previous_)return;
    D3D12_RESOURCE_BARRIER b[2]{};for(auto& x:b){x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;x.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;}
    b[0].Transition={source,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,sourceState,D3D12_RESOURCE_STATE_COPY_SOURCE};
    b[1].Transition={previous_.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST};
    list->ResourceBarrier(2,b); list->CopyResource(previous_.Get(),source);
    b[0].Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;b[0].Transition.StateAfter=sourceState;
    b[1].Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST;b[1].Transition.StateAfter=D3D12_RESOURCE_STATE_COMMON;
    list->ResourceBarrier(2,b); hasPrevious_=true;
}

void NvofBridge::UnregisterAll(){
    if(!api_||!api_->fn.nvOFUnregisterResourceD3D12)return;
    for(auto& h:sourceHandles_) if(h){NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12 u{};u.hOFGpuBuffer=static_cast<NvOFGPUBufferHandle>(h);api_->fn.nvOFUnregisterResourceD3D12(&u);h=nullptr;}
    void** handles[]={&previousHandle_,&flowHandle_,&costHandle_,&backwardHandle_,&backwardCostHandle_};
    for(void** p:handles)if(*p){NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12 u{};u.hOFGpuBuffer=static_cast<NvOFGPUBufferHandle>(*p);api_->fn.nvOFUnregisterResourceD3D12(&u);*p=nullptr;}
}
void NvofBridge::Shutdown(){
    ready_=false;hasPrevious_=false;flowValid_=false;UnregisterAll();sources_.clear();sourceHandles_.clear();previous_.Reset();flow_.Reset();cost_.Reset();backward_.Reset();backwardCost_.Reset();
    if(api_&&session_&&api_->fn.nvOFDestroy)api_->fn.nvOFDestroy(static_cast<NvOFHandle>(session_));session_=nullptr;ofaFence_.Reset();device_.Reset();delete api_;api_=nullptr;
    // Driver module is intentionally left loaded until process exit.
    library_=nullptr;
}
