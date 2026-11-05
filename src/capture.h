#pragma once
#include "common.h"
#include <memory>
// Frozen legacy Windows NvFBC CUDA ABI. Driver availability is checked at runtime.
// The capture is PID-scoped; a mismatched PID is never displayed.
class Capture {
 struct Impl; std::unique_ptr<Impl> p;
public:
 Capture();~Capture();
 Capture(const Capture&)=delete;
 void open(HWND game,DWORD pid);
 LUID luid() const;
 unsigned width() const;unsigned height() const;unsigned pitch() const;
 void bind(ID3D12Device*,ID3D12Resource**);
 // Called only after previous rendering completes; source buffer has one owner.
 void grab();
 ID3D12Fence* readyFence() const;uint64_t readyValue() const;
};
