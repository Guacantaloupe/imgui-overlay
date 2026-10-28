// 各渲染后端关闭的统一入口
#include "pch.h"

namespace hooks
{
    void ShutdownD3D11();
    void ShutdownD3D12();
    void ShutdownVulkan();
    void ShutdownOpenGL();

    void ShutdownAll()
    {
        ShutdownD3D11();
        ShutdownD3D12();
        ShutdownVulkan();
        ShutdownOpenGL();
    }
}
