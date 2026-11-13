using System;
using System.IO.MemoryMappedFiles;

namespace ExplorerTabUtility.Managers;

// Shared state consumed by the injected hook DLL:
//   int enabled        - merge new windows into tabs
//   int allowNextTick  - the next created window must stay visible (intentional new window)
//   int heartbeat      - updated every second so the DLL goes passive if the app exits
internal sealed class HookPolicy : IDisposable
{
    private const string MapName = "Local\\ExplorerTabUtility.HookPolicy";
    private const int MapSize = 16;

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

    public void Dispose()
    {
        _view.Dispose();
        _map.Dispose();
    }
}
