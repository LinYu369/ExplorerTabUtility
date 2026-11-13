using System;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Threading;

namespace ExplorerTabUtility.Helpers;

// Removes/restores a window's taskbar button. Windows creates the button as soon as a window is
// shown, even if its content is fully transparent, so a hidden temporary window can still "flash"
// in the taskbar until it is closed.
internal static class TaskbarHelper
{
    [ComImport]
    [Guid("56FDF342-FD6D-11d0-958A-006097C9A090")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface ITaskbarList
    {
        void HrInit();
        void AddTab(nint hwnd);
        void DeleteTab(nint hwnd);
        void ActivateTab(nint hwnd);
        void SetActiveAlt(nint hwnd);
    }

    private static ITaskbarList? _taskbar;
    private static bool _initialized;

    public static void DeleteTab(nint hWnd) => Post(t => t.DeleteTab(hWnd));
    public static void AddTab(nint hWnd) => Post(t => t.AddTab(hWnd));

    private static void Post(Action<ITaskbarList> action)
    {
        var dispatcher = Application.Current?.Dispatcher;
        if (dispatcher == null) return;

        try
        {
            dispatcher.BeginInvoke(new Action(() =>
            {
                try
                {
                    if (!_initialized)
                    {
                        _initialized = true;
                        var type = Type.GetTypeFromCLSID(new Guid("56FDF344-FD6D-11d0-958A-006097C9A090"));
                        _taskbar = (ITaskbarList)Activator.CreateInstance(type!)!;
                        _taskbar.HrInit();
                    }

                    action(_taskbar!);
                }
                catch
                {
                    // Taskbar bookkeeping must never break window merging.
                }
            }), DispatcherPriority.Send);
        }
        catch
        {
            // The dispatcher may already be shutting down.
        }
    }
}
