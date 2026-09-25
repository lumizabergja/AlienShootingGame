#pragma once
#include <windows.h>
#include <atomic>
#include <cstring>
#include <string>

// Experimental, process-local compatibility shim. Only sl.common.dll's import
// of GetForegroundWindow is redirected. Windows input focus is never changed.
// The NVIDIA DLL files on disk remain signed and unmodified.
namespace focus_compat {
inline std::atomic<HWND> target{}, output{};
inline std::atomic<bool> enabled{false};
inline std::atomic<unsigned long long> queries{}, redirects{};
inline HMODULE pinnedModule{};
inline void* volatile* importSlot{};
inline void* original{};

inline HWND WINAPI ForegroundForStreamline() {
    // This call uses the executable's untouched USER32 import, not sl.common's.
    HWND actual = ::GetForegroundWindow();
    queries.fetch_add(1, std::memory_order_relaxed);
    if (!enabled.load(std::memory_order_acquire)) return actual;
    HWND game = target.load(std::memory_order_relaxed);
    HWND presenter = output.load(std::memory_order_relaxed);
    if (!game || !presenter || !actual) return actual;
    // Exact selected window/root only. Alt-tab to any unrelated window passes
    // through, including other windows belonging to the same game process.
    if (actual != game && GetAncestor(actual, GA_ROOT) != game) return actual;
    if (!IsWindow(game) || IsIconic(game) || !IsWindowVisible(game) ||
        !IsWindow(presenter) || !IsWindowVisible(presenter)) return actual;
    redirects.fetch_add(1, std::memory_order_relaxed);
    return presenter;
}

inline void Enable(bool value) {
    enabled.store(value, std::memory_order_release);
}

inline bool Install(HWND game, HWND presenter, std::wstring& error) {
    Enable(false);
    queries = 0; redirects = 0;
    target = game; output = presenter;
    if (importSlot) { error = L"Focus compatibility was not fully restored; restart the app."; return false; }
    HMODULE module{};
    if (!GetModuleHandleExW(0, L"sl.common.dll", &module)) {
        error = L"Focus compatibility: sl.common.dll is not loaded."; return false;
    }
    // The module is loaded and signature-checked by Streamline's host. Walk its
    // x64 PE imports by name; never scan or patch instructions at guessed offsets.
    auto base = reinterpret_cast<unsigned char*>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        FreeLibrary(module); error = L"Focus compatibility: unsupported module format."; return false;
    }
    auto directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    void* volatile* found = nullptr;
    if (directory.VirtualAddress && directory.Size) {
        auto descriptors = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
        for (size_t i = 0; i < directory.Size / sizeof(*descriptors) && descriptors[i].Name; ++i) {
            auto& d = descriptors[i];
            if (!d.OriginalFirstThunk || !d.FirstThunk ||
                _stricmp(reinterpret_cast<char*>(base + d.Name), "USER32.dll")) continue;
            auto names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + d.OriginalFirstThunk);
            auto addresses = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + d.FirstThunk);
            for (size_t j = 0; names[j].u1.AddressOfData; ++j) {
                if (IMAGE_SNAP_BY_ORDINAL64(names[j].u1.Ordinal)) continue;
                auto name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names[j].u1.AddressOfData);
                if (std::strcmp(reinterpret_cast<char*>(name->Name), "GetForegroundWindow") == 0) {
                    found = reinterpret_cast<void* volatile*>(&addresses[j].u1.Function);
                    break;
                }
            }
            if (found) break;
        }
    }
    void* expected = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetForegroundWindow"));
    if (!found || !expected || *found != expected) {
        FreeLibrary(module);
        error = L"Focus compatibility: expected USER32 import missing or already redirected; no change made.";
        return false;
    }
    DWORD protection{};
    if (!VirtualProtect(const_cast<void**>(found), sizeof(void*), PAGE_READWRITE, &protection)) {
        FreeLibrary(module); error = L"Focus compatibility: cannot update local import."; return false;
    }
    void* hook = reinterpret_cast<void*>(&ForegroundForStreamline);
    void* previous = InterlockedCompareExchangePointer(found, hook, expected);
    DWORD ignored{};
    bool protectedAgain = VirtualProtect(const_cast<void**>(found), sizeof(void*), protection, &ignored) != FALSE;
    if (previous != expected) {
        FreeLibrary(module); error = L"Focus compatibility: import changed concurrently; no hook installed."; return false;
    }
    pinnedModule = module; importSlot = found; original = expected;
    if (!protectedAgain) {
        error = L"Focus compatibility: could not restore import page protection; close the app."; return false;
    }
    Enable(true);
    return true;
}

inline bool Remove() {
    Enable(false);
    target = nullptr; output = nullptr;
    if (!importSlot) return true;
    DWORD protection{};
    if (!VirtualProtect(const_cast<void**>(importSlot), sizeof(void*), PAGE_READWRITE, &protection)) return false;
    void* hook = reinterpret_cast<void*>(&ForegroundForStreamline);
    void* previous = InterlockedCompareExchangePointer(importSlot, original, hook);
    DWORD ignored{};
    bool protectedAgain = VirtualProtect(const_cast<void**>(importSlot), sizeof(void*), protection, &ignored) != FALSE;
    if (previous != hook || !protectedAgain) return false;
    importSlot = nullptr; original = nullptr;
    FreeLibrary(pinnedModule); pinnedModule = nullptr;
    return true;
}

inline std::wstring Report() {
    auto q = queries.exchange(0);
    auto r = redirects.exchange(0);
    return L"Focus compatibility queries=" + std::to_wstring(q) +
           L", redirected=" + std::to_wstring(r) + L" (not a generated-frame counter)";
}
}
