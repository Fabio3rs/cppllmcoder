#include "file_execution_logger.hpp"

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;

FileExecutionLogger::FileExecutionLogger(std::string log_path, bool echo_stdout)
    : log_path_(std::move(log_path)), echo_stdout_(echo_stdout) {
    fs::path p(log_path_);
    if (p.has_parent_path()) {
        fs::create_directories(p.parent_path());
    }
    file_.open(log_path_, std::ios::app);
}

std::string FileExecutionLogger::timestamp_iso_ms() const {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3)
        << std::setfill('0') << ms.count() << "Z";
    return oss.str();
}

void FileExecutionLogger::write_line(const std::string &line) {
    std::lock_guard lock(mutex_);
    if (file_.is_open()) {
        file_ << line << '\n';
        file_.flush();
    }
    if (echo_stdout_) {
        std::cout << line << std::endl;
    }
}

void FileExecutionLogger::logMessage(const Message &msg,
                                     const SessionInfo &session) {
    std::ostringstream oss;
    oss << timestamp_iso_ms() << " [msg] session=" << session.id
        << " role=" << msg.role_to_string() << " tokens=" << msg.token_count
        << " duration_ms=" << msg.duration.count();
    write_line(oss.str());
}

void FileExecutionLogger::logToolEvent(const ToolInvocationContext &ctx,
                                       const ToolDecision &decision,
                                       std::chrono::milliseconds duration,
                                       bool success,
                                       std::string_view result_summary,
                                       const SessionInfo &session) {
    std::ostringstream oss;
    oss << timestamp_iso_ms() << " [tool] session=" << session.id
        << " name=" << ctx.metadata.name
        << " decision=" << static_cast<int>(decision.action)
        << " success=" << (success ? "true" : "false")
        << " duration_ms=" << duration.count() << " summary=" << result_summary;
    write_line(oss.str());
}
