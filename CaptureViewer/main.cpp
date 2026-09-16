#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <string>
using Microsoft::WRL::ComPtr;

static HWND g_hwnd{};
static bool g_fullscreen = true;

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_KEYDOWN && w == VK_ESCAPE) { PostQuitMessage(0); return 0; }
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProc(h,m,w,l);
}

static void Fail(const wchar_t* s, HRESULT hr = S_OK) {
    wchar_t b[512];
    swprintf_s(b, L"%s\nHRESULT: 0x%08X", s, (unsigned)hr);
    MessageBoxW(nullptr,b,L"CaptureViewer",MB_ICONERROR);
}

int WINAPI wWinMain(HINSTANCE hi,HINSTANCE,LPWSTR,int) {
    WNDCLASSEXW wc{sizeof(wc)}; wc.lpfnWndProc=WndProc; wc.hInstance=hi; wc.lpszClassName=L"CaptureViewerWnd"; wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
    RegisterClassExW(&wc);
    int sw=GetSystemMetrics(SM_CXSCREEN), sh=GetSystemMetrics(SM_CYSCREEN);
    g_hwnd=CreateWindowExW(WS_EX_TOPMOST,wc.lpszClassName,L"CaptureViewer - ESC to exit",WS_POPUP,0,0,sw,sh,nullptr,nullptr,hi,nullptr);
    if(!g_hwnd) return 1;

    DXGI_SWAP_CHAIN_DESC sd{}; sd.BufferCount=2; sd.BufferDesc.Width=sw; sd.BufferDesc.Height=sh; sd.BufferDesc.Format=DXGI_FORMAT_B8G8R8A8_UNORM; sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow=g_hwnd; sd.SampleDesc.Count=1; sd.Windowed=TRUE; sd.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx; ComPtr<IDXGISwapChain> sc;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr=D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&sd,&sc,&dev,&fl,&ctx);
    if(FAILED(hr)){Fail(L"D3D11CreateDeviceAndSwapChain failed",hr);return 2;}

    ComPtr<IDXGIDevice> dxdev; dev.As(&dxdev);
    ComPtr<IDXGIAdapter> ad; dxdev->GetAdapter(&ad);
    ComPtr<IDXGIOutput> out; hr=ad->EnumOutputs(0,&out); if(FAILED(hr)){Fail(L"EnumOutputs failed",hr);return 3;}
    ComPtr<IDXGIOutput1> out1; out.As(&out1);
    ComPtr<IDXGIOutputDuplication> dup; hr=out1->DuplicateOutput(dev.Get(),&dup); if(FAILED(hr)){Fail(L"DXGI Desktop Duplication failed. If DXVK is installed beside this EXE, its DXGI implementation may not support DuplicateOutput.",hr);return 4;}

    ShowWindow(g_hwnd,SW_SHOW); SetForegroundWindow(g_hwnd);
    MSG msg{}; bool running=true;
    while(running){
        while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){ if(msg.message==WM_QUIT){running=false;break;} TranslateMessage(&msg);DispatchMessageW(&msg); }
        if(!running) break;
        DXGI_OUTDUPL_FRAME_INFO fi{}; ComPtr<IDXGIResource> res;
        hr=dup->AcquireNextFrame(0,&fi,&res);
        if(hr==DXGI_ERROR_WAIT_TIMEOUT){Sleep(0);continue;}
        if(hr==DXGI_ERROR_ACCESS_LOST){Fail(L"Capture access was lost. Restart CaptureViewer.",hr);break;}
        if(FAILED(hr)){Fail(L"AcquireNextFrame failed",hr);break;}
        ComPtr<ID3D11Texture2D> src; res.As(&src);
        ComPtr<ID3D11Texture2D> back; sc->GetBuffer(0,IID_PPV_ARGS(&back));
        D3D11_TEXTURE2D_DESC a{},b{}; src->GetDesc(&a); back->GetDesc(&b);
        if(a.Width==b.Width && a.Height==b.Height && a.Format==b.Format) ctx->CopyResource(back.Get(),src.Get());
        else {
            UINT w2=(a.Width<b.Width?a.Width:b.Width), h2=(a.Height<b.Height?a.Height:b.Height);
            D3D11_BOX box{0,0,0,w2,h2,1}; ctx->CopySubresourceRegion(back.Get(),0,0,0,0,src.Get(),0,&box);
        }
        dup->ReleaseFrame();
        sc->Present(0,DXGI_PRESENT_ALLOW_TEARING);
    }
    return 0;
}
