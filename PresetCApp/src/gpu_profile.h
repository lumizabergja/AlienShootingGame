#pragma once
#include "common.h"
#include "gpu_timing.h"
// Optional diagnostics. Read only after the frame context's existing GPU fence.
// No extra waits, stalls, or synchronous file writes are introduced for results.
class GpuProfiler {
    ComPtr<ID3D12QueryHeap> queries;
    ComPtr<ID3D12Resource> readback;
    uint64_t *mapped{};
    uint64_t frequency{};
    std::array<uint64_t, FrameCount> frames{};
    std::array<bool, FrameCount> motions{};
  public:
    void init(ID3D12Device *device, ID3D12CommandQueue *queue) {
        check(queue->GetTimestampFrequency(&frequency), "GPU timestamp frequency");
        if (!frequency) throw std::runtime_error("GPU timestamp frequency is zero");
        D3D12_QUERY_HEAP_DESC q{};
        q.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        q.Count = FrameCount * 4;
        check(device->CreateQueryHeap(&q, IID_PPV_ARGS(&queries)), "GPU timestamp heap");
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = FrameCount * 4 * sizeof(uint64_t);
        d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "GPU timing readback");
        D3D12_RANGE range{0, SIZE_T(d.Width)};
        check(readback->Map(0, &range, reinterpret_cast<void **>(&mapped)), "Map GPU timing readback");
    }
    void mark(ID3D12GraphicsCommandList *list, UINT context, UINT point) {
        if (queries) list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, context * 4 + point);
    }
    void finish(ID3D12GraphicsCommandList *list, UINT context, uint64_t frame, bool motion) {
        if (!queries) return;
        list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, context * 4,
            motion ? 4 : 2, readback.Get(), context * 4 * sizeof(uint64_t));
        frames[context] = frame;
        motions[context] = motion;
    }
    GpuSample read(UINT context) {
        if (!mapped || !frames[context]) return {};
        auto sample = decodeGpuTiming(mapped + context * 4, frequency, frames[context], motions[context]);
        frames[context] = 0;
        return sample;
    }
    ~GpuProfiler() {
        if (mapped) {
            D3D12_RANGE noWrites{0, 0};
            readback->Unmap(0, &noWrites);
        }
    }
};
