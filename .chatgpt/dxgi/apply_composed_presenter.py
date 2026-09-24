from pathlib import Path
import re

p=Path("source/main.cpp")
s=p.read_text(encoding="utf-8-sig")

# Force the presenter to remain DWM-composed on a single monitor.
old='''HWND App::CreatePresenter(){
    HMONITOR m=MonitorFromWindow(control_,MONITOR_DEFAULTTOPRIMARY);MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(m,&mi);
    HWND w=CreateWindowExW'''
if old not in s: raise SystemExit("CreatePresenter header not found")
new='''HWND App::CreatePresenter(){
    HMONITOR m=MonitorFromWindow(control_,MONITOR_DEFAULTTOPRIMARY);MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(m,&mi);
    constexpr int kComposedInset=1;
    const int px=mi.rcMonitor.left+kComposedInset;
    const int py=mi.rcMonitor.top+kComposedInset;
    const int pw=(mi.rcMonitor.right-mi.rcMonitor.left)-2*kComposedInset;
    const int ph=(mi.rcMonitor.bottom-mi.rcMonitor.top)-2*kComposedInset;
    HWND w=CreateWindowExW'''
s=s.replace(old,new,1)

old='''        mi.rcMonitor.left,mi.rcMonitor.top,mi.rcMonitor.right-mi.rcMonitor.left,mi.rcMonitor.bottom-mi.rcMonitor.top,nullptr,nullptr,inst_,nullptr);'''
if old not in s: raise SystemExit("CreatePresenter size args not found")
s=s.replace(old,'''        px,py,pw,ph,nullptr,nullptr,inst_,nullptr);''',1)

# Replace final SetWindowPos with inset rectangle.
pat=r'SetWindowPos\(w,HWND_TOPMOST,mi\.rcMonitor\.left,mi\.rcMonitor\.top,mi\.rcMonitor\.right-mi\.rcMonitor\.left,mi\.rcMonitor\.bottom-mi\.rcMonitor\.top,SWP_NOACTIVATE\|SWP_SHOWWINDOW\);'
s,n=re.subn(pat,'SetWindowPos(w,HWND_TOPMOST,px,py,pw,ph,SWP_NOACTIVATE|SWP_SHOWWINDOW);',s,count=1)
if n!=1: raise SystemExit("Presenter SetWindowPos not found")

# Keep 1080p render target based on the monitor rather than the slightly inset window size.
old='''bool App::CreateSwapchain(HWND out,const Settings& s,Dx12State& d){
    RECT r{};GetClientRect(out,&r);d.width=(UINT)std::max<LONG>(1,r.right);d.height=(UINT)std::max<LONG>(1,r.bottom);
'''
if old not in s: raise SystemExit("CreateSwapchain size block not found")
new='''bool App::CreateSwapchain(HWND out,const Settings& s,Dx12State& d){
    HMONITOR mon=MonitorFromWindow(out,MONITOR_DEFAULTTOPRIMARY);MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(mon,&mi);
    d.width=(UINT)std::max<LONG>(1,mi.rcMonitor.right-mi.rcMonitor.left);
    d.height=(UINT)std::max<LONG>(1,mi.rcMonitor.bottom-mi.rcMonitor.top);
'''
s=s.replace(old,new,1)

# Do not request ALLOW_TEARING; that can permit independent flip promotion.
old='BOOL allow=FALSE;d.factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,&allow,sizeof(allow));d.tearing=allow==TRUE;'
if old not in s: raise SystemExit("tearing feature line not found")
s=s.replace(old,'BOOL allow=FALSE;d.tearing=false;',1)

old='sd.Flags=(d.tearing?DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING:0)|DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;'
if old not in s: raise SystemExit("swapchain flags line not found")
s=s.replace(old,'sd.Flags=DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;',1)

# Make the diagnostic explicit.
needle='gLog.Write(L"Output pacing: host-owned frame-latency waitable object; max latency=1.");'
if needle not in s: raise SystemExit("output pacing log not found")
s=s.replace(needle,needle+'\n    gLog.Write(L"Single-monitor DXGI: presenter forced to DWM composition with 1px inset and tearing disabled.");',1)

s=s.replace(
    'RUNNING: DXGI / presenter excluded from DDA / strict Acquire-Release lifecycle / 0 software queue',
    'RUNNING: DXGI / DWM-composed 1px inset presenter / no tearing / strict Acquire-Release / 0 software queue',
    1)

p.write_text(s,encoding="utf-8")
print("Applied composed-presenter DXGI fix")
