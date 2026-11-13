#include <windows.h>
#include <vector>
#include <utility>
#include <cwchar>
#include "MinHook.h"

// Injected into explorer.exe. While the app is running (heartbeat in shared memory), a newly
// created top-level CabinetWClass window is made layered and fully transparent *before* it is
// shown, so it never paints a first frame or starts the shell's open animation. The app then
// merges it into an existing window and quits it as usual.

namespace
{
    struct Policy
    {
        volatile LONG enabled;        // 1 while the app wants new windows merged into tabs
        volatile LONG allowNextTick;  // When set, the next created window must stay visible
        volatile LONG heartbeat;      // Updated by the app every second
    };

    Policy* g_policy = nullptr;
    HANDLE g_policyMap = nullptr;

    using CreateWindowExW_t = HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int,
        HWND, HMENU, HINSTANCE, LPVOID);
    CreateWindowExW_t g_createWindowExW = nullptr;

    CRITICAL_SECTION g_trackLock;
    std::vector<std::pair<HWND, DWORD>> g_tracked; // window, tick when it should be unhidden again

    bool CtrlShiftDown()
    {
        return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 &&
            (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    }

    bool PolicyEnabled()
    {
        if (g_policy == nullptr) return false;

        if (static_cast<DWORD>(GetTickCount() - g_policy->heartbeat) > 5'000) return false;
        return g_policy->enabled != 0;
    }

    bool ConsumeAllowNext()
    {
        if (g_policy == nullptr) return false;

        const LONG until = g_policy->allowNextTick;
        if (until == 0) return false;

        if (static_cast<int>(GetTickCount() - static_cast<DWORD>(until)) < 0)
        {
            InterlockedExchange(&g_policy->allowNextTick, 0);
            return true;
        }

        return false;
    }

    BOOL CALLBACK CountCabinetProc(HWND hWnd, LPARAM lParam)
    {
        wchar_t className[64];
        if (GetClassNameW(hWnd, className, 64) > 0 &&
            _wcsicmp(className, L"CabinetWClass") == 0)
        {
            ++*reinterpret_cast<int*>(lParam);
        }
        return TRUE;
    }

    bool AnotherExplorerWindowExists()
    {
        int count = 0;
        EnumWindows(CountCabinetProc, reinterpret_cast<LPARAM>(&count));
        return count > 0;
    }

    HWND WINAPI HookCreateWindowExW(DWORD exStyle, LPCWSTR className, LPCWSTR windowName,
        DWORD style, int x, int y, int width, int height, HWND parent, HMENU menu,
        HINSTANCE instance, LPVOID param)
    {
        bool hide = false;

        if (parent == nullptr && className != nullptr &&
            _wcsicmp(className, L"CabinetWClass") == 0 &&
            PolicyEnabled() && !CtrlShiftDown() && !ConsumeAllowNext() &&
            AnotherExplorerWindowExists())
        {
            exStyle |= WS_EX_LAYERED;
            hide = true;
        }

        const HWND hWnd = g_createWindowExW(exStyle, className, windowName, style, x, y, width,
            height, parent, menu, instance, param);

        if (hide && hWnd != nullptr)
        {
            // Make it transparent immediately, still before the window is first shown.
            SetLayeredWindowAttributes(hWnd, 0, 0, LWA_ALPHA);

            EnterCriticalSection(&g_trackLock);
            g_tracked.emplace_back(hWnd, GetTickCount() + 4'000);
            LeaveCriticalSection(&g_trackLock);
        }

        return hWnd;
    }

    // Safety net: if the app dies between window creation and merging, don't leave an invisible
    // window behind forever.
    DWORD WINAPI UnhideWorker(LPVOID)
    {
        for (;;)
        {
            Sleep(300);

            const DWORD now = GetTickCount();
            std::vector<HWND> due;

            EnterCriticalSection(&g_trackLock);
            for (auto it = g_tracked.begin(); it != g_tracked.end();)
            {
                if (static_cast<int>(now - it->second) >= 0)
                {
                    due.push_back(it->first);
                    it = g_tracked.erase(it);
                }
                else
                {
                    ++it;
                }
            }
            LeaveCriticalSection(&g_trackLock);

            for (const HWND hWnd : due)
            {
                if (IsWindow(hWnd))
                    SetLayeredWindowAttributes(hWnd, 0, 255, LWA_ALPHA);
            }
        }
    }

    DWORD WINAPI InitThread(LPVOID)
    {
        g_policyMap = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE,
            L"Local\\ExplorerTabUtility.HookPolicy");
        if (g_policyMap != nullptr)
        {
            g_policy = static_cast<Policy*>(MapViewOfFile(g_policyMap, FILE_MAP_ALL_ACCESS,
                0, 0, sizeof(Policy)));
        }

        // If the app never created the mapping, stay completely passive.
        if (g_policy == nullptr) return 0;

        InitializeCriticalSection(&g_trackLock);

        if (MH_Initialize() != MH_OK) return 0;

        const MH_STATUS status = MH_CreateHookApi(L"user32.dll", "CreateWindowExW",
            reinterpret_cast<LPVOID>(&HookCreateWindowExW),
            reinterpret_cast<LPVOID*>(&g_createWindowExW));
        if (status != MH_OK) return 0;

        MH_EnableHook(MH_ALL_HOOKS);
        CreateThread(nullptr, 0, UnhideWorker, nullptr, 0, nullptr);
        return 0;
    }
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
        // Hook installation must not run under the loader lock.
        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
