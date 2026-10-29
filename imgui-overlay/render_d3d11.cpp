// Direct3D 11 钩子：IDXGISwapChain::Present / ResizeBuffers
#include "pch.h"
#include "imgui_hook.h"

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include "minhook/include/MinHook.h"
#include "debug_log.h"

#include <d3d11.h>
#include <dxgi.h>

#include "kiero/kiero.hpp"
#include "kiero/kiero_d3d11.hpp"

namespace
{
    using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

    PresentFn       oPresent = nullptr;
    ResizeBuffersFn oResizeBuffers = nullptr;

    ID3D11Device*        g_device = nullptr;
    ID3D11DeviceContext* g_context = nullptr;
    bool                 g_initialized = false;

    HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* swapchain, UINT syncInterval, UINT flags)
    {
        if (!g_initialized)
        {
            if (SUCCEEDED(swapchain->GetDevice(__uuidof(ID3D11Device), (void**)&g_device)))
            {
                g_device->GetImmediateContext(&g_context);

                DXGI_SWAP_CHAIN_DESC desc = {};
                swapchain->GetDesc(&desc);

                if (overlay::EnsureImGui(desc.OutputWindow))
                {
                    g_initialized = ImGui_ImplDX11_Init(g_device, g_context);
                    ImhLog("[imgui-overlay] D3D11 ImGui 后端初始化 -> %d", (int)g_initialized);
                    overlay::SetActiveApi(overlay::RenderApi::D3D11);
                }
            }
        }

        if (g_initialized && overlay::OwnsSwapChain(swapchain))
        {
            overlay::OnFrameStart();
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            overlay::DrawMenu(overlay::RenderApi::D3D11);

            ImGui::Render();
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }

        return oPresent(swapchain, syncInterval, flags);
    }

    HRESULT STDMETHODCALLTYPE hkResizeBuffers(IDXGISwapChain* swapchain, UINT bufferCount,
                                              UINT width, UINT height, DXGI_FORMAT format, UINT flags)
    {
        // 释放旧的设备资源，ImGui 会在下一帧惰性重建
        if (g_initialized)
            ImGui_ImplDX11_InvalidateDeviceObjects();

        return oResizeBuffers(swapchain, bufferCount, width, height, format, flags);
    }
}

namespace hooks
{
    bool InstallD3D11(const kiero::D3D11Output& methods)
    {
        // IDXGISwapChain vtable: Present = 8, ResizeBuffers = 13
        if (methods.swapchain_methods.size() <= 13)
        {
            ImhLog("[imgui-overlay] D3D11 方法表过小\n");
            return false;
        }
        ImhLog("[imgui-overlay] D3D11 Present/ResizeBuffers 钩子已安装\n");

        if (MH_CreateHook(methods.swapchain_methods[8], (void*)&hkPresent, (void**)&oPresent) != MH_OK)
            return false;
        if (MH_CreateHook(methods.swapchain_methods[13], (void*)&hkResizeBuffers, (void**)&oResizeBuffers) != MH_OK)
            return false;

        MH_EnableHook(methods.swapchain_methods[8]);
        MH_EnableHook(methods.swapchain_methods[13]);
        return true;
    }

    void ShutdownD3D11()
    {
        if (g_initialized)
        {
            ImGui_ImplDX11_Shutdown();
            if (g_context) { g_context->Release(); g_context = nullptr; }
            if (g_device) { g_device->Release(); g_device = nullptr; }
            g_initialized = false;
        }
    }
}
