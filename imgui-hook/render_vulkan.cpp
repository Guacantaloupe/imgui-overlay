// Vulkan 钩子：vkQueuePresentKHR / vkCreateSwapchainKHR 等
// 通过挂钩加载器导出函数 + 拦截 vkGetInstanceProcAddr/vkGetDeviceProcAddr 的返回，
// 覆盖游戏无论用哪种方式解析函数指针的情况。
#include "pch.h"
#include "imgui_hook.h"

#define VK_USE_PLATFORM_WIN32_KHR
#include "vulkan_functions.h"

#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"
#include "backends/imgui_impl_win32.h"
#include "minhook/include/MinHook.h"
#include "debug_log.h"

#include <cstring>
#include <unordered_map>
#include <vector>

using namespace vkfn;

namespace
{
    // ---- 原始函数指针 ----
    using CreateInstanceFn      = VkResult(VKAPI_PTR*)(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
    using CreateDeviceFn        = VkResult(VKAPI_PTR*)(VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice*);
    using GetDeviceQueueFn      = void(VKAPI_PTR*)(VkDevice, uint32_t, uint32_t, VkQueue*);
    using GetDeviceQueue2Fn     = void(VKAPI_PTR*)(VkDevice, const VkDeviceQueueInfo2*, VkQueue*);
    using CreateWin32SurfaceFn  = VkResult(VKAPI_PTR*)(VkInstance, const VkWin32SurfaceCreateInfoKHR*, const VkAllocationCallbacks*, VkSurfaceKHR*);
    using CreateSwapchainFn     = VkResult(VKAPI_PTR*)(VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR*);
    using DestroySwapchainFn    = void(VKAPI_PTR*)(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*);
    using QueuePresentFn        = VkResult(VKAPI_PTR*)(VkQueue, const VkPresentInfoKHR*);
    using GetDeviceProcAddrFn   = PFN_vkVoidFunction(VKAPI_PTR*)(VkDevice, const char*);
    using GetInstanceProcAddrFn = PFN_vkVoidFunction(VKAPI_PTR*)(VkInstance, const char*);

    CreateInstanceFn      oCreateInstance = nullptr;
    CreateDeviceFn        oCreateDevice = nullptr;
    GetDeviceQueueFn      oGetDeviceQueue = nullptr;
    GetDeviceQueue2Fn     oGetDeviceQueue2 = nullptr;
    CreateWin32SurfaceFn  oCreateWin32Surface = nullptr;
    CreateSwapchainFn     oCreateSwapchain = nullptr;
    DestroySwapchainFn    oDestroySwapchain = nullptr;
    QueuePresentFn        oQueuePresent = nullptr;
    GetDeviceProcAddrFn   oGetDeviceProcAddr = nullptr;
    GetInstanceProcAddrFn oGetInstanceProcAddr = nullptr;

    // ---- 状态 ----
    VkInstance       g_instance = VK_NULL_HANDLE;
    VkPhysicalDevice g_physicalDevice = VK_NULL_HANDLE;
    VkDevice         g_device = VK_NULL_HANDLE;
    HWND             g_window = nullptr;
    std::unordered_map<VkQueue, uint32_t> g_queueFamilies;

    struct SwapchainRes
    {
        VkFormat                  format = VK_FORMAT_UNDEFINED;
        VkExtent2D                extent = {};
        uint32_t                  imageCount = 0;
        VkRenderPass              renderPass = VK_NULL_HANDLE;
        std::vector<VkImageView>  imageViews;
        std::vector<VkFramebuffer> framebuffers;
    };
    std::unordered_map<VkSwapchainKHR, SwapchainRes> g_swapchains;
    VkSwapchainKHR g_activeSwapchain = VK_NULL_HANDLE;

    VkCommandPool             g_commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> g_commandBuffers;
    bool          g_imguiInitialized = false;
    uint32_t      g_imguiImageCount = 0;
    VkFormat      g_imguiFormat = VK_FORMAT_UNDEFINED;
    uint32_t      g_imguiQueueFamily = 0;
    VkRenderPass  g_imguiRenderPass = VK_NULL_HANDLE;

    HWND FindMainWindow()
    {
        struct Ctx { DWORD pid; HWND hwnd; };
        Ctx ctx = { GetCurrentProcessId(), nullptr };
        EnumWindows([](HWND hwnd, LPARAM lparam)->BOOL
        {
            auto c = (Ctx*)lparam;
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == c->pid && IsWindowVisible(hwnd) && GetWindowTextLengthW(hwnd) > 0)
            {
                c->hwnd = hwnd;
                return FALSE;
            }
            return TRUE;
        }, (LPARAM)&ctx);
        return ctx.hwnd;
    }

    // ---- 交换链资源 ----
    bool CreateSwapchainResources(VkSwapchainKHR sc, const VkSwapchainCreateInfoKHR& info, SwapchainRes& res)
    {
        res.format = info.imageFormat;
        res.extent = info.imageExtent;

        uint32_t count = 0;
        vkGetSwapchainImagesKHR(g_device, sc, &count, nullptr);
        if (count == 0)
            return false;
        res.imageCount = count;

        std::vector<VkImage> images(count);
        vkGetSwapchainImagesKHR(g_device, sc, &count, images.data());

        VkAttachmentDescription attachment = {};
        attachment.format = res.format;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;          // 保留游戏画面，叠加绘制
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription subpass = {};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &ref;

        VkSubpassDependency dep = {};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask = 0;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo rp = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        rp.attachmentCount = 1;
        rp.pAttachments = &attachment;
        rp.subpassCount = 1;
        rp.pSubpasses = &subpass;
        rp.dependencyCount = 1;
        rp.pDependencies = &dep;
        if (vkCreateRenderPass(g_device, &rp, nullptr, &res.renderPass) != VK_SUCCESS)
            return false;

        res.imageViews.resize(count);
        res.framebuffers.resize(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            VkImageViewCreateInfo iv = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            iv.image = images[i];
            iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
            iv.format = res.format;
            iv.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            iv.subresourceRange.levelCount = 1;
            iv.subresourceRange.layerCount = 1;
            if (vkCreateImageView(g_device, &iv, nullptr, &res.imageViews[i]) != VK_SUCCESS)
                return false;

            VkFramebufferCreateInfo fb = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
            fb.renderPass = res.renderPass;
            fb.attachmentCount = 1;
            fb.pAttachments = &res.imageViews[i];
            fb.width = res.extent.width;
            fb.height = res.extent.height;
            fb.layers = 1;
            if (vkCreateFramebuffer(g_device, &fb, nullptr, &res.framebuffers[i]) != VK_SUCCESS)
                return false;
        }
        return true;
    }

    void DestroySwapchainResources(SwapchainRes& res)
    {
        if (g_device == VK_NULL_HANDLE)
        {
            res = {};
            return;
        }
        for (auto fb : res.framebuffers) if (fb) vkDestroyFramebuffer(g_device, fb, nullptr);
        for (auto iv : res.imageViews)   if (iv) vkDestroyImageView(g_device, iv, nullptr);
        if (res.renderPass) vkDestroyRenderPass(g_device, res.renderPass, nullptr);
        res = {};
    }

    // ---- 实例 / 设备 / 队列捕获 ----
    VkResult VKAPI_PTR hkCreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* alloc, VkInstance* instance)
    {
        VkResult r = oCreateInstance(ci, alloc, instance);
        if (r == VK_SUCCESS && g_instance == VK_NULL_HANDLE)
            g_instance = *instance;
        return r;
    }

    VkResult VKAPI_PTR hkCreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks* alloc, VkDevice* device)
    {
        VkResult r = oCreateDevice(phys, ci, alloc, device);
        if (r == VK_SUCCESS && g_device == VK_NULL_HANDLE)
        {
            g_device = *device;
            g_physicalDevice = phys;
        }
        return r;
    }

    void VKAPI_PTR hkGetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* queue)
    {
        oGetDeviceQueue(device, family, index, queue);
        if (queue && *queue)
            g_queueFamilies[*queue] = family;
    }

    void VKAPI_PTR hkGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* queue)
    {
        oGetDeviceQueue2(device, info, queue);
        if (queue && *queue)
            g_queueFamilies[*queue] = info->queueFamilyIndex;
    }

    VkResult VKAPI_PTR hkCreateWin32Surface(VkInstance inst, const VkWin32SurfaceCreateInfoKHR* ci, const VkAllocationCallbacks* alloc, VkSurfaceKHR* surface)
    {
        VkResult r = oCreateWin32Surface(inst, ci, alloc, surface);
        if (r == VK_SUCCESS && g_window == nullptr)
            g_window = ci->hwnd;
        return r;
    }

    VkResult VKAPI_PTR hkCreateSwapchain(VkDevice device, const VkSwapchainCreateInfoKHR* ci, const VkAllocationCallbacks* alloc, VkSwapchainKHR* sc)
    {
        VkResult r = oCreateSwapchain(device, ci, alloc, sc);
        if (r != VK_SUCCESS || device != g_device || !sc)
            return r;

        // 交换链重建：清理旧资源
        VkSwapchainKHR old = ci->oldSwapchain ? ci->oldSwapchain : (g_activeSwapchain != *sc ? g_activeSwapchain : VK_NULL_HANDLE);
        if (old && old != *sc)
        {
            auto it = g_swapchains.find(old);
            if (it != g_swapchains.end())
            {
                vkDeviceWaitIdle(device);
                DestroySwapchainResources(it->second);
                g_swapchains.erase(it);
            }
        }

        SwapchainRes res = {};
        if (CreateSwapchainResources(*sc, *ci, res))
        {
            g_swapchains[*sc] = res;
            g_activeSwapchain = *sc;
        }
        return r;
    }

    void VKAPI_PTR hkDestroySwapchain(VkDevice device, VkSwapchainKHR sc, const VkAllocationCallbacks* alloc)
    {
        auto it = g_swapchains.find(sc);
        if (it != g_swapchains.end())
        {
            vkDeviceWaitIdle(device);
            DestroySwapchainResources(it->second);
            g_swapchains.erase(it);
            if (g_activeSwapchain == sc)
                g_activeSwapchain = VK_NULL_HANDLE;
        }
        oDestroySwapchain(device, sc, alloc);
    }

    bool EnsureImGuiVulkan(VkQueue queue, uint32_t family, SwapchainRes& res)
    {
        if (g_window == nullptr)
        {
            g_window = FindMainWindow();
            if (g_window == nullptr)
                return false;
        }

        // 交换链格式/数量变化时重建后端
        bool stale = g_imguiInitialized &&
            (g_imguiImageCount != res.imageCount || g_imguiFormat != res.format ||
             g_imguiQueueFamily != family || g_imguiRenderPass != res.renderPass);
        if (stale)
        {
            vkDeviceWaitIdle(g_device);
            ImGui_ImplVulkan_Shutdown();
            if (g_commandPool) { vkDestroyCommandPool(g_device, g_commandPool, nullptr); g_commandPool = VK_NULL_HANDLE; }
            g_commandBuffers.clear();
            g_imguiInitialized = false;
        }

        if (!g_imguiInitialized)
        {
            if (!overlay::EnsureImGui(g_window))
                return false;

            VkCommandPoolCreateInfo pool = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
            pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pool.queueFamilyIndex = family;
            if (vkCreateCommandPool(g_device, &pool, nullptr, &g_commandPool) != VK_SUCCESS)
                return false;

            ImGui_ImplVulkan_InitInfo info = {};
            info.Instance = g_instance;
            info.PhysicalDevice = g_physicalDevice;
            info.Device = g_device;
            info.QueueFamily = family;
            info.Queue = queue;
            info.DescriptorPoolSize = 1000;     // 让后端自建 descriptor pool
            info.RenderPass = res.renderPass;
            info.MinImageCount = 2;
            info.ImageCount = res.imageCount;
            info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
            info.CheckVkResultFn = +[](VkResult err)
            {
                if (err != VK_SUCCESS)
                    ImhLog("[imgui-hook] ImGui Vulkan 后端报错\n");
            };

            if (!ImGui_ImplVulkan_Init(&info) || !ImGui_ImplVulkan_CreateFontsTexture())
                return false;

            g_commandBuffers.resize(res.imageCount);
            VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            ai.commandPool = g_commandPool;
            ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = res.imageCount;
            if (vkAllocateCommandBuffers(g_device, &ai, g_commandBuffers.data()) != VK_SUCCESS)
                return false;

            g_imguiInitialized = true;
            g_imguiImageCount = res.imageCount;
            g_imguiFormat = res.format;
            g_imguiQueueFamily = family;
            g_imguiRenderPass = res.renderPass;
            overlay::SetActiveApi(overlay::RenderApi::Vulkan);
        }
        return true;
    }

    VkResult VKAPI_PTR hkQueuePresent(VkQueue queue, const VkPresentInfoKHR* info)
    {
        if (g_device != VK_NULL_HANDLE && info && info->swapchainCount > 0 &&
            info->pSwapchains && info->pImageIndices)
        {
            VkSwapchainKHR sc = info->pSwapchains[0];
            auto it = g_swapchains.find(sc);
            if (it != g_swapchains.end() && it->second.imageCount > 0)
            {
                SwapchainRes& res = it->second;
                uint32_t imageIndex = info->pImageIndices[0];
                uint32_t family = 0;
                auto famIt = g_queueFamilies.find(queue);
                if (famIt != g_queueFamilies.end())
                    family = famIt->second;

                if (imageIndex < res.framebuffers.size() && EnsureImGuiVulkan(queue, family, res))
                {
                    // 等待队列空闲，确保交换链图像可写、上一帧 ImGui 命令已执行完
                    vkQueueWaitIdle(queue);

                    overlay::OnFrameStart();
                    ImGui_ImplVulkan_NewFrame();
                    ImGui_ImplWin32_NewFrame();
                    ImGui::NewFrame();

                    overlay::DrawMenu(overlay::RenderApi::Vulkan);

                    ImGui::Render();

                    VkCommandBuffer cmd = g_commandBuffers[imageIndex % g_commandBuffers.size()];
                    vkResetCommandBuffer(cmd, 0);

                    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
                    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                    vkBeginCommandBuffer(cmd, &begin);

                    VkRenderPassBeginInfo rp = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
                    rp.renderPass = res.renderPass;
                    rp.framebuffer = res.framebuffers[imageIndex];
                    rp.renderArea.offset = { 0, 0 };
                    rp.renderArea.extent = res.extent;
                    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

                    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);

                    vkCmdEndRenderPass(cmd);
                    vkEndCommandBuffer(cmd);

                    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
                    submit.commandBufferCount = 1;
                    submit.pCommandBuffers = &cmd;
                    vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);

                    vkQueueWaitIdle(queue);   // 保证 ImGui 绘制在 present 前完成
                }
            }
        }
        return oQueuePresent(queue, info);
    }

    // ---- 拦截函数指针解析，覆盖游戏从 ICD 直接取地址的情况 ----
    PFN_vkVoidFunction VKAPI_PTR hkGetInstanceProcAddr(VkInstance instance, const char* name)
    {
        PFN_vkVoidFunction r = oGetInstanceProcAddr(instance, name);
        if (r && name)
        {
            if (!strcmp(name, "vkCreateInstance"))     return (PFN_vkVoidFunction)&hkCreateInstance;
            if (!strcmp(name, "vkCreateWin32SurfaceKHR")) return (PFN_vkVoidFunction)&hkCreateWin32Surface;
        }
        return r;
    }

    PFN_vkVoidFunction VKAPI_PTR hkGetDeviceProcAddr(VkDevice device, const char* name)
    {
        PFN_vkVoidFunction r = oGetDeviceProcAddr(device, name);
        if (r && name)
        {
            if (!strcmp(name, "vkQueuePresentKHR"))     return (PFN_vkVoidFunction)&hkQueuePresent;
            if (!strcmp(name, "vkCreateSwapchainKHR"))  return (PFN_vkVoidFunction)&hkCreateSwapchain;
            if (!strcmp(name, "vkDestroySwapchainKHR")) return (PFN_vkVoidFunction)&hkDestroySwapchain;
            if (!strcmp(name, "vkGetDeviceQueue"))      return (PFN_vkVoidFunction)&hkGetDeviceQueue;
            if (!strcmp(name, "vkGetDeviceQueue2"))     return (PFN_vkVoidFunction)&hkGetDeviceQueue2;
        }
        return r;
    }
}

namespace hooks
{
    bool InstallVulkan()
    {
        if (!vkfn::Load())
            return false;

        // ImGui Vulkan 后端以 VK_NO_PROTOTYPES 编译，需先用自定义 loader 填充其函数表
        static PFN_vkVoidFunction(*imguiLoader)(const char*, void*) = +[](const char* name, void*)->PFN_vkVoidFunction
        {
            return vkfn::GetProc(name);
        };
        if (!ImGui_ImplVulkan_LoadFunctions(imguiLoader, nullptr))
            return false;

        struct Def { const char* name; void* hook; void** orig; };
        Def defs[] = {
            { "vkCreateInstance",      (void*)&hkCreateInstance,      (void**)&oCreateInstance },
            { "vkCreateDevice",        (void*)&hkCreateDevice,        (void**)&oCreateDevice },
            { "vkGetDeviceQueue",      (void*)&hkGetDeviceQueue,      (void**)&oGetDeviceQueue },
            { "vkGetDeviceQueue2",     (void*)&hkGetDeviceQueue2,     (void**)&oGetDeviceQueue2 },
            { "vkGetInstanceProcAddr", (void*)&hkGetInstanceProcAddr, (void**)&oGetInstanceProcAddr },
            { "vkGetDeviceProcAddr",   (void*)&hkGetDeviceProcAddr,   (void**)&oGetDeviceProcAddr },
            { "vkQueuePresentKHR",     (void*)&hkQueuePresent,        (void**)&oQueuePresent },
            { "vkCreateSwapchainKHR",  (void*)&hkCreateSwapchain,     (void**)&oCreateSwapchain },
            { "vkDestroySwapchainKHR", (void*)&hkDestroySwapchain,    (void**)&oDestroySwapchain },
            { "vkCreateWin32SurfaceKHR", (void*)&hkCreateWin32Surface, (void**)&oCreateWin32Surface },
        };

        bool any = false;
        ImhLog("[imgui-hook] Vulkan 钩子安装中\n");
        for (auto& d : defs)
        {
            void* target = (void*)vkfn::GetProc(d.name);
            if (!target)
                continue;
            if (MH_CreateHook(target, d.hook, d.orig) == MH_OK)
            {
                MH_EnableHook(target);
                any = true;
            }
        }
        return any;
    }

    void ShutdownVulkan()
    {
        if (g_imguiInitialized && g_device)
        {
            vkDeviceWaitIdle(g_device);
            ImGui_ImplVulkan_Shutdown();
        }
        for (auto& kv : g_swapchains)
            DestroySwapchainResources(kv.second);
        g_swapchains.clear();
        g_activeSwapchain = VK_NULL_HANDLE;
        if (g_commandPool && g_device)
            vkDestroyCommandPool(g_device, g_commandPool, nullptr);
        g_commandPool = VK_NULL_HANDLE;
        g_commandBuffers.clear();
        g_imguiInitialized = false;
        g_device = VK_NULL_HANDLE;
        g_instance = VK_NULL_HANDLE;
        g_physicalDevice = VK_NULL_HANDLE;
    }
}
