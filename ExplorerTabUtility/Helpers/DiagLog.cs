using System;
using System.IO;
using System.Threading;

namespace ExplorerTabUtility.Helpers;

// Temporary diagnostics: appends timestamped lines to %APPDATA%\ExplorerTabUtility\diag.log
internal static class DiagLog
{
    private static readonly object Sync = new();
    private static readonly string FilePath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
        Constants.AppName,
        "diag.log");

    static DiagLog()
    {
        Write("==== session start ====");
    }

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
