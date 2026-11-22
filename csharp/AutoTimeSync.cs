// AutoTimeSync.cs
// Drop-in helper that makes sure the Windows clock is correct before a pwfauth.com license login.
// Targets .NET Framework 4.5+ and .NET 6/8+, C# 7.3, no external dependencies.
// Author: Fahads7ee7

using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace PwfTimeSync
{
    /// <summary>How the clock was found or fixed.</summary>
    public enum TimeSyncMethod
    {
        AlreadyCorrect,
        WindowsSyncNow,
        W32tmResync,
        SetSystemTime,
        ElevatedHelper,
        NoReference,
        Failed,
        Skipped
    }

    public sealed class TimeSyncOptions
    {
        /// <summary>Largest accepted difference from the reference clock. pwfauth.com allows 300 s.</summary>
        public double MaxAllowedOffsetSeconds { get; set; } = 30;

        /// <summary>
        /// Relaunch the app elevated (UAC prompt) when nothing else fixed the clock. Setting this to false
        /// only stops that relaunch: standard users may still see Windows' own credential prompt for "Sync now".
        /// </summary>
        public bool AllowElevationPrompt { get; set; } = true;

        /// <summary>Run the Windows sync even when the clock already looks correct (for testing).</summary>
        public bool ForceSync { get; set; }

        public int SyncNowTimeoutMs { get; set; } = 15000;

        public IList<string> NtpServers { get; set; } = new List<string>
        {
            "time.windows.com", "time.google.com", "time.cloudflare.com", "pool.ntp.org"
        };

        public IList<string> HttpFallbackHosts { get; set; } = new List<string>
        {
            "www.msftconnecttest.com", "www.google.com"
        };

        /// <summary>Optional log callback. No output by default.</summary>
        public Action<string> Log { get; set; }
    }

    public sealed class TimeSyncResult
    {
        internal TimeSyncResult(bool success, TimeSyncMethod method, double? before, double? after, string message)
        {
            Success = success;
            Method = method;
            OffsetBeforeSeconds = before;
            OffsetAfterSeconds = after;
            Message = message;
        }

        /// <summary>True when the clock is within the threshold, or no reference was reachable.</summary>
        public bool Success { get; }
        public TimeSyncMethod Method { get; }
        /// <summary>Reference UTC minus local UTC, in seconds, before any fix.</summary>
        public double? OffsetBeforeSeconds { get; }
        public double? OffsetAfterSeconds { get; }
        public string Message { get; }

        public override string ToString()
        {
            return string.Format(CultureInfo.InvariantCulture,
                "Success={0}, Method={1}, OffsetBefore={2}, OffsetAfter={3}, Message={4}",
                Success, Method, FormatOffset(OffsetBeforeSeconds), FormatOffset(OffsetAfterSeconds), Message);
        }

        private static string FormatOffset(double? value)
        {
            return value.HasValue ? value.Value.ToString("0.000", CultureInfo.InvariantCulture) + " s" : "n/a";
        }
    }

    /// <summary>
    /// Call <see cref="EnsureClockIsCorrect"/> before the license login. Most customers already have a
    /// correct clock: for them this only measures the offset (a quick network check) and changes nothing,
    /// so they never notice it. Customers whose clock drifted get it fixed silently, the same way the
    /// Windows "Sync now" button does, so the license key is accepted instead of rejected for clock skew.
    /// </summary>
    public static class AutoTimeSync
    {
        public const string ElevatedFlag = "--pwf-timesync-elevated";

        private const int NtpTimeoutMs = 1500;
        private const int HttpTimeoutMs = 3000;
        private const int ElevatedHelperTimeoutMs = 60000;
        private const int ElevatedFixBudgetMs = 45000;
        private const int W32tmTimeoutMs = 10000;
        private const int ErrorCancelled = 1223;

        private const string DeclinedMessage = "Administrator permission was declined, so the clock could not be fixed. " +
                                               "Turn on 'Set time automatically' in Windows Date & time settings.";

        private static readonly DateTime NtpEpoch = new DateTime(1900, 1, 1, 0, 0, 0, DateTimeKind.Utc);

        private enum SyncNowOutcome
        {
            Ran,
            NotAvailable,
            Declined
        }

        /// <summary>Per-run memory: once NTP failed and HTTP answered, later re-measures skip NTP.</summary>
        private sealed class MeasureState
        {
            public bool SkipNtp;
        }

        public static TimeSyncResult EnsureClockIsCorrect(TimeSyncOptions options = null)
        {
            TimeSyncOptions o = options ?? new TimeSyncOptions();
            Action<string> log = SafeLog(o.Log);
            try
            {
                return Run(o, log);
            }
            catch (Exception ex)
            {
                log("Unexpected error: " + ex.Message);
                return new TimeSyncResult(false, TimeSyncMethod.Failed, null, null, "Unexpected error: " + ex.Message);
            }
        }

        public static Task<TimeSyncResult> EnsureClockIsCorrectAsync(TimeSyncOptions options = null)
        {
            return Task.Run(() => EnsureClockIsCorrect(options));
        }

        /// <summary>
        /// Reference UTC minus local UTC in seconds, or null when no reference is reachable.
        /// <paramref name="timeoutMs"/> is the total time limit for the whole measurement.
        /// </summary>
        public static double? GetClockOffsetSeconds(int timeoutMs = 3000)
        {
            try
            {
                TimeSyncOptions defaults = new TimeSyncOptions();
                int ntpBudget = Math.Max(200, Math.Min(NtpTimeoutMs, timeoutMs / 2));
                int httpBudget = Math.Max(300, timeoutMs - ntpBudget);
                return MeasureOffset(defaults.NtpServers, defaults.HttpFallbackHosts, ntpBudget, httpBudget,
                    new MeasureState(), msg => { });
            }
            catch
            {
                return null;
            }
        }

        /// <summary>
        /// Call as the first line of Main. When the app was relaunched with <see cref="ElevatedFlag"/>,
        /// fixes the clock with administrator rights and exits the process (0 = ok, 1 = failed).
        /// Otherwise returns false and does nothing.
        /// </summary>
        public static bool HandleElevatedCommand(string[] args)
        {
            if (!HasElevatedFlag(args ?? SafeCommandLineArgs()))
                return false;

            int exitCode = 1;
            try
            {
                TimeSyncOptions o = new TimeSyncOptions();
                TimeSyncMethod method;
                double? after;
                if (IsWindows() && TryElevatedFix(o, new MeasureState(), SafeLog(null), out method, out after))
                    exitCode = 0;
            }
            catch
            {
                exitCode = 1;
            }

            Environment.Exit(exitCode);
            return true;
        }

        private static TimeSyncResult Run(TimeSyncOptions o, Action<string> log)
        {
            if (!IsWindows())
                return new TimeSyncResult(true, TimeSyncMethod.Skipped, null, null, "Not running on Windows; nothing to do.");

            MeasureState state = new MeasureState();
            double? before = Measure(o, state, log);
            if (!before.HasValue)
            {
                log("No time reference reachable; leaving the clock unchanged.");
                return new TimeSyncResult(true, TimeSyncMethod.NoReference, null, null,
                    "No time reference reachable; clock left unchanged.");
            }

            if (IsWithin(before.Value, o) && !o.ForceSync)
                return new TimeSyncResult(true, TimeSyncMethod.AlreadyCorrect, before, before, "Clock is already correct.");

            log(IsWithin(before.Value, o)
                ? "ForceSync is set; running the Windows time sync."
                : "Clock is off by " + FormatSeconds(before.Value) + "; trying to fix it.");

            double? after = before;
            bool attempted = false;

            // Step 1: the same action as the "Sync now" button in Windows Settings.
            SyncNowOutcome syncNow = RunWindowsSyncNow(o, log);
            if (syncNow == SyncNowOutcome.Ran)
            {
                attempted = true;
                bool fixedNow;
                after = WaitForCorrectClock(o, state, log, 8000, out fixedNow) ?? after;
                if (fixedNow)
                    return new TimeSyncResult(true, TimeSyncMethod.WindowsSyncNow, before, after,
                        "Clock synchronized with Windows time sync.");
            }
            else if (syncNow == SyncNowOutcome.Declined && !IsWithin(after.Value, o))
            {
                // The user already said no to Windows' prompt; do not ask a second time.
                return new TimeSyncResult(false, TimeSyncMethod.Failed, before, after, DeclinedMessage);
            }

            bool stillOff = !IsWithin(after.Value, o);
            bool elevated = IsProcessElevated();

            // Step 2: we already have admin rights, fix it directly.
            if (elevated && (stillOff || !attempted))
            {
                TimeSyncMethod method;
                double? fixedOffset;
                if (TryElevatedFix(o, state, log, out method, out fixedOffset))
                {
                    // A late "Sync now" or a w32tm run that reported an error can still have fixed the clock.
                    if (method == TimeSyncMethod.AlreadyCorrect && !IsWithin(before.Value, o))
                        method = attempted ? TimeSyncMethod.WindowsSyncNow : TimeSyncMethod.W32tmResync;
                    return new TimeSyncResult(true, method, before, fixedOffset, DescribeMethod(method));
                }
                after = fixedOffset ?? after;
                stillOff = !IsWithin(after.Value, o);
            }

            // Step 3: ask for admin rights once and let the elevated copy of this app do step 2.
            if (stillOff && !elevated && o.AllowElevationPrompt)
            {
                string error;
                bool launched = RunElevatedHelper(log, out error);
                if (error != null)
                    return new TimeSyncResult(false, TimeSyncMethod.Failed, before, after, error);

                // Re-measure whatever the exit code: a timed-out or partly failed helper may still have fixed it.
                if (launched)
                {
                    bool fixedNow;
                    after = WaitForCorrectClock(o, state, log, 3000, out fixedNow) ?? after;
                    if (fixedNow)
                        return new TimeSyncResult(true, TimeSyncMethod.ElevatedHelper, before, after,
                            "Clock fixed with administrator rights.");
                }
                stillOff = !IsWithin(after.Value, o);
            }

            if (!stillOff)
                return new TimeSyncResult(true, TimeSyncMethod.AlreadyCorrect, before, after,
                    "Clock is correct; no sync method was available.");

            return new TimeSyncResult(false, TimeSyncMethod.Failed, before, after,
                "Could not correct the clock (off by " + FormatSeconds(after.Value) +
                "). Turn on 'Set time automatically' in Windows Date & time settings.");
        }

        /// <summary>Needs admin rights: w32tm resync first, then set the clock directly.</summary>
        private static bool TryElevatedFix(TimeSyncOptions o, MeasureState state, Action<string> log,
            out TimeSyncMethod method, out double? after)
        {
            method = TimeSyncMethod.Failed;
            after = null;
            Stopwatch watch = Stopwatch.StartNew();

            if (RunW32tmResync(log))
            {
                bool fixedNow;
                after = WaitForCorrectClock(o, state, log, 5000, out fixedNow);
                if (fixedNow)
                {
                    method = TimeSyncMethod.W32tmResync;
                    return true;
                }
            }

            // Keeps the elevated helper well inside the time its parent waits for it.
            if (watch.ElapsedMilliseconds > ElevatedFixBudgetMs)
            {
                log("Out of time; not setting the clock directly.");
                return false;
            }

            double? fresh = Measure(o, state, log);
            if (!fresh.HasValue)
            {
                log("No time reference reachable; cannot set the clock.");
                return false;
            }
            after = fresh;
            if (IsWithin(fresh.Value, o))
            {
                method = TimeSyncMethod.AlreadyCorrect;
                return true;
            }

            if (SetClock(fresh.Value, log))
            {
                bool fixedNow;
                after = WaitForCorrectClock(o, state, log, 3000, out fixedNow) ?? after;
                if (fixedNow)
                {
                    method = TimeSyncMethod.SetSystemTime;
                    return true;
                }
            }
            return false;
        }

        private static double? WaitForCorrectClock(TimeSyncOptions o, MeasureState state, Action<string> log,
            int maxWaitMs, out bool corrected)
        {
            Stopwatch watch = Stopwatch.StartNew();
            double? last = null;
            while (true)
            {
                double? offset = Measure(o, state, log);
                if (offset.HasValue)
                {
                    last = offset;
                    if (IsWithin(offset.Value, o))
                    {
                        corrected = true;
                        return offset;
                    }
                }
                if (watch.ElapsedMilliseconds + 1000 > maxWaitMs)
                    break;
                Thread.Sleep(1000);
            }
            corrected = false;
            return last;
        }

        private static bool IsWithin(double offset, TimeSyncOptions o)
        {
            return Math.Abs(offset) <= o.MaxAllowedOffsetSeconds;
        }

        private static string DescribeMethod(TimeSyncMethod method)
        {
            switch (method)
            {
                case TimeSyncMethod.W32tmResync: return "Clock synchronized with w32tm.";
                case TimeSyncMethod.SetSystemTime: return "Clock set from the reference time.";
                default: return "Clock is correct.";
            }
        }

        private static double? Measure(TimeSyncOptions o, MeasureState state, Action<string> log)
        {
            return MeasureOffset(o.NtpServers, o.HttpFallbackHosts, NtpTimeoutMs, HttpTimeoutMs, state, log);
        }

        private static double? MeasureOffset(IList<string> ntpServers, IList<string> httpHosts,
            int ntpBudgetMs, int httpBudgetMs, MeasureState state, Action<string> log)
        {
            if (!state.SkipNtp)
            {
                double? ntp = FirstAnswer(ntpServers, host => QueryNtp(host, ntpBudgetMs), ntpBudgetMs, "NTP", log);
                if (ntp.HasValue)
                    return ntp;
            }

            double? http = FirstAnswer(httpHosts, host => QueryHttpDate(host, httpBudgetMs), httpBudgetMs, "HTTP", log);
            if (http.HasValue && !state.SkipNtp)
            {
                // UDP 123 is probably blocked on this network; do not wait for NTP again in this run.
                state.SkipNtp = true;
                log("NTP did not answer; using HTTP for the rest of this check.");
            }
            return http;
        }

        /// <summary>Queries all hosts at once and returns the first usable answer within the budget.</summary>
        private static double? FirstAnswer(IList<string> hosts, Func<string, double?> query, int budgetMs,
            string source, Action<string> log)
        {
            if (hosts == null)
                return null;

            List<Task<double?>> pending = new List<Task<double?>>();
            List<string> names = new List<string>();
            foreach (string raw in hosts)
            {
                if (string.IsNullOrWhiteSpace(raw))
                    continue;
                string host = raw.Trim();
                // Dedicated threads: the queries block, and a busy thread pool must not delay them.
                Task<double?> task = Task.Factory.StartNew(() => query(host), CancellationToken.None,
                    TaskCreationOptions.LongRunning, TaskScheduler.Default);
                Observe(task);
                pending.Add(task);
                names.Add(host);
            }

            // Queries still running at the deadline are abandoned; each one ends on its own timeout.
            Stopwatch watch = Stopwatch.StartNew();
            while (pending.Count > 0)
            {
                long left = budgetMs - watch.ElapsedMilliseconds;
                if (left <= 0)
                    break;
                int index = Task.WaitAny(pending.ToArray(), (int)left);
                if (index < 0)
                    break;

                Task<double?> done = pending[index];
                string host = names[index];
                pending.RemoveAt(index);
                names.RemoveAt(index);

                double? offset = done.Status == TaskStatus.RanToCompletion ? done.Result : null;
                if (offset.HasValue)
                {
                    log("Offset from " + source + " " + host + ": " + FormatSeconds(offset.Value));
                    return offset;
                }
            }
            return null;
        }

        private static double? QueryNtp(string server, int timeoutMs)
        {
            try
            {
                Stopwatch watch = Stopwatch.StartNew();
                IPAddress address = ResolveHost(server, timeoutMs);
                if (address == null)
                    return null;

                using (Socket socket = new Socket(address.AddressFamily, SocketType.Dgram, ProtocolType.Udp))
                {
                    int remaining = Remaining(watch, timeoutMs);
                    socket.ReceiveTimeout = remaining;
                    socket.SendTimeout = remaining;
                    socket.Connect(new IPEndPoint(address, 123));

                    byte[] request = new byte[48];
                    request[0] = 0x23; // LI = 0, VN = 4, Mode = 3 (client)
                    byte[] buffer = new byte[512];

                    DateTime t1 = DateTime.UtcNow;
                    socket.Send(request);
                    int received = socket.Receive(buffer);
                    DateTime t4 = DateTime.UtcNow;

                    return ParseNtpResponse(buffer, received, t1, t4);
                }
            }
            catch (Exception)
            {
                return null;
            }
        }

        /// <summary>Returns the clock offset from an SNTP reply, or null when the reply is not usable.</summary>
        internal static double? ParseNtpResponse(byte[] data, int length, DateTime t1Utc, DateTime t4Utc)
        {
            if (data == null || length < 48 || data.Length < length)
                return null;

            int mode = data[0] & 0x07;
            if (mode != 4 && mode != 5)
                return null;

            int stratum = data[1];
            if (stratum < 1 || stratum > 15)
                return null;

            if (ReadUInt32BigEndian(data, 40) == 0 && ReadUInt32BigEndian(data, 44) == 0)
                return null;

            DateTime t3 = NtpTimestampToUtc(data, 40);
            bool hasReceive = ReadUInt32BigEndian(data, 32) != 0 || ReadUInt32BigEndian(data, 36) != 0;
            DateTime t2 = hasReceive ? NtpTimestampToUtc(data, 32) : t3;

            return ((t2 - t1Utc).TotalSeconds + (t3 - t4Utc).TotalSeconds) / 2.0;
        }

        /// <summary>Converts an NTP 32.32 timestamp (seconds since 1900) to UTC, handling era 1 (from 2036).</summary>
        internal static DateTime NtpTimestampToUtc(byte[] data, int offset)
        {
            uint seconds = ReadUInt32BigEndian(data, offset);
            uint fraction = ReadUInt32BigEndian(data, offset + 4);

            long totalSeconds = seconds;
            if (seconds < 0x80000000u)
                totalSeconds += 0x100000000L; // era 1 starts 2036-02-07 06:28:16 UTC

            long fractionTicks = (long)(fraction * (double)TimeSpan.TicksPerSecond / 4294967296.0);
            return NtpEpoch.AddTicks(totalSeconds * TimeSpan.TicksPerSecond + fractionTicks);
        }

        private static uint ReadUInt32BigEndian(byte[] data, int offset)
        {
            return ((uint)data[offset] << 24) | ((uint)data[offset + 1] << 16) |
                   ((uint)data[offset + 2] << 8) | data[offset + 3];
        }

        /// <summary>Plain HTTP on port 80 (not TLS, which can fail when the clock is badly wrong).</summary>
        private static double? QueryHttpDate(string host, int timeoutMs)
        {
            try
            {
                Stopwatch watch = Stopwatch.StartNew();
                IPAddress address = ResolveHost(host, timeoutMs);
                if (address == null)
                    return null;

                using (TcpClient client = new TcpClient(address.AddressFamily))
                {
                    Task connect = client.ConnectAsync(address, 80);
                    Observe(connect);
                    if (!connect.Wait(Remaining(watch, timeoutMs)) || !client.Connected)
                        return null;

                    NetworkStream stream = client.GetStream();
                    client.SendTimeout = Remaining(watch, timeoutMs);
                    byte[] request = Encoding.ASCII.GetBytes(
                        "HEAD / HTTP/1.1\r\nHost: " + host + "\r\nUser-Agent: AutoTimeSync\r\nAccept: */*\r\nConnection: close\r\n\r\n");

                    DateTime t1 = DateTime.UtcNow;
                    stream.Write(request, 0, request.Length);

                    DateTime t4;
                    string headers = ReadHeaders(client, stream, watch, timeoutMs, out t4);

                    DateTime serverDate;
                    if (headers == null || !TryParseHttpDate(headers, out serverDate))
                        return null;
                    return ComputeHttpOffset(serverDate, t1, t4);
                }
            }
            catch (Exception)
            {
                return null;
            }
        }

        private static string ReadHeaders(TcpClient client, NetworkStream stream, Stopwatch watch, int timeoutMs, out DateTime firstByteUtc)
        {
            firstByteUtc = DateTime.MinValue;
            StringBuilder text = new StringBuilder();
            byte[] buffer = new byte[4096];

            while (text.Length < 16384 && watch.ElapsedMilliseconds < timeoutMs)
            {
                client.ReceiveTimeout = Remaining(watch, timeoutMs);
                int read = stream.Read(buffer, 0, buffer.Length);
                if (read <= 0)
                    break;
                if (firstByteUtc == DateTime.MinValue)
                    firstByteUtc = DateTime.UtcNow;
                text.Append(Encoding.ASCII.GetString(buffer, 0, read));
                if (text.ToString().Contains("\r\n\r\n"))
                    break;
            }
            return firstByteUtc == DateTime.MinValue ? null : text.ToString();
        }

        private static readonly string[] HttpDateFormats =
        {
            "r",                                   // Sun, 06 Nov 1994 08:49:37 GMT
            "ddd, d MMM yyyy HH':'mm':'ss 'GMT'",
            "dddd, dd'-'MMM'-'yy HH':'mm':'ss 'GMT'", // RFC 850
            "ddd MMM d HH':'mm':'ss yyyy"          // asctime
        };

        /// <summary>Finds the Date header in an HTTP response head and parses it as UTC.</summary>
        internal static bool TryParseHttpDate(string responseHead, out DateTime dateUtc)
        {
            dateUtc = DateTime.MinValue;
            if (string.IsNullOrEmpty(responseHead))
                return false;

            foreach (string rawLine in responseHead.Split('\n'))
            {
                string line = rawLine.TrimEnd('\r');
                if (line.Length == 0)
                    break; // end of headers
                if (!line.StartsWith("Date:", StringComparison.OrdinalIgnoreCase))
                    continue;

                string value = line.Substring(5).Trim();
                return DateTime.TryParseExact(value, HttpDateFormats, CultureInfo.InvariantCulture,
                    DateTimeStyles.AllowWhiteSpaces | DateTimeStyles.AssumeUniversal | DateTimeStyles.AdjustToUniversal,
                    out dateUtc);
            }
            return false;
        }

        /// <summary>The Date header is truncated to the second, so add half a second.</summary>
        internal static double ComputeHttpOffset(DateTime serverDateUtc, DateTime t1Utc, DateTime t4Utc)
        {
            DateTime midpoint = t1Utc.AddTicks((t4Utc - t1Utc).Ticks / 2);
            return (serverDateUtc - midpoint).TotalSeconds + 0.5;
        }

        private static IPAddress ResolveHost(string host, int timeoutMs)
        {
            IPAddress literal;
            if (IPAddress.TryParse(host, out literal))
                return literal;

            try
            {
                Task<IPAddress[]> lookup = Dns.GetHostAddressesAsync(host);
                Observe(lookup);
                if (!lookup.Wait(timeoutMs) || lookup.Result == null)
                    return null;

                IPAddress fallback = null;
                foreach (IPAddress address in lookup.Result)
                {
                    if (address.AddressFamily == AddressFamily.InterNetwork)
                        return address;
                    if (fallback == null && address.AddressFamily == AddressFamily.InterNetworkV6)
                        fallback = address;
                }
                return fallback;
            }
            catch (Exception)
            {
                return null;
            }
        }

        private static int Remaining(Stopwatch watch, int timeoutMs)
        {
            long left = timeoutMs - watch.ElapsedMilliseconds;
            return left < 100 ? 100 : (int)left;
        }

        /// <summary>Marks a task's exception as seen, for tasks that are abandoned after a timeout.</summary>
        private static void Observe(Task task)
        {
            task.ContinueWith(t => { var ignored = t.Exception; },
                CancellationToken.None,
                TaskContinuationOptions.OnlyOnFaulted | TaskContinuationOptions.ExecuteSynchronously,
                TaskScheduler.Default);
        }

        private static SyncNowOutcome RunWindowsSyncNow(TimeSyncOptions o, Action<string> log)
        {
            string exe = SystemToolPath("SystemSettingsAdminFlows.exe");
            if (!File.Exists(exe))
            {
                log("SystemSettingsAdminFlows.exe not found; skipping Windows Sync now.");
                return SyncNowOutcome.NotAvailable;
            }

            log("Running Windows Sync now.");
            if (IsWow64())
                return RunWindowsSyncNowFromWow64(o, log);

            try
            {
                // Shell execute is required: the tool auto-elevates for administrators.
                ProcessStartInfo info = new ProcessStartInfo(exe, "ForceTimeSync 1")
                {
                    UseShellExecute = true,
                    WindowStyle = ProcessWindowStyle.Hidden
                };
                using (Process process = Process.Start(info))
                {
                    if (process == null)
                        return SyncNowOutcome.NotAvailable;
                    if (process.WaitForExit(Math.Max(1000, o.SyncNowTimeoutMs)))
                        log("Windows Sync now exited with code " + process.ExitCode + ".");
                    else
                        log("Windows Sync now is still running; checking the clock anyway.");
                }
                return SyncNowOutcome.Ran;
            }
            catch (Win32Exception ex) when (ex.NativeErrorCode == ErrorCancelled)
            {
                log("Windows Sync now was cancelled at the permission prompt.");
                return SyncNowOutcome.Declined;
            }
            catch (Exception ex)
            {
                log("Windows Sync now failed: " + ex.Message);
                return SyncNowOutcome.NotAvailable;
            }
        }

        /// <summary>
        /// A 32-bit process cannot shell-execute the Sysnative path when elevation is involved (the elevation
        /// service is 64-bit and has no Sysnative folder), so the native 64-bit cmd starts the System32 copy.
        /// </summary>
        private static SyncNowOutcome RunWindowsSyncNowFromWow64(TimeSyncOptions o, Action<string> log)
        {
            string windows = WindowsDirectory();
            string cmd = Path.Combine(windows, "Sysnative", "cmd.exe");
            if (!File.Exists(cmd))
            {
                log("64-bit cmd.exe not found; skipping Windows Sync now.");
                return SyncNowOutcome.NotAvailable;
            }

            string target = Path.Combine(windows, "System32", "SystemSettingsAdminFlows.exe");
            int code = RunHidden(cmd, "/c start \"\" /wait \"" + target + "\" ForceTimeSync 1",
                Math.Max(1000, o.SyncNowTimeoutMs), log);
            if (code == ErrorCancelled)
            {
                log("Windows Sync now was cancelled at the permission prompt.");
                return SyncNowOutcome.Declined;
            }
            log("Windows Sync now exited with code " + code + ".");
            return SyncNowOutcome.Ran;
        }

        private static bool RunW32tmResync(Action<string> log)
        {
            StartTimeService(log);
            string w32tm = SystemToolPath("w32tm.exe");

            Stopwatch watch = Stopwatch.StartNew();
            int code = RunHidden(w32tm, "/resync /force", W32tmTimeoutMs, log);
            // Older w32tm builds reject /force at once; only that quick failure is worth a plain retry.
            if (code != 0 && watch.ElapsedMilliseconds < 2000)
                code = RunHidden(w32tm, "/resync", W32tmTimeoutMs, log);

            log("w32tm /resync exited with code " + code + ".");
            return code == 0;
        }

        private static void StartTimeService(Action<string> log)
        {
            IntPtr manager = IntPtr.Zero;
            IntPtr service = IntPtr.Zero;
            try
            {
                manager = NativeMethods.OpenSCManager(null, null, NativeMethods.SC_MANAGER_CONNECT);
                if (manager == IntPtr.Zero)
                {
                    log("OpenSCManager failed: " + Marshal.GetLastWin32Error());
                    return;
                }

                service = NativeMethods.OpenService(manager, "W32Time",
                    NativeMethods.SERVICE_START | NativeMethods.SERVICE_QUERY_STATUS);
                if (service == IntPtr.Zero)
                {
                    log("OpenService(W32Time) failed: " + Marshal.GetLastWin32Error());
                    return;
                }

                if (!NativeMethods.StartService(service, 0, IntPtr.Zero))
                {
                    int error = Marshal.GetLastWin32Error();
                    if (error != NativeMethods.ERROR_SERVICE_ALREADY_RUNNING)
                    {
                        log("Starting W32Time failed: " + error);
                        return;
                    }
                }

                for (int i = 0; i < 30; i++)
                {
                    NativeMethods.SERVICE_STATUS status;
                    if (NativeMethods.QueryServiceStatus(service, out status) &&
                        status.dwCurrentState == NativeMethods.SERVICE_RUNNING)
                        return;
                    Thread.Sleep(100);
                }
                log("W32Time did not report running in time.");
            }
            catch (Exception ex)
            {
                log("Starting W32Time failed: " + ex.Message);
            }
            finally
            {
                if (service != IntPtr.Zero)
                    NativeMethods.CloseServiceHandle(service);
                if (manager != IntPtr.Zero)
                    NativeMethods.CloseServiceHandle(manager);
            }
        }

        private static bool SetClock(double offsetSeconds, Action<string> log)
        {
            try
            {
                if (!EnablePrivilege("SeSystemtimePrivilege"))
                {
                    log("SeSystemtimePrivilege is not available.");
                    return false;
                }

                DateTime target = DateTime.UtcNow.AddSeconds(offsetSeconds);
                NativeMethods.SYSTEMTIME time = new NativeMethods.SYSTEMTIME
                {
                    wYear = (ushort)target.Year,
                    wMonth = (ushort)target.Month,
                    wDayOfWeek = (ushort)target.DayOfWeek,
                    wDay = (ushort)target.Day,
                    wHour = (ushort)target.Hour,
                    wMinute = (ushort)target.Minute,
                    wSecond = (ushort)target.Second,
                    wMilliseconds = (ushort)target.Millisecond
                };

                if (!NativeMethods.SetSystemTime(ref time))
                {
                    log("SetSystemTime failed: " + Marshal.GetLastWin32Error());
                    return false;
                }
                log("System clock set by " + FormatSeconds(offsetSeconds) + ".");
                return true;
            }
            catch (Exception ex)
            {
                log("SetSystemTime failed: " + ex.Message);
                return false;
            }
        }

        private static bool EnablePrivilege(string name)
        {
            IntPtr token;
            if (!NativeMethods.OpenProcessToken(NativeMethods.GetCurrentProcess(),
                    NativeMethods.TOKEN_ADJUST_PRIVILEGES | NativeMethods.TOKEN_QUERY, out token))
                return false;

            try
            {
                NativeMethods.TOKEN_PRIVILEGES privileges = new NativeMethods.TOKEN_PRIVILEGES
                {
                    PrivilegeCount = 1,
                    Attributes = NativeMethods.SE_PRIVILEGE_ENABLED
                };
                if (!NativeMethods.LookupPrivilegeValue(null, name, out privileges.Luid))
                    return false;
                if (!NativeMethods.AdjustTokenPrivileges(token, false, ref privileges, 0, IntPtr.Zero, IntPtr.Zero))
                    return false;
                return Marshal.GetLastWin32Error() != NativeMethods.ERROR_NOT_ALL_ASSIGNED;
            }
            finally
            {
                NativeMethods.CloseHandle(token);
            }
        }

        /// <summary>
        /// Relaunches this app elevated. Returns true when the helper ran, whatever its exit code;
        /// <paramref name="error"/> is set when the user declined the UAC prompt.
        /// </summary>
        private static bool RunElevatedHelper(Action<string> log, out string error)
        {
            error = null;
            string exe;
            string arguments;
            if (!TryGetRelaunchCommand(out exe, out arguments))
            {
                log("Could not determine the current executable; skipping elevation.");
                return false;
            }

            log("Asking for administrator rights to fix the clock.");
            try
            {
                ProcessStartInfo info = new ProcessStartInfo(exe, arguments)
                {
                    UseShellExecute = true,
                    Verb = "runas",
                    WindowStyle = ProcessWindowStyle.Hidden
                };
                using (Process process = Process.Start(info))
                {
                    if (process == null)
                        return false;
                    if (process.WaitForExit(ElevatedHelperTimeoutMs))
                        log("Elevated helper exited with code " + process.ExitCode + ".");
                    else
                        log("Elevated helper did not finish in time; checking the clock anyway.");
                    return true;
                }
            }
            catch (Win32Exception ex) when (ex.NativeErrorCode == ErrorCancelled)
            {
                log("Administrator permission was declined.");
                error = DeclinedMessage;
                return false;
            }
            catch (Exception ex)
            {
                log("Elevated helper failed: " + ex.Message);
                return false;
            }
        }

        private static bool TryGetRelaunchCommand(out string exe, out string arguments)
        {
            exe = null;
            arguments = ElevatedFlag;
            try
            {
                using (Process current = Process.GetCurrentProcess())
                    exe = current.MainModule.FileName;
                if (string.IsNullOrEmpty(exe))
                    return false;

                // Started as "dotnet app.dll": relaunch the host with the same assembly.
                if (string.Equals(Path.GetFileNameWithoutExtension(exe), "dotnet", StringComparison.OrdinalIgnoreCase))
                {
                    Assembly entry = Assembly.GetEntryAssembly();
                    string location = entry == null ? null : entry.Location;
                    if (string.IsNullOrEmpty(location))
                        return false;
                    arguments = "\"" + location + "\" " + ElevatedFlag;
                }
                return true;
            }
            catch (Exception)
            {
                return false;
            }
        }

        private static int RunHidden(string exe, string arguments, int timeoutMs, Action<string> log)
        {
            try
            {
                ProcessStartInfo info = new ProcessStartInfo(exe, arguments)
                {
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    WindowStyle = ProcessWindowStyle.Hidden,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true
                };
                using (Process process = Process.Start(info))
                {
                    if (process == null)
                        return -1;
                    Task<string> output = process.StandardOutput.ReadToEndAsync();
                    Task<string> errors = process.StandardError.ReadToEndAsync();
                    Observe(output);
                    Observe(errors);
                    if (!process.WaitForExit(timeoutMs))
                    {
                        log(Path.GetFileName(exe) + " timed out.");
                        try
                        {
                            process.Kill();
                        }
                        catch (Exception)
                        {
                            // Already exited, or access denied; nothing more to do.
                        }
                        return -1;
                    }
                    if (output.Wait(1000))
                        LogFirstLine(log, output.Result);
                    if (errors.Wait(1000))
                        LogFirstLine(log, errors.Result);
                    return process.ExitCode;
                }
            }
            catch (Exception ex)
            {
                log(Path.GetFileName(exe) + " failed: " + ex.Message);
                return -1;
            }
        }

        private static void LogFirstLine(Action<string> log, string text)
        {
            if (string.IsNullOrWhiteSpace(text))
                return;
            foreach (string line in text.Split('\n'))
            {
                string trimmed = line.Trim();
                if (trimmed.Length > 0)
                {
                    log(trimmed);
                    return;
                }
            }
        }

        private static string WindowsDirectory()
        {
            string windows = Environment.GetEnvironmentVariable("windir");
            return string.IsNullOrEmpty(windows) ? Environment.GetFolderPath(Environment.SpecialFolder.Windows) : windows;
        }

        /// <summary>A 32-bit process on 64-bit Windows, which sees System32 redirected to SysWOW64.</summary>
        private static bool IsWow64()
        {
            return Environment.Is64BitOperatingSystem && !Environment.Is64BitProcess;
        }

        private static string SystemToolPath(string exeName)
        {
            string windows = WindowsDirectory();
            if (IsWow64())
            {
                string native = Path.Combine(windows, "Sysnative", exeName);
                if (File.Exists(native))
                    return native;
            }
            return Path.Combine(windows, "System32", exeName);
        }

        private static bool IsProcessElevated()
        {
            IntPtr token;
            if (!NativeMethods.OpenProcessToken(NativeMethods.GetCurrentProcess(), NativeMethods.TOKEN_QUERY, out token))
                return false;
            try
            {
                int elevation;
                int returned;
                return NativeMethods.GetTokenInformation(token, NativeMethods.TokenElevation,
                           out elevation, sizeof(int), out returned) && elevation != 0;
            }
            finally
            {
                NativeMethods.CloseHandle(token);
            }
        }

        private static bool IsWindows()
        {
            return Environment.OSVersion.Platform == PlatformID.Win32NT;
        }

        private static bool HasElevatedFlag(string[] args)
        {
            if (args == null)
                return false;
            foreach (string arg in args)
            {
                if (string.Equals(arg, ElevatedFlag, StringComparison.OrdinalIgnoreCase))
                    return true;
            }
            return false;
        }

        private static string[] SafeCommandLineArgs()
        {
            try
            {
                return Environment.GetCommandLineArgs();
            }
            catch (Exception)
            {
                return null;
            }
        }

        private static Action<string> SafeLog(Action<string> log)
        {
            return message =>
            {
                if (log == null)
                    return;
                try
                {
                    log("[TimeSync] " + message);
                }
                catch
                {
                    // A broken log callback must never break the sync.
                }
            };
        }

        private static string FormatSeconds(double seconds)
        {
            return seconds.ToString("+0.000;-0.000;0.000", CultureInfo.InvariantCulture) + " s";
        }

        private static class NativeMethods
        {
            public const uint TOKEN_QUERY = 0x0008;
            public const uint TOKEN_ADJUST_PRIVILEGES = 0x0020;
            public const int TokenElevation = 20;
            public const uint SE_PRIVILEGE_ENABLED = 0x00000002;
            public const int ERROR_NOT_ALL_ASSIGNED = 1300;

            public const uint SC_MANAGER_CONNECT = 0x0001;
            public const uint SERVICE_QUERY_STATUS = 0x0004;
            public const uint SERVICE_START = 0x0010;
            public const uint SERVICE_RUNNING = 0x00000004;
            public const int ERROR_SERVICE_ALREADY_RUNNING = 1056;

            [StructLayout(LayoutKind.Sequential)]
            public struct LUID
            {
                public uint LowPart;
                public int HighPart;
            }

            [StructLayout(LayoutKind.Sequential)]
            public struct TOKEN_PRIVILEGES
            {
                public uint PrivilegeCount;
                public LUID Luid;
                public uint Attributes;
            }

            [StructLayout(LayoutKind.Sequential)]
            public struct SYSTEMTIME
            {
                public ushort wYear;
                public ushort wMonth;
                public ushort wDayOfWeek;
                public ushort wDay;
                public ushort wHour;
                public ushort wMinute;
                public ushort wSecond;
                public ushort wMilliseconds;
            }

            [StructLayout(LayoutKind.Sequential)]
            public struct SERVICE_STATUS
            {
                public uint dwServiceType;
                public uint dwCurrentState;
                public uint dwControlsAccepted;
                public uint dwWin32ExitCode;
                public uint dwServiceSpecificExitCode;
                public uint dwCheckPoint;
                public uint dwWaitHint;
            }

            [DllImport("kernel32.dll")]
            public static extern IntPtr GetCurrentProcess();

            [DllImport("kernel32.dll", SetLastError = true)]
            [return: MarshalAs(UnmanagedType.Bool)]
            public static extern bool CloseHandle(IntPtr handle);

            [DllImport("kernel32.dll", SetLastError = true)]
            [return: MarshalAs(UnmanagedType.Bool)]
            public static extern bool SetSystemTime(ref SYSTEMTIME time);

            [DllImport("advapi32.dll", SetLastError = true)]
            [return: MarshalAs(UnmanagedType.Bool)]
            public static extern bool OpenProcessToken(IntPtr process, uint desiredAccess, out IntPtr token);

            [DllImport("advapi32.dll", SetLastError = true)]
            [return: MarshalAs(UnmanagedType.Bool)]
            public static extern bool GetTokenInformation(IntPtr token, int infoClass, out int info, int length, out int returnLength);

            [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
            [return: MarshalAs(UnmanagedType.Bool)]
            public static extern bool LookupPrivilegeValue(string systemName, string name, out LUID luid);

            [DllImport("advapi32.dll", SetLastError = true)]
            [return: MarshalAs(UnmanagedType.Bool)]
            public static extern bool AdjustTokenPrivileges(IntPtr token, [MarshalAs(UnmanagedType.Bool)] bool disableAll,
                ref TOKEN_PRIVILEGES newState, int bufferLength, IntPtr previousState, IntPtr returnLength);

            [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
            public static extern IntPtr OpenSCManager(string machineName, string databaseName, uint desiredAccess);

            [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
            public static extern IntPtr OpenService(IntPtr manager, string serviceName, uint desiredAccess);

            [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
            [return: MarshalAs(UnmanagedType.Bool)]
            public static extern bool StartService(IntPtr service, int argumentCount, IntPtr arguments);

            [DllImport("advapi32.dll", SetLastError = true)]
            [return: MarshalAs(UnmanagedType.Bool)]
            public static extern bool QueryServiceStatus(IntPtr service, out SERVICE_STATUS status);

            [DllImport("advapi32.dll", SetLastError = true)]
            [return: MarshalAs(UnmanagedType.Bool)]
            public static extern bool CloseServiceHandle(IntPtr handle);
        }
    }
}
