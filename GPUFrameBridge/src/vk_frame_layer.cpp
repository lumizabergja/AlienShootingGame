// Vulkan-loader frame sampler: a proof of concept for cooperating/unprotected
// Vulkan applications. It does not copy images or generate frames yet.
#if !defined(_WIN32)
#error This layer requires Windows.
#endif

#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include "present_sampler.hpp"
#include "bridge_shared.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>

using gpu_frame_bridge::PresentSampler;

namespace {
struct InstanceDispatch {
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkDestroyInstance destroy = nullptr;
};
struct DeviceDispatch {
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    PFN_vkDestroyDevice destroy = nullptr;
    PFN_vkQueuePresentKHR present = nullptr;
    PFN_vkGetDeviceQueue get_queue = nullptr;
    PFN_vkGetDeviceQueue2 get_queue2 = nullptr;
};

std::mutex g_mutex;
std::unordered_map<VkInstance, InstanceDispatch> g_instances;
std::unordered_map<VkDevice, DeviceDispatch> g_devices;
std::unordered_map<VkQueue, VkDevice> g_queues;
PresentSampler g_sampler(60.0);
bool g_configured = false;
std::uint64_t g_presented = 0;
std::uint64_t g_selected = 0;
std::chrono::steady_clock::time_point g_last_report{};
HANDLE g_settings_mapping = nullptr;
gpu_frame_bridge::SharedSettings* g_settings = nullptr;
ULONGLONG g_next_settings_probe = 0;
LONG g_last_revision = -1;

void logLine(const std::string& message) {
    char local_appdata[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", local_appdata, MAX_PATH);
    const std::string path = (n > 0 && n < MAX_PATH)
        ? std::string(local_appdata) + "\\VkFrameBridge.log"
        : "VkFrameBridge.log";
    std::ofstream file(path, std::ios::app);
    if (file) file << message << '\n';
}

void configure() {
    if (g_configured) return;
    g_configured = true;
    if (const char* fps = std::getenv("VKFG_CAPTURE_FPS")) {
        try { g_sampler.setCaptureFps(std::stod(fps)); }
        catch (...) { logLine("[WARN] Bad VKFG_CAPTURE_FPS; using 60"); }
    }
    logLine("[INFO] Vulkan frame sampler active (telemetry-only, no GPU capture)");
}

// The controller owns a named file mapping. Read only at the Present boundary;
// never poll or sleep on the GPU submission thread.
void updateSettings() {
    if (!g_settings) {
        const auto now = GetTickCount64();
        if (now < g_next_settings_probe) return;
        g_next_settings_probe = now + 1000;
        g_settings_mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE,
                                                gpu_frame_bridge::kSettingsName);
        if (!g_settings_mapping) return;
        g_settings = static_cast<gpu_frame_bridge::SharedSettings*>(
            MapViewOfFile(g_settings_mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                          sizeof(gpu_frame_bridge::SharedSettings)));
        if (!g_settings) {
            CloseHandle(g_settings_mapping);
            g_settings_mapping = nullptr;
            return;
        }
    }
    if (gpu_frame_bridge::atomicRead(&g_settings->magic) !=
        gpu_frame_bridge::kSettingsMagic) return;
    const auto revision = gpu_frame_bridge::atomicRead(&g_settings->revision);
    if ((revision & 1) != 0 || revision == g_last_revision) return;
    const auto fpsMilli = gpu_frame_bridge::atomicRead(&g_settings->capture_fps_milli);
    const auto multiplier = gpu_frame_bridge::atomicRead(&g_settings->fg_multiplier);
    if (revision != gpu_frame_bridge::atomicRead(&g_settings->revision)) return;
    if (fpsMilli < 0 || fpsMilli > 1'000'000 ||
        (multiplier != 0 && (multiplier < 2 || multiplier > 6))) {
        logLine("[WARN] Invalid shared settings ignored");
        g_last_revision = revision;
        return;
    }
    g_sampler.setCaptureFps(fpsMilli / 1000.0);
    g_last_revision = revision;
    logLine("[CONFIG] Capture FPS=" + std::to_string(fpsMilli / 1000.0) +
            " FG requested=" + std::to_string(multiplier) +
            " (selection-only: no real FG or export currently)");
}

VkLayerInstanceCreateInfo* findInstanceLink(const VkInstanceCreateInfo* ci) {
    auto* p = reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(ci->pNext));
    while (p) {
        if (p->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
            p->function == VK_LAYER_LINK_INFO) return p;
        p = reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(p->pNext));
    }
    return nullptr;
}

VkLayerDeviceCreateInfo* findDeviceLink(const VkDeviceCreateInfo* ci) {
    auto* p = reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(ci->pNext));
    while (p) {
        if (p->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
            p->function == VK_LAYER_LINK_INFO) return p;
        p = reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(p->pNext));
    }
    return nullptr;
}
} // namespace

extern "C" {
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance, const char*);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice, const char*);

__declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* version) {
    if (!version) return VK_ERROR_INITIALIZATION_FAILED;
    if (version->loaderLayerInterfaceVersion > 2) version->loaderLayerInterfaceVersion = 2;
    version->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    version->pfnGetDeviceProcAddr = vkGetDeviceProcAddr;
    version->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}

__declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL
vkCreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* alloc, VkInstance* instance) {
    auto* link = findInstanceLink(ci);
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    auto* next = link->u.pLayerInfo;
    const auto gipa = next->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = next->pNext;
    const auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult result = create(ci, alloc, instance);
    if (result == VK_SUCCESS) {
        std::lock_guard lock(g_mutex);
        configure();
        g_instances[*instance] = {gipa, reinterpret_cast<PFN_vkDestroyInstance>(gipa(*instance, "vkDestroyInstance"))};
        logLine("[INFO] vkCreateInstance successful");
    }
    return result;
}

__declspec(dllexport) VKAPI_ATTR void VKAPI_CALL
vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks* alloc) {
    PFN_vkDestroyInstance next = nullptr;
    {
        std::lock_guard lock(g_mutex);
        const auto it = g_instances.find(instance);
        if (it != g_instances.end()) { next = it->second.destroy; g_instances.erase(it); }
    }
    if (next) next(instance, alloc);
}

__declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL
vkCreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* ci,
               const VkAllocationCallbacks* alloc, VkDevice* device) {
    auto* link = findDeviceLink(ci);
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    auto* next = link->u.pLayerInfo;
    const auto gipa = next->pfnNextGetInstanceProcAddr;
    const auto gdpa = next->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = next->pNext;
    const auto create = reinterpret_cast<PFN_vkCreateDevice>(gipa(VK_NULL_HANDLE, "vkCreateDevice"));
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult result = create(physical, ci, alloc, device);
    if (result == VK_SUCCESS) {
        DeviceDispatch state;
        state.gdpa = gdpa;
        state.destroy = reinterpret_cast<PFN_vkDestroyDevice>(gdpa(*device, "vkDestroyDevice"));
        state.present = reinterpret_cast<PFN_vkQueuePresentKHR>(gdpa(*device, "vkQueuePresentKHR"));
        state.get_queue = reinterpret_cast<PFN_vkGetDeviceQueue>(gdpa(*device, "vkGetDeviceQueue"));
        state.get_queue2 = reinterpret_cast<PFN_vkGetDeviceQueue2>(gdpa(*device, "vkGetDeviceQueue2"));
        std::lock_guard lock(g_mutex);
        g_devices[*device] = state;
        logLine("[INFO] vkCreateDevice successful");
    }
    return result;
}

__declspec(dllexport) VKAPI_ATTR void VKAPI_CALL
vkDestroyDevice(VkDevice device, const VkAllocationCallbacks* alloc) {
    PFN_vkDestroyDevice next = nullptr;
    {
        std::lock_guard lock(g_mutex);
        const auto it = g_devices.find(device);
        if (it != g_devices.end()) { next = it->second.destroy; g_devices.erase(it); }
        for (auto itq = g_queues.begin(); itq != g_queues.end();) {
            if (itq->second == device) itq = g_queues.erase(itq);
            else ++itq;
        }
    }
    if (next) next(device, alloc);
}

__declspec(dllexport) VKAPI_ATTR void VKAPI_CALL
vkGetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* queue) {
    PFN_vkGetDeviceQueue next = nullptr;
    {
        std::lock_guard lock(g_mutex);
        if (auto it = g_devices.find(device); it != g_devices.end()) next = it->second.get_queue;
    }
    if (!next) return;
    next(device, family, index, queue);
    if (queue && *queue) { std::lock_guard lock(g_mutex); g_queues[*queue] = device; }
}

__declspec(dllexport) VKAPI_ATTR void VKAPI_CALL
vkGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* queue) {
    PFN_vkGetDeviceQueue2 next = nullptr;
    {
        std::lock_guard lock(g_mutex);
        if (auto it = g_devices.find(device); it != g_devices.end()) next = it->second.get_queue2;
    }
    if (!next) return;
    next(device, info, queue);
    if (queue && *queue) { std::lock_guard lock(g_mutex); g_queues[*queue] = device; }
}

__declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL
vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* info) {
    PFN_vkQueuePresentKHR next = nullptr;
    {
        std::lock_guard lock(g_mutex);
        updateSettings();
        const auto iq = g_queues.find(queue);
        if (iq != g_queues.end()) {
            const auto id = g_devices.find(iq->second);
            if (id != g_devices.end()) next = id->second.present;
        }
        const auto now = std::chrono::steady_clock::now();
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
        ++g_presented;
        if (g_sampler.shouldCopy(static_cast<std::uint64_t>(ns))) ++g_selected;
        if (g_last_report.time_since_epoch().count() == 0 ||
            now - g_last_report >= std::chrono::seconds(1)) {
            logLine("[STATS] presents=" + std::to_string(g_presented) +
                    " selected=" + std::to_string(g_selected) +
                    " (selection only; no image export yet)");
            g_last_report = now;
            g_presented = g_selected = 0;
        }
    }
    return next ? next(queue, info) : VK_ERROR_INITIALIZATION_FAILED;
}

__declspec(dllexport) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char* name) {
    if (!name) return nullptr;
    if (std::strcmp(name, "vkGetInstanceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkGetInstanceProcAddr);
    if (std::strcmp(name, "vkCreateInstance") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkCreateInstance);
    if (std::strcmp(name, "vkDestroyInstance") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkDestroyInstance);
    std::lock_guard lock(g_mutex);
    if (auto it = g_instances.find(instance); it != g_instances.end()) return it->second.gipa(instance, name);
    return nullptr;
}

__declspec(dllexport) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char* name) {
    if (!name) return nullptr;
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr);
    if (std::strcmp(name, "vkCreateDevice") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkCreateDevice);
    if (std::strcmp(name, "vkDestroyDevice") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkDestroyDevice);
    if (std::strcmp(name, "vkGetDeviceQueue") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceQueue);
    if (std::strcmp(name, "vkGetDeviceQueue2") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceQueue2);
    if (std::strcmp(name, "vkQueuePresentKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(vkQueuePresentKHR);
    std::lock_guard lock(g_mutex);
    if (auto it = g_devices.find(device); it != g_devices.end()) return it->second.gdpa(device, name);
    return nullptr;
}
} // extern "C"
