from pathlib import Path

# Move Reflex sleep to the moment a fresh WGC frame is actually available.
p=Path("source/main.cpp")
s=p.read_text(encoding="utf-8-sig")
old='DWORD wr=WaitForSingleObject(cap.FrameEvent(),50);if(stop_)break;if(wr!=WAIT_OBJECT_0)continue;\n            auto frame=cap.TakeFrame();if(!frame)continue;'
if old not in s: raise SystemExit("capture wait sequence not found")
new='''DWORD wr=WaitForSingleObject(cap.FrameEvent(),50);if(stop_)break;if(wr!=WAIT_OBJECT_0)continue;
            // External-capture pacing: only enter Reflex sleep once WGC has announced
            // that a new source frame is available. Do not sleep immediately after Present.
            gDLSS.PrepareFrame();
            auto frame=cap.TakeFrame();if(!frame)continue;'''
s=s.replace(old,new,1)

old='            gDLSS.PrepareFrame();\n            auto now=std::chrono::steady_clock::now();'
if old not in s: raise SystemExit("post-present PrepareFrame call not found")
s=s.replace(old,'            auto now=std::chrono::steady_clock::now();',1)

s=s.replace('RUNNING: WGC1 / safe copy fence / host-owned DXGI latency pacing / Reflex / Streamline hooked Present',
            'RUNNING: WGC1 / capture-triggered Reflex sleep / safe copy fence / host-owned DXGI pacing',1)
p.write_text(s,encoding="utf-8")

# Add bounded Reflex sleep tracing (first four real frames only).
p=Path("source/dlss_bridge.cpp")
s=p.read_text(encoding="utf-8-sig")
old='''    uint32_t n=index++;Check(newToken(token,&n),"New frame token");
    if(fg) Check(reflexSleep(*token),"Reflex sleep");
    framePrepared = true;
'''
if old not in s: raise SystemExit("PrepareFrame body not found")
new='''    uint32_t n=index++;Check(newToken(token,&n),"New frame token");
    if(fg) {
        if(n<4) log(L"Reflex sleep enter frame "+std::to_wstring(n));
        Check(reflexSleep(*token),"Reflex sleep");
        if(n<4) log(L"Reflex sleep return frame "+std::to_wstring(n));
    }
    framePrepared = true;
'''
s=s.replace(old,new,1)
p.write_text(s,encoding="utf-8")
