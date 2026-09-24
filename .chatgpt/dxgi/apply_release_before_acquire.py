from pathlib import Path

p=Path("source/main.cpp")
s=p.read_text(encoding="utf-8-sig")

# Release the currently owned Desktop Duplication frame immediately before acquiring
# the next one. Microsoft recommends minimizing this gap, and it guarantees a strict
# one-Acquire/one-Release lifecycle.
old='''    bool AcquireNewest(LARGE_INTEGER& captureQpc){
        if(!dup_)return false;
        DXGI_OUTDUPL_FRAME_INFO fi{};ComPtr<IDXGIResource> res;
        HRESULT hr=dup_->AcquireNextFrame(0,&fi,&res);
'''
if old not in s: raise SystemExit("AcquireNewest start not found")
new='''    bool AcquireNewest(LARGE_INTEGER& captureQpc){
        if(!dup_)return false;

        if(held_){
            HRESULT rr=dup_->ReleaseFrame();
            held_=false;
            if(rr==DXGI_ERROR_ACCESS_LOST){
                gLog.Write(L"DXGI access lost while releasing previous frame; recreating duplication...");
                RecoverDuplication();
                return false;
            }
            HR(rr,"DXGI ReleaseFrame before next Acquire");
        }

        DXGI_OUTDUPL_FRAME_INFO fi{};ComPtr<IDXGIResource> res;
        HRESULT hr=dup_->AcquireNextFrame(0,&fi,&res);
'''
s=s.replace(old,new,1)

# INVALID_CALL means the duplication object's ownership state is inconsistent.
# Recreate it as a defensive fallback rather than terminating.
old='''        if(hr==DXGI_ERROR_ACCESS_LOST){
            RecoverDuplication();
            return false;
        }
        HR(hr,"DXGI AcquireNextFrame");held_=true;
'''
if old not in s: raise SystemExit("AcquireNextFrame error handling block not found")
new='''        if(hr==DXGI_ERROR_ACCESS_LOST){
            RecoverDuplication();
            return false;
        }
        if(hr==DXGI_ERROR_INVALID_CALL){
            gLog.Write(L"DXGI AcquireNextFrame returned INVALID_CALL; rebuilding duplication state...");
            RecoverDuplication();
            return false;
        }
        HR(hr,"DXGI AcquireNextFrame");held_=true;
'''
s=s.replace(old,new,1)

# Keep ownership after the copy. The next call releases immediately before AcquireNextFrame.
old='''        ReleaseHeld();return true;
    }
private:
'''
if old not in s: raise SystemExit("success ReleaseHeld call not found")
s=s.replace(old,'''        // Keep this duplication frame owned through DLSS/Present.
        // It is released immediately before the next AcquireNextFrame call.
        return true;
    }
private:
''',1)

# Improve lifecycle logging.
s=s.replace(
    'DXGI Desktop Duplication: AcquireNextFrame(0), 0 software queue, cropped selected client area, GPU-only copy.',
    'DXGI Desktop Duplication: AcquireNextFrame(0), 0 software queue, GPU-only copy; ReleaseFrame occurs immediately before the next Acquire.',
    1)

s=s.replace(
    'RUNNING: DXGI Desktop Duplication / AcquireNextFrame(0) / 0 software queue / cropped game client',
    'RUNNING: DXGI / strict Acquire-Release lifecycle / Release-before-next-Acquire / 0 software queue',
    1)

p.write_text(s,encoding="utf-8")
print("DXGI release-before-acquire lifecycle applied")
