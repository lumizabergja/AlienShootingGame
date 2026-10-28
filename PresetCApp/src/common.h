#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <d3d11_4.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>
#include <avrt.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
constexpr unsigned CaptureCount = 4, FrameCount = 3;
inline void check(HRESULT hr, const char *op) {
    if (FAILED(hr)) {
        char b[180];
        snprintf(b, sizeof b, "%s: 0x%08lx", op, (unsigned long)hr);
        throw std::runtime_error(b);
    }
}

// Latency-focused scheduling helpers. HIGH priority is intentionally used instead of
// REALTIME so the presenter cannot starve driver/system threads. MMCSS keeps the two
// hot threads responsive under foreground contention; power throttling is explicitly
// disabled so Windows does not place them in EcoQoS.
struct MmcssScope {
    HANDLE task{};
    MmcssScope() {
        DWORD index = 0;
        task = AvSetMmThreadCharacteristicsW(L"Games", &index);
        if (task)
            AvSetMmThreadPriority(task, AVRT_PRIORITY_CRITICAL);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#ifdef THREAD_POWER_THROTTLING_CURRENT_VERSION
        THREAD_POWER_THROTTLING_STATE state{};
        state.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
        state.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
        state.StateMask = 0;
        SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &state, sizeof(state));
#else
        // MinGW's older headers omit this Windows 11 API. Keep the same
        // behavior using the documented struct layout and enum value (3).
        struct ThreadPowerState { DWORD Version, ControlMask, StateMask; } state{1, 1, 0};
        using SetInfo = BOOL(WINAPI *)(HANDLE, int, LPVOID, DWORD);
        auto setInfo = reinterpret_cast<SetInfo>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadInformation"));
        if (setInfo)
            setInfo(GetCurrentThread(), 3, &state, sizeof(state));
#endif
    }
    ~MmcssScope() {
        if (task)
            AvRevertMmThreadCharacteristics(task);
    }
    MmcssScope(const MmcssScope &) = delete;
    MmcssScope &operator=(const MmcssScope &) = delete;
};
inline void optimizeProcessLatency() {
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state));
}
inline void createLowLatencyQueue(ID3D12Device *device, D3D12_COMMAND_LIST_TYPE type,
                                  ComPtr<ID3D12CommandQueue> &queue, const char *operation) {
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = type;
    desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    auto hr = device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue));
    if (FAILED(hr)) {
        desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), operation);
    }
}
inline int64_t ticks() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return q.QuadPart;
}
inline double milliseconds(int64_t n) {
    static double f = []() {
        LARGE_INTEGER q;
        QueryPerformanceFrequency(&q);
        return double(q.QuadPart) / 1000.;
    }();
    return n / f;
}
struct Event {
    HANDLE h{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    Event() {
        if (!h)
            throw std::runtime_error("CreateEvent failed");
    }
    ~Event() {
        if (h)
            CloseHandle(h);
    }
    Event(const Event &) = delete;
    Event &operator=(const Event &) = delete;
};
inline void waitFence(ID3D12Fence *f, uint64_t v, HANDLE e, std::atomic<bool> *stop = nullptr,
                      unsigned spinChecks = 0) {
    if (!v)
        return;
    auto completed = f->GetCompletedValue();
    if (completed == UINT64_MAX)
        throw std::runtime_error("GPU removed while waiting for fence");
    if (completed >= v)
        return;
    // LL4 can request a short user-mode wait for fences that are normally only a
    // few microseconds late. Fall back to the original kernel event path if the
    // GPU is not immediately ready, so longer waits do not burn a CPU core.
    for (unsigned spins = 0; spins < spinChecks; ++spins) {
        YieldProcessor();
        completed = f->GetCompletedValue();
        if (completed == UINT64_MAX)
            throw std::runtime_error("GPU removed while waiting for fence");
        if (completed >= v)
            return;
        if ((spins & 255u) == 255u && stop && stop->load(std::memory_order_relaxed))
            throw std::runtime_error("GPU fence wait stopped or timed out");
    }
    check(f->SetEventOnCompletion(v, e), "Fence event");
    auto end = GetTickCount64() + 5000;
    while (true) {
        auto r = WaitForSingleObject(e, 20);
        // A reusable event can still carry a prior fence notification.
        completed = f->GetCompletedValue();
        if (completed == UINT64_MAX)
            throw std::runtime_error("GPU removed while waiting for fence");
        if (completed >= v)
            return;
        if ((r != WAIT_TIMEOUT && r != WAIT_OBJECT_0) || (stop && stop->load()) || GetTickCount64() > end)
            throw std::runtime_error("GPU fence wait stopped or timed out");
    }
}
inline void barrier(ID3D12GraphicsCommandList *l, ID3D12Resource *r, D3D12_RESOURCE_STATES a,
                    D3D12_RESOURCE_STATES b) {
    if (a == b)
        return;
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
    l->ResourceBarrier(1, &x);
}
inline ComPtr<ID3D12Resource> texture(ID3D12Device *d, UINT w, UINT h, DXGI_FORMAT fmt,
                                      D3D12_RESOURCE_FLAGS flags,
                                      D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.Format = fmt;
    rd.SampleDesc.Count = 1;
    rd.Flags = flags;
    ComPtr<ID3D12Resource> r;
    check(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r)),
          "Create texture");
    return r;
}
inline ComPtr<ID3DBlob> compile(const char *s, const char *entry, const char *model) {
    ComPtr<ID3DBlob> b, e;
    auto hr = D3DCompile(s, strlen(s), nullptr, nullptr, nullptr, entry, model,
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, &e);
    if (FAILED(hr))
        throw std::runtime_error(e ? std::string((char *)e->GetBufferPointer(), e->GetBufferSize())
                                   : "Shader compile failed");
    return b;
}
struct Commands {
    ComPtr<ID3D12CommandAllocator> a;
    ComPtr<ID3D12GraphicsCommandList> l;
    void init(ID3D12Device *d, D3D12_COMMAND_LIST_TYPE type) {
        check(d->CreateCommandAllocator(type, IID_PPV_ARGS(&a)), "Allocator");
        check(d->CreateCommandList(0, type, a.Get(), nullptr, IID_PPV_ARGS(&l)), "Command list");
        check(l->Close(), "Initial close");
    }
    void begin() {
        check(a->Reset(), "Reset allocator");
        check(l->Reset(a.Get(), nullptr), "Reset list");
    }
    void submit(ID3D12CommandQueue *q) {
        check(l->Close(), "Close list");
        ID3D12CommandList *lists[] = {l.Get()};
        q->ExecuteCommandLists(1, lists);
    }
};
