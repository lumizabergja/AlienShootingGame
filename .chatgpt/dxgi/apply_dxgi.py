from pathlib import Path
import re

root=Path("source")

# Streamline helper: keep native interfaces for normal D3D12 work.
h=root/"dlss_bridge.h"
s=h.read_text(encoding="utf-8-sig")
if "GetNativeInterface(" not in s:
    s=s.replace(
        '    void UpgradeInterface(void** p) {Check(upgradeInterface(p),"slUpgradeInterface");}\n',
        '    void UpgradeInterface(void** p) {Check(upgradeInterface(p),"slUpgradeInterface");}\n'
        '    void GetNativeInterface(void* proxy, void** native) {Check(getNativeInterface(proxy,native),"slGetNativeInterface");}\n',
        1)
    s=s.replace(
        '    PFun_slUpgradeInterface* upgradeInterface{};\n',
        '    PFun_slUpgradeInterface* upgradeInterface{};\n'
        '    PFun_slGetNativeInterface* getNativeInterface{};\n',
        1)
h.write_text(s,encoding="utf-8")

p=root/"dlss_bridge.cpp"
s=p.read_text(encoding="utf-8-sig")
if 'Export(getNativeInterface,"slGetNativeInterface")' not in s:
    s=s.replace(
        'Export(init,"slInit"); Export(shutdown,"slShutdown"); Export(setDevice,"slSetD3DDevice"); Export(upgradeInterface,"slUpgradeInterface");',
        'Export(init,"slInit"); Export(shutdown,"slShutdown"); Export(setDevice,"slSetD3DDevice"); Export(upgradeInterface,"slUpgradeInterface"); Export(getNativeInterface,"slGetNativeInterface");',
        1)
old='''    uint32_t n=index++;Check(newToken(token,&n),"New frame token");
    if(fg) Check(reflexSleep(*token),"Reflex sleep");
    framePrepared = true;
'''
if old in s:
    s=s.replace(old,'''    uint32_t n=index++;Check(newToken(token,&n),"New frame token");
    if(fg) {
        if(n<4) log(L"Reflex sleep enter frame "+std::to_wstring(n));
        Check(reflexSleep(*token),"Reflex sleep");
        if(n<4) log(L"Reflex sleep return frame "+std::to_wstring(n));
    }
    framePrepared = true;
''',1)
p.write_text(s,encoding="utf-8")

p=root/"main.cpp"
s=p.read_text(encoding="utf-8-sig")

for inc in [
    '#include <windows.graphics.capture.interop.h>\n',
    '#include <windows.graphics.directx.direct3d11.interop.h>\n',
    '#include <winrt/base.h>\n',
    '#include <winrt/Windows.Foundation.h>\n',
    '#include <winrt/Windows.Graphics.Capture.h>\n',
    '#include <winrt/Windows.Graphics.DirectX.h>\n',
    '#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>\n']:
    s=s.replace(inc,'')
s=s.replace('using namespace winrt;\nnamespace wgc = winrt::Windows::Graphics::Capture;\nnamespace wdx = winrt::Windows::Graphics::DirectX;\nnamespace wd3d = winrt::Windows::Graphics::DirectX::Direct3D11;\n','')
s=s.replace('constexpr wchar_t kAppName[] = L"DLSS 4.5 Low Latency Presenter - WGC1";',
            'constexpr wchar_t kAppName[] = L"DLSS Low Latency Presenter - DXGI Desktop Duplication";',1)
s=s.replace('constexpr wchar_t kControlClass[] = L"DLSS45WGC.Control";',
            'constexpr wchar_t kControlClass[] = L"DLSSDXGI.Control";',1)
s=s.replace('constexpr wchar_t kPresenterClass[] = L"DLSS45WGC.Presenter";',
            'constexpr wchar_t kPresenterClass[] = L"DLSSDXGI.Presenter";',1)

a=s.find("class WgcCapture {")
b=s.find("struct Dx12State {",a)
if a<0 or b<0: raise SystemExit("WGC capture class boundaries not found")
dxgi_class=r'''class DxgiCapture {
public:
    ~DxgiCapture(){Stop();}
    bool Init(HWND target,IDXGIAdapter1* adapter,ID3D12Device* d12,ID3D12CommandQueue* queue,UINT& width,UINT& height){
        target_=target;d12Queue_=queue;
        UINT flags=D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_12_1,D3D_FEATURE_LEVEL_12_0,D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL got{};IUnknown* queues[]={queue};
        HR(D3D11On12CreateDevice(d12,flags,levels,ARRAYSIZE(levels),queues,1,0,&dev_,&ctx_,&got),"D3D11On12 duplication device");
        HR(dev_.As(&on12_),"ID3D11On12Device2");

        HMONITOR wanted=MonitorFromWindow(target_,MONITOR_DEFAULTTONEAREST);
        ComPtr<IDXGIOutput> output;
        for(UINT i=0;;++i){
            ComPtr<IDXGIOutput> x;HRESULT hr=adapter->EnumOutputs(i,x.GetAddressOf());
            if(hr==DXGI_ERROR_NOT_FOUND)break;HR(hr,"EnumOutputs");
            DXGI_OUTPUT_DESC od{};HR(x->GetDesc(&od),"Output GetDesc");
            if(od.Monitor==wanted){output=x;outputDesc_=od;break;}
        }
        if(!output)throw std::runtime_error("Could not find the DXGI output containing the selected game window.");
        ComPtr<IDXGIOutput1> output1;HR(output.As(&output1),"IDXGIOutput1");
        HR(output1->DuplicateOutput(dev_.Get(),&dup_),"DXGI DuplicateOutput");

        if(!UpdateClientRect(width,height))throw std::runtime_error("Could not resolve the selected game client rectangle on the duplicated monitor.");
        width_=width;height_=height;CreatePersistentInput(d12);
        HR(d12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&copyFence_)),"Create DXGI copy fence");
        copyEvent_=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if(!copyEvent_)throw std::runtime_error("Could not create DXGI copy event.");
        gLog.Write(L"DXGI Desktop Duplication: AcquireNextFrame(0), 0 software queue, cropped selected client area, GPU-only copy.");
        return true;
    }
    void Stop(){
        if(held_&&dup_){dup_->ReleaseFrame();held_=false;}
        if(copyEvent_){CloseHandle(copyEvent_);copyEvent_=nullptr;}
        input11_.Reset();input12_.Reset();copyFence_.Reset();dup_.Reset();on12_.Reset();ctx_.Reset();dev_.Reset();d12Queue_.Reset();
    }
    UINT Width()const{return width_;}UINT Height()const{return height_;}
    ID3D12Resource* Input12()const{return input12_.Get();}
    uint64_t Dropped()const{return dropped_;}

    bool AcquireNewest(LARGE_INTEGER& captureQpc){
        if(!dup_)return false;
        DXGI_OUTDUPL_FRAME_INFO fi{};ComPtr<IDXGIResource> res;
        HRESULT hr=dup_->AcquireNextFrame(0,&fi,&res);
        if(hr==DXGI_ERROR_WAIT_TIMEOUT)return false;
        if(hr==DXGI_ERROR_ACCESS_LOST)throw std::runtime_error("DXGI desktop duplication access was lost. Restart capture.");
        HR(hr,"DXGI AcquireNextFrame");held_=true;
        if(fi.AccumulatedFrames>1)dropped_+=fi.AccumulatedFrames-1;
        captureQpc=fi.LastPresentTime;if(!captureQpc.QuadPart)QueryPerformanceCounter(&captureQpc);

        UINT cw=0,ch=0;
        if(!UpdateClientRect(cw,ch)){ReleaseHeld();throw std::runtime_error("Selected game window moved outside the duplicated monitor.");}
        if(cw!=width_||ch!=height_){ReleaseHeld();throw std::runtime_error("Game client size changed. Stop and Start again.");}

        ComPtr<ID3D11Texture2D> src;HR(res.As(&src),"DXGI frame texture");
        D3D11_BOX box{};
        box.left=(UINT)(clientDesktop_.left-outputDesc_.DesktopCoordinates.left);
        box.top=(UINT)(clientDesktop_.top-outputDesc_.DesktopCoordinates.top);
        box.right=box.left+width_;box.bottom=box.top+height_;box.front=0;box.back=1;
        UINT ow=(UINT)(outputDesc_.DesktopCoordinates.right-outputDesc_.DesktopCoordinates.left);
        UINT oh=(UINT)(outputDesc_.DesktopCoordinates.bottom-outputDesc_.DesktopCoordinates.top);
        if(box.right>ow||box.bottom>oh){ReleaseHeld();throw std::runtime_error("Game client is not fully inside the duplicated monitor.");}

        ID3D11Resource* wrapped[]={input11_.Get()};
        on12_->AcquireWrappedResources(wrapped,1);
        ctx_->CopySubresourceRegion(input11_.Get(),0,0,0,0,src.Get(),0,&box);
        on12_->ReleaseWrappedResources(wrapped,1);
        ctx_->Flush();

        uint64_t v=++copyValue_;
        HR(d12Queue_->Signal(copyFence_.Get(),v),"Signal DXGI capture copy");
        if(copyFence_->GetCompletedValue()<v){
            HR(copyFence_->SetEventOnCompletion(v,copyEvent_),"Arm DXGI copy event");
            DWORD wr=WaitForSingleObject(copyEvent_,500);
            if(wr!=WAIT_OBJECT_0){ReleaseHeld();throw std::runtime_error("DXGI capture GPU copy did not finish within 500 ms.");}
        }
        ReleaseHeld();return true;
    }
private:
    bool UpdateClientRect(UINT& w,UINT& h){
        RECT c{};if(!GetClientRect(target_,&c))return false;
        POINT tl{c.left,c.top},br{c.right,c.bottom};
        if(!ClientToScreen(target_,&tl)||!ClientToScreen(target_,&br))return false;
        clientDesktop_={tl.x,tl.y,br.x,br.y};
        w=(UINT)std::max<LONG>(1,br.x-tl.x);h=(UINT)std::max<LONG>(1,br.y-tl.y);
        RECT o=outputDesc_.DesktopCoordinates;
        return clientDesktop_.left>=o.left&&clientDesktop_.top>=o.top&&clientDesktop_.right<=o.right&&clientDesktop_.bottom<=o.bottom;
    }
    void CreatePersistentInput(ID3D12Device* d12){
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;hp.CreationNodeMask=1;hp.VisibleNodeMask=1;
        D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;rd.Width=width_;rd.Height=height_;
        rd.DepthOrArraySize=1;rd.MipLevels=1;rd.Format=DXGI_FORMAT_B8G8R8A8_UNORM;rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;
        HR(d12->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&input12_)),"Create persistent DXGI D3D12 input");
        D3D11_RESOURCE_FLAGS rf{};rf.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        HR(on12_->CreateWrappedResource(input12_.Get(),&rf,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON,IID_PPV_ARGS(&input11_)),"Wrap persistent DXGI input");
    }
    void ReleaseHeld(){
        if(held_&&dup_){HRESULT hr=dup_->ReleaseFrame();held_=false;if(FAILED(hr)&&hr!=DXGI_ERROR_ACCESS_LOST)HR(hr,"DXGI ReleaseFrame");}
    }
    HWND target_{};UINT width_{},height_{};RECT clientDesktop_{};DXGI_OUTPUT_DESC outputDesc_{};
    bool held_{};uint64_t dropped_{},copyValue_{};
    ComPtr<ID3D11Device> dev_;ComPtr<ID3D11DeviceContext> ctx_;ComPtr<ID3D11On12Device2> on12_;
    ComPtr<IDXGIOutputDuplication> dup_;ComPtr<ID3D12Resource> input12_;ComPtr<ID3D11Resource> input11_;
    ComPtr<ID3D12CommandQueue> d12Queue_;ComPtr<ID3D12Fence> copyFence_;HANDLE copyEvent_{};
};

'''
s=s[:a]+dxgi_class+s[b:]

s=s.replace(
'''struct Dx12State {
    ComPtr<IDXGIFactory6> factory;ComPtr<IDXGIAdapter1> adapter;ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;
''',
'''struct Dx12State {
    ComPtr<IDXGIFactory6> factory,proxyFactory;ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device,proxyDevice;ComPtr<ID3D12CommandQueue> queue,proxyQueue;
''',1)
s=s.replace('    bool RenderOne(WgcCapture& cap,Dx12State& d,ID3D12Resource* source,const Settings&,LARGE_INTEGER captureQpc,double& ageSum,double& ageMax,uint64_t& ageN);',
            '    bool RenderOne(Dx12State& d,ID3D12Resource* source,const Settings&,LARGE_INTEGER captureQpc,double& ageSum,double& ageMax,uint64_t& ageN);',1)
s=s.replace('Latency core: WGC1 + D3D11On12 direct unwrap | 1 buffer | 0 capture copy | 0 software queue | freshest-frame drop',
            'Latency core: DXGI Desktop Duplication | AcquireNextFrame(0) | 0 software queue | cropped game client | GPU-only copy',1)
s=s.replace('HWND cur=CreateWindowW(L"BUTTON",L"Capture system cursor"',
            'HWND cur=CreateWindowW(L"BUTTON",L"Cursor not composited by DXGI"',1)
s=s.replace('if(w){SetWindowDisplayAffinity(w,WDA_EXCLUDEFROMCAPTURE_VALUE);ShowWindow(w,SW_SHOWNOACTIVATE);',
            'if(w){if(!SetWindowDisplayAffinity(w,WDA_EXCLUDEFROMCAPTURE_VALUE))gLog.Write(L"Warning: presenter exclusion from capture was not accepted.");ShowWindow(w,SW_SHOWNOACTIVATE);',1)

m=re.search(r'bool App::CreateSwapchain\(HWND out,const Settings& s,Dx12State& d\)\{.*?\n\}',s,re.S)
if not m: raise SystemExit("CreateSwapchain not found")
swap='''bool App::CreateSwapchain(HWND out,const Settings& s,Dx12State& d){
    RECT r{};GetClientRect(out,&r);d.width=(UINT)std::max<LONG>(1,r.right);d.height=(UINT)std::max<LONG>(1,r.bottom);
    BOOL allow=FALSE;d.factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,&allow,sizeof(allow));d.tearing=allow==TRUE;
    DXGI_SWAP_CHAIN_DESC1 sd{};sd.Width=d.width;sd.Height=d.height;sd.Format=DXGI_FORMAT_B8G8R8A8_UNORM;sd.SampleDesc.Count=1;
    sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sd.BufferCount=kBackBufferCount;sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode=DXGI_ALPHA_MODE_IGNORE;sd.Scaling=DXGI_SCALING_STRETCH;
    sd.Flags=(d.tearing?DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING:0)|DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    ComPtr<IDXGISwapChain1> tmp;
    HR(d.proxyFactory->CreateSwapChainForHwnd(d.proxyQueue.Get(),out,&sd,nullptr,nullptr,&tmp),"Create Streamline-hooked swapchain");
    HR(tmp.As(&d.swap),"Streamline proxy Swapchain3");d.factory->MakeWindowAssociation(out,DXGI_MWA_NO_ALT_ENTER);
    ComPtr<IDXGISwapChain2> ls;HR(d.swap.As(&ls),"Swapchain2 host pacing");HR(ls->SetMaximumFrameLatency(1),"Host SetMaximumFrameLatency(1)");
    gLog.Write(L"Output pacing: host-owned frame-latency waitable object; max latency=1.");
    D3D12_CPU_DESCRIPTOR_HANDLE h=d.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for(UINT i=0;i<kBackBufferCount;i++){HR(d.swap->GetBuffer(i,IID_PPV_ARGS(&d.back[i])),"Backbuffer");d.device->CreateRenderTargetView(d.back[i].Get(),nullptr,h);h.ptr+=d.rtvStride;}
    return true;
}'''
s=s[:m.start()]+swap+s[m.end():]

m=re.search(r'bool App::InitDx12\(HWND output,IDXGIAdapter1\* adapter,const Settings&s,Dx12State& d\)\{.*?\n\}',s,re.S)
if not m: raise SystemExit("InitDx12 not found")
init='''bool App::InitDx12(HWND output,IDXGIAdapter1* adapter,const Settings&s,Dx12State& d){
    if(!adapter)return false;d.adapter=adapter;
    HR(D3D12CreateDevice(d.adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d.device)),"Native D3D12 device");
    gDLSS.BindDevice(d.device.Get());
    ID3D12Device* pd=d.device.Get();gDLSS.UpgradeInterface(reinterpret_cast<void**>(&pd));d.proxyDevice.Attach(pd);
    HR(CreateDXGIFactory2(0,IID_PPV_ARGS(&d.factory)),"Native DXGI factory");
    IDXGIFactory6* pf=d.factory.Get();gDLSS.UpgradeInterface(reinterpret_cast<void**>(&pf));d.proxyFactory.Attach(pf);
    D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;q.Priority=D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    HR(d.proxyDevice->CreateCommandQueue(&q,IID_PPV_ARGS(&d.proxyQueue)),"Streamline-hooked queue creation");
    ID3D12CommandQueue* nq=nullptr;gDLSS.GetNativeInterface(d.proxyQueue.Get(),reinterpret_cast<void**>(&nq));d.queue.Attach(nq);
    D3D12_DESCRIPTOR_HEAP_DESC rh{};rh.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;rh.NumDescriptors=kBackBufferCount;
    HR(d.device->CreateDescriptorHeap(&rh,IID_PPV_ARGS(&d.rtvHeap)),"RTV heap");d.rtvStride=d.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_DESCRIPTOR_HEAP_DESC sh{};sh.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;sh.NumDescriptors=1;sh.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HR(d.device->CreateDescriptorHeap(&sh,IID_PPV_ARGS(&d.srvHeap)),"SRV heap");
    HR(d.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&d.allocator)),"Allocator");
    HR(d.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,d.allocator.Get(),nullptr,IID_PPV_ARGS(&d.list)),"Command list");d.list->Close();
    HR(d.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&d.frameFence)),"Frame fence");
    HR(d.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&d.captureReadyFence)),"Capture ready fence");
    d.fenceEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!d.fenceEvent)return false;
    CreateBlitPipeline(d);CreateSwapchain(output,s,d);return true;
}'''
s=s[:m.start()]+init+s[m.end():]

s=s.replace('bool App::RenderOne(WgcCapture& cap,Dx12State& d,ID3D12Resource* source,const Settings&s,LARGE_INTEGER cq,double& ageSum,double& ageMax,uint64_t& ageN){',
            'bool App::RenderOne(Dx12State& d,ID3D12Resource* source,const Settings&s,LARGE_INTEGER cq,double& ageSum,double& ageMax,uint64_t& ageN){',1)
s=s.replace('    gDLSS.TagBackbuffer(d.list.Get(),d.back[bi].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET);\n','',1)
old='gDLSS.Marker(sl::PCLMarker::ePresentStart);UINT sync=s.vsync?1:0;UINT flags=(!s.vsync&&d.tearing)?DXGI_PRESENT_ALLOW_TEARING:0;HRESULT ph=d.swap->Present(sync,flags);gDLSS.Marker(sl::PCLMarker::ePresentEnd);if(FAILED(ph))return false;gDLSS.AfterPresent();'
if old not in s: raise SystemExit("Present block not found")
s=s.replace(old,'''gDLSS.Marker(sl::PCLMarker::ePresentStart);UINT sync=s.vsync?1:0;UINT flags=(!s.vsync&&d.tearing)?DXGI_PRESENT_ALLOW_TEARING:0;
    gLog.Write(L"Present enter");HRESULT ph=d.swap->Present(sync,flags);wchar_t pm[96]{};swprintf_s(pm,L"Present return hr=0x%08X",(unsigned)ph);gLog.Write(pm);
    gDLSS.Marker(sl::PCLMarker::ePresentEnd);if(FAILED(ph))return false;gDLSS.AfterPresent();''',1)

m=re.search(r'void App::Worker\(HWND target,Settings s\)\{.*?\n\}\n\nvoid App::PostStatus',s,re.S)
if not m: raise SystemExit("Worker not found")
worker='''void App::Worker(HWND target,Settings s){
    DWORD task=0;HANDLE av=AvSetMmThreadCharacteristicsW(L"Games",&task);if(av)AvSetMmThreadPriority(av,AVRT_PRIORITY_HIGH);SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_HIGHEST);
    DxgiCapture cap;Dx12State d;try{
        ComPtr<IDXGIFactory6> tf;ComPtr<IDXGIAdapter1> adapter;if(!PickAdapter(tf,adapter))throw std::runtime_error("No suitable GPU adapter found.");
        if(!presenter_)throw std::runtime_error("Presenter window was not created.");
        if(!InitDx12(presenter_,adapter.Get(),s,d))throw std::runtime_error("DX12 initialization failed.");
        UINT cw=0,ch=0;cap.Init(target,adapter.Get(),d.device.Get(),d.queue.Get(),cw,ch);
        PostStatus(L"Initializing DLSS / MFG before DXGI capture...");
        gDLSS.Init(d.device.Get(),d.adapter.Get(),cap.Input12(),cap.Width(),cap.Height(),d.width,d.height,s.sr,s.preset,s.fg,s.multiplier,s.dynamicTarget,s.boost,s.nvof);
        D3D12_SHADER_RESOURCE_VIEW_DESC v{};v.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;v.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;v.Texture2D.MipLevels=1;
        d.device->CreateShaderResourceView(gDLSS.result.Get(),&v,d.srvHeap->GetCPUDescriptorHandleForHeapStart());d.dlssReady=true;
        PostStatus(L"RUNNING: DXGI Desktop Duplication / AcquireNextFrame(0) / 0 software queue / cropped game client");
        uint64_t frames=0,lastDrops=0,ageN=0;double ageSum=0,ageMax=0;auto report=std::chrono::steady_clock::now();
        while(!stop_&&IsWindow(target)&&IsWindow(presenter_)){
            if(paused_){Sleep(2);continue;}
            LARGE_INTEGER cq{};
            gDLSS.BeforeFrame(d.queue.Get());
            if(!cap.AcquireNewest(cq)){SwitchToThread();continue;}
            gDLSS.PrepareFrame();
            if(!RenderOne(d,cap.Input12(),s,cq,ageSum,ageMax,ageN))throw std::runtime_error("Present failed.");
            ++frames;
            auto now=std::chrono::steady_clock::now();double sec=std::chrono::duration<double>(now-report).count();
            if(sec>=1.0){
                uint64_t drops=cap.Dropped();wchar_t msg[1000]{};double avg=ageN?ageSum/ageN:0;
                swprintf_s(msg,L"Base %.0f FPS | DXGI accumulated drops %llu/s | capture->Present %.2f ms avg / %.2f max | %s",
                    frames/sec,(unsigned long long)((drops-lastDrops)/std::max(0.001,sec)),avg,ageMax,gDLSS.Status().c_str());
                PostStatus(msg);gLog.Write(msg);frames=0;lastDrops=drops;ageN=0;ageSum=ageMax=0;report=now;
            }
        }
    }catch(const std::exception&e){std::string m=e.what();std::wstring w(m.begin(),m.end());gLog.Write(L"Stopped: "+w);PostStatus(L"Stopped: "+w);}
    try{gDLSS.BeforeFrame(d.queue.Get());}catch(...){}cap.Stop();DestroyDx12(d);gDLSS.Release();
    if(av)AvRevertMmThreadCharacteristics(av);PostMessageW(control_,WM_APP_STOPPED,0,0);
}

void App::PostStatus'''
s=s[:m.start()]+worker+s[m.end():]

p.write_text(s,encoding="utf-8")
print("DXGI transform applied")
