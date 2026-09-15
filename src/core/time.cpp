#include "streamforge/core/time.hpp"

#include <cctype>
#include <cstdlib>
#include <map>
#include <mutex>
#include <sstream>
#include <utility>

#include <date/date.h>
#include <date/tz.h>

namespace streamforge {

int64_t to_unix_us(TimePointUs tp) {
    return tp.time_since_epoch().count();
}

TimePointUs from_unix_us(int64_t us) {
    return TimePointUs{std::chrono::microseconds{us}};
}

int64_t now_unix_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

TimePointUs now_time() {
    return from_unix_us(now_unix_us());
}

namespace {

using SysUs = std::chrono::time_point<std::chrono::system_clock, std::chrono::microseconds>;
using LocalUs = date::local_time<std::chrono::microseconds>;

bool all_digits(const std::string& s) {
    if (s.empty())
        return false;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c)))
            return false;
    }
    return true;
}

// Rewrites a seconds field of ":60" (leap second) into ":00" so the remainder of the value can
// be parsed normally; the caller adds one minute afterwards. Returns false when the string has
// no leap-second seconds field.
bool extract_leap_second(const std::string& raw, std::string& out) {
    size_t t_pos = raw.find('T');
    if (t_pos == std::string::npos)
        t_pos = raw.find(' ');
    if (t_pos == std::string::npos)
        return false;
    // Seconds token ends before the fraction/zone suffix; locate it by scanning digits and
    // colons of the time part.
    size_t time_end = t_pos + 1;
    while (time_end < raw.size() && (std::isdigit(static_cast<unsigned char>(raw[time_end])) || raw[time_end] == ':')) {
        ++time_end;
    }
    std::string time_part = raw.substr(t_pos + 1, time_end - t_pos - 1);
    size_t last_colon = time_part.rfind(':');
    if (last_colon == std::string::npos)
        return false;
    size_t sec_pos = t_pos + 1 + last_colon + 1;
    if (sec_pos + 1 >= raw.size() + 1)
        return false;
    if (sec_pos + 1 > time_end)
        return false;
    if (raw[sec_pos] != '6' || raw[sec_pos + 1] != '0')
        return false;
    if (sec_pos + 2 != time_end)
        return false; // exactly "…:60"
    // The following character (if any) must be a fraction/zone separator.
    if (time_end < raw.size()) {
        char c = raw[time_end];
        if (c != '.' && c != ',' && c != '+' && c != '-' && c != 'Z' && c != 'z')
            return false;
    }
    out = raw;
    out[sec_pos] = '0';
    out[sec_pos + 1] = '0';
    return true;
}

// Normalizes the timezone suffix into the form date-lib %z accepts ("+HHMM"):
// 'Z'/'z' becomes "+0000"; "+HH:MM"/"-HH:MM" drops the colon. Returns false when a
// recognizable offset is out of range (hours > 23 or minutes > 59).
bool normalize_offset_suffix(std::string& s) {
    if (s.empty())
        return true;
    char last = s.back();
    if (last == 'Z' || last == 'z') {
        s = s.substr(0, s.size() - 1) + "+0000";
        return true;
    }
    if (s.size() >= 6) {
        char sign = s[s.size() - 6];
        char colon = s[s.size() - 3];
        auto is_digit = [&s](size_t pos) { return std::isdigit(static_cast<unsigned char>(s[pos])) != 0; };
        bool digits =
            is_digit(s.size() - 5) && is_digit(s.size() - 4) && is_digit(s.size() - 2) && is_digit(s.size() - 1);
        if ((sign == '+' || sign == '-') && colon == ':' && digits) {
            int hours = (s[s.size() - 5] - '0') * 10 + (s[s.size() - 4] - '0');
            int minutes = (s[s.size() - 2] - '0') * 10 + (s[s.size() - 1] - '0');
            if (hours > 23 || minutes > 59)
                return false;
            s = s.substr(0, s.size() - 3) + s.substr(s.size() - 2);
        }
    }
    return true;
}

bool consumed_all(std::istringstream& in) {
    if (in.fail())
        return false;
    in >> std::ws;
    return in.eof();
}

} // namespace

Result<ParsedTime> parse_event_time(const std::string& raw, const date::time_zone* device_tz, bool ambiguous_earlier) {
    ParsedTime result;
    if (raw.empty()) {
        return Result<ParsedTime>::Err(Error::make(ErrorCode::ValidationTimeInvalid, "empty time"));
    }

    // Unix epoch milliseconds.
    const std::string digits_part = (!raw.empty() && raw[0] == '-') ? raw.substr(1) : raw;
    if (all_digits(digits_part)) {
        if (digits_part.size() > 16) {
            return Result<ParsedTime>::Err(
                Error::make(ErrorCode::ValidationTimeInvalid, "unix milliseconds out of range").ctx("value", raw));
        }
        errno = 0;
        char* endp = nullptr;
        long long ms = std::strtoll(raw.c_str(), &endp, 10);
        if (errno != 0 || endp != raw.c_str() + raw.size()) {
            return Result<ParsedTime>::Err(
                Error::make(ErrorCode::ValidationTimeInvalid, "unix milliseconds out of range").ctx("value", raw));
        }
        result.tp = from_unix_us(ms * 1000);
        return Result<ParsedTime>::Ok(result);
    }

    std::string normalized = raw;
    if (!normalize_offset_suffix(normalized)) {
        return Result<ParsedTime>::Err(
            Error::make(ErrorCode::ValidationTimeInvalid, "UTC offset out of range").ctx("value", raw));
    }
    bool leap = false;
    {
        std::string rewritten;
        if (extract_leap_second(normalized, rewritten)) {
            leap = true;
            normalized = rewritten;
        }
    }

    // ISO 8601 with explicit UTC offset.
    {
        SysUs tp{};
        std::istringstream in(normalized);
        in >> date::parse("%FT%T%z", tp);
        if (consumed_all(in)) {
            result.tp = tp;
            result.leap_second = leap;
            if (leap)
                result.tp += std::chrono::minutes{1};
            return Result<ParsedTime>::Ok(result);
        }
    }

    // ISO 8601 without offset: interpret in the device timezone with DST rules.
    {
        LocalUs lt{};
        std::istringstream in(normalized);
        in >> date::parse("%FT%T", lt);
        if (consumed_all(in)) {
            if (device_tz == nullptr) {
                return Result<ParsedTime>::Err(
                    Error::make(ErrorCode::ValidationTimeInvalid, "time without offset requires a device timezone")
                        .ctx("value", raw));
            }
            auto info = device_tz->get_info(lt);
            if (info.result == date::local_info::nonexistent) {
                return Result<ParsedTime>::Err(
                    Error::make(ErrorCode::ValidationTimeInvalid, "local time does not exist (DST gap)")
                        .ctx("value", raw)
                        .ctx("zone", device_tz->name()));
            }
            date::choose choice = (info.result == date::local_info::ambiguous)
                                      ? (ambiguous_earlier ? date::choose::earliest : date::choose::latest)
                                      : date::choose::earliest;
            auto sys = date::make_zoned(device_tz, lt, choice).get_sys_time();
            result.tp = TimePointUs{sys.time_since_epoch()};
            result.leap_second = leap;
            if (leap)
                result.tp += std::chrono::minutes{1};
            return Result<ParsedTime>::Ok(result);
        }
    }

    return Result<ParsedTime>::Err(
        Error::make(ErrorCode::ValidationTimeInvalid, "time is not a supported format").ctx("value", raw));
}

std::string format_utc_us(TimePointUs tp) {
    std::ostringstream out;
    out << date::format("%FT%TZ", date::floor<std::chrono::microseconds>(tp));
    return out.str();
}

Result<TimePointUs> parse_admin_time(const std::string& raw) {
    auto parsed = parse_event_time(raw, nullptr, true);
    if (parsed.ok()) {
        return Result<TimePointUs>::Ok(parsed.value().tp);
    }
    // Retry: offset-less values are interpreted as UTC for administrative inputs.
    LocalUs lt{};
    std::istringstream in(raw);
    in >> date::parse("%FT%T", lt);
    if (consumed_all(in)) {
        return Result<TimePointUs>::Ok(TimePointUs{lt.time_since_epoch()});
    }
    return Result<TimePointUs>::Err(parsed.error());
}

const date::time_zone* locate_timezone(const std::string& name) {
    static std::mutex mu;
    static std::map<std::string, const date::time_zone*> cache;
    std::lock_guard<std::mutex> lock(mu);
    auto it = cache.find(name);
    if (it != cache.end())
        return it->second;
    const date::time_zone* zone = nullptr;
    try {
        zone = date::locate_zone(name);
    } catch (...) {
        zone = nullptr;
    }
    cache[name] = zone;
    return zone;
}

} // namespace streamforge
