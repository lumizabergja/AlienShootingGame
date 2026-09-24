from pathlib import Path
import re

p=Path("source/main.cpp")
s=p.read_text(encoding="utf-8-sig")

# Keep adapter so Desktop Duplication can be recreated after mode/desktop changes.
old='        target_=target;d12Queue_=queue;'
if old not in s: raise SystemExit("DXGI init start not found")
s=s.replace(old,'        target_=target;adapter_=adapter;d12Queue_=queue;',1)

# Replace one-shot output/duplication creation with recoverable helper.
pat=r'''        HMONITOR wanted=MonitorFromWindow\(target_,MONITOR_DEFAULTTONEAREST\);.*?        HR\(output1->DuplicateOutput\(dev_\.Get\(\),&dup_\),"DXGI DuplicateOutput"\);\n'''
m=re.search(pat,s,flags=re.S)
if not m: raise SystemExit("one-shot DuplicateOutput block not found")
s=s[:m.start()]+'        OpenDuplication(true);\n'+s[m.end():]

# Replace fatal ACCESS_LOST with transparent recreation.
old='        if(hr==DXGI_ERROR_ACCESS_LOST)throw std::runtime_error("DXGI desktop duplication access was lost. Restart capture.");'
if old not in s: raise SystemExit("fatal access-lost branch not found")
s=s.replace(old,'''        if(hr==DXGI_ERROR_ACCESS_LOST){
            RecoverDuplication();
            return false;
        }''',1)

# Add recovery counter accessor for status diagnostics.
old='    uint64_t Dropped()const{return dropped_;}\n'
if old not in s: raise SystemExit("Dropped accessor not found")
s=s.replace(old,old+'    uint64_t Recoveries()const{return recoveries_;}\n',1)

# Add robust output re-enumeration and DuplicateOutput1 fallback before UpdateClientRect.
marker='private:\n    bool UpdateClientRect(UINT& w,UINT& h){'
if marker not in s: raise SystemExit("DXGI private marker not found")
helper=r'''private:
    void OpenDuplication(bool initial){
        if(held_&&dup_){dup_->ReleaseFrame();held_=false;}
        dup_.Reset();
        HMONITOR wanted=MonitorFromWindow(target_,MONITOR_DEFAULTTONEAREST);
        ComPtr<IDXGIOutput> output;
        for(UINT i=0;;++i){
            ComPtr<IDXGIOutput> x;HRESULT hr=adapter_->EnumOutputs(i,x.GetAddressOf());
            if(hr==DXGI_ERROR_NOT_FOUND)break;
            HR(hr,"DXGI EnumOutputs");
            DXGI_OUTPUT_DESC od{};HR(x->GetDesc(&od),"DXGI Output GetDesc");
            if(od.Monitor==wanted){output=x;outputDesc_=od;break;}
        }
        if(!output)throw std::runtime_error("Could not find the DXGI output containing the selected game window.");

        DXGI_FORMAT fmt=DXGI_FORMAT_B8G8R8A8_UNORM;
        ComPtr<IDXGIOutput5> output5;
        HRESULT hr=E_NOINTERFACE;
        if(SUCCEEDED(output.As(&output5))){
            hr=output5->DuplicateOutput1(dev_.Get(),0,1,&fmt,&dup_);
        }else{
            ComPtr<IDXGIOutput1> output1;HR(output.As(&output1),"IDXGIOutput1");
            hr=output1->DuplicateOutput(dev_.Get(),&dup_);
        }
        HR(hr,initial?"DXGI DuplicateOutput":"Recreate DXGI DuplicateOutput");
    }
    void RecoverDuplication(){
        gLog.Write(L"DXGI access lost: recreating Desktop Duplication in-place...");
        // ACCESS_LOST is expected across display-mode / desktop transitions.
        // Retry briefly without tearing down DLSS/MFG or the presenter.
        HRESULT last=DXGI_ERROR_ACCESS_LOST;
        for(int attempt=0;attempt<200&&!stopRecovery_;++attempt){
            try{
                OpenDuplication(false);
                UINT cw=0,ch=0;
                if(!UpdateClientRect(cw,ch))throw std::runtime_error("Selected game window is not fully on the duplicated output after recovery.");
                if(cw!=width_||ch!=height_)throw std::runtime_error("Game client size changed during DXGI recovery. Stop and Start again.");
                ++recoveries_;
                wchar_t msg[160]{};swprintf_s(msg,L"DXGI duplication recovered successfully (recovery #%llu).",(unsigned long long)recoveries_);gLog.Write(msg);
                return;
            }catch(const std::exception&){
                Sleep(10);
            }
        }
        throw std::runtime_error("DXGI desktop duplication could not recover after access loss.");
    }
    bool UpdateClientRect(UINT& w,UINT& h){'''
s=s.replace(marker,helper,1)

# Add adapter and counters to members.
old='    bool held_{};uint64_t dropped_{},copyValue_{};\n'
if old not in s: raise SystemExit("DXGI member counters not found")
s=s.replace(old,'    bool held_{},stopRecovery_{};uint64_t dropped_{},recoveries_{},copyValue_{};\n    ComPtr<IDXGIAdapter1> adapter_;\n',1)

# Release adapter on stop.
old='        input11_.Reset();input12_.Reset();copyFence_.Reset();dup_.Reset();on12_.Reset();ctx_.Reset();dev_.Reset();d12Queue_.Reset();'
if old not in s: raise SystemExit("DXGI Stop reset line not found")
s=s.replace(old,'        stopRecovery_=true;input11_.Reset();input12_.Reset();copyFence_.Reset();dup_.Reset();adapter_.Reset();on12_.Reset();ctx_.Reset();dev_.Reset();d12Queue_.Reset();',1)

# Include recovery count in once-per-second status.
old='swprintf_s(msg,L"Base %.0f FPS | DXGI accumulated drops %llu/s | capture->Present %.2f ms avg / %.2f max | %s",\n                    frames/sec,(unsigned long long)((drops-lastDrops)/std::max(0.001,sec)),avg,ageMax,gDLSS.Status().c_str());'
if old not in s: raise SystemExit("DXGI status format not found")
new='swprintf_s(msg,L"Base %.0f FPS | DXGI drops %llu/s | recoveries %llu | capture->Present %.2f ms avg / %.2f max | %s",\n                    frames/sec,(unsigned long long)((drops-lastDrops)/std::max(0.001,sec)),(unsigned long long)cap.Recoveries(),avg,ageMax,gDLSS.Status().c_str());'
s=s.replace(old,new,1)

p.write_text(s,encoding="utf-8")
print("DXGI auto-recovery transform applied")
