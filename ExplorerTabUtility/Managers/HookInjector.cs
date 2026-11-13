using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Threading;
using ExplorerTabUtility.WinAPI;

namespace ExplorerTabUtility.Managers;

// Injects ExplorerTabUtility.Hook.dll into explorer.exe and keeps its policy state up to date.
// The DLL makes new Explorer windows invisible from creation, so they can be merged into a tab
// without ever painting a frame. Injection is best-effort: when it fails, the app falls back to
// its normal hide/merge path.
internal static class HookInjector
{
    private const string DllName = "ExplorerTabUtility.Hook.dll";

    private static readonly HashSet<int> Injected = new();
    private static readonly object Sync = new();
    private static Timer? _timer;
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

        // A short interval keeps the gap small when Explorer restarts or spawns a new process.
        _timer = new Timer(_ => Tick(), null, 0, 300);
    }

    public static void AllowNextWindow(int milliseconds = 3_000)
    {
        try
        {
            _policy?.AllowNextWindow(milliseconds);
        }
        catch
        {
            // Never break window creation because of the hook.
        }
    }

    private static void Tick()
    {
        try
        {
            _policy?.Write(SettingsManager.IsWindowHookActive || SettingsManager.ReuseTabs,
                heartbeat: true);

            Process[] processes;
            try
            {
                processes = Process.GetProcessesByName("explorer");
            }
            catch
            {
                return;
            }

            foreach (var process in processes)
            {
                try
                {
                    lock (Sync)
                    {
                        if (Injected.Contains(process.Id)) continue;
                    }

                    if (Inject(process.Id, _dllPath))
                    {
                        lock (Sync) Injected.Add(process.Id);
                    }
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

            lock (Sync)
            {
                Injected.RemoveWhere(IsProcessGone);
            }
        }
        catch
        {
            // Injection must never break the app.
        }
    }

    private static bool IsProcessGone(int processId)
    {
        try
        {
            using var process = Process.GetProcessById(processId);
            return process.HasExited;
        }
        catch
        {
            return true;
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
            }
            finally
            {
                WinApi.CloseHandle(thread);
            }

            return true;
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
}
