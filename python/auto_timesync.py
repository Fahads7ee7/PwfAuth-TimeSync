"""Fix the Windows clock before a pwfauth license login.

Every pwfauth request carries a signed timestamp taken from the local clock, and
the server rejects it when the clock is more than 300 seconds off. Most customers
never run into this, but the few whose clock has drifted would see "key rejected"
and have to press "Sync now" in Windows Settings themselves. Calling
ensure_clock_is_correct() before the login fixes the clock silently, so the login
just works for everyone.

What customers see:

* Clock already correct (almost everyone): nothing. The check takes well under a second.
* Clock off, account in the Administrators group (the usual setup on home PCs):
  nothing either. Windows' own "Sync now" runs silently and fixes the clock.
* Clock off, standard-user account: Windows shows its own prompt for administrator
  permission, exactly as when the user presses "Sync now" in Settings. Declining it
  is reported as a Failed result; the user is not asked a second time.

Usage::

    import auto_timesync

    if __name__ == "__main__":
        auto_timesync.handle_elevated_command()   # first line of the program
        auto_timesync.ensure_clock_is_correct()
        # ... license login ...

The elevated fallback (relaunching this program as administrator when nothing else
worked) is only used when handle_elevated_command() was called at startup.

Standard library only, Python 3.9+. On other platforms every call is a no-op.

Author: Fahads7ee7
"""

import calendar
import email.utils
import os
import socket
import struct
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, replace
from enum import Enum
from typing import Callable, List, Optional, Sequence, Tuple, Union

__all__ = [
    "TimeSyncMethod",
    "TimeSyncOptions",
    "TimeSyncResult",
    "ensure_clock_is_correct",
    "get_clock_offset_seconds",
    "handle_elevated_command",
    "ELEVATED_FLAG",
]

ELEVATED_FLAG = "--pwf-timesync-elevated"

DEFAULT_NTP_SERVERS = ("time.windows.com", "time.google.com", "time.cloudflare.com", "pool.ntp.org")
DEFAULT_HTTP_FALLBACK_HOSTS = ("www.msftconnecttest.com", "www.google.com")

_NTP_PORT = 123
_NTP_TIMEOUT = 1.5
_HTTP_TIMEOUT = 3.0
_MEASURE_TIMEOUT_MS = 3000
_NTP_TO_UNIX = 2208988800  # seconds from 1900-01-01 to 1970-01-01
_NTP_ERA_SECONDS = 1 << 32
_MIN_SANE_TIME = 978307200  # 2001-01-01, rejects obviously broken Date headers
_MAX_SOURCE_DISAGREEMENT = 5.0  # seconds; two references must agree before SetSystemTime

_SYNC_NOW_EXE = "SystemSettingsAdminFlows.exe"
_SYNC_NOW_ARGS = "ForceTimeSync 1"
_SYNC_NOW_SETTLE = 8.0
_RESYNC_SETTLE = 5.0
_W32TM_TIMEOUT = 10.0
_ELEVATED_WAIT_MS = 60000

_elevated_handler_ready = False


class TimeSyncMethod(str, Enum):
    ALREADY_CORRECT = "AlreadyCorrect"
    WINDOWS_SYNC_NOW = "WindowsSyncNow"
    W32TM_RESYNC = "W32tmResync"
    SET_SYSTEM_TIME = "SetSystemTime"
    ELEVATED_HELPER = "ElevatedHelper"
    NO_REFERENCE = "NoReference"
    FAILED = "Failed"
    SKIPPED = "Skipped"

    def __str__(self) -> str:
        return self.value


@dataclass
class TimeSyncOptions:
    max_allowed_offset_seconds: float = 30.0
    allow_elevation_prompt: bool = True
    force_sync: bool = False
    sync_now_timeout_ms: int = 15000
    ntp_servers: Sequence[str] = DEFAULT_NTP_SERVERS
    http_fallback_hosts: Sequence[str] = DEFAULT_HTTP_FALLBACK_HOSTS
    log: Optional[Callable[[str], None]] = None


@dataclass
class TimeSyncResult:
    success: bool
    method: TimeSyncMethod
    offset_before_seconds: Optional[float]
    offset_after_seconds: Optional[float]
    message: str


def ensure_clock_is_correct(options: Optional[TimeSyncOptions] = None, **overrides) -> TimeSyncResult:
    """Make sure the UTC clock is close to a network reference. Never raises.

    Keyword overrides are applied on top of ``options``, for example
    ``ensure_clock_is_correct(force_sync=True, allow_elevation_prompt=False)``.
    """
    log = _Logger(getattr(options, "log", None) or overrides.get("log"))
    try:
        opts = replace(options or TimeSyncOptions(), **overrides)
        return _ensure(opts, _Logger(opts.log))
    except Exception as exc:  # the caller must always get a result
        log("Time sync failed unexpectedly: %r" % (exc,))
        return TimeSyncResult(False, TimeSyncMethod.FAILED, None, None, "Unexpected error: %s" % (exc,))


def get_clock_offset_seconds(timeout_ms: int = _MEASURE_TIMEOUT_MS,
                             ntp_servers: Optional[Sequence[str]] = None,
                             http_fallback_hosts: Optional[Sequence[str]] = None) -> Optional[float]:
    """Return (reference UTC - local UTC) in seconds, or None if no reference answered.

    ``timeout_ms`` is the time budget of each stage: NTP first, then the HTTP fallback.
    """
    try:
        return _measure_offset(
            DEFAULT_NTP_SERVERS if ntp_servers is None else ntp_servers,
            DEFAULT_HTTP_FALLBACK_HOSTS if http_fallback_hosts is None else http_fallback_hosts,
            timeout_ms,
            _Logger(None),
        )
    except Exception:
        return None


def handle_elevated_command(argv: Optional[Sequence[str]] = None) -> bool:
    """Call first thing in the program. Returns False unless started as the elevated helper.

    When ``argv`` contains ELEVATED_FLAG the elevated fix runs and the process
    exits with 0 (clock fixed) or 1 (failed); this function then never returns.
    """
    global _elevated_handler_ready
    args = sys.argv[1:] if argv is None else argv
    if ELEVATED_FLAG not in args:
        _elevated_handler_ready = True  # a relaunch with the flag will be handled here
        return False

    exit_code = 1
    try:
        if sys.platform == "win32":
            opts = TimeSyncOptions()
            method, _ = _fix_as_admin(opts, _Logger(None))
            exit_code = 0 if method is not None else 1
    except Exception:
        exit_code = 1
    _hard_exit(exit_code)
    return True  # not reached


class _Logger:
    def __init__(self, callback: Optional[Callable[[str], None]]):
        self._callback = callback

    def __call__(self, message: str) -> None:
        if self._callback is None:
            return
        try:
            self._callback("[TimeSync] " + message)
        except Exception:
            pass


def _ensure(opts: TimeSyncOptions, log: _Logger) -> TimeSyncResult:
    if sys.platform != "win32":
        return TimeSyncResult(True, TimeSyncMethod.SKIPPED, None, None, "Not Windows; nothing to do.")

    threshold = abs(float(opts.max_allowed_offset_seconds))

    def within(offset: Optional[float]) -> bool:
        return offset is not None and abs(offset) <= threshold

    def done(method: TimeSyncMethod, after: Optional[float], message: str) -> TimeSyncResult:
        log(message)
        return TimeSyncResult(method != TimeSyncMethod.FAILED, method, before, after, message)

    before = _measure(opts, log)
    if before is None:
        return done(TimeSyncMethod.NO_REFERENCE, None, "No time reference reachable; clock left unchanged.")
    log("Clock offset: %+.3f s (threshold %.0f s)" % (before, threshold))

    if within(before) and not opts.force_sync:
        return done(TimeSyncMethod.ALREADY_CORRECT, before, "Clock is correct.")

    latest = before
    # Step 3: the same thing the Settings "Sync now" button does.
    sync_error = _run_sync_now(opts, log)
    if sync_error == _ERROR_CANCELLED:
        return done(TimeSyncMethod.FAILED, _measure(opts, log),
                    "Administrator permission was declined; the clock could not be fixed.")
    if sync_error == 0:
        after = _wait_until_within(opts, log, within, _SYNC_NOW_SETTLE)
        if within(after):
            return done(TimeSyncMethod.WINDOWS_SYNC_NOW, after, "Clock synchronized with Windows Sync now.")
        log("Still off after Sync now: %s" % _fmt(after))
        if after is not None:
            latest = after

    win = _win32()
    if win.is_elevated():
        method, after = _fix_as_admin(opts, log)
        if method is not None:
            return done(method, after, "Clock fixed (%s)." % method)
    elif opts.allow_elevation_prompt and not within(latest):
        result = _run_elevated_helper(opts, log, within)
        if result is not None:
            method, after, message = result
            return done(method, after, message)

    after = _measure(opts, log)
    if within(after):
        # Typically force_sync on a correct clock where no sync method was available.
        return done(TimeSyncMethod.ALREADY_CORRECT, after, "Clock is within the threshold; no sync was applied.")
    return done(TimeSyncMethod.FAILED, after,
                "Clock is still off (%s) and could not be fixed automatically." % _fmt(after))


def _fix_as_admin(opts: TimeSyncOptions, log: _Logger) -> Tuple[Optional[TimeSyncMethod], Optional[float]]:
    """Step 4, needs an elevated process: W32Time resync, then SetSystemTime as a last resort."""
    threshold = abs(float(opts.max_allowed_offset_seconds))

    def within(offset: Optional[float]) -> bool:
        return offset is not None and abs(offset) <= threshold

    win = _win32()
    win.start_w32time(log)
    _run_w32tm_resync(log)
    after = _wait_until_within(opts, log, within, _RESYNC_SETTLE)
    if within(after):
        return TimeSyncMethod.W32TM_RESYNC, after

    first = _measure_with_source(opts.ntp_servers, opts.http_fallback_hosts, _MEASURE_TIMEOUT_MS, log)
    if first is None:
        log("No reference for SetSystemTime; clock left unchanged.")
        return None, None
    fresh, source = first
    if within(fresh):
        return TimeSyncMethod.W32TM_RESYNC, fresh

    fresh = _confirm_offset(opts, log, fresh, source)
    if fresh is None:
        return None, first[0]
    if not win.set_system_time_utc(time.time() + fresh, log):
        return None, fresh
    after = _measure(opts, log)
    if within(after):
        return TimeSyncMethod.SET_SYSTEM_TIME, after
    return None, after


def _confirm_offset(opts: TimeSyncOptions, log: _Logger, offset: float, source: str) -> Optional[float]:
    """Ask a second, different reference before setting the clock from an unauthenticated answer."""
    second = _measure_offset([s for s in opts.ntp_servers if s != source],
                             [h for h in opts.http_fallback_hosts if h != source],
                             _MEASURE_TIMEOUT_MS, log)
    if second is None or abs(second - offset) > _MAX_SOURCE_DISAGREEMENT:
        log("%s (%+.1f s) is not confirmed by a second reference (%s); clock left unchanged."
            % (source, offset, _fmt(second)))
        return None
    return (offset + second) / 2.0


def _run_elevated_helper(opts: TimeSyncOptions, log: _Logger, within) -> Optional[Tuple[TimeSyncMethod, Optional[float], str]]:
    """Step 5: relaunch this program elevated with ELEVATED_FLAG and wait for it."""
    if not _elevated_handler_ready:
        log("handle_elevated_command() was not called at startup; not relaunching elevated.")
        return None
    command = _relaunch_command()
    if command is None:
        log("Cannot relaunch this program elevated (no script path).")
        return None
    exe, params = command
    log("Asking for administrator permission to fix the clock.")
    error, exit_code = _win32().shell_execute_wait(exe, params, "runas", _ELEVATED_WAIT_MS, os.getcwd())
    if error == _ERROR_CANCELLED:
        return (TimeSyncMethod.FAILED, _measure(opts, log),
                "Administrator permission was declined; the clock could not be fixed.")
    if error:
        log("Elevated relaunch failed (error %d)." % error)
        return None
    log("Elevated helper finished with exit code %s." % exit_code)

    after = _wait_until_within(opts, log, within, 3.0)
    if within(after):
        return TimeSyncMethod.ELEVATED_HELPER, after, "Clock fixed by the elevated helper."
    return None


def _relaunch_command() -> Optional[Tuple[str, str]]:
    frozen = getattr(sys, "frozen", False) or "__compiled__" in globals()
    if frozen:
        return sys.executable, ELEVATED_FLAG

    script = sys.argv[0] if sys.argv else ""
    module = _main_module_name()
    if "ipykernel" in os.path.basename(script) or (module or "").startswith("ipykernel"):
        return None  # an interactive kernel would never exit
    if module:
        # Started with "python -m package": a plain script relaunch would break relative imports.
        return sys.executable, subprocess.list2cmdline(["-m", module, ELEVATED_FLAG])
    if not script or script == "-c" or not os.path.exists(script):
        return None
    return sys.executable, subprocess.list2cmdline([os.path.abspath(script), ELEVATED_FLAG])


def _main_module_name() -> Optional[str]:
    spec = getattr(sys.modules.get("__main__"), "__spec__", None)
    name = getattr(spec, "name", None)
    if not name or name == "__main__":
        return None
    suffix = ".__main__"
    return name[:-len(suffix)] if name.endswith(suffix) else name


def _run_sync_now(opts: TimeSyncOptions, log: _Logger) -> Optional[int]:
    """Step 3. Returns None when Sync now is not available, 0 when it ran, else the Win32 error."""
    system_dir = _system_dir()
    exe = os.path.join(system_dir, _SYNC_NOW_EXE)
    if not os.path.isfile(exe):
        log("%s not found; skipping Sync now." % _SYNC_NOW_EXE)
        return None
    log("Running Windows Sync now.")
    timeout_ms = int(opts.sync_now_timeout_ms)
    if os.path.basename(system_dir).lower() == "sysnative":
        error, exit_code = _sync_now_from_wow64(system_dir, timeout_ms)
    else:
        # The exe requires administrator and auto-elevates, so it must go through ShellExecuteEx.
        error, exit_code = _win32().shell_execute_wait(exe, _SYNC_NOW_ARGS, None, timeout_ms)
    if error:
        log("Sync now could not start (error %d)." % error)
        return error
    log("Sync now exit code: %s" % ("timeout" if exit_code is None else exit_code))
    return 0


def _sync_now_from_wow64(sysnative: str, timeout_ms: int) -> Tuple[int, Optional[int]]:
    """Launch Sync now from a 32-bit process through the 64-bit cmd.exe.

    The elevation is done by a 64-bit service that cannot see Sysnative, and auto-elevation
    only applies to the real System32 path, so the 64-bit cmd does the shell launch instead.
    """
    cmd = os.path.join(sysnative, "cmd.exe")
    if not os.path.isfile(cmd):
        return _ERROR_FILE_NOT_FOUND, None
    target = os.path.join(os.path.dirname(sysnative), "System32", _SYNC_NOW_EXE)
    return _shell_start_via_cmd(cmd, target, _SYNC_NOW_ARGS, timeout_ms)


def _shell_start_via_cmd(cmd: str, target: str, params: str, timeout_ms: int) -> Tuple[int, Optional[int]]:
    """Run ``start /wait`` in a hidden cmd.exe; returns (win32_error, exit_code) like shell_execute_wait."""
    # cmd /c would exit with 0 after start; pass on the exit code that start /wait received.
    command = '"%s" /d /v:on /c start "" /b /wait "%s" %s & exit !errorlevel!' % (cmd, target, params)
    code, _ = _run_hidden(command, timeout_ms / 1000.0)
    if code == _ERROR_CANCELLED:
        return _ERROR_CANCELLED, None
    return 0, code


def _run_w32tm_resync(log: _Logger) -> None:
    w32tm = os.path.join(_system_dir(), "w32tm.exe")
    # /force is not accepted by every Windows build. A rejected switch fails at once,
    # so only a quick failure is retried as a plain resync; a slow one is a real sync failure.
    for args in (["/resync", "/force"], ["/resync"]):
        started = time.monotonic()
        code, output = _run_hidden([w32tm] + args, _W32TM_TIMEOUT)
        log("w32tm %s -> %s %s" % (" ".join(args), code, output.strip()[:200]))
        if code == 0 or time.monotonic() - started > 2.0:
            return


def _wait_until_within(opts: TimeSyncOptions, log: _Logger, within, seconds: float) -> Optional[float]:
    deadline = time.monotonic() + seconds
    offset = None
    while True:
        offset = _measure(opts, log)
        if within(offset) or time.monotonic() >= deadline:
            return offset
        time.sleep(1.0)


def _fmt(offset: Optional[float]) -> str:
    return "unknown" if offset is None else "%+.1f s" % offset


# Offset measurement: SNTP first, then the HTTP Date header.

def _measure(opts: TimeSyncOptions, log: _Logger) -> Optional[float]:
    return _measure_offset(opts.ntp_servers, opts.http_fallback_hosts, _MEASURE_TIMEOUT_MS, log)


def _measure_offset(ntp_servers: Sequence[str], http_hosts: Sequence[str],
                    timeout_ms: int, log: _Logger) -> Optional[float]:
    found = _measure_with_source(ntp_servers, http_hosts, timeout_ms, log)
    return None if found is None else found[0]


def _measure_with_source(ntp_servers: Sequence[str], http_hosts: Sequence[str],
                         timeout_ms: int, log: _Logger) -> Optional[Tuple[float, str]]:
    """Return (offset, name of the server that answered), or None."""
    budget = max(0.1, timeout_ms / 1000.0)

    deadline = time.monotonic() + budget
    for server in ntp_servers:
        remaining = deadline - time.monotonic()
        if remaining <= 0.05:
            break
        offset = _try(lambda: _query_ntp(server, min(_NTP_TIMEOUT, remaining)))
        if offset is not None:
            log("NTP %s: %+.3f s" % (server, offset))
            return offset, server
        log("NTP %s: no answer" % server)

    deadline = time.monotonic() + budget
    for host in http_hosts:
        remaining = deadline - time.monotonic()
        if remaining <= 0.05:
            break
        offset = _try(lambda: _query_http_date(host, min(_HTTP_TIMEOUT, remaining)))
        if offset is not None:
            log("HTTP %s: %+.3f s" % (host, offset))
            return offset, host
        log("HTTP %s: no answer" % host)
    return None


def _try(func):
    """One bad server or a malformed reply must never stop the other sources from being tried."""
    try:
        return func()
    except Exception:
        return None


def _resolve(host: str, port: int, socktype: int, timeout: float) -> List[tuple]:
    """getaddrinfo has no timeout, so run it on a daemon thread and stop waiting after ``timeout``."""
    found: List[list] = []

    def work() -> None:
        try:
            found.append(socket.getaddrinfo(host, port, 0, socktype))
        except Exception:  # bad names raise UnicodeError or TypeError, not only OSError
            pass

    worker = threading.Thread(target=work, name="timesync-dns", daemon=True)
    worker.start()
    worker.join(timeout)
    if not found:
        return []
    # Prefer IPv4: broken IPv6 setups are more common than broken IPv4.
    return sorted(found[0], key=lambda info: info[0] != socket.AF_INET)


def _query_ntp(server: str, timeout: float) -> Optional[float]:
    deadline = time.monotonic() + timeout
    addresses = _resolve(server, _NTP_PORT, socket.SOCK_DGRAM, timeout)
    remaining = deadline - time.monotonic()
    if not addresses or remaining <= 0:
        return None

    family, _, _, _, sockaddr = addresses[0]
    nonce = os.urandom(8)  # sent as our transmit timestamp, echoed back as originate
    request = b"\x23" + bytes(39) + nonce
    with socket.socket(family, socket.SOCK_DGRAM) as sock:
        sock.settimeout(remaining)
        sock.connect(sockaddr)
        t1 = time.time()
        start = time.perf_counter()
        sock.send(request)
        data = sock.recv(1024)
        t4 = t1 + (time.perf_counter() - start)

    stamps = _parse_ntp_response(data, nonce)
    if stamps is None:
        return None
    return _ntp_offset(t1, stamps[0], stamps[1], t4)


def _parse_ntp_response(data: bytes, expected_origin: Optional[bytes] = None) -> Optional[Tuple[float, float]]:
    """Validate an SNTP reply and return (receive, transmit) as Unix time, or None."""
    if len(data) < 48:
        return None
    leap = data[0] >> 6
    mode = data[0] & 0x07
    stratum = data[1]
    if mode not in (4, 5) or not 1 <= stratum <= 15:
        return None
    if leap == 3:  # alarm: the server's own clock is not synchronized
        return None
    if expected_origin is not None and data[24:32] != expected_origin:
        return None
    receive, transmit = struct.unpack("!QQ", data[32:48])
    if receive == 0 or transmit == 0:
        return None
    return _ntp_to_unix(receive), _ntp_to_unix(transmit)


def _ntp_to_unix(timestamp: int) -> float:
    """Convert a 64-bit NTP 32.32 timestamp to Unix seconds, handling era 1 (from 2036)."""
    seconds = timestamp >> 32
    fraction = timestamp & 0xFFFFFFFF
    if seconds < 0x80000000:
        seconds += _NTP_ERA_SECONDS
    return seconds - _NTP_TO_UNIX + fraction / float(_NTP_ERA_SECONDS)


def _ntp_offset(t1: float, t2: float, t3: float, t4: float) -> float:
    return ((t2 - t1) + (t3 - t4)) / 2.0


def _query_http_date(host: str, timeout: float) -> Optional[float]:
    """Plain HTTP on port 80: TLS itself can fail when the clock is badly wrong."""
    deadline = time.monotonic() + timeout
    addresses = _resolve(host, 80, socket.SOCK_STREAM, timeout)
    if not addresses or deadline - time.monotonic() <= 0:
        return None

    family, _, _, _, sockaddr = addresses[0]
    request = ("HEAD / HTTP/1.1\r\nHost: %s\r\nUser-Agent: PwfTimeSync\r\n"
               "Accept: */*\r\nConnection: close\r\n\r\n" % host).encode("ascii")
    head = b""
    with socket.socket(family, socket.SOCK_STREAM) as sock:
        sock.settimeout(max(0.01, deadline - time.monotonic()))
        sock.connect(sockaddr)
        t1 = time.time()
        start = time.perf_counter()
        sock.sendall(request)
        t4 = None
        while b"\r\n\r\n" not in head and len(head) < 16384:
            sock.settimeout(max(0.01, deadline - time.monotonic()))
            chunk = sock.recv(4096)
            if t4 is None:
                t4 = t1 + (time.perf_counter() - start)
            if not chunk:
                break
            head += chunk

    server_time = _parse_http_date(head)
    if server_time is None or t4 is None:
        return None
    # Date is truncated to whole seconds, so on average it is half a second behind.
    return server_time - (t1 + t4) / 2.0 + 0.5


def _parse_http_date(response_head: bytes) -> Optional[float]:
    """Return the Date header of a raw HTTP response head as Unix time, or None."""
    lines = response_head.decode("latin-1").split("\r\n")
    if not lines or not lines[0].startswith("HTTP/"):
        return None
    for line in lines[1:]:
        if not line:
            break
        name, sep, value = line.partition(":")
        if sep and name.strip().lower() == "date":
            return _parse_rfc1123(value.strip())
    return None


def _parse_rfc1123(value: str) -> Optional[float]:
    try:
        parsed = email.utils.parsedate_tz(value)
        if parsed is None:
            return None
        if parsed[9] is None:  # no zone given: HTTP dates are always GMT
            result = float(calendar.timegm(parsed[:9]))
        else:
            result = float(email.utils.mktime_tz(parsed))
    except (ValueError, OverflowError, IndexError, TypeError):
        return None  # malformed header, for example from a captive portal
    return result if result >= _MIN_SANE_TIME else None


# Windows helpers

_ERROR_FILE_NOT_FOUND = 2
_ERROR_CANCELLED = 1223


def _system_dir() -> str:
    windir = os.environ.get("SystemRoot") or os.environ.get("windir") or r"C:\Windows"
    if _win32().is_wow64():
        # A 32-bit process sees System32 redirected to SysWOW64.
        sysnative = os.path.join(windir, "Sysnative")
        if os.path.isdir(sysnative):
            return sysnative
    return os.path.join(windir, "System32")


def _run_hidden(args: Union[str, List[str]], timeout: float) -> Tuple[Optional[int], str]:
    startupinfo = subprocess.STARTUPINFO()
    startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startupinfo.wShowWindow = 0  # SW_HIDE
    try:
        completed = subprocess.run(
            args,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            startupinfo=startupinfo,
            creationflags=0x08000000,  # CREATE_NO_WINDOW
            timeout=timeout,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        return None, str(exc)
    return completed.returncode, completed.stdout.decode("oem", "replace")


def _hard_exit(code: int) -> None:
    for stream in (sys.stdout, sys.stderr):
        try:
            if stream is not None:
                stream.flush()
        except Exception:
            pass
    os._exit(code)


_win32_instance = None


def _win32() -> "_Win32":
    global _win32_instance
    if _win32_instance is None:
        _win32_instance = _Win32()
    return _win32_instance


class _Win32:
    """Thin ctypes layer. Every function gets explicit argtypes/restype so handles stay 64-bit safe."""

    TOKEN_QUERY = 0x0008
    TOKEN_ADJUST_PRIVILEGES = 0x0020
    TOKEN_ELEVATION_CLASS = 20
    SE_PRIVILEGE_ENABLED = 0x00000002
    ERROR_NOT_ALL_ASSIGNED = 1300

    SC_MANAGER_CONNECT = 0x0001
    SERVICE_START = 0x0010
    SERVICE_QUERY_STATUS = 0x0004
    SERVICE_RUNNING = 4
    ERROR_SERVICE_ALREADY_RUNNING = 1056

    SEE_MASK_NOCLOSEPROCESS = 0x00000040
    SEE_MASK_NOASYNC = 0x00000100
    SEE_MASK_FLAG_NO_UI = 0x00000400
    SW_HIDE = 0
    WAIT_OBJECT_0 = 0

    def __init__(self) -> None:
        import ctypes
        from ctypes import wintypes

        self._ctypes = ctypes
        self._wt = wintypes
        self._define_structs()

        self._k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self._adv = ctypes.WinDLL("advapi32", use_last_error=True)
        self._sh32 = ctypes.WinDLL("shell32", use_last_error=True)
        self._declare()

    def _define_structs(self) -> None:
        ctypes, wt = self._ctypes, self._wt

        class LUID(ctypes.Structure):
            _fields_ = [("LowPart", wt.DWORD), ("HighPart", wt.LONG)]

        class LUID_AND_ATTRIBUTES(ctypes.Structure):
            _fields_ = [("Luid", LUID), ("Attributes", wt.DWORD)]

        class TOKEN_PRIVILEGES(ctypes.Structure):
            _fields_ = [("PrivilegeCount", wt.DWORD), ("Privileges", LUID_AND_ATTRIBUTES * 1)]

        class SYSTEMTIME(ctypes.Structure):
            _fields_ = [(name, wt.WORD) for name in (
                "wYear", "wMonth", "wDayOfWeek", "wDay",
                "wHour", "wMinute", "wSecond", "wMilliseconds")]

        class SERVICE_STATUS(ctypes.Structure):
            _fields_ = [(name, wt.DWORD) for name in (
                "dwServiceType", "dwCurrentState", "dwControlsAccepted", "dwWin32ExitCode",
                "dwServiceSpecificExitCode", "dwCheckPoint", "dwWaitHint")]

        class SHELLEXECUTEINFOW(ctypes.Structure):
            _fields_ = [
                ("cbSize", wt.DWORD),
                ("fMask", wt.ULONG),
                ("hwnd", wt.HWND),
                ("lpVerb", wt.LPCWSTR),
                ("lpFile", wt.LPCWSTR),
                ("lpParameters", wt.LPCWSTR),
                ("lpDirectory", wt.LPCWSTR),
                ("nShow", ctypes.c_int),
                ("hInstApp", wt.HINSTANCE),
                ("lpIDList", wt.LPVOID),
                ("lpClass", wt.LPCWSTR),
                ("hkeyClass", wt.HKEY),
                ("dwHotKey", wt.DWORD),
                ("hIconOrMonitor", wt.HANDLE),
                ("hProcess", wt.HANDLE),
            ]

        self.LUID = LUID
        self.TOKEN_PRIVILEGES = TOKEN_PRIVILEGES
        self.SYSTEMTIME = SYSTEMTIME
        self.SERVICE_STATUS = SERVICE_STATUS
        self.SHELLEXECUTEINFOW = SHELLEXECUTEINFOW

    def _declare(self) -> None:
        ctypes, wt = self._ctypes, self._wt
        P = ctypes.POINTER

        def fn(dll, name, restype, *argtypes):
            func = getattr(dll, name)
            func.restype = restype
            func.argtypes = list(argtypes)
            return func

        k32, adv, sh32 = self._k32, self._adv, self._sh32
        self.GetCurrentProcess = fn(k32, "GetCurrentProcess", wt.HANDLE)
        self.CloseHandle = fn(k32, "CloseHandle", wt.BOOL, wt.HANDLE)
        self.IsWow64Process = fn(k32, "IsWow64Process", wt.BOOL, wt.HANDLE, P(wt.BOOL))
        self.WaitForSingleObject = fn(k32, "WaitForSingleObject", wt.DWORD, wt.HANDLE, wt.DWORD)
        self.GetExitCodeProcess = fn(k32, "GetExitCodeProcess", wt.BOOL, wt.HANDLE, P(wt.DWORD))
        self.SetSystemTime = fn(k32, "SetSystemTime", wt.BOOL, P(self.SYSTEMTIME))

        self.OpenProcessToken = fn(adv, "OpenProcessToken", wt.BOOL, wt.HANDLE, wt.DWORD, P(wt.HANDLE))
        self.GetTokenInformation = fn(adv, "GetTokenInformation", wt.BOOL,
                                      wt.HANDLE, ctypes.c_int, wt.LPVOID, wt.DWORD, P(wt.DWORD))
        self.LookupPrivilegeValueW = fn(adv, "LookupPrivilegeValueW", wt.BOOL,
                                        wt.LPCWSTR, wt.LPCWSTR, P(self.LUID))
        self.AdjustTokenPrivileges = fn(adv, "AdjustTokenPrivileges", wt.BOOL,
                                        wt.HANDLE, wt.BOOL, P(self.TOKEN_PRIVILEGES),
                                        wt.DWORD, wt.LPVOID, P(wt.DWORD))
        self.OpenSCManagerW = fn(adv, "OpenSCManagerW", wt.HANDLE, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD)
        self.OpenServiceW = fn(adv, "OpenServiceW", wt.HANDLE, wt.HANDLE, wt.LPCWSTR, wt.DWORD)
        self.StartServiceW = fn(adv, "StartServiceW", wt.BOOL, wt.HANDLE, wt.DWORD, wt.LPVOID)
        self.QueryServiceStatus = fn(adv, "QueryServiceStatus", wt.BOOL, wt.HANDLE, P(self.SERVICE_STATUS))
        self.CloseServiceHandle = fn(adv, "CloseServiceHandle", wt.BOOL, wt.HANDLE)

        self.ShellExecuteExW = fn(sh32, "ShellExecuteExW", wt.BOOL, P(self.SHELLEXECUTEINFOW))

    def _last_error(self) -> int:
        return self._ctypes.get_last_error()

    def is_wow64(self) -> bool:
        flag = self._wt.BOOL(False)
        if not self.IsWow64Process(self.GetCurrentProcess(), self._ctypes.byref(flag)):
            return False
        return bool(flag.value)

    def is_elevated(self) -> bool:
        ctypes, wt = self._ctypes, self._wt
        token = wt.HANDLE()
        if not self.OpenProcessToken(self.GetCurrentProcess(), self.TOKEN_QUERY, ctypes.byref(token)):
            return False
        try:
            elevation = wt.DWORD(0)
            size = wt.DWORD(0)
            ok = self.GetTokenInformation(token, self.TOKEN_ELEVATION_CLASS, ctypes.byref(elevation),
                                          ctypes.sizeof(elevation), ctypes.byref(size))
            return bool(ok) and elevation.value != 0
        finally:
            self.CloseHandle(token)

    def shell_execute_wait(self, file: str, params: str, verb: Optional[str],
                           timeout_ms: int, directory: Optional[str] = None) -> Tuple[int, Optional[int]]:
        """Start ``file`` hidden through ShellExecuteEx and wait for it.

        Returns (win32_error, exit_code); exit_code is None if it did not finish in time.
        Shell error dialogs are suppressed; a UAC prompt can still appear when one is needed.
        """
        ctypes, wt = self._ctypes, self._wt
        info = self.SHELLEXECUTEINFOW()
        info.cbSize = ctypes.sizeof(info)
        info.fMask = self.SEE_MASK_NOCLOSEPROCESS | self.SEE_MASK_NOASYNC | self.SEE_MASK_FLAG_NO_UI
        info.lpVerb = verb
        info.lpFile = file
        info.lpParameters = params
        info.lpDirectory = directory or os.path.dirname(file) or None
        info.nShow = self.SW_HIDE
        if not self.ShellExecuteExW(ctypes.byref(info)):
            return self._last_error() or 1, None
        if not info.hProcess:
            return 0, None
        try:
            if self.WaitForSingleObject(info.hProcess, max(0, int(timeout_ms))) != self.WAIT_OBJECT_0:
                return 0, None
            code = wt.DWORD(0)
            if not self.GetExitCodeProcess(info.hProcess, ctypes.byref(code)):
                return 0, None
            return 0, int(code.value)
        finally:
            self.CloseHandle(info.hProcess)

    def start_w32time(self, log: _Logger) -> bool:
        ctypes = self._ctypes
        manager = self.OpenSCManagerW(None, None, self.SC_MANAGER_CONNECT)
        if not manager:
            log("OpenSCManager failed (error %d)." % self._last_error())
            return False
        try:
            service = self.OpenServiceW(manager, "W32Time", self.SERVICE_START | self.SERVICE_QUERY_STATUS)
            if not service:
                log("OpenService(W32Time) failed (error %d)." % self._last_error())
                return False
            try:
                if not self.StartServiceW(service, 0, None):
                    error = self._last_error()
                    if error != self.ERROR_SERVICE_ALREADY_RUNNING:
                        log("StartService(W32Time) failed (error %d)." % error)
                        return False
                status = self.SERVICE_STATUS()
                deadline = time.monotonic() + 5.0
                while time.monotonic() < deadline:
                    if self.QueryServiceStatus(service, ctypes.byref(status)) and \
                            status.dwCurrentState == self.SERVICE_RUNNING:
                        return True
                    time.sleep(0.2)
                log("W32Time did not reach the running state in time.")
                return False
            finally:
                self.CloseServiceHandle(service)
        finally:
            self.CloseServiceHandle(manager)

    def set_system_time_utc(self, unix_time: float, log: _Logger) -> bool:
        if not self._enable_privilege("SeSystemtimePrivilege", log):
            return False
        whole = int(unix_time)
        parts = time.gmtime(whole)
        st = self.SYSTEMTIME(
            wYear=parts.tm_year, wMonth=parts.tm_mon, wDayOfWeek=(parts.tm_wday + 1) % 7,
            wDay=parts.tm_mday, wHour=parts.tm_hour, wMinute=parts.tm_min, wSecond=parts.tm_sec,
            wMilliseconds=min(999, int((unix_time - whole) * 1000)))
        if not self.SetSystemTime(self._ctypes.byref(st)):
            log("SetSystemTime failed (error %d)." % self._last_error())
            return False
        log("System time set from the network reference.")
        return True

    def _enable_privilege(self, name: str, log: _Logger) -> bool:
        ctypes, wt = self._ctypes, self._wt
        token = wt.HANDLE()
        access = self.TOKEN_ADJUST_PRIVILEGES | self.TOKEN_QUERY
        if not self.OpenProcessToken(self.GetCurrentProcess(), access, ctypes.byref(token)):
            log("OpenProcessToken failed (error %d)." % self._last_error())
            return False
        try:
            privileges = self.TOKEN_PRIVILEGES()
            privileges.PrivilegeCount = 1
            if not self.LookupPrivilegeValueW(None, name, ctypes.byref(privileges.Privileges[0].Luid)):
                log("LookupPrivilegeValue failed (error %d)." % self._last_error())
                return False
            privileges.Privileges[0].Attributes = self.SE_PRIVILEGE_ENABLED
            ok = self.AdjustTokenPrivileges(token, False, ctypes.byref(privileges), 0, None, None)
            error = self._last_error()
            if not ok or error == self.ERROR_NOT_ALL_ASSIGNED:
                log("Could not enable %s (error %d)." % (name, error))
                return False
            return True
        finally:
            self.CloseHandle(token)
