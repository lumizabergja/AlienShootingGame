#include "wgc_capture.h"
#include <algorithm>
#include <exception>

#if V12_HAS_WGC
using Microsoft::WRL::ComPtr;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
namespace {
std::wstring Failure(const wchar_t* stage, HRESULT hr, const wchar_t* detail) {
    wchar_t code[32]{}; swprintf_s(code,L" (0x%08X): ",static_cast<unsigned>(hr));
    return std::wstring(stage)+code+detail;
}
}
#endif

bool WgcCaptureBackend::Init(HWND target, ID3D11Device* device, ID3D11Fence* copyFence,
                             LARGE_INTEGER frequency, std::wstring& error) {
    Shutdown();
#if !V12_HAS_WGC
    error=L"WGC support was not compiled in."; return false;
#else
    const wchar_t* stage=L"COM apartment";
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        apartmentInitialized_=true;
        target_=target; qpcFrequency_=frequency; copyFence_=copyFence;
        stage=L"WGC support check";
        if(!GraphicsCaptureSession::IsSupported()) { error=L"Windows reports WGC is unavailable."; Shutdown(); return false; }
        stage=L"D3D11 IDXGIDevice interface";
        ComPtr<IDXGIDevice> dxgi;
        winrt::check_hresult(device->QueryInterface(IID_PPV_ARGS(&dxgi)));
        stage=L"CreateDirect3D11DeviceFromDXGIDevice";
        winrt::com_ptr<::IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(),inspectable.put()));
        stage=L"WinRT IDirect3DDevice interface";
        winrtDevice_=inspectable.as<IDirect3DDevice>();
        stage=L"GraphicsCaptureItem interop factory";
        auto factory=winrt::get_activation_factory<GraphicsCaptureItem,IGraphicsCaptureItemInterop>();
        stage=L"CreateForWindow";
        // Request the capture interface explicitly, never a runtime-class IID.
        winrt::check_hresult(factory->CreateForWindow(target,winrt::guid_of<IGraphicsCaptureItem>(),winrt::put_abi(item_)));
        auto size=item_.Size();
        if(size.Width<=0 || size.Height<=0) { error=L"Restore the target window before starting WGC."; Shutdown(); return false; }
        width_=size.Width; height_=size.Height;
        stage=L"CreateFreeThreaded frame pool";
        framePool_=Direct3D11CaptureFramePool::CreateFreeThreaded(winrtDevice_,DirectXPixelFormat::B8G8R8A8UIntNormalized,2,size);
        notification_=std::make_shared<SignalEvent>();
        auto signal=notification_; // Callback owns its event even during shutdown.
        arrivedToken_=framePool_.FrameArrived([signal](auto const&, auto const&) {
            if(signal->handle) SetEvent(signal->handle);
        });
        subscribed_=true;
        stage=L"CreateCaptureSession";
        session_=framePool_.CreateCaptureSession(item_);
        try { session_.IsCursorCaptureEnabled(false); } catch(winrt::hresult_error const&) {}
        stage=L"StartCapture";
        session_.StartCapture(); initialized_=true; return true;
    } catch(winrt::hresult_error const& e) { error=Failure(stage,e.code(),e.message().c_str()); }
      catch(std::exception const&) { error=std::wstring(stage)+L": allocation/initialization failure."; }
    Shutdown(); return false;
#endif
}

bool WgcCaptureBackend::TryGetNextFrame(ComPtr<ID3D11Texture2D>& texture,
    LARGE_INTEGER& timestamp, bool& sizeChanged, std::wstring& error) {
    texture.Reset(); timestamp={}; sizeChanged=false;
#if !V12_HAS_WGC
    error=L"WGC is not compiled in."; return false;
#else
    if(!initialized_) return true;
    try {
        const UINT64 completed=copyFence_->GetCompletedValue();
        if(completed==UINT64_MAX) { error=L"WGC copy device was removed."; return false; }
        for(auto& pending:pending_) {
            if(pending.frame && pending.value<=completed) { pending.frame.Close(); pending.frame=nullptr; }
        }
        FinishFrame();
        heldFrame_=framePool_.TryGetNextFrame();
        if(!heldFrame_) return true;
        // Bounded drain: choose the newest available frame without growing a queue.
        auto newer=framePool_.TryGetNextFrame();
        if(newer) { heldFrame_.Close(); heldFrame_=std::move(newer); }
        auto content=heldFrame_.ContentSize();
        if(content.Width!=(int)width_ || content.Height!=(int)height_) {
            FinishFrame(); sizeChanged=true; return true;
        }
        auto access=heldFrame_.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&texture)));
        timestamp.QuadPart=(LONGLONG)((long double)heldFrame_.SystemRelativeTime().count()*qpcFrequency_.QuadPart/10000000.0L);
        return true;
    } catch(winrt::hresult_error const& e) { error=Failure(L"WGC frame retrieval",e.code(),e.message().c_str()); }
      catch(std::exception const&) { error=L"WGC frame allocation failed."; }
    FinishFrame(); return false;
#endif
}

void WgcCaptureBackend::FinishFrame(UINT64 copyValue) noexcept {
#if V12_HAS_WGC
    if(!heldFrame_) return;
    try {
        if(copyValue) {
            for(auto& pending:pending_) {
                if(!pending.frame) { pending.frame=heldFrame_; pending.value=copyValue; heldFrame_=nullptr; return; }
            }
            // Unreachable for a two-buffer pool; keep the lease rather than recycle early.
            return;
        } else heldFrame_.Close();
        heldFrame_=nullptr;
    } catch(...) { /* A failed Close is retained until shutdown. */ }
#endif
}
void WgcCaptureBackend::WaitForFrame(DWORD timeoutMs) {
#if V12_HAS_WGC
    if(notification_ && notification_->handle) WaitForSingleObject(notification_->handle,timeoutMs);
    else Sleep(timeoutMs);
#endif
}
void WgcCaptureBackend::Shutdown() {
#if V12_HAS_WGC
    try { if(subscribed_ && framePool_) framePool_.FrameArrived(arrivedToken_); } catch(...) {}
    subscribed_=false;
    try { if(session_) session_.Close(); } catch(...) {}
    FinishFrame();
    for(auto& p:pending_) { try { p.frame.Close(); } catch(...) {} }
    for(auto& p:pending_) p.frame=nullptr;
    try { if(framePool_) framePool_.Close(); } catch(...) {}
    session_=nullptr; framePool_=nullptr; item_=nullptr; winrtDevice_=nullptr;
    copyFence_.Reset(); notification_.reset();
    if(apartmentInitialized_) { winrt::uninit_apartment(); apartmentInitialized_=false; }
#endif
    initialized_=false; target_=nullptr; width_=height_=0;
}
