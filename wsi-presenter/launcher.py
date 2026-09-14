import ctypes
from ctypes import wintypes
import os
import subprocess
import sys
import time

u = ctypes.windll.user32
try:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)
except Exception:
    try:
        u.SetProcessDPIAware()
    except Exception:
        pass

def message(text, title="WSI Capture Presenter", flags=0x40):
    u.MessageBoxW(None, text, title, flags)

def find_lol():
    matches = []
    CALLBACK = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
    def visit(hwnd, _):
        if not u.IsWindowVisible(hwnd):
            return True
        n = u.GetWindowTextLengthW(hwnd)
        if not n:
            return True
        buf = ctypes.create_unicode_buffer(n + 1)
        u.GetWindowTextW(hwnd, buf, n + 1)
        if "league of legends" in buf.value.lower():
            matches.append((hwnd, buf.value))
        return True
    callback = CALLBACK(visit)
    u.EnumWindows(callback, 0)
    return matches[-1] if matches else (None, None)

def client_rect(hwnd):
    rect = wintypes.RECT()
    if not u.GetClientRect(hwnd, ctypes.byref(rect)):
        raise OSError("GetClientRect failed")
    pt = wintypes.POINT(0, 0)
    if not u.ClientToScreen(hwnd, ctypes.byref(pt)):
        raise OSError("ClientToScreen failed")
    return pt.x, pt.y, rect.right, rect.bottom

def main():
    hwnd, _ = find_lol()
    if not hwnd:
        message("League of Legends was not found.\n\nStart a match in Borderless mode, then run this app.", flags=0x30)
        return 2
    x, y, width, height = client_rect(hwnd)
    if width < 64 or height < 64:
        message("The League window has an invalid client size.", flags=0x10)
        return 3
    if x < 0 or y < 0:
        message("This build captures DXGI output 0. Move League to the primary monitor and try again.", flags=0x30)
        return 4

    core = os.path.join(os.path.dirname(sys.executable), "VulkanPresenterCore.exe")
    if not os.path.exists(core):
        message("VulkanPresenterCore.exe must be beside this launcher.", flags=0x10)
        return 5
    args = [core, "--source", "monitor:0", "--crop", str(x), str(y),
            "--at", str(x), str(y), "--size", str(width), str(height),
            "--title", "LoL Vulkan WSI Presenter"]
    proc = subprocess.Popen(args, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    message("DXGI Desktop Duplication is now feeding a real Vulkan swapchain.\n\nPress F8 to stop. Keep League in Borderless mode.\n\nEnable NVIDIA Smooth Motion for VulkanPresenterCore.exe, not League.")
    while proc.poll() is None:
        if u.GetAsyncKeyState(0x77) & 1:
            proc.terminate()
            break
        time.sleep(0.02)
    return proc.returncode or 0

if __name__ == "__main__":
    raise SystemExit(main())
