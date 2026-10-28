// Xbox Game Input (GameInput API) 支持
// - 挂钩 GameInputCreate 捕获 IGameInput
// - 每帧轮询手柄并映射为 ImGui 游戏手柄导航键（NavEnableGamepad）
// - 菜单打开时通过 vtable 挂钩 IGameInput::GetCurrentReading 屏蔽输入传给游戏
#include "pch.h"
#include "imgui_hook.h"

#include "imgui.h"
#include "minhook/include/MinHook.h"
#include "debug_log.h"
#include <GameInput.h>

#include <cmath>
#include <cstring>

namespace
{
    using GameInputCreateFn    = HRESULT(WINAPI*)(IGameInput**);
    using GetCurrentReadingFn  = HRESULT(STDMETHODCALLTYPE*)(IGameInput*, GameInputKind, IGameInputDevice*, IGameInputReading**);
    using LoadLibraryExWFn     = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
    using LoadLibraryWFn       = HMODULE(WINAPI*)(LPCWSTR);

    GameInputCreateFn   oGameInputCreate = nullptr;
    GetCurrentReadingFn oGetCurrentReading = nullptr;   // vtable 挂钩前的原始地址
    LoadLibraryExWFn    oLoadLibraryExW = nullptr;
    LoadLibraryWFn      oLoadLibraryW = nullptr;

    IGameInput* g_gameInput = nullptr;
    bool g_vtableHooked = false;
    bool g_moduleHooked = false;    // GameInputCreate 是否已挂
    bool g_waitingLateLoad = false; // 是否已挂 LoadLibrary* 等待延迟加载
    bool g_menuButtonHeld = false;

    HRESULT WINAPI hkGameInputCreate(IGameInput** gameInput);

    void TryHookGameInputModule()
    {
        if (g_moduleHooked)
            return;
        HMODULE m = GetModuleHandleA("GameInput.dll");
        if (!m)
            return;
        g_moduleHooked = true;  // 只尝试一次

        void* proc = (void*)GetProcAddress(m, "GameInputCreate");
        if (!proc)
            return;
        if (MH_CreateHook(proc, (void*)&hkGameInputCreate, (void**)&oGameInputCreate) == MH_OK)
            MH_EnableHook(proc);
    }

    HRESULT STDMETHODCALLTYPE hkGetCurrentReading(IGameInput* self, GameInputKind kind, IGameInputDevice* device, IGameInputReading** reading)
    {
        // 菜单打开时，游戏读不到任何输入（避免菜单操作时角色还在动）
        if (overlay::IsMenuOpen() && reading)
        {
            *reading = nullptr;
            return S_OK;
        }
        return oGetCurrentReading(self, kind, device, reading);
    }

    bool PatchVtable(IGameInput* gi)
    {
        void** vtable = *(void***)gi;
        if (!vtable)
            return false;

        DWORD oldProtect = 0;
        if (!VirtualProtect(&vtable[4], sizeof(void*), PAGE_READWRITE, &oldProtect))
            return false;
        oGetCurrentReading = (GetCurrentReadingFn)vtable[4];   // IGameInput::GetCurrentReading 为 vtable[4]
        vtable[4] = (void*)&hkGetCurrentReading;
        VirtualProtect(&vtable[4], sizeof(void*), oldProtect, &oldProtect);
        g_vtableHooked = true;
        return true;
    }

    HRESULT WINAPI hkGameInputCreate(IGameInput** gameInput)
    {
        HRESULT hr = oGameInputCreate(gameInput);
        if (SUCCEEDED(hr) && gameInput && *gameInput && g_gameInput == nullptr)
        {
            g_gameInput = *gameInput;
            PatchVtable(g_gameInput);
        }
        return hr;
    }

    HMODULE WINAPI hkLoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags)
    {
        HMODULE m = oLoadLibraryExW(name, file, flags);
        if (m && name && wcsstr(name, L"GameInput.dll"))
            TryHookGameInputModule();
        return m;
    }

    HMODULE WINAPI hkLoadLibraryW(LPCWSTR name)
    {
        HMODULE m = oLoadLibraryW(name);
        if (m && name && wcsstr(name, L"GameInput.dll"))
            TryHookGameInputModule();
        return m;
    }

    void ApplyDeadzone(float& x, float& y, float dz)
    {
        float mag = sqrtf(x * x + y * y);
        if (mag < dz)
        {
            x = 0.f;
            y = 0.f;
            return;
        }
        float scale = (mag - dz) / (1.f - dz) / mag;
        x *= scale;
        y *= scale;
    }
}

namespace gameinput
{
    bool InstallHooks()
    {
        TryHookGameInputModule();   // 已加载则直接挂
        if (!g_moduleHooked)
        {
            // 未加载：挂 LoadLibrary*，等 GameInput.dll 出现时再挂 GameInputCreate
            HMODULE k32 = GetModuleHandleA("kernel32.dll");
            void* pEx = k32 ? (void*)GetProcAddress(k32, "LoadLibraryExW") : nullptr;
            void* pW  = k32 ? (void*)GetProcAddress(k32, "LoadLibraryW") : nullptr;
            if (pEx && MH_CreateHook(pEx, (void*)&hkLoadLibraryExW, (void**)&oLoadLibraryExW) == MH_OK)
            {
                MH_EnableHook(pEx);
                g_waitingLateLoad = true;
            }
            if (pW && MH_CreateHook(pW, (void*)&hkLoadLibraryW, (void**)&oLoadLibraryW) == MH_OK)
            {
                MH_EnableHook(pW);
                g_waitingLateLoad = true;
            }
        }
        return true;
    }

    void UninstallHooks()
    {
        if (g_vtableHooked && g_gameInput)
        {
            void** vtable = *(void***)g_gameInput;
            DWORD oldProtect = 0;
            if (VirtualProtect(&vtable[4], sizeof(void*), PAGE_READWRITE, &oldProtect))
            {
                vtable[4] = (void*)oGetCurrentReading;
                VirtualProtect(&vtable[4], sizeof(void*), oldProtect, &oldProtect);
            }
            g_vtableHooked = false;
        }
        if (g_gameInput)
        {
            g_gameInput->Release();
            g_gameInput = nullptr;
        }
        g_waitingLateLoad = false;
    }

    bool IsAvailable()
    {
        return g_gameInput != nullptr;
    }

    void Poll()
    {
        if (!g_gameInput || ImGui::GetCurrentContext() == nullptr)
            return;

        // 绕过我们自己的 GetCurrentReading 挂钩，避免菜单打开时读不到手柄
        IGameInputReading* reading = nullptr;
        HRESULT hr = g_vtableHooked
            ? oGetCurrentReading(g_gameInput, GameInputKindGamepad, nullptr, &reading)
            : g_gameInput->GetCurrentReading(GameInputKindGamepad, nullptr, &reading);
        if (FAILED(hr) || !reading)
            return;

        GameInputGamepadState s = {};
        bool ok = SUCCEEDED(reading->GetGamepadState(&s));
        reading->Release();
        if (!ok)
            return;

        ImGuiIO& io = ImGui::GetIO();
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;

        // START 键切换菜单（边沿触发）
        bool menuDown = (s.buttons & GameInputGamepadMenu) != 0;
        if (menuDown && !g_menuButtonHeld)
            overlay::ToggleMenu();
        g_menuButtonHeld = menuDown;

        // 面板按键 -> ImGui 游戏手柄键
        io.AddKeyEvent(ImGuiKey_GamepadFaceDown,  (s.buttons & GameInputGamepadA) != 0);
        io.AddKeyEvent(ImGuiKey_GamepadFaceRight, (s.buttons & GameInputGamepadB) != 0);
        io.AddKeyEvent(ImGuiKey_GamepadFaceLeft,  (s.buttons & GameInputGamepadX) != 0);
        io.AddKeyEvent(ImGuiKey_GamepadFaceUp,    (s.buttons & GameInputGamepadY) != 0);
        io.AddKeyEvent(ImGuiKey_GamepadDpadLeft,  (s.buttons & GameInputGamepadDPadLeft) != 0);
        io.AddKeyEvent(ImGuiKey_GamepadDpadRight, (s.buttons & GameInputGamepadDPadRight) != 0);
        io.AddKeyEvent(ImGuiKey_GamepadDpadUp,    (s.buttons & GameInputGamepadDPadUp) != 0);
        io.AddKeyEvent(ImGuiKey_GamepadDpadDown,  (s.buttons & GameInputGamepadDPadDown) != 0);
        io.AddKeyEvent(ImGuiKey_GamepadL1, (s.buttons & GameInputGamepadLeftShoulder) != 0);
        io.AddKeyEvent(ImGuiKey_GamepadR1, (s.buttons & GameInputGamepadRightShoulder) != 0);
        io.AddKeyEvent(ImGuiKey_GamepadL3, (s.buttons & GameInputGamepadLeftThumbstick) != 0);
        io.AddKeyEvent(ImGuiKey_GamepadR3, (s.buttons & GameInputGamepadRightThumbstick) != 0);
        io.AddKeyEvent(ImGuiKey_GamepadStart, menuDown);

        // 左摇杆 -> 导航（带死区）
        float lx = s.leftThumbstickX;
        float ly = s.leftThumbstickY;
        ApplyDeadzone(lx, ly, 0.20f);
        io.AddKeyEvent(ImGuiKey_GamepadLStickLeft,  lx < -0.5f);
        io.AddKeyEvent(ImGuiKey_GamepadLStickRight, lx >  0.5f);
        io.AddKeyEvent(ImGuiKey_GamepadLStickUp,    ly >  0.5f);
        io.AddKeyEvent(ImGuiKey_GamepadLStickDown,  ly < -0.5f);
    }
}
