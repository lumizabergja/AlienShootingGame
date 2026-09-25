#include "dlss_bridge.h"
#include "compute_bytecode.h"
#include <wintrust.h>
#include <softpub.h>

static const char kNvofResolveShader[] = R"HLSL(
Texture2D<float4> current : register(t1);
Texture2D<float4> previous : register(t2);
Texture2D<int2> nvofFlow : register(t3);
Texture2D<uint> nvofCost : register(t4);
RWTexture2D<float2> motion : register(u1);
RWTexture2D<float> depth : register(u2);

cbuffer Settings : register(b0) {
    uint width;
    uint height;
    uint resetHistory;
    uint flowGrid;
    uint flowWidth;
    uint flowHeight;
    uint hasCost;
    uint sourceWidth;
    uint sourceHeight;
};

int2 ClampPixel(int2 p) {
    return clamp(p, int2(0,0), int2((int)width-1,(int)height-1));
}
int2 ClampCell(int2 p) {
    return clamp(p, int2(0,0), int2((int)flowWidth-1,(int)flowHeight-1));
}
float2 LoadFlow(int2 c) {
    c=ClampCell(c);
    return float2(nvofFlow.Load(int3(c,0))) * (1.0/32.0);
}
float Confidence(int2 c) {
    if(!hasCost) return 1.0;
    c=ClampCell(c);
    float v=(float)nvofCost.Load(int3(c,0))*(1.0/255.0);
    return 0.10 + 0.90*saturate(1.0-v);
}
float3 ColorAtCell(int2 c) {
    float2 sp=float2(c*(int)flowGrid)+float2((float)flowGrid*0.5,(float)flowGrid*0.5);
    float2 op=sp*float2(width,height)/max(float2(1,1),float2(sourceWidth,sourceHeight));
    return current.Load(int3(ClampPixel(int2(op)),0)).rgb;
}
float2 ResolveFlow(float2 pixel) {
    float2 sp=(pixel+0.5)*float2(sourceWidth,sourceHeight)/float2(width,height)-0.5;
    float2 g=(sp+0.5)/max(1.0,(float)flowGrid)-0.5;
    int2 b=int2(floor(g));
    float2 f=frac(g);
    float3 center=current.Load(int3(ClampPixel(int2(pixel)),0)).rgb;
    float2 sum=0.0;
    float wsum=0.0;
    [unroll] for(int y=0;y<2;++y) {
        [unroll] for(int x=0;x<2;++x) {
            int2 c=b+int2(x,y);
            float spatial=(x?f.x:1.0-f.x)*(y?f.y:1.0-f.y);
            float3 cc=ColorAtCell(c);
            float delta=abs(center.r-cc.r)+abs(center.g-cc.g)+abs(center.b-cc.b);
            float edge=rcp(1.0+12.0*delta);
            float w=spatial*edge*Confidence(c);
            sum+=LoadFlow(c)*w;
            wsum+=w;
        }
    }
    if(wsum<1e-5) return LoadFlow(int2(round(g)));
    return sum/wsum;
}

[numthreads(8,8,1)]
void ResolveNvof(uint3 id:SV_DispatchThreadID) {
    if(id.x>=width||id.y>=height) return;
    float2 v=0.0;
    if(!resetHistory) {
        v=ResolveFlow(float2(id.xy));
        v*=float2(width,height)/max(float2(1,1),float2(sourceWidth,sourceHeight));
        float3 a=current.Load(int3(id.xy,0)).rgb;
        float3 b=previous.Load(int3(id.xy,0)).rgb;
        float same=abs(a.r-b.r)+abs(a.g-b.g)+abs(a.b-b.b);
        if(same<0.006) v=0.0;
        if(any(v!=v)||any(abs(v)>512.0)) v=0.0;
    }
    motion[id.xy]=v;
    depth[id.xy]=0.5;
}
)HLSL";


static void VerifyRuntime(const std::wstring& path) {
    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = path.c_str();
    WINTRUST_DATA trust{};
    trust.cbStruct = sizeof(trust);
    trust.dwUIChoice = WTD_UI_NONE;
    trust.fdwRevocationChecks = WTD_REVOKE_NONE;
    trust.dwUnionChoice = WTD_CHOICE_FILE;
    trust.pFile = &file;
    trust.dwStateAction = WTD_STATEACTION_VERIFY;
    trust.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    LONG status = WinVerifyTrust(nullptr, &action, &trust);
    trust.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &trust);
    if (status != ERROR_SUCCESS)
        throw std::runtime_error("NVIDIA runtime signature verification failed. Restore the original DLLs and check Windows certificate trust.");
}
void DlssExperiment::Check(sl::Result r,const char* where) {
    if(r!=sl::Result::eOk) throw std::runtime_error(std::string(where)+" Streamline error "+std::to_string((int)r));
}
void DlssExperiment::Load() {
    wchar_t path[32768]{}; GetModuleFileNameW(nullptr,path,32768);
    auto dir=std::filesystem::path(path).parent_path().wstring();
    for (const wchar_t* file : {L"sl.interposer.dll", L"sl.common.dll", L"sl.pcl.dll",
                               L"sl.reflex.dll", L"sl.dlss_g.dll", L"nvngx_dlssg.dll"})
        (void)file;
    module=LoadLibraryExW((dir+L"\\sl.interposer.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if(!module) throw std::runtime_error("Cannot load sl.interposer.dll; extract the entire ZIP first.");
    Export(init,"slInit"); Export(shutdown,"slShutdown"); Export(setDevice,"slSetD3DDevice");
    Export(feature,"slGetFeatureFunction");Export(supported,"slIsFeatureSupported");
    Export(newToken,"slGetNewFrameToken");Export(constants,"slSetConstants");Export(tags,"slSetTagForFrame");
    Export(evaluate,"slEvaluateFeature");Export(freeResources,"slFreeResources");
    Export(factory,"CreateDXGIFactory1"); Export(createDevice,"D3D12CreateDevice");
    const wchar_t* dirs[]={dir.c_str()};
    sl::Feature features[]={sl::kFeatureDLSS_G,sl::kFeatureReflex,sl::kFeaturePCL};
    sl::Preferences p{};p.pathsToPlugins=dirs;p.numPathsToPlugins=1;p.pathToLogsAndData=dir.c_str();
    p.logLevel=sl::LogLevel::eDefault;p.featuresToLoad=features;p.numFeaturesToLoad=3;
    p.flags=sl::PreferenceFlags::eDisableCLStateTracking|sl::PreferenceFlags::eUseFrameBasedResourceTagging|sl::PreferenceFlags::eUseDXGIFactoryProxy;
    p.engine=sl::EngineType::eCustom;p.engineVersion="DXGI-DX12 Simple Fixed FG";
    p.projectId="e86d5d1d-b886-47ac-a90d-48708688358b";p.renderAPI=sl::RenderAPI::eD3D12;
    Check(init(p,sl::kSDKVersion),"slInit");initialized=true;
    log(L"Streamline 2.14.1 initialized for DLSS Frame Generation only (no DLSS scaler).");
}
void DlssExperiment::Texture(ComPtr<ID3D12Resource>& r,UINT w,UINT h,DXGI_FORMAT f,D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;
    d.DepthOrArraySize=1;d.MipLevels=1;d.Format=f;d.SampleDesc.Count=1;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    HR(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&r)),"Create DLSS texture");
}
void DlssExperiment::SRV(ID3D12DescriptorHeap* target,ID3D12Resource* r,DXGI_FORMAT fmt,UINT slot) {
    D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.Format=fmt;d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;d.Texture2D.MipLevels=1;
    auto h=target->GetCPUDescriptorHandleForHeapStart();h.ptr+=slot*stride;device->CreateShaderResourceView(r,&d,h);
}
void DlssExperiment::UAV(ID3D12DescriptorHeap* target,ID3D12Resource* r,DXGI_FORMAT fmt,UINT slot) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC d{};d.Format=fmt;d.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
    auto h=target->GetCPUDescriptorHandleForHeapStart();h.ptr+=slot*stride;device->CreateUnorderedAccessView(r,nullptr,&d,h);
}
void DlssExperiment::Init(ID3D12Device* dev,IDXGIAdapter1* adapter,ID3D12Resource* const* sources,UINT sourceCount,UINT w,UINT h,bool, bool useFG,UINT multiplier,bool boost) {
    if (multiplier < 2 || multiplier > 4) throw std::runtime_error("Select a fixed FG multiplier of 2x, 3x or 4x.");
    device=dev;sr=false;fg=useFG;ow=w;oh=h;rw=w;rh=h;history=false;index=0;srEvaluated=false;
    DXGI_ADAPTER_DESC1 ad{};HR(adapter->GetDesc1(&ad),"Adapter desc");
    sl::AdapterInfo ai{};ai.deviceLUID=(uint8_t*)&ad.AdapterLuid;ai.deviceLUIDSizeInBytes=sizeof(LUID);
    if(fg) {
        Check(supported(sl::kFeatureDLSS_G,ai),"DLSS Frame Generation support");
        Feature(fgOptions,sl::kFeatureDLSS_G,"slDLSSGSetOptions");Feature(fgState,sl::kFeatureDLSS_G,"slDLSSGGetState");
        Feature(reflexOptions,sl::kFeatureReflex,"slReflexSetOptions");Feature(reflexSleep,sl::kFeatureReflex,"slReflexSleep");
        Feature(marker,sl::kFeaturePCL,"slPCLSetMarker");
        sl::ReflexOptions ro{};ro.mode=boost ? sl::ReflexMode::eLowLatencyWithBoost : sl::ReflexMode::eLowLatency;
        Check(reflexOptions(ro),"Enable Reflex");
        log(boost ? L"Presenter Reflex: On + Boost" : L"Presenter Reflex: On");
        sl::DLSSGOptions go{};go.mode=sl::DLSSGMode::eOff;go.numBackBuffers=2;
        go.colorWidth=w;go.colorHeight=h;go.colorBufferFormat=DXGI_FORMAT_B8G8R8A8_UNORM;
        go.mvecDepthWidth=rw;go.mvecDepthHeight=rh;go.mvecBufferFormat=DXGI_FORMAT_R16G16_FLOAT;go.depthBufferFormat=DXGI_FORMAT_R32_FLOAT;
        go.hudLessBufferFormat=DXGI_FORMAT_R16G16B16A16_FLOAT;
        sl::DLSSGState state{};Check(fgState(viewport,state,&go),"DLSS-G capabilities");
        if (multiplier - 1 > state.numFramesToGenerateMax)
            throw std::runtime_error("Selected " + std::to_string(multiplier) + "x FG exceeds runtime support (maximum " +
                std::to_string(state.numFramesToGenerateMax + 1) + "x). Choose a lower multiplier.");
        go.mode = sl::DLSSGMode::eOn;
        go.numFramesToGenerate = multiplier - 1;
        go.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
        activeOptions = go;
        suspended = false;
        framePrepared = false;
        reportCalls = reportPresents = 0;
        reportResult = sl::Result::eOk;
        reportStatus = sl::DLSSGStatus::eOk;
        Check(fgOptions(viewport,go),"Enable fixed NVIDIA Frame Generation");
        log(L"Requested fixed " + std::to_wstring(multiplier) + L"x FG: " +
            std::to_wstring(go.numFramesToGenerate) + L" generated frames per captured frame. Dynamic mode OFF. Runtime max generated frames=" +
            std::to_wstring(state.numFramesToGenerateMax));
    }
    auto read=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    auto colorRead=static_cast<D3D12_RESOURCE_STATES>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Texture(current,rw,rh,DXGI_FORMAT_R16G16B16A16_FLOAT,colorRead);
    Texture(previous,rw,rh,DXGI_FORMAT_R16G16B16A16_FLOAT,read);
    Texture(motion,rw,rh,DXGI_FORMAT_R16G16_FLOAT,read);Texture(depth,rw,rh,DXGI_FORMAT_R32_FLOAT,read);
    // V5: result was only a native-resolution duplicate of current for presenter
    // sampling/HUD-less tagging.  Bind/tag current directly and avoid that full copy.
    result.Reset();
    if(!sources || sourceCount==0 || !sources[0]) throw std::runtime_error("No capture source textures.");
    auto sourceDesc=sources[0]->GetDesc();
    nvof.Init(dev,sources,sourceCount,(UINT)sourceDesc.Width,sourceDesc.Height,log);
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=8;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    stride=dev->GetDescriptorHandleIncrementSize(hd.Type);
    heaps.resize(sourceCount);
    for(UINT i=0;i<sourceCount;++i) {
        HR(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heaps[i])),"Compute descriptors");
        auto* hp=heaps[i].Get();
        SRV(hp,sources[i],DXGI_FORMAT_B8G8R8A8_UNORM,0);SRV(hp,current.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,1);SRV(hp,previous.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,2);
        SRV(hp,nvof.Available()?nvof.Flow():nullptr,DXGI_FORMAT_R16G16_SINT,3);
        SRV(hp,nvof.Available()&&nvof.HasCost()?nvof.Cost():nullptr,DXGI_FORMAT_R8_UINT,4);
        UAV(hp,current.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,5);UAV(hp,motion.Get(),DXGI_FORMAT_R16G16_FLOAT,6);UAV(hp,depth.Get(),DXGI_FORMAT_R32_FLOAT,7);
    }
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,5,0,0,0};ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_UAV,3,0,0,5};
    D3D12_ROOT_PARAMETER parameters[2]{};parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable={2,ranges};parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[1].Constants={0,0,9};
    D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;sampler.MaxLOD=D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=2;rd.pParameters=parameters;rd.NumStaticSamplers=1;rd.pStaticSamplers=&sampler;
    ComPtr<ID3DBlob> blob,errors;HR(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors),"Compute root serialize");
    HR(dev->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"Compute root");
    auto pipeline=[&](const unsigned char* bytes,SIZE_T size,ComPtr<ID3D12PipelineState>& pso) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={bytes,size};
        HR(dev->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pso)),"Compute pipeline (Shader Model 6.0)");
    };
    pipeline(resampleCode,sizeof(resampleCode),resamplePSO);
    pipeline(motionCode,sizeof(motionCode),motionPSO);
    {
        ComPtr<ID3DBlob> code,err;
        HRESULT hr=D3DCompile(kNvofResolveShader,sizeof(kNvofResolveShader)-1,nullptr,nullptr,nullptr,
                              "ResolveNvof","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&err);
        if(FAILED(hr)) {
            std::string msg="NVOFA resolve shader compile failed";
            if(err) msg += ": "+std::string((const char*)err->GetBufferPointer(),err->GetBufferSize());
            throw std::runtime_error(msg);
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={code->GetBufferPointer(),code->GetBufferSize()};
        HR(dev->CreateComputePipelineState(&pd,IID_PPV_ARGS(&nvofResolvePSO)),"NVOFA resolve pipeline");
    }
    log(L"Native-resolution FG input/output "+std::to_wstring(ow)+L"x"+std::to_wstring(oh)+L"; DLSS scaling is not loaded.");
    log(nvof.Available()?L"Motion source: NVIDIA Optical Flow Accelerator; presenter/capture/pacing unchanged.":L"Motion source: original software estimator fallback; presenter/capture/pacing unchanged.");
}
void DlssExperiment::Marker(sl::PCLMarker m) { if(fg&&token&&marker) Check(marker(m,*token),"Reflex frame marker"); }
void DlssExperiment::PrepareFrame() {
    // Capture timeouts and cursor-only updates retain the same pending token.
    // Sleep only once per actual submitted frame, before capture in fresh mode.
    if (framePrepared) return;
    uint32_t n=index++;Check(newToken(token,&n),"New frame token");
    if(fg) Check(reflexSleep(*token),"Reflex sleep");
    framePrepared = true;
}
bool DlssExperiment::PrepareMotion(ID3D12Fence* captureFence,uint64_t captureValue,UINT sourceIndex) {
    activeSourceIndex=sourceIndex;
    return nvof.Available() ? nvof.SubmitFrame(captureFence,captureValue,sourceIndex) : false;
}
bool DlssExperiment::WaitMotion(ID3D12CommandQueue* queue) {
    return nvof.WaitOnFlow(queue);
}
void DlssExperiment::RecordPreMotion(ID3D12GraphicsCommandList* list,ID3D12Resource* source,UINT sourceIndex) {
    activeSourceIndex=sourceIndex;
    framePrepared = false;
    Marker(sl::PCLMarker::eSimulationStart);Marker(sl::PCLMarker::eSimulationEnd);
    sl::Constants c{};
    sl::float4x4 identity{};identity[0]={1,0,0,0};identity[1]={0,1,0,0};identity[2]={0,0,1,0};identity[3]={0,0,0,1};
    c.cameraViewToClip=identity;c.clipToCameraView=identity;c.clipToLensClip=identity;c.clipToPrevClip=identity;c.prevClipToClip=identity;
    c.jitterOffset={0,0};c.mvecScale={1.0f/rw,1.0f/rh};c.cameraPinholeOffset={0,0};
    c.cameraPos={0,0,0};c.cameraUp={0,1,0};c.cameraRight={1,0,0};c.cameraFwd={0,0,1};
    c.cameraNear=0.1f;c.cameraFar=1000;c.cameraFOV=1.04719755f;c.cameraAspectRatio=float(ow)/oh;
    c.motionVectorsInvalidValue=65504;c.depthInverted=sl::eFalse;c.cameraMotionIncluded=sl::eTrue;
    c.motionVectors3D=sl::eFalse;c.reset=history?sl::eFalse:sl::eTrue;c.orthographicProjection=sl::eTrue;
    Check(constants(c,*token,viewport),"Set synthetic flat-plane constants");

    const auto read=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto colorRead=static_cast<D3D12_RESOURCE_STATES>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    // source is read-only in this list.  NVOFA also only reads it, allowing the
    // resample work to overlap optical-flow execution without a write/write race.
    Transition(list,source,D3D12_RESOURCE_STATE_COMMON,read);
    Transition(list,current.Get(),colorRead,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12DescriptorHeap* hs[]={heaps[activeSourceIndex].Get()};list->SetDescriptorHeaps(1,hs);list->SetComputeRootSignature(root.Get());
    list->SetComputeRootDescriptorTable(0,heaps[activeSourceIndex]->GetGPUDescriptorHandleForHeapStart());
    UINT settings[]={rw,rh,history?0u:1u,nvof.Available()?nvof.Grid():4u,
                     nvof.Available()?nvof.FlowWidth():0u,nvof.Available()?nvof.FlowHeight():0u,
                     nvof.Available()&&nvof.HasCost()?1u:0u,
                     nvof.Available()?nvof.SourceWidth():rw,nvof.Available()?nvof.SourceHeight():rh};
    list->SetComputeRoot32BitConstants(1,9,settings,0);
    list->SetPipelineState(resamplePSO.Get());list->Dispatch((rw+7)/8,(rh+7)/8,1);
    Transition(list,current.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,colorRead);
    // The FP16 previous texture is only required by the software motion estimator.
    // With NVOFA active its own BGRA history is maintained separately, so avoid an
    // otherwise redundant ~16.6 MB copy at 1080p.
    if(!history && !nvof.Available()) {
        Transition(list,current.Get(),colorRead,D3D12_RESOURCE_STATE_COPY_SOURCE);Transition(list,previous.Get(),read,D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(previous.Get(),current.Get());
        Transition(list,current.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,colorRead);Transition(list,previous.Get(),D3D12_RESOURCE_STATE_COPY_DEST,read);
    }
}

void DlssExperiment::RecordPostMotion(ID3D12GraphicsCommandList* list,ID3D12Resource* source,UINT sourceIndex) {
    activeSourceIndex=sourceIndex;
    const auto read=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto colorRead=static_cast<D3D12_RESOURCE_STATES>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    // A new command list does not inherit descriptor/root state from the pre list.
    ID3D12DescriptorHeap* hs[]={heaps[activeSourceIndex].Get()};list->SetDescriptorHeaps(1,hs);list->SetComputeRootSignature(root.Get());
    list->SetComputeRootDescriptorTable(0,heaps[activeSourceIndex]->GetGPUDescriptorHandleForHeapStart());
    UINT settings[]={rw,rh,history?0u:1u,nvof.Available()?nvof.Grid():4u,
                     nvof.Available()?nvof.FlowWidth():0u,nvof.Available()?nvof.FlowHeight():0u,
                     nvof.Available()&&nvof.HasCost()?1u:0u,
                     nvof.Available()?nvof.SourceWidth():rw,nvof.Available()?nvof.SourceHeight():rh};
    list->SetComputeRoot32BitConstants(1,9,settings,0);

    Transition(list,motion.Get(),read,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);Transition(list,depth.Get(),read,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if(nvof.Available()&&nvof.FlowReady()) {
        nvof.BeginRead(list);
        list->SetPipelineState(nvofResolvePSO.Get());list->Dispatch((rw+7)/8,(rh+7)/8,1);
        nvof.EndRead(list);
    } else if(nvof.Available()) {
        // NVOFA normally has no vector on the very first frame and may rarely fail
        // a submission.  Do not fall back to a stale FP16 previous frame: clear to
        // zero motion for this reset/recovery frame instead.
        auto cpu=heaps[activeSourceIndex]->GetCPUDescriptorHandleForHeapStart();
        auto gpu=heaps[activeSourceIndex]->GetGPUDescriptorHandleForHeapStart();
        cpu.ptr += SIZE_T(stride)*6; gpu.ptr += UINT64(stride)*6;
        const FLOAT zero[4]={0,0,0,0};
        list->ClearUnorderedAccessViewFloat(gpu,cpu,motion.Get(),zero,0,nullptr);
        cpu=heaps[activeSourceIndex]->GetCPUDescriptorHandleForHeapStart();gpu=heaps[activeSourceIndex]->GetGPUDescriptorHandleForHeapStart();
        cpu.ptr += SIZE_T(stride)*7; gpu.ptr += UINT64(stride)*7;
        const FLOAT flatDepth[4]={1,1,1,1};
        list->ClearUnorderedAccessViewFloat(gpu,cpu,depth.Get(),flatDepth,0,nullptr);
    } else {
        list->SetPipelineState(motionPSO.Get());list->Dispatch((rw+63)/64,(rh+63)/64,1);
    }
    Transition(list,motion.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,read);Transition(list,depth.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,read);
    sl::Extent input{};input.width=rw;input.height=rh;sl::Extent output{};output.width=ow;output.height=oh;
    sl::Resource dr(sl::ResourceType::eTex2d,depth.Get(),read),mv(sl::ResourceType::eTex2d,motion.Get(),read);
    dr.width=mv.width=rw;dr.height=mv.height=rh;
    dr.nativeFormat=DXGI_FORMAT_R32_FLOAT;mv.nativeFormat=DXGI_FORMAT_R16G16_FLOAT;
    sl::ResourceTag inputs[]={
        {&dr,sl::kBufferTypeDepth,sl::eValidUntilPresent,&input},
        {&mv,sl::kBufferTypeMotionVectors,sl::eValidUntilPresent,&input}
    };
    Check(tags(*token,viewport,inputs,2,list),"Tag motion and depth buffers");

    // V5 zero-copy native-resolution presenter path: current already contains the
    // resampled FP16 frame and is readable by both compute and pixel stages.  Use it
    // directly for HUD-less tagging and the final fullscreen draw.
    if(fg) {
        sl::Resource hud(sl::ResourceType::eTex2d,current.Get(),colorRead);
        hud.width=rw;hud.height=rh;hud.nativeFormat=DXGI_FORMAT_R16G16B16A16_FLOAT;
        sl::ResourceTag tag{&hud,sl::kBufferTypeHUDLessColor,sl::eValidUntilPresent,&output};
        Check(tags(*token,viewport,&tag,1,list),"Tag zero-copy HUD-less color");
    }

    // Only the software estimator needs FP16 previous/current history.  NVOFA owns
    // an independent BGRA previous-capture texture, updated below.
    if(!nvof.Available()) {
        Transition(list,current.Get(),colorRead,D3D12_RESOURCE_STATE_COPY_SOURCE);Transition(list,previous.Get(),read,D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(previous.Get(),current.Get());
        Transition(list,current.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,colorRead);Transition(list,previous.Get(),D3D12_RESOURCE_STATE_COPY_DEST,read);
    }
    if(nvof.Available()) nvof.RecordHistoryCopy(list,source,read);
    Transition(list,source,read,D3D12_RESOURCE_STATE_COMMON);history=true;
}
void DlssExperiment::TagBackbuffer(ID3D12GraphicsCommandList* list,ID3D12Resource* backbuffer,D3D12_RESOURCE_STATES state) {
    if(!fg||!token) return;
    sl::Extent full{};full.width=ow;full.height=oh;
    sl::Resource back(sl::ResourceType::eTex2d,backbuffer,state);
    back.width=ow;back.height=oh;back.nativeFormat=DXGI_FORMAT_B8G8R8A8_UNORM;
    sl::ResourceTag tag{&back,sl::kBufferTypeBackbuffer,sl::eValidUntilPresent,&full};
    Check(tags(*token,viewport,&tag,1,list),"Tag final swapchain backbuffer");
}
void DlssExperiment::BeforeFrame(ID3D12CommandQueue* queue) {
    if (inputFence && inputFenceValue)
        HR(queue->Wait(inputFence.Get(), inputFenceValue), "Wait for previous FG input consumption");
}
void DlssExperiment::AfterPresent() {
    if (!fg) return;
    sl::DLSSGState state{};
    auto r = fgState(viewport, state, nullptr);
    if (r != sl::Result::eOk) { reportResult = r; return; }
    reportStatus = static_cast<sl::DLSSGStatus>(
        static_cast<unsigned>(reportStatus) | static_cast<unsigned>(state.status));
    ++reportCalls;
    reportPresents += state.numFramesActuallyPresented;
    inputFence = static_cast<ID3D12Fence*>(state.inputsProcessingCompletionFence);
    inputFenceValue = state.lastPresentInputsProcessingCompletionFenceValue;
}
void DlssExperiment::Suspend(bool value) {
    if (!fg || !fgOptions || value == suspended) return;
    auto options = activeOptions;
    if (value) options.mode = sl::DLSSGMode::eOff;
    Check(fgOptions(viewport, options), "Suspend/resume fixed FG");
    suspended = value;
    framePrepared = false;
    history = false;
    nvof.ResetHistory();
}
std::wstring DlssExperiment::Status() {
    std::wstring s = L"Fixed " + std::to_wstring(activeOptions.numFramesToGenerate + 1) + L"x requested";
    if (reportResult != sl::Result::eOk)
        s += L" | SDK error=" + std::to_wstring(static_cast<int>(reportResult));
    else if (static_cast<unsigned>(reportStatus))
        s += L" | BLOCKED flags=" + std::to_wstring(static_cast<unsigned>(reportStatus));
    else if (reportCalls && reportPresents > reportCalls) {
        wchar_t text[100]{};
        swprintf_s(text, L" | SDK reports %.2fx presents", double(reportPresents) / double(reportCalls));
        s += text;
    } else
        s += L" | No extra frames reported; check sl.log";
    reportCalls = reportPresents = 0;
    reportResult = sl::Result::eOk;
    reportStatus = sl::DLSSGStatus::eOk;
    return s;
}
void DlssExperiment::Release() {
    if(device) {
        if(fg&&fgOptions) {sl::DLSSGOptions o{};o.mode=sl::DLSSGMode::eOff;fgOptions(viewport,o);}
        if(freeResources) {if(sr)freeResources(sl::kFeatureDLSS,viewport);if(fg)freeResources(sl::kFeatureDLSS_G,viewport);}
    }
    nvof.Shutdown();
    token=nullptr;framePrepared=false;history=false;current.Reset();previous.Reset();motion.Reset();depth.Reset();result.Reset();
    inputFence.Reset();inputFenceValue=0;
    resamplePSO.Reset();motionPSO.Reset();nvofResolvePSO.Reset();root.Reset();heaps.clear();device.Reset();
}
void DlssExperiment::Shutdown() {
    Release();if(initialized&&shutdown)shutdown();initialized=false;
    if(module)FreeLibrary(module);module=nullptr;
}
