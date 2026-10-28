// 统一调试日志：OutputDebugStringA + 可选文件输出（环境变量 IMGUI_HOOK_LOG=<路径>）
#pragma once
#ifndef DEBUG_LOG_H
#define DEBUG_LOG_H

void ImhLog(const char* fmt, ...);

#endif // DEBUG_LOG_H
