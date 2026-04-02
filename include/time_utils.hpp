#pragma once

#include <chrono>
#include <ctime>
#include <functional>
#include <iomanip>
#include <optional>
#include <sstream>
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
    const auto subseconds =
        duration_cast<milliseconds>(tp.time_since_epoch()) % 1000;

    std::tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &t);
#else
    gmtime_r(&t, &tm_utc);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm_utc, "%Y-%m-%d %H:%M:%S");
    oss << '.' << std::setw(3) << std::setfill('0') << subseconds.count();
    return oss.str();
}

// Parse "YYYY-MM-DD HH:MM:SS[.mmm]" (UTC) para system_clock
inline std::optional<std::chrono::system_clock::time_point>
parse_iso8601_ms(std::string_view str) {
    std::tm tm_utc{};
    std::istringstream iss((std::string(str)));
    iss >> std::get_time(&tm_utc, "%Y-%m-%d %H:%M:%S");
    if (iss.fail()) {
        return std::nullopt;
    }

    // Verifica se há parte fracionária
    long millis = 0;
    if (iss.peek() == '.') {
        iss.get(); // consome '.'
        std::string frac;
        iss >> frac;
        if (!frac.empty()) {
            // aceita até 3 dígitos; truncando/adequando conforme tamanho
            if (frac.size() > 3) {
                frac = frac.substr(0, 3);
            } else if (frac.size() < 3) {
                frac.append(3 - frac.size(), '0');
            }
            millis = std::stol(frac);
        }
    }

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
    std::ostringstream oss;
    oss << std::put_time(&tm_local, "%Y-%m-%d %H:%M:%S");
    return oss.str();
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

    ~ScopedTimer() {
        if (on_finish_) {
            const auto end = now_steady();
            on_finish_(elapsed_ms(start_, end));
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
