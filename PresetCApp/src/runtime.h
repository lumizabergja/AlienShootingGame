#pragma once
#include "common.h"
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>

struct FrameGenerationState {
    sl::DLSSGState state{};
    std::array<ID3D12Fence *, 4> fences{};
    std::array<uint64_t, 4> values{};
    UINT fenceCount{};
};

class FrameGeneration {
    HMODULE dll{};
    bool initialized{};
    UINT index{}, width{}, height{};
    bool enabled{};
    UINT viewportCount{1};
    sl::DLSSGOptions activeOptions{};
    sl::Constants baseConstants{};
    std::array<sl::Extent, 4> inputExtents{};
    std::array<sl::ViewportHandle, 4> viewports{
        sl::ViewportHandle{0}, sl::ViewportHandle{1}, sl::ViewportHandle{2}, sl::ViewportHandle{3}};
    sl::FrameToken *token{};
    PFun_slInit *initFn{};
    PFun_slShutdown *shutdownFn{};
    PFun_slSetD3DDevice *deviceFn{};
    PFun_slGetFeatureFunction *featureFn{};
    PFun_slIsFeatureSupported *supportedFn{};
    PFun_slGetFeatureRequirements *requirementsFn{};
    PFun_slGetNewFrameToken *tokenFn{};
    PFun_slSetConstants *constantsFn{};
    PFun_slSetTagForFrame *tagsFn{};
    PFun_slDLSSGSetOptions *optionsFn{};
    PFun_slDLSSGGetState *stateFn{};
    PFun_slReflexSetOptions *reflexFn{};
    PFun_slReflexSleep *sleepFn{};
    PFun_slPCLSetMarker *markerFn{};

    template <class T> void load(T &f, const char *n) {
        f = (T)GetProcAddress(dll, n);
        if (!f)
            throw std::runtime_error(std::string("Missing runtime export: ") + n);
    }
    template <class T> void feature(T &f, sl::Feature id, const char *n) {
        void *p{};
        ck(featureFn(id, n, p), n);
        f = (T)p;
        if (!f)
            throw std::runtime_error(n);
    }
    static void ck(sl::Result r, const char *n) {
        if (r != sl::Result::eOk)
            throw std::runtime_error(std::string(n) + ": Streamline " + std::to_string(int(r)));
    }

  public:
    using Factory = HRESULT(WINAPI *)(REFIID, void **);
    using Device = HRESULT(WINAPI *)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
    Factory createFactory{};
    Device createDevice{};
    void load(bool diagnostics);
    void bind(ID3D12Device *d) { ck(deviceFn(d), "Set Streamline device"); }
    void configure(IDXGIAdapter1 *, UINT, UINT, UINT, bool, UINT centerBypassPx);
    void pause(bool);
    void begin();
    void constants(bool reset);
    void tag(ID3D12GraphicsCommandList *, ID3D12Resource *, ID3D12Resource *, ID3D12Resource *,
             ID3D12Resource *uiColorAndAlpha = nullptr);
    void marker(sl::PCLMarker m) {
        if (enabled)
            ck(markerFn(m, *token), "Reflex marker");
    }
    FrameGenerationState afterPresent();
    void shutdown();
    ~FrameGeneration() {
        shutdown();
        if (dll)
            FreeLibrary(dll);
    }
};
