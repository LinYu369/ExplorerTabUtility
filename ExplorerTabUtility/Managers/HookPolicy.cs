using System;
using System.IO.MemoryMappedFiles;

namespace ExplorerTabUtility.Managers;

// Shared state consumed by the injected hook DLL:
//   int enabled        - merge new windows into tabs
//   int allowNextTick  - the next created window must stay visible (intentional new window)
//   int heartbeat      - updated every second so the DLL goes passive if the app exits
//   long condemned[8]  - windows already merged into a tab and quit, at offset 16
// The layout must stay in sync with the Policy struct in native/ExplorerTabUtility.Hook/dllmain.cpp.
internal sealed class HookPolicy : IDisposable
{
    private const string MapName = "Local\\ExplorerTabUtility.HookPolicy";
    private const int CondemnedOffset = 16;
    private const int CondemnedSlots = 8;
    private const int MapSize = CondemnedOffset + CondemnedSlots * sizeof(long);

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
    public void CondemnWindow(nint hWnd)
    {
        if (hWnd == 0) return;

        lock (_sync)
        {
            for (var i = 0; i < CondemnedSlots; i++)
            {
                var offset = CondemnedOffset + i * sizeof(long);
                if (_view.ReadInt64(offset) != 0) continue;

                _view.Write(offset, (long)hWnd);
                return;
            }

            // Every slot is taken. Overwrite the first one: the worst case is that a single
            // window falls back to the old transparency-only behaviour.
            _view.Write(CondemnedOffset, (long)hWnd);
        }
    }

    public void Dispose()
    {
        _view.Dispose();
        _map.Dispose();
    }
}
