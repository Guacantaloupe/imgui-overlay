// OpenGL 钩子：wglSwapBuffers
#include "pch.h"
#include "imgui_hook.h"

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_win32.h"
#include "minhook/include/MinHook.h"
#include "debug_log.h"

#include <cstring>

#include "kiero/kiero.hpp"
#include "kiero/kiero_opengl.hpp"

namespace
{
    using WglSwapBuffersFn = int(WINAPI*)(HDC);

    WglSwapBuffersFn oWglSwapBuffers = nullptr;
    bool g_initialized = false;

    int WINAPI hkWglSwapBuffers(HDC hdc)
    {
        if (!g_initialized)
        {
            HWND hwnd = WindowFromDC(hdc);
            if (hwnd && overlay::EnsureImGui(hwnd) && ImGui_ImplOpenGL3_Init())
            {
                overlay::SetActiveApi(overlay::RenderApi::OpenGL);
                g_initialized = true;
            }
        }

        if (g_initialized)
        {
            overlay::OnFrameStart();
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            overlay::DrawMenu(overlay::RenderApi::OpenGL);

            ImGui::Render();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        }

        return oWglSwapBuffers(hdc);
    }
}

namespace hooks
{
    bool InstallOpenGL(const kiero::OpenGLOutput& methods)
    {
        // kiero2 的 methods 是 function-name -> pointer 的哈希表
        void* target = nullptr;
        auto it = methods.methods.find("wglSwapBuffers");
        if (it != methods.methods.end())
            target = it->second;
        if (!target)
        {
            ImhLog("[imgui-overlay] OpenGL 未找到 wglSwapBuffers\n");
            return false;
        }
        ImhLog("[imgui-overlay] OpenGL wglSwapBuffers 钩子已安装\n");

        if (MH_CreateHook(target, (void*)&hkWglSwapBuffers, (void**)&oWglSwapBuffers) != MH_OK)
            return false;
        MH_EnableHook(target);
        return true;
    }

    void ShutdownOpenGL()
    {
        if (g_initialized)
        {
            ImGui_ImplOpenGL3_Shutdown();
            g_initialized = false;
        }
    }
}
