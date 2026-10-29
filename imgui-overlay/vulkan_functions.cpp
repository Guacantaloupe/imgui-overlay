// Vulkan 函数指针表实现
#include "pch.h"
#include "vulkan_functions.h"
#include <windows.h>

namespace vkfn
{
    static HMODULE g_vulkanModule = nullptr;

#define X(name) PFN_##name name = nullptr;
    VULKAN_FUNCTION_LIST
#undef X

    PFN_vkVoidFunction GetProc(const char* name)
    {
        if (!g_vulkanModule)
            return nullptr;
        return (PFN_vkVoidFunction)GetProcAddress(g_vulkanModule, name);
    }

    bool Loaded()
    {
        return g_vulkanModule != nullptr;
    }

    bool Load()
    {
        if (g_vulkanModule)
            return true;

        g_vulkanModule = GetModuleHandleA("vulkan-1.dll");
        if (!g_vulkanModule)
            return false;

#define X(name) \
        name = (PFN_##name)GetProcAddress(g_vulkanModule, #name); \
        if (!name) { OutputDebugStringA("[imgui-overlay] Vulkan 函数缺失: " #name "\n"); return false; }
        VULKAN_FUNCTION_LIST
#undef X

        return true;
    }
}
