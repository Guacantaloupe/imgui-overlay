// 统一调试日志实现
#include "pch.h"
#include "debug_log.h"
#include <cstdio>
#include <cstdarg>
#include <windows.h>

void ImhLog(const char* fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    OutputDebugStringA(buf);
    OutputDebugStringA("\n");

    static const char* logPath = []() -> const char*
    {
        char env[MAX_PATH] = {};
        return GetEnvironmentVariableA("IMGUI_HOOK_LOG", env, MAX_PATH) ? _strdup(env) : nullptr;
    }();
    if (logPath)
    {
        FILE* f = nullptr;
        if (fopen_s(&f, logPath, "a") == 0 && f)
        {
            fprintf(f, "%s\n", buf);
            fclose(f);
        }
    }
}
