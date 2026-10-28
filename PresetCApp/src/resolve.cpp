#include "resolve.h"

void MotionResolve::initFlat(ID3D12Device *d, UINT w, UINT h, ID3D12CommandQueue *q) {
    depth = texture(d, w, h, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    for (auto &m : motion)
        m = texture(d, w, h, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = FrameCount + 1;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    check(d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)), "Desktop FG descriptors");
    stride = d->GetDescriptorHandleIncrementSize(hd.Type);
    auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
    for (auto &m : motion) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC v{};
        v.Format = DXGI_FORMAT_R16G16_FLOAT;
        v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        d->CreateUnorderedAccessView(m.Get(), nullptr, &v, cpu);
        cpu.ptr += stride;
    }
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC v{};
        v.Format = DXGI_FORMAT_R32_FLOAT;
        v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        d->CreateUnorderedAccessView(depth.Get(), nullptr, &v, cpu);
    }

    Commands c;
    c.init(d, D3D12_COMMAND_LIST_TYPE_DIRECT);
    c.begin();
    auto l = c.l.Get();
    ID3D12DescriptorHeap *heaps[] = {heap.Get()};
    l->SetDescriptorHeaps(1, heaps);
    auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
    auto cpu0 = heap->GetCPUDescriptorHandleForHeapStart();
    const float zero[4] = {0, 0, 0, 0};
    for (UINT i = 0; i < FrameCount; ++i) {
        auto gc = gpu; gc.ptr += UINT64(i) * stride;
        auto cc = cpu0; cc.ptr += SIZE_T(i) * stride;
        l->ClearUnorderedAccessViewFloat(gc, cc, motion[i].Get(), zero, 0, nullptr);
    }
    const float flat[4] = {.5f, .5f, .5f, .5f};
    auto gd = gpu; gd.ptr += UINT64(FrameCount) * stride;
    auto cd = cpu0; cd.ptr += SIZE_T(FrameCount) * stride;
    l->ClearUnorderedAccessViewFloat(gd, cd, depth.Get(), flat, 0, nullptr);

    std::array<D3D12_RESOURCE_BARRIER, FrameCount + 1> barriers{};
    UINT bi = 0;
    for (auto &m : motion) {
        auto &b = barriers[bi++];
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {m.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    }
    auto &b = barriers[bi++];
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {depth.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    l->ResourceBarrier(bi, barriers.data());
    c.submit(q);
    ComPtr<ID3D12Fence> fence;
    check(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Desktop FG input fence");
    check(q->Signal(fence.Get(), 1), "Desktop FG input signal");
    Event event;
    waitFence(fence.Get(), 1, event.h);
}
