#include "diagnostics.h"
#include <windows.h>
#include <winver.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <vector>

namespace {
std::mutex logMutex;
std::ofstream logFile;
std::filesystem::path executableDirectory;

std::string timestamp() {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    char buf[48]{};
    snprintf(buf, sizeof buf, "%04u-%02u-%02u %02u:%02u:%02u.%03u",
             unsigned(t.wYear), unsigned(t.wMonth), unsigned(t.wDay),
             unsigned(t.wHour), unsigned(t.wMinute), unsigned(t.wSecond),
             unsigned(t.wMilliseconds));
    return buf;
}
}

void diagnostics::start() {
    std::lock_guard<std::mutex> lock(logMutex);
    wchar_t exe[32768]{};
    if (!GetModuleFileNameW(nullptr, exe, 32768)) return;
    executableDirectory = std::filesystem::path(exe).parent_path();
    logFile.open(executableDirectory / L"PresetC-Diagnostics.log", std::ios::out | std::ios::trunc);
    if (logFile) {
        logFile << "[" << timestamp() << "] Preset C diagnostics v2 - new launch\n"
                << "[" << timestamp() << "] Note: DRS value 3 proves only the driver-profile selection.\n"
                << "[" << timestamp() << "] Actual DLSS-G model is not exposed by Streamline state API.\n"
                << "[" << timestamp() << "] To confirm actual Preset C, check NVIDIA DLSS indicator for UIR: NA - Lite.\n";
        logFile.flush();
    }
}

void diagnostics::write(const std::string &message) {
    std::lock_guard<std::mutex> lock(logMutex);
    if (!logFile) return;
    logFile << "[" << timestamp() << "] " << message << '\n';
    logFile.flush();
}

void diagnostics::fileVersion(const wchar_t *filename) {
    if (executableDirectory.empty()) return;
    const std::filesystem::path path = executableDirectory / filename;
    const auto narrow = std::filesystem::path(filename).string();
    DWORD ignored = 0;
    DWORD bytes = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!bytes) {
        write("Runtime " + narrow + ": version resource unavailable (file may be missing)");
        return;
    }
    std::vector<char> data(bytes);
    VS_FIXEDFILEINFO *info = nullptr;
    UINT len = 0;
    if (!GetFileVersionInfoW(path.c_str(), 0, bytes, data.data()) ||
        !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void **>(&info), &len) ||
        !info || len < sizeof(VS_FIXEDFILEINFO)) {
        write("Runtime " + narrow + ": unable to inspect file version");
        return;
    }
    std::ostringstream os;
    os << "Runtime " << narrow << " file version="
       << HIWORD(info->dwFileVersionMS) << '.' << LOWORD(info->dwFileVersionMS)
       << '.' << HIWORD(info->dwFileVersionLS) << '.' << LOWORD(info->dwFileVersionLS);
    write(os.str());
}
