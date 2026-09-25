#pragma once
#include <windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>
#include <string>
#include <array>
#include <memory>

#if __has_include(<winrt/base.h>) && __has_include(<winrt/Windows.Graphics.Capture.h>) && \
    __has_include(<windows.graphics.capture.interop.h>) && __has_include(<windows.graphics.directx.direct3d11.interop.h>)
#define V12_HAS_WGC 1
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#else
#define V12_HAS_WGC 0
#endif

class WgcCaptureBackend {
public:
    bool Init(HWND target, ID3D11Device* device, ID3D11Fence* copyFence, LARGE_INTEGER qpcFrequency, std::wstring& error);
    bool TryGetNextFrame(Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture,
                         LARGE_INTEGER& sourceTimestamp, bool& sizeChanged,
                         std::wstring& error);
    void FinishFrame(UINT64 copyValue = 0) noexcept;
    void WaitForFrame(DWORD timeoutMs);
    void Shutdown();
    bool Available() const { return initialized_; }
    bool CompiledIn() const { return V12_HAS_WGC != 0; }
    UINT Width() const { return width_; }
    UINT Height() const { return height_; }
private:
#if V12_HAS_WGC
    struct SignalEvent { HANDLE handle{CreateEventW(nullptr,FALSE,FALSE,nullptr)}; ~SignalEvent(){if(handle)CloseHandle(handle);} };
    std::shared_ptr<SignalEvent> notification_;
    winrt::event_token arrivedToken_{};
    bool subscribed_{};
    bool apartmentInitialized_{};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame heldFrame_{nullptr};
    struct PendingFrame { winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame frame{nullptr}; UINT64 value{}; };
    std::array<PendingFrame,2> pending_{};
    Microsoft::WRL::ComPtr<ID3D11Fence> copyFence_;
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item_{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool framePool_{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session_{nullptr};
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice winrtDevice_{nullptr};
#endif
    HWND target_{};
    LARGE_INTEGER qpcFrequency_{};
    UINT width_{};
    UINT height_{};
    bool initialized_{false};
};
