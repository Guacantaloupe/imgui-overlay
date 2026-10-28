// Vulkan 函数指针表：全部函数在运行时从已加载的 vulkan-1.dll 解析，
// 不链接 vulkan-1.lib，因此 x86 / x64、有无 Vulkan SDK 都能编译，
// 且 DLL 不会因静态导入而在无 Vulkan 的进程中加载失败。
#pragma once
#ifndef VULKAN_FUNCTIONS_H
#define VULKAN_FUNCTIONS_H

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

namespace vkfn
{
    // 从 vulkan-1.dll 解析本模块与 ImGui 后端所需函数。成功后方可调用下方指针。
    bool Load();
    bool Loaded();

    // 通用获取，供 ImGui_ImplVulkan_LoadFunctions 的 loader 回调使用
    PFN_vkVoidFunction GetProc(const char* name);

#define VULKAN_FUNCTION_LIST \
    X(vkGetSwapchainImagesKHR) \
    X(vkCreateRenderPass) \
    X(vkDestroyRenderPass) \
    X(vkCreateImageView) \
    X(vkDestroyImageView) \
    X(vkCreateFramebuffer) \
    X(vkDestroyFramebuffer) \
    X(vkDeviceWaitIdle) \
    X(vkQueueWaitIdle) \
    X(vkQueueSubmit) \
    X(vkCreateCommandPool) \
    X(vkDestroyCommandPool) \
    X(vkAllocateCommandBuffers) \
    X(vkResetCommandBuffer) \
    X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) \
    X(vkCmdBeginRenderPass) \
    X(vkCmdEndRenderPass)

#define X(name) extern PFN_##name name;
    VULKAN_FUNCTION_LIST
#undef X
}

#endif // VULKAN_FUNCTIONS_H
