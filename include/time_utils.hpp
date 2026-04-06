#pragma once

#include <algorithm>
#include <charconv>
#include <chrono>
#include <ctime>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace time_utils {

// ------------ Clocks ------------
inline std::chrono::system_clock::time_point now_system() {
    return std::chrono::system_clock::now();
}

inline std::chrono::steady_clock::time_point now_steady() {
    return std::chrono::steady_clock::now();
}

// ------------ Conversions & Formatting ------------
// Formato UTC: "YYYY-MM-DD HH:MM:SS.mmm"
inline std::string to_iso8601_ms(std::chrono::system_clock::time_point tp) {
    using namespace std::chrono;
    const auto t = system_clock::to_time_t(tp);

    std::tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &t);
#else
    gmtime_r(&t, &tm_utc);
#endif

    auto mksec = std::chrono::duration_cast<std::chrono::microseconds>(
                     tp.time_since_epoch())
                     .count();
    mksec %= 1000000;

    std::string str;
    std::array<char, 32> buf{};

    size_t strft_res_sz =
        strftime(buf.data(), buf.size(), "%Y/%m/%d %H:%M:%S.", &tm_utc);

    str.reserve(28);
    str.append(buf.data(), strft_res_sz);

    {
        std::string mksecstr = std::to_string(mksec);
        size_t mksecsz = mksecstr.size();

        if (mksecsz < 6) {
            {
                str.append(6 - mksecsz, '0');
            }
        }

        str += mksecstr;
    }

    return str;
}

// Parse "YYYY-MM-DD HH:MM:SS[.mmm]" (UTC) para system_clock
inline std::optional<std::chrono::system_clock::time_point>
parse_iso8601_ms(std::string_view str) {
    constexpr size_t base_len = 19; // "YYYY-MM-DD HH:MM:SS"

    auto parse_int = [](std::string_view v) -> std::optional<int> {
        int value = 0;
        auto res = std::from_chars(v.data(), v.data() + v.size(), value);
        if (res.ec != std::errc{}) {
            return std::nullopt;
        }
        return value;
    };

    if (str.size() < base_len) {
        return std::nullopt;
    }

    auto valid_separators = [](std::string_view s) {
        return s[4] == '-' && s[7] == '-' && s[10] == ' ' && s[13] == ':' &&
               s[16] == ':';
    };

    if (!valid_separators(str)) {
        return std::nullopt;
    }

    auto year = parse_int(str.substr(0, 4));
    auto month = parse_int(str.substr(5, 2));
    auto day = parse_int(str.substr(8, 2));
    auto hour = parse_int(str.substr(11, 2));
    auto minute = parse_int(str.substr(14, 2));
    auto second = parse_int(str.substr(17, 2));

    if (!year || !month || !day || !hour || !minute || !second) {
        return std::nullopt;
    }

    long millis = 0;
    if (str.size() > base_len && str[base_len] == '.') {
        const size_t frac_start = base_len + 1;
        const size_t frac_len = std::min<size_t>(3, str.size() - frac_start);
        auto frac = str.substr(frac_start, frac_len);
        if (auto parsed = parse_int(frac)) {
            millis = *parsed;
            if (frac_len == 1) {
                millis *= 100;
            } else if (frac_len == 2) {
                millis *= 10;
            }
        }
    }

    std::tm tm_utc{};
    tm_utc.tm_year = *year - 1900;
    tm_utc.tm_mon = *month - 1;
    tm_utc.tm_mday = *day;
    tm_utc.tm_hour = *hour;
    tm_utc.tm_min = *minute;
    tm_utc.tm_sec = *second;

#if defined(_WIN32)
    std::time_t tt = _mkgmtime(&tm_utc);
#else
    std::time_t tt = timegm(&tm_utc);
#endif
    if (tt == -1) {
        return std::nullopt;
    }

    auto tp = std::chrono::system_clock::from_time_t(tt);
    tp += std::chrono::milliseconds(millis);
    return tp;
}

// Formata para horário local legível "YYYY-MM-DD HH:MM:SS"
inline std::string to_local_datetime(std::chrono::system_clock::time_point tp) {
    const auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_local{};
#if defined(_WIN32)
    localtime_s(&tm_local, &t);
#else
    localtime_r(&t, &tm_local);
#endif

    std::string str;
    std::array<char, 32> buf{};

    size_t strft_res_sz =
        strftime(buf.data(), buf.size(), "%Y/%m/%d %H:%M:%S", &tm_local);

    str.reserve(28);
    str.append(buf.data(), strft_res_sz);

    return str;
}

// ------------ Durations ------------
inline std::chrono::milliseconds
elapsed_ms(std::chrono::steady_clock::time_point start,
           std::chrono::steady_clock::time_point end) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
}

// ------------ RAII Timer ------------
class ScopedTimer {
  public:
    explicit ScopedTimer(
        std::function<void(std::chrono::milliseconds)> on_finish)
        : on_finish_(std::move(on_finish)), start_(now_steady()) {}

    ~ScopedTimer() noexcept {
        try {
            if (on_finish_) {
                const auto end = now_steady();
                on_finish_(elapsed_ms(start_, end));
            }
        } catch (...) {
            // NOLINT
        }
    }

    ScopedTimer(const ScopedTimer &) = delete;
    ScopedTimer &operator=(const ScopedTimer &) = delete;
    ScopedTimer(ScopedTimer &&) = default;
    ScopedTimer &operator=(ScopedTimer &&) = default;

  private:
    std::function<void(std::chrono::milliseconds)> on_finish_;
    std::chrono::steady_clock::time_point start_;
};

} // namespace time_utils
