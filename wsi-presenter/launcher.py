import ctypes
from ctypes import wintypes
import os
import subprocess
import sys
import time

u = ctypes.windll.user32
k = ctypes.windll.kernel32
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000

k.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
k.OpenProcess.restype = wintypes.HANDLE
k.QueryFullProcessImageNameW.argtypes = (wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD))
k.QueryFullProcessImageNameW.restype = wintypes.BOOL
k.CloseHandle.argtypes = (wintypes.HANDLE,)
k.CloseHandle.restype = wintypes.BOOL

try:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)
except Exception:
    try:
        u.SetProcessDPIAware()
    except Exception:
        pass

def message(text, title="Lossless Scaling Vulkan WSI", flags=0x40):
    u.MessageBoxW(None, text, title, flags)

def process_name(hwnd):
    pid = wintypes.DWORD()
    u.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    handle = k.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid.value)
    if not handle:
        return ""
    try:
        size = wintypes.DWORD(32768)
        buf = ctypes.create_unicode_buffer(size.value)
        if not k.QueryFullProcessImageNameW(handle, 0, buf, ctypes.byref(size)):
            return ""
        return os.path.basename(buf.value).lower()
    finally:
        k.CloseHandle(handle)

def client_rect(hwnd):
    rect = wintypes.RECT()
    if not u.GetClientRect(hwnd, ctypes.byref(rect)):
        raise OSError("GetClientRect failed")
    pt = wintypes.POINT(0, 0)
    if not u.ClientToScreen(hwnd, ctypes.byref(pt)):
        raise OSError("ClientToScreen failed")
    return pt.x, pt.y, rect.right - rect.left, rect.bottom - rect.top

def find_lossless_output():
    matches = []
    CALLBACK = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
    def visit(hwnd, _):
        if not u.IsWindowVisible(hwnd) or u.IsIconic(hwnd):
            return True
        if process_name(hwnd) != "losslessscaling.exe":
            return True
        try:
            x, y, width, height = client_rect(hwnd)
            if width >= 64 and height >= 64:
                matches.append((width * height, hwnd, x, y, width, height))
        except OSError:
            pass
        return True
    callback = CALLBACK(visit)
    u.EnumWindows(callback, 0)
    return max(matches, default=None)

def main():
    target = find_lossless_output()
    if not target:
        message("Lossless Scaling output was not found.\n\nStart Lossless Scaling first, activate scaling, then run this app.", flags=0x30)
        return 2
    _, hwnd, x, y, width, height = target
    if x < 0 or y < 0:
        message("This build captures DXGI output 0. Put Lossless Scaling on the primary monitor.", flags=0x30)
        return 3

    core = os.path.join(os.path.dirname(sys.executable), "VulkanPresenterCore.exe")
    if not os.path.exists(core):
        message("VulkanPresenterCore.exe must be beside this launcher.", flags=0x10)
        return 4

    args = [core, "--source", "monitor:0", "--crop", str(x), str(y),
            "--at", str(x), str(y), "--size", str(width), str(height),
            "--title", "Lossless Scaling Vulkan WSI", "--exclude"]
    proc = subprocess.Popen(args, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    message("Lossless Scaling is now captured through DXGI and presented through a Vulkan WSI swapchain.\n\nPress F8 to stop.\n\nApply NVIDIA Smooth Motion to VulkanPresenterCore.exe.")
    while proc.poll() is None:
        if u.GetAsyncKeyState(0x77) & 1:
            proc.terminate()
            break
        time.sleep(0.02)
    return proc.returncode or 0

if __name__ == "__main__":
    raise SystemExit(main())
