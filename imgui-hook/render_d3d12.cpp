// Direct3D 12 钩子：Present / ExecuteCommandLists / ResizeBuffers
// 参考 DX12-Hook 项目的结构：捕获命令队列，在 Present 前用独立命令列表渲染 ImGui。
#include "pch.h"
#include "imgui_hook.h"

#include "imgui.h"
#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_win32.h"
#include "minhook/include/MinHook.h"
#include "debug_log.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <vector>

#include "kiero/kiero.hpp"
#include "kiero/kiero_d3d12.hpp"

namespace
{
    using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    using ExecuteCommandListsFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

    PresentFn              oPresent = nullptr;
    ResizeBuffersFn        oResizeBuffers = nullptr;
    ExecuteCommandListsFn  oExecuteCommandLists = nullptr;

    struct FrameContext
    {
        ID3D12CommandAllocator*     allocator = nullptr;
        ID3D12Resource*             backBuffer = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = {};
    };

    ID3D12Device*           g_device = nullptr;
    ID3D12CommandQueue*     g_commandQueue = nullptr;      // 从 ExecuteCommandLists 捕获
    ID3D12DescriptorHeap*   g_rtvHeap = nullptr;
    ID3D12DescriptorHeap*   g_srvHeap = nullptr;           // ImGui 字体/纹理用
    ID3D12GraphicsCommandList* g_commandList = nullptr;
    std::vector<FrameContext> g_frames;
    UINT                    g_rtvHandleSize = 0;
    bool                    g_initialized = false;

    void ReleaseFrameResources()
    {
        for (auto& f : g_frames)
        {
            if (f.allocator) { f.allocator->Release(); f.allocator = nullptr; }
            if (f.backBuffer) { f.backBuffer->Release(); f.backBuffer = nullptr; }
        }
        g_frames.clear();

        if (g_rtvHeap) { g_rtvHeap->Release(); g_rtvHeap = nullptr; }
        if (g_commandList) { g_commandList->Release(); g_commandList = nullptr; }
        // g_srvHeap 由 ImGui_ImplDX12_Shutdown 释放，这里保留
    }

    void STDMETHODCALLTYPE hkExecuteCommandLists(ID3D12CommandQueue* queue, UINT numLists, ID3D12CommandList* const* lists)
    {
        // 捕获游戏的主直接队列，我们的 ImGui 命令也提交到它上面
        if (g_commandQueue == nullptr && queue != nullptr)
            g_commandQueue = queue;

        oExecuteCommandLists(queue, numLists, lists);
    }

    bool CreateFrameResources(IDXGISwapChain* swapchain)
    {
        DXGI_SWAP_CHAIN_DESC desc = {};
        swapchain->GetDesc(&desc);
        const UINT count = desc.BufferCount;

        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
        rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvDesc.NumDescriptors = count;
        rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(g_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&g_rtvHeap))))
            return false;

        g_rtvHandleSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE handle = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();

        g_frames.resize(count);
        for (UINT i = 0; i < count; ++i)
        {
            if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_frames[i].allocator))))
                return false;
            if (FAILED(swapchain->GetBuffer(i, IID_PPV_ARGS(&g_frames[i].backBuffer))))
                return false;

            g_device->CreateRenderTargetView(g_frames[i].backBuffer, nullptr, handle);
            g_frames[i].rtv = handle;
            handle.ptr += g_rtvHandleSize;
        }

        ID3D12CommandAllocator* initAllocator = g_frames[0].allocator;
        if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, initAllocator, nullptr, IID_PPV_ARGS(&g_commandList))) ||
            FAILED(g_commandList->Close()))
            return false;

        return true;
    }

    bool EnsureInit(IDXGISwapChain* swapchain)
    {
        if (g_initialized)
            return true;

        if (g_commandQueue == nullptr)
            return false;   // 等 ExecuteCommandLists 捕获到队列

        if (FAILED(swapchain->GetDevice(__uuidof(ID3D12Device), (void**)&g_device)))
            return false;

        DXGI_SWAP_CHAIN_DESC desc = {};
        swapchain->GetDesc(&desc);

        // ImGui 需要的 shader-visible SRV 堆
        D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
        srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvDesc.NumDescriptors = desc.BufferCount;
        srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&g_srvHeap))))
            return false;

        if (!CreateFrameResources(swapchain))
            return false;

        if (!overlay::EnsureImGui(desc.OutputWindow))
            return false;

        if (!ImGui_ImplDX12_Init(g_device, (int)desc.BufferCount, desc.BufferDesc.Format,
                                 g_srvHeap,
                                 g_srvHeap->GetCPUDescriptorHandleForHeapStart(),
                                 g_srvHeap->GetGPUDescriptorHandleForHeapStart()))
            return false;

        overlay::SetActiveApi(overlay::RenderApi::D3D12);
        g_initialized = true;
        return true;
    }

    void STDMETHODCALLTYPE hkPresent(IDXGISwapChain* swapchain, UINT syncInterval, UINT flags)
    {
        if (EnsureInit(swapchain) && overlay::OwnsSwapChain(swapchain))
        {
            const UINT index = ((IDXGISwapChain3*)swapchain)->GetCurrentBackBufferIndex();
            FrameContext& frame = g_frames[index];

            overlay::OnFrameStart();
            ImGui_ImplDX12_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            overlay::DrawMenu(overlay::RenderApi::D3D12);

            ImGui::Render();

            // 记录 ImGui 渲染命令并插到 Present 之前
            frame.allocator->Reset();

            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier.Transition.pResource = frame.backBuffer;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

            g_commandList->Reset(frame.allocator, nullptr);
            g_commandList->ResourceBarrier(1, &barrier);
            g_commandList->OMSetRenderTargets(1, &frame.rtv, FALSE, nullptr);
            g_commandList->SetDescriptorHeaps(1, &g_srvHeap);

            ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_commandList);

            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            g_commandList->ResourceBarrier(1, &barrier);
            g_commandList->Close();

            ID3D12CommandList* lists[] = { g_commandList };
            g_commandQueue->ExecuteCommandLists(1, lists);
        }

        oPresent(swapchain, syncInterval, flags);
    }

    HRESULT STDMETHODCALLTYPE hkResizeBuffers(IDXGISwapChain* swapchain, UINT bufferCount,
                                              UINT width, UINT height, DXGI_FORMAT format, UINT flags)
    {
        // 交换链重建：释放 RTV 相关资源，下一帧 Present 重新创建
        if (g_initialized)
        {
            g_commandQueue = nullptr;
            ReleaseFrameResources();
            g_initialized = false;   // 重新走 EnsureInit（保留 ImGui 上下文与 DX12 后端）
        }

        return oResizeBuffers(swapchain, bufferCount, width, height, format, flags);
    }
}

namespace hooks
{
    bool InstallD3D12(const kiero::D3D12Output& methods)
    {
        // IDXGISwapChain: Present = 8, ResizeBuffers = 13
        // ID3D12CommandQueue: ExecuteCommandLists = 9
        ImhLog("[imgui-hook] D3D12 钩子安装中\n");
        bool ok = true;
        if (methods.swapchain_methods.size() > 13)
        {
            ok &= MH_CreateHook(methods.swapchain_methods[8], (void*)&hkPresent, (void**)&oPresent) == MH_OK;
            ok &= MH_CreateHook(methods.swapchain_methods[13], (void*)&hkResizeBuffers, (void**)&oResizeBuffers) == MH_OK;
        }
        if (methods.command_queue_methods.size() > 9)
            ok &= MH_CreateHook(methods.command_queue_methods[9], (void*)&hkExecuteCommandLists, (void**)&oExecuteCommandLists) == MH_OK;

        if (ok)
        {
            if (methods.swapchain_methods.size() > 13)
            {
                MH_EnableHook(methods.swapchain_methods[8]);
                MH_EnableHook(methods.swapchain_methods[13]);
            }
            if (methods.command_queue_methods.size() > 9)
                MH_EnableHook(methods.command_queue_methods[9]);
        }
        return ok;
    }

    void ShutdownD3D12()
    {
        if (g_initialized)
        {
            ImGui_ImplDX12_Shutdown();
            ReleaseFrameResources();
            if (g_srvHeap) { g_srvHeap->Release(); g_srvHeap = nullptr; }
            if (g_device) { g_device->Release(); g_device = nullptr; }
            g_commandQueue = nullptr;
            g_initialized = false;
        }
    }
}
