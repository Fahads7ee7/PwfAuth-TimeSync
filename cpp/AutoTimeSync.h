// AutoTimeSync.h - fixes a wrong Windows clock before a pwfauth license login.
//
// Single header, C++17, Windows only (MSVC and MinGW). Include it before <windows.h>, or
// define WIN32_LEAN_AND_MEAN before your own <windows.h>, so that winsock2.h does not clash
// with the older winsock.h. It does not define WIN32_LEAN_AND_MEAN or NOMINMAX for you.
//
// Usage:
//     int main()
//     {
//         if (PwfTimeSync::HandleElevatedCommand()) return 0;   // must be the first line
//         PwfTimeSync::EnsureClockIsCorrect();
//         // ... PwfClient client(appSecret); client.Login(key);
//     }
//
// MinGW: link with -lws2_32 -ladvapi32 -lshell32 -luser32.
//
// Author: Fahads7ee7. MIT License.

#ifndef PWF_AUTO_TIME_SYNC_H
#define PWF_AUTO_TIME_SYNC_H

#ifndef _WIN32
#error "AutoTimeSync.h supports Windows only."
#endif

// The standard headers come first so the min/max macros of <windows.h> cannot reach them.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#if defined(__MINGW32__)
#include <_mingw.h>
// GetTickCount64 and TOKEN_ELEVATION need Windows Vista. Only raise the target, never lower it.
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winsvc.h>
#include <shellapi.h>

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#endif

namespace PwfTimeSync
{
    /// How the clock was found or fixed.
    enum class TimeSyncMethod
    {
        AlreadyCorrect,
        WindowsSyncNow,
        W32tmResync,
        SetSystemTime,
        ElevatedHelper,
        NoReference,
        Failed,
        Skipped
    };

    struct TimeSyncOptions
    {
        /// pwfauth rejects requests beyond 300 s; keep a safe margin.
        double MaxAllowedOffsetSeconds = 30;

        /// Relaunch the app elevated (UAC prompt) when nothing else could fix the clock.
        bool AllowElevationPrompt = true;

        /// Run the Windows sync even if the clock already looks fine (useful for testing).
        bool ForceSync = false;

        /// How long to wait for the Windows "Sync now" helper to finish.
        int SyncNowTimeoutMs = 15000;

        std::vector<std::wstring> NtpServers{
            L"time.windows.com", L"time.google.com", L"time.cloudflare.com", L"pool.ntp.org"};

        /// Plain HTTP (port 80) hosts whose Date header is used when UDP 123 is blocked.
        std::vector<std::wstring> HttpFallbackHosts{L"www.msftconnecttest.com", L"www.google.com"};

        /// Optional progress log. Nothing is printed by default.
        std::function<void(const std::wstring&)> Log;

        /// Window that owns the Windows permission prompt, so it opens in front instead of
        /// flashing in the taskbar. GUI apps should pass their main window; console apps
        /// use the console window automatically.
        HWND Owner = nullptr;
    };

    using Options = TimeSyncOptions;

    struct TimeSyncResult
    {
        /// True when the clock is within the threshold, or no reference was reachable
        /// (so the app should still try to log in).
        bool Success = false;
        TimeSyncMethod Method = TimeSyncMethod::Failed;

        /// Reference UTC minus local UTC, in seconds.
        std::optional<double> OffsetBeforeSeconds;
        std::optional<double> OffsetAfterSeconds;

        std::wstring Message;
    };

    /// The argument used when the app relaunches itself elevated.
    inline constexpr const wchar_t* ElevatedFlag = L"--pwf-timesync-elevated";

    inline const wchar_t* ToString(TimeSyncMethod method) noexcept
    {
        switch (method)
        {
        case TimeSyncMethod::AlreadyCorrect: return L"AlreadyCorrect";
        case TimeSyncMethod::WindowsSyncNow: return L"WindowsSyncNow";
        case TimeSyncMethod::W32tmResync: return L"W32tmResync";
        case TimeSyncMethod::SetSystemTime: return L"SetSystemTime";
        case TimeSyncMethod::ElevatedHelper: return L"ElevatedHelper";
        case TimeSyncMethod::NoReference: return L"NoReference";
        case TimeSyncMethod::Failed: return L"Failed";
        case TimeSyncMethod::Skipped: return L"Skipped";
        }
        return L"Unknown";
    }

    namespace detail
    {
        inline constexpr DWORD kNtpTimeoutMs = 1500;
        inline constexpr DWORD kHttpTimeoutMs = 3000;
        inline constexpr DWORD kSyncNowSettleMs = 8000;
        inline constexpr DWORD kW32tmTimeoutMs = 10000;
        inline constexpr DWORD kW32tmQuickFailureMs = 2000;
        inline constexpr DWORD kElevatedHelperTimeoutMs = 60000;
        inline constexpr DWORD kElevatedFixBudgetMs = 40000; // keeps the helper inside the parent's wait
        inline constexpr long long kNtpToUnixSeconds = 2208988800LL;
        inline constexpr ULONGLONG kUnixEpochFileTime = 116444736000000000ULL;

        inline constexpr const wchar_t* kDeclinedMessage =
            L"Administrator permission was declined; the clock was not changed.";

        inline void Log(const TimeSyncOptions& options, const std::wstring& message) noexcept
        {
            if (!options.Log)
                return;
            try
            {
                options.Log(message);
            }
            catch (...)
            {
            }
        }

        inline std::wstring Widen(const char* text)
        {
            std::wstring result;
            for (; text && *text; ++text)
                result.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*text)));
            return result;
        }

        inline std::string NarrowAscii(const std::wstring& text)
        {
            std::string result;
            result.reserve(text.size());
            for (wchar_t ch : text)
                result.push_back(ch < 128 ? static_cast<char>(ch) : '?');
            return result;
        }

        inline std::wstring FormatSeconds(double seconds)
        {
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%+.3f s", seconds);
            return Widen(buffer);
        }

        inline std::wstring FormatOffset(const std::optional<double>& offset)
        {
            return offset ? FormatSeconds(*offset) : std::wstring(L"unknown");
        }

        inline bool IsWithin(const std::optional<double>& offset, double limit) noexcept
        {
            return offset.has_value() && std::fabs(*offset) <= limit;
        }

        inline DWORD RemainingMs(ULONGLONG deadline) noexcept
        {
            const ULONGLONG now = GetTickCount64();
            return now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
        }

        inline DWORD ClampMs(DWORD value, DWORD low, DWORD high) noexcept
        {
            return value < low ? low : (value > high ? high : value);
        }

        /// Current UTC as FILETIME ticks (100 ns since 1601), using the precise clock when available.
        inline ULONGLONG FileTimeNow() noexcept
        {
            typedef VOID(WINAPI * PreciseTimeFn)(LPFILETIME);
            static const PreciseTimeFn precise = []() -> PreciseTimeFn {
                HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
                if (!kernel)
                    return nullptr;
                FARPROC proc = GetProcAddress(kernel, "GetSystemTimePreciseAsFileTime");
                return proc ? reinterpret_cast<PreciseTimeFn>(reinterpret_cast<void (*)()>(proc)) : nullptr;
            }();

            FILETIME ft{};
            if (precise)
                precise(&ft);
            else
                GetSystemTimeAsFileTime(&ft);

            ULARGE_INTEGER value;
            value.LowPart = ft.dwLowDateTime;
            value.HighPart = ft.dwHighDateTime;
            return value.QuadPart;
        }

        /// Current UTC as Unix seconds with sub-second precision.
        inline double UnixNow() noexcept
        {
            const long long ticks = static_cast<long long>(FileTimeNow() - kUnixEpochFileTime);
            return static_cast<double>(ticks) / 1e7;
        }

        /// Days since 1970-01-01 for a proleptic Gregorian date.
        inline long long DaysFromCivil(int year, int month, int day) noexcept
        {
            if (month <= 2)
                --year;
            const int era = (year >= 0 ? year : year - 399) / 400;
            const int yearOfEra = year - era * 400;
            const int shiftedMonth = (month + 9) % 12; // March = 0
            const int dayOfYear = (153 * shiftedMonth + 2) / 5 + day - 1;
            const int dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
            return static_cast<long long>(era) * 146097LL + dayOfEra - 719468LL;
        }

        inline std::uint32_t ReadBigEndian32(const unsigned char* p) noexcept
        {
            return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
                   (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
        }

        /// NTP 32.32 timestamp to Unix seconds. Seconds below 2^31 belong to era 1 (from 2036).
        inline double NtpTimestampToUnix(const unsigned char* p) noexcept
        {
            std::uint64_t seconds = ReadBigEndian32(p);
            const std::uint32_t fraction = ReadBigEndian32(p + 4);
            if (seconds < 0x80000000ULL)
                seconds += 0x100000000ULL;
            return static_cast<double>(static_cast<long long>(seconds) - kNtpToUnixSeconds) +
                   static_cast<double>(fraction) / 4294967296.0;
        }

        /// Validates an SNTP reply and returns the clock offset. t1/t4 are local send/receive times.
        inline std::optional<double> ParseSntpResponse(const unsigned char* data, std::size_t length,
                                                       double t1, double t4) noexcept
        {
            if (!data || length < 48)
                return std::nullopt;

            const int mode = data[0] & 0x07;
            const int stratum = data[1];
            if ((mode != 4 && mode != 5) || stratum < 1 || stratum > 15)
                return std::nullopt;

            const bool receiveIsZero = ReadBigEndian32(data + 32) == 0 && ReadBigEndian32(data + 36) == 0;
            const bool transmitIsZero = ReadBigEndian32(data + 40) == 0 && ReadBigEndian32(data + 44) == 0;
            if (receiveIsZero || transmitIsZero)
                return std::nullopt;

            const double t2 = NtpTimestampToUnix(data + 32);
            const double t3 = NtpTimestampToUnix(data + 40);
            return ((t2 - t1) + (t3 - t4)) / 2.0;
        }

        inline char ToLowerAscii(char ch) noexcept
        {
            return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
        }

        inline bool StartsWithIgnoreCase(const std::string& text, std::size_t offset, const char* prefix) noexcept
        {
            for (std::size_t i = 0; prefix[i] != '\0'; ++i)
            {
                if (offset + i >= text.size() || ToLowerAscii(text[offset + i]) != ToLowerAscii(prefix[i]))
                    return false;
            }
            return true;
        }

        inline int MonthFromName(const std::string& text, std::size_t offset) noexcept
        {
            static const char* const names[] = {"jan", "feb", "mar", "apr", "may", "jun",
                                                "jul", "aug", "sep", "oct", "nov", "dec"};
            for (int i = 0; i < 12; ++i)
            {
                if (StartsWithIgnoreCase(text, offset, names[i]))
                    return i + 1;
            }
            return 0;
        }

        /// Parses an RFC 1123 date such as "Sun, 06 Nov 1994 08:49:37 GMT" into Unix seconds.
        inline std::optional<long long> ParseRfc1123Date(const std::string& text) noexcept
        {
            std::size_t i = 0;
            const std::size_t n = text.size();

            auto skipSpaces = [&]() {
                while (i < n && (text[i] == ' ' || text[i] == '\t'))
                    ++i;
            };
            auto readNumber = [&](int maxDigits, int& value) {
                int digits = 0;
                value = 0;
                while (i < n && digits < maxDigits && text[i] >= '0' && text[i] <= '9')
                {
                    value = value * 10 + (text[i] - '0');
                    ++i;
                    ++digits;
                }
                return digits > 0;
            };
            auto expect = [&](char ch) {
                if (i >= n || text[i] != ch)
                    return false;
                ++i;
                return true;
            };

            skipSpaces();
            const std::size_t comma = text.find(',', i);
            if (comma != std::string::npos)
                i = comma + 1; // skip the weekday

            int day = 0, year = 0, hour = 0, minute = 0, second = 0;
            skipSpaces();
            if (!readNumber(2, day))
                return std::nullopt;
            skipSpaces();
            const int month = MonthFromName(text, i);
            if (month == 0)
                return std::nullopt;
            i += 3;
            skipSpaces();
            if (!readNumber(4, year))
                return std::nullopt;
            skipSpaces();
            if (!readNumber(2, hour) || !expect(':') || !readNumber(2, minute) || !expect(':') ||
                !readNumber(2, second))
                return std::nullopt;
            skipSpaces();
            if (!StartsWithIgnoreCase(text, i, "GMT") && !StartsWithIgnoreCase(text, i, "UTC") &&
                !StartsWithIgnoreCase(text, i, "+0000"))
                return std::nullopt;

            if (year < 1970 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60)
                return std::nullopt;

            return DaysFromCivil(year, month, day) * 86400LL + hour * 3600LL + minute * 60LL + second;
        }

        /// Finds the Date header in a raw HTTP response head and returns it as Unix seconds.
        inline std::optional<long long> ParseHttpDateHeader(const std::string& response) noexcept
        {
            if (!StartsWithIgnoreCase(response, 0, "HTTP/"))
                return std::nullopt;

            std::size_t pos = 0;
            while (pos < response.size())
            {
                std::size_t end = response.find("\r\n", pos);
                if (end == std::string::npos)
                    end = response.size();
                if (end == pos)
                    break; // blank line: end of headers

                if (StartsWithIgnoreCase(response, pos, "date:"))
                {
                    try
                    {
                        return ParseRfc1123Date(response.substr(pos + 5, end - pos - 5));
                    }
                    catch (...)
                    {
                        return std::nullopt;
                    }
                }
                pos = end + 2;
            }
            return std::nullopt;
        }

        /// WSAStartup/WSACleanup pair. Winsock reference-counts these calls.
        class WinsockScope
        {
        public:
            WinsockScope() noexcept
            {
                WSADATA data;
                ok_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
            }
            ~WinsockScope()
            {
                if (ok_)
                    WSACleanup();
            }
            WinsockScope(const WinsockScope&) = delete;
            WinsockScope& operator=(const WinsockScope&) = delete;

            bool Ok() const noexcept { return ok_; }

        private:
            bool ok_ = false;
        };

        class SocketHandle
        {
        public:
            explicit SocketHandle(SOCKET handle) noexcept : socket_(handle) {}
            ~SocketHandle()
            {
                if (socket_ != INVALID_SOCKET)
                    closesocket(socket_);
            }
            SocketHandle(const SocketHandle&) = delete;
            SocketHandle& operator=(const SocketHandle&) = delete;

            SOCKET Get() const noexcept { return socket_; }
            bool Valid() const noexcept { return socket_ != INVALID_SOCKET; }

        private:
            SOCKET socket_;
        };

        struct ResolvedAddress
        {
            int family = 0;
            int socketType = 0;
            int protocol = 0;
            sockaddr_storage address{};
            int addressLength = 0;
        };

        struct ResolveJob
        {
            std::wstring host;
            std::wstring port;
            int socketType = 0;
            int protocol = 0;
            std::vector<ResolvedAddress> addresses;
            HANDLE done = nullptr;
            LONG references = 2; // caller + worker thread
        };

        /// Jobs shared by a caller and a worker thread; the last one to let go frees it.
        template <typename Job>
        inline void ReleaseJob(Job* job) noexcept
        {
            if (InterlockedDecrement(&job->references) == 0)
            {
                CloseHandle(job->done);
                delete job;
            }
        }

        inline DWORD WINAPI ResolveThreadProc(LPVOID parameter)
        {
            ResolveJob* job = static_cast<ResolveJob*>(parameter);
            try
            {
                WinsockScope winsock;
                ADDRINFOW hints{};
                hints.ai_family = AF_UNSPEC;
                hints.ai_socktype = job->socketType;
                hints.ai_protocol = job->protocol;

                PADDRINFOW list = nullptr;
                if (winsock.Ok() && GetAddrInfoW(job->host.c_str(), job->port.c_str(), &hints, &list) == 0)
                {
                    for (PADDRINFOW item = list; item; item = item->ai_next)
                    {
                        if ((item->ai_family != AF_INET && item->ai_family != AF_INET6) || !item->ai_addr ||
                            item->ai_addrlen > sizeof(sockaddr_storage))
                            continue;

                        ResolvedAddress address;
                        address.family = item->ai_family;
                        address.socketType = item->ai_socktype;
                        address.protocol = item->ai_protocol;
                        address.addressLength = static_cast<int>(item->ai_addrlen);
                        std::memcpy(&address.address, item->ai_addr, item->ai_addrlen);
                        job->addresses.push_back(address);
                    }
                    FreeAddrInfoW(list);
                }
            }
            catch (...)
            {
                job->addresses.clear();
            }
            SetEvent(job->done);
            ReleaseJob(job);
            return 0;
        }

        /// DNS lookup with a timeout. getaddrinfo itself cannot be cancelled, so it runs on a
        /// worker thread that is simply abandoned when it takes too long.
        inline std::vector<ResolvedAddress> Resolve(const std::wstring& host, const wchar_t* port, int socketType,
                                                    int protocol, DWORD timeoutMs)
        {
            ResolveJob* job = new ResolveJob();
            job->host = host;
            job->port = port;
            job->socketType = socketType;
            job->protocol = protocol;
            job->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!job->done)
            {
                delete job;
                return {};
            }

            HANDLE thread = CreateThread(nullptr, 0, &ResolveThreadProc, job, 0, nullptr);
            if (!thread)
            {
                CloseHandle(job->done);
                delete job;
                return {};
            }
            CloseHandle(thread);

            std::vector<ResolvedAddress> result;
            if (WaitForSingleObject(job->done, timeoutMs) == WAIT_OBJECT_0)
                result = std::move(job->addresses);
            ReleaseJob(job);

            // Try IPv4 first: a half-working IPv6 setup would otherwise eat the whole timeout.
            static_cast<void>(std::stable_partition(result.begin(), result.end(),
                                  [](const ResolvedAddress& address) { return address.family == AF_INET; }));
            return result;
        }

        inline bool WaitReadable(SOCKET sock, DWORD timeoutMs) noexcept
        {
            if (timeoutMs == 0)
                return false;
            fd_set readSet;
            readSet.fd_count = 1;
            readSet.fd_array[0] = sock;
            timeval timeout;
            timeout.tv_sec = static_cast<long>(timeoutMs / 1000);
            timeout.tv_usec = static_cast<long>((timeoutMs % 1000) * 1000);
            return select(0, &readSet, nullptr, nullptr, &timeout) > 0 && readSet.fd_count == 1;
        }

        inline bool ConnectWithTimeout(SOCKET sock, const ResolvedAddress& address, DWORD timeoutMs) noexcept
        {
            u_long nonBlocking = 1;
            if (timeoutMs == 0 || ioctlsocket(sock, static_cast<long>(FIONBIO), &nonBlocking) != 0)
                return false;

            const sockaddr* target = reinterpret_cast<const sockaddr*>(&address.address);
            if (connect(sock, target, address.addressLength) == SOCKET_ERROR)
            {
                if (WSAGetLastError() != WSAEWOULDBLOCK)
                    return false;

                fd_set writeSet;
                writeSet.fd_count = 1;
                writeSet.fd_array[0] = sock;
                fd_set errorSet;
                errorSet.fd_count = 1;
                errorSet.fd_array[0] = sock;
                timeval timeout;
                timeout.tv_sec = static_cast<long>(timeoutMs / 1000);
                timeout.tv_usec = static_cast<long>((timeoutMs % 1000) * 1000);
                if (select(0, nullptr, &writeSet, &errorSet, &timeout) <= 0 || errorSet.fd_count != 0 ||
                    writeSet.fd_count != 1)
                    return false;
            }

            int error = 0;
            int length = static_cast<int>(sizeof(error));
            if (getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) != 0 ||
                error != 0)
                return false;

            u_long blocking = 0;
            return ioctlsocket(sock, static_cast<long>(FIONBIO), &blocking) == 0;
        }

        inline bool SendAll(SOCKET sock, const std::string& data) noexcept
        {
            std::size_t sent = 0;
            while (sent < data.size())
            {
                const int n = send(sock, data.data() + sent, static_cast<int>(data.size() - sent), 0);
                if (n == SOCKET_ERROR || n == 0)
                    return false;
                sent += static_cast<std::size_t>(n);
            }
            return true;
        }

        inline std::optional<double> QuerySntp(const std::wstring& server, DWORD timeoutMs)
        {
            const ULONGLONG deadline = GetTickCount64() + timeoutMs;
            const std::vector<ResolvedAddress> addresses =
                Resolve(server, L"123", SOCK_DGRAM, IPPROTO_UDP, timeoutMs);

            for (const ResolvedAddress& address : addresses)
            {
                if (RemainingMs(deadline) == 0)
                    break;

                SocketHandle sock(socket(address.family, address.socketType, address.protocol));
                if (!sock.Valid())
                    continue;

                // A connected UDP socket only receives datagrams from this server.
                if (connect(sock.Get(), reinterpret_cast<const sockaddr*>(&address.address), address.addressLength) ==
                    SOCKET_ERROR)
                    continue;

                unsigned char request[48] = {};
                request[0] = 0x23; // LI = 0, VN = 4, Mode = 3 (client)

                const double t1 = UnixNow();
                if (send(sock.Get(), reinterpret_cast<const char*>(request), static_cast<int>(sizeof(request)), 0) ==
                    SOCKET_ERROR)
                    continue;

                // Keep reading until a valid reply arrives or time runs out.
                while (WaitReadable(sock.Get(), RemainingMs(deadline)))
                {
                    unsigned char reply[512];
                    const int received =
                        recv(sock.Get(), reinterpret_cast<char*>(reply), static_cast<int>(sizeof(reply)), 0);
                    const double t4 = UnixNow();
                    if (received == SOCKET_ERROR)
                        break; // e.g. WSAECONNRESET after an ICMP port unreachable

                    const std::optional<double> offset =
                        ParseSntpResponse(reply, static_cast<std::size_t>(received), t1, t4);
                    if (offset)
                        return offset;
                }
            }
            return std::nullopt;
        }

        inline std::optional<double> QueryHttpDate(const std::wstring& host, DWORD timeoutMs)
        {
            const ULONGLONG deadline = GetTickCount64() + timeoutMs;
            const std::vector<ResolvedAddress> addresses = Resolve(host, L"80", SOCK_STREAM, IPPROTO_TCP, timeoutMs);
            const std::string request = "HEAD / HTTP/1.1\r\nHost: " + NarrowAscii(host) +
                                        "\r\nUser-Agent: PwfTimeSync\r\nConnection: close\r\n\r\n";

            for (const ResolvedAddress& address : addresses)
            {
                SocketHandle sock(socket(address.family, address.socketType, address.protocol));
                if (!sock.Valid() || !ConnectWithTimeout(sock.Get(), address, RemainingMs(deadline)))
                    continue;

                const double t1 = UnixNow();
                if (!SendAll(sock.Get(), request))
                    continue;

                std::string response;
                double t4 = 0;
                while (response.find("\r\n\r\n") == std::string::npos && response.size() < 16384 &&
                       WaitReadable(sock.Get(), RemainingMs(deadline)))
                {
                    char buffer[2048];
                    const int received = recv(sock.Get(), buffer, static_cast<int>(sizeof(buffer)), 0);
                    if (received == SOCKET_ERROR || received == 0)
                        break;
                    if (response.empty())
                        t4 = UnixNow();
                    response.append(buffer, static_cast<std::size_t>(received));
                }

                const std::optional<long long> date = ParseHttpDateHeader(response);
                if (date && t4 > 0)
                {
                    // The Date header is truncated to whole seconds, hence the +0.5.
                    return static_cast<double>(*date) - (t1 + t4) / 2.0 + 0.5;
                }
            }
            return std::nullopt;
        }

        using OffsetQuery = std::optional<double> (*)(const std::wstring& host, DWORD timeoutMs);

        struct QueryJob
        {
            OffsetQuery query = nullptr;
            std::wstring host;
            DWORD timeoutMs = 0;
            std::optional<double> offset;
            HANDLE done = nullptr;
            LONG references = 2; // caller + worker thread
        };

        struct JobReleaser
        {
            template <typename Job>
            void operator()(Job* job) const noexcept
            {
                ReleaseJob(job);
            }
        };

        inline DWORD WINAPI QueryThreadProc(LPVOID parameter)
        {
            QueryJob* job = static_cast<QueryJob*>(parameter);
            try
            {
                // Own reference: the caller may stop waiting and clean up Winsock first.
                WinsockScope winsock;
                if (winsock.Ok())
                    job->offset = job->query(job->host, job->timeoutMs);
            }
            catch (...)
            {
                job->offset.reset();
            }
            SetEvent(job->done);
            ReleaseJob(job);
            return 0;
        }

        inline QueryJob* StartQuery(OffsetQuery query, const std::wstring& host, DWORD timeoutMs) noexcept
        {
            QueryJob* job = nullptr;
            try
            {
                job = new QueryJob();
                job->query = query;
                job->host = host;
                job->timeoutMs = timeoutMs;
            }
            catch (...)
            {
                delete job;
                return nullptr;
            }

            job->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!job->done)
            {
                delete job;
                return nullptr;
            }

            HANDLE thread = CreateThread(nullptr, 0, &QueryThreadProc, job, 0, nullptr);
            if (!thread)
            {
                CloseHandle(job->done);
                delete job;
                return nullptr;
            }
            CloseHandle(thread);
            return job;
        }

        /// Queries every host at once and returns the first usable answer within one shared budget.
        /// Queries still running at the deadline are abandoned; each one ends on its own timeout.
        inline std::optional<double> FirstAnswer(const TimeSyncOptions& options, const std::vector<std::wstring>& hosts,
                                                 OffsetQuery query, DWORD budgetMs, const wchar_t* source)
        {
            const ULONGLONG deadline = GetTickCount64() + budgetMs;

            std::vector<std::unique_ptr<QueryJob, JobReleaser>> jobs;
            jobs.reserve(hosts.size());
            for (const std::wstring& host : hosts)
            {
                if (host.empty() || jobs.size() >= MAXIMUM_WAIT_OBJECTS)
                    continue;
                std::unique_ptr<QueryJob, JobReleaser> job(StartQuery(query, host, budgetMs));
                if (job)
                    jobs.push_back(std::move(job));
            }

            std::vector<QueryJob*> waiting;
            waiting.reserve(jobs.size());
            for (const auto& job : jobs)
                waiting.push_back(job.get());

            std::vector<HANDLE> events;
            events.reserve(waiting.size());
            while (!waiting.empty())
            {
                events.clear();
                for (QueryJob* job : waiting)
                    events.push_back(job->done);

                const DWORD wait = WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(), FALSE,
                                                          RemainingMs(deadline));
                const std::size_t index = static_cast<std::size_t>(wait - WAIT_OBJECT_0);
                if (index >= events.size())
                    break; // timeout or failure

                QueryJob* job = waiting[index];
                waiting.erase(waiting.begin() + static_cast<std::ptrdiff_t>(index));
                if (job->offset)
                {
                    Log(options, L"Clock offset " + FormatSeconds(*job->offset) + L" (" + source + L" " + job->host +
                                     L")");
                    return job->offset;
                }
                Log(options, std::wstring(L"No ") + source + L" answer from " + job->host);
            }

            for (QueryJob* job : waiting)
                Log(options, std::wstring(L"No ") + source + L" answer from " + job->host + L" in time");
            return std::nullopt;
        }

        /// Measures the offset with NTP first and HTTP as a fallback. Remembers when UDP 123
        /// looks blocked so repeated measurements do not wait for NTP timeouts again.
        class OffsetProbe
        {
        public:
            explicit OffsetProbe(const TimeSyncOptions& options) : options_(options) {}

            /// Default budgets: up to 1.5 s for NTP, then up to 3 s for HTTP.
            std::optional<double> Measure() { return Measure(kNtpTimeoutMs, kHttpTimeoutMs); }

            std::optional<double> Measure(DWORD ntpBudgetMs, DWORD httpBudgetMs)
            {
                if (!winsock_.Ok())
                {
                    Log(options_, L"Winsock is not available.");
                    return std::nullopt;
                }

                if (!ntpBlocked_)
                {
                    if (const std::optional<double> offset =
                            FirstAnswer(options_, options_.NtpServers, &QuerySntp, ntpBudgetMs, L"NTP"))
                        return offset;
                }

                const std::optional<double> offset =
                    FirstAnswer(options_, options_.HttpFallbackHosts, &QueryHttpDate, httpBudgetMs, L"HTTP");
                if (offset && !ntpBlocked_)
                {
                    ntpBlocked_ = true;
                    Log(options_, L"NTP did not answer; using HTTP for the rest of this check.");
                }
                return offset;
            }

            /// True once NTP failed and HTTP answered: UDP 123 is probably blocked on this network.
            bool NtpBlocked() const noexcept { return ntpBlocked_; }

        private:
            const TimeSyncOptions& options_;
            WinsockScope winsock_;
            bool ntpBlocked_ = false;
        };

        /// Re-measures about once per second until the clock is within the limit or time runs out.
        inline std::optional<double> WaitForCorrectClock(const TimeSyncOptions& options, OffsetProbe& probe,
                                                         DWORD waitMs)
        {
            const ULONGLONG deadline = GetTickCount64() + waitMs;
            std::optional<double> last;
            for (;;)
            {
                if (const std::optional<double> offset = probe.Measure())
                {
                    last = offset;
                    if (IsWithin(offset, options.MaxAllowedOffsetSeconds))
                        return last;
                }
                if (RemainingMs(deadline) <= 1000)
                    return last;
                Sleep(1000);
            }
        }

        inline bool FileExists(const std::wstring& path) noexcept
        {
            const DWORD attributes = GetFileAttributesW(path.c_str());
            return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
        }

        inline bool IsWow64() noexcept
        {
            BOOL wow64 = FALSE;
            return IsWow64Process(GetCurrentProcess(), &wow64) != FALSE && wow64 != FALSE;
        }

        /// Full path of a tool in the native System32 folder. A 32-bit process on 64-bit Windows
        /// is redirected to SysWOW64, so it goes through Sysnative instead.
        inline std::wstring SystemToolPath(const wchar_t* fileName)
        {
            wchar_t windowsDir[MAX_PATH];
            const UINT length = GetSystemWindowsDirectoryW(windowsDir, MAX_PATH);
            if (length == 0 || length >= MAX_PATH)
                return {};

            const std::wstring root(windowsDir, length);
            if (IsWow64())
            {
                const std::wstring native = root + L"\\Sysnative\\" + fileName;
                if (FileExists(native))
                    return native;
            }
            const std::wstring system32Path = root + L"\\System32\\" + fileName;
            return FileExists(system32Path) ? system32Path : std::wstring();
        }

        inline std::wstring CurrentExecutablePath()
        {
            std::vector<wchar_t> buffer(MAX_PATH);
            for (;;)
            {
                const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
                if (length == 0)
                    return {};
                if (length < buffer.size())
                    return std::wstring(buffer.data(), length);
                if (buffer.size() >= 32768)
                    return {};
                buffer.resize(buffer.size() * 2);
            }
        }

        inline bool IsProcessElevated() noexcept
        {
            HANDLE token = nullptr;
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
                return false;

            TOKEN_ELEVATION elevation{};
            DWORD size = 0;
            const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation,
                                                static_cast<DWORD>(sizeof(elevation)), &size);
            CloseHandle(token);
            return ok != FALSE && elevation.TokenIsElevated != 0;
        }

        inline bool EnablePrivilege(const wchar_t* name) noexcept
        {
            HANDLE token = nullptr;
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
                return false;

            TOKEN_PRIVILEGES privileges{};
            privileges.PrivilegeCount = 1;
            privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

            bool ok = LookupPrivilegeValueW(nullptr, name, &privileges.Privileges[0].Luid) != FALSE;
            if (ok)
            {
                // AdjustTokenPrivileges succeeds even when nothing was assigned; check the last error.
                ok = AdjustTokenPrivileges(token, FALSE, &privileges, static_cast<DWORD>(sizeof(privileges)),
                                           nullptr, nullptr) != FALSE &&
                     GetLastError() == ERROR_SUCCESS;
            }
            CloseHandle(token);
            return ok;
        }

        /// Runs a console tool without a window and returns its exit code (nullopt on failure or timeout).
        inline std::optional<DWORD> RunHidden(const std::wstring& exe, const std::wstring& arguments, DWORD timeoutMs)
        {
            const std::wstring commandLine = L"\"" + exe + L"\" " + arguments;
            std::vector<wchar_t> buffer(commandLine.begin(), commandLine.end());
            buffer.push_back(L'\0');

            STARTUPINFOW startup{};
            startup.cb = static_cast<DWORD>(sizeof(startup));
            startup.dwFlags = STARTF_USESHOWWINDOW;
            startup.wShowWindow = static_cast<WORD>(SW_HIDE);
            PROCESS_INFORMATION process{};

            if (!CreateProcessW(exe.c_str(), buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                                nullptr, &startup, &process))
                return std::nullopt;

            CloseHandle(process.hThread);
            std::optional<DWORD> result;
            DWORD exitCode = 0;
            const DWORD wait = WaitForSingleObject(process.hProcess, timeoutMs);
            if (wait == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &exitCode))
                result = exitCode;
            else if (wait == WAIT_TIMEOUT)
                TerminateProcess(process.hProcess, 1); // do not leave it running behind a retry
            CloseHandle(process.hProcess);
            return result;
        }

        struct ShellRunResult
        {
            bool started = false;
            DWORD error = ERROR_SUCCESS;
            std::optional<DWORD> exitCode; // empty when still running at the timeout
        };

        /// Owner for the Windows permission prompt. Without one, the prompt often opens behind
        /// other windows and only flashes in the taskbar.
        inline HWND PromptOwner(const TimeSyncOptions& options) noexcept
        {
            if (options.Owner)
                return options.Owner;
            if (HWND console = GetConsoleWindow())
                return console;

            HWND foreground = GetForegroundWindow();
            DWORD processId = 0;
            if (foreground && GetWindowThreadProcessId(foreground, &processId) != 0 &&
                processId == GetCurrentProcessId())
                return foreground;
            return nullptr;
        }

        /// ShellExecuteEx with a hidden window; needed for executables that require elevation.
        inline ShellRunResult ShellRunHidden(const std::wstring& exe, const wchar_t* verb, const wchar_t* parameters,
                                             DWORD timeoutMs, HWND owner)
        {
            SHELLEXECUTEINFOW info{};
            info.cbSize = static_cast<DWORD>(sizeof(info));
            info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
            info.hwnd = owner;
            info.lpVerb = verb;
            info.lpFile = exe.c_str();
            info.lpParameters = parameters;
            info.nShow = SW_HIDE;

            ShellRunResult result;
            if (!ShellExecuteExW(&info))
            {
                result.error = GetLastError();
                return result;
            }

            result.started = true;
            if (info.hProcess)
            {
                DWORD exitCode = 0;
                if (WaitForSingleObject(info.hProcess, timeoutMs) == WAIT_OBJECT_0 &&
                    GetExitCodeProcess(info.hProcess, &exitCode))
                    result.exitCode = exitCode;
                CloseHandle(info.hProcess);
            }
            return result;
        }

        enum class SyncNowOutcome
        {
            Ran,
            NotAvailable,
            Declined
        };

        /// The same action as Settings > Time & language > Date & time > "Sync now".
        /// The helper auto-elevates silently for users in the Administrators group.
        inline SyncNowOutcome RunWindowsSyncNow(const TimeSyncOptions& options)
        {
            const std::wstring exe = SystemToolPath(L"SystemSettingsAdminFlows.exe");
            if (exe.empty())
            {
                Log(options, L"Windows \"Sync now\" helper not found on this system.");
                return SyncNowOutcome::NotAvailable;
            }

            Log(options, L"Running Windows \"Sync now\"...");
            const ShellRunResult run =
                ShellRunHidden(exe, nullptr, L"ForceTimeSync 1",
                               static_cast<DWORD>(options.SyncNowTimeoutMs > 0 ? options.SyncNowTimeoutMs : 0),
                               PromptOwner(options));
            if (!run.started)
            {
                if (run.error == ERROR_CANCELLED)
                {
                    Log(options, L"\"Sync now\" was cancelled at the permission prompt.");
                    return SyncNowOutcome::Declined;
                }
                Log(options, L"\"Sync now\" could not start (error " + std::to_wstring(run.error) + L").");
                return SyncNowOutcome::NotAvailable;
            }
            if (run.exitCode)
                Log(options, L"\"Sync now\" finished with exit code " + std::to_wstring(*run.exitCode) + L".");
            else
                Log(options, L"\"Sync now\" is still running; checking the clock anyway.");
            return SyncNowOutcome::Ran;
        }

        inline bool StartTimeService(const TimeSyncOptions& options)
        {
            SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
            if (!manager)
                return false;

            bool running = false;
            SC_HANDLE service = OpenServiceW(manager, L"W32Time", SERVICE_START | SERVICE_QUERY_STATUS);
            if (service)
            {
                if (StartServiceW(service, 0, nullptr) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING)
                {
                    for (int i = 0; i < 50 && !running; ++i)
                    {
                        SERVICE_STATUS status{};
                        if (!QueryServiceStatus(service, &status))
                            break;
                        running = status.dwCurrentState == SERVICE_RUNNING;
                        if (!running)
                            Sleep(100);
                    }
                }
                else
                {
                    Log(options, L"Could not start W32Time (error " + std::to_wstring(GetLastError()) + L").");
                }
                CloseServiceHandle(service);
            }
            CloseServiceHandle(manager);
            return running;
        }

        /// Sets the clock to local UTC + offset. Requires an elevated process.
        inline bool SetClockByOffset(double offsetSeconds) noexcept
        {
            if (!EnablePrivilege(L"SeSystemtimePrivilege"))
                return false;

            const long long delta = std::llround(offsetSeconds * 1e7);
            ULARGE_INTEGER target;
            target.QuadPart = FileTimeNow() + static_cast<ULONGLONG>(delta);

            FILETIME fileTime;
            fileTime.dwLowDateTime = target.LowPart;
            fileTime.dwHighDateTime = target.HighPart;

            SYSTEMTIME systemTime;
            return FileTimeToSystemTime(&fileTime, &systemTime) != FALSE && ::SetSystemTime(&systemTime) != FALSE;
        }

        /// Runs w32tm /resync and returns its exit code (nullopt when it did not run or timed out).
        inline std::optional<DWORD> RunW32tmResync(const TimeSyncOptions& options)
        {
            if (!StartTimeService(options))
                return std::nullopt;
            const std::wstring w32tm = SystemToolPath(L"w32tm.exe");
            if (w32tm.empty())
                return std::nullopt;

            Log(options, L"Running w32tm /resync...");
            const ULONGLONG start = GetTickCount64();
            std::optional<DWORD> code = RunHidden(w32tm, L"/resync /force", kW32tmTimeoutMs);
            // Older w32tm builds reject /force at once; only that quick failure is worth a plain retry.
            if (code && *code != 0 && GetTickCount64() - start < kW32tmQuickFailureMs)
            {
                Sleep(1000);
                code = RunHidden(w32tm, L"/resync", kW32tmTimeoutMs);
            }
            Log(options, code ? L"w32tm exit code " + std::to_wstring(*code) + L"."
                              : std::wstring(L"w32tm did not finish."));
            return code;
        }

        /// Step for elevated processes: W32Time resync first, then set the clock directly.
        /// Returns AlreadyCorrect when the clock turned out fine without w32tm having run.
        inline std::optional<TimeSyncMethod> FixClockAsAdmin(const TimeSyncOptions& options, OffsetProbe& probe,
                                                             std::optional<double>& lastOffset)
        {
            const double limit = options.MaxAllowedOffsetSeconds;
            const ULONGLONG start = GetTickCount64();
            bool w32tmRan = false;

            if (probe.NtpBlocked())
            {
                // W32Time also needs UDP 123, so it cannot help on this network.
                Log(options, L"NTP is blocked on this network; skipping w32tm.");
            }
            else if (RunW32tmResync(options))
            {
                w32tmRan = true;
                if (const std::optional<double> offset = WaitForCorrectClock(options, probe, 6000))
                    lastOffset = offset;
                if (IsWithin(lastOffset, limit))
                    return TimeSyncMethod::W32tmResync;
            }

            if (GetTickCount64() - start > kElevatedFixBudgetMs)
            {
                Log(options, L"Out of time; not setting the clock directly.");
                return std::nullopt;
            }

            // Fresh measurement right before setting the clock.
            const std::optional<double> fresh = probe.Measure();
            if (!fresh)
                return std::nullopt;
            lastOffset = fresh;
            if (IsWithin(fresh, limit))
                return w32tmRan ? TimeSyncMethod::W32tmResync : TimeSyncMethod::AlreadyCorrect;

            Log(options, L"Setting the system clock directly...");
            if (!SetClockByOffset(*fresh))
            {
                Log(options, L"SetSystemTime failed (error " + std::to_wstring(GetLastError()) + L").");
                return std::nullopt;
            }

            if (const std::optional<double> offset = probe.Measure())
                lastOffset = offset;
            return IsWithin(lastOffset, limit) ? std::optional<TimeSyncMethod>(TimeSyncMethod::SetSystemTime)
                                               : std::nullopt;
        }

        /// Relaunches this executable elevated with the helper flag. Returns false with a message
        /// when it could not run (for example when the UAC prompt was declined).
        inline bool RunElevatedHelper(const TimeSyncOptions& options, std::wstring& error)
        {
            const std::wstring exe = CurrentExecutablePath();
            if (exe.empty())
            {
                error = L"Could not determine the executable path for the elevated helper.";
                return false;
            }

            Log(options, L"Asking for administrator rights to fix the clock...");
            const ShellRunResult run =
                ShellRunHidden(exe, L"runas", ElevatedFlag, kElevatedHelperTimeoutMs, PromptOwner(options));
            if (!run.started)
            {
                error = run.error == ERROR_CANCELLED
                            ? std::wstring(kDeclinedMessage)
                            : L"Could not start the elevated helper (error " + std::to_wstring(run.error) + L").";
                return false;
            }

            Log(options, run.exitCode ? L"Elevated helper exit code " + std::to_wstring(*run.exitCode) + L"."
                                      : std::wstring(L"Elevated helper did not finish in time."));
            return true;
        }

        inline void SetOutcome(TimeSyncResult& result, const TimeSyncOptions& options, bool success,
                               TimeSyncMethod method, std::wstring message)
        {
            result.Success = success;
            result.Method = method;
            result.Message = std::move(message);
            Log(options, result.Message);
        }

        inline const wchar_t* DescribeFix(TimeSyncMethod method) noexcept
        {
            switch (method)
            {
            case TimeSyncMethod::WindowsSyncNow: return L"The clock was synchronized with Windows \"Sync now\".";
            case TimeSyncMethod::W32tmResync: return L"The clock was synchronized with w32tm.";
            case TimeSyncMethod::SetSystemTime: return L"The system clock was set directly.";
            default: return L"The clock is correct now.";
            }
        }

        inline void RunEnsure(const TimeSyncOptions& options, TimeSyncResult& result)
        {
            const double limit = options.MaxAllowedOffsetSeconds;
            OffsetProbe probe(options);

            result.OffsetBeforeSeconds = probe.Measure();
            if (!result.OffsetBeforeSeconds)
            {
                SetOutcome(result, options, true, TimeSyncMethod::NoReference,
                           L"No time reference reachable; the clock was left unchanged.");
                return;
            }

            const bool alreadyCorrect = IsWithin(result.OffsetBeforeSeconds, limit);
            if (alreadyCorrect && !options.ForceSync)
            {
                result.OffsetAfterSeconds = result.OffsetBeforeSeconds;
                SetOutcome(result, options, true, TimeSyncMethod::AlreadyCorrect, L"The clock is already correct.");
                return;
            }

            std::optional<double> current = result.OffsetBeforeSeconds;

            const SyncNowOutcome syncNow = RunWindowsSyncNow(options);
            if (syncNow == SyncNowOutcome::Ran)
            {
                if (const std::optional<double> offset = WaitForCorrectClock(options, probe, kSyncNowSettleMs))
                    current = offset;
                if (IsWithin(current, limit))
                {
                    result.OffsetAfterSeconds = current;
                    SetOutcome(result, options, true, TimeSyncMethod::WindowsSyncNow,
                               L"The clock was synchronized with Windows \"Sync now\".");
                    return;
                }
            }
            else if (alreadyCorrect)
            {
                result.OffsetAfterSeconds = current;
                SetOutcome(result, options, true, TimeSyncMethod::AlreadyCorrect, L"The clock is already correct.");
                return;
            }
            else if (syncNow == SyncNowOutcome::Declined)
            {
                // The user already said no to the Windows prompt; do not ask a second time.
                result.OffsetAfterSeconds = current;
                SetOutcome(result, options, false, TimeSyncMethod::Failed, kDeclinedMessage);
                return;
            }

            if (IsProcessElevated())
            {
                if (std::optional<TimeSyncMethod> method = FixClockAsAdmin(options, probe, current))
                {
                    // A late "Sync now" can fix the clock while the elevated step is still checking.
                    if (*method == TimeSyncMethod::AlreadyCorrect && syncNow == SyncNowOutcome::Ran)
                        method = TimeSyncMethod::WindowsSyncNow;
                    result.OffsetAfterSeconds = current;
                    SetOutcome(result, options, true, *method, DescribeFix(*method));
                    return;
                }
            }
            else if (options.AllowElevationPrompt)
            {
                std::wstring error;
                if (!RunElevatedHelper(options, error))
                {
                    result.OffsetAfterSeconds = current;
                    SetOutcome(result, options, false, TimeSyncMethod::Failed, error);
                    return;
                }

                if (const std::optional<double> offset = WaitForCorrectClock(options, probe, 3000))
                    current = offset;
                if (IsWithin(current, limit))
                {
                    result.OffsetAfterSeconds = current;
                    SetOutcome(result, options, true, TimeSyncMethod::ElevatedHelper,
                               L"The clock was fixed with administrator rights.");
                    return;
                }
            }

            result.OffsetAfterSeconds = current;
            SetOutcome(result, options, false, TimeSyncMethod::Failed,
                       L"Could not correct the system clock (offset " + FormatOffset(current) + L").");
        }

        inline bool HasElevatedFlag()
        {
            int count = 0;
            LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
            if (!arguments)
                return false;

            bool found = false;
            for (int i = 1; i < count && !found; ++i)
                found = std::wcscmp(arguments[i], ElevatedFlag) == 0;
            LocalFree(arguments);
            return found;
        }

        /// The elevated side of the helper: W32Time resync and SetSystemTime only, no UI.
        inline bool RunElevatedSide(const TimeSyncOptions& options)
        {
            if (!IsProcessElevated())
                return false;

            OffsetProbe probe(options);
            std::optional<double> offset = probe.Measure();
            if (!offset)
                return false;
            if (IsWithin(offset, options.MaxAllowedOffsetSeconds))
                return true;
            return FixClockAsAdmin(options, probe, offset).has_value();
        }
    } // namespace detail

    /// Reference UTC minus local UTC in seconds, or nullopt when no reference answered.
    /// timeoutMs limits the whole measurement: up to half of it (at most 1.5 s) for NTP,
    /// the rest for the HTTP fallback.
    inline std::optional<double> GetClockOffsetSeconds(const TimeSyncOptions& options,
                                                       int timeoutMs = 3000) noexcept
    {
        try
        {
            const DWORD total = static_cast<DWORD>(timeoutMs > 0 ? timeoutMs : 0);
            const DWORD ntpBudget = detail::ClampMs(total / 2, 200, detail::kNtpTimeoutMs);
            const DWORD httpBudget = total > ntpBudget + 300 ? total - ntpBudget : 300;

            detail::OffsetProbe probe(options);
            return probe.Measure(ntpBudget, httpBudget);
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    inline std::optional<double> GetClockOffsetSeconds(int timeoutMs = 3000) noexcept
    {
        try
        {
            const TimeSyncOptions options{};
            return GetClockOffsetSeconds(options, timeoutMs);
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    /// Checks the clock and fixes it when it is off. Call it before the license login.
    /// Never throws.
    inline TimeSyncResult EnsureClockIsCorrect(const TimeSyncOptions& options = {}) noexcept
    {
        TimeSyncResult result;
        try
        {
            detail::RunEnsure(options, result);
        }
        catch (const std::exception& e)
        {
            result.Success = false;
            result.Method = TimeSyncMethod::Failed;
            try
            {
                result.Message = L"Unexpected error: " + detail::Widen(e.what());
                detail::Log(options, result.Message);
            }
            catch (...)
            {
            }
        }
        catch (...)
        {
            result.Success = false;
            result.Method = TimeSyncMethod::Failed;
            try
            {
                result.Message = L"Unexpected error while checking the clock.";
                detail::Log(options, result.Message);
            }
            catch (...)
            {
            }
        }
        return result;
    }

    /// Call as the first line of main. When the process was relaunched elevated with
    /// --pwf-timesync-elevated it fixes the clock and exits (0 = fixed, 1 = failed);
    /// otherwise it returns false immediately.
    inline bool HandleElevatedCommand(const TimeSyncOptions& options = {}) noexcept
    {
        bool elevatedRun = false;
        try
        {
            elevatedRun = detail::HasElevatedFlag();
        }
        catch (...)
        {
            return false;
        }
        if (!elevatedRun)
            return false;

        int exitCode = 1;
        try
        {
            exitCode = detail::RunElevatedSide(options) ? 0 : 1;
        }
        catch (...)
        {
        }
        std::exit(exitCode);
    }
} // namespace PwfTimeSync

#endif // PWF_AUTO_TIME_SYNC_H
