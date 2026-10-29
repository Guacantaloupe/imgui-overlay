// overlay 公共层实现：ImGui 上下文、WndProc 子类化、菜单、API 检测与钩子安装。
#include "pch.h"
#include "imgui_hook.h"
#include "debug_log.h"

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "minhook/include/MinHook.h"

// imgui_impl_win32.h 中故意把该声明放在 #if 0 里，按官方说明在这里手动前置声明
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

#include "kiero/kiero_backends.hpp"

#include <cstdio>
#include <mutex>

// ---- 各渲染 API 钩子模块 ----
namespace hooks
{
    bool InstallD3D11(const kiero::D3D11Output& methods);
    bool InstallD3D12(const kiero::D3D12Output& methods);
    bool InstallVulkan();
    bool InstallOpenGL(const kiero::OpenGLOutput& methods);
    void ShutdownAll();
}

// ---- GameInput (Xbox 手柄等) ----
namespace gameinput
{
    bool InstallHooks();   // 若 GameInput.dll 已加载则立即挂钩，否则挂钩 LoadLibraryExW 等待其出现
    void UninstallHooks();
    void Poll();           // 每帧把手柄状态喂给 ImGui
    bool IsAvailable();    // 当前是否已捕获到 IGameInput
}

static WNDPROC s_originalWndProc = nullptr;
static HWND    s_targetWindow = nullptr;
static bool    s_menuOpen = true;
static bool    s_showDemo = false;
static void*   s_ownedSwapchain = nullptr;   // 我们完成初始化的那个交换链
static overlay::RenderApi s_activeApi = overlay::RenderApi::Unknown;
static std::mutex s_imguiMutex;

void overlay::SetActiveApi(RenderApi api)
{
    if (s_activeApi == RenderApi::Unknown)
        s_activeApi = api;
}

overlay::RenderApi overlay::GetActiveApi()
{
    return s_activeApi;
}

const char* overlay::ApiName(RenderApi api)
{
    switch (api)
    {
    case RenderApi::D3D11:  return "Direct3D 11";
    case RenderApi::D3D12:  return "Direct3D 12";
    case RenderApi::Vulkan: return "Vulkan";
    case RenderApi::OpenGL: return "OpenGL";
    default:                return "Unknown";
    }
}

bool overlay::IsMenuOpen()
{
    return s_menuOpen;
}

void overlay::SetMenuOpen(bool open)
{
    s_menuOpen = open;
}

void overlay::ToggleMenu()
{
    s_menuOpen = !s_menuOpen;
}

bool overlay::OwnsSwapChain(void* swapchain)
{
    // 尚未初始化时第一个交换链即归我们；之后只认已登记的那个
    if (s_ownedSwapchain == nullptr)
    {
        s_ownedSwapchain = swapchain;
        return true;
    }
    return s_ownedSwapchain == swapchain;
}

bool overlay::EnsureImGui(HWND window)
{
    std::lock_guard<std::mutex> lock(s_imguiMutex);
    if (ImGui::GetCurrentContext() != nullptr)
        return true;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   // 禁用 imgui.ini
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;   // Xbox GameInput 手柄导航

    ImGui::StyleColorsDark();

    if (!ImGui_ImplWin32_Init(window))
        return false;

    HookWindow(window);
    return true;
}

void overlay::OnFrameStart()
{
    gameinput::Poll();
}

void overlay::DrawMenu(RenderApi api)
{
    if (!s_menuOpen)
        return;

    ImGui::SetNextWindowSize(ImVec2(360, 220), ImGuiCond_FirstUseEver);
    ImGui::Begin("Universal ImGui Hook");
    ImGui::Text("Render API : %s", ApiName(api));
    ImGui::Text("GameInput  : %s", gameinput::IsAvailable() ? "connected (Xbox controller ready)" : "not found");
    ImGui::Text("FPS        : %.1f", ImGui::GetIO().Framerate);
    ImGui::Separator();
    ImGui::TextWrapped("Press INSERT or gamepad START to toggle this menu.");
    ImGui::Checkbox("Show ImGui Demo Window", &s_showDemo);
    ImGui::End();

    if (s_showDemo)
        ImGui::ShowDemoWindow(&s_showDemo);
}

// ---------------- WndProc 子类化 ----------------

LRESULT CALLBACK overlay::HookedWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    ImGuiIO& io = ImGui::GetIO();

    // 键盘切换菜单
    if (msg == WM_KEYDOWN && wParam == VK_INSERT)
        ToggleMenu();

    // 把消息交给 ImGui 的 Win32 平台后端
    if (ImGui::GetCurrentContext() != nullptr)
        ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);

    // 菜单打开且 ImGui 想独占输入时，吞掉键鼠消息，避免游戏同时响应
    if (s_menuOpen)
    {
        switch (msg)
        {
        case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: case WM_MOUSEMOVE:
            if (io.WantCaptureMouse || io.WantTextInput ||
                msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR)
                return 0;
            break;
        default:
            break;
        }
    }

    return CallWindowProc(s_originalWndProc, hWnd, msg, wParam, lParam);
}

bool overlay::HookWindow(HWND hwnd)
{
    if (s_targetWindow != nullptr || hwnd == nullptr)
        return s_targetWindow != nullptr;

    LONG_PTR prev = SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)HookedWndProc);
    if (prev == 0)
        return false;

    s_originalWndProc = (WNDPROC)prev;
    s_targetWindow = hwnd;
    return true;
}

void overlay::UnhookWindow()
{
    if (s_originalWndProc && s_targetWindow)
        SetWindowLongPtrW(s_targetWindow, GWLP_WNDPROC, (LONG_PTR)s_originalWndProc);
    s_originalWndProc = nullptr;
    s_targetWindow = nullptr;
}

HWND overlay::TargetWindow()
{
    return s_targetWindow;
}

// ---------------- 钩子安装 ----------------


bool overlay::InstallHooks()
{
    ImhLog("[imgui-overlay] InstallHooks 开始");
    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED)
    {
        ImhLog("[imgui-overlay] MinHook 初始化失败");
        return false;
    }

    bool any = false;

    // Direct3D 12
    if (GetModuleHandleA("d3d12.dll") && GetModuleHandleA("dxgi.dll"))
    {
        kiero::D3D12Output out = {};
        if (kiero::locate<kiero::Implementation_D3D12>(nullptr, &out) == kiero::Error_Nil)
        {
            ImhLog("[imgui-overlay] 已定位 D3D12 方法表 (%zu 个交换链方法)", out.swapchain_methods.size());
            any |= hooks::InstallD3D12(out);
        }
    }

    // Direct3D 11
    if (GetModuleHandleA("d3d11.dll") && GetModuleHandleA("dxgi.dll"))
    {
        ImhLog("[imgui-overlay] d3d11/dxgi 已加载，定位方法表中...");
        kiero::D3D11Output out = {};
        auto err = kiero::locate<kiero::Implementation_D3D11>(nullptr, &out);
        ImhLog("[imgui-overlay] kiero locate D3D11 返回 %d", (int)err);
        if (err == kiero::Error_Nil)
        {
            ImhLog("[imgui-overlay] 已定位 D3D11 方法表 (%zu 个交换链方法)", out.swapchain_methods.size());
            any |= hooks::InstallD3D11(out);
        }
    }

    // Vulkan
    if (GetModuleHandleA("vulkan-1.dll"))
        any |= hooks::InstallVulkan();

    // OpenGL
    if (GetModuleHandleA("opengl32.dll"))
    {
        kiero::OpenGLOutput out = {};
        if (kiero::locate<kiero::Implementation_OpenGL>(nullptr, &out) == kiero::Error_Nil)
        {
            ImhLog("[imgui-overlay] 已定位 OpenGL 方法表 (%zu 个导出)", out.methods.size());
            any |= hooks::InstallOpenGL(out);
        }
    }

    // Xbox GameInput（无论图形 API 都尝试）
    gameinput::InstallHooks();

    if (!any)
        ImhLog("[imgui-overlay] 未找到任何可挂钩的图形 API");
    return any;
}

void overlay::UninstallHooks()
{
    hooks::ShutdownAll();
    gameinput::UninstallHooks();
    UnhookWindow();

    if (ImGui::GetCurrentContext())
    {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }

    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}
