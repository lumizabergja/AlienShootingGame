#include "preset_c.h"
#include <windows.h>
#ifndef __success
// Microsoft's SAL attribute is absent in MinGW, but used in NVAPI prototypes.
#define __success(expression)
#endif
#include <nvapi.h>
#include <NvApiDriverSettings.h>
#include <algorithm>
#include <cstdint>
#include <cwchar>

namespace {
using Query = void *(__cdecl *)(unsigned int);
template <typename T> T api(Query query, unsigned int id) {
    return reinterpret_cast<T>(query(id));
}

template <size_t N> void copyUnicode(NvU16 (&out)[N], const wchar_t *value) {
    // NVAPI uses UTF-16; wchar_t is also 16-bit in the Windows build.
    static_assert(sizeof(wchar_t) == sizeof(NvU16), "NVAPI requires Windows UTF-16");
    size_t i = 0;
    while (i + 1 < N && value[i]) {
        out[i] = static_cast<NvU16>(value[i]);
        ++i;
    }
    out[i] = 0;
}

constexpr unsigned int kInitialize = 0x0150e828;
constexpr unsigned int kUnload = 0xd22bdd7e;
constexpr unsigned int kCreateSession = 0x0694d52e;
constexpr unsigned int kDestroySession = 0xdad9cff8;
constexpr unsigned int kLoadSettings = 0x375dbd6b;
constexpr unsigned int kSaveSettings = 0xfcbc7e14;
constexpr unsigned int kCreateProfile = 0xcc176068;
constexpr unsigned int kFindProfileByName = 0x7e4a9a0b;
constexpr unsigned int kCreateApplication = 0x4347a9de;
constexpr unsigned int kFindApplicationByName = 0xeee566b2;
constexpr unsigned int kSetSetting = 0x577dd202;
constexpr unsigned int kGetSetting = 0x73bf8338;

using Initializer = NvAPI_Status(__cdecl *)();
using SessionCall = NvAPI_Status(__cdecl *)(NvDRSSessionHandle);
using CreateSession = NvAPI_Status(__cdecl *)(NvDRSSessionHandle *);
using FindProfile = NvAPI_Status(__cdecl *)(NvDRSSessionHandle, NvAPI_UnicodeString, NvDRSProfileHandle *);
using CreateProfile = NvAPI_Status(__cdecl *)(NvDRSSessionHandle, NVDRS_PROFILE *, NvDRSProfileHandle *);
using FindApplication = NvAPI_Status(__cdecl *)(NvDRSSessionHandle, NvAPI_UnicodeString,
                                                 NvDRSProfileHandle *, NVDRS_APPLICATION *);
using CreateApplication = NvAPI_Status(__cdecl *)(NvDRSSessionHandle, NvDRSProfileHandle,
                                                   NVDRS_APPLICATION *);
using SetSetting = NvAPI_Status(__cdecl *)(NvDRSSessionHandle, NvDRSProfileHandle, NVDRS_SETTING *);
using GetSetting = NvAPI_Status(__cdecl *)(NvDRSSessionHandle, NvDRSProfileHandle, NvU32, NVDRS_SETTING *);

struct Cleanup {
    HMODULE dll{};
    SessionCall destroy{};
    Initializer unload{};
    NvDRSSessionHandle session{};
    bool initialized{};

    ~Cleanup() {
        if (session && destroy) destroy(session);
        if (initialized && unload) unload();
        if (dll) FreeLibrary(dll);
    }
};

constexpr wchar_t kProfileName[] = L"FrameGeneration - DLSS FG Preset C";
constexpr wchar_t kFriendlyName[] = L"FrameGeneration (Preset C)";
}

PresetCResult applyPresetCProfile() {
    Cleanup cleanup;
    cleanup.dll = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!cleanup.dll) return {false, int(GetLastError()), L"Load nvapi64.dll"};
    auto query = reinterpret_cast<Query>(GetProcAddress(cleanup.dll, "nvapi_QueryInterface"));
    if (!query) return {false, int(GetLastError()), L"NVAPI interface"};

    const auto initialize = api<Initializer>(query, kInitialize);
    cleanup.unload = api<Initializer>(query, kUnload);
    const auto createSession = api<CreateSession>(query, kCreateSession);
    cleanup.destroy = api<SessionCall>(query, kDestroySession);
    const auto loadSettings = api<SessionCall>(query, kLoadSettings);
    const auto saveSettings = api<SessionCall>(query, kSaveSettings);
    const auto createProfile = api<CreateProfile>(query, kCreateProfile);
    const auto findProfile = api<FindProfile>(query, kFindProfileByName);
    const auto findApplication = api<FindApplication>(query, kFindApplicationByName);
    const auto createApplication = api<CreateApplication>(query, kCreateApplication);
    const auto setSetting = api<SetSetting>(query, kSetSetting);
    const auto getSetting = api<GetSetting>(query, kGetSetting);
    if (!initialize || !cleanup.unload || !createSession || !cleanup.destroy ||
        !loadSettings || !saveSettings || !createProfile || !findProfile ||
        !findApplication || !createApplication || !setSetting || !getSetting)
        return {false, -1, L"NVAPI driver interfaces"};

    NvAPI_Status rc = initialize();
    if (rc != NVAPI_OK) return {false, rc, L"Initialize NVAPI"};
    cleanup.initialized = true;
    rc = createSession(&cleanup.session);
    if (rc != NVAPI_OK) return {false, rc, L"Create DRS session"};
    rc = loadSettings(cleanup.session);
    if (rc != NVAPI_OK) return {false, rc, L"Load DRS settings"};

    wchar_t exe[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH))
        return {false, int(GetLastError()), L"Find presenter EXE"};
    NvAPI_UnicodeString appName{};
    copyUnicode(appName, exe);

    NvDRSProfileHandle profile{};
    NVDRS_APPLICATION existing{};
    existing.version = NVDRS_APPLICATION_VER;
    rc = findApplication(cleanup.session, appName, &profile, &existing);
    if (rc == NVAPI_EXECUTABLE_NOT_FOUND) {
        NvAPI_UnicodeString profileName{};
        copyUnicode(profileName, kProfileName);
        rc = findProfile(cleanup.session, profileName, &profile);
        if (rc == NVAPI_PROFILE_NOT_FOUND) {
            NVDRS_PROFILE profileData{};
            profileData.version = NVDRS_PROFILE_VER;
            copyUnicode(profileData.profileName, kProfileName);
            rc = createProfile(cleanup.session, &profileData, &profile);
        }
        if (rc != NVAPI_OK) return {false, rc, L"Create or find FG profile"};

        NVDRS_APPLICATION app{};
        app.version = NVDRS_APPLICATION_VER;
        copyUnicode(app.appName, exe);
        copyUnicode(app.userFriendlyName, kFriendlyName);
        rc = createApplication(cleanup.session, profile, &app);
        if (rc != NVAPI_OK) return {false, rc, L"Attach presenter to FG profile"};
    } else if (rc != NVAPI_OK) {
        return {false, rc, L"Find presenter driver profile"};
    }

    // Only the Frame Generation model preset is changed. Do NOT enable NVIDIA's
    // FG DLL override: the user's original nvngx_dlssg.dll remains in use.
    NVDRS_SETTING setting{};
    setting.version = NVDRS_SETTING_VER;
    setting.settingId = NGX_DLSS_FG_OVERRIDE_RENDER_PRESET_SELECTION_ID;
    setting.settingType = NVDRS_DWORD_TYPE;
    setting.u32CurrentValue = NGX_DLSS_FG_OVERRIDE_RENDER_PRESET_SELECTION_RENDER_PRESET_C;
    rc = setSetting(cleanup.session, profile, &setting);
    if (rc != NVAPI_OK) return {false, rc, L"Set FG Preset C"};
    rc = saveSettings(cleanup.session);
    if (rc != NVAPI_OK) return {false, rc, L"Save FG Preset C"};

    NVDRS_SETTING verify{};
    verify.version = NVDRS_SETTING_VER;
    rc = getSetting(cleanup.session, profile,
                    NGX_DLSS_FG_OVERRIDE_RENDER_PRESET_SELECTION_ID, &verify);
    if (rc != NVAPI_OK || verify.u32CurrentValue !=
                             NGX_DLSS_FG_OVERRIDE_RENDER_PRESET_SELECTION_RENDER_PRESET_C)
        return {false, rc != NVAPI_OK ? rc : -2, L"Verify FG Preset C"};
    return {true, 0, L"Preset C driver profile saved"};
}
