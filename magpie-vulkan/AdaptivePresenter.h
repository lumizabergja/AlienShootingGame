#pragma once
#include "PresenterBase.h"
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>

namespace Magpie {

class AdaptivePresenter final : public PresenterBase {
public:
    ~AdaptivePresenter() noexcept override;

    bool BeginFrame(
        winrt::com_ptr<ID3D11Texture2D>& frameTex,
        winrt::com_ptr<ID3D11RenderTargetView>& frameRtv,
        POINT& drawOffset
    ) noexcept override;

    void EndFrame(bool waitForRenderComplete = false) noexcept override;
    bool OnResize() noexcept override;
    void OnEndResize(bool& shouldRedraw) noexcept override;

protected:
    bool _Initialize(HWND hwndAttach) noexcept override;

private:
    bool _CreateD3DTargets() noexcept;
    bool _InitVulkan() noexcept;
    bool _CreateSwapchainResources() noexcept;
    bool _CreateUploadBuffer() noexcept;
    bool _PresentVulkan() noexcept;
    void _DestroySwapchainResources() noexcept;
    void _DestroyUploadBuffer() noexcept;
    void _CleanupVulkan() noexcept;
    uint32_t _FindMemoryType(uint32_t bits, VkMemoryPropertyFlags flags) noexcept;

    HWND _hwnd = nullptr;
    uint32_t _width = 0;
    uint32_t _height = 0;

    winrt::com_ptr<ID3D11Texture2D> _renderTex;
    winrt::com_ptr<ID3D11RenderTargetView> _renderRtv;
    winrt::com_ptr<ID3D11Texture2D> _readbackTex;

    VkInstance _vkInstance = VK_NULL_HANDLE;
    VkSurfaceKHR _vkSurface = VK_NULL_HANDLE;
    VkPhysicalDevice _vkPhysicalDevice = VK_NULL_HANDLE;
    VkDevice _vkDevice = VK_NULL_HANDLE;
    uint32_t _vkQueueFamily = UINT32_MAX;
    VkQueue _vkQueue = VK_NULL_HANDLE;

    VkSwapchainKHR _vkSwapchain = VK_NULL_HANDLE;
    VkFormat _vkSwapchainFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D _vkExtent{};
    std::vector<VkImage> _vkImages;

    VkBuffer _vkUploadBuffer = VK_NULL_HANDLE;
    VkDeviceMemory _vkUploadMemory = VK_NULL_HANDLE;
    void* _vkUploadMapped = nullptr;
    VkDeviceSize _vkUploadSize = 0;

    VkCommandPool _vkCommandPool = VK_NULL_HANDLE;
    VkCommandBuffer _vkCommandBuffer = VK_NULL_HANDLE;
    VkSemaphore _vkImageAvailable = VK_NULL_HANDLE;
    VkSemaphore _vkRenderFinished = VK_NULL_HANDLE;
    VkFence _vkFrameFence = VK_NULL_HANDLE;

    bool _needsBgraSwizzle = false;
};

}
