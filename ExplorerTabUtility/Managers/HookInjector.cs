using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using ExplorerTabUtility.Helpers;
using ExplorerTabUtility.WinAPI;

namespace ExplorerTabUtility.Managers;

// Injects ExplorerTabUtility.Hook.dll into explorer.exe and keeps its policy state up to date.
// Injection is event driven: an initial scan plus events for new windows and new explorer
// processes. There is no periodic process polling. Every successful injection is verified by
// checking that the DLL is actually loaded in the target process.
internal static class HookInjector
{
    private const string DllName = "ExplorerTabUtility.Hook.dll";
    private const uint ListModulesAll = 0x03;

    private static readonly Dictionary<int, DateTime> Injected = new(); // pid -> process start time
    private static readonly object Sync = new();
    private static readonly int CurrentProcessId = Process.GetCurrentProcess().Id;

    private static Timer? _heartbeat;
    private static HookPolicy? _policy;
    private static string _dllPath = string.Empty;

    public static void Start()
    {
        // The hook DLL is x64; explorer.exe is x64 on a 64-bit system.
        if (!Environment.Is64BitOperatingSystem || !Environment.Is64BitProcess) return;

        _dllPath = Path.Combine(AppContext.BaseDirectory, DllName);
        if (!File.Exists(_dllPath)) return;

        try
        {
            _policy = new HookPolicy();
        }
        catch
        {
            _policy = null;
            return;
        }

        WritePolicy();

        // Only a heartbeat remains: it keeps the DLL enabled while the app runs. No process scanning.
        _heartbeat = new Timer(_ => WritePolicy(), null, 0, 1_000);

        // One initial pass for explorer instances that already exist.
        Process[] processes;
        try
        {
            processes = Process.GetProcessesByName("explorer");
        }
        catch
        {
            processes = Array.Empty<Process>();
        }

        foreach (var process in processes)
        {
            try
            {
                EnsureInjected(process.Id);
            }
            catch
            {
                // Access denied or the process exited.
            }
            finally
            {
                process.Dispose();
            }
        }
    }

    private static void WritePolicy()
    {
        try
        {
            _policy?.Write(SettingsManager.IsWindowHookActive || SettingsManager.ReuseTabs,
                heartbeat: true);
        }
        catch
        {
            // The heartbeat must never break the app.
        }
    }

    public static void AllowNextWindow(int milliseconds = 3_000)
    {
        try
        {
            _policy?.AllowNextWindow(milliseconds);
            DiagLog.Write($"ALLOW {milliseconds}ms");
        }
        catch
        {
            // Never break window creation because of the hook.
        }
    }

    public static void EnsureInjectedForWindow(nint hWnd)
    {
        if (_policy == null || hWnd == 0) return;

        try
        {
            WinApi.GetWindowThreadProcessId(hWnd, out var processId);
            if (processId != 0)
                EnsureInjected((int)processId);
        }
        catch
        {
            // Best effort.
        }
    }

    public static void EnsureInjected(int processId)
    {
        if (_policy == null || processId <= 0 || processId == CurrentProcessId) return;

        DateTime startedAt = default;
        try
        {
            using var process = Process.GetProcessById(processId);
            startedAt = process.StartTime;
        }
        catch
        {
            return; // Exited.
        }

        lock (Sync)
        {
            // PID reuse guard: only trust the cache when it is the same process instance.
            if (Injected.TryGetValue(processId, out var cached) && cached == startedAt) return;
        }

        if (IsHookLoaded(processId))
        {
            lock (Sync) Injected[processId] = startedAt;
            DiagLog.Write($"INJECT already loaded pid={processId}");
            return;
        }

        if (Inject(processId, _dllPath))
        {
            lock (Sync) Injected[processId] = startedAt;
            DiagLog.Write($"INJECT ok pid={processId}");
        }
        else
        {
            // Not cached: the next window/process event will try again.
            DiagLog.Write($"INJECT failed pid={processId}");
        }
    }

    private static bool IsHookLoaded(int processId)
    {
        // PROCESS_QUERY_INFORMATION | PROCESS_VM_READ
        var process = WinApi.OpenProcess(0x0400 | 0x0010, false, (uint)processId);
        if (process == 0) return false;

        try
        {
            var modules = new nint[1024];
            var size = (uint)(modules.Length * IntPtr.Size);
            if (!EnumProcessModulesEx(process, modules, size, out var needed, ListModulesAll))
                return false;

            var count = Math.Min(needed / (uint)IntPtr.Size, (uint)modules.Length);
            var buffer = new StringBuilder(1024);

            for (uint i = 0; i < count; i++)
            {
                if (GetModuleFileNameExW(process, modules[i], buffer, (uint)buffer.Capacity) == 0)
                    continue;

                if (string.Equals(Path.GetFileName(buffer.ToString()), DllName,
                        StringComparison.OrdinalIgnoreCase))
                    return true;
            }

            return false;
        }
        catch
        {
            return false;
        }
        finally
        {
            WinApi.CloseHandle(process);
        }
    }

    private static bool Inject(int processId, string dllPath)
    {
        // PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_READ |
        // PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION
        const uint access = 0x0002 | 0x0008 | 0x0010 | 0x0020 | 0x0400;

        var process = WinApi.OpenProcess(access, false, (uint)processId);
        if (process == 0) return false;

        nint remote = 0;
        try
        {
            var bytes = Encoding.Unicode.GetBytes(dllPath + "\0");
            remote = WinApi.VirtualAllocEx(process, 0, (nuint)bytes.Length,
                0x3000 /* MEM_COMMIT | MEM_RESERVE */, 0x04 /* PAGE_READWRITE */);
            if (remote == 0) return false;

            if (!WinApi.WriteProcessMemory(process, remote, bytes, (nuint)bytes.Length, out _))
                return false;

            var loadLibrary = WinApi.GetProcAddress(WinApi.GetModuleHandleW("kernel32.dll"),
                "LoadLibraryW");
            if (loadLibrary == 0) return false;

            var thread = WinApi.CreateRemoteThread(process, 0, 0, loadLibrary, remote, 0, out _);
            if (thread == 0) return false;

            try
            {
                WinApi.WaitForSingleObject(thread, 5_000);
                return GetExitCodeThread(thread, out var exitCode) && exitCode != 0;
            }
            finally
            {
                WinApi.CloseHandle(thread);
            }
        }
        catch
        {
            return false;
        }
        finally
        {
            if (remote != 0)
                WinApi.VirtualFreeEx(process, remote, 0, 0x8000 /* MEM_RELEASE */);
            WinApi.CloseHandle(process);
        }
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetExitCodeThread(nint hThread, out uint lpExitCode);

    [DllImport("psapi.dll", SetLastError = true)]
    private static extern bool EnumProcessModulesEx(nint hProcess, [Out] nint[] lphModule,
        uint cb, out uint lpcbNeeded, uint dwFilterFlag);

    [DllImport("psapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern uint GetModuleFileNameExW(nint hProcess, nint hModule,
        StringBuilder lpFilename, uint nSize);
}
