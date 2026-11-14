using System;
using System.Threading;
using System.Collections.Generic;
using System.Drawing;
using H.Hooks;
using ExplorerTabUtility.Models;
using ExplorerTabUtility.Helpers;
using ExplorerTabUtility.WinAPI;

namespace ExplorerTabUtility.Hooks;

public sealed class Mouse : IHook
{
    private const int TabDragThresholdPx = 12;

    private int _lastClickTime;
    private Key _lastClickKey;
    private Point _lastClickPosition;
    private bool _hasLastClick;
    private nint _dragTab;
    private Point _dragStart;
    private bool _leftDown;
    private bool _dragDetected;
    private readonly LowLevelMouseHook _lowLevelMouseHook;
    private readonly IReadOnlyCollection<HotKeyProfile> _hotkeyProfiles;
    public bool IsHookActive => _lowLevelMouseHook.IsStarted;
    public event Action<HotKeyEventArgs>? OnHotKeyProfileTriggered;
    public event Action<nint>? OnTabDragDetected;
    public event Action? OnTabDragEnded;

    public Mouse(IReadOnlyCollection<HotKeyProfile> hotkeyProfiles)
    {
        _hotkeyProfiles = hotkeyProfiles;
        _lowLevelMouseHook = new LowLevelMouseHook { AddKeyboardKeys = true };
        _lowLevelMouseHook.Down += LowLevelMouseHook_Down;
        _lowLevelMouseHook.Move += LowLevelMouseHook_Move;
        _lowLevelMouseHook.Up += LowLevelMouseHook_Up;
    }

    public void StartHook()
    {
        DiagLog.Write("MOUSE hook start");
        _lowLevelMouseHook.Start();
    }
    public void StopHook() => _lowLevelMouseHook.Stop();

    private void LowLevelMouseHook_Down(object? sender, MouseEventArgs e)
    {
        // A drag that starts on a folder tab is Explorer's native tab tear-out. The app has no
        // Detach hotkey record for it, so remember it until the new window registers.
        if (e.CurrentKey == Key.MouseLeft)
        {
            _leftDown = true;
            _dragDetected = false;
            _dragTab = FindTabUnderCursor(e.Position);
            if (_dragTab != 0)
                _dragStart = e.Position;
        }

        var handler = OnHotKeyProfileTriggered;
        if (handler == null) return;

        var isDoubleClick = IsDoubleClick(e.CurrentKey, e.Position);

        bool? isFileExplorerForeground = null;
        nint handle = 0;
        string? windowTitle = null;
        var windowTitleCaptured = false;

        foreach (var profile in _hotkeyProfiles)
        {
            // Skip disabled, empty or non mouse
            if (!profile.IsMouse || !profile.IsEnabled || profile.HotKeys is null || profile.HotKeys.Length == 0)
                continue;
            
            // Skip if it requires double-click and it is not
            if (profile.IsDoubleClick && !isDoubleClick) continue;
            
            // Skip if keys do not match
            if (!e.Keys.Are(profile.HotKeys)) continue;

            // Let's see if we need to check File Explorer
            if (profile.Scope == HotkeyScope.FileExplorer)
            {
                // Check if File Explorer is foreground (only once)
                isFileExplorerForeground ??= Helper.IsFileExplorerForeground(out handle);

                if (isFileExplorerForeground == false)
                {
                    handle = 0; // Reset handle if not File Explorer
                    continue;
                }

                if (!windowTitleCaptured)
                {
                    windowTitle = Helper.GetWindowTitle(handle);
                    windowTitleCaptured = true;
                }
            }
            
            // Queue the hotkey trigger in a separate thread.
#if NET7_0_OR_GREATER
            ThreadPool.QueueUserWorkItem(static s => s.Handler.Invoke(new HotKeyEventArgs(s.Profile, s.Handle, s.Position, s.Title)),
                new State(handler, profile, handle, e.Position, windowTitle), false);
#else
            ThreadPool.QueueUserWorkItem(static state =>
            {
                var s = (State)state!;
                s.Handler.Invoke(new HotKeyEventArgs(s.Profile, s.Handle, s.Position, s.Title));
            }, new State(handler, profile, handle, e.Position, windowTitle));
#endif
        }
    }

    private void LowLevelMouseHook_Move(object? sender, MouseEventArgs e)
    {
        if (!_leftDown || _dragTab == 0 || _dragDetected) return;
        if (Math.Abs(e.Position.X - _dragStart.X) < TabDragThresholdPx &&
            Math.Abs(e.Position.Y - _dragStart.Y) < TabDragThresholdPx) return;

        _dragDetected = true;
        DiagLog.Write($"MOUSE drag detected pos={e.Position.X},{e.Position.Y} tab=0x{(long)_dragTab:X}");
        OnTabDragDetected?.Invoke(_dragTab);
    }

    private void LowLevelMouseHook_Up(object? sender, MouseEventArgs e)
    {
        if (!_leftDown && _dragTab == 0) return;

        // The low-level hook receives the button-up before Explorer handles the drop and
        // creates the tear-out window, so decide here if the tab was actually dragged.
        if (_dragTab != 0 && !_dragDetected &&
            (Math.Abs(e.Position.X - _dragStart.X) >= TabDragThresholdPx ||
             Math.Abs(e.Position.Y - _dragStart.Y) >= TabDragThresholdPx))
        {
            _dragDetected = true;
            DiagLog.Write($"MOUSE drag detected pos={e.Position.X},{e.Position.Y} tab=0x{(long)_dragTab:X}");
            OnTabDragDetected?.Invoke(_dragTab);
        }

        if (_dragDetected)
        {
            DiagLog.Write("MOUSE drag up");
            OnTabDragEnded?.Invoke();
        }

        _leftDown = false;
        _dragTab = 0;
        _dragDetected = false;
    }

    private static nint FindTabUnderCursor(Point position)
    {
        var hWnd = WinApi.WindowFromPoint(position);
        nint tabStrip = 0;

        while (hWnd != 0)
        {
            if (Helper.IsFileExplorerTab(hWnd)) return hWnd;

            if (WinApi.IsWindowHasClassName(hWnd, "Microsoft.UI.Content.DesktopChildSiteBridge"))
                tabStrip = hWnd;

            if (WinApi.IsWindowHasClassName(hWnd, "CabinetWClass"))
            {
                // The Win11 tab strip is a WinUI island in the title bar.
                if (tabStrip != 0 &&
                    WinApi.GetWindowRect(hWnd, out var rect) &&
                    position.Y <= rect.Top + 80 &&
                    position.X < rect.Right - 160)
                    return tabStrip;

                return 0;
            }

            hWnd = WinApi.GetParent(hWnd);
        }

        return 0;
    }

    private bool IsDoubleClick(Key currentKey, Point position)
    {
        var now = Environment.TickCount;

        if (_hasLastClick
            && now - _lastClickTime < Constants.DoubleClickIntervalMs
            && _lastClickKey == currentKey
            && Math.Abs(position.X - _lastClickPosition.X) <= Constants.DoubleClickMaxMovePx
            && Math.Abs(position.Y - _lastClickPosition.Y) <= Constants.DoubleClickMaxMovePx)
        {
            // Forget the pair so the third click of a triple-click doesn't trigger the hotkey again.
            _hasLastClick = false;
            return true;
        }

        _hasLastClick = true;
        _lastClickTime = now;
        _lastClickKey = currentKey;
        _lastClickPosition = position;
        return false;
    }

    public void Dispose()
    {
        StopHook();
        _lowLevelMouseHook.Dispose();
    }

    private readonly struct State(Action<HotKeyEventArgs> handler, HotKeyProfile profile, nint handle,
        Point position, string? title)
    {
        public readonly Action<HotKeyEventArgs> Handler = handler;
        public readonly HotKeyProfile Profile = profile;
        public readonly nint Handle = handle;
        public readonly Point Position = position;
        public readonly string? Title = title;
    }
}