#include "pch.h"
#include "AdaptivePresenter.h"
#include "DeviceResources.h"
#include "Logger.h"
#include "ScalingWindow.h"
#include "Win32Helper.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace Magpie {

AdaptivePresenter::~AdaptivePresenter() noexcept {
    _CleanupVulkan();
}

bool AdaptivePresenter::_Initialize(HWND hwndAttach) noexcept {
    _hwnd = hwndAttach;

    const SIZE rendererSize = Win32Helper::GetSizeOfRect(ScalingWindow::Get().RendererRect());
    _width = std::max<LONG>(1, rendererSize.cx);
    _height = std::max<LONG>(1, rendererSize.cy);

    if (!_InitVulkan()) {
        Logger::Get().Error("Vulkan presenter initialization failed");
        return false;
    }

    if (!_CreateD3DTargets()) {
        Logger::Get().Error("Failed to create D3D render targets for Vulkan presenter");
        return false;
    }

    return true;
}

bool AdaptivePresenter::_CreateD3DTargets() noexcept {
    _renderTex = nullptr;
    _renderRtv = nullptr;
    _readbackTex = nullptr;

    D3D11_TEXTURE2D_DESC renderDesc{};
    renderDesc.Width = _width;
    renderDesc.Height = _height;
    renderDesc.MipLevels = 1;
    renderDesc.ArraySize = 1;
    renderDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    renderDesc.SampleDesc.Count = 1;
    renderDesc.Usage = D3D11_USAGE_DEFAULT;
    renderDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = _deviceResources->GetD3DDevice()->CreateTexture2D(
        &renderDesc, nullptr, _renderTex.put());
    if (FAILED(hr)) {
        Logger::Get().ComError("CreateTexture2D(render) failed", hr);
        return false;
    }

    hr = _deviceResources->GetD3DDevice()->CreateRenderTargetView(
        _renderTex.get(), nullptr, _renderRtv.put());
    if (FAILED(hr)) {
        Logger::Get().ComError("CreateRenderTargetView failed", hr);
        return false;
    }

    D3D11_TEXTURE2D_DESC readbackDesc = renderDesc;
    readbackDesc.Usage = D3D11_USAGE_STAGING;
    readbackDesc.BindFlags = 0;
    readbackDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    hr = _deviceResources->GetD3DDevice()->CreateTexture2D(
        &readbackDesc, nullptr, _readbackTex.put());
    if (FAILED(hr)) {
        Logger::Get().ComError("CreateTexture2D(readback) failed", hr);
        return false;
    }

    return true;
}

bool AdaptivePresenter::BeginFrame(
    winrt::com_ptr<ID3D11Texture2D>& frameTex,
    winrt::com_ptr<ID3D11RenderTargetView>& frameRtv,
    POINT& drawOffset
) noexcept {
    drawOffset = {};
    frameTex = _renderTex;
    frameRtv = _renderRtv;
    return frameTex && frameRtv;
}

void AdaptivePresenter::EndFrame(bool waitForRenderComplete) noexcept {
    if (waitForRenderComplete) {
        _WaitForRenderComplete();
    }

    if (!_PresentVulkan()) {
        Logger::Get().Error("Vulkan present failed");
    }
}

bool AdaptivePresenter::OnResize() noexcept {
    const SIZE rendererSize = Win32Helper::GetSizeOfRect(ScalingWindow::Get().RendererRect());
    const uint32_t newWidth = std::max<LONG>(1, rendererSize.cx);
    const uint32_t newHeight = std::max<LONG>(1, rendererSize.cy);

    if (newWidth == _width && newHeight == _height) {
        return true;
    }

    _width = newWidth;
    _height = newHeight;

    if (_vkDevice) {
        vkDeviceWaitIdle(_vkDevice);
    }

    _DestroySwapchainResources();
    _DestroyUploadBuffer();

    if (!_CreateSwapchainResources()) {
        return false;
    }
    if (!_CreateUploadBuffer()) {
        return false;
    }
    return _CreateD3DTargets();
}

void AdaptivePresenter::OnEndResize(bool& shouldRedraw) noexcept {
    shouldRedraw = true;
}

uint32_t AdaptivePresenter::_FindMemoryType(uint32_t bits, VkMemoryPropertyFlags flags) noexcept {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(_vkPhysicalDevice, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & flags) == flags) {
            return i;
        }
    }
    return UINT32_MAX;
}

bool AdaptivePresenter::_InitVulkan() noexcept {
    VkApplicationInfo appInfo{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    appInfo.pApplicationName = "Magpie Vulkan Presenter";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "Magpie";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_1;

    const char* instanceExts[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME
    };

    VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &appInfo;
    ici.enabledExtensionCount = ARRAYSIZE(instanceExts);
    ici.ppEnabledExtensionNames = instanceExts;

    if (vkCreateInstance(&ici, nullptr, &_vkInstance) != VK_SUCCESS) {
        return false;
    }

    VkWin32SurfaceCreateInfoKHR sci{ VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    sci.hinstance = GetModuleHandleW(nullptr);
    sci.hwnd = _hwnd;
    if (vkCreateWin32SurfaceKHR(_vkInstance, &sci, nullptr, &_vkSurface) != VK_SUCCESS) {
        return false;
    }

    uint32_t gpuCount = 0;
    if (vkEnumeratePhysicalDevices(_vkInstance, &gpuCount, nullptr) != VK_SUCCESS || gpuCount == 0) {
        return false;
    }
    std::vector<VkPhysicalDevice> gpus(gpuCount);
    vkEnumeratePhysicalDevices(_vkInstance, &gpuCount, gpus.data());

    struct Candidate {
        VkPhysicalDevice gpu = VK_NULL_HANDLE;
        uint32_t queueFamily = UINT32_MAX;
        int score = -1;
    } best;

    for (VkPhysicalDevice gpu : gpus) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(gpu, &props);

        uint32_t qCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qCount, nullptr);
        std::vector<VkQueueFamilyProperties> qProps(qCount);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qCount, qProps.data());

        for (uint32_t q = 0; q < qCount; ++q) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(gpu, q, _vkSurface, &present);
            if ((qProps[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                int score = 100;
                if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score += 1000;
                if (props.vendorID == 0x10DE) score += 500;
                if (score > best.score) {
                    best = { gpu, q, score };
                }
            }
        }
    }

    if (!best.gpu) {
        return false;
    }

    _vkPhysicalDevice = best.gpu;
    _vkQueueFamily = best.queueFamily;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = _vkQueueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    const char* deviceExts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = ARRAYSIZE(deviceExts);
    dci.ppEnabledExtensionNames = deviceExts;

    if (vkCreateDevice(_vkPhysicalDevice, &dci, nullptr, &_vkDevice) != VK_SUCCESS) {
        return false;
    }

    vkGetDeviceQueue(_vkDevice, _vkQueueFamily, 0, &_vkQueue);

    VkCommandPoolCreateInfo cpci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = _vkQueueFamily;
    if (vkCreateCommandPool(_vkDevice, &cpci, nullptr, &_vkCommandPool) != VK_SUCCESS) {
        return false;
    }

    VkCommandBufferAllocateInfo cbai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool = _vkCommandPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(_vkDevice, &cbai, &_vkCommandBuffer) != VK_SUCCESS) {
        return false;
    }

    VkSemaphoreCreateInfo semInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    if (vkCreateSemaphore(_vkDevice, &semInfo, nullptr, &_vkImageAvailable) != VK_SUCCESS ||
        vkCreateSemaphore(_vkDevice, &semInfo, nullptr, &_vkRenderFinished) != VK_SUCCESS) {
        return false;
    }

    VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateFence(_vkDevice, &fenceInfo, nullptr, &_vkFrameFence) != VK_SUCCESS) {
        return false;
    }

    if (!_CreateSwapchainResources()) return false;
    if (!_CreateUploadBuffer()) return false;

    return true;
}

bool AdaptivePresenter::_CreateSwapchainResources() noexcept {
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(_vkPhysicalDevice, _vkSurface, &caps) != VK_SUCCESS) {
        return false;
    }

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(_vkPhysicalDevice, _vkSurface, &formatCount, nullptr);
    if (!formatCount) return false;
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(_vkPhysicalDevice, _vkSurface, &formatCount, formats.data());

    VkSurfaceFormatKHR chosen = formats[0];
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_R8G8B8A8_UNORM) {
            chosen = f;
            break;
        }
    }
    if (chosen.format != VK_FORMAT_R8G8B8A8_UNORM) {
        for (const auto& f : formats) {
            if (f.format == VK_FORMAT_B8G8R8A8_UNORM) {
                chosen = f;
                break;
            }
        }
    }

    if (chosen.format != VK_FORMAT_R8G8B8A8_UNORM && chosen.format != VK_FORMAT_B8G8R8A8_UNORM) {
        Logger::Get().Error("No compatible 8-bit Vulkan surface format");
        return false;
    }

    _vkSwapchainFormat = chosen.format;
    _needsBgraSwizzle = chosen.format == VK_FORMAT_B8G8R8A8_UNORM;

    if (caps.currentExtent.width != UINT32_MAX) {
        _vkExtent = caps.currentExtent;
    } else {
        _vkExtent.width = std::clamp(_width, caps.minImageExtent.width, caps.maxImageExtent.width);
        _vkExtent.height = std::clamp(_height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    uint32_t presentModeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(_vkPhysicalDevice, _vkSurface, &presentModeCount, nullptr);
    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(_vkPhysicalDevice, _vkSurface, &presentModeCount, presentModes.data());

    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    for (VkPresentModeKHR m : presentModes) {
        if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) {
            presentMode = m;
            break;
        }
    }
    if (presentMode == VK_PRESENT_MODE_FIFO_KHR) {
        for (VkPresentModeKHR m : presentModes) {
            if (m == VK_PRESENT_MODE_MAILBOX_KHR) {
                presentMode = m;
                break;
            }
        }
    }

    uint32_t imageCount = std::max(2u, caps.minImageCount + 1);
    if (caps.maxImageCount && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    ci.surface = _vkSurface;
    ci.minImageCount = imageCount;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = _vkExtent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = presentMode;
    ci.clipped = VK_TRUE;

    if (vkCreateSwapchainKHR(_vkDevice, &ci, nullptr, &_vkSwapchain) != VK_SUCCESS) {
        return false;
    }

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(_vkDevice, _vkSwapchain, &count, nullptr);
    _vkImages.resize(count);
    vkGetSwapchainImagesKHR(_vkDevice, _vkSwapchain, &count, _vkImages.data());
    return true;
}

bool AdaptivePresenter::_CreateUploadBuffer() noexcept {
    _vkUploadSize = static_cast<VkDeviceSize>(_width) * _height * 4;

    VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = _vkUploadSize;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(_vkDevice, &bci, nullptr, &_vkUploadBuffer) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(_vkDevice, _vkUploadBuffer, &req);
    const uint32_t memoryType = _FindMemoryType(req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memoryType == UINT32_MAX) return false;

    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memoryType;
    if (vkAllocateMemory(_vkDevice, &mai, nullptr, &_vkUploadMemory) != VK_SUCCESS) {
        return false;
    }
    if (vkBindBufferMemory(_vkDevice, _vkUploadBuffer, _vkUploadMemory, 0) != VK_SUCCESS) {
        return false;
    }
    if (vkMapMemory(_vkDevice, _vkUploadMemory, 0, _vkUploadSize, 0, &_vkUploadMapped) != VK_SUCCESS) {
        return false;
    }
    return true;
}

bool AdaptivePresenter::_PresentVulkan() noexcept {
    if (!_renderTex || !_readbackTex || !_vkSwapchain || !_vkUploadMapped) return false;

    ID3D11DeviceContext4* dc = _deviceResources->GetD3DDC();
    dc->CopyResource(_readbackTex.get(), _renderTex.get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = dc->Map(_readbackTex.get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        return false;
    }

    auto* dst = static_cast<uint8_t*>(_vkUploadMapped);
    const auto* src = static_cast<const uint8_t*>(mapped.pData);
    const size_t rowBytes = static_cast<size_t>(_width) * 4;

    if (!_needsBgraSwizzle) {
        for (uint32_t y = 0; y < _height; ++y) {
            std::memcpy(dst + static_cast<size_t>(y) * rowBytes,
                src + static_cast<size_t>(y) * mapped.RowPitch,
                rowBytes);
        }
    } else {
        for (uint32_t y = 0; y < _height; ++y) {
            const uint8_t* s = src + static_cast<size_t>(y) * mapped.RowPitch;
            uint8_t* d = dst + static_cast<size_t>(y) * rowBytes;
            for (uint32_t x = 0; x < _width; ++x) {
                d[x * 4 + 0] = s[x * 4 + 2];
                d[x * 4 + 1] = s[x * 4 + 1];
                d[x * 4 + 2] = s[x * 4 + 0];
                d[x * 4 + 3] = s[x * 4 + 3];
            }
        }
    }
    dc->Unmap(_readbackTex.get(), 0);

    vkWaitForFences(_vkDevice, 1, &_vkFrameFence, VK_TRUE, UINT64_MAX);
    vkResetFences(_vkDevice, 1, &_vkFrameFence);

    uint32_t imageIndex = 0;
    VkResult acquire = vkAcquireNextImageKHR(
        _vkDevice, _vkSwapchain, UINT64_MAX, _vkImageAvailable, VK_NULL_HANDLE, &imageIndex);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        return OnResize();
    }
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
        return false;
    }

    vkResetCommandBuffer(_vkCommandBuffer, 0);
    VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(_vkCommandBuffer, &beginInfo) != VK_SUCCESS) return false;

    VkImageMemoryBarrier toTransfer{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    toTransfer.srcAccessMask = 0;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = _vkImages[imageIndex];
    toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toTransfer.subresourceRange.levelCount = 1;
    toTransfer.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(_vkCommandBuffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toTransfer);

    VkBufferImageCopy copy{};
    copy.bufferOffset = 0;
    copy.bufferRowLength = _width;
    copy.bufferImageHeight = _height;
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = { _width, _height, 1 };
    vkCmdCopyBufferToImage(_vkCommandBuffer,
        _vkUploadBuffer,
        _vkImages[imageIndex],
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &copy);

    VkImageMemoryBarrier toPresent{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    toPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toPresent.dstAccessMask = 0;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toPresent.image = _vkImages[imageIndex];
    toPresent.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toPresent.subresourceRange.levelCount = 1;
    toPresent.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(_vkCommandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toPresent);

    if (vkEndCommandBuffer(_vkCommandBuffer) != VK_SUCCESS) return false;

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &_vkImageAvailable;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &_vkCommandBuffer;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &_vkRenderFinished;
    if (vkQueueSubmit(_vkQueue, 1, &submit, _vkFrameFence) != VK_SUCCESS) return false;

    VkPresentInfoKHR pi{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &_vkRenderFinished;
    pi.swapchainCount = 1;
    pi.pSwapchains = &_vkSwapchain;
    pi.pImageIndices = &imageIndex;

    VkResult present = vkQueuePresentKHR(_vkQueue, &pi);
    if (present == VK_ERROR_OUT_OF_DATE_KHR || present == VK_SUBOPTIMAL_KHR) {
        return OnResize();
    }
    return present == VK_SUCCESS;
}

void AdaptivePresenter::_DestroyUploadBuffer() noexcept {
    if (!_vkDevice) return;
    if (_vkUploadMapped && _vkUploadMemory) {
        vkUnmapMemory(_vkDevice, _vkUploadMemory);
        _vkUploadMapped = nullptr;
    }
    if (_vkUploadBuffer) {
        vkDestroyBuffer(_vkDevice, _vkUploadBuffer, nullptr);
        _vkUploadBuffer = VK_NULL_HANDLE;
    }
    if (_vkUploadMemory) {
        vkFreeMemory(_vkDevice, _vkUploadMemory, nullptr);
        _vkUploadMemory = VK_NULL_HANDLE;
    }
}

void AdaptivePresenter::_DestroySwapchainResources() noexcept {
    if (_vkDevice && _vkSwapchain) {
        vkDestroySwapchainKHR(_vkDevice, _vkSwapchain, nullptr);
        _vkSwapchain = VK_NULL_HANDLE;
    }
    _vkImages.clear();
}

void AdaptivePresenter::_CleanupVulkan() noexcept {
    if (_vkDevice) {
        vkDeviceWaitIdle(_vkDevice);
        _DestroyUploadBuffer();
        _DestroySwapchainResources();
        if (_vkFrameFence) vkDestroyFence(_vkDevice, _vkFrameFence, nullptr);
        if (_vkRenderFinished) vkDestroySemaphore(_vkDevice, _vkRenderFinished, nullptr);
        if (_vkImageAvailable) vkDestroySemaphore(_vkDevice, _vkImageAvailable, nullptr);
        if (_vkCommandPool) vkDestroyCommandPool(_vkDevice, _vkCommandPool, nullptr);
        vkDestroyDevice(_vkDevice, nullptr);
        _vkDevice = VK_NULL_HANDLE;
    }
    if (_vkSurface && _vkInstance) {
        vkDestroySurfaceKHR(_vkInstance, _vkSurface, nullptr);
        _vkSurface = VK_NULL_HANDLE;
    }
    if (_vkInstance) {
        vkDestroyInstance(_vkInstance, nullptr);
        _vkInstance = VK_NULL_HANDLE;
    }
}

}
