// dllmain.cpp : DLL 入口。创建工作线程完成所有图形 API 的检测与挂钩。
#include "pch.h"
#include "imgui_hook.h"
#include "debug_log.h"


static DWORD WINAPI WorkerThread(LPVOID)
{
    ImhLog("worker started");
    // 给目标进程一点时间完成图形模块加载（静态注入时尤其重要）
    ImhLog("worker installing hooks");
    overlay::InstallHooks();
    ImhLog("worker done");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        ImhLog("dll attach");
        DisableThreadLibraryCalls(hModule);
        {
            HANDLE t = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
            ImhLog(t ? "thread created" : "thread create FAILED");
        }
        break;
    case DLL_PROCESS_DETACH:
        overlay::UninstallHooks();
        break;
    }
    return TRUE;
}
