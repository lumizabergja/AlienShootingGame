from pathlib import Path
import sys

root = Path(sys.argv[1])
common = root / 'src' / 'Common.cpp'
cmake = root / 'CMakeLists.txt'

s = common.read_text(encoding='utf-8')
if '#include <bcrypt.h>' not in s:
    s = s.replace('#include <softpub.h>\n', '#include <softpub.h>\n#include <bcrypt.h>\n#include <array>\n#include <vector>\n')
if '#pragma comment(lib, "bcrypt.lib")' not in s:
    s = s.replace('#pragma comment(lib, "crypt32.lib")\n', '#pragma comment(lib, "crypt32.lib")\n#pragma comment(lib, "bcrypt.lib")\n')

start = s.index('bool VerifyEmbeddedSignature(')
end = s.index('\nvoid DebugLog(', start)
new = r'''bool VerifyEmbeddedSignature(const std::wstring& filePath, std::wstring* reason) {
    WINTRUST_FILE_INFO fileInfo{};
    fileInfo.cbStruct = sizeof(fileInfo);
    fileInfo.pcwszFilePath = filePath.c_str();

    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &fileInfo;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    // Do NOT force cache-only certificate retrieval here. On a clean/offline Windows
    // installation that can reject NVIDIA's valid signature with CERT_E_UNTRUSTEDROOT
    // (0x800B0109) simply because an intermediate/root update is not cached yet.
    data.dwProvFlags = 0;

    LONG status = WinVerifyTrust(nullptr, &policy, &data);
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &policy, &data);

    if (status == ERROR_SUCCESS) return true;

    // The packaged interposer comes from the checksum-verified official Streamline 2.14.1
    // SDK. Pin its exact SHA-256 as a deterministic fallback when the local Windows trust
    // store cannot build the Authenticode chain. A modified/replaced DLL still fails.
    static constexpr std::array<uint8_t, 32> kPinnedInterposerSha256 = {
        0x8c,0x87,0xc9,0x49,0x94,0x61,0xda,0x56,
        0x1e,0xdd,0x52,0x9a,0xa9,0xbf,0x78,0x31,
        0xd6,0x7d,0x7b,0x94,0xeb,0xb1,0xc1,0xa5,
        0xed,0x54,0xef,0x49,0x34,0xe1,0xea,0x4c
    };

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    HANDLE file = INVALID_HANDLE_VALUE;
    std::vector<uint8_t> hashObject;
    std::array<uint8_t, 32> digest{};
    bool pinnedMatch = false;

    do {
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) break;

        DWORD objectLen = 0, cb = 0;
        if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH,
                              reinterpret_cast<PUCHAR>(&objectLen), sizeof(objectLen), &cb, 0) < 0) break;
        hashObject.resize(objectLen);
        if (BCryptCreateHash(alg, &hash, hashObject.data(), objectLen, nullptr, 0, 0) < 0) break;

        file = CreateFileW(filePath.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE) break;

        std::array<uint8_t, 64 * 1024> buffer{};
        for (;;) {
            DWORD got = 0;
            if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr)) break;
            if (!got) {
                if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0)
                    pinnedMatch = (digest == kPinnedInterposerSha256);
                break;
            }
            if (BCryptHashData(hash, buffer.data(), got, 0) < 0) break;
        }
    } while (false);

    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);

    if (pinnedMatch) {
        DebugLog(L"Authenticode chain validation failed locally, but sl.interposer.dll matches the pinned official Streamline 2.14.1 SHA-256; allowing it.");
        return true;
    }

    if (reason) {
        std::wstringstream ss;
        ss << L"WinVerifyTrust failed: 0x" << std::hex << static_cast<unsigned long>(status)
           << L" and the DLL does not match the pinned official Streamline 2.14.1 SHA-256.";
        *reason = ss.str();
    }
    return false;
}
'''
s = s[:start] + new + s[end:]
common.write_text(s, encoding='utf-8')

c = cmake.read_text(encoding='utf-8')
if 'crypt32 bcrypt' not in c:
    c = c.replace('avrt wintrust crypt32 dwmapi shcore comctl32', 'avrt wintrust crypt32 bcrypt dwmapi shcore comctl32')
cmake.write_text(c, encoding='utf-8')

(root / 'FIX-NOTES.txt').write_text(
    'Fix: Streamline interposer trust check no longer forces cache-only certificate retrieval.\n'
    'Fallback: the exact official Streamline 2.14.1 sl.interposer.dll is accepted by pinned SHA-256\n'
    '8c87c9499461da561edd529aa9bf7831d67d7b94ebb1c1a5ed54ef4934e1ea4c.\n'
    'A modified/replaced interposer is still rejected.\n',
    encoding='utf-8')
print('Applied Streamline trust verification fix to', root)
