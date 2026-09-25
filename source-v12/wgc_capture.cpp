#include "wgc_capture.h"
#include <algorithm>

#if V12_HAS_WGC
using Microsoft::WRL::ComPtr;
using namespace winrt;
using namespace Windows::Graphics;
using namespace Windows::Graphics::Capture;
using namespace Windows::Graphics::DirectX;
using namespace Windows::Graphics::DirectX::Direct3D11;

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
        auto frame = framePool_.TryGetNextFrame();
        if (!frame) return true; // no frame ready, not an error

        auto content = frame.ContentSize();
        if ((UINT)std::max(1, content.Width) != width_ || (UINT)std::max(1, content.Height) != height_) {
            width_ = std::max(1, content.Width);
            height_ = std::max(1, content.Height);
            framePool_.Recreate(winrtDevice_, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, content);
            sizeChanged = true;
        }

        texture = GetDxgiInterface<ID3D11Texture2D>(frame.Surface());
        auto relative = frame.SystemRelativeTime();
        if (qpcFrequency_.QuadPart > 0) {
            // TimeSpan is 100-ns units on the same system-relative time base.
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
    return false;
#endif
}

void WgcCaptureBackend::Shutdown() {
#if V12_HAS_WGC
    try { if (session_) session_.Close(); } catch (...) {}
    try { if (framePool_) framePool_.Close(); } catch (...) {}
    session_ = nullptr;
    framePool_ = nullptr;
    item_ = nullptr;
    winrtDevice_ = nullptr;
#endif
    initialized_ = false;
    target_ = nullptr;
    width_ = height_ = 0;
}
