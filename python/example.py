"""Example: fix the clock before the pwfauth license login.

Run:            python example.py
Force a sync:   python example.py --force   (runs Windows "Sync now" even if the clock is fine)
"""

import sys

import auto_timesync
from auto_timesync import TimeSyncOptions

# from pwfauth import PwfClient


def main() -> int:
    force = "--force" in sys.argv[1:]
    options = TimeSyncOptions(
        force_sync=force,
        # No elevated relaunch in the forced test. Admin accounts see no prompt at all;
        # a standard-user account still gets the Windows prompt that "Sync now" shows.
        allow_elevation_prompt=not force,
        log=print,
    )

    result = auto_timesync.ensure_clock_is_correct(options)
    print("Success:       %s" % result.success)
    print("Method:        %s" % result.method)
    print("Offset before: %s" % _seconds(result.offset_before_seconds))
    print("Offset after:  %s" % _seconds(result.offset_after_seconds))
    print("Message:       %s" % result.message)

    # The clock is now correct (or no reference was reachable), so log in as usual:
    # client = PwfClient(secret)
    # client.login(key)
    return 0


def _seconds(value):
    return "n/a" if value is None else "%+.3f s" % value


if __name__ == "__main__":
    # Must run first: when relaunched elevated to fix the clock, this exits the process.
    auto_timesync.handle_elevated_command()
    sys.exit(main())
