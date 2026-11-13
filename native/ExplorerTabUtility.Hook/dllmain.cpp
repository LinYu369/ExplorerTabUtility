#include <windows.h>
#include <cstdio>
#include <cstdarg>
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
    constexpr int CondemnedSlots = 8;
    constexpr int ReleasedSlots = 8;

    struct Policy
    {
        volatile LONG enabled;        // 1 while the app wants new windows merged into tabs
        volatile LONG allowNextTick;  // When set, the next created window must stay visible
        volatile LONG heartbeat;      // Updated by the app every second
        volatile LONG reserved;
        // Windows the app has already merged into a tab and quit. Explorer still shows them
        // once while tearing them down; those shows must never reach the screen.
        volatile ULONGLONG condemned[CondemnedSlots];
        // Windows the app decided to keep. Explicit, because Explorer also strips WS_EX_LAYERED
        // while initializing, so "no layered style" alone cannot tell the two apart.
        volatile ULONGLONG released[ReleasedSlots];
    };

    Policy* g_policy = nullptr;
    HANDLE g_policyMap = nullptr;

    using CreateWindowExW_t = HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int,
        HWND, HMENU, HINSTANCE, LPVOID);
    CreateWindowExW_t g_createWindowExW = nullptr;

    using ShowWindow_t = BOOL(WINAPI*)(HWND, int);
    ShowWindow_t g_showWindow = nullptr;

    using SetWindowPos_t = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
    SetWindowPos_t g_setWindowPos = nullptr;

    using DestroyWindow_t = BOOL(WINAPI*)(HWND);
    DestroyWindow_t g_destroyWindow = nullptr;

    CRITICAL_SECTION g_trackLock;
    std::vector<HWND> g_tracked;

    void Log(const char* format, ...)
    {
        char path[MAX_PATH];
        if (GetTempPathA(MAX_PATH, path) == 0) return;
        strcat_s(path, "ExplorerTabUtility-hook.log");

        FILE* file = nullptr;
        if (fopen_s(&file, path, "a") != 0 || file == nullptr) return;

        SYSTEMTIME time;
        GetLocalTime(&time);
        std::fprintf(file, "%02d:%02d:%02d.%03d ", time.wHour, time.wMinute, time.wSecond,
            time.wMilliseconds);

        va_list args;
        va_start(args, format);
        std::vfprintf(file, format, args);
        va_end(args);

        std::fprintf(file, "\n");
        std::fclose(file);
    }

    bool IsTracked(HWND hWnd)
    {
        bool found = false;
        EnterCriticalSection(&g_trackLock);
        for (const HWND tracked : g_tracked)
        {
            if (tracked == hWnd)
            {
                found = true;
                break;
            }
        }
        LeaveCriticalSection(&g_trackLock);
        return found;
    }

    void Untrack(HWND hWnd)
    {
        EnterCriticalSection(&g_trackLock);
        for (auto it = g_tracked.begin(); it != g_tracked.end(); ++it)
        {
            if (*it == hWnd)
            {
                g_tracked.erase(it);
                break;
            }
        }
        LeaveCriticalSection(&g_trackLock);
    }

    void MakeTransparent(HWND hWnd)
    {
        const LONG exStyle = GetWindowLongW(hWnd, GWL_EXSTYLE);
        if ((exStyle & WS_EX_LAYERED) == 0)
            SetWindowLongW(hWnd, GWL_EXSTYLE, exStyle | WS_EX_LAYERED);

        SetLayeredWindowAttributes(hWnd, 0, 0, LWA_ALPHA);
    }

    // The app releases a window by setting alpha back to 255, plus an explicit released slot.
    // A failed attribute read is NOT a release: Explorer strips WS_EX_LAYERED while initializing
    // a window that must still stay hidden.
    bool AppRestored(HWND hWnd)
    {
        BYTE alpha = 0;
        DWORD flags = 0;
        if (GetLayeredWindowAttributes(hWnd, nullptr, &alpha, &flags) == FALSE) return false;
        return (flags & LWA_ALPHA) != 0 && alpha != 0;
    }

    bool CtrlShiftDown()
    {
        return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 &&
            (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    }

    bool PolicyAlive()
    {
        if (g_policy == nullptr) return false;
        return static_cast<DWORD>(GetTickCount() - g_policy->heartbeat) <= 5'000;
    }

    bool PolicyEnabled()
    {
        return PolicyAlive() && g_policy->enabled != 0;
    }

    bool AllowWindowActive()
    {
        if (g_policy == nullptr) return false;

        const LONG until = g_policy->allowNextTick;
        if (until == 0) return false;

        // A time window, not a one-shot flag: a single user action (e.g. detaching a tab) can
        // produce more than one window, and all of them must stay visible.
        return static_cast<int>(GetTickCount() - static_cast<DWORD>(until)) < 0;
    }

    bool IsCondemned(HWND hWnd)
    {
        if (g_policy == nullptr || hWnd == nullptr || !PolicyAlive()) return false;

        const auto value = static_cast<ULONGLONG>(reinterpret_cast<UINT_PTR>(hWnd));
        for (int i = 0; i < CondemnedSlots; ++i)
            if (g_policy->condemned[i] == value) return true;

        return false;
    }

    void ClearCondemned(HWND hWnd)
    {
        if (g_policy == nullptr || hWnd == nullptr) return;

        const auto value = static_cast<ULONGLONG>(reinterpret_cast<UINT_PTR>(hWnd));
        for (int i = 0; i < CondemnedSlots; ++i)
        {
            if (g_policy->condemned[i] == value)
                InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&g_policy->condemned[i]), 0);
        }
    }

    // Consumes an explicit "the app keeps this window" notice and stops managing the handle.
    bool ConsumeRelease(HWND hWnd)
    {
        if (g_policy == nullptr || hWnd == nullptr) return false;

        const auto value = static_cast<ULONGLONG>(reinterpret_cast<UINT_PTR>(hWnd));
        bool found = false;
        for (int i = 0; i < ReleasedSlots; ++i)
        {
            if (g_policy->released[i] != value) continue;
            InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&g_policy->released[i]), 0);
            found = true;
        }

        if (found)
        {
            Untrack(hWnd);
            // The window is meant to be used now, so let it take focus again.
            const LONG exStyle = GetWindowLongW(hWnd, GWL_EXSTYLE);
            if ((exStyle & WS_EX_NOACTIVATE) != 0)
                SetWindowLongW(hWnd, GWL_EXSTYLE, exStyle & ~WS_EX_NOACTIVATE);
        }
        return found;
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
            _wcsicmp(className, L"CabinetWClass") == 0)
        {
            const bool enabled = PolicyEnabled();
            const bool ctrlShift = CtrlShiftDown();
            const bool allow = AllowWindowActive();
            const bool other = AnotherExplorerWindowExists();
            hide = enabled && !ctrlShift && !allow && other;

            Log("CREATE enabled=%d ctrlShift=%d allow=%d other=%d hide=%d visible=%d",
                enabled, ctrlShift, allow, other, hide, (style & WS_VISIBLE) != 0);

            if (hide)
                exStyle |= WS_EX_LAYERED | WS_EX_NOACTIVATE;
        }

        const HWND hWnd = g_createWindowExW(exStyle, className, windowName, style, x, y, width,
            height, parent, menu, instance, param);

        if (hide && hWnd != nullptr)
        {
            // Make it transparent immediately, still before the window is first shown.
            MakeTransparent(hWnd);

            EnterCriticalSection(&g_trackLock);
            g_tracked.push_back(hWnd);
            LeaveCriticalSection(&g_trackLock);
        }

        return hWnd;
    }

    BOOL WINAPI HookShowWindow(HWND hWnd, int nCmdShow)
    {
        if (hWnd != nullptr)
        {
            if (ConsumeRelease(hWnd))
                return g_showWindow(hWnd, nCmdShow);

            // The app merged this window into a tab and quit it. Explorer still shows it once
            // while tearing it down, which activates it and puts it on screen and in the
            // taskbar. Restoring transparency alone is not enough: by then the show/activate
            // already happened. Turn it into a hide instead, so the outcome no longer depends
            // on which side wins the race.
            if (nCmdShow != SW_HIDE && IsCondemned(hWnd) && !AppRestored(hWnd))
            {
                Log("SHOWWINDOW hwnd=%p cmd=%d condemned=1 suppressed=1", hWnd, nCmdShow);
                return g_showWindow(hWnd, SW_HIDE);
            }

            // Explorer may reset window styles while initializing. Re-apply transparency at the
            // last moment before the window is actually shown.
            if (IsTracked(hWnd))
            {
                BYTE alpha = 0;
                DWORD flags = 0;
                const bool read = GetLayeredWindowAttributes(hWnd, nullptr, &alpha, &flags) != FALSE;
                const bool rehide = !AppRestored(hWnd);

                Log("SHOWWINDOW hwnd=%p cmd=%d read=%d alpha=%d rehide=%d", hWnd, nCmdShow, read, alpha, rehide);
                if (rehide)
                    MakeTransparent(hWnd);
            }
        }

        return g_showWindow(hWnd, nCmdShow);
    }

    // Explorer does not show every window through ShowWindow: SetWindowPos(SWP_SHOWWINDOW) is
    // the other common path, and it bypasses the hook above entirely.
    BOOL WINAPI HookSetWindowPos(HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx, int cy,
        UINT uFlags)
    {
        if (hWnd != nullptr && (uFlags & SWP_SHOWWINDOW) != 0)
        {
            if (ConsumeRelease(hWnd))
                return g_setWindowPos(hWnd, hWndInsertAfter, X, Y, cx, cy, uFlags);

            if (IsCondemned(hWnd) && !AppRestored(hWnd))
            {
                Log("SETWINDOWPOS hwnd=%p flags=0x%x condemned=1 suppressed=1", hWnd, uFlags);
                uFlags = (uFlags & ~SWP_SHOWWINDOW) | SWP_HIDEWINDOW;
            }
            else if (IsTracked(hWnd) && !AppRestored(hWnd))
            {
                Log("SETWINDOWPOS hwnd=%p flags=0x%x rehide=1", hWnd, uFlags);
                MakeTransparent(hWnd);
            }
        }

        return g_setWindowPos(hWnd, hWndInsertAfter, X, Y, cx, cy, uFlags);
    }

    BOOL WINAPI HookDestroyWindow(HWND hWnd)
    {
        const BOOL result = g_destroyWindow(hWnd);

        // Release the handle only after the window is really gone, so a recycled HWND can never
        // inherit the suppression and hide a window the user is supposed to see.
        if (hWnd != nullptr)
        {
            ClearCondemned(hWnd);
            Untrack(hWnd);
        }

        return result;
    }

    // While the app is alive it owns the show/hide decision for every tracked window: it either
    // merges and quits it, or releases it. The hook must never restore opacity on its own here,
    // because doing that mid-merge is exactly what flashes a fully opaque window on screen.
    // Only when the app is gone do we reveal, so no invisible window is left behind.
    DWORD WINAPI UnhideWorker(LPVOID)
    {
        for (;;)
        {
            Sleep(300);

            const bool alive = PolicyAlive();

            std::vector<HWND> snapshot;
            EnterCriticalSection(&g_trackLock);
            snapshot = g_tracked;
            LeaveCriticalSection(&g_trackLock);

            std::vector<HWND> reveal;
            std::vector<HWND> drop;

            // Query window state outside the lock: these are user32 calls and the hook
            // callbacks contend on the same section.
            for (const HWND hWnd : snapshot)
            {
                if (!IsWindow(hWnd))
                {
                    drop.push_back(hWnd);
                    continue;
                }

                if (ConsumeRelease(hWnd) || AppRestored(hWnd))
                {
                    drop.push_back(hWnd);
                    continue;
                }

                if (IsCondemned(hWnd))
                {
                    if (IsWindowVisible(hWnd))
                        g_showWindow(hWnd, SW_HIDE);
                    drop.push_back(hWnd);
                    continue;
                }

                if (!alive)
                {
                    reveal.push_back(hWnd);
                    drop.push_back(hWnd);
                    continue;
                }

                // Explorer can strip WS_EX_LAYERED while initializing. Put it back before the
                // window gets a chance to paint opaque. A successful read with alpha 0 needs
                // nothing: the window is already invisible.
                BYTE alpha = 0;
                DWORD flags = 0;
                if (GetLayeredWindowAttributes(hWnd, nullptr, &alpha, &flags) == FALSE)
                    MakeTransparent(hWnd);
            }

            if (!drop.empty())
            {
                EnterCriticalSection(&g_trackLock);
                for (auto it = g_tracked.begin(); it != g_tracked.end();)
                {
                    bool remove = false;
                    for (const HWND hWnd : drop)
                        if (*it == hWnd) { remove = true; break; }

                    if (remove) it = g_tracked.erase(it);
                    else ++it;
                }
                LeaveCriticalSection(&g_trackLock);
            }

            for (const HWND hWnd : reveal)
            {
                if (!IsWindow(hWnd)) continue;
                SetLayeredWindowAttributes(hWnd, 0, 255, LWA_ALPHA);
                const LONG exStyle = GetWindowLongW(hWnd, GWL_EXSTYLE);
                if ((exStyle & WS_EX_NOACTIVATE) != 0)
                    SetWindowLongW(hWnd, GWL_EXSTYLE, exStyle & ~WS_EX_NOACTIVATE);
            }

            // DestroyWindow clears condemned slots precisely; this only catches windows that
            // went away without it (e.g. the whole process exited).
            if (g_policy != nullptr)
            {
                for (int i = 0; i < CondemnedSlots; ++i)
                {
                    const ULONGLONG value = g_policy->condemned[i];
                    if (value == 0) continue;

                    const HWND hWnd = reinterpret_cast<HWND>(static_cast<UINT_PTR>(value));
                    if (!IsWindow(hWnd))
                        InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&g_policy->condemned[i]), 0);
                }
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
        if (g_policy == nullptr)
        {
            Log("INIT no policy mapping");
            return 0;
        }

        InitializeCriticalSection(&g_trackLock);

        if (MH_Initialize() != MH_OK)
        {
            Log("INIT MH_Initialize failed");
            return 0;
        }

        const MH_STATUS createStatus = MH_CreateHookApi(L"user32.dll", "CreateWindowExW",
            reinterpret_cast<LPVOID>(&HookCreateWindowExW),
            reinterpret_cast<LPVOID*>(&g_createWindowExW));
        const MH_STATUS showStatus = MH_CreateHookApi(L"user32.dll", "ShowWindow",
            reinterpret_cast<LPVOID>(&HookShowWindow),
            reinterpret_cast<LPVOID*>(&g_showWindow));
        const MH_STATUS posStatus = MH_CreateHookApi(L"user32.dll", "SetWindowPos",
            reinterpret_cast<LPVOID>(&HookSetWindowPos),
            reinterpret_cast<LPVOID*>(&g_setWindowPos));
        const MH_STATUS destroyStatus = MH_CreateHookApi(L"user32.dll", "DestroyWindow",
            reinterpret_cast<LPVOID>(&HookDestroyWindow),
            reinterpret_cast<LPVOID*>(&g_destroyWindow));

        Log("INIT createHook=%d showHook=%d posHook=%d destroyHook=%d",
            createStatus, showStatus, posStatus, destroyStatus);
        if (createStatus != MH_OK || showStatus != MH_OK) return 0;

        // SetWindowPos and DestroyWindow widen coverage; neither may take the whole hook down
        // if it cannot be installed on a given build of Windows.
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