from pathlib import Path

p=Path("source/main.cpp")
s=p.read_text(encoding="utf-8-sig")

old='BOOL allow=FALSE;d.factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,&allow,sizeof(allow));d.tearing=allow==TRUE&&!s.vsync;'
if old not in s:
    old='BOOL allow=FALSE;d.factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,&allow,sizeof(allow));d.tearing=allow==TRUE;'
if old not in s: raise SystemExit("tearing capability line not found")
s=s.replace(old,'BOOL allow=FALSE;d.factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,&allow,sizeof(allow));d.tearing=allow==TRUE;',1)

old='sd.Flags=d.tearing?DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING:0;'
if old not in s: raise SystemExit("swapchain flags line not found")
s=s.replace(old,'sd.Flags=(d.tearing?DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING:0)|DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;',1)

old='HR(tmp.As(&d.swap),"Streamline proxy Swapchain3");'
if old not in s: raise SystemExit("proxy swapchain attach line not found")
new='''HR(tmp.As(&d.swap),"Streamline proxy Swapchain3");
    ComPtr<IDXGISwapChain2> latencySwap;
    HR(d.swap.As(&latencySwap),"Swapchain2 for host pacing");
    HR(latencySwap->SetMaximumFrameLatency(1),"Host SetMaximumFrameLatency(1)");
    gLog.Write(L"DXGI pacing: host owns FRAME_LATENCY_WAITABLE_OBJECT; max frame latency=1; Streamline flip-queue wait disabled by contract.");'''
s=s.replace(old,new,1)

# Make Present entry/return explicit in the app log for this diagnostic build.
old='gDLSS.Marker(sl::PCLMarker::ePresentStart);UINT sync=s.vsync?1:0;UINT flags=(!s.vsync&&d.tearing)?DXGI_PRESENT_ALLOW_TEARING:0;HRESULT ph=d.swap->Present(sync,flags);gDLSS.Marker(sl::PCLMarker::ePresentEnd);if(FAILED(ph))return false;gDLSS.AfterPresent();'
if old not in s: raise SystemExit("Present call block not found")
new='''gDLSS.Marker(sl::PCLMarker::ePresentStart);
    UINT sync=s.vsync?1:0;UINT flags=(!s.vsync&&d.tearing)?DXGI_PRESENT_ALLOW_TEARING:0;
    gLog.Write(L"Present enter");
    HRESULT ph=d.swap->Present(sync,flags);
    wchar_t presentMsg[128]{};swprintf_s(presentMsg,L"Present return hr=0x%08X",(unsigned)ph);gLog.Write(presentMsg);
    gDLSS.Marker(sl::PCLMarker::ePresentEnd);if(FAILED(ph))return false;gDLSS.AfterPresent();'''
s=s.replace(old,new,1)

s=s.replace('RUNNING: WGC1 / safe copy fence / native D3D12 work / Streamline proxy only for hooked Present path',
            'RUNNING: WGC1 / safe copy fence / host-owned DXGI latency pacing / Reflex / Streamline hooked Present',1)

p.write_text(s,encoding="utf-8")
