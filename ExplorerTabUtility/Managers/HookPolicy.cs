using System;
using System.IO.MemoryMappedFiles;

namespace ExplorerTabUtility.Managers;

// Shared state consumed by the injected hook DLL:
//   int enabled        - merge new windows into tabs
//   int allowNextTick  - the next created window must stay visible (intentional new window)
//   int heartbeat      - updated every second so the DLL goes passive if the app exits
//   long condemned[8]  - windows already merged into a tab and quit, at offset 16
//   long released[8]   - windows the app decided to keep visible, at offset 80
// The layout must stay in sync with the Policy struct in native/ExplorerTabUtility.Hook/dllmain.cpp.
internal sealed class HookPolicy : IDisposable
{
    private const string MapName = "Local\\ExplorerTabUtility.HookPolicy";
    private const int CondemnedOffset = 16;
    private const int CondemnedSlots = 8;
    private const int ReleasedOffset = CondemnedOffset + CondemnedSlots * sizeof(long);
    private const int ReleasedSlots = 8;
    private const int MapSize = ReleasedOffset + ReleasedSlots * sizeof(long);

    private readonly MemoryMappedFile _map;
    private readonly MemoryMappedViewAccessor _view;
    private readonly object _sync = new();

    public HookPolicy()
    {
        _map = MemoryMappedFile.CreateOrOpen(MapName, MapSize);
        _view = _map.CreateViewAccessor();
    }

    public void Write(bool enabled, bool heartbeat)
    {
        lock (_sync)
        {
            _view.Write(0, enabled ? 1 : 0);
            if (heartbeat)
                _view.Write(8, Environment.TickCount);
        }
    }

    public void AllowNextWindow(int milliseconds = 3_000)
    {
        lock (_sync)
        {
            _view.Write(4, Environment.TickCount + milliseconds);
            _view.Write(8, Environment.TickCount);
        }
    }

    // Tells the hook that this window is gone for good: Explorer still shows it once while
    // tearing it down, and that show must be suppressed rather than only made transparent.
    // The hook frees the slot when the window is destroyed.
    public void CondemnWindow(nint hWnd) => WriteHandle(CondemnedOffset, CondemnedSlots, hWnd);

    // Tells the hook to stop managing this window because the app wants it on screen. Needed
    // explicitly: Explorer also strips WS_EX_LAYERED while initializing, so the hook cannot
    // infer it from the window style alone.
    public void ReleaseWindow(nint hWnd) => WriteHandle(ReleasedOffset, ReleasedSlots, hWnd);

    private void WriteHandle(int baseOffset, int slots, nint hWnd)
    {
        if (hWnd == 0) return;

        var value = (long)hWnd;
        lock (_sync)
        {
            var freeOffset = -1;
            for (var i = 0; i < slots; i++)
            {
                var offset = baseOffset + i * sizeof(long);
                var existing = _view.ReadInt64(offset);
                if (existing == value) return; // already pending, don't burn another slot
                if (existing == 0 && freeOffset < 0) freeOffset = offset;
            }

            // Every slot is taken. Overwrite the first one: the worst case is that a single
            // window falls back to the previous behaviour.
            _view.Write(freeOffset >= 0 ? freeOffset : baseOffset, value);
        }
    }

    public void Dispose()
    {
        _view.Dispose();
        _map.Dispose();
    }
}
