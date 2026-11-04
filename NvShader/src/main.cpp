
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <d3d9.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <sl_pcl.h>
#include "nvfbc_capture.h"
#include <atomic>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <cstring>
#include <fstream>

using Microsoft::WRL::ComPtr;
static void hrcheck(HRESULT h,const char* what){
  if(FAILED(h))throw std::runtime_error(std::string(what)+" HRESULT "+std::to_string((unsigned)h));
}
static void slcheck(sl::Result h,const char* what){
  if(h!=sl::Result::eOk)throw std::runtime_error(std::string(what)+" Streamline "+std::to_string((int)h));
}
static std::wstring wide(const std::string& str){
  if(str.empty())return L"";
  int n=MultiByteToWideChar(CP_UTF8,0,str.c_str(),-1,nullptr,0);
  if(n<=0)return L"Unknown error";
  std::wstring out(n,L'\0');MultiByteToWideChar(CP_UTF8,0,str.c_str(),-1,out.data(),n);out.pop_back();return out;
}
static void waitGpu(ID3D12Fence* f,uint64_t value){
  if(!f||!value||f->GetCompletedValue()>=value)return;
  HANDLE e=CreateEventW(nullptr,FALSE,FALSE,nullptr);
  if(!e)throw std::runtime_error("CreateEvent");
  HRESULT hr=f->SetEventOnCompletion(value,e);
  if(FAILED(hr)){CloseHandle(e);hrcheck(hr,"SetEventOnCompletion");}
  DWORD result=WaitForSingleObject(e,5000);CloseHandle(e);
  if(result!=WAIT_OBJECT_0)throw std::runtime_error("GPU fence timeout");
}
static void transition(ID3D12GraphicsCommandList* l,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
  if(a==b)return;
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource=r;
  barrier.Transition.StateBefore=a;
  barrier.Transition.StateAfter=b;
  barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  l->ResourceBarrier(1,&barrier);
}
static void uavBarrier(ID3D12GraphicsCommandList* l,ID3D12Resource* r){
  D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;b.UAV.pResource=r;
  l->ResourceBarrier(1,&b);
}
static ComPtr<ID3D12Resource> tex(ID3D12Device* d,UINT w,UINT h,DXGI_FORMAT fmt,D3D12_RESOURCE_STATES state){
  D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rd.Width=w;rd.Height=h;rd.DepthOrArraySize=1;rd.MipLevels=1;rd.Format=fmt;
  rd.SampleDesc.Count=1;rd.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  ComPtr<ID3D12Resource> r;hrcheck(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,state,nullptr,IID_PPV_ARGS(&r)),"texture");
  return r;
}
static ComPtr<ID3DBlob> compileShader(const wchar_t* path,const char* entry,const char* profile){
  ComPtr<ID3DBlob> out,errors;
  HRESULT result=D3DCompileFromFile(path,nullptr,D3D_COMPILE_STANDARD_FILE_INCLUDE,entry,profile,
    D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&out,&errors);
  if(FAILED(result)){
    std::string message="shader compile";
    if(errors)message += ": "+std::string((char*)errors->GetBufferPointer(),errors->GetBufferSize());
    throw std::runtime_error(message);
  }
  return out;
}
struct Params {UINT width,height,pitchCurrent,pitchPrevious,motionWidth,motionHeight,blockWidth,blockHeight,firstFrame,reserved0,reserved1,reserved2;};
struct Config {HWND game{};DWORD pid{};int factor{2};int motionPercent{50};};
static std::atomic<bool> stopRequested{false};
static std::mutex statusLock;
static std::wstring sessionStatus=L"Ready";
static void appendDiagnostic(const std::wstring& value){
  wchar_t exe[MAX_PATH]{};GetModuleFileNameW(nullptr,exe,MAX_PATH);
  const auto path=std::filesystem::path(exe).parent_path()/L"NvShader-Diagnostic.log";
  std::wofstream file(path,std::ios::app);
  if(file)file<<value<<L"\n";
}
static void status(const std::wstring& message){std::lock_guard<std::mutex> lock(statusLock);if(sessionStatus!=message){sessionStatus=message;appendDiagnostic(message);}}
static std::wstring readStatus(){std::lock_guard<std::mutex> lock(statusLock);return sessionStatus;}

class DlssG {
    HMODULE dll_{};
    bool initialized_{},enabled_{};
    sl::FrameToken* token_{};
    uint32_t frameId_{};
    sl::ViewportHandle viewport_{0};
    sl::Constants constants_{};
    sl::Extent extent_{};
    PFun_slInit* init_{};
    PFun_slShutdown* shutdown_{};
    PFun_slSetD3DDevice* setDevice_{};
    PFun_slGetFeatureFunction* getFeature_{};
    PFun_slIsFeatureSupported* supported_{};
    PFun_slGetNewFrameToken* tokenFn_{};
    PFun_slSetConstants* constantsFn_{};
    PFun_slSetTagForFrame* tagFn_{};
    PFun_slDLSSGSetOptions* optionsFn_{};
    PFun_slDLSSGGetState* stateFn_{};
    PFun_slReflexSetOptions* reflexFn_{};
    PFun_slPCLSetMarker* pclFn_{};
    template<typename T> void symbol(T& fn,const char* name){
      fn=reinterpret_cast<T>(GetProcAddress(dll_,name));
      if(!fn)throw std::runtime_error(std::string("Missing Streamline export ")+name);
    }
    template<typename T> void plugin(T& fn,sl::Feature feature,const char* name){
      void* pointer=nullptr;
      slcheck(getFeature_(feature,name,pointer),name);
      fn=reinterpret_cast<T>(pointer);
      if(!fn)throw std::runtime_error(std::string("Missing plugin function ")+name);
    }
public:
    using FactoryFn=HRESULT(WINAPI*)(REFIID,void**);
    using DeviceFn=HRESULT(WINAPI*)(IUnknown*,D3D_FEATURE_LEVEL,REFIID,void**);
    FactoryFn factory{};DeviceFn deviceCreate{};
    void initialize() {
      wchar_t exe[32768]{};
      GetModuleFileNameW(nullptr,exe,32768);
      std::wstring directory=std::filesystem::path(exe).parent_path().wstring();
      std::wstring file=directory+L"\\sl.interposer.dll";
      dll_=LoadLibraryW(file.c_str());
      if(!dll_)throw std::runtime_error("Missing sl.interposer.dll beside executable");
      symbol(init_,"slInit");symbol(shutdown_,"slShutdown");
      symbol(setDevice_,"slSetD3DDevice");symbol(getFeature_,"slGetFeatureFunction");
      symbol(supported_,"slIsFeatureSupported");symbol(tokenFn_,"slGetNewFrameToken");
      symbol(constantsFn_,"slSetConstants");symbol(tagFn_,"slSetTagForFrame");
      symbol(factory,"CreateDXGIFactory1");symbol(deviceCreate,"D3D12CreateDevice");
      const wchar_t* paths[]={directory.c_str()};
      sl::Feature features[]={sl::kFeatureDLSS_G,sl::kFeatureReflex,sl::kFeaturePCL};
      sl::Preferences p{};p.pathsToPlugins=paths;p.numPathsToPlugins=1;
      p.pathToLogsAndData=directory.c_str();p.featuresToLoad=features;p.numFeaturesToLoad=3;
      p.logLevel=sl::LogLevel::eOff;
      p.flags=sl::PreferenceFlags::eUseDXGIFactoryProxy|sl::PreferenceFlags::eUseFrameBasedResourceTagging;
      p.engine=sl::EngineType::eCustom;p.engineVersion="NvShaderFresh/1.0";
      p.projectId="c4e7b76a-1bc0-4d38-aad0-6ba998e4733c";
      p.renderAPI=sl::RenderAPI::eD3D12;
      slcheck(init_(p,sl::kSDKVersion),"Streamline init");
      initialized_=true;
    }
    void bind(ID3D12Device* dev){slcheck(setDevice_(dev),"Streamline device");}
    void configure(IDXGIAdapter1* adapter,UINT w,UINT h,int multiplier){
      enabled_=multiplier>1;extent_={0,0,w,h};
      if(!enabled_)return;
      DXGI_ADAPTER_DESC1 desc{};hrcheck(adapter->GetDesc1(&desc),"Adapter desc");
      sl::AdapterInfo info{};info.deviceLUID=reinterpret_cast<uint8_t*>(&desc.AdapterLuid);info.deviceLUIDSizeInBytes=sizeof(LUID);
      slcheck(supported_(sl::kFeatureDLSS_G,info),"DLSS-G supported");
      plugin(optionsFn_,sl::kFeatureDLSS_G,"slDLSSGSetOptions");
      plugin(stateFn_,sl::kFeatureDLSS_G,"slDLSSGGetState");
      plugin(reflexFn_,sl::kFeatureReflex,"slReflexSetOptions");
      plugin(pclFn_,sl::kFeaturePCL,"slPCLSetMarker");
      sl::DLSSGOptions options{};
      options.mode=sl::DLSSGMode::eOff;
      options.numBackBuffers=2;
      options.numFramesToGenerate=multiplier-1;
      options.colorWidth=w;options.colorHeight=h;
      options.colorBufferFormat=DXGI_FORMAT_B8G8R8A8_UNORM;
      options.mvecDepthWidth=w;options.mvecDepthHeight=h;
      options.mvecBufferFormat=DXGI_FORMAT_R16G16_FLOAT;
      options.depthBufferFormat=DXGI_FORMAT_R32_FLOAT;
      options.enableUserInterfaceRecomposition=sl::Boolean::eFalse;
      sl::DLSSGState state{};
      slcheck(stateFn_(viewport_,state,&options),"DLSS-G capabilities");
      if((unsigned)(multiplier-1)>state.numFramesToGenerateMax)throw std::runtime_error("Requested multiplier exceeds DLSS-G capability");
      sl::ReflexOptions reflex{};reflex.mode=sl::ReflexMode::eLowLatencyWithBoost;
      slcheck(reflexFn_(reflex),"Reflex configure");
      options.mode=sl::DLSSGMode::eOn;
      slcheck(optionsFn_(viewport_,options),"DLSS-G enable");
      sl::float4x4 identity{};
      identity[0]={1,0,0,0};identity[1]={0,1,0,0};identity[2]={0,0,1,0};identity[3]={0,0,0,1};
      constants_.cameraViewToClip=constants_.clipToCameraView=constants_.clipToLensClip=
       constants_.clipToPrevClip=constants_.prevClipToClip=identity;
      constants_.mvecScale={1.f/w,1.f/h};
      constants_.cameraUp={0,1,0};constants_.cameraRight={1,0,0};constants_.cameraFwd={0,0,1};
      constants_.cameraNear=0.1f;constants_.cameraFar=1000;
      constants_.cameraFOV=1.04719755f;constants_.cameraAspectRatio=float(w)/float(h);
      constants_.cameraMotionIncluded=sl::eTrue;constants_.motionVectors3D=sl::eFalse;
      constants_.orthographicProjection=sl::eTrue;
      constants_.motionVectorsInvalidValue=65504;
    }
    void begin(bool reset){
      if(!enabled_)return;
      slcheck(tokenFn_(token_,&frameId_),"Frame token");++frameId_;
      constants_.reset=reset?sl::eTrue:sl::eFalse;
      slcheck(constantsFn_(constants_,*token_,viewport_),"Frame constants");
      marker(sl::PCLMarker::eSimulationStart);
      marker(sl::PCLMarker::eSimulationEnd);
      marker(sl::PCLMarker::eRenderSubmitStart);
    }
    void marker(sl::PCLMarker m){if(enabled_)slcheck(pclFn_(m,*token_),"PCL marker");}
    void tag(ID3D12GraphicsCommandList* cmd,ID3D12Resource* motion,ID3D12Resource* depth,ID3D12Resource* back,UINT w,UINT h){
      if(!enabled_)return;
      sl::Resource mv(sl::ResourceType::eTex2d,motion,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      sl::Resource d(sl::ResourceType::eTex2d,depth,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      sl::Resource b(sl::ResourceType::eTex2d,back,D3D12_RESOURCE_STATE_PRESENT);
      mv.width=d.width=b.width=w;mv.height=d.height=b.height=h;
      mv.nativeFormat=DXGI_FORMAT_R16G16_FLOAT;
      d.nativeFormat=DXGI_FORMAT_R32_FLOAT;b.nativeFormat=DXGI_FORMAT_B8G8R8A8_UNORM;
      sl::ResourceTag tags[]={
        {&mv,sl::kBufferTypeMotionVectors,sl::eValidUntilPresent,&extent_},
        {&d,sl::kBufferTypeDepth,sl::eValidUntilPresent,&extent_},
        {&b,sl::kBufferTypeBackbuffer,sl::eValidUntilPresent,&extent_}
      };
      slcheck(tagFn_(*token_,viewport_,tags,3,cmd),"DLSS-G input resources");
    }
    std::pair<ComPtr<ID3D12Fence>,uint64_t> afterPresent(){
      if(!enabled_)return {};
      sl::DLSSGState state{};
      slcheck(stateFn_(viewport_,state,nullptr),"DLSS-G frame state");
      std::pair<ComPtr<ID3D12Fence>,uint64_t> out;
      if(state.inputsProcessingCompletionFence&&state.lastPresentInputsProcessingCompletionFenceValue){
        out.first=static_cast<ID3D12Fence*>(state.inputsProcessingCompletionFence);
        out.second=state.lastPresentInputsProcessingCompletionFenceValue;
      }
      return out;
    }
    void shutdown(){
      if(initialized_){shutdown_();initialized_=false;}
      if(dll_){FreeLibrary(dll_);dll_=nullptr;}
    }
    ~DlssG(){shutdown();}
};

struct Slot {
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Resource> flow,motion,depth,color;
  ComPtr<ID3D12Fence> streamlineFence;
  uint64_t streamlineValue{},done{};
};
struct Engine {
  Config config;
  HWND overlay{};
  RECT monitorRect{};
  UINT sourceW{},sourceH{},outputW{},outputH{},motionW{},motionH{},blocksW{},blocksH{};
  HMONITOR monitor{};
  ComPtr<IDXGIAdapter1> adapter;
  ComPtr<IDXGIFactory4> factory;
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<IDXGISwapChain3> swap;
  ComPtr<ID3D12Fence> captureReady,finished;
  std::array<ComPtr<ID3D12Resource>,4> captured;
  std::array<uint64_t,4> rawDone{};
  std::array<Slot,3> slots;
  ComPtr<ID3D12DescriptorHeap> descriptors,rtvs;
  ComPtr<ID3D12RootSignature> computeRoot,drawRoot;
  ComPtr<ID3D12PipelineState> flowPipeline,decodePipeline,drawPipeline;
  ComPtr<ID3D12Resource> back[2];
  NvFbcCaptureBackend capture;
  DlssG fg;
  UINT descriptorSize{},rtvSize{};
  uint64_t nextReady{},nextDone{},frameNo{};
  bool tearing{},activeFg{},running{};
  std::wstring shaderFile;
  Engine(Config cfg):config(cfg){}
  ~Engine(){
    try{if(queue)waitGpu(finished.Get(),nextDone);}catch(...){}
    capture.Shutdown();fg.shutdown();
    if(overlay)DestroyWindow(overlay);
  }
  D3D12_CPU_DESCRIPTOR_HANDLE cpu(UINT i){
    auto a=descriptors->GetCPUDescriptorHandleForHeapStart();
    a.ptr+=SIZE_T(i)*descriptorSize;return a;
  }
  D3D12_GPU_DESCRIPTOR_HANDLE gpu(UINT i){
    auto a=descriptors->GetGPUDescriptorHandleForHeapStart();
    a.ptr+=UINT64(i)*descriptorSize;return a;
  }
  void makeSrvRaw(UINT descriptor,ID3D12Resource* r){
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format=DXGI_FORMAT_R32_TYPELESS;
    desc.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
    desc.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Buffer.NumElements=(UINT)(r->GetDesc().Width/4);
    desc.Buffer.Flags=D3D12_BUFFER_SRV_FLAG_RAW;
    device->CreateShaderResourceView(r,&desc,cpu(descriptor));
  }
  void makeSrvTex(UINT descriptor,ID3D12Resource* r,DXGI_FORMAT format){
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format=format;desc.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Texture2D.MipLevels=1;
    device->CreateShaderResourceView(r,&desc,cpu(descriptor));
  }
  void makeUav(UINT descriptor,ID3D12Resource* r,DXGI_FORMAT format){
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    desc.Format=format;desc.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(r,nullptr,&desc,cpu(descriptor));
  }
  void selectAdapter(){
    ComPtr<IDXGIFactory1> raw;
    hrcheck(CreateDXGIFactory1(IID_PPV_ARGS(&raw)),"enumerate adapters");
    monitor=MonitorFromWindow(config.game,MONITOR_DEFAULTTONEAREST);
    for(UINT i=0;;i++){
      ComPtr<IDXGIAdapter1> a;
      if(raw->EnumAdapters1(i,&a)==DXGI_ERROR_NOT_FOUND)break;
      for(UINT j=0;;j++){
        ComPtr<IDXGIOutput> output;
        if(a->EnumOutputs(j,&output)==DXGI_ERROR_NOT_FOUND)break;
        DXGI_OUTPUT_DESC desc{};hrcheck(output->GetDesc(&desc),"monitor");
        if(desc.Monitor==monitor){
          adapter=a;monitorRect=desc.DesktopCoordinates;break;
        }
      }
      if(adapter)break;
    }
    if(!adapter)throw std::runtime_error("Game monitor adapter not found");
    DXGI_ADAPTER_DESC1 desc{};hrcheck(adapter->GetDesc1(&desc),"adapter");
    if(desc.VendorId!=0x10de)throw std::runtime_error("Select a game on an NVIDIA monitor");
    outputW=monitorRect.right-monitorRect.left;
    outputH=monitorRect.bottom-monitorRect.top;
    if(!outputW||!outputH)throw std::runtime_error("Invalid monitor geometry");
  }
  void createOverlay(){
    overlay=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TRANSPARENT,
      L"NvShaderOverlay",L"NvShaderPresenter",WS_POPUP|WS_DISABLED,
      monitorRect.left,monitorRect.top,outputW,outputH,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!overlay)throw std::runtime_error("Cannot create presenter window");
    // Disabled top-level windows do not receive mouse or keyboard input;
    // WS_EX_TRANSPARENT alone is not reliable across process boundaries.
    EnableWindow(overlay,FALSE);
    ShowWindow(overlay,SW_SHOWNOACTIVATE);
    SetWindowPos(overlay,HWND_TOPMOST,monitorRect.left,monitorRect.top,outputW,outputH,
      SWP_NOACTIVATE|SWP_SHOWWINDOW);
  }
  void createD3D(){
    activeFg=config.factor>1;
    if(activeFg){
      fg.initialize();
      hrcheck(fg.factory(IID_PPV_ARGS(&factory)),"Streamline DXGI factory");
      hrcheck(fg.deviceCreate(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"Streamline DX12 device");
      fg.bind(device.Get());
    }else{
      hrcheck(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DXGI factory");
      hrcheck(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"D3D12 device");
    }
    D3D12_COMMAND_QUEUE_DESC desc{};desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    desc.Priority=D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    HRESULT result=device->CreateCommandQueue(&desc,IID_PPV_ARGS(&queue));
    if(FAILED(result)){desc.Priority=D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
      hrcheck(device->CreateCommandQueue(&desc,IID_PPV_ARGS(&queue)),"command queue");}
    hrcheck(device->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&captureReady)),"shared capture fence");
    hrcheck(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&finished)),"finish fence");
    ID3D12Resource* allocated[4]{};
    std::wstring error;
    bool okay=capture.Bind(device.Get(),captureReady.Get(),allocated,error);
    for(UINT i=0;i<4;i++)captured[i].Attach(allocated[i]);
    if(!okay)throw std::runtime_error(std::string(error.begin(),error.end()));
    // Actual NvFBC dimensions come from the validated game-PID capture, not the monitor.
    if(activeFg)fg.configure(adapter.Get(),outputW,outputH,config.factor);
    ComPtr<IDXGIFactory5> f5;
    BOOL support=FALSE;
    if(SUCCEEDED(factory.As(&f5)))f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,&support,sizeof(support));
    tearing=support!=FALSE;
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width=outputW;sd.Height=outputH;sd.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sd.BufferCount=2;
    sd.SampleDesc.Count=1;sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Scaling=DXGI_SCALING_STRETCH;sd.AlphaMode=DXGI_ALPHA_MODE_IGNORE;
    sd.Flags=tearing?DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING:0;
    ComPtr<IDXGISwapChain1> s;
    hrcheck(factory->CreateSwapChainForHwnd(queue.Get(),overlay,&sd,nullptr,nullptr,&s),"present swapchain");
    hrcheck(s.As(&swap),"swapchain interface");
    factory->MakeWindowAssociation(overlay,DXGI_MWA_NO_ALT_ENTER);
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=2;
    hrcheck(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtvs)),"rtv heap");
    rtvSize=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    auto handle=rtvs->GetCPUDescriptorHandleForHeapStart();
    for(UINT i=0;i<2;i++){
      hrcheck(swap->GetBuffer(i,IID_PPV_ARGS(&back[i])),"swap buffer");
      device->CreateRenderTargetView(back[i].Get(),nullptr,handle);handle.ptr+=rtvSize;
    }
    hd={};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=3*8;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hrcheck(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&descriptors)),"shader heap");
    descriptorSize=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  }
  void makePipelines(){
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;ranges[0].NumDescriptors=3;ranges[0].BaseShaderRegister=0;
    ranges[1].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_UAV;ranges[1].NumDescriptors=4;ranges[1].BaseShaderRegister=0;
    D3D12_ROOT_PARAMETER rp[3]{};
    rp[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable={1,&ranges[0]};rp[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    rp[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable={1,&ranges[1]};rp[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    rp[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[2].Constants={0,0,16};rp[2].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rs{};rs.NumParameters=3;rs.pParameters=rp;
    ComPtr<ID3DBlob> blob,error;
    hrcheck(D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error),"serialize compute root");
    hrcheck(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&computeRoot)),"compute root");
    auto shader=compileShader(shaderFile.c_str(),"Flow4x4","cs_5_1");
    D3D12_COMPUTE_PIPELINE_STATE_DESC cs{};cs.pRootSignature=computeRoot.Get();
    cs.CS={shader->GetBufferPointer(),shader->GetBufferSize()};
    hrcheck(device->CreateComputePipelineState(&cs,IID_PPV_ARGS(&flowPipeline)),"motion PSO");
    shader=compileShader(shaderFile.c_str(),"ExpandAndDecode","cs_5_1");
    cs.CS={shader->GetBufferPointer(),shader->GetBufferSize()};
    hrcheck(device->CreateComputePipelineState(&cs,IID_PPV_ARGS(&decodePipeline)),"decode PSO");
    D3D12_DESCRIPTOR_RANGE dr{};dr.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;dr.NumDescriptors=1;dr.BaseShaderRegister=0;
    D3D12_ROOT_PARAMETER pr{};pr.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    pr.DescriptorTable={1,&dr};pr.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderRegister=0;sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    rs={};rs.NumParameters=1;rs.pParameters=&pr;rs.NumStaticSamplers=1;rs.pStaticSamplers=&sampler;
    rs.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    blob.Reset();error.Reset();
    hrcheck(D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error),"serialize draw root");
    hrcheck(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&drawRoot)),"draw root");
    auto vs=compileShader(shaderFile.c_str(),"FullscreenVS","vs_5_1");
    auto ps=compileShader(shaderFile.c_str(),"FullscreenPS","ps_5_1");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC gr{};gr.pRootSignature=drawRoot.Get();
    gr.VS={vs->GetBufferPointer(),vs->GetBufferSize()};
    gr.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
    gr.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;
    gr.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
    gr.RasterizerState.DepthClipEnable=TRUE;
    gr.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    gr.DepthStencilState.DepthEnable=FALSE;gr.DepthStencilState.StencilEnable=FALSE;
    gr.SampleMask=UINT_MAX;gr.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    gr.NumRenderTargets=1;gr.RTVFormats[0]=DXGI_FORMAT_B8G8R8A8_UNORM;gr.SampleDesc.Count=1;
    hrcheck(device->CreateGraphicsPipelineState(&gr,IID_PPV_ARGS(&drawPipeline)),"draw PSO");
  }

  void makeSlots(){
    motionW=std::max<UINT>(4,(sourceW*config.motionPercent+99)/100);
    motionH=std::max<UINT>(4,(sourceH*config.motionPercent+99)/100);
    blocksW=(motionW+3)/4;blocksH=(motionH+3)/4;
    for(auto& slot:slots){
      slot.flow=tex(device.Get(),blocksW,blocksH,DXGI_FORMAT_R16G16_FLOAT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      slot.motion=tex(device.Get(),outputW,outputH,DXGI_FORMAT_R16G16_FLOAT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      slot.depth=tex(device.Get(),outputW,outputH,DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      slot.color=tex(device.Get(),outputW,outputH,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      hrcheck(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&slot.allocator)),"command allocator");
      hrcheck(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,slot.allocator.Get(),nullptr,IID_PPV_ARGS(&slot.list)),"command list");
      hrcheck(slot.list->Close(),"close initial list");
    }
  }
  void setDescriptors(UINT index,UINT current,UINT previous){
    const UINT start=index*8;
    makeSrvRaw(start+0,captured[current].Get());
    makeSrvRaw(start+1,captured[previous].Get());
    auto& s=slots[index];
    makeSrvTex(start+2,s.flow.Get(),DXGI_FORMAT_R16G16_FLOAT);
    makeUav(start+3,s.flow.Get(),DXGI_FORMAT_R16G16_FLOAT);
    makeUav(start+4,s.motion.Get(),DXGI_FORMAT_R16G16_FLOAT);
    makeUav(start+5,s.depth.Get(),DXGI_FORMAT_R32_FLOAT);
    makeUav(start+6,s.color.Get(),DXGI_FORMAT_R8G8B8A8_UNORM);
    makeSrvTex(start+7,s.color.Get(),DXGI_FORMAT_R8G8B8A8_UNORM);
  }
  void doFrame(UINT rawCurrent,UINT rawPrevious,uint64_t capturedSeq,bool first){
    UINT index=UINT(frameNo%slots.size());
    Slot& slot=slots[index];
    waitGpu(finished.Get(),slot.done);
    waitGpu(slot.streamlineFence.Get(),slot.streamlineValue);
    // Reuse is now safe for both our command queue and Streamline's async FG queue.
    setDescriptors(index,rawCurrent,rawPrevious);
    hrcheck(slot.allocator->Reset(),"reset allocator");
    hrcheck(slot.list->Reset(slot.allocator.Get(),nullptr),"reset list");
    auto* cmd=slot.list.Get();
    ID3D12DescriptorHeap* heaps[]={descriptors.Get()};
    cmd->SetDescriptorHeaps(1,heaps);
    hrcheck(queue->Wait(captureReady.Get(),capturedSeq),"wait CUDA frame");
    transition(cmd,captured[rawCurrent].Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if(rawPrevious!=rawCurrent)
      transition(cmd,captured[rawPrevious].Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if(slot.done){
      transition(cmd,slot.motion.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      transition(cmd,slot.depth.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    Params p{};
    p.width=sourceW;p.height=sourceH;
    p.pitchCurrent=capture.Pitch();p.pitchPrevious=capture.Pitch();
    p.motionWidth=motionW;p.motionHeight=motionH;p.blockWidth=blocksW;p.blockHeight=blocksH;
    p.firstFrame=first?1:0;
    p.reserved0=outputW;p.reserved1=outputH;
    cmd->SetComputeRootSignature(computeRoot.Get());
    cmd->SetComputeRootDescriptorTable(0,gpu(index*8+0));
    cmd->SetComputeRootDescriptorTable(1,gpu(index*8+3));
    cmd->SetComputeRoot32BitConstants(2,12,&p,0);
    cmd->SetPipelineState(flowPipeline.Get());
    cmd->Dispatch((blocksW+7)/8,(blocksH+7)/8,1);
    uavBarrier(cmd,slot.flow.Get());
    transition(cmd,slot.flow.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cmd->SetPipelineState(decodePipeline.Get());
    cmd->Dispatch((outputW+7)/8,(outputH+7)/8,1);
    uavBarrier(cmd,slot.motion.Get());
    uavBarrier(cmd,slot.depth.Get());
    uavBarrier(cmd,slot.color.Get());
    transition(cmd,slot.motion.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(cmd,slot.depth.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(cmd,slot.flow.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(cmd,slot.color.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(cmd,captured[rawCurrent].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
    if(rawPrevious!=rawCurrent)
      transition(cmd,captured[rawPrevious].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
    UINT bi=swap->GetCurrentBackBufferIndex();
    auto* b=back[bi].Get();
    transition(cmd,b,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv=rtvs->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr+=SIZE_T(bi)*rtvSize;
    cmd->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
    D3D12_VIEWPORT view{0.f,0.f,float(outputW),float(outputH),0.f,1.f};
    D3D12_RECT rect{0,0,(LONG)outputW,(LONG)outputH};
    cmd->RSSetViewports(1,&view);cmd->RSSetScissorRects(1,&rect);
    cmd->SetGraphicsRootSignature(drawRoot.Get());
    cmd->SetGraphicsRootDescriptorTable(0,gpu(index*8+7));
    cmd->SetPipelineState(drawPipeline.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3,1,0,0);
    transition(cmd,b,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);
    transition(cmd,slot.color.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if(activeFg)fg.tag(cmd,slot.motion.Get(),slot.depth.Get(),b,outputW,outputH);
    hrcheck(cmd->Close(),"close command list");
    ID3D12CommandList* lists[]={cmd};
    queue->ExecuteCommandLists(1,lists);
    slot.done=++nextDone;
    hrcheck(queue->Signal(finished.Get(),slot.done),"signal finished");
    rawDone[rawCurrent]=std::max(rawDone[rawCurrent],slot.done);
    rawDone[rawPrevious]=std::max(rawDone[rawPrevious],slot.done);
    if(activeFg)fg.marker(sl::PCLMarker::eRenderSubmitEnd);
    if(activeFg)fg.marker(sl::PCLMarker::ePresentStart);
    HRESULT present=swap->Present(0,tearing?DXGI_PRESENT_ALLOW_TEARING:0);
    if(activeFg)fg.marker(sl::PCLMarker::ePresentEnd);
    hrcheck(present,"DXGI Present");
    if(activeFg){
      auto fence=fg.afterPresent();
      slot.streamlineFence=fence.first;slot.streamlineValue=fence.second;
    }
  }
  void run(){
    selectAdapter();
    createOverlay();
    std::wstring err;
    if(!capture.Init(overlay,config.pid,monitor,monitorRect,monitorRect,outputW,outputH,err))
      throw std::runtime_error(std::string(err.begin(),err.end()));
    sourceW=capture.Width();sourceH=capture.Height();
    if(!sourceW||!sourceH)throw std::runtime_error("NvFBC returned zero-sized frame");
    status(L"NvFBC CUDA active: "+std::to_wstring(sourceW)+L"x"+std::to_wstring(sourceH));
    status(L"Creating DX12 / Streamline presenter...");
    createD3D();
    wchar_t modulePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr,modulePath,MAX_PATH);
    shaderFile=(std::filesystem::path(modulePath).parent_path()/L"shader_motion.hlsl").wstring();
    status(L"Compiling shaders...");
    makePipelines();
    status(L"Allocating shader resources...");
    makeSlots();
    status(L"Presenter ready (message pump enabled). Activate game window.");
    running=true;
    UINT previousRaw=0;bool first=true;
    while(!stopRequested.load(std::memory_order_acquire)){
      // A worker-owned HWND must pump its thread message queue or DWM/Win32
      // activation and visibility changes can stall despite successful Present calls.
      MSG overlayMessage{};
      while(PeekMessageW(&overlayMessage,nullptr,0,0,PM_REMOVE)){
        if(overlayMessage.message==WM_QUIT){stopRequested.store(true);break;}
        TranslateMessage(&overlayMessage);
        DispatchMessageW(&overlayMessage);
      }
      if(stopRequested.load())break;
      if(!IsWindow(config.game))throw std::runtime_error("Selected game was closed");
      DWORD focusedPid=0;GetWindowThreadProcessId(GetForegroundWindow(),&focusedPid);
      if(focusedPid!=config.pid){
        if(first)status(L"Waiting for game focus — Alt+Tab to game");
        ShowWindow(overlay,SW_HIDE);
        first=true;Sleep(8);continue;
      }
      if(first)status(L"Game focused. Capturing first frame...");
      ShowWindow(overlay,SW_SHOWNOACTIVATE);
      SetWindowPos(overlay,HWND_TOPMOST,monitorRect.left,monitorRect.top,outputW,outputH,
                   SWP_NOACTIVATE|SWP_SHOWWINDOW|SWP_NOSENDCHANGING);
      UINT currentRaw=UINT(frameNo%captured.size());
      waitGpu(finished.Get(),rawDone[currentRaw]);
      LARGE_INTEGER stamped{};
      auto result=capture.Grab(currentRaw,stamped,err);
      if(result==NvFbcGrabResult::Recreate){
        if(!capture.RecreateSession(err))throw std::runtime_error("NvFBC recreation failed");
        first=true;continue;
      }
      if(result==NvFbcGrabResult::Resize)throw std::runtime_error("Game resolution changed; press Start again");
      if(result==NvFbcGrabResult::Protected)throw std::runtime_error("NvFBC returned protected content");
      if(result==NvFbcGrabResult::Timeout)continue;
      if(result!=NvFbcGrabResult::Frame)throw std::runtime_error(std::string(err.begin(),err.end()));
      if(capture.Width()!=sourceW||capture.Height()!=sourceH)
        throw std::runtime_error("Capture resized; press Start again");
      if(!capture.Signal(++nextReady,err))
        throw std::runtime_error(std::string(err.begin(),err.end()));
      if(activeFg)fg.begin(first);
      doFrame(currentRaw,first?currentRaw:previousRaw,nextReady,first);
      previousRaw=currentRaw;
      if(first)status(L"Presenting frames");
      first=false;++frameNo;
    }
    status(L"Stopped");
  }
};

static HWND gWindow{},gGames{},gFactor{},gQuality{},gStart{},gStop{},gStatus{};
static std::thread gWorker;
static std::vector<HWND> gGameWindows;
static constexpr int ID_GAME=101,ID_FACTOR=102,ID_QUALITY=103,ID_START=104,ID_STOP=105,ID_REFRESH=106;
static BOOL CALLBACK enumGames(HWND h,LPARAM){
  if(!IsWindowVisible(h)||h==gWindow||GetWindow(h,GW_OWNER))return TRUE;
  wchar_t name[256]{};
  if(GetWindowTextW(h,name,256)<2)return TRUE;
  DWORD pid=0;GetWindowThreadProcessId(h,&pid);
  if(!pid||pid==GetCurrentProcessId())return TRUE;
  LONG_PTR style=GetWindowLongPtrW(h,GWL_STYLE);
  if(!(style&WS_CAPTION)&&!(style&WS_POPUP))return TRUE;
  gGameWindows.push_back(h);
  int i=(int)SendMessageW(gGames,CB_ADDSTRING,0,(LPARAM)name);
  SendMessageW(gGames,CB_SETITEMDATA,i,(LPARAM)h);
  return TRUE;
}
static void refreshGames(){
  HWND prev=(HWND)SendMessageW(gGames,CB_GETITEMDATA,SendMessageW(gGames,CB_GETCURSEL,0,0),0);
  gGameWindows.clear();SendMessageW(gGames,CB_RESETCONTENT,0,0);
  EnumWindows(enumGames,0);
  UINT select=0;
  for(UINT i=0;i<gGameWindows.size();i++)if(gGameWindows[i]==prev)select=i;
  if(!gGameWindows.empty())SendMessageW(gGames,CB_SETCURSEL,select,0);
}
static void halt(){
  stopRequested.store(true,std::memory_order_release);
  if(gWorker.joinable())gWorker.join();
}
static void startSelected(){
  halt();
  int i=(int)SendMessageW(gGames,CB_GETCURSEL,0,0);
  if(i<0)return;
  HWND game=(HWND)SendMessageW(gGames,CB_GETITEMDATA,i,0);
  if(!IsWindow(game))return;
  int fg=(int)SendMessageW(gFactor,CB_GETCURSEL,0,0);
  int mode=(int)SendMessageW(gQuality,CB_GETCURSEL,0,0);
  static constexpr int percent[]={100,50,25,10};
  Config config{};config.game=game;
  GetWindowThreadProcessId(game,&config.pid);
  config.factor=std::max(1,fg+1);
  config.motionPercent=percent[std::clamp(mode,0,3)];
  stopRequested.store(false,std::memory_order_release);
  status(L"Starting NvFBC and Streamline...");
  // Start is invoked from the foreground controller thread: Windows permits
  // this thread to activate the chosen game without input injection.
  if(IsIconic(game))ShowWindow(game,SW_RESTORE);
  if(!SetForegroundWindow(game))status(L"Started. Click the game once if focus is blocked by Windows.");
  gWorker=std::thread([config](){
    try{
      Engine app(config);
      app.run();
    }catch(const std::exception& e){status(L"Error: "+wide(e.what()));MessageBoxW(gWindow,wide(e.what()).c_str(),L"NvShader failed to start",MB_OK|MB_ICONERROR); }
  });
}
static LRESULT CALLBACK overlayProc(HWND h,UINT msg,WPARAM w,LPARAM l){
  switch(msg){
    case WM_MOUSEACTIVATE:return MA_NOACTIVATE;
    case WM_NCHITTEST:return HTTRANSPARENT;
    case WM_ERASEBKGND:return 1;
  }
  return DefWindowProcW(h,msg,w,l);
}
static void label(HWND parent,const wchar_t* caption,int x,int y,int w){
  CreateWindowExW(0,L"STATIC",caption,WS_CHILD|WS_VISIBLE,x,y,w,22,parent,nullptr,GetModuleHandleW(nullptr),nullptr);
}
static HWND dropdown(HWND parent,int id,int x,int y,int width){
  return CreateWindowExW(0,L"COMBOBOX",nullptr,WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,
     x,y,width,230,parent,(HMENU)(INT_PTR)id,GetModuleHandleW(nullptr),nullptr);
}
static LRESULT CALLBACK controllerProc(HWND h,UINT msg,WPARAM w,LPARAM l){
  switch(msg){
    case WM_CREATE:{
      gWindow=h;
      label(h,L"Game window:",18,17,360);
      gGames=dropdown(h,ID_GAME,18,43,370);
      CreateWindowExW(0,L"BUTTON",L"Refresh",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,391,43,72,26,h,
          (HMENU)(INT_PTR)ID_REFRESH,GetModuleHandleW(nullptr),nullptr);
      label(h,L"Frame generation:",18,92,180);
      gFactor=dropdown(h,ID_FACTOR,18,115,180);
      const wchar_t* factors[]={L"Off (native)",L"2x",L"3x",L"4x",L"5x",L"6x"};
      for(auto f:factors)SendMessageW(gFactor,CB_ADDSTRING,0,(LPARAM)f);
      SendMessageW(gFactor,CB_SETCURSEL,1,0);
      label(h,L"4x4 shader motion:",216,92,180);
      gQuality=dropdown(h,ID_QUALITY,216,115,247);
      const wchar_t* quality[]={L"Native resolution",L"50% - balanced",L"25% - performance",L"10% - ultra-light"};
      for(auto q:quality)SendMessageW(gQuality,CB_ADDSTRING,0,(LPARAM)q);
      SendMessageW(gQuality,CB_SETCURSEL,1,0);
      gStart=CreateWindowExW(0,L"BUTTON",L"Start",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
          18,165,135,30,h,(HMENU)(INT_PTR)ID_START,GetModuleHandleW(nullptr),nullptr);
      gStop=CreateWindowExW(0,L"BUTTON",L"Stop",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
          167,165,135,30,h,(HMENU)(INT_PTR)ID_STOP,GetModuleHandleW(nullptr),nullptr);
      gStatus=CreateWindowExW(0,L"STATIC",L"Ready",WS_CHILD|WS_VISIBLE|SS_LEFT,
          18,212,445,70,h,nullptr,GetModuleHandleW(nullptr),nullptr);
      refreshGames();
      SetTimer(h,1,400,nullptr);
      return 0;
    }
    case WM_TIMER:{
      auto message=readStatus();
      SetWindowTextW(gStatus,message.c_str());
      return 0;
    }
    case WM_COMMAND:{
      int id=LOWORD(w);
      if(id==ID_START)startSelected();
      else if(id==ID_STOP){halt();status(L"Stopped");}
      else if(id==ID_REFRESH)refreshGames();
      return 0;
    }
    case WM_CLOSE:DestroyWindow(h);return 0;
    case WM_DESTROY:KillTimer(h,1);halt();PostQuitMessage(0);return 0;
  }
  return DefWindowProcW(h,msg,w,l);
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int show){
  INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES};InitCommonControlsEx(&controls);
  WNDCLASSEXW c{sizeof(c)};c.hInstance=instance;c.lpszClassName=L"NvShaderController";
  c.lpfnWndProc=controllerProc;c.hCursor=LoadCursorW(nullptr,IDC_ARROW);
  c.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
  RegisterClassExW(&c);
  c.lpszClassName=L"NvShaderOverlay";c.lpfnWndProc=overlayProc;c.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH);
  RegisterClassExW(&c);
  HWND window=CreateWindowExW(0,L"NvShaderController",L"NvFBC Shader Motion + DLSS-G Preset C",
    WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,CW_USEDEFAULT,CW_USEDEFAULT,500,340,nullptr,nullptr,instance,nullptr);
  if(!window)return 1;
  ShowWindow(window,show);UpdateWindow(window);
  MSG msg{};
  while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
  return 0;
}
