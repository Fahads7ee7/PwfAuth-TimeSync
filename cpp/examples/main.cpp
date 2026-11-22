// Example: make sure the Windows clock is correct before the pwfauth license login.
//
// Build: cmake -S cpp -B build && cmake --build build --config Release
// Run:   timesync_example.exe            (normal check)
//        timesync_example.exe --force    (run the Windows sync even if the clock is fine;
//                                         never relaunch elevated)

#include <cwchar>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

// Include AutoTimeSync.h before PwfClient.h / <windows.h>.
#include "../AutoTimeSync.h"

namespace
{
    std::wstring FormatOffset(const std::optional<double>& seconds)
    {
        if (!seconds)
            return L"n/a";
        std::wostringstream text;
        text << std::showpos << std::fixed << std::setprecision(3) << *seconds << L" s";
        return text.str();
    }

    bool HasArgument(int argc, wchar_t* argv[], const wchar_t* name)
    {
        for (int i = 1; i < argc; ++i)
        {
            if (std::wcscmp(argv[i], name) == 0)
                return true;
        }
        return false;
    }
}

int wmain(int argc, wchar_t* argv[])
{
    // Must be the first line: when the app is relaunched elevated to fix the clock,
    // this does the work and exits the process.
    if (PwfTimeSync::HandleElevatedCommand())
        return 0;

    PwfTimeSync::TimeSyncOptions options;
    options.Log = [](const std::wstring& line) { std::wcout << line << std::endl; };

    // --force: run the Windows "Sync now" even if the clock is fine, but never relaunch this app
    // elevated. Standard users may still see Windows' own prompt for "Sync now".
    if (HasArgument(argc, argv, L"--force"))
    {
        options.ForceSync = true;
        options.AllowElevationPrompt = false;
    }

    // Customers whose clock is already correct pass straight through, usually in well under a
    // second (about 1.5 s more on networks that block NTP, where the HTTP fallback is used).
    const PwfTimeSync::TimeSyncResult result = PwfTimeSync::EnsureClockIsCorrect(options);

    std::wcout << L"Success: " << (result.Success ? L"true" : L"false") << L'\n'
               << L"Method:  " << PwfTimeSync::ToString(result.Method) << L'\n'
               << L"Before:  " << FormatOffset(result.OffsetBeforeSeconds) << L'\n'
               << L"After:   " << FormatOffset(result.OffsetAfterSeconds) << L'\n'
               << L"Message: " << result.Message << std::endl;

    // Now log in with the pwfauth C++ SDK (PwfClient.h) as usual:
    //
    //   PwfClient client(appSecret);
    //   auto login = client.Login(key);
    //
    // The clock has already been checked, so the key is not rejected for clock skew.

    return result.Success ? 0 : 1;
}
