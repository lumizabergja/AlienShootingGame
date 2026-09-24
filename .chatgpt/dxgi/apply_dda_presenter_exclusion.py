from pathlib import Path

p=Path("source/main.cpp")
s=p.read_text(encoding="utf-8-sig")

old='if(w){if(!SetWindowDisplayAffinity(w,WDA_EXCLUDEFROMCAPTURE_VALUE))gLog.Write(L"Warning: presenter exclusion from capture was not accepted.");ShowWindow(w,SW_SHOWNOACTIVATE);'
if old not in s: raise SystemExit("presenter display-affinity block not found")
new='''if(w){
        // Desktop Duplication needs the presenter excluded at the DDA compositor layer.
        // WDA_EXCLUDEFROMCAPTURE is too broad here and can leave a black/unstable
        // full-screen region on a single-monitor feedback path.
        SetWindowDisplayAffinity(w,WDA_NONE);
        struct WcaData { int Attrib; PVOID pvData; SIZE_T cbData; };
        using SetWcaFn=BOOL (WINAPI*)(HWND,WcaData*);
        auto setWca=reinterpret_cast<SetWcaFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"SetWindowCompositionAttribute"));
        BOOL excludeFromDda=TRUE;
        WcaData dda{24,&excludeFromDda,sizeof(excludeFromDda)}; // WCA_EXCLUDED_FROM_DDA
        if(!setWca||!setWca(w,&dda)){
            DWORD e=GetLastError();wchar_t msg[192]{};
            swprintf_s(msg,L"Warning: WCA_EXCLUDED_FROM_DDA failed (GetLastError=%lu). Single-monitor DXGI may self-capture.",e);
            gLog.Write(msg);
        }else{
            gLog.Write(L"Presenter excluded from Desktop Duplication using WCA_EXCLUDED_FROM_DDA.");
        }
        ShowWindow(w,SW_SHOWNOACTIVATE);'''
s=s.replace(old,new,1)

s=s.replace(
    'RUNNING: DXGI / strict Acquire-Release lifecycle / Release-before-next-Acquire / 0 software queue',
    'RUNNING: DXGI / presenter excluded from DDA / strict Acquire-Release lifecycle / 0 software queue',
    1)

# Prevent an uncontrolled log/recreate storm if Windows still invalidates DDA every frame.
old='''    void RecoverDuplication(){
        gLog.Write(L"DXGI access lost: recreating Desktop Duplication in-place...");
'''
if old not in s: raise SystemExit("RecoverDuplication start not found")
new='''    void RecoverDuplication(){
        LARGE_INTEGER now{},freq{};QueryPerformanceCounter(&now);QueryPerformanceFrequency(&freq);
        if(recoveryWindowQpc_==0||double(now.QuadPart-recoveryWindowQpc_)/double(freq.QuadPart)>=1.0){
            recoveryWindowQpc_=now.QuadPart;recoveriesThisSecond_=0;
        }
        if(++recoveriesThisSecond_>30)
            throw std::runtime_error("DXGI duplication is being invalidated continuously. Presenter exclusion from DDA did not stabilize the output.");
        gLog.Write(L"DXGI access lost: recreating Desktop Duplication in-place...");
'''
s=s.replace(old,new,1)

old='    bool held_{},stopRecovery_{};uint64_t dropped_{},recoveries_{},copyValue_{};\n'
if old not in s: raise SystemExit("DXGI recovery member line not found")
s=s.replace(old,'    bool held_{},stopRecovery_{};uint64_t dropped_{},recoveries_{},copyValue_{};LONGLONG recoveryWindowQpc_{};uint32_t recoveriesThisSecond_{};\n',1)

p.write_text(s,encoding="utf-8")
print("Applied DDA presenter exclusion")
