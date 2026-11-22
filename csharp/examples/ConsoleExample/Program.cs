using System;
using PwfTimeSync;

namespace ConsoleExample
{
    internal static class Program
    {
        private static int Main(string[] args)
        {
            // Must be the first line: when the app is relaunched elevated to fix the clock,
            // this does the work and exits the process.
            AutoTimeSync.HandleElevatedCommand(args);

            TimeSyncOptions options = new TimeSyncOptions { Log = Console.WriteLine };

            // --force: run the Windows "Sync now" even if the clock is fine, and do not relaunch this app elevated.
            // (Standard users may still see Windows' own credential prompt for "Sync now".)
            if (Array.IndexOf(args, "--force") >= 0)
            {
                options.ForceSync = true;
                options.AllowElevationPrompt = false;
            }

            // Most customers already have a correct clock, so the clock-skew rejection never happens for them:
            // they pass straight through after a quick check and notice nothing. Only customers whose clock
            // drifted get it fixed here, silently, before the key is sent.
            TimeSyncResult result = AutoTimeSync.EnsureClockIsCorrect(options);
            Console.WriteLine(result);

            // Now log in with the PWFAuth SDK (NuGet) as usual, e.g. inside an async method:
            //
            //   var client = new PwfClient(secret);
            //   var login = await client.LoginAsync(key);
            //
            // The clock has already been checked, so the key is not rejected for clock skew.

            return result.Success ? 0 : 1;
        }
    }
}
