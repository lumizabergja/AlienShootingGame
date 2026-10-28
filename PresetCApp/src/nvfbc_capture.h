#pragma once
#include <array>
#include <memory>
#include <string>
#include <cstdint>
#include <windows.h>
#include <d3d9.h>
#include <d3d12.h>
#include <wrl/client.h>
enum class NvFbcGrabResult { Frame, Timeout, Recreate, Resize, Protected, Fatal };
class NvFbcCaptureBackend {
public:
    NvFbcCaptureBackend(); ~NvFbcCaptureBackend();
    NvFbcCaptureBackend(const NvFbcCaptureBackend&)=delete;
    NvFbcCaptureBackend& operator=(const NvFbcCaptureBackend&)=delete;
    bool Init(HWND,DWORD,HMONITOR,const RECT&,const RECT&,UINT,UINT,std::wstring&);
    bool Bind(ID3D12Device*,ID3D12Fence*,ID3D12Resource**,std::wstring&);
    NvFbcGrabResult Grab(UINT,LARGE_INTEGER&,std::wstring&);
    bool Signal(uint64_t,std::wstring&);
    bool RecreateSession(std::wstring&);
    UINT Pitch() const;
    UINT Width() const;
    UINT Height() const;
    void Diagnostic(const std::wstring&) const;
    void Shutdown();
private:
    struct CudaState; std::unique_ptr<CudaState> cuda_;
    bool LoadApi(std::wstring&),EnsureEnabled(std::wstring&),CreateSession(std::wstring&);
    bool ReadStatus(bool&,bool&,std::wstring&);
    std::wstring ResultText(int) const;
    HMODULE module_{};
    using GetSdkVersionFn=int(__stdcall *)(unsigned long*);
    using GetStatusFn=int(__stdcall *)(void*);
    using CreateFn=int(__stdcall *)(void*);
    using EnableFn=int(__stdcall *)(int);
    GetSdkVersionFn getSdkVersion_{};GetStatusFn getStatus_{};CreateFn create_{};EnableFn enable_{};
    Microsoft::WRL::ComPtr<IDirect3D9Ex> d3d9_;
    Microsoft::WRL::ComPtr<IDirect3DDevice9Ex> device9_;
    UINT adapterOrdinal_{},sdkVersion_{},sourceWidth_{},sourceHeight_{},width_{},height_{},cropX_{},cropY_{};
    bool acceptNextDimensions_{};
    DWORD targetPid_{},lastSourcePid_{}; int pidCandidate_{};
};
