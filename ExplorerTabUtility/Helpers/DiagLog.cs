using System;
using System.IO;
using System.Threading;

namespace ExplorerTabUtility.Helpers;

// Temporary diagnostics for the detach flow (removed again for the daily build).
internal static class DiagLog
{
    private static readonly object Sync = new();
    private static readonly string FilePath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
        Constants.AppName,
        "detach.log");

    public static void Write(string message)
    {
        try
        {
            lock (Sync)
            {
                Directory.CreateDirectory(Path.GetDirectoryName(FilePath)!);
                File.AppendAllText(FilePath,
                    $"{DateTime.Now:HH:mm:ss.fff} T{Thread.CurrentThread.ManagedThreadId} {message}{Environment.NewLine}");
            }
        }
        catch
        {
            // Diagnostics must never break the app.
        }
    }
}
