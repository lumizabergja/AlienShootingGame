#pragma once
#include "common.h"
class MotionResolve {
    ComPtr<ID3D12DescriptorHeap> heap;
    UINT stride{};
  public:
    std::array<ComPtr<ID3D12Resource>, FrameCount> motion;
    ComPtr<ID3D12Resource> depth;
    void initFlat(ID3D12Device *, UINT, UINT, ID3D12CommandQueue *);
};
