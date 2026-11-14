#include <windows.h>
#include <dwmapi.h>
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

    using ShowWindowAsync_t = BOOL(WINAPI*)(HWND, int);
    ShowWindowAsync_t g_showWindowAsync = nullptr;

    // Explorer sometimes shows windows through win32u directly, bypassing every user32 export.
    using NtUserShowWindow_t = BOOL(NTAPI*)(HWND, int);
    NtUserShowWindow_t g_ntUserShowWindow = nullptr;

    using NtUserSetWindowPos_t = BOOL(NTAPI*)(HWND, HWND, int, int, int, int, UINT);
    NtUserSetWindowPos_t g_ntUserSetWindowPos = nullptr;

    using SetWindowPos_t = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
    SetWindowPos_t g_setWindowPos = nullptr;

    using DestroyWindow_t = BOOL(WINAPI*)(HWND);
    DestroyWindow_t g_destroyWindow = nullptr;

    // Windows can be made visible by changing GWL_STYLE or by SetWindowPlacement instead of
    // going through any ShowWindow/SetWindowPos export.
    using SetWindowLongW_t = LONG(WINAPI*)(HWND, int, LONG);
    SetWindowLongW_t g_setWindowLongW = nullptr;

    using SetWindowLongPtrW_t = LONG_PTR(WINAPI*)(HWND, int, LONG_PTR);
    SetWindowLongPtrW_t g_setWindowLongPtrW = nullptr;

    using SetWindowPlacement_t = BOOL(WINAPI*)(HWND, const WINDOWPLACEMENT*);
    SetWindowPlacement_t g_setWindowPlacement = nullptr;

    CRITICAL_SECTION g_trackLock;
    std::vector<HWND> g_tracked;

    // Set while the hooked CreateWindowExW is building a temporary CabinetWClass window.
    // Explorer shows the window *inside* that call, before the post-create code has tracked it.
    thread_local bool t_creatingTarget = false;

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

    bool IsCabinetWindow(HWND hWnd)
    {
        wchar_t className[64];
        return hWnd != nullptr && GetClassNameW(hWnd, className, 64) > 0 &&
            _wcsicmp(className, L"CabinetWClass") == 0;
    }

    // A temporary window that is still inside the hooked CreateWindowExW call. It is not in
    // g_tracked yet, but Explorer may already show it and start the open animation.
    bool IsPretrackedTarget(HWND hWnd)
    {
        return t_creatingTarget && !IsTracked(hWnd) && IsCabinetWindow(hWnd);
    }

    void MakeTransparent(HWND hWnd)
    {
        const LONG exStyle = GetWindowLongW(hWnd, GWL_EXSTYLE);
        if ((exStyle & WS_EX_LAYERED) == 0)
            SetWindowLongW(hWnd, GWL_EXSTYLE, exStyle | WS_EX_LAYERED);

        SetLayeredWindowAttributes(hWnd, 0, 0, LWA_ALPHA);
    }

    // Layered transparency only stops the window from painting. DWM still composes the window
    // frame and plays the shell's open animation. A cloaked window is not composed at all, so
    // the temporary window never animates on screen regardless of which code path shows it.
    // Use explicit attribute ids so the hook builds with older SDK headers as well.
    constexpr DWORD DwmwaCloak = 13;
    constexpr DWORD DwmwaCloaked = 14;

    HRESULT SetCloaked(HWND hWnd, bool cloak)
    {
        const BOOL value = cloak ? TRUE : FALSE;
        return DwmSetWindowAttribute(hWnd, DwmwaCloak, &value, sizeof(value));
    }

    bool IsCloaked(HWND hWnd)
    {
        int cloaked = 0;
        if (DwmGetWindowAttribute(hWnd, DwmwaCloaked, &cloaked, sizeof(cloaked)) != S_OK)
            return false;
        return cloaked != 0;
    }

    void SuppressPretracked(HWND hWnd, const char* api)
    {
        MakeTransparent(hWnd);
        if (!IsCloaked(hWnd))
            SetCloaked(hWnd, true);

        Log("%s hwnd=%p pretracked=1 suppressed=1", api, hWnd);
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
            SetCloaked(hWnd, false);
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

        const bool previousCreatingTarget = t_creatingTarget;
        if (hide)
            t_creatingTarget = true;

        const HWND hWnd = g_createWindowExW(exStyle, className, windowName, style, x, y, width,
            height, parent, menu, instance, param);

        t_creatingTarget = previousCreatingTarget;

        if (hide && hWnd != nullptr)
        {
            // Kill every visual trace before Explorer gets a chance to show the window: no
            // content (layered alpha 0), no DWM composition, and no open animation (cloak).
            MakeTransparent(hWnd);
            const HRESULT cloakResult = SetCloaked(hWnd, true);
            Log("CREATE cloak=0x%08X", cloakResult);

            // A window created with WS_VISIBLE is already on its way to the screen; take it
            // back before it can start the open animation.
            if ((style & WS_VISIBLE) != 0)
                g_showWindow(hWnd, SW_HIDE);

            EnterCriticalSection(&g_trackLock);
            g_tracked.push_back(hWnd);
            LeaveCriticalSection(&g_trackLock);
        }

        return hWnd;
    }

    // Returns the command that should actually be forwarded. A tracked window is a temporary
    // window the app has not decided about yet. It may still be shown by Explorer (some show
    // paths bypass our hooks), so it is made transparent AND DWM-cloaked before the show
    // reaches the window: a cloaked window is never composed, so no open animation is played.
    // The app releases the handle when it wants to keep the window.
    int FilterShow(HWND hWnd, int nCmdShow, const char* api)
    {
        if (hWnd == nullptr) return nCmdShow;
        if (ConsumeRelease(hWnd)) return nCmdShow;

        const bool tracked = IsTracked(hWnd);
        const bool pretracked = !tracked && IsPretrackedTarget(hWnd);
        if (!tracked && !pretracked) return nCmdShow;

        if (pretracked)
        {
            if (nCmdShow != SW_HIDE)
            {
                SuppressPretracked(hWnd, api);
                return SW_HIDE;
            }

            MakeTransparent(hWnd);
            if (!IsCloaked(hWnd))
                SetCloaked(hWnd, true);
            return nCmdShow;
        }

        if (nCmdShow != SW_HIDE && IsCondemned(hWnd) && !AppRestored(hWnd))
        {
            Log("%s hwnd=%p cmd=%d condemned=1 suppressed=1", api, hWnd, nCmdShow);
            return SW_HIDE;
        }

        if (AppRestored(hWnd))
        {
            SetCloaked(hWnd, false);
            Untrack(hWnd);
            return nCmdShow;
        }

        MakeTransparent(hWnd);
        if (!IsCloaked(hWnd))
            SetCloaked(hWnd, true);

        // Never let the temporary window become visible: that is what starts the DWM open
        // animation (and the accent border of an activating maximized window). The app
        // releases the handle before showing a window it wants to keep.
        if (nCmdShow != SW_HIDE)
        {
            Log("%s hwnd=%p cmd=%d tracked=1 suppressed=1", api, hWnd, nCmdShow);
            return SW_HIDE;
        }

        return nCmdShow;
    }

    BOOL WINAPI HookShowWindow(HWND hWnd, int nCmdShow)
    {
        return g_showWindow(hWnd, FilterShow(hWnd, nCmdShow, "SHOWWINDOW"));
    }

    // ShowWindowAsync is a separate export; Explorer uses it on some code paths.
    BOOL WINAPI HookShowWindowAsync(HWND hWnd, int nCmdShow)
    {
        return g_showWindowAsync(hWnd, FilterShow(hWnd, nCmdShow, "SHOWWINDOWASYNC"));
    }

    // The lowest-level show path. Explorer bypasses every user32 export on this Windows build,
    // so this hook is what actually keeps the temporary window from becoming visible.
    BOOL NTAPI HookNtUserShowWindow(HWND hWnd, int nCmdShow)
    {
        return g_ntUserShowWindow(hWnd, FilterShow(hWnd, nCmdShow, "NTUSER_SHOWWINDOW"));
    }

    UINT FilterSetWindowPos(HWND hWnd, UINT uFlags, const char* api)
    {
        if (hWnd == nullptr || (uFlags & SWP_SHOWWINDOW) == 0) return uFlags;
        if (ConsumeRelease(hWnd)) return uFlags;

        const bool tracked = IsTracked(hWnd);
        const bool pretracked = !tracked && IsPretrackedTarget(hWnd);
        if (!tracked && !pretracked) return uFlags;

        if (pretracked)
        {
            SuppressPretracked(hWnd, api);
            return (uFlags & ~SWP_SHOWWINDOW) | SWP_HIDEWINDOW;
        }

        if (IsCondemned(hWnd) && !AppRestored(hWnd))
        {
            Log("%s hwnd=%p flags=0x%x condemned=1 suppressed=1", api, hWnd, uFlags);
            return (uFlags & ~SWP_SHOWWINDOW) | SWP_HIDEWINDOW;
        }

        if (AppRestored(hWnd))
        {
            SetCloaked(hWnd, false);
            Untrack(hWnd);
            return uFlags;
        }

        MakeTransparent(hWnd);
        if (!IsCloaked(hWnd))
            SetCloaked(hWnd, true);

        Log("%s hwnd=%p flags=0x%x tracked=1 suppressed=1", api, hWnd, uFlags);
        return (uFlags & ~SWP_SHOWWINDOW) | SWP_HIDEWINDOW;
    }

    // Explorer does not show every window through ShowWindow: SetWindowPos(SWP_SHOWWINDOW) is
    // the other common path, and it bypasses the hook above entirely.
    BOOL WINAPI HookSetWindowPos(HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx, int cy,
        UINT uFlags)
    {
        uFlags = FilterSetWindowPos(hWnd, uFlags, "SETWINDOWPOS");
        return g_setWindowPos(hWnd, hWndInsertAfter, X, Y, cx, cy, uFlags);
    }

    BOOL NTAPI HookNtUserSetWindowPos(HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx,
        int cy, UINT uFlags)
    {
        uFlags = FilterSetWindowPos(hWnd, uFlags, "NTUSER_SETWINDOWPOS");
        return g_ntUserSetWindowPos(hWnd, hWndInsertAfter, X, Y, cx, cy, uFlags);
    }

    LONG WINAPI HookSetWindowLongW(HWND hWnd, int nIndex, LONG dwNewLong)
    {
        if (nIndex == GWL_STYLE && (dwNewLong & WS_VISIBLE) != 0 &&
            IsPretrackedTarget(hWnd))
        {
            SuppressPretracked(hWnd, "SETWINDOWLONG");
            dwNewLong &= ~WS_VISIBLE;
        }
        return g_setWindowLongW(hWnd, nIndex, dwNewLong);
    }

    LONG_PTR WINAPI HookSetWindowLongPtrW(HWND hWnd, int nIndex, LONG_PTR dwNewLong)
    {
        if (nIndex == GWL_STYLE && (dwNewLong & WS_VISIBLE) != 0 &&
            IsPretrackedTarget(hWnd))
        {
            SuppressPretracked(hWnd, "SETWINDOWLONGPTR");
            dwNewLong &= ~WS_VISIBLE;
        }
        return g_setWindowLongPtrW(hWnd, nIndex, dwNewLong);
    }

    BOOL WINAPI HookSetWindowPlacement(HWND hWnd, const WINDOWPLACEMENT* placement)
    {
        if (placement != nullptr && placement->showCmd != SW_HIDE &&
            IsPretrackedTarget(hWnd))
        {
            WINDOWPLACEMENT hidden = *placement;
            hidden.showCmd = SW_HIDE;
            SuppressPretracked(hWnd, "SETWINDOWPLACEMENT");
            return g_setWindowPlacement(hWnd, &hidden);
        }
        return g_setWindowPlacement(hWnd, placement);
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
            bool anyTracked;
            EnterCriticalSection(&g_trackLock);
            anyTracked = !g_tracked.empty();
            LeaveCriticalSection(&g_trackLock);

            // Poll fast while a temporary window exists: Explorer can show it through paths we
            // do not hook, and the animation starts within milliseconds.
            Sleep(anyTracked ? 10 : 100);

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

                if (ConsumeRelease(hWnd))
                {
                    // The app may keep a window after the hook already hid it. The service
                    // release uncloaks it; make sure it is actually visible now.
                    if (!IsWindowVisible(hWnd))
                        g_showWindow(hWnd, SW_SHOWNOACTIVATE);
                    drop.push_back(hWnd);
                    continue;
                }

                if (AppRestored(hWnd))
                {
                    SetCloaked(hWnd, false);
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

                // Explorer may show the window through a path that bypasses our hooks (or strip
                // its styles while initializing). The window may stay WS_VISIBLE; what matters
                // is that it is never composed, so keep it transparent and cloaked.
                BYTE alpha = 0;
                DWORD flags = 0;
                const bool transparent =
                    GetLayeredWindowAttributes(hWnd, nullptr, &alpha, &flags) != FALSE &&
                    (flags & LWA_ALPHA) != 0 && alpha == 0;
                if (!transparent)
                {
                    Log("WORKER hwnd=%p transparent=0 rehide=1", hWnd);
                    MakeTransparent(hWnd);
                }

                if (!IsCloaked(hWnd))
                {
                    Log("WORKER hwnd=%p cloak=0 rehide=1", hWnd);
                    SetCloaked(hWnd, true);
                }
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
                SetCloaked(hWnd, false);
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
        const MH_STATUS showAsyncStatus = MH_CreateHookApi(L"user32.dll", "ShowWindowAsync",
            reinterpret_cast<LPVOID>(&HookShowWindowAsync),
            reinterpret_cast<LPVOID*>(&g_showWindowAsync));
        const MH_STATUS posStatus = MH_CreateHookApi(L"user32.dll", "SetWindowPos",
            reinterpret_cast<LPVOID>(&HookSetWindowPos),
            reinterpret_cast<LPVOID*>(&g_setWindowPos));
        const MH_STATUS destroyStatus = MH_CreateHookApi(L"user32.dll", "DestroyWindow",
            reinterpret_cast<LPVOID>(&HookDestroyWindow),
            reinterpret_cast<LPVOID*>(&g_destroyWindow));

        // Explorer's internal show path goes through win32u, not through any user32 export.
        // These are optional: the user32 hooks and the worker still work if they fail.
        const MH_STATUS ntShowStatus = MH_CreateHookApi(L"win32u.dll", "NtUserShowWindow",
            reinterpret_cast<LPVOID>(&HookNtUserShowWindow),
            reinterpret_cast<LPVOID*>(&g_ntUserShowWindow));
        const MH_STATUS ntPosStatus = MH_CreateHookApi(L"win32u.dll", "NtUserSetWindowPos",
            reinterpret_cast<LPVOID>(&HookNtUserSetWindowPos),
            reinterpret_cast<LPVOID*>(&g_ntUserSetWindowPos));
        const MH_STATUS swlStatus = MH_CreateHookApi(L"user32.dll", "SetWindowLongW",
            reinterpret_cast<LPVOID>(&HookSetWindowLongW),
            reinterpret_cast<LPVOID*>(&g_setWindowLongW));
        const MH_STATUS swlpStatus = MH_CreateHookApi(L"user32.dll", "SetWindowLongPtrW",
            reinterpret_cast<LPVOID>(&HookSetWindowLongPtrW),
            reinterpret_cast<LPVOID*>(&g_setWindowLongPtrW));
        const MH_STATUS swpStatus = MH_CreateHookApi(L"user32.dll", "SetWindowPlacement",
            reinterpret_cast<LPVOID>(&HookSetWindowPlacement),
            reinterpret_cast<LPVOID*>(&g_setWindowPlacement));

        Log("INIT createHook=%d showHook=%d showAsyncHook=%d posHook=%d destroyHook=%d ntShowHook=%d ntPosHook=%d swlHook=%d swlpHook=%d swpHook=%d",
            createStatus, showStatus, showAsyncStatus, posStatus, destroyStatus, ntShowStatus,
            ntPosStatus, swlStatus, swlpStatus, swpStatus);
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