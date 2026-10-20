#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include "nvsdk_ngx.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace wg = winrt::Windows::Graphics;
namespace wc = winrt::Windows::Graphics::Capture;
namespace wd = winrt::Windows::Graphics::DirectX;
namespace w11 = winrt::Windows::Graphics::DirectX::Direct3D11;
static constexpr int IDC_GAMES=1001,IDC_REFRESH=1002,IDC_START=1003,IDC_STOP=1004,IDC_FOV=1005,IDC_GAIN=1006,IDC_VSYNC=1007,IDC_WARP=1008,IDC_STATUS=1009,IDC_CURSOR=1010;
static constexpr UINT WM_STATUS=WM_APP+80;
static constexpr wchar_t MAIN_CLASS[]=L"FWCustom.Control", OVERLAY_CLASS[]=L"FWCustom.TransparentOutput", INPUT_CLASS[]=L"FWCustom.Input";
static std::atomic<bool> g_run{false};
static std::atomic<bool> g_presenting{false};
static std::atomic<HWND> g_target{nullptr};
static HWND g_main=nullptr, g_overlay=nullptr, g_combo=nullptr, g_start=nullptr, g_stop=nullptr, g_status=nullptr;
static std::vector<HWND> g_windows;
static std::thread g_worker;
static std::wstring g_message=L"Select your game/window and press Start.";
static std::mutex g_messageLock;
static std::mutex g_mouseLock;
struct MouseSample {long long timestamp; long dx,dy;};
static std::deque<MouseSample> g_mouse;
static LARGE_INTEGER g_frequency{};
struct Settings { HWND target{}; int fov=60; double degreesPerCount=0.028; bool vsync=false; bool wantWarp=true; bool cursor=false; };
static Settings g_opts;
static void Status(const std::wstring& s){std::lock_guard<std::mutex> lock(g_messageLock);g_message=s; if(g_main) PostMessageW(g_main,WM_STATUS,0,0);}
static void Verify(HRESULT hr,const char* msg){if(FAILED(hr))throw std::runtime_error(std::string(msg)+" HRESULT "+std::to_string((unsigned)hr));}
static std::wstring Widen(const std::string& s){return std::wstring(s.begin(),s.end());}
static std::wstring Folder(){wchar_t path[MAX_PATH]{};GetModuleFileNameW(nullptr,path,MAX_PATH);return std::filesystem::path(path).parent_path().wstring();}
static std::wstring WindowTitle(HWND h){wchar_t title[512]{};GetWindowTextW(h,title,512);return title;}
static BOOL CALLBACK EnumGames(HWND h,LPARAM){
 if(!IsWindowVisible(h) || !GetWindowTextLengthW(h) || IsIconic(h) || h==g_main || h==g_overlay)return TRUE;
 if(GetWindowLongPtrW(h,GWL_EXSTYLE)&WS_EX_TOOLWINDOW)return TRUE;
 DWORD pid=0;GetWindowThreadProcessId(h,&pid);if(pid==GetCurrentProcessId())return TRUE;
 DWORD cloaked=0; if(SUCCEEDED(DwmGetWindowAttribute(h,DWMWA_CLOAKED,&cloaked,sizeof(cloaked))) && cloaked)return TRUE;
 RECT r{};GetClientRect(h,&r);if(r.right<320||r.bottom<200)return TRUE;
 g_windows.push_back(h);return TRUE;
}
static void RefreshGames(){
 g_windows.clear();SendMessageW(g_combo,CB_RESETCONTENT,0,0);EnumWindows(EnumGames,0);
 int index=0,prefer=0;
 for(HWND h:g_windows){auto title=WindowTitle(h);auto name=title+L"  ["+std::to_wstring((unsigned long long)(uintptr_t)h)+L"]";SendMessageW(g_combo,CB_ADDSTRING,0,(LPARAM)name.c_str());if(h==g_opts.target)prefer=index;index++;}
 if(!g_windows.empty())SendMessageW(g_combo,CB_SETCURSEL,prefer,0);
}
static void MouseDiff(long long begin,long long end,long& dx,long& dy){
 dx=dy=0;std::lock_guard<std::mutex> lock(g_mouseLock);
 for(const auto& s:g_mouse){if(s.timestamp>begin && s.timestamp<=end){dx+=s.dx;dy+=s.dy;}}
}
static LRESULT CALLBACK OverlayProc(HWND h,UINT m,WPARAM w,LPARAM l){
 if(m==WM_NCHITTEST)return HTTRANSPARENT;
 if(m==WM_MOUSEACTIVATE)return MA_NOACTIVATE;
 return DefWindowProcW(h,m,w,l);
}
static void CaptureWorker(Settings opts);
static LRESULT CALLBACK ControlProc(HWND h,UINT m,WPARAM w,LPARAM l){
 switch(m){
  case WM_INPUT:{
   UINT bytes=0;GetRawInputData((HRAWINPUT)l,RID_INPUT,nullptr,&bytes,sizeof(RAWINPUTHEADER));
   if(!bytes || bytes>4096)break;
   std::vector<BYTE> buf(bytes);
   if(GetRawInputData((HRAWINPUT)l,RID_INPUT,buf.data(),&bytes,sizeof(RAWINPUTHEADER))!=bytes)break;
   const RAWINPUT* ri=(const RAWINPUT*)buf.data();
   if(ri->header.dwType==RIM_TYPEMOUSE && !(ri->data.mouse.usFlags&MOUSE_MOVE_ABSOLUTE)){
     LARGE_INTEGER t{};QueryPerformanceCounter(&t);
     std::lock_guard<std::mutex> lock(g_mouseLock);
     g_mouse.push_back({t.QuadPart,ri->data.mouse.lLastX,ri->data.mouse.lLastY});
     while(g_mouse.size()>10000)g_mouse.pop_front();
     while(!g_mouse.empty() && t.QuadPart-g_mouse.front().timestamp>g_frequency.QuadPart*2)g_mouse.pop_front();
   }
   break;
  }
  case WM_STATUS:{
   std::lock_guard<std::mutex> lock(g_messageLock);SetWindowTextW(g_status,g_message.c_str());return 0;
  }
  case WM_COMMAND:{
   int id=LOWORD(w);
   if(id==IDC_REFRESH){RefreshGames();return 0;}
   if(id==IDC_START && !g_run.load()){
      int i=(int)SendMessageW(g_combo,CB_GETCURSEL,0,0);
      if(i<0 || i>=(int)g_windows.size()){Status(L"Choose a window first.");return 0;}
      Settings s{};s.target=g_windows[i];
      wchar_t fov[64]{},gain[64]{};
      GetWindowTextW(GetDlgItem(h,IDC_FOV),fov,64);
      GetWindowTextW(GetDlgItem(h,IDC_GAIN),gain,64);
      s.fov=std::clamp(_wtoi(fov),30,130);
      s.degreesPerCount=std::clamp(_wtof(gain),0.001,2.0);
      s.vsync=SendDlgItemMessageW(h,IDC_VSYNC,BM_GETCHECK,0,0)==BST_CHECKED;
      s.wantWarp=SendDlgItemMessageW(h,IDC_WARP,BM_GETCHECK,0,0)==BST_CHECKED;
      s.cursor=SendDlgItemMessageW(h,IDC_CURSOR,BM_GETCHECK,0,0)==BST_CHECKED;
      if(g_worker.joinable())g_worker.join();
      g_opts=s;g_target=s.target;g_run=true;
      EnableWindow(g_start,FALSE);EnableWindow(g_stop,TRUE);
      g_worker=std::thread(CaptureWorker,s);
      return 0;
   }
   if(id==IDC_STOP){g_run=false;EnableWindow(g_stop,FALSE);Status(L"Stopping capture...");return 0;}
   break;
  }
  case WM_TIMER:{
     if(w==1 && !g_run.load()){EnableWindow(g_start,TRUE);EnableWindow(g_stop,FALSE);if(g_worker.joinable())g_worker.join();}
     return 0;
  }
  case WM_DESTROY:{g_run=false;PostQuitMessage(0);return 0;}
 }
 return DefWindowProcW(h,m,w,l);
}
static D3D12_HEAP_PROPERTIES Heap(D3D12_HEAP_TYPE type){D3D12_HEAP_PROPERTIES h{};h.Type=type;h.CPUPageProperty=D3D12_CPU_PAGE_PROPERTY_UNKNOWN;h.MemoryPoolPreference=D3D12_MEMORY_POOL_UNKNOWN;h.CreationNodeMask=h.VisibleNodeMask=1;return h;}
static D3D12_RESOURCE_DESC Texture(UINT w,UINT h,DXGI_FORMAT fmt,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE){
 D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;d.DepthOrArraySize=1;d.MipLevels=1;d.Format=fmt;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;d.Flags=flags;return d;
}
static ComPtr<ID3D12Resource> Tex(ID3D12Device* d,UINT w,UINT h,DXGI_FORMAT fmt,D3D12_RESOURCE_FLAGS flags,D3D12_RESOURCE_STATES state,D3D12_HEAP_FLAGS heapFlags=D3D12_HEAP_FLAG_NONE){
 ComPtr<ID3D12Resource> r;auto hd=Heap(D3D12_HEAP_TYPE_DEFAULT);auto rd=Texture(w,h,fmt,flags);
 Verify(d->CreateCommittedResource(&hd,heapFlags,&rd,state,nullptr,IID_PPV_ARGS(&r)),"CreateCommittedResource");return r;
}
static void Barrier(ID3D12GraphicsCommandList* cl,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
 if(a==b)return;D3D12_RESOURCE_BARRIER v{};v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;v.Transition.pResource=r;v.Transition.StateBefore=a;v.Transition.StateAfter=b;v.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;cl->ResourceBarrier(1,&v);
}
static void UavBarrier(ID3D12GraphicsCommandList* cl,ID3D12Resource* r){D3D12_RESOURCE_BARRIER v{};v.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;v.UAV.pResource=r;cl->ResourceBarrier(1,&v);}
struct FrameWarp{
 NVSDK_NGX_Parameter* params=nullptr;
 NVSDK_NGX_Handle* handle=nullptr;
 ID3D12Device* dev=nullptr;
 bool ready=false;unsigned long long frameID=0;
 ComPtr<ID3D12Resource> depth,mv,ui;
 float projection[16]{},prevview[16]{},currview[16]{};
 void Destroy(){
  if(handle){NVSDK_NGX_D3D12_ReleaseFeature(handle);handle=nullptr;}
  if(params){NVSDK_NGX_D3D12_DestroyParameters(params);params=nullptr;}
  ready=false;depth.Reset();mv.Reset();ui.Reset();
 }
 static void Projection(float p[16],float vfov,float aspect){
  float f=1.f/tanf(vfov*0.5f),n=0.1f,z=1000.f,q=z/(z-n);float m[16]={f/aspect,0,0,0, 0,f,0,0, 0,0,q,1, 0,0,-n*q,0};
  memcpy(p,m,sizeof m);
 }
 static void View(float m[16],float yaw,float pitch){
  float cy=cosf(yaw),sy=sinf(yaw),cp=cosf(pitch),sp=sinf(pitch);
  const float v[16]={cy,sy*sp,sy*cp,0, 0,cp,-sp,0, -sy,cy*sp,cy*cp,0, 0,0,0,1};
  memcpy(m,v,sizeof v);
 }
 bool Init(ID3D12Device* device,ID3D12GraphicsCommandList* cl,UINT width,UINT height,const std::wstring& dir,
           ID3D12Resource* d,ID3D12Resource* motion,ID3D12Resource* overlay){
  dev=device;depth=d;mv=motion;ui=overlay;
  if(GetFileAttributesW((dir+L"\\nvngx_latewarp.dll").c_str())==INVALID_FILE_ATTRIBUTES)return false;
  NVSDK_NGX_FeatureCommonInfo common{};
  auto hr=NVSDK_NGX_D3D12_Init(0x1000000ULL,dir.c_str(),device,&common,NVSDK_NGX_Version_API);
  if(NVSDK_NGX_FAILED(hr))return false;
  hr=NVSDK_NGX_D3D12_AllocateParameters(&params);
  if(NVSDK_NGX_FAILED(hr)||!params)return false;
  params->Set("Latewarp.Output.Width",width);
  params->Set("Latewarp.Output.Height",height);
  hr=NVSDK_NGX_D3D12_CreateFeature(cl,NVSDK_NGX_Feature_Reserved15,params,&handle);
  ready=!NVSDK_NGX_FAILED(hr)&&handle;return ready;
 }
 bool Warp(ID3D12GraphicsCommandList* cl,ID3D12Resource* color,ID3D12Resource* out,UINT width,UINT height,bool newframe,float yaw,float pitch,int fov){
  if(!ready)return false;
  auto p=params;
  Projection(projection,float(fov)*3.14159265f/180.f,float(width)/float(height));
  View(prevview,0,0);View(currview,yaw,pitch);
  p->Set("Latewarp.Backbuffer",color);p->Set("Latewarp.HudlessColor",color);
  p->Set("Latewarp.UIColorAlpha",ui.Get());p->Set("Depth",depth.Get());p->Set("MotionVectors",mv.Get());p->Set("Output",out);
  p->Set("Latewarp.NoWarpMask",(ID3D12Resource*)nullptr);
  auto rect=[&](const char*name,UINT w,UINT h){
    std::string n(name);
    p->Set((n+".Subrect.Base.X").c_str(),0u);p->Set((n+".Subrect.Base.Y").c_str(),0u);
    p->Set((n+".Subrect.Width").c_str(),w);p->Set((n+".Subrect.Height").c_str(),h);
  };
  rect("Latewarp.Backbuffer",width,height);rect("Latewarp.HudlessColor",width,height);
  rect("Latewarp.UIColorAlpha",width,height);rect("Latewarp.Output",width,height);
  rect("Latewarp.Depth",width,height);rect("Latewarp.MV",width,height);
  p->Set("Latewarp.WorldToViewMatrix",(void*)currview);
  p->Set("Latewarp.ViewToClipMatrix",(void*)projection);
  p->Set("Latewarp.PrevRenderedWorldToViewMatrix",(void*)prevview);
  p->Set("Latewarp.PrevRenderedViewToClipMatrix",(void*)projection);
  p->Set("Latewarp.DepthInverted",0u);p->Set("Latewarp.EvalFlags",0u);
  p->Set("Latewarp.UsePremultiplyUIAlpha",0u);
  auto evaluate=[&](unsigned render){
    p->Set("Latewarp.IsRenderedFrame",render);p->Set("Latewarp.FrameID",++frameID);
    return NVSDK_NGX_D3D12_EvaluateFeature(cl,handle,p,nullptr);
  };
  if(newframe){auto r=evaluate(1u);if(NVSDK_NGX_FAILED(r))return false;UavBarrier(cl,out);}
  return !NVSDK_NGX_FAILED(evaluate(0u));
 }
};
struct Graphics{
 ComPtr<IDXGIFactory6> factory;
 ComPtr<IDXGIAdapter1> adapter;
 ComPtr<ID3D12Device> d12;
 ComPtr<ID3D12CommandQueue> queue;
 ComPtr<IDXGISwapChain3> swap;
 ComPtr<ID3D12CommandAllocator> allocator;
 ComPtr<ID3D12GraphicsCommandList> cl;
 ComPtr<ID3D12Fence> fence,bridgeFence;
 ComPtr<ID3D11Fence> d11Fence;
 ComPtr<ID3D11Device> d11;
 ComPtr<ID3D11Device1> d11v1;
 ComPtr<ID3D11Device5> d11v5;
 ComPtr<ID3D11DeviceContext> c11;
 ComPtr<ID3D11DeviceContext4> c11v4;
 ComPtr<ID3D11Texture2D> src11;
 ComPtr<ID3D12Resource> src12,converted,warped,depth,mv,ui;
 ComPtr<ID3D12Resource> back[2];
 ComPtr<ID3D12RootSignature> root;
 ComPtr<ID3D12PipelineState> convertPSO;
 ComPtr<ID3D12DescriptorHeap> shaders,clears;
 HANDLE event=nullptr,shareTex=nullptr,shareFence=nullptr;
 UINT width=0,height=0,stride=0;
 uint64_t fenceValue=0,bridgeValue=0;
 FrameWarp warp;
 bool tearing=false,warpAsked=false,warpAvailable=false;
 ~Graphics(){warp.Destroy();if(event)CloseHandle(event);if(shareTex)CloseHandle(shareTex);if(shareFence)CloseHandle(shareFence);}
 void Sync(){
  const UINT64 v=++fenceValue;Verify(queue->Signal(fence.Get(),v),"signal render fence");
  if(fence->GetCompletedValue()<v){Verify(fence->SetEventOnCompletion(v,event),"SetEventOnCompletion");WaitForSingleObject(event,5000);}
 }
 void Begin(){Verify(allocator->Reset(),"Allocator reset");Verify(cl->Reset(allocator.Get(),nullptr),"list reset");}
 void End(){Verify(cl->Close(),"list close");ID3D12CommandList* lists[]={cl.Get()};queue->ExecuteCommandLists(1,lists);Sync();}
 D3D12_CPU_DESCRIPTOR_HANDLE CPU(ID3D12DescriptorHeap*h,int n){auto a=h->GetCPUDescriptorHandleForHeapStart();a.ptr+=SIZE_T(n)*stride;return a;}
 D3D12_GPU_DESCRIPTOR_HANDLE GPU(ID3D12DescriptorHeap*h,int n){auto a=h->GetGPUDescriptorHandleForHeapStart();a.ptr+=UINT64(n)*stride;return a;}
 void Init(HWND overlay,HWND target, const Settings&s, UINT frameW, UINT frameH){
  width=frameW;height=frameH;
  if(width>7680||height>4320)throw std::runtime_error("Capture resolution too high");
  Verify(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)),"DXGI factory");
  bool found=false;
  for(UINT i=0;;++i){
   ComPtr<IDXGIAdapter1>a;
   if(factory->EnumAdapters1(i,&a)==DXGI_ERROR_NOT_FOUND)break;
   DXGI_ADAPTER_DESC1 desc{};a->GetDesc1(&desc);
   if(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)continue;
   if(SUCCEEDED(D3D12CreateDevice(a.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d12)))){
     adapter=a;found=true;if(desc.VendorId==0x10DE)break;
   }
  }
  if(!found)throw std::runtime_error("D3D12 capable adapter not found");
  D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
  Verify(d12->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"D3D12 queue");
  Verify(d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"allocator");
  Verify(d12->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&cl)),"command list");
  Verify(cl->Close(),"first close");
  Verify(d12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"render fence");
  Verify(d12->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&bridgeFence)),"bridge fence");
  event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
  if(!event)throw std::runtime_error("CreateEvent failure");
  UINT allow=0;ComPtr<IDXGIFactory5> f5;
  if(SUCCEEDED(factory.As(&f5)))f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,&allow,sizeof allow);
  tearing=allow && !s.vsync;
  DXGI_SWAP_CHAIN_DESC1 sc{};
  sc.Width=width;sc.Height=height;sc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
  sc.SampleDesc.Count=1;sc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sc.BufferCount=2;
  sc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
  sc.Flags=allow?DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING:0;
  ComPtr<IDXGISwapChain1> temp;
  Verify(factory->CreateSwapChainForHwnd(queue.Get(),overlay,&sc,nullptr,nullptr,&temp),"CreateSwapChainForHwnd");
  Verify(temp.As(&swap),"swapchain 3");
  for(UINT i=0;i<2;i++)Verify(swap->GetBuffer(i,IID_PPV_ARGS(&back[i])),"swap buffer");
  UINT flags=D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  Verify(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,flags,nullptr,0,D3D11_SDK_VERSION,&d11,nullptr,&c11),"D3D11 device");
  Verify(d11.As(&d11v1),"D3D11.1");Verify(d11.As(&d11v5),"D3D11.5");
  Verify(c11.As(&c11v4),"D3D11 context4");
  // D3D11 OpenSharedResource1 requires the shared D3D12 resource to expose D3D11-compatible flags.
  // D3D12_RESOURCE_FLAG_NONE was the cause of E_INVALIDARG at the D3D11 shared color stage.
  src12=Tex(d12.Get(),width,height,DXGI_FORMAT_B8G8R8A8_UNORM,
      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS,
      D3D12_RESOURCE_STATE_COMMON,D3D12_HEAP_FLAG_SHARED);
  Verify(d12->CreateSharedHandle(src12.Get(),nullptr,GENERIC_ALL,nullptr,&shareTex),"shared color handle");
  Verify(d11v1->OpenSharedResource1(shareTex,IID_PPV_ARGS(&src11)),"D3D11 shared color");
  Verify(d12->CreateSharedHandle(bridgeFence.Get(),nullptr,GENERIC_ALL,nullptr,&shareFence),"shared fence handle");
  Verify(d11v5->OpenSharedFence(shareFence,IID_PPV_ARGS(&d11Fence)),"D3D11 shared fence");
  converted=Tex(d12.Get(),width,height,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  warped=Tex(d12.Get(),width,height,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  depth=Tex(d12.Get(),width,height,DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  mv=Tex(d12.Get(),width,height,DXGI_FORMAT_R16G16_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ui=Tex(d12.Get(),width,height,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  stride=d12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=5;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  Verify(d12->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&shaders)),"GPU descriptor heap");
  hd.NumDescriptors=3;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  Verify(d12->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&clears)),"CPU descriptor heap");
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=DXGI_FORMAT_B8G8R8A8_UNORM;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
  d12->CreateShaderResourceView(src12.Get(),&srv,CPU(shaders.Get(),0));
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
  uav.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d12->CreateUnorderedAccessView(converted.Get(),nullptr,&uav,CPU(shaders.Get(),1));
  ID3D12Resource* guides[]={depth.Get(),mv.Get(),ui.Get()};
  const DXGI_FORMAT formats[]={DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R16G16_FLOAT,DXGI_FORMAT_R8G8B8A8_UNORM};
  for(int i=0;i<3;i++){uav.Format=formats[i];d12->CreateUnorderedAccessView(guides[i],nullptr,&uav,CPU(shaders.Get(),i+2));d12->CreateUnorderedAccessView(guides[i],nullptr,&uav,CPU(clears.Get(),i));}
  D3D12_DESCRIPTOR_RANGE ranges[2]{};
  ranges[0].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;ranges[0].NumDescriptors=1;ranges[0].BaseShaderRegister=0;
  ranges[1].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_UAV;ranges[1].NumDescriptors=1;ranges[1].BaseShaderRegister=0;ranges[1].OffsetInDescriptorsFromTableStart=1;
  D3D12_ROOT_PARAMETER rootParam{};rootParam.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;rootParam.DescriptorTable.NumDescriptorRanges=2;
  rootParam.DescriptorTable.pDescriptorRanges=ranges;rootParam.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC signature{};signature.NumParameters=1;signature.pParameters=&rootParam;
  ComPtr<ID3DBlob>sig,errors;Verify(D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,&sig,&errors),"root serialize");
  Verify(d12->CreateRootSignature(0,sig->GetBufferPointer(),sig->GetBufferSize(),IID_PPV_ARGS(&root)),"root signature");
  const char* shader=R"(Texture2D<float4> capture : register(t0);
    RWTexture2D<float4> result : register(u0);
    [numthreads(8,8,1)] void CSMain(uint3 tid : SV_DispatchThreadID){
      uint w,h;result.GetDimensions(w,h);
      if(tid.x>=w||tid.y>=h)return;
      result[tid.xy]=capture.Load(int3(tid.xy,0));
    })";
  ComPtr<ID3DBlob>code;HRESULT hr=D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"CSMain","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);
  if(FAILED(hr)){std::string str="Compile conversion shader";if(errors)str+=": "+std::string((const char*)errors->GetBufferPointer(),errors->GetBufferSize());throw std::runtime_error(str);}
  D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};ps.pRootSignature=root.Get();ps.CS.pShaderBytecode=code->GetBufferPointer();ps.CS.BytecodeLength=code->GetBufferSize();
  Verify(d12->CreateComputePipelineState(&ps,IID_PPV_ARGS(&convertPSO)),"compute PSO");
  Begin();
  ID3D12DescriptorHeap* heaps[]={shaders.Get()};cl->SetDescriptorHeaps(1,heaps);
  const float flat[]={0.9995f,0.9995f,0.9995f,0.9995f},zero[]={0,0,0,0};
  for(int i=0;i<3;i++)cl->ClearUnorderedAccessViewFloat(GPU(shaders.Get(),i+2),CPU(clears.Get(),i),guides[i],i?zero:flat,0,nullptr);
  for(auto r:guides)Barrier(cl.Get(),r,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  warpAsked=s.wantWarp;
  if(warpAsked)warpAvailable=warp.Init(d12.Get(),cl.Get(),width,height,Folder(),depth.Get(),mv.Get(),ui.Get());
  End();
 }
 bool Capture(ID3D11Texture2D* src){
  if(!src)return false;
  c11->CopyResource(src11.Get(),src);
  Verify(c11v4->Signal(d11Fence.Get(),++bridgeValue),"D3D11 fence signal");c11->Flush();
  return true;
 }
 void Draw(bool fresh,float yaw,float pitch,const Settings&s){
  Begin();ID3D12DescriptorHeap* heap[]={shaders.Get()};cl->SetDescriptorHeaps(1,heap);
  if(fresh){
   Verify(queue->Wait(bridgeFence.Get(),bridgeValue),"wait capture GPU fence");
   Barrier(cl.Get(),src12.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
   Barrier(cl.Get(),converted.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
   cl->SetPipelineState(convertPSO.Get());cl->SetComputeRootSignature(root.Get());
   cl->SetComputeRootDescriptorTable(0,GPU(shaders.Get(),0));
   cl->Dispatch((width+7)/8,(height+7)/8,1);
   Barrier(cl.Get(),converted.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
   Barrier(cl.Get(),src12.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
  }
  ID3D12Resource* display=converted.Get();
  if(warpAvailable){
   if(!warp.Warp(cl.Get(),converted.Get(),warped.Get(),width,height,fresh,yaw,pitch,s.fov)){
     warpAvailable=false;Status(L"NVIDIA Frame Warp evaluation failed. Running WGC passthrough.");
   }else {UavBarrier(cl.Get(),warped.Get());display=warped.Get();}
  }
  if(display==warped.Get())Barrier(cl.Get(),warped.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
  else Barrier(cl.Get(),converted.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
  UINT index=swap->GetCurrentBackBufferIndex();
  Barrier(cl.Get(),back[index].Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_DEST);
  cl->CopyResource(back[index].Get(),display);
  Barrier(cl.Get(),back[index].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PRESENT);
  if(display==warped.Get())Barrier(cl.Get(),warped.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  else Barrier(cl.Get(),converted.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  End();
  HRESULT hr=swap->Present(s.vsync?1:0,tearing?DXGI_PRESENT_ALLOW_TEARING:0);
  if(FAILED(hr))Verify(hr,"Present");
 }
};
static void CaptureWorker(Settings opts){
 try{
  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  wc::GraphicsCaptureItem item{nullptr};
  auto factory=winrt::get_activation_factory<wc::GraphicsCaptureItem,IGraphicsCaptureItemInterop>();
  Verify(factory->CreateForWindow(opts.target,winrt::guid_of<wc::GraphicsCaptureItem>(),winrt::put_abi(item)),"capture item");
  const auto initialSize=item.Size();
  Graphics gpu;gpu.Init(g_overlay,opts.target,opts,UINT(initialSize.Width),UINT(initialSize.Height));
  ComPtr<IDXGIDevice> dxgi;Verify(gpu.d11.As(&dxgi),"DXGI D3D11 device");
  winrt::com_ptr<IInspectable> inspectable;
  Verify(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(),inspectable.put()),"WinRT WGC device");
  auto captureDevice=inspectable.as<w11::IDirect3DDevice>();
  auto pool=wc::Direct3D11CaptureFramePool::CreateFreeThreaded(captureDevice,wd::DirectXPixelFormat::B8G8R8A8UIntNormalized,2,item.Size());
  auto session=pool.CreateCaptureSession(item);
  try{session.IsCursorCaptureEnabled(opts.cursor);session.IsBorderRequired(false);}catch(...){}
  try{session.MinUpdateInterval(std::chrono::milliseconds(1));}catch(...){}
  session.StartCapture();
  bool have=false;long long timestamp=0;uint64_t frames=0;
  RECT last{};
  Status(gpu.warpAvailable?L"Running: NVIDIA Frame Warp active (NGX feature 15).":opts.wantWarp?
     L"Running WGC passthrough: NVIDIA Frame Warp runtime missing or feature unavailable.":
     L"Running WGC passthrough (Frame Warp off).");
  while(g_run.load()){
   if(!IsWindow(opts.target)||IsIconic(opts.target)){ShowWindow(g_overlay,SW_HIDE);g_presenting=false;Sleep(15);continue;}
   RECT visual{};
   if(FAILED(DwmGetWindowAttribute(opts.target,DWMWA_EXTENDED_FRAME_BOUNDS,&visual,sizeof visual)))GetWindowRect(opts.target,&visual);
   SetWindowPos(g_overlay,HWND_TOPMOST,visual.left,visual.top,gpu.width,gpu.height,SWP_NOACTIVATE);
   HWND fg=GetForegroundWindow();
   bool foreground=(fg==opts.target || GetAncestor(fg,GA_ROOT)==opts.target);
   if(!foreground){ShowWindow(g_overlay,SW_HIDE);g_presenting=false;Sleep(5);continue;}
   if(!g_presenting.exchange(true))ShowWindow(g_overlay,SW_SHOWNOACTIVATE);
   wc::Direct3D11CaptureFrame newest{nullptr};
   for(int i=0;i<4;i++){
     wc::Direct3D11CaptureFrame next{nullptr};
     try{next=pool.TryGetNextFrame();}catch(...){next=nullptr;}
     if(!next)break;newest=next;
   }
   bool fresh=bool(newest);
   if(fresh){
     auto size=newest.ContentSize();
     if((UINT)size.Width!=gpu.width||(UINT)size.Height!=gpu.height){Status(L"Game capture size changed. Restart capture.");break;}
     auto surface=newest.Surface();
     auto access=surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
     ComPtr<ID3D11Texture2D> texture;
     Verify(access->GetInterface(IID_PPV_ARGS(&texture)),"WGC surface");
     gpu.Capture(texture.Get());
     timestamp=(newest.SystemRelativeTime().count()*g_frequency.QuadPart)/10000000LL;
     ++frames;have=true;
   }
   if(have){
     LARGE_INTEGER now{};QueryPerformanceCounter(&now);
     long mx=0,my=0;MouseDiff(timestamp,now.QuadPart,mx,my);
     const float deg=float(opts.degreesPerCount*3.14159265358979323846/180.);
     gpu.Draw(fresh,mx*deg,my*deg,opts);
     if(!opts.vsync)Sleep(1);
   }else Sleep(1);
  }
  session.Close();pool.Close();
 }catch(const winrt::hresult_error&e){Status(L"Windows capture error: "+std::wstring(e.message().c_str())+L" (0x"+std::to_wstring((unsigned)e.code().value)+L")");}
 catch(const std::exception&e){Status(L"Error: "+Widen(e.what()));}
 catch(...){Status(L"Unexpected Windows/NGX error.");}
 ShowWindow(g_overlay,SW_HIDE);g_presenting=false;g_run=false;
}
static HWND MakeLabel(HWND parent,const wchar_t* text,int x,int y,int w,int h){
 return CreateWindowW(L"STATIC",text,WS_CHILD|WS_VISIBLE,x,y,w,h,parent,nullptr,GetModuleHandleW(nullptr),nullptr);
}
static void BuildUI(HWND h){
 HFONT font=(HFONT)GetStockObject(DEFAULT_GUI_FONT);
 auto add=[&](const wchar_t*name,DWORD style,int x,int y,int w,int ht,int id){
  HWND c=CreateWindowW(L"BUTTON",name,WS_CHILD|WS_VISIBLE|style,x,y,w,ht,h,(HMENU)(INT_PTR)id,GetModuleHandleW(nullptr),nullptr);
  SendMessageW(c,WM_SETFONT,(WPARAM)font,TRUE);return c;
 };
 MakeLabel(h,L"Game / Window",18,18,200,20);
 g_combo=CreateWindowW(L"COMBOBOX",L"",WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,18,43,385,400,h,(HMENU)IDC_GAMES,GetModuleHandleW(nullptr),nullptr);
 SendMessageW(g_combo,WM_SETFONT,(WPARAM)font,TRUE);
 add(L"Refresh",BS_PUSHBUTTON,411,42,100,27,IDC_REFRESH);
 MakeLabel(h,L"Vertical FOV (degrees)",18,91,210,18);
 HWND fov=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"60",WS_CHILD|WS_VISIBLE|ES_NUMBER,18,113,120,26,h,(HMENU)IDC_FOV,GetModuleHandleW(nullptr),nullptr);
 MakeLabel(h,L"Mouse rotation (degrees / count)",174,91,270,18);
 HWND gain=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"0.028",WS_CHILD|WS_VISIBLE,174,113,125,26,h,(HMENU)IDC_GAIN,GetModuleHandleW(nullptr),nullptr);
 SendMessageW(fov,WM_SETFONT,(WPARAM)font,TRUE);SendMessageW(gain,WM_SETFONT,(WPARAM)font,TRUE);
 add(L"Use NVIDIA Frame Warp",BS_AUTOCHECKBOX,18,159,250,25,IDC_WARP);
 add(L"V-Sync (off = lower delay)",BS_AUTOCHECKBOX,18,188,280,25,IDC_VSYNC);
 add(L"Capture game cursor",BS_AUTOCHECKBOX,18,217,250,25,IDC_CURSOR);
 SendDlgItemMessageW(h,IDC_WARP,BM_SETCHECK,BST_CHECKED,0);
 g_start=add(L"Start Capture",BS_DEFPUSHBUTTON,18,256,242,36,IDC_START);
 g_stop=add(L"Stop",BS_PUSHBUTTON,269,256,242,36,IDC_STOP);EnableWindow(g_stop,FALSE);
 g_status=MakeLabel(h,L"Select a game/window and press Start.",18,308,494,46);
 SendMessageW(g_status,WM_SETFONT,(WPARAM)font,TRUE);
 MakeLabel(h,L"Click-through overlay | WGC 1 ms request | DX12 | Read-only mouse input",18,366,505,24);
 RefreshGames();SetTimer(h,1,500,nullptr);
}
int WINAPI wWinMain(HINSTANCE inst,HINSTANCE,LPWSTR,int show){
 SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
 QueryPerformanceFrequency(&g_frequency);
 WNDCLASSW wc{};wc.lpfnWndProc=ControlProc;wc.hInstance=inst;wc.lpszClassName=MAIN_CLASS;wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
 wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);RegisterClassW(&wc);
 WNDCLASSW ov{};ov.lpfnWndProc=OverlayProc;ov.hInstance=inst;ov.lpszClassName=OVERLAY_CLASS;ov.hCursor=LoadCursor(nullptr,IDC_ARROW);RegisterClassW(&ov);
 g_main=CreateWindowExW(0,MAIN_CLASS,L"FrameWarp Custom — WGC + NVIDIA NGX",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
    CW_USEDEFAULT,CW_USEDEFAULT,550,440,nullptr,nullptr,inst,nullptr);
 g_overlay=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_TRANSPARENT|WS_EX_LAYERED|WS_EX_NOACTIVATE,OVERLAY_CLASS,L"",WS_POPUP,
      0,0,1280,720,nullptr,nullptr,inst,nullptr);
 SetLayeredWindowAttributes(g_overlay,0,255,LWA_ALPHA);ShowWindow(g_overlay,SW_HIDE);
 BuildUI(g_main);
 RAWINPUTDEVICE rid{};rid.usUsagePage=1;rid.usUsage=2;rid.dwFlags=RIDEV_INPUTSINK;rid.hwndTarget=g_main;
 RegisterRawInputDevices(&rid,1,sizeof rid);
 ShowWindow(g_main,show);UpdateWindow(g_main);
 MSG msg{};
 while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
 g_run=false;if(g_worker.joinable())g_worker.join();if(g_overlay)DestroyWindow(g_overlay);return (int)msg.wParam;
}
