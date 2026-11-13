using System.Drawing;

namespace ExplorerTabUtility.Models;

public class HotKeyEventArgs(HotKeyProfile profile, nint foregroundWindow, Point? mousePosition = null,
    string? windowTitle = null)
{
    public HotKeyProfile Profile { get; } = profile;
    public nint ForegroundWindow { get; } = foregroundWindow;
    public Point? MousePosition { get; } = mousePosition;

    // Explorer window title captured when the mouse gesture happened. Used to detect that the
    // view has already navigated (e.g. a double-click opened a folder) before handling the hotkey.
    public string? WindowTitle { get; } = windowTitle;
}