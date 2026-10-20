from pathlib import Path
p=Path("upstream/src/main.cpp")
s=p.read_text(encoding="utf-8")
def rep(a,b):
 global s
 if a not in s: raise RuntimeError("missing upstream anchor "+a[:100])
 s=s.replace(a,b,1)
rep('static void HotkeyDefs(', '''// External custom controller can request a graceful shutdown through a named Windows event.
static HANDLE gFwControllerStop = nullptr;
static bool FwControllerStop() { return gFwControllerStop && WaitForSingleObject(gFwControllerStop, 0) == WAIT_OBJECT_0; }
static void HotkeyDefs(''')
rep('if (!SingleInstance(L"Local\\\\JustFlow.SingleInstance"', 'if (!SingleInstance(L"Local\\\\FrameWarpCustomCore.SingleInstance"')
rep('std::wstring log_path = JoinPath(dir, cfg.log_file);','gFwControllerStop = OpenEventW(SYNCHRONIZE, FALSE, L"Local\\\\FrameWarpCustomCoreStop");\n    std::wstring log_path = JoinPath(dir, cfg.log_file);')
rep('Tray* tray = TrayCreate(L"JustFlow");', 'Tray* tray = nullptr; // controller provides the only user-facing UI')
rep('while (!quit)\n    {', 'while (!quit)\n    {\n        if (FwControllerStop()) { quit = true; break; }')
rep('while (!quit && pending_profile < 0) { Sleep(100); handle_tray(); }',
    'while (!quit && pending_profile < 0) { if (FwControllerStop()) { quit = true; break; } Sleep(100); handle_tray(); }')
rep('for (int i = 0; !quit && !go_idle && pending_profile < 0 && !(target = FindTarget(cfg)); ++i)',
    'for (int i = 0; !quit && !go_idle && pending_profile < 0 && !(target = FindTarget(cfg)); ++i)')
rep('for (int t = 0; t < 10 && !quit && !go_idle && pending_profile < 0; ++t) { Sleep(100); handle_tray(); }',
    'for (int t = 0; t < 10 && !quit && !go_idle && pending_profile < 0; ++t) { if (FwControllerStop()) { quit = true; break; } Sleep(100); handle_tray(); }')
rep('for (;;)\n        {\n            handle_tray();', 'for (;;)\n        {\n            if (FwControllerStop()) { quit = true; break; }\n            handle_tray();')
p.write_text(s,encoding="utf-8")
# Keep these two calls syntactically balanced: they close add_custom_command.
cm=Path("upstream/CMakeLists.txt");c=cm.read_text(encoding="utf-8")
for old in [
    'COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/runtime/nvngx_dlssnr.dll $<TARGET_FILE_DIR:justflow_spike>)',
    'COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/runtime/nvngx_dlssnr.dll ${CMAKE_SOURCE_DIR}/runtime/nvngx_dlssg.dll $<TARGET_FILE_DIR:justflow>)'
]:
    if old not in c: raise RuntimeError("missing CMake optional DLL copy command "+old)
    c=c.replace(old,'COMMAND ${CMAKE_COMMAND} -E echo "FrameWarp-only build: no neural or DLSS-G runtime copy")')
cm.write_text(c,encoding="utf-8")
print("Patched upstream Frame Warp backend: camera auto-fit, OFA, latest-frame latch, stop event.")
