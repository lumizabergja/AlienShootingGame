#pragma once
#include "common.h"
// Compatibility is confined to this presenter's loaded NVIDIA modules.
// No DLL injection, game memory access, or global hook is performed.
class LocalFocus {
    struct Patch {
        ULONG_PTR *address;
        ULONG_PTR original;
    };
    std::vector<Patch> patches;
    static inline std::atomic<HWND> target{}, output{};
    static HWND WINAPI foreground() {
        HWND real = ::GetForegroundWindow();
        DWORD a = 0, b = 0;
        GetWindowThreadProcessId(real, &a);
        GetWindowThreadProcessId(target.load(), &b);
        return a && a == b ? output.load() : real;
    }
    static HWND WINAPI active() {
        return foreground();
    }

  public:
    LocalFocus(HWND t, HWND o) {
        target = t;
        output = o;
    }
    void install() {
        for (const wchar_t *name :
             {L"sl.interposer.dll", L"sl.common.dll", L"sl.dlss_g.dll", L"sl.reflex.dll"}) {
            auto base = (BYTE *)GetModuleHandleW(name);
            if (!base)
                continue;
            auto dos = (IMAGE_DOS_HEADER *)base;
            if (dos->e_magic != IMAGE_DOS_SIGNATURE)
                continue;
            auto nt = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
            auto &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            if (!dir.VirtualAddress)
                continue;
            for (auto imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); imp->Name; ++imp) {
                if (!imp->OriginalFirstThunk)
                    continue;
                auto names = (IMAGE_THUNK_DATA64 *)(base + imp->OriginalFirstThunk);
                auto iat = (IMAGE_THUNK_DATA64 *)(base + imp->FirstThunk);
                for (; names->u1.AddressOfData; ++names, ++iat) {
                    if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal))
                        continue;
                    auto symbol = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
                    ULONG_PTR replacement = 0;
                    if (strcmp((char *)symbol->Name, "GetForegroundWindow") == 0)
                        replacement = (ULONG_PTR)&foreground;
                    if (strcmp((char *)symbol->Name, "GetActiveWindow") == 0)
                        replacement = (ULONG_PTR)&active;
                    if (!replacement)
                        continue;
                    auto address = (ULONG_PTR *)&iat->u1.Function;
                    if (*address == replacement)
                        continue;
                    DWORD old = 0;
                    if (VirtualProtect(address, sizeof *address, PAGE_READWRITE, &old)) {
                        patches.push_back({address, *address});
                        *address = replacement;
                        DWORD unused;
                        VirtualProtect(address, sizeof *address, old, &unused);
                    }
                }
            }
        }
    }
    void restore() {
        for (auto it = patches.rbegin(); it != patches.rend(); ++it) {
            DWORD old;
            if (VirtualProtect(it->address, sizeof(ULONG_PTR), PAGE_READWRITE, &old)) {
                *it->address = it->original;
                DWORD u;
                VirtualProtect(it->address, sizeof(ULONG_PTR), old, &u);
            }
        }
        patches.clear();
    }
    ~LocalFocus() {
        restore();
    }
};
