#include "engine.h"
#include "diagnostics.h"
#include "gpu_profile.h"
#include "focus.h"
#include "mailbox.h"
#include "nvfbc_capture.h"
#include "nvofa_motion.h"
#include "resolve.h"
#include "runtime.h"
#include "telemetry.h"
#include "raw_color.h"
#include <memory>
#include <sstream>
#include <iomanip>
namespace {
struct Frame {
    Commands color;
    uint64_t done{};
};
struct Engine {
    HWND target, output;
    Options options;
    SessionControl &control;
    FrameGeneration fg;
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_OUTPUT_DESC display{};
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain3> swap;
    RawColor rawDraw;
    ComPtr<ID3D12DescriptorHeap> backRtv;
    UINT rtvStride{};
    // Opaque native center square supplied to DLSS-G UI recomposition.  It is a
    // single full-size texture that stays transparent outside the requested square.
    ComPtr<ID3D12Resource> centerUi;
    ComPtr<ID3D12DescriptorHeap> centerUiRtvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE centerUiRtv{};
    std::array<ComPtr<ID3D12Fence>, 4> centerUiInputFence;
    std::array<uint64_t, 4> centerUiInputValue{};
    UINT centerUiInputCount{};
    std::array<ComPtr<ID3D12Resource>, CaptureCount> shared12;
    std::array<ComPtr<ID3D12Resource>, 2> back;
    ComPtr<ID3D12Fence> captured, release, finished;
    std::atomic<uint64_t> nextCapture{0};
    uint64_t nextRelease = 0, nextFinished = 0;
    std::array<Frame, FrameCount> frames;
    LatestMailbox<CaptureCount> mailbox;
    NvFbcCaptureBackend capture;
    std::unique_ptr<MotionResolve> resolve;
    std::unique_ptr<NvOfMotion> opticalFlow;
    std::unique_ptr<LocalFocus> focus;
    Event readyEvent, fenceEvent, producerWake;
    HANDLE latencyWait{};
    bool nativePermit{};
    bool nativeCopy{};
    std::array<std::array<ComPtr<ID3D12Fence>, 4>, 2> backInputFence;
    std::array<std::array<uint64_t, 4>, 2> backInputValue{};
    std::array<UINT, 2> backInputCount{};
    void gpuWait(ID3D12Fence *f, uint64_t value, const char *operation) {
        if (!f || !value) return;
        auto completed = f->GetCompletedValue();
        if (completed == UINT64_MAX)
            throw std::runtime_error("GPU removed during input reuse");
        if (completed < value)
            check(queue->Wait(f, value), operation);
    }
    std::atomic<bool> pauseCapture{false}, producerPaused{false};
    std::thread producer;
    std::atomic<bool> producerFailed{false};
    std::mutex errorMutex;
    std::string producerError;
    UINT width{}, height{};
    bool tearing{};
    uint64_t epoch = 1;
    std::unique_ptr<CsvRecorder> telemetry;
    GpuProfiler profiler;
    Engine(HWND t, HWND o, const Options &opt, SessionControl &c)
        : target(t), output(o), options(opt), control(c) {
        // Minimum-latency default: keep the logging thread completely out of the
        // process unless diagnostics were explicitly requested.
        if (options.gpuProfile)
            telemetry = std::make_unique<CsvRecorder>();
    }
    void init();
    void makeSwap();
    void initCenterUi();
    void captureLoop();
    void loop();
    void drain();
    ~Engine() {
        control.stop = true;
        SetEvent(readyEvent.h);
        SetEvent(producerWake.h);
        if (producer.joinable())
            producer.join();
        try {
            drain();
        } catch (...) {
        }
        if (focus)
            focus->restore();
        opticalFlow.reset();
        capture.Shutdown();
        fg.shutdown();
        if (latencyWait)
            CloseHandle(latencyWait);
    }
};
void Engine::init() {
    ComPtr<IDXGIFactory1> rawFactory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&rawFactory)), "Capture factory");
    HMONITOR monitor = MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST);
    for (UINT ai = 0;; ++ai) {
        ComPtr<IDXGIAdapter1> a;
        if (rawFactory->EnumAdapters1(ai, &a) == DXGI_ERROR_NOT_FOUND)
            break;
        for (UINT oi = 0;; ++oi) {
            ComPtr<IDXGIOutput> out;
            if (a->EnumOutputs(oi, &out) == DXGI_ERROR_NOT_FOUND)
                break;
            DXGI_OUTPUT_DESC desc{};
            check(out->GetDesc(&desc), "Output descriptor");
            if (desc.Monitor == monitor) {
                adapter = a;
                display = desc;
                break;
            }
        }
        if (adapter)
            break;
    }
    if (!adapter)
        throw std::runtime_error("Cannot find selected game's display adapter");
    DXGI_ADAPTER_DESC1 ad{};
    check(adapter->GetDesc1(&ad), "Adapter descriptor");
    if (ad.VendorId != 0x10de)
        throw std::runtime_error("The selected display must use the NVIDIA adapter");
    width = display.DesktopCoordinates.right - display.DesktopCoordinates.left;
    height = display.DesktopCoordinates.bottom - display.DesktopCoordinates.top;
    if (!width || !height)
        throw std::runtime_error("Invalid display size");
    DWORD pid=0;GetWindowThreadProcessId(target,&pid);
    std::wstring error;
    if(!capture.Init(output,pid,monitor,display.DesktopCoordinates,display.DesktopCoordinates,width,height,error))
        throw std::runtime_error(std::string(error.begin(),error.end()));
    // NvFBC PID capture can report a game resolution that differs from the monitor
    // desktop rectangle.  Build every size-dependent GPU resource from the actual
    // captured frame dimensions, not the desktop-mode dimensions.
    width = capture.Width();
    height = capture.Height();
    diagnostics::write("NvFBC captured source resolution=" + std::to_string(width) + "x" +
                       std::to_string(height));
    if (!width || !height)
        throw std::runtime_error("NvFBC returned an invalid capture size");
    if (options.multiplier > 1) {
        fg.load(options.gpuProfile);
        focus = std::make_unique<LocalFocus>(target, output);
        focus->install();
        check(fg.createFactory(IID_PPV_ARGS(&factory)), "Streamline factory");
        check(fg.createDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12 device");
        fg.bind(device.Get());
    } else {
        // A true native baseline: no DLSS plugin or second presentation pacer.
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Native factory");
        check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "Native D3D12 device");
    }
    createLowLatencyQueue(device.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, queue, "Presentation queue");
    check(device->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&captured)),"CUDA capture fence");
    ID3D12Resource *raw[CaptureCount]{};
    bool bound=capture.Bind(device.Get(),captured.Get(),raw,error);
    for(UINT i=0;i<CaptureCount;++i)shared12[i].Attach(raw[i]);
    if(!bound)throw std::runtime_error(std::string(error.begin(),error.end()));
    LARGE_INTEGER acquired{};
    auto preflight = capture.Grab(0,acquired,error);
    if (preflight == NvFbcGrabResult::Resize) {
        control.resolutionChanged.store(true, std::memory_order_release);
        control.stop = true;
        throw std::runtime_error("Capture resolution changed.");
    }
    if(preflight!=NvFbcGrabResult::Frame || !capture.Signal(1,error))
        throw std::runtime_error("CUDA shared-buffer preflight: "+std::string(error.begin(),error.end()));
    nextCapture=1;
    waitFence(captured.Get(),1,fenceEvent.h);
    capture.Diagnostic(L"CUDA SHARED CAPTURE VALIDATED: selected PID, pitch, and D3D12 ready fence passed.");
    nativeCopy = capture.Pitch() >= width * 4 &&
                 (capture.Pitch() % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) == 0;
    diagnostics::write(std::string("NvFBC direct shared allocation: ") +
                       (nativeCopy ? "native D3D12 copy path" : "raw draw fallback"));
    if (!nativeCopy)
        rawDraw.init(device.Get());
    if(options.gpuProfile)profiler.init(device.Get(),queue.Get());
    for(auto &f:frames) f.color.init(device.Get(),D3D12_COMMAND_LIST_TYPE_DIRECT);
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&release)), "Source release fence");
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&finished)), "Frame completion fence");
    // Center bypass is implemented as a literal hole in the FG output window.
    // Do not feed the center to DLSS-G UI recomposition; it remains the native game below.
    fg.configure(adapter.Get(), width, height, options.multiplier, options.vsync, 0);
    if (focus)
        focus->install();
    if (options.multiplier > 1) {
        // Preserve flat depth and immutable zero-motion as the fallback baseline.
        resolve = std::make_unique<MotionResolve>();
        resolve->initFlat(device.Get(), width, height, queue.Get());
        if (options.opticalFlow && !nativeCopy) {
            diagnostics::write("NVOFA 4x4 FAST unavailable for unaligned NvFBC row pitch; "
                               "using zero-motion fallback");
        } else if (options.opticalFlow) {
            try {
                opticalFlow = std::make_unique<NvOfMotion>();
                opticalFlow->init(device.Get(), width, height, options.opticalFlowPercent, *resolve);
            } catch (const std::exception &e) {
                diagnostics::write(std::string("NVOFA INIT FAILED; zero-motion fallback: ") + e.what());
                opticalFlow.reset();
            }
        } else {
            diagnostics::write("NVOFA disabled by checkbox; zero-motion baseline");
        }
    }
    makeSwap();
    producer = std::thread([this] { captureLoop(); });
}
void Engine::initCenterUi() {
    if (options.multiplier <= 1 || options.centerRadius == 0)
        return;
    const UINT cx = width / 2, cy = height / 2;
    if (options.centerRadius >= cx || options.centerRadius >= cy)
        throw std::runtime_error("Center square is too large for the output resolution");

    centerUi = texture(device.Get(), width, height, DXGI_FORMAT_B8G8R8A8_UNORM,
                       D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 1;
    check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&centerUiRtvHeap)),
          "Center UI RTV heap");
    centerUiRtv = centerUiRtvHeap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(centerUi.Get(), nullptr, centerUiRtv);

    // Default-heap contents are undefined.  Clear once; subsequent frame updates are
    // scissored to the exact square so every pixel outside it remains transparent.
    Commands init;
    init.init(device.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT);
    init.begin();
    const float transparent[4] = {0.f, 0.f, 0.f, 0.f};
    init.l->ClearRenderTargetView(centerUiRtv, transparent, 0, nullptr);
    barrier(init.l.Get(), centerUi.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    init.submit(queue.Get());
    ComPtr<ID3D12Fence> f;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f)),
          "Center UI init fence");
    check(queue->Signal(f.Get(), 1), "Center UI init signal");
    waitFence(f.Get(), 1, fenceEvent.h);
}

void Engine::makeSwap() {
    nativePermit = false;
    for (auto &perBuffer : backInputFence)
        for (auto &f : perBuffer)
            f.Reset();
    for (auto &perBuffer : backInputValue)
        perBuffer.fill(0);
    backInputCount.fill(0);
    for (auto &f : centerUiInputFence) f.Reset();
    centerUiInputValue.fill(0);
    centerUiInputCount = 0;
    if (latencyWait) {
        CloseHandle(latencyWait);
        latencyWait = nullptr;
    }
    for (auto &b : back)
        b.Reset();
    swap.Reset();
    ComPtr<IDXGIFactory5> f5;
    BOOL supported = FALSE;
    if (SUCCEEDED(factory.As(&f5)))
        f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &supported, sizeof supported);
    tearing = supported;
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = width;
    sd.Height = height;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    if (options.multiplier == 1)
        sd.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    ComPtr<IDXGISwapChain1> s;
    check(factory->CreateSwapChainForHwnd(queue.Get(), output, &sd, nullptr, nullptr, &s),
          "Output swapchain");
    check(s.As(&swap), "Swapchain interface");
    if (options.multiplier == 1) {
        check(swap->SetMaximumFrameLatency(1), "Native maximum frame latency");
        latencyWait = swap->GetFrameLatencyWaitableObject();
        if (!latencyWait)
            throw std::runtime_error("Native swapchain pacing handle unavailable");
    }
    factory->MakeWindowAssociation(output, DXGI_MWA_NO_ALT_ENTER);
    backRtv.Reset();
    D3D12_CPU_DESCRIPTOR_HANDLE rh{};
    if (!nativeCopy) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=2;
        check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&backRtv)),"Backbuffer RTVs");
        rtvStride=device->GetDescriptorHandleIncrementSize(hd.Type);
        rh=backRtv->GetCPUDescriptorHandleForHeapStart();
    }
    for(UINT i=0;i<2;++i){
        check(swap->GetBuffer(i,IID_PPV_ARGS(&back[i])),"Backbuffer");
        if (!nativeCopy) {
            device->CreateRenderTargetView(back[i].Get(),nullptr,rh);
            rh.ptr+=rtvStride;
        }
    }
    if (focus)
        focus->install();
}
void Engine::captureLoop() {
    try {
        MmcssScope mmcss;
        using Clock = std::chrono::steady_clock;
        const Clock::duration samplePeriod = options.captureFps > 0.0
            ? std::chrono::duration_cast<Clock::duration>(
                  std::chrono::duration<double>(1.0 / options.captureFps))
            : Clock::duration::zero();
        Clock::time_point nextSample{};
        uint64_t seq = 0;
        bool wasPaused = false;
        while (!control.stop) {
            if (pauseCapture || !control.focused.load(std::memory_order_relaxed)) {
                producerPaused.store(true, std::memory_order_release);
                wasPaused = true;
                WaitForSingleObject(producerWake.h, 10);
                continue;
            }
            producerPaused.store(false, std::memory_order_release);
            // Recheck after publishing the acknowledgement transition.
            if (pauseCapture)
                continue;
            if (wasPaused) {
                ++epoch;
                wasPaused = false;
                nextSample = Clock::time_point{}; // first frame after refocus is immediate
            }
            // Rate-limit source grabs without adding a queue to the presenter.
            // Always take a fresh NvFBC frame when due; never duplicate an older grab.
            if (samplePeriod != Clock::duration::zero() &&
                nextSample.time_since_epoch().count() != 0) {
                const auto now = Clock::now();
                if (now < nextSample) {
                    const auto remainingUs =
                        std::chrono::duration_cast<std::chrono::microseconds>(nextSample - now).count();
                    if (remainingUs > 1500) Sleep(1);
                    else if (remainingUs > 100) SwitchToThread();
                    else YieldProcessor();
                    continue;
                }
            }
            auto captureDone=captured->GetCompletedValue(),releaseDone=release->GetCompletedValue();
            if(captureDone==UINT64_MAX || releaseDone==UINT64_MAX)throw std::runtime_error("GPU removed during capture");
            int slot=mailbox.reserve([&](unsigned,const auto &s){return captureDone>=s.ready && releaseDone>=s.release;});
            if(slot<0){WaitForSingleObject(producerWake.h,INFINITE);continue;}
            std::wstring error; LARGE_INTEGER time{};
            auto result=capture.Grab(UINT(slot),time,error);
            if(result!=NvFbcGrabResult::Frame) {
                mailbox.cancel(slot);
                if(control.stop)break;
                if(result==NvFbcGrabResult::Timeout)continue;
                if(result==NvFbcGrabResult::Recreate) {
                    if(!capture.RecreateSession(error))throw std::runtime_error(std::string(error.begin(),error.end()));
                    ++epoch;continue;
                }
                if(result==NvFbcGrabResult::Resize) {
                    control.resolutionChanged.store(true, std::memory_order_release);
                    control.hasFrame = false;
                    control.stop = true;
                    break;
                }
                throw std::runtime_error(std::string(error.begin(),error.end()));
            }
            if (samplePeriod != Clock::duration::zero()) {
                const auto now = Clock::now();
                if (nextSample.time_since_epoch().count() == 0) {
                    nextSample = now + samplePeriod;
                } else {
                    // Preserve the average requested rate even if a real game frame
                    // arrives later than its sampling deadline.
                    do { nextSample += samplePeriod; } while (nextSample <= now);
                }
            }
            // Commit the fence counter only after CUDA accepted its signal.
            auto ready=nextCapture.load()+1;
            if(!capture.Signal(ready,error)){mailbox.cancel(slot);throw std::runtime_error(std::string(error.begin(),error.end()));}
            // Wake the producer exactly when a CUDA-written slot becomes GPU-readable.
            // This removes the former 1 ms reuse poll without busy-spinning.
            check(captured->SetEventOnCompletion(ready, producerWake.h), "Capture wake fence");
            nextCapture=ready;
            ++seq;
            mailbox.publish(slot, seq, ready, epoch, time.QuadPart);
            SetEvent(readyEvent.h);
        }
    } catch (const std::exception &e) {
        if (!control.stop) {
            std::lock_guard<std::mutex> l(errorMutex);
            producerError = e.what();
            producerFailed = true;
            SetEvent(readyEvent.h);
        }
    }
}
void Engine::drain() {
    if (captured)
        waitFence(captured.Get(), nextCapture, fenceEvent.h);
    if (queue && finished) {
        check(queue->Signal(finished.Get(), ++nextFinished), "Drain queue");
        waitFence(finished.Get(), nextFinished, fenceEvent.h);
    }
    for (UINT bi = 0; bi < backInputFence.size(); ++bi)
        for (UINT fi = 0; fi < backInputCount[bi]; ++fi)
            if (backInputFence[bi][fi])
                waitFence(backInputFence[bi][fi].Get(), backInputValue[bi][fi], fenceEvent.h);
    for (UINT fi = 0; fi < centerUiInputCount; ++fi)
        if (centerUiInputFence[fi])
            waitFence(centerUiInputFence[fi].Get(), centerUiInputValue[fi], fenceEvent.h);
}
void Engine::loop() {
    int previous = -1;
    uint64_t previousEpoch = 0, count = 0;
    int64_t lastPresented = 0, lastReport = ticks();
    bool paused = false;
    double ageSum = 0, submitSum = 0, presentSum = 0;
    double ofaCpuSum = 0;
    uint64_t ofaSubmissions = 0;
    uint64_t samples = 0, presents = 0;
    while (!control.stop && IsWindow(target)) {
        if (producerFailed) {
            std::lock_guard<std::mutex> l(errorMutex);
            throw std::runtime_error(producerError);
        }
        if (!control.focused.load(std::memory_order_relaxed)) {
            if (!paused) {
                control.hasFrame = false;
                pauseCapture = true;
                // Require a fresh acknowledgement, not one from an earlier focus loss.
                producerPaused = false;
                SetEvent(producerWake.h);
                // Stop publication before draining fences and clearing slot ownership.
                auto deadline = GetTickCount64() + 5000;
                while (!producerPaused.load(std::memory_order_acquire) && !producerFailed && !control.stop) {
                    if (GetTickCount64() > deadline)
                        throw std::runtime_error("Capture pause acknowledgement timed out");
                    WaitForSingleObject(readyEvent.h, 1);
                }
                if (control.stop)
                    break;
                drain();
                fg.pause(true);
                mailbox.clear();
                previous = -1;
                paused = true;
            }
            WaitForSingleObject(readyEvent.h, 10);
            continue;
        }
        if (paused) {
            mailbox.clear();
            makeSwap(); // Recreate while DLSS-G is still paused.
            fg.pause(false);
            if (opticalFlow) opticalFlow->resetHistory();
            pauseCapture = false;
            SetEvent(producerWake.h);
            paused = false;
            lastPresented = 0;
        }
        if (!mailbox.available()) {
            WaitForSingleObject(readyEvent.h, 10);
            continue;
        }
        auto ci = UINT(count % FrameCount);
        auto &frame = frames[ci];

        // Start Reflex pacing before the strict previous-base fence wait. The GPU keeps
        // retiring the prior base while the CPU is in slReflexSleep, turning two serial
        // waits into an overlap. The newest capture is still latched only after both are done.
        if (options.multiplier > 1)
            fg.begin();

        // LL3 strict base-depth gate: never select a capture while an earlier real/base
        // submission is still pending on the presentation queue. Capture continues on
        // its own thread while we wait, so mailbox.take() below still latches the newest
        // completed NvFBC frame rather than aging a frame that was selected too early.
        // This keeps at most one real/base workload in flight without adding a queue.
        if (options.multiplier > 1 && count)
            waitFence(finished.Get(), nextFinished, fenceEvent.h, &control.stop, 2048);

        // Frame-context recycling remains as a safety invariant. With the LL3 gate above
        // this should normally already be complete, but retaining it preserves the
        // proven three-context Streamline ownership rules.
        waitFence(finished.Get(), frame.done, fenceEvent.h, &control.stop);
        GpuSample gpuSample{};
        if (telemetry)
            gpuSample = profiler.read(ci);
        // All potentially long pacing/recycle waits precede the newest-frame selection.
        if (options.multiplier == 1 && count)
            waitFence(finished.Get(), nextFinished, fenceEvent.h, &control.stop);
        if (latencyWait && !nativePermit) {
            while (!control.stop && control.focused.load(std::memory_order_relaxed)) {
                DWORD result = WaitForSingleObject(latencyWait, 10);
                if (result == WAIT_OBJECT_0) {
                    nativePermit = true;
                    break;
                }
                if (result != WAIT_TIMEOUT)
                    throw std::runtime_error("Native presentation pacing failed");
            }
        }
        if (control.stop || !control.focused.load(std::memory_order_relaxed))
            continue;

        // Do every CPU/API operation that does not depend on the captured slot before
        // latching a frame.  The NvFBC producer keeps running during this setup, so the
        // capture selected below is measurably newer than when these calls followed take().
        UINT bi = swap->GetCurrentBackBufferIndex();
        auto buffer = back[bi].Get();
        frame.color.begin();
        fg.marker(sl::PCLMarker::eSimulationStart);
        fg.marker(sl::PCLMarker::eSimulationEnd);
        fg.marker(sl::PCLMarker::eRenderSubmitStart);
        // Track the actual backbuffer separately from the immutable desktop-FG input contexts.
        for (UINT fi = 0; fi < backInputCount[bi]; ++fi)
            gpuWait(backInputFence[bi][fi].Get(), backInputValue[bi][fi], "Backbuffer input reuse");
        for (UINT fi = 0; fi < centerUiInputCount; ++fi)
            gpuWait(centerUiInputFence[fi].Get(), centerUiInputValue[fi], "Center square input reuse");
        centerUiInputCount = 0;

        LatestMailbox<CaptureCount>::Slot selected{};
        int current = mailbox.take(&selected);
        if (current < 0) {
            // The producer may reclaim the only Ready slot between available and take.
            check(frame.color.l->Close(), "Close unused color list");
            continue;
        }

        // Setting Streamline constants is CPU work and does not consume the capture
        // allocation.  Let NvFBC continue publishing while we do it, then make one
        // final atomic latest-frame latch immediately before recording the copy.
        auto latchTime = ticks();
        bool reset = previous < 0 || previousEpoch != selected.epoch || !lastPresented ||
                     milliseconds(latchTime - lastPresented) > 100;
        fg.constants(reset);
        const uint64_t constantsEpoch = selected.epoch;
        if (mailbox.retakeLatest(current, &selected)) {
            latchTime = ticks();
            const bool latestReset = previous < 0 || previousEpoch != selected.epoch || !lastPresented ||
                                     milliseconds(latchTime - lastPresented) > 100;
            // A capture-session epoch transition is rare.  If one landed during
            // slSetConstants, update reset for the same frame token before tagging.
            if (latestReset != reset || selected.epoch != constantsEpoch) {
                reset = latestReset;
                fg.constants(reset);
            } else {
                reset = latestReset;
            }
        }
        auto selectedAt = ticks();
        gpuWait(captured.Get(), selected.ready, "Color waits for capture");
        profiler.mark(frame.color.l.Get(), ci, 0);
        if (nativeCopy) {
            // The NvFBC CUDA buffer is already native-size BGRA with a D3D12-valid row pitch.
            // Copy it directly into the swapchain image instead of sampling every pixel
            // through a fullscreen shader. Buffers decay back to COMMON after execution.
            barrier(frame.color.l.Get(),buffer,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = buffer;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = shared12[current].Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Offset = 0;
            src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            src.PlacedFootprint.Footprint.Width = width;
            src.PlacedFootprint.Footprint.Height = height;
            src.PlacedFootprint.Footprint.Depth = 1;
            src.PlacedFootprint.Footprint.RowPitch = capture.Pitch();
            frame.color.l->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
            barrier(frame.color.l.Get(),buffer,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PRESENT);
        } else {
            barrier(frame.color.l.Get(),buffer,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);
            auto rh=backRtv->GetCPUDescriptorHandleForHeapStart();rh.ptr+=bi*rtvStride;
            rawDraw.draw(frame.color.l.Get(),shared12[current].Get(),rh,width,height,capture.Pitch(),width,height);
            barrier(frame.color.l.Get(),buffer,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);
        }
        if (centerUi) {
            const LONG r = LONG(options.centerRadius);
            const LONG cx = LONG(width / 2), cy = LONG(height / 2);
            barrier(frame.color.l.Get(), centerUi.Get(),
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_RENDER_TARGET);
            rawDraw.drawRegion(frame.color.l.Get(), shared12[current].Get(), centerUiRtv,
                               width, height, capture.Pitch(), width, height,
                               cx - r, cy - r, cx + r, cy + r);
            barrier(frame.color.l.Get(), centerUi.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        if (opticalFlow) {
            if (reset) opticalFlow->resetHistory();
            opticalFlow->upload(frame.color.l.Get(), shared12[current].Get(),
                                capture.Pitch(), unsigned(count));
        }
        profiler.mark(frame.color.l.Get(), ci, 1);
        if (options.multiplier > 1 && !opticalFlow)
            fg.tag(frame.color.l.Get(), resolve->motion[ci].Get(), resolve->depth.Get(), buffer,
                   centerUi.Get());
        profiler.finish(frame.color.l.Get(), ci, count + 1, false);
        frame.color.submit(queue.Get());
        if (opticalFlow) {
            const auto beganOFA = ticks();
            const bool hasVectors = opticalFlow->execute(queue.Get(), unsigned(count));
            ofaCpuSum += milliseconds(ticks() - beganOFA);
            if (hasVectors) ++ofaSubmissions;
            auto tag = [&](ID3D12GraphicsCommandList *l) {
                fg.tag(l, resolve->motion[ci].Get(), resolve->depth.Get(), buffer,
                       centerUi.Get());
            };
            if (hasVectors)
                opticalFlow->convertAndTag(queue.Get(), ci, *resolve, tag);
            else
                opticalFlow->tagWithoutFlow(queue.Get(), ci, *resolve, tag);
        }
        auto submittedAt = ticks();
        // Capture slots no longer depend on FG: only the output backbuffer, motion and
        // immutable flat depth are tagged. Release their color inputs before Present.
        uint64_t sourceRelease = ++nextRelease;
        check(queue->Signal(release.Get(), sourceRelease), "Capture source release");
        check(release->SetEventOnCompletion(sourceRelease, producerWake.h), "Capture release wake");
        mailbox.commit(current, previous, sourceRelease);
        // commit() can free a history slot before its release fence fires; wake now so
        // the producer can re-evaluate ownership, then the fence wake handles readiness.
        SetEvent(producerWake.h);
        previous = current;
        previousEpoch = selected.epoch;
        fg.marker(sl::PCLMarker::eRenderSubmitEnd);
        fg.marker(sl::PCLMarker::ePresentStart);
        auto presentBegin = ticks();
        HRESULT hr = swap->Present(options.vsync ? 1 : 0,
                                   (!options.vsync && tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0);
        auto presentEnd = ticks();
        check(hr, "Present");
        // LL4: queue the strict-base-depth completion signal immediately after
        // Present returns. The Streamline state query below is CPU work and no
        // longer delays when the next base frame is allowed to latch.
        check(queue->Signal(finished.Get(), ++nextFinished), "Frame completion");
        frame.done = nextFinished;
        nativePermit = false;
        fg.marker(sl::PCLMarker::ePresentEnd);
        auto fgState = fg.afterPresent();
        auto state = fgState.state;
        if (options.multiplier == 1)
            state.numFramesActuallyPresented = 1;
        backInputCount[bi] = fgState.fenceCount;
        for (UINT fi = 0; fi < 4; ++fi) {
            if (fi < fgState.fenceCount) {
                backInputFence[bi][fi] = fgState.fences[fi];
                backInputValue[bi][fi] = fgState.values[fi];
            } else {
                backInputFence[bi][fi].Reset();
                backInputValue[bi][fi] = 0;
            }
        }
        if (centerUi) {
            centerUiInputCount = fgState.fenceCount;
            for (UINT fi = 0; fi < 4; ++fi) {
                if (fi < fgState.fenceCount) {
                    centerUiInputFence[fi] = fgState.fences[fi];
                    centerUiInputValue[fi] = fgState.values[fi];
                } else {
                    centerUiInputFence[fi].Reset();
                    centerUiInputValue[fi] = 0;
                }
            }
        }
        control.hasFrame = true;
        lastPresented = presentEnd;
        ++count;
        double age = milliseconds(selectedAt - selected.acquired),
               submission = milliseconds(submittedAt - selected.acquired),
               present = milliseconds(presentEnd - presentBegin);
        if (telemetry)
            telemetry->push(count, selected.sequence, mailbox.dropCount(), age, submission, present,
                            state.numFramesActuallyPresented, unsigned(state.status), gpuSample);
        ageSum += age;
        submitSum += submission;
        presentSum += present;
        ++samples;
        presents += state.numFramesActuallyPresented;
        if (milliseconds(presentEnd - lastReport) >= 1000) {
            // LL4: publish a compact atomic snapshot only. The UI thread performs
            // formatting and display/monitor checks, keeping mutex/string/Win32
            // reporting work off the presenter hot thread.
            double elapsed = milliseconds(presentEnd - lastReport) / 1000.;
            {
                std::ostringstream diag;
                diag << std::fixed << std::setprecision(2)
                     << "FG interval: base_fps=" << samples / elapsed
                     << " SL_reported_present_fps=" << presents / elapsed
                     << " multiplier_requested=" << options.multiplier
                     << " actual_reported_ratio=" << (samples ? double(presents) / samples : 0.0)
                     << " capture_age_after_grab_ms=" << ageSum / samples
                     << " submit_age_after_grab_ms=" << submitSum / samples
                     << " Present_cpu_ms=" << presentSum / samples
                     << " mailbox_dropped_total=" << mailbox.dropCount()
                     << " DLSSG_status=0x" << std::hex << unsigned(state.status) << std::dec
                     << " motion_mode=" << (opticalFlow ? "NVOFA_4x4_FAST" : "ZERO")
                     << " ofa_resolution_percent=" << (opticalFlow ? options.opticalFlowPercent : 0)
                     << " nvofa_executed=" << ofaSubmissions
                     << " nvofa_cpu_enqueue_ms=" << (opticalFlow ? ofaCpuSum / samples : 0.0)
                     << " (reported presents are NOT measured monitor scanouts)";
                diagnostics::write(diag.str());
            }
            auto milli = [](double v) -> uint32_t {
                v = std::clamp(v * 1000.0, 0.0, double(UINT32_MAX));
                return uint32_t(v + 0.5);
            };
            control.perf.baseFpsMilli.store(milli(samples / elapsed), std::memory_order_relaxed);
            control.perf.presentFpsMilli.store(milli(presents / elapsed), std::memory_order_relaxed);
            control.perf.ageUs.store(milli(ageSum / samples), std::memory_order_relaxed);
            control.perf.submitUs.store(milli(submitSum / samples), std::memory_order_relaxed);
            control.perf.presentUs.store(milli(presentSum / samples), std::memory_order_relaxed);
            control.perf.dropped.store(mailbox.dropCount(), std::memory_order_relaxed);
            control.perf.dlssStatus.store(unsigned(state.status), std::memory_order_relaxed);
            control.perf.bridgeDirect.store(true, std::memory_order_relaxed);
            control.perf.outputWidth.store(width, std::memory_order_relaxed);
            control.perf.outputHeight.store(height, std::memory_order_relaxed);
            control.perf.centerRadius.store(options.centerRadius, std::memory_order_relaxed);
            control.perf.version.fetch_add(1, std::memory_order_release);
            control.perfActive.store(true, std::memory_order_release);
            lastReport = presentEnd;
            samples = presents = 0;
            ageSum = submitSum = presentSum = 0;
            ofaCpuSum = 0;
            ofaSubmissions = 0;
        }
    }
}
} // namespace
void runPresenter(HWND t, HWND o, const Options &options, SessionControl &control) {
    try {
        diagnostics::write("Presenter start: FG multiplier=" + std::to_string(options.multiplier) +
                           " VSync=off preset_model_activation=UNVERIFIED" +
                           std::string(options.opticalFlow ? " NVOFA=REQUESTED_4x4_FAST" : " NVOFA=OFF") +
                           " NVOFA_resolution_percent=" + std::to_string(options.opticalFlowPercent));
        optimizeProcessLatency();
        MmcssScope mmcss;
        Engine engine(t, o, options, control);
        control.report(L"Starting NvFBC and DLSS-G...");
        engine.init();
        engine.loop();
        if (control.stop)
            control.report(L"Stopped.");
        diagnostics::write("Presenter stopped normally");
    } catch (const std::exception &e) {
        diagnostics::write(std::string("Presenter error: ") + e.what());
        control.report(std::wstring(L"Stopped: ") + std::wstring(e.what(), e.what() + strlen(e.what())));
        control.stop = true;
    }
    control.hasFrame = false;
}
