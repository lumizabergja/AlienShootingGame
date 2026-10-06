#include "wgc_capture.h"
#include <algorithm>

#if V12_HAS_WGC
using Microsoft::WRL::ComPtr;
using namespace winrt;
using namespace winrt::Windows::Graphics;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

namespace {
template <typename T>
ComPtr<T> GetDxgiInterface(IDirect3DSurface const& surface) {
    auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    ComPtr<T> result;
    winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&result)));
    return result;
}
}
#endif

bool WgcCaptureBackend::Init(HWND target, ID3D11Device* device, LARGE_INTEGER qpcFrequency, std::wstring& error) {
    Shutdown();
#if !V12_HAS_WGC
    (void)target; (void)device; (void)qpcFrequency;
    error = L"Windows Graphics Capture support was not compiled in. Build V12 with MSVC and the Windows 10/11 SDK C++/WinRT headers.";
    return false;
#else
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        target_ = target;
        qpcFrequency_ = qpcFrequency;

        ComPtr<IDXGIDevice> dxgiDevice;
        winrt::check_hresult(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)));
        winrt::com_ptr<::IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.put()));
        winrtDevice_ = inspectable.as<IDirect3DDevice>();

        auto interopFactory = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        winrt::check_hresult(interopFactory->CreateForWindow(target, winrt::guid_of<GraphicsCaptureItem>(),
                                                             winrt::put_abi(item_)));
        auto size = item_.Size();
        width_ = std::max(1, size.Width);
        height_ = std::max(1, size.Height);
        framePool_ = Direct3D11CaptureFramePool::CreateFreeThreaded(
            winrtDevice_, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        session_ = framePool_.CreateCaptureSession(item_);
        if (GraphicsCaptureSession::IsSupported()) {
            try { session_.IsCursorCaptureEnabled(false); } catch (...) {}
        }
        frameEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!frameEvent_) {
            error = L"CreateEventW failed for the WGC FrameArrived wake event.";
            Shutdown();
            return false;
        }
        frameArrivedToken_ = framePool_.FrameArrived([this](auto const&, auto const&) noexcept {
            HANDLE eventHandle = frameEvent_;
            if (eventHandle) SetEvent(eventHandle);
        });
        frameArrivedRegistered_ = true;
        session_.StartCapture();
        initialized_ = true;
        return true;
    } catch (winrt::hresult_error const& e) {
        error = e.message().c_str();
    } catch (...) {
        error = L"Unknown Windows Graphics Capture initialization failure.";
    }
    Shutdown();
    return false;
#endif
}

bool WgcCaptureBackend::WaitForFrame(HANDLE controlEvent) {
#if !V12_HAS_WGC
    (void)controlEvent;
    return false;
#else
    if (!frameEvent_ || !controlEvent) return false;
    HANDLE waits[2]{controlEvent, frameEvent_};
    const DWORD result = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    return result == WAIT_OBJECT_0 + 1;
#endif
}

bool WgcCaptureBackend::TryGetNextFrame(Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture,
                                        LARGE_INTEGER& sourceTimestamp, bool& sizeChanged,
                                        std::wstring& error) {
    texture.Reset();
    sourceTimestamp = {};
    sizeChanged = false;
#if !V12_HAS_WGC
    error = L"WGC is not compiled in.";
    return false;
#else
    if (!initialized_ || !framePool_) return true;
    try {
        // A Direct3D11CaptureFrame owns the lifetime of the surface returned by
        // frame.Surface(). Keep the selected frame checked out until the caller has
        // queued and flushed its D3D11 copy; releasing it here makes the texture
        // reference unsafe as soon as this function returns.
        ReleaseFrame();

        auto newest = framePool_.TryGetNextFrame();
        if (!newest) return true; // no frame ready, not an error

        // The pool has two buffers. If both contain frames, drop the older one and
        // consume the freshest frame. This bounds capture-side queueing without
        // increasing the frame-pool depth or blocking the capture thread.
        for (int i = 0; i < 1; ++i) {
            auto newer = framePool_.TryGetNextFrame();
            if (!newer) break;
            try { newest.Close(); } catch (...) {}
            newest = newer;
        }

        auto content = newest.ContentSize();
        const UINT newWidth = (UINT)std::max(1, content.Width);
        const UINT newHeight = (UINT)std::max(1, content.Height);
        if (newWidth != width_ || newHeight != height_) {
            // WGC window surfaces can legitimately change between the initial DWM
            // bounds and the steady-state client-sized surface. Recreate the pool in
            // place instead of treating that transition as a fatal pipeline resize.
            // The presenter keeps its fixed-size mailbox/DLSS resources; the caller
            // will re-evaluate the crop against the new source surface on the next
            // delivered frame.
            try { newest.Close(); } catch (...) {}
            width_ = newWidth;
            height_ = newHeight;
            framePool_.Recreate(winrtDevice_, DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                2, content);
            sizeChanged = true;
            return true;
        }

        activeFrame_ = newest;
        texture = GetDxgiInterface<ID3D11Texture2D>(activeFrame_.Surface());
        auto relative = activeFrame_.SystemRelativeTime();
        if (qpcFrequency_.QuadPart > 0) {
            // TimeSpan uses 100-ns ticks on the system-relative/QPC time base.
            const auto ticks100ns = relative.count();
            sourceTimestamp.QuadPart = (LONGLONG)((long double)ticks100ns *
                (long double)qpcFrequency_.QuadPart / 10000000.0L);
        } else {
            QueryPerformanceCounter(&sourceTimestamp);
        }
        return true;
    } catch (winrt::hresult_error const& e) {
        error = e.message().c_str();
    } catch (...) {
        error = L"Unknown Windows Graphics Capture frame failure.";
    }
    ReleaseFrame();
    return false;
#endif
}

void WgcCaptureBackend::ReleaseFrame() {
#if V12_HAS_WGC
    if (activeFrame_) {
        try { activeFrame_.Close(); } catch (...) {}
        activeFrame_ = nullptr;
    }
#endif
}

void WgcCaptureBackend::Shutdown() {
#if V12_HAS_WGC
    if (frameArrivedRegistered_ && framePool_) {
        try { framePool_.FrameArrived(frameArrivedToken_); } catch (...) {}
        frameArrivedRegistered_ = false;
    }
    ReleaseFrame();
    try { if (session_) session_.Close(); } catch (...) {}
    try { if (framePool_) framePool_.Close(); } catch (...) {}
    session_ = nullptr;
    framePool_ = nullptr;
    item_ = nullptr;
    winrtDevice_ = nullptr;
    if (frameEvent_) {
        CloseHandle(frameEvent_);
        frameEvent_ = nullptr;
    }
#endif
    initialized_ = false;
    target_ = nullptr;
    width_ = height_ = 0;
}
