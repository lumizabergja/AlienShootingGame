from pathlib import Path
import re

h=Path("source/dlss_bridge.h")
s=h.read_text(encoding="utf-8-sig")
old='    void UpgradeInterface(void** p) {Check(upgradeInterface(p),"slUpgradeInterface");}\n'
if old not in s: raise SystemExit("UpgradeInterface declaration not found")
s=s.replace(old,old+'    void GetNativeInterface(void* proxy, void** native) {Check(getNativeInterface(proxy,native),"slGetNativeInterface");}\n',1)
old='    PFun_slUpgradeInterface* upgradeInterface{};\n'
if old not in s: raise SystemExit("upgradeInterface member not found")
s=s.replace(old,old+'    PFun_slGetNativeInterface* getNativeInterface{};\n',1)
h.write_text(s,encoding="utf-8")

p=Path("source/dlss_bridge.cpp")
s=p.read_text(encoding="utf-8-sig")
old='Export(init,"slInit"); Export(shutdown,"slShutdown"); Export(setDevice,"slSetD3DDevice"); Export(upgradeInterface,"slUpgradeInterface");'
if old not in s: raise SystemExit("Streamline export line not found")
s=s.replace(old,old+' Export(getNativeInterface,"slGetNativeInterface");',1)
p.write_text(s,encoding="utf-8")

p=Path("source/main.cpp")
s=p.read_text(encoding="utf-8-sig")

old='        HR(dev_.As(&on12_),"ID3D11On12Device2");\n'
if old not in s: raise SystemExit("D3D11On12 init point not found")
new=old+'''        d12Queue_=queue;
        HR(d12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&copyFence_)),"Create WGC copy-completion fence");
        copyEvent_=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if(!copyEvent_) throw std::runtime_error("Could not create WGC copy-completion event.");
'''
s=s.replace(old,new,1)

old='        if(frameEvent_){SetEvent(frameEvent_);CloseHandle(frameEvent_);frameEvent_=nullptr;}\n        input11_.Reset();input12_.Reset();on12_.Reset();ctx_.Reset();dev_.Reset();\n'
if old not in s: raise SystemExit("WGC Stop cleanup block not found")
new='''        if(frameEvent_){SetEvent(frameEvent_);CloseHandle(frameEvent_);frameEvent_=nullptr;}
        if(copyEvent_){SetEvent(copyEvent_);CloseHandle(copyEvent_);copyEvent_=nullptr;}
        input11_.Reset();input12_.Reset();copyFence_.Reset();d12Queue_.Reset();on12_.Reset();ctx_.Reset();dev_.Reset();
'''
s=s.replace(old,new,1)

pat=r'on12_->ReleaseWrappedResources\\(wrapped,1\\);\\s*ctx_->Flush\\(\\);\\s*src11\\.Reset\\(\\);\\s*frame\\.Close\\(\\);frame=nullptr;'
new='''on12_->ReleaseWrappedResources(wrapped,1);
        ctx_->Flush();
        const uint64_t value=++copyValue_;
        HR(d12Queue_->Signal(copyFence_.Get(),value),"Signal WGC copy completion");
        if(copyFence_->GetCompletedValue()<value){
            HR(copyFence_->SetEventOnCompletion(value,copyEvent_),"Arm WGC copy completion event");
            DWORD wr=WaitForSingleObject(copyEvent_,500);
            if(wr!=WAIT_OBJECT_0) throw std::runtime_error("WGC GPU copy did not complete within 500 ms.");
        }
        src11.Reset();
        frame.Close();frame=nullptr;'''
s,n=re.subn(pat,new,s,count=1,flags=re.S)
if n!=1: raise SystemExit("WGC copy release block not found")

old='    ComPtr<ID3D12Resource> input12_;ComPtr<ID3D11Resource> input11_;\n    HANDLE frameEvent_{};\n'
if old not in s: raise SystemExit("WGC member insertion point not found")
new='''    ComPtr<ID3D12Resource> input12_;ComPtr<ID3D11Resource> input11_;
    ComPtr<ID3D12CommandQueue> d12Queue_;ComPtr<ID3D12Fence> copyFence_;
    HANDLE frameEvent_{},copyEvent_{};uint64_t copyValue_{};
'''
s=s.replace(old,new,1)

s=s.replace('Capture bridge: one persistent D3D12 BGRA8 input, GPU-only D3D11On12 copy, WGC frame released immediately.',
            'Capture bridge: WGC1 GPU copy is fenced before the single capture surface is returned; DLSS/MFG stays asynchronous.',1)

old='''struct Dx12State {
    ComPtr<IDXGIFactory6> factory;ComPtr<IDXGIAdapter1> adapter;ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain3> swap;ComPtr<ID3D12DescriptorHeap> rtvHeap,srvHeap;
'''
if old not in s: raise SystemExit("Dx12State header not found")
new='''struct Dx12State {
    ComPtr<IDXGIFactory6> factory,proxyFactory;ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device,proxyDevice;ComPtr<ID3D12CommandQueue> queue,proxyQueue;
    ComPtr<IDXGISwapChain3> swap;ComPtr<ID3D12DescriptorHeap> rtvHeap,srvHeap;
'''
s=s.replace(old,new,1)

old='''    BOOL allow=FALSE;d.factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,&allow,sizeof(allow));d.tearing=allow==TRUE&&!s.vsync;
    DXGI_SWAP_CHAIN_DESC1 sd{};sd.Width=d.width;sd.Height=d.height;sd.Format=DXGI_FORMAT_B8G8R8A8_UNORM;sd.SampleDesc.Count=1;sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sd.BufferCount=kBackBufferCount;sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;sd.AlphaMode=DXGI_ALPHA_MODE_IGNORE;sd.Scaling=DXGI_SCALING_STRETCH;sd.Flags=d.tearing?DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING:0;
    ComPtr<IDXGISwapChain1> tmp;HR(d.factory->CreateSwapChainForHwnd(d.queue.Get(),out,&sd,nullptr,nullptr,&tmp),"Create flip swapchain");HR(tmp.As(&d.swap),"Swapchain3");d.factory->MakeWindowAssociation(out,DXGI_MWA_NO_ALT_ENTER);
    if(s.fg==sl::DLSSGMode::eOff){ComPtr<IDXGISwapChain2>x;if(SUCCEEDED(d.swap.As(&x)))x->SetMaximumFrameLatency(1);} // FG path uses Reflex pacing instead.
'''
if old not in s: raise SystemExit("CreateSwapchain integration block not found")
new='''    BOOL allow=FALSE;d.factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,&allow,sizeof(allow));d.tearing=allow==TRUE&&!s.vsync;
    DXGI_SWAP_CHAIN_DESC1 sd{};sd.Width=d.width;sd.Height=d.height;sd.Format=DXGI_FORMAT_B8G8R8A8_UNORM;sd.SampleDesc.Count=1;sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sd.BufferCount=kBackBufferCount;sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;sd.AlphaMode=DXGI_ALPHA_MODE_IGNORE;sd.Scaling=DXGI_SCALING_STRETCH;sd.Flags=d.tearing?DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING:0;
    ComPtr<IDXGISwapChain1> tmp;
    HR(d.proxyFactory->CreateSwapChainForHwnd(d.proxyQueue.Get(),out,&sd,nullptr,nullptr,&tmp),"Create Streamline-hooked flip swapchain");
    HR(tmp.As(&d.swap),"Streamline proxy Swapchain3");
    d.factory->MakeWindowAssociation(out,DXGI_MWA_NO_ALT_ENTER);
'''
s=s.replace(old,new,1)

pat=r'''bool App::InitDx12\(HWND output,IDXGIAdapter1\* adapter,const Settings&s,Dx12State& d\)\{\n    if\(!adapter\)return false;\n    d\.adapter=adapter;\n.*?    D3D12_DESCRIPTOR_HEAP_DESC rh\{\};'''
m=re.search(pat,s,flags=re.S)
if not m: raise SystemExit("InitDx12 proxy/device block not found")
replacement='''bool App::InitDx12(HWND output,IDXGIAdapter1* adapter,const Settings&s,Dx12State& d){
    if(!adapter)return false;
    d.adapter=adapter;

    HR(D3D12CreateDevice(d.adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d.device)),"Native D3D12 device");
    gDLSS.BindDevice(d.device.Get());

    ID3D12Device* upgradedDevice=d.device.Get();
    gDLSS.UpgradeInterface(reinterpret_cast<void**>(&upgradedDevice));
    d.proxyDevice.Attach(upgradedDevice);

    HR(CreateDXGIFactory2(0,IID_PPV_ARGS(&d.factory)),"Native DXGI factory");
    IDXGIFactory6* upgradedFactory=d.factory.Get();
    gDLSS.UpgradeInterface(reinterpret_cast<void**>(&upgradedFactory));
    d.proxyFactory.Attach(upgradedFactory);

    D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;q.Priority=D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    HR(d.proxyDevice->CreateCommandQueue(&q,IID_PPV_ARGS(&d.proxyQueue)),"Streamline-hooked command queue creation");
    ID3D12CommandQueue* nativeQueue=nullptr;
    gDLSS.GetNativeInterface(d.proxyQueue.Get(),reinterpret_cast<void**>(&nativeQueue));
    d.queue.Attach(nativeQueue);

    D3D12_DESCRIPTOR_HEAP_DESC rh{};'''
s=s[:m.start()]+replacement+s[m.end():]

old='    gDLSS.TagBackbuffer(d.list.Get(),d.back[bi].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET);\n'
if old not in s: raise SystemExit("explicit backbuffer tag call not found")
s=s.replace(old,'',1)

s=s.replace('RUNNING: WGC1 / 1 buffer / immediate GPU copy / frame released before DLSS / queue 0',
            'RUNNING: WGC1 / safe copy fence / native D3D12 work / Streamline proxy only for hooked Present path',1)

p.write_text(s,encoding="utf-8")
