#include <windows.h>
#include <commctrl.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <sl_pcl.h>
#include <atomic>
#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <utility>
#include <cstring>
#include <cwchar>
#include <exception>
#include "nvfbc_capture.h"
#include "mailbox.h"

// Match the D3D12 Agility runtime version documented in the supplied V39 ZIP.
// D3D12/D3D12Core.dll must be packaged next to the executable.
extern "C" {
    __declspec(dllexport) extern const UINT D3D12SDKVersion = 620;
    __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}

static void startupLog(const std::wstring& message){
    static std::mutex fileMutex;
    std::lock_guard<std::mutex> guard(fileMutex);
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr,exePath,MAX_PATH);
    std::wstring path(exePath);
    auto lastSlash=path.find_last_of(L"\\/");
    if(lastSlash!=std::wstring::npos)path.resize(lastSlash+1);
    else path.clear();
    path+=L"FreshMotionFG-Diagnostic.log";
    SYSTEMTIME now{};GetLocalTime(&now);
    wchar_t stamp[48]{};
    swprintf_s(stamp,L"[%02u:%02u:%02u.%03u] ",now.wHour,now.wMinute,now.wSecond,now.wMilliseconds);
    std::wstring line=std::wstring(stamp)+message+L"\r\n";
    int size=WideCharToMultiByte(CP_UTF8,0,line.c_str(),int(line.size()),nullptr,0,nullptr,nullptr);
    if(size<=0)return;
    std::string utf8(size,'\0');
    WideCharToMultiByte(CP_UTF8,0,line.c_str(),int(line.size()),utf8.data(),size,nullptr,nullptr);
    HANDLE output=CreateFileW(path.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,
                              nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(output!=INVALID_HANDLE_VALUE){
        DWORD written=0;WriteFile(output,utf8.data(),DWORD(utf8.size()),&written,nullptr);
        CloseHandle(output);
    }
}
static LONG WINAPI unhandledCrash(EXCEPTION_POINTERS* exception){
    auto record=exception?exception->ExceptionRecord:nullptr;
    wchar_t code[150]{};
    swprintf_s(code,L"Uncaught Windows exception 0x%08X at %p",
               record?record->ExceptionCode:0,record?record->ExceptionAddress:nullptr);
    startupLog(code);
    if(record&&record->ExceptionAddress){
        HMODULE module{};
        if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(record->ExceptionAddress),&module)){
            wchar_t path[MAX_PATH]{};
            if(GetModuleFileNameW(module,path,MAX_PATH))
                startupLog(L"Crashing module: "+std::wstring(path));
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

using Microsoft::WRL::ComPtr;
static void check(HRESULT code,const char* label) {
    if(FAILED(code))throw std::runtime_error(std::string(label)+" HRESULT "+std::to_string((unsigned)code));
}
static void checkSL(sl::Result code,const char* label) {
    if(code!=sl::Result::eOk)throw std::runtime_error(std::string(label)+" Streamline "+std::to_string((int)code));
}
static std::wstring widen(const std::string& value) {
    int count=MultiByteToWideChar(CP_UTF8,0,value.c_str(),-1,nullptr,0);
    if(count<=0)return L"Unknown error";
    std::wstring result(count,L'\0');
    MultiByteToWideChar(CP_UTF8,0,value.c_str(),-1,result.data(),count);
    result.pop_back();
    return result;
}
static void waitFence(ID3D12Fence* fence,UINT64 number) {
    if(!number||!fence||fence->GetCompletedValue()>=number)return;
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if(!event)throw std::runtime_error("Unable to create fence event");
    check(fence->SetEventOnCompletion(number,event),"SetEventOnCompletion");
    DWORD result=WaitForSingleObject(event,5000);CloseHandle(event);
    if(result!=WAIT_OBJECT_0)throw std::runtime_error("GPU wait timed out");
}
static void barrier(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,
                    D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
    if(before==after)return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource=resource;b.Transition.StateBefore=before;b.Transition.StateAfter=after;
    b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1,&b);
}
static void uavBarrier(ID3D12GraphicsCommandList* list,ID3D12Resource* resource) {
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;b.UAV.pResource=resource;
    list->ResourceBarrier(1,&b);
}
static ComPtr<ID3D12Resource> gpuTexture(ID3D12Device* dev,UINT w,UINT h,DXGI_FORMAT format) {
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width=w;d.Height=h;d.DepthOrArraySize=1;d.MipLevels=1;d.Format=format;
    d.SampleDesc.Count=1;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> output;
    check(dev->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&output)),"Create motion texture");
    return output;
}
static ComPtr<ID3DBlob> shader(const std::wstring& file,const char* entry,const char* profile) {
    ComPtr<ID3DBlob> binary,errors;
    auto hr=D3DCompileFromFile(file.c_str(),nullptr,D3D_COMPILE_STANDARD_FILE_INCLUDE,entry,profile,
                               D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&binary,&errors);
    if(FAILED(hr)) {
        std::string error="Compiling HLSL "+std::string(entry);
        if(errors)error+=": "+std::string((char*)errors->GetBufferPointer(),errors->GetBufferSize());
        throw std::runtime_error(error);
    }
    return binary;
}
struct Settings {
    HWND game{};DWORD pid{};int factor{4};int analysisPercent{25};
    float mouseGain{0.3f};float fovX{90.f};
};
static std::atomic<bool> quitting{false};
static std::mutex statusMutex;
static std::wstring lastStatus=L"Ready";
static void setStatus(const std::wstring& message) {
    std::lock_guard<std::mutex> lock(statusMutex);
    lastStatus=message;
}
static std::wstring getStatus() {
    std::lock_guard<std::mutex> lock(statusMutex);return lastStatus;
}

// Frame generation uses official Streamline interfaces; the model/preset is
// driver-controlled. This does not assume or silently claim Preset C.
class StreamlineFG {
    HMODULE library{};
    bool started{},enabled{};
    sl::FrameToken* frameToken{};
    uint32_t frameNumber{};
    sl::ViewportHandle viewport{0};
    sl::Constants constants{};
    sl::DLSSGOptions activeOptions{};
    sl::Extent region{};
    PFun_slInit* slInitFn{};
    PFun_slShutdown* slShutdownFn{};
    PFun_slSetD3DDevice* setDevice{};
    PFun_slGetFeatureFunction* getFunction{};
    PFun_slIsFeatureSupported* isSupported{};
    PFun_slGetNewFrameToken* newToken{};
    PFun_slSetConstants* setConstants{};
    PFun_slSetTagForFrame* setTag{};
    PFun_slDLSSGSetOptions* setOptions{};
    PFun_slDLSSGGetState* getState{};
    PFun_slReflexSetOptions* setReflex{};
    PFun_slPCLSetMarker* setMarker{};
    template<typename T>void load(T& pointer,const char* name) {
        pointer=reinterpret_cast<T>(GetProcAddress(library,name));
        if(!pointer)throw std::runtime_error(std::string("Missing Streamline export ")+name);
    }
    template<typename T>void extension(T& pointer,sl::Feature feature,const char* name) {
        void* out=nullptr;checkSL(getFunction(feature,name,out),name);
        pointer=reinterpret_cast<T>(out);
        if(!pointer)throw std::runtime_error(std::string("Missing Streamline feature ")+name);
    }
public:
    using DXGIFactoryFn=HRESULT(WINAPI*)(REFIID,void**);
    using D3D12CreateFn=HRESULT(WINAPI*)(IUnknown*,D3D_FEATURE_LEVEL,REFIID,void**);
    DXGIFactoryFn makeFactory{};
    D3D12CreateFn makeDevice{};
    void initialize() {
        wchar_t path[MAX_PATH]{};GetModuleFileNameW(nullptr,path,MAX_PATH);
        std::wstring directory=std::filesystem::path(path).parent_path().wstring();
        library=LoadLibraryW((directory+L"\\sl.interposer.dll").c_str());
        if(!library)throw std::runtime_error("sl.interposer.dll is missing");
        load(slInitFn,"slInit");load(slShutdownFn,"slShutdown");
        load(setDevice,"slSetD3DDevice");load(getFunction,"slGetFeatureFunction");
        load(isSupported,"slIsFeatureSupported");load(newToken,"slGetNewFrameToken");
        load(setConstants,"slSetConstants");load(setTag,"slSetTagForFrame");
        load(makeFactory,"CreateDXGIFactory1");load(makeDevice,"D3D12CreateDevice");
        const wchar_t* searchPaths[]={directory.c_str()};
        sl::Feature features[]={sl::kFeatureDLSS_G,sl::kFeatureReflex,sl::kFeaturePCL};
        sl::Preferences prefs{};prefs.pathsToPlugins=searchPaths;prefs.numPathsToPlugins=1;
        prefs.featuresToLoad=features;prefs.numFeaturesToLoad=3;
        prefs.pathToLogsAndData=directory.c_str();
        prefs.logLevel=sl::LogLevel::eDefault;
        prefs.flags=sl::PreferenceFlags::eUseDXGIFactoryProxy|sl::PreferenceFlags::eUseFrameBasedResourceTagging;
        prefs.engine=sl::EngineType::eCustom;prefs.engineVersion="FreshMotionFG/0.1";
        prefs.projectId="dfa52080-4946-4e60-af6a-d1eab95c13b1";
        prefs.renderAPI=sl::RenderAPI::eD3D12;
        checkSL(slInitFn(prefs,sl::kSDKVersion),"Initialize Streamline");
        started=true;
    }
    void bind(ID3D12Device* dev){checkSL(setDevice(dev),"Bind D3D12 device");}
    void configure(IDXGIAdapter1* adapter,UINT width,UINT height,int multiplier){
        enabled=multiplier>1;region={0,0,width,height};
        if(!enabled)return;
        DXGI_ADAPTER_DESC1 desc{};check(adapter->GetDesc1(&desc),"Adapter descriptor");
        sl::AdapterInfo adapterInfo{};
        adapterInfo.deviceLUID=reinterpret_cast<uint8_t*>(&desc.AdapterLuid);
        adapterInfo.deviceLUIDSizeInBytes=sizeof(LUID);
        checkSL(isSupported(sl::kFeatureDLSS_G,adapterInfo),"DLSS-G GPU support");
        extension(setOptions,sl::kFeatureDLSS_G,"slDLSSGSetOptions");
        extension(getState,sl::kFeatureDLSS_G,"slDLSSGGetState");
        extension(setReflex,sl::kFeatureReflex,"slReflexSetOptions");
        extension(setMarker,sl::kFeaturePCL,"slPCLSetMarker");
        sl::DLSSGOptions options{};
        options.mode=sl::DLSSGMode::eOff;
        options.numBackBuffers=2;
        options.numFramesToGenerate=multiplier-1;
        options.colorWidth=width;options.colorHeight=height;
        options.colorBufferFormat=DXGI_FORMAT_B8G8R8A8_UNORM;
        options.mvecDepthWidth=width;options.mvecDepthHeight=height;
        options.mvecBufferFormat=DXGI_FORMAT_R16G16_FLOAT;
        options.depthBufferFormat=DXGI_FORMAT_R32_FLOAT;
        options.enableUserInterfaceRecomposition=sl::Boolean::eFalse;
        sl::DLSSGState state{};
        checkSL(getState(viewport,state,&options),"Query supported FG multiplier");
        if((unsigned)(multiplier-1)>state.numFramesToGenerateMax)
            throw std::runtime_error("Requested FG multiplier is not supported by driver");
        sl::ReflexOptions reflex{};reflex.mode=sl::ReflexMode::eLowLatencyWithBoost;
        checkSL(setReflex(reflex),"Enable Reflex");
        options.mode=sl::DLSSGMode::eOn;
        checkSL(setOptions(viewport,options),"Enable DLSS-G");
        activeOptions=options;
        sl::float4x4 identity{};
        identity[0]={1,0,0,0};identity[1]={0,1,0,0};
        identity[2]={0,0,1,0};identity[3]={0,0,0,1};
        constants.cameraViewToClip=identity;
        constants.clipToCameraView=identity;
        constants.clipToPrevClip=identity;
        constants.prevClipToClip=identity;
        constants.clipToLensClip=identity;
        constants.cameraUp={0,1,0};constants.cameraRight={1,0,0};
        constants.cameraFwd={0,0,1};
        // Every Streamline common constant must be initialized explicitly.
        // The previous build left these invalid; the supplied startup log
        // reported all four immediately before the application exited.
        constants.jitterOffset={0,0};
        constants.cameraPinholeOffset={0,0};
        constants.cameraPos={0,0,0};
        constants.depthInverted=sl::eFalse;
        constants.cameraNear=.1f;constants.cameraFar=1000;
        constants.cameraFOV=1.5707963f;
        constants.cameraAspectRatio=float(width)/height;
        constants.mvecScale={1.f/width,1.f/height};
        constants.cameraMotionIncluded=sl::eTrue;
        constants.motionVectors3D=sl::eFalse;
        constants.orthographicProjection=sl::eTrue;
        constants.motionVectorsInvalidValue=65504;
    }
    void pause(bool paused){
        if(!enabled)return;
        auto options=activeOptions;
        options.mode=paused?sl::DLSSGMode::eOff:sl::DLSSGMode::eOn;
        checkSL(setOptions(viewport,options),paused?"Pause DLSS-G":"Resume DLSS-G");
    }
    void marker(sl::PCLMarker value) {
        if(enabled)checkSL(setMarker(value,*frameToken),"PCL marker");
    }
    void begin(bool reset) {
        if(!enabled)return;
        checkSL(newToken(frameToken,&frameNumber),"Frame token");++frameNumber;
        resetFrame(reset);
        marker(sl::PCLMarker::eSimulationStart);
        marker(sl::PCLMarker::eSimulationEnd);
        marker(sl::PCLMarker::eRenderSubmitStart);
    }
    void resetFrame(bool reset){
        if(!enabled)return;
        constants.reset=reset?sl::eTrue:sl::eFalse;
        checkSL(setConstants(constants,*frameToken,viewport),"FG frame constants");
    }
    void tag(ID3D12GraphicsCommandList* cmd,ID3D12Resource* motion,
             ID3D12Resource* depth,ID3D12Resource* color,UINT width,UINT height) {
        if(!enabled)return;
        sl::Resource mv(sl::ResourceType::eTex2d,motion,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        sl::Resource d(sl::ResourceType::eTex2d,depth,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        sl::Resource b(sl::ResourceType::eTex2d,color,D3D12_RESOURCE_STATE_PRESENT);
        mv.width=d.width=b.width=width;mv.height=d.height=b.height=height;
        mv.nativeFormat=DXGI_FORMAT_R16G16_FLOAT;d.nativeFormat=DXGI_FORMAT_R32_FLOAT;
        b.nativeFormat=DXGI_FORMAT_B8G8R8A8_UNORM;
        sl::ResourceTag resources[]={
            {&mv,sl::kBufferTypeMotionVectors,sl::eValidUntilPresent,&region},
            {&d,sl::kBufferTypeDepth,sl::eValidUntilPresent,&region},
            {&b,sl::kBufferTypeBackbuffer,sl::eValidUntilPresent,&region}
        };
        checkSL(setTag(*frameToken,viewport,resources,3,cmd),"Tag FG resources");
    }
    std::pair<ComPtr<ID3D12Fence>,UINT64> consumed() {
        if(!enabled)return {};
        sl::DLSSGState state{};
        checkSL(getState(viewport,state,nullptr),"Frame state");
        std::pair<ComPtr<ID3D12Fence>,UINT64> result;
        if(state.inputsProcessingCompletionFence&&state.lastPresentInputsProcessingCompletionFenceValue){
            result.first=static_cast<ID3D12Fence*>(state.inputsProcessingCompletionFence);
            result.second=state.lastPresentInputsProcessingCompletionFenceValue;
        }
        return result;
    }
    void close(){
        if(started){slShutdownFn();started=false;}
        if(library){FreeLibrary(library);library=nullptr;}
    }
    ~StreamlineFG(){close();}
};

struct Display {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> output;
    RECT rect{};
    UINT width{},height{};
    void find(HWND game) {
        HMONITOR monitor=MonitorFromWindow(game,MONITOR_DEFAULTTONEAREST);
        ComPtr<IDXGIFactory1> factory;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DXGI factory");
        for(UINT a=0;!adapter;a++) {
            ComPtr<IDXGIAdapter1> candidate;
            HRESULT result=factory->EnumAdapters1(a,&candidate);
            if(result==DXGI_ERROR_NOT_FOUND)break;
            check(result,"EnumAdapters1");
            for(UINT n=0;;n++) {
                ComPtr<IDXGIOutput> out;
                if(candidate->EnumOutputs(n,&out)==DXGI_ERROR_NOT_FOUND)break;
                DXGI_OUTPUT_DESC desc{};
                check(out->GetDesc(&desc),"Output desc");
                if(desc.Monitor==monitor) {
                    adapter=candidate;output=out;rect=desc.DesktopCoordinates;break;
                }
            }
        }
        if(!adapter||!output)throw std::runtime_error("The selected game monitor was not found");
        width=rect.right-rect.left;height=rect.bottom-rect.top;
        if(!width||!height)throw std::runtime_error("The game monitor has invalid dimensions");
        DXGI_ADAPTER_DESC1 desc{};check(adapter->GetDesc1(&desc),"Adapter desc");
        if(desc.VendorId!=0x10de)throw std::runtime_error("Game monitor must use an NVIDIA adapter");
    }
};

// NvFBC PID -> CUDA shared raw buffers -> dedicated DX12 copy queue -> color
// mailbox. Matches the *architecture* described by V39's build validation;
// its compiled binary was not available as modifiable source.
class Capture {
    NvFbcCaptureBackend backend;
    ComPtr<ID3D12Fence> cudaReady,copyReady;
    ComPtr<ID3D12CommandQueue> copyQueue;
    std::array<ComPtr<ID3D12Resource>,4> raw;
    std::array<ComPtr<ID3D12Resource>,3> color;
    std::array<ComPtr<ID3D12CommandAllocator>,3> allocators;
    std::array<ComPtr<ID3D12GraphicsCommandList>,3> copyCommands;
    std::array<UINT64,3> slotCopyDone{};
    UINT64 cudaSerial{},copySerial{};
    UINT width{},height{};
    [[noreturn]] static void fail(const std::wstring& operation,const std::wstring& error){
        throw std::runtime_error(std::string(operation.begin(),operation.end())+": "+
                                 std::string(error.begin(),error.end()));
    }
public:
    void preflight(Display& display,HWND overlay,DWORD pid) {
        startupLog(L"Starting NvFBC PID CUDA preflight");
        std::wstring error;
        if(!backend.Init(overlay,pid,MonitorFromWindow(overlay,MONITOR_DEFAULTTONEAREST),
                         display.rect,display.rect,display.width,display.height,error))
            fail(L"NvFBC CUDA PID initialization",error);
        width=backend.Width();height=backend.Height();
        if(!width||!height)throw std::runtime_error("NvFBC returned zero capture dimensions");
        startupLog(L"NvFBC PID preflight passed: "+std::to_wstring(width)+L"x"+std::to_wstring(height));
    }
    void bind(ID3D12Device* d3d12){
        startupLog(L"Binding NvFBC CUDA to the D3D12 shared buffer");
        if(backend.Pitch()<width*4 || backend.Pitch()%D3D12_TEXTURE_DATA_PITCH_ALIGNMENT)
            throw std::runtime_error("NvFBC row pitch cannot use a D3D12 copy footprint");
        check(d3d12->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&cudaReady)),
              "Create NvFBC CUDA ready fence");
        check(d3d12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&copyReady)),
              "Create color-mailbox copy ready fence");
        ID3D12Resource* resources[4]{};
        std::wstring error;
        if(!backend.Bind(d3d12,cudaReady.Get(),resources,error))fail(L"NvFBC CUDA-D3D12 bind",error);
        for(UINT i=0;i<4;i++)raw[i].Attach(resources[i]);
        D3D12_COMMAND_QUEUE_DESC cq{};cq.Type=D3D12_COMMAND_LIST_TYPE_COPY;
        check(d3d12->CreateCommandQueue(&cq,IID_PPV_ARGS(&copyQueue)),
              "Create dedicated NvFBC copy queue");
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width=width;d.Height=height;d.DepthOrArraySize=1;d.MipLevels=1;
        d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;d.SampleDesc.Count=1;
        d.Flags=D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        for(UINT i=0;i<color.size();i++) {
            check(d3d12->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,
                  D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&color[i])),
                  "Allocate V39-style color mailbox");
            check(d3d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,
                  IID_PPV_ARGS(&allocators[i])),"Create mailbox copy allocator");
            check(d3d12->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_COPY,
                  allocators[i].Get(),nullptr,IID_PPV_ARGS(&copyCommands[i])),
                  "Create mailbox copy list");
            check(copyCommands[i]->Close(),"Close mailbox copy list");
        }
        startupLog(L"NvFBC copy queue and color mailbox ready");
    }
    NvFbcGrabResult next(UINT slot,UINT64& fenceValue) {
        LARGE_INTEGER timestamp{};std::wstring error;
        auto result=backend.Grab(slot,timestamp,error);
        if(result==NvFbcGrabResult::Timeout||result==NvFbcGrabResult::Recreate)
            return result;
        if(result!=NvFbcGrabResult::Frame)fail(L"NvFBC capture",error);
        if(backend.Width()!=width||backend.Height()!=height)
            throw std::runtime_error("Game capture resolution changed. Stop and restart.");
        UINT64 cudaValue=cudaSerial+1;
        if(!backend.Signal(cudaValue,error))fail(L"CUDA fence signal",error);
        cudaSerial=cudaValue;
        // All copy queue waits are on GPU; no CPU readback or bridge API.
        check(copyQueue->Wait(cudaReady.Get(),cudaValue),"Copy queue waits for CUDA frame");
        if(copyReady->GetCompletedValue()<slotCopyDone[slot])
            throw std::runtime_error("Attempted to overwrite in-flight color mailbox slot");
        check(allocators[slot]->Reset(),"Reset reusable mailbox copy allocator");
        check(copyCommands[slot]->Reset(allocators[slot].Get(),nullptr),
              "Reset reusable mailbox copy command list");
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource=raw[slot].Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
        src.PlacedFootprint.Footprint.Width=width;
        src.PlacedFootprint.Footprint.Height=height;
        src.PlacedFootprint.Footprint.Depth=1;
        src.PlacedFootprint.Footprint.RowPitch=backend.Pitch();
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource=color[slot].Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        copyCommands[slot]->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        check(copyCommands[slot]->Close(),"Close CUDA-to-color copy list");
        ID3D12CommandList* copy[]={copyCommands[slot].Get()};
        copyQueue->ExecuteCommandLists(1,copy);
        check(copyQueue->Signal(copyReady.Get(),++copySerial),"Signal color mailbox ready");
        slotCopyDone[slot]=copySerial;
        fenceValue=copySerial;
        return NvFbcGrabResult::Frame;
    }
    void recreate(){std::wstring error;if(!backend.RecreateSession(error))fail(L"NvFBC PID recreate",error);}
    ID3D12Resource* image(UINT slot)const{return color[slot].Get();}
    ID3D12Fence* readyFence()const{return copyReady.Get();}
    UINT Width()const{return width;}
    UINT Height()const{return height;}
    UINT Pitch()const{return backend.Pitch();}
};

struct alignas(4) MotionConstants {
    UINT imageWidth{},imageHeight{},analysisWidth{},analysisHeight{};
    UINT flowWidth{},flowHeight{},firstFrame{},outputWidth{};
    UINT outputHeight{};float mouseX{},mouseY{},cameraGain{};
    float tanHalfFovX{},confidenceCutoff{};UINT capturePitchBytes{};float pad1{};
};
static_assert(sizeof(MotionConstants)==16*sizeof(UINT));
struct RenderSlot {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Resource> forwardFlow,reverseFlow,motion,depth;
    ComPtr<ID3D12Fence> fgFence;
    UINT64 fgFenceValue{},finishedValue{};
};

class Presenter {
    Settings cfg;
    Display display;
    HWND overlay{};
    StreamlineFG dlss;
    Capture capture;
    ComPtr<IDXGIFactory4> factory;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain3> swap;
    ComPtr<ID3D12Fence> finished;
    std::array<RenderSlot,3> slots;
    ComPtr<ID3D12DescriptorHeap> heap,rtvs;
    ComPtr<ID3D12RootSignature> computeRoot,drawRoot;
    ComPtr<ID3D12PipelineState> forwardPSO,reversePSO,expandPSO,drawPSO;
    std::array<ComPtr<ID3D12Resource>,2> backBuffer;
    UINT descriptorSize{},rtvSize{},analysisW{},analysisH{},flowW{},flowH{};
    UINT64 fenceSerial{},frameCount{};
    std::wstring shaderPath;
    LatestMailbox<3> mailbox;
    std::thread captureThread;
    std::atomic<bool> stopCapture{false},pauseCapture{false},producerPaused{false},producerFailed{false};
    std::mutex producerMutex;
    std::condition_variable producerWake;
    std::string producerError;
    void captureLoop(){
        uint64_t sequence=0,epoch=1;
        bool wasPaused=false;
        try {
            while(!stopCapture.load(std::memory_order_acquire)&&
                  !quitting.load(std::memory_order_acquire)){
                if(pauseCapture.load(std::memory_order_acquire)){
                    producerPaused.store(true,std::memory_order_release);
                    wasPaused=true;
                    std::unique_lock<std::mutex> lock(producerMutex);
                    producerWake.wait_for(lock,std::chrono::milliseconds(12),[&]{
                        return stopCapture.load()||!pauseCapture.load()||quitting.load();
                    });
                    continue;
                }
                producerPaused.store(false,std::memory_order_release);
                if(wasPaused){++epoch;wasPaused=false;}
                UINT64 copyDone=capture.readyFence()->GetCompletedValue();
                UINT64 renderDone=finished->GetCompletedValue();
                if(copyDone==UINT64_MAX||renderDone==UINT64_MAX)
                    throw std::runtime_error("GPU device removed during capture");
                int candidate=mailbox.reserve([&](unsigned,const auto& state){
                    return copyDone>=state.ready&&renderDone>=state.release;
                });
                if(candidate<0){
                    std::unique_lock<std::mutex> lock(producerMutex);
                    producerWake.wait_for(lock,std::chrono::milliseconds(2));
                    continue;
                }
                UINT64 copyValue=0;
                auto result=capture.next(UINT(candidate),copyValue);
                if(result!=NvFbcGrabResult::Frame){
                    mailbox.cancel(candidate);
                    if(result==NvFbcGrabResult::Recreate){capture.recreate();++epoch;}
                    continue;
                }
                mailbox.publish(UINT(candidate),++sequence,copyValue,epoch,0);
                producerWake.notify_all();
            }
        }catch(const std::exception& ex){
            {
                std::lock_guard<std::mutex> lock(producerMutex);
                producerError=ex.what();
            }
            producerFailed.store(true,std::memory_order_release);
            producerWake.notify_all();
        }
        producerPaused.store(true,std::memory_order_release);
    }
    void throwProducerError(){
        if(producerFailed.load(std::memory_order_acquire)){
            std::lock_guard<std::mutex> lock(producerMutex);
            throw std::runtime_error("NvFBC producer: "+producerError);
        }
    }
    void changeFocus(bool focused){
        if(!focused){
            pauseCapture.store(true,std::memory_order_release);
            producerWake.notify_all();
            const auto deadline=GetTickCount64()+2500;
            while(!producerPaused.load(std::memory_order_acquire)){
                throwProducerError();
                if(GetTickCount64()>deadline)
                    throw std::runtime_error("Alt-Tab capture pause acknowledgement timed out");
                Sleep(2);
            }
            waitFence(finished.Get(),fenceSerial);
            dlss.pause(true);
            mailbox.clear();
            ShowWindow(overlay,SW_HIDE);
        }else{
            // First frame after Alt-Tab resets history and cannot interpolate
            // against a frame from the old visibility/capture epoch.
            mailbox.clear();
            dlss.pause(false);
            ShowWindow(overlay,SW_SHOWNOACTIVATE);
            pauseCapture.store(false,std::memory_order_release);
            producerWake.notify_all();
        }
    }
    UINT cpuOffset(UINT index)const{return index*15;}
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(UINT index) {
        auto handle=heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr+=SIZE_T(index)*descriptorSize;return handle;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(UINT index) {
        auto handle=heap->GetGPUDescriptorHandleForHeapStart();
        handle.ptr+=UINT64(index)*descriptorSize;return handle;
    }
    void srv(UINT index,ID3D12Resource* source,DXGI_FORMAT format) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        d.Format=format;d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Texture2D.MipLevels=1;
        device->CreateShaderResourceView(source,&d,cpu(index));
    }
    void rawSrv(UINT index,ID3D12Resource* source){
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        d.Format=DXGI_FORMAT_R32_TYPELESS;
        d.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
        d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Buffer.NumElements=UINT(source->GetDesc().Width/4);
        d.Buffer.Flags=D3D12_BUFFER_SRV_FLAG_RAW;
        device->CreateShaderResourceView(source,&d,cpu(index));
    }
    void uav(UINT index,ID3D12Resource* source,DXGI_FORMAT format) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
        d.Format=format;d.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(source,nullptr,&d,cpu(index));
    }
    void createOverlay() {
        // This is an opaque mirror presenter, not an alpha-composited overlay.
        // Use a real HWND flip swap chain so Streamline can intercept Present.
        // WS_EX_TRANSPARENT and HTTRANSPARENT keep the window click-through.
        overlay=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TRANSPARENT,
            L"FreshMotionFGOverlay",L"FreshMotionFG Output",WS_POPUP,display.rect.left,display.rect.top,
            display.width,display.height,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(!overlay)throw std::runtime_error("Create presenter window failed");
        ShowWindow(overlay,SW_SHOWNOACTIVATE);
        if(!SetWindowDisplayAffinity(overlay,WDA_EXCLUDEFROMCAPTURE))
            throw std::runtime_error("Windows could not exclude presenter from capture");
    }
    void createGraphics() {
        // A first NvFBC PID grab must succeed before Streamline is initialized.
        capture.preflight(display,overlay,cfg.pid);
        dlss.initialize();
        startupLog(L"Streamline initialization passed");
        check(dlss.makeFactory(IID_PPV_ARGS(&factory)),"Streamline DXGI factory");
        check(dlss.makeDevice(display.adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),
              "Streamline D3D12 device");
        dlss.bind(device.Get());
        D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        q.Priority=D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
        if(FAILED(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)))) {
            q.Priority=D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
            check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"Command queue");
        }
        check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&finished)),"GPU completion fence");
        capture.bind(device.Get());

        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width=display.width;d.Height=display.height;
        d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
        d.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;d.BufferCount=2;
        d.SampleDesc.Count=1;d.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
        d.Scaling=DXGI_SCALING_STRETCH;d.AlphaMode=DXGI_ALPHA_MODE_IGNORE;
        ComPtr<IDXGISwapChain1> s;
        check(factory->CreateSwapChainForHwnd(queue.Get(),overlay,&d,nullptr,nullptr,&s),
              "DX12 Streamline HWND flip swapchain");
        check(s.As(&swap),"Swapchain interface");
        check(factory->MakeWindowAssociation(overlay,DXGI_MWA_NO_ALT_ENTER),
              "Disable implicit DXGI fullscreen transitions");
        D3D12_DESCRIPTOR_HEAP_DESC h{};
        h.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;h.NumDescriptors=2;
        check(device->CreateDescriptorHeap(&h,IID_PPV_ARGS(&rtvs)),"RTV heap");
        rtvSize=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        for(UINT i=0;i<2;i++) {
            check(swap->GetBuffer(i,IID_PPV_ARGS(&backBuffer[i])),"Swapchain buffer");
            auto cpuRtv=rtvs->GetCPUDescriptorHandleForHeapStart();
            cpuRtv.ptr+=SIZE_T(i)*rtvSize;
            device->CreateRenderTargetView(backBuffer[i].Get(),nullptr,cpuRtv);
        }
        h={};h.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        h.NumDescriptors=UINT(slots.size())*15;
        h.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        check(device->CreateDescriptorHeap(&h,IID_PPV_ARGS(&heap)),"Shader descriptor heap");
        descriptorSize=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        dlss.configure(display.adapter.Get(),display.width,display.height,cfg.factor);
        startupLog(L"DX12 HWND flip presenter and Streamline DLSS-G configuration passed");
    }
    void createPipelines() {
        startupLog(L"Compiling shader-motion HLSL pipelines");
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors=4;ranges[0].BaseShaderRegister=0;
        ranges[1].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors=3;ranges[1].BaseShaderRegister=0;
        D3D12_ROOT_PARAMETER root[3]{};
        root[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        root[0].DescriptorTable={1,&ranges[0]};
        root[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        root[1].DescriptorTable={1,&ranges[1]};
        root[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        root[2].Constants={0,0,16};
        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters=3;desc.pParameters=root;
        ComPtr<ID3DBlob> blob;
        check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,
              &blob,nullptr),"Serialize compute root");
        check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),
              IID_PPV_ARGS(&computeRoot)),"Compute root signature");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature=computeRoot.Get();
        auto makeCompute=[&](const char* entry,ComPtr<ID3D12PipelineState>& out){
            auto code=shader(shaderPath,entry,"cs_5_1");
            pipeline.CS={code->GetBufferPointer(),code->GetBufferSize()};
            check(device->CreateComputePipelineState(&pipeline,IID_PPV_ARGS(&out)),entry);
        };
        makeCompute("EstimateForward",forwardPSO);
        makeCompute("EstimateReverse",reversePSO);
        makeCompute("ExpandMotion",expandPSO);

        D3D12_DESCRIPTOR_RANGE drawRange{};
        drawRange.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        drawRange.NumDescriptors=1;drawRange.BaseShaderRegister=0;
        D3D12_ROOT_PARAMETER drawParam[2]{};
        drawParam[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        drawParam[0].DescriptorTable={1,&drawRange};
        drawParam[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        drawParam[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        drawParam[1].Constants={0,0,16};
        drawParam[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        desc={};desc.NumParameters=2;desc.pParameters=drawParam;
        desc.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        blob.Reset();
        check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,
              &blob,nullptr),"Serialize draw root");
        check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),
              IID_PPV_ARGS(&drawRoot)),"Draw root signature");
        auto vs=shader(shaderPath,"VSMain","vs_5_0");
        auto ps=shader(shaderPath,"PSMain","ps_5_0");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC draw{};
        draw.pRootSignature=drawRoot.Get();
        draw.VS={vs->GetBufferPointer(),vs->GetBufferSize()};
        draw.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
        draw.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;
        draw.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
        draw.RasterizerState.DepthClipEnable=TRUE;
        draw.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        draw.DepthStencilState.DepthEnable=FALSE;draw.DepthStencilState.StencilEnable=FALSE;
        draw.SampleMask=UINT_MAX;
        draw.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        draw.NumRenderTargets=1;draw.RTVFormats[0]=DXGI_FORMAT_B8G8R8A8_UNORM;
        draw.SampleDesc.Count=1;
        check(device->CreateGraphicsPipelineState(&draw,IID_PPV_ARGS(&drawPSO)),"Draw PSO");
        startupLog(L"GPU shader-motion HLSL pipelines ready");
    }
    void prepareSlots() {
        analysisW=std::max<UINT>(16,(capture.Width()*cfg.analysisPercent+99)/100);
        analysisH=std::max<UINT>(16,(capture.Height()*cfg.analysisPercent+99)/100);
        flowW=(analysisW+3)/4;flowH=(analysisH+3)/4;
        for(UINT i=0;i<slots.size();i++){
            auto& s=slots[i];
            s.forwardFlow=gpuTexture(device.Get(),flowW,flowH,DXGI_FORMAT_R16G16B16A16_FLOAT);
            s.reverseFlow=gpuTexture(device.Get(),flowW,flowH,DXGI_FORMAT_R16G16B16A16_FLOAT);
            s.motion=gpuTexture(device.Get(),display.width,display.height,DXGI_FORMAT_R16G16_FLOAT);
            s.depth=gpuTexture(device.Get(),display.width,display.height,DXGI_FORMAT_R32_FLOAT);
            check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                  IID_PPV_ARGS(&s.allocator)),"Command allocator");
            check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,
                  s.allocator.Get(),nullptr,IID_PPV_ARGS(&s.commands)),"Command list");
            check(s.commands->Close(),"Close initial list");
        }
        startupLog(L"Motion and depth surfaces allocated");
    }
    void setDescriptors(UINT slot,UINT current,UINT previous) {
        UINT base=cpuOffset(slot);
        srv(base+0,capture.image(current),DXGI_FORMAT_B8G8R8A8_UNORM);
        srv(base+1,capture.image(previous),DXGI_FORMAT_B8G8R8A8_UNORM);
        srv(base+2,slots[slot].forwardFlow.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT);
        srv(base+3,slots[slot].reverseFlow.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT);
        uav(base+4,slots[slot].forwardFlow.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT);
        uav(base+5,slots[slot].motion.Get(),DXGI_FORMAT_R16G16_FLOAT);
        uav(base+6,slots[slot].depth.Get(),DXGI_FORMAT_R32_FLOAT);
        srv(base+7,capture.image(current),DXGI_FORMAT_B8G8R8A8_UNORM);
        srv(base+8,capture.image(previous),DXGI_FORMAT_B8G8R8A8_UNORM);
        srv(base+9,capture.image(current),DXGI_FORMAT_B8G8R8A8_UNORM);
        srv(base+10,slots[slot].forwardFlow.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT);
        srv(base+11,slots[slot].reverseFlow.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT);
        uav(base+12,slots[slot].reverseFlow.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT);
        uav(base+13,slots[slot].motion.Get(),DXGI_FORMAT_R16G16_FLOAT);
        uav(base+14,slots[slot].depth.Get(),DXGI_FORMAT_R32_FLOAT);
    }
public:
    explicit Presenter(Settings config):cfg(config){}
    ~Presenter(){
        stopCapture.store(true,std::memory_order_release);
        producerWake.notify_all();
        if(captureThread.joinable())captureThread.join();
        try{waitFence(finished.Get(),fenceSerial);}catch(...){}
        dlss.close();
        if(overlay)DestroyWindow(overlay);
    }
    void render(UINT current,UINT previous,UINT64 ready,bool first) {
        if(frameCount==0)startupLog(L"First render: waiting on CUDA copy fence and recording GPU commands");
        auto slotIndex=UINT(frameCount%slots.size());
        auto& s=slots[slotIndex];
        waitFence(finished.Get(),s.finishedValue);
        waitFence(s.fgFence.Get(),s.fgFenceValue);
        setDescriptors(slotIndex,current,previous);
        check(s.allocator->Reset(),"Reset allocator");
        check(s.commands->Reset(s.allocator.Get(),nullptr),"Reset commands");
        auto* list=s.commands.Get();
        check(queue->Wait(capture.readyFence(),ready),"D3D12 waits for capture");
        ID3D12DescriptorHeap* heaps[]={heap.Get()};
        list->SetDescriptorHeaps(1,heaps);
        constexpr auto readStates=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|
                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier(list,capture.image(current),D3D12_RESOURCE_STATE_COMMON,readStates);
        if(current!=previous)barrier(list,capture.image(previous),D3D12_RESOURCE_STATE_COMMON,readStates);
        if(s.finishedValue) {
            barrier(list,s.motion.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            barrier(list,s.depth.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        MotionConstants constants{};
        constants.imageWidth=capture.Width();constants.imageHeight=capture.Height();
        constants.analysisWidth=analysisW;constants.analysisHeight=analysisH;
        constants.flowWidth=flowW;constants.flowHeight=flowH;
        constants.firstFrame=first?1u:0u;
        constants.outputWidth=display.width;constants.outputHeight=display.height;
        constants.cameraGain=0;constants.mouseX=0;constants.mouseY=0; // no prediction
        constants.tanHalfFovX=std::tan(cfg.fovX*3.14159265f/360.f);
        constants.confidenceCutoff=.08f;
        constants.capturePitchBytes=capture.Pitch();

        list->SetComputeRootSignature(computeRoot.Get());
        list->SetComputeRoot32BitConstants(2,16,&constants,0);
        list->SetComputeRootDescriptorTable(0,gpu(cpuOffset(slotIndex)));
        list->SetComputeRootDescriptorTable(1,gpu(cpuOffset(slotIndex)+4));
        list->SetPipelineState(forwardPSO.Get());
        list->Dispatch((flowW+7)/8,(flowH+7)/8,1);
        uavBarrier(list,s.forwardFlow.Get());
        // Reverse pass uses the same source pair but swapped SRVs in descriptor tables.
        // The final confidence mask rejects flows that do not reverse consistently.
        list->SetComputeRootDescriptorTable(0,gpu(cpuOffset(slotIndex)+8));
        list->SetComputeRootDescriptorTable(1,gpu(cpuOffset(slotIndex)+12));
        list->SetPipelineState(reversePSO.Get());
        list->Dispatch((flowW+7)/8,(flowH+7)/8,1);
        uavBarrier(list,s.reverseFlow.Get());
        barrier(list,s.forwardFlow.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barrier(list,s.reverseFlow.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->SetComputeRootDescriptorTable(0,gpu(cpuOffset(slotIndex)));
        list->SetComputeRootDescriptorTable(1,gpu(cpuOffset(slotIndex)+4));
        list->SetPipelineState(expandPSO.Get());
        list->Dispatch((display.width+7)/8,(display.height+7)/8,1);
        uavBarrier(list,s.motion.Get());uavBarrier(list,s.depth.Get());
        barrier(list,s.motion.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barrier(list,s.depth.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barrier(list,s.forwardFlow.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barrier(list,s.reverseFlow.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        UINT bi=swap->GetCurrentBackBufferIndex();
        auto* back=backBuffer[bi].Get();
        barrier(list,back,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv=rtvs->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr+=SIZE_T(bi)*rtvSize;
        list->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
        D3D12_VIEWPORT vp{0,0,float(display.width),float(display.height),0,1};
        D3D12_RECT sc{0,0,LONG(display.width),LONG(display.height)};
        list->RSSetViewports(1,&vp);list->RSSetScissorRects(1,&sc);
        list->SetGraphicsRootSignature(drawRoot.Get());
        list->SetGraphicsRootDescriptorTable(0,gpu(cpuOffset(slotIndex)+7));
        list->SetGraphicsRoot32BitConstants(1,16,&constants,0);
        list->SetPipelineState(drawPSO.Get());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3,1,0,0);
        barrier(list,back,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);
        barrier(list,capture.image(current),readStates,D3D12_RESOURCE_STATE_COMMON);
        if(current!=previous)barrier(list,capture.image(previous),
                readStates,D3D12_RESOURCE_STATE_COMMON);
        dlss.tag(list,s.motion.Get(),s.depth.Get(),back,display.width,display.height);
        check(list->Close(),"Close render commands");
        ID3D12CommandList* commands[]={list};
        queue->ExecuteCommandLists(1,commands);
        if(frameCount==0)startupLog(L"First render: GPU command list submitted");
        check(queue->Signal(finished.Get(),++fenceSerial),"Render fence");
        s.finishedValue=fenceSerial;
        dlss.marker(sl::PCLMarker::eRenderSubmitEnd);
        dlss.marker(sl::PCLMarker::ePresentStart);
        if(frameCount==0)startupLog(L"First render: entering HWND Streamline Present");
        check(swap->Present(0,0),"Present");
        if(frameCount==0)startupLog(L"First render: Present returned successfully");
        dlss.marker(sl::PCLMarker::ePresentEnd);
        auto consumed=dlss.consumed();
        if(frameCount==0)startupLog(L"First render: DLSS-G state query passed");
        s.fgFence=consumed.first;s.fgFenceValue=consumed.second;
        ++frameCount;
    }
    void run() {
        startupLog(L"Starting FreshMotionFG V39-mailbox session");
        display.find(cfg.game);
        createOverlay();
        setStatus(L"Starting V39-style NvFBC mailbox + shader DLSS-G...");
        createGraphics();
        wchar_t module[MAX_PATH]{};GetModuleFileNameW(nullptr,module,MAX_PATH);
        shaderPath=(std::filesystem::path(module).parent_path()/L"motion.hlsl").wstring();
        createPipelines();prepareSlots();
        captureThread=std::thread([this]{captureLoop();});
        startupLog(L"Independent NvFBC capture producer started");
        setStatus(L"Ready. CUDA producer and latest-frame mailbox active.");
        int previous=-1;
        uint64_t previousEpoch=0;
        bool paused=false;
        while(!quitting.load(std::memory_order_acquire)){
            MSG msg{};
            while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){
                if(msg.message==WM_QUIT){quitting.store(true);break;}
                TranslateMessage(&msg);DispatchMessageW(&msg);
            }
            if(quitting)break;
            if(!IsWindow(cfg.game))throw std::runtime_error("Selected game window closed");
            throwProducerError();
            DWORD foregroundPID=0;
            GetWindowThreadProcessId(GetForegroundWindow(),&foregroundPID);
            if(foregroundPID!=cfg.pid) {
                if(!paused){
                    changeFocus(false);
                    paused=true;previous=-1;
                    setStatus(L"Paused on Alt-Tab. NvFBC capture and DLSS-G suspended.");
                }
                Sleep(8);continue;
            }
            if(paused){
                changeFocus(true);
                paused=false;previous=-1;
                setStatus(L"Resumed. Waiting for a fresh NvFBC PID frame.");
            }
            if(!mailbox.available()){
                std::unique_lock<std::mutex> lock(producerMutex);
                producerWake.wait_for(lock,std::chrono::milliseconds(4));
                continue;
            }
            // Recycle renderer/FG resources before selecting the newest source.
            // The NvFBC producer keeps acquiring frames during these waits.
            auto& slot=slots[frameCount%slots.size()];
            waitFence(finished.Get(),slot.finishedValue);
            waitFence(slot.fgFence.Get(),slot.fgFenceValue);
            LatestMailbox<3>::Slot selected{};
            int current=mailbox.take(&selected);
            if(current<0)continue;
            bool reset=previous<0||selected.epoch!=previousEpoch;
            if(previous<0)startupLog(L"First image selected from color mailbox");
            dlss.begin(reset);
            if(previous<0)startupLog(L"First Streamline frame token and constants accepted");
            // Last chance to latch a newer completed capture before rendering.
            if(mailbox.retakeLatest(current,&selected)){
                bool newerReset=previous<0||selected.epoch!=previousEpoch;
                if(newerReset!=reset)dlss.resetFrame(newerReset);
                reset=newerReset;
            }
            render(UINT(current),reset?UINT(current):UINT(previous),selected.ready,reset);
            mailbox.commit(UINT(current),previous,fenceSerial);
            producerWake.notify_all();
            previous=current;
            previousEpoch=selected.epoch;
            if(reset)setStatus(L"Generating frames. Latest NvFBC mailbox, no NvOFA.");
        }
        setStatus(L"Stopped");
    }
};

static HWND controller{},gamePicker{},factorPicker{},qualityPicker{},statusLabel{},gainEdit{};
static std::thread worker;
static std::vector<HWND> games;
static constexpr int ID_GAME=1101,ID_FG=1102,ID_QUALITY=1103,ID_START=1104,
                     ID_STOP=1105,ID_REFRESH=1106;
static BOOL CALLBACK enumerate(HWND h,LPARAM){
    if(!IsWindowVisible(h)||h==controller||GetWindow(h,GW_OWNER))return TRUE;
    DWORD pid{};
    GetWindowThreadProcessId(h,&pid);
    if(!pid||pid==GetCurrentProcessId())return TRUE;
    wchar_t title[256]{};
    if(GetWindowTextW(h,title,256)<2)return TRUE;
    int item=(int)SendMessageW(gamePicker,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(title));
    SendMessageW(gamePicker,CB_SETITEMDATA,item,reinterpret_cast<LPARAM>(h));
    games.push_back(h);
    return TRUE;
}
static void refreshGames(){
    games.clear();SendMessageW(gamePicker,CB_RESETCONTENT,0,0);
    EnumWindows(enumerate,0);
    if(!games.empty())SendMessageW(gamePicker,CB_SETCURSEL,0,0);
}
static void stopWorker(){
    quitting.store(true,std::memory_order_release);
    if(worker.joinable())worker.join();
}
static void startWorker(){
    stopWorker();
    int index=(int)SendMessageW(gamePicker,CB_GETCURSEL,0,0);
    if(index<0)throw std::runtime_error("Select a game window");
    auto game=reinterpret_cast<HWND>(SendMessageW(gamePicker,CB_GETITEMDATA,index,0));
    if(!IsWindow(game))throw std::runtime_error("Game window is no longer available");
    Settings settings{};
    settings.game=game;
    GetWindowThreadProcessId(game,&settings.pid);
    int factorIndex=int(SendMessageW(factorPicker,CB_GETCURSEL,0,0));
    settings.factor=std::clamp(factorIndex+1,1,6);
    int qualityIndex=int(SendMessageW(qualityPicker,CB_GETCURSEL,0,0));
    constexpr int percentages[]{50,25,10};
    settings.analysisPercent=percentages[std::clamp(qualityIndex,0,2)];
    quitting.store(false,std::memory_order_release);
    setStatus(L"Starting...");
    if(IsIconic(game))ShowWindow(game,SW_RESTORE);
    SetForegroundWindow(game);
    worker=std::thread([settings](){
        try{
            Presenter presenter(settings);
            presenter.run();
        }catch(const std::exception& ex){
            auto message=widen(ex.what());
            startupLog(L"FATAL: "+message);
            setStatus(L"ERROR: "+message);
            MessageBoxW(controller,message.c_str(),L"FreshMotionFG error",MB_OK|MB_ICONERROR);
        }
    });
}
static HWND text(HWND parent,const wchar_t* label,int x,int y,int w){
    return CreateWindowExW(0,L"STATIC",label,WS_CHILD|WS_VISIBLE,
                           x,y,w,22,parent,nullptr,GetModuleHandleW(nullptr),nullptr);
}
static HWND combo(HWND parent,int id,int x,int y,int w){
    return CreateWindowExW(0,L"COMBOBOX",nullptr,WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|
                           WS_VSCROLL,x,y,w,220,parent,(HMENU)(INT_PTR)id,
                           GetModuleHandleW(nullptr),nullptr);
}
static LRESULT CALLBACK overlayProc(HWND h,UINT msg,WPARAM w,LPARAM l){
    switch(msg) {
        case WM_MOUSEACTIVATE:return MA_NOACTIVATE;
        case WM_NCHITTEST:return HTTRANSPARENT;
        case WM_ERASEBKGND:return 1;
        default:return DefWindowProcW(h,msg,w,l);
    }
}
static LRESULT CALLBACK controllerProc(HWND h,UINT msg,WPARAM w,LPARAM l){
    switch(msg) {
        case WM_CREATE:{
            controller=h;
            text(h,L"Game window (NvFBC CUDA PID):",18,14,350);
            gamePicker=combo(h,ID_GAME,18,40,375);
            CreateWindowExW(0,L"BUTTON",L"Refresh",WS_CHILD|WS_VISIBLE,399,40,80,28,
                            h,(HMENU)(INT_PTR)ID_REFRESH,GetModuleHandleW(nullptr),nullptr);
            text(h,L"DLSS Frame Generation:",18,95,210);
            factorPicker=combo(h,ID_FG,18,121,190);
            const wchar_t* factors[]={L"Off",L"2x",L"3x",L"4x",L"5x",L"6x"};
            for(auto value:factors)SendMessageW(factorPicker,CB_ADDSTRING,0,(LPARAM)value);
            SendMessageW(factorPicker,CB_SETCURSEL,3,0);
            text(h,L"GPU motion analysis:",228,95,240);
            qualityPicker=combo(h,ID_QUALITY,228,121,251);
            const wchar_t* qualities[]={L"50% - quality",L"25% - balanced",L"10% - fastest"};
            for(auto value:qualities)SendMessageW(qualityPicker,CB_ADDSTRING,0,(LPARAM)value);
            SendMessageW(qualityPicker,CB_SETCURSEL,1,0);
            CreateWindowExW(0,L"BUTTON",L"Start",WS_CHILD|WS_VISIBLE,
                            18,174,150,31,h,(HMENU)(INT_PTR)ID_START,
                            GetModuleHandleW(nullptr),nullptr);
            CreateWindowExW(0,L"BUTTON",L"Stop",WS_CHILD|WS_VISIBLE,
                            185,174,150,31,h,(HMENU)(INT_PTR)ID_STOP,
                            GetModuleHandleW(nullptr),nullptr);
            statusLabel=text(h,L"Ready",18,225,460);
            text(h,L"Bidirectional GPU flow | No NvOFA | No mouse prediction",18,265,470);
            refreshGames();SetTimer(h,1,400,nullptr);
            return 0;
        }
        case WM_TIMER:SetWindowTextW(statusLabel,getStatus().c_str());return 0;
        case WM_COMMAND:{
            int id=LOWORD(w);
            if(id==ID_START){
                try{startWorker();}
                catch(const std::exception& e){MessageBoxW(h,widen(e.what()).c_str(),L"Start failed",MB_OK|MB_ICONERROR);}
            }else if(id==ID_STOP){stopWorker();setStatus(L"Stopped");}
            else if(id==ID_REFRESH)refreshGames();
            return 0;
        }
        case WM_CLOSE:DestroyWindow(h);return 0;
        case WM_DESTROY:
            KillTimer(h,1);stopWorker();PostQuitMessage(0);return 0;
        default:return DefWindowProcW(h,msg,w,l);
    }
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int show){
    SetUnhandledExceptionFilter(unhandledCrash);
    std::set_terminate([]{startupLog(L"FATAL: std::terminate called");std::abort();});
    startupLog(L"FreshMotionFG application process launched");
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    WNDCLASSEXW desc{sizeof(desc)};
    desc.hInstance=instance;desc.lpszClassName=L"FreshMotionFGController";
    desc.lpfnWndProc=controllerProc;
    desc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    desc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
    RegisterClassExW(&desc);
    desc.lpszClassName=L"FreshMotionFGOverlay";
    desc.lpfnWndProc=overlayProc;
    desc.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassExW(&desc);
    HWND window=CreateWindowExW(0,L"FreshMotionFGController",L"Fresh Motion + DLSS-G (experimental)",
         WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
         CW_USEDEFAULT,CW_USEDEFAULT,500,350,nullptr,nullptr,instance,nullptr);
    if(!window)return 1;
    ShowWindow(window,show);UpdateWindow(window);
    MSG msg{};
    while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
    return 0;
}
