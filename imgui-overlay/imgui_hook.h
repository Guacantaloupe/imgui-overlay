// 万能 ImGui 注入：公共 overlay 层
// 负责 ImGui 上下文、窗口子类化、菜单绘制、手柄输入汇总与各渲染 API 的调度。
#pragma once

#include <windows.h>

namespace overlay
{
    enum class RenderApi
    {
        Unknown = 0,
        D3D11,
        D3D12,
        Vulkan,
        OpenGL,
    };

    const char* ApiName(RenderApi api);

    // 安装 / 卸载所有已加载图形 API 的钩子（在独立线程中调用）
    bool InstallHooks();
    void UninstallHooks();

    // ---- 各渲染钩子在第一帧时调用 ----

    // 创建 ImGui 上下文 + Win32 平台后端（幂等，线程安全）
    bool EnsureImGui(HWND window);
    // 每帧渲染前调用：汇总手柄输入等
    void OnFrameStart();
    // 绘制菜单窗口
    void DrawMenu(RenderApi api);
    // 是否由本 overlay 负责该交换链（防止多交换链时重复初始化）
    bool OwnsSwapChain(void* swapchain);

    // ---- 菜单状态 ----
    bool IsMenuOpen();
    void ToggleMenu();
    void SetMenuOpen(bool open);

    // ---- 当前渲染 API ----
    RenderApi GetActiveApi();
    void SetActiveApi(RenderApi api);

    // ---- 窗口子类化 ----
    bool HookWindow(HWND hwnd);
    void UnhookWindow();
    LRESULT CALLBACK HookedWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    HWND TargetWindow();
}
