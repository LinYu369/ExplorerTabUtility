using System;
using System.Threading;
using System.Collections.Generic;
using System.Drawing;
using H.Hooks;
using ExplorerTabUtility.Models;
using ExplorerTabUtility.Helpers;

namespace ExplorerTabUtility.Hooks;

public sealed class Mouse : IHook
{
    private int _lastClickTime;
    private Key _lastClickKey;
    private Point _lastClickPosition;
    private bool _hasLastClick;
    private readonly LowLevelMouseHook _lowLevelMouseHook;
    private readonly IReadOnlyCollection<HotKeyProfile> _hotkeyProfiles;
    public bool IsHookActive => _lowLevelMouseHook.IsStarted;
    public event Action<HotKeyEventArgs>? OnHotKeyProfileTriggered;

    public Mouse(IReadOnlyCollection<HotKeyProfile> hotkeyProfiles)
    {
        _hotkeyProfiles = hotkeyProfiles;
        _lowLevelMouseHook = new LowLevelMouseHook { AddKeyboardKeys = true };
        _lowLevelMouseHook.Down += LowLevelMouseHook_Down;
    }

    public void StartHook() => _lowLevelMouseHook.Start();
    public void StopHook() => _lowLevelMouseHook.Stop();

    private void LowLevelMouseHook_Down(object? sender, MouseEventArgs e)
    {
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