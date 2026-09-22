import re, sys
from pathlib import Path

src = Path(sys.argv[1])
main = src / "main.cpp"
text = main.read_text(encoding="utf-8")

# Add a second allocator/list set used only for the FG/motion pass.
old = "    ComPtr<ID3D12CommandAllocator> allocators[kFrameCount];\n    ComPtr<ID3D12GraphicsCommandList> list;"
new = "    ComPtr<ID3D12CommandAllocator> allocators[kFrameCount];\n    ComPtr<ID3D12CommandAllocator> fgAllocators[kFrameCount];\n    ComPtr<ID3D12GraphicsCommandList> list;\n    ComPtr<ID3D12GraphicsCommandList> fgList;"
if old not in text:
    raise RuntimeError("Dx12State command list fields not found")
text = text.replace(old, new, 1)

old = """    for (UINT i = 0; i < kFrameCount; ++i) {
        hr = d.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&d.allocators[i]));
        if (FAILED(hr)) return false;
    }
    hr = d.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        d.allocators[0].Get(), nullptr, IID_PPV_ARGS(&d.list));
    if (FAILED(hr)) return false;
    d.list->Close();
"""
new = """    for (UINT i = 0; i < kFrameCount; ++i) {
        hr = d.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&d.allocators[i]));
        if (FAILED(hr)) return false;
        hr = d.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&d.fgAllocators[i]));
        if (FAILED(hr)) return false;
    }
    hr = d.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        d.allocators[0].Get(), nullptr, IID_PPV_ARGS(&d.list));
    if (FAILED(hr)) return false;
    d.list->Close();
    hr = d.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        d.fgAllocators[0].Get(), nullptr, IID_PPV_ARGS(&d.fgList));
    if (FAILED(hr)) return false;
    d.fgList->Close();
"""
if old not in text:
    raise RuntimeError("Command allocator/list creation block not found")
text = text.replace(old, new, 1)

# Prepare Reflex only after a real usable captured frame exists, and use 1ms acquisition.
old = """            gDLSS.PrepareFrame();
            DXGI_OUTDUPL_FRAME_INFO info{};ComPtr<IDXGIResource> resource;
            HRESULT hr=c.duplication->AcquireNextFrame(2,&info,&resource);"""
new = """            DXGI_OUTDUPL_FRAME_INFO info{};ComPtr<IDXGIResource> resource;
            HRESULT hr=c.duplication->AcquireNextFrame(1,&info,&resource);"""
if old not in text:
    raise RuntimeError("Current PrepareFrame/AcquireNextFrame block not found")
text = text.replace(old, new, 1)

anchor = """            if (stop_ || paused_ || IsIconic(target) ||
                (currentForeground != target && currentForeground != outputWindow &&
                 GetAncestor(currentForeground, GA_ROOT) != target)) continue;
            d.captureTimestamp = info.LastPresentTime;"""
replacement = """            if (stop_ || paused_ || IsIconic(target) ||
                (currentForeground != target && currentForeground != outputWindow &&
                 GetAncestor(currentForeground, GA_ROOT) != target)) continue;
            // Sleep/token as late as possible: only for a captured frame we will actually submit.
            gDLSS.PrepareFrame();
            d.captureTimestamp = info.LastPresentTime;"""
if anchor not in text:
    raise RuntimeError("Post-acquire focus anchor not found")
text = text.replace(anchor, replacement, 1)

new_render = r'''bool App::RenderFrame(CaptureState& c, Dx12State& d, UINT64 copyReady,
                      float syntheticOffsetPixels) {
    // Start OFA immediately. It independently waits on the capture fence and can run
    // in parallel with the graphics pre-pass below.
    gDLSS.PrepareMotion(d.sharedFence.Get(),copyReady);

    UINT frame = d.swapchain->GetCurrentBackBufferIndex();
    // Only wait for this backbuffer/allocator before the pre-pass.
    if (d.frameFenceValues[frame] && d.frameFence->GetCompletedValue() < d.frameFenceValues[frame]) {
        HRESULT hr = d.frameFence->SetEventOnCompletion(d.frameFenceValues[frame], d.frameEvent);
        if (FAILED(hr)) return false;
        if (WaitForSingleObject(d.frameEvent, 3000) != WAIT_OBJECT_0) return false;
    }

    HRESULT hr = d.allocators[frame]->Reset();
    if (FAILED(hr)) return false;
    hr = d.list->Reset(d.allocators[frame].Get(), d.pipeline.Get());
    if (FAILED(hr)) return false;

    // Pre-pass: put the newest captured image on the backbuffer while NVOFA works.
    d.list->SetPipelineState(d.pipeline.Get());
    D3D12_RESOURCE_BARRIER backBarrier{};
    backBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    backBarrier.Transition.pResource = d.backBuffers[frame].Get();
    backBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    backBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    backBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    d.list->ResourceBarrier(1, &backBarrier);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = d.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(frame) * d.rtvStride;
    d.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(d.width), static_cast<float>(d.height), 0.0f, 1.0f};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(d.width), static_cast<LONG>(d.height)};
    d.list->RSSetViewports(1, &vp);
    d.list->RSSetScissorRects(1, &scissor);
    d.list->SetGraphicsRootSignature(d.rootSignature.Get());
    ID3D12DescriptorHeap* heaps[] = {d.srvHeap.Get()};
    d.list->SetDescriptorHeaps(1, heaps);
    d.list->SetGraphicsRootDescriptorTable(0, d.srvHeap->GetGPUDescriptorHandleForHeapStart());
    float jitterX = syntheticOffsetPixels / static_cast<float>(std::max(1u, d.sourceWidth));
    d.list->SetGraphicsRoot32BitConstants(1, 1, &jitterX, 0);
    d.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d.list->DrawInstanced(3, 1, 0, 0);
    backBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    backBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    d.list->ResourceBarrier(1, &backBarrier);
    hr = d.list->Close();
    if (FAILED(hr)) return false;

    // Both the pre-pass and OFA consume this captured frame after the same fence.
    hr = d.queue->Wait(d.sharedFence.Get(), copyReady);
    if (FAILED(hr)) return false;
    ID3D12CommandList* preLists[] = {d.list.Get()};
    gDLSS.Marker(sl::PCLMarker::eRenderSubmitStart);
    d.queue->ExecuteCommandLists(1, preLists);

    // Shared FG history/motion resources still require the prior FG submission to finish,
    // but this CPU wait now overlaps the already-submitted pre-pass and OFA work.
    UINT64 pending=*std::max_element(std::begin(d.frameFenceValues),std::end(d.frameFenceValues));
    if(pending && d.frameFence->GetCompletedValue()<pending) {
        HR(d.frameFence->SetEventOnCompletion(pending,d.frameEvent),"FG resource completion");
        if(WaitForSingleObject(d.frameEvent,3000)!=WAIT_OBJECT_0) throw std::runtime_error("GPU stalled.");
    }

    hr = d.fgAllocators[frame]->Reset();
    if (FAILED(hr)) return false;
    hr = d.fgList->Reset(d.fgAllocators[frame].Get(), nullptr);
    if (FAILED(hr)) return false;

    // Record FG/motion work while OFA is still free to finish in parallel.
    gDLSS.Record(d.fgList.Get(),d.sharedTexture.Get());
    gDLSS.TagBackbuffer(d.fgList.Get(),d.backBuffers[frame].Get(),D3D12_RESOURCE_STATE_PRESENT);
    hr = d.fgList->Close();
    if (FAILED(hr)) return false;

    // The queue only waits for previous FG input consumption and OFA immediately
    // before executing the command list that actually reads the motion vectors.
    gDLSS.BeforeFrame(d.queue.Get());
    if(!gDLSS.WaitMotion(d.queue.Get())) return false;
    ID3D12CommandList* fgLists[] = {d.fgList.Get()};
    d.queue->ExecuteCommandLists(1, fgLists);
    gDLSS.Marker(sl::PCLMarker::eRenderSubmitEnd);

    UINT64 renderDone = c.nextFenceValue++;
    hr = d.queue->Signal(d.sharedFence.Get(), renderDone);
    if (FAILED(hr)) return false;
    c.lastRenderDone = renderDone;
    UINT sync = d.allowTearing ? 0 : 1;
    UINT flags = d.allowTearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
    gDLSS.Marker(sl::PCLMarker::ePresentStart);
    LARGE_INTEGER atPresent{}; QueryPerformanceCounter(&atPresent);
    if (d.qpcFrequency.QuadPart > 0 && d.captureTimestamp.QuadPart > 0 &&
        atPresent.QuadPart >= d.captureTimestamp.QuadPart) {
        double ageMs = 1000.0 * double(atPresent.QuadPart - d.captureTimestamp.QuadPart) / double(d.qpcFrequency.QuadPart);
        d.ageSumMs += ageMs; d.ageMaxMs = std::max(d.ageMaxMs, ageMs); ++d.ageSamples;
    }
    hr = d.swapchain->Present(sync, flags);
    gDLSS.Marker(sl::PCLMarker::ePresentEnd);
    if (FAILED(hr)) return false;
    gDLSS.AfterPresent();
    {
        ComPtr<IDXGISwapChain2> latencySwap;
        HRESULT latencyHr = d.swapchain.As(&latencySwap);
        if (SUCCEEDED(latencyHr)) {
            latencyHr = latencySwap->SetMaximumFrameLatency(1);
            if (FAILED(latencyHr)) return false;
        }
    }
    UINT64 maximum = *std::max_element(std::begin(d.frameFenceValues),std::end(d.frameFenceValues));
    UINT64 own = maximum + 1;
    hr = d.queue->Signal(d.frameFence.Get(), own);
    if (FAILED(hr)) return false;
    d.frameFenceValues[frame] = own;
    return true;
}

'''

pattern = re.compile(r'bool App::RenderFrame\(CaptureState& c, Dx12State& d, UINT64 copyReady,\n.*?\n}\n\nvoid App::DestroyDx12', re.S)
m = pattern.search(text)
if not m:
    raise RuntimeError("RenderFrame function block not found")
text = text[:m.start()] + new_render + "void App::DestroyDx12" + text[m.end():]

# Guardrails.
if "AcquireNextFrame(1,&info,&resource)" not in text:
    raise RuntimeError("1ms capture setting missing")
if "DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT" in text:
    raise RuntimeError("unsafe waitable-swapchain path present")
if text.count("gDLSS.PrepareMotion(d.sharedFence.Get(),copyReady);") != 1:
    raise RuntimeError("unexpected PrepareMotion count")
if "fgAllocators[kFrameCount]" not in text or "fgList" not in text:
    raise RuntimeError("split command-list resources missing")

main.write_text(text, encoding="utf-8", newline="")
