#include "file_execution_logger.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <print>

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
    const auto whole = std::chrono::floor<std::chrono::seconds>(now);
    const auto ms = duration_cast<milliseconds>(now - whole);
    return std::format("{:%Y-%m-%dT%H:%M:%S}.{:03}Z", whole, ms.count());
}

void FileExecutionLogger::write_line(const std::string &line) {
    std::lock_guard lock(mutex_);
    if (file_.is_open()) {
        file_ << line << '\n';
        file_.flush();
    }
    if (echo_stdout_) {
        std::print("{}\n", line);
        std::fflush(stdout);
    }
}

void FileExecutionLogger::logMessage(const Message &msg,
                                     const SessionInfo &session) {
    auto line =
        std::format("{} [msg] session={} role={} tokens={} duration_ms={}",
                    timestamp_iso_ms(), session.id, msg.role_to_string(),
                    msg.token_count, msg.duration.count());
    write_line(line);
}

void FileExecutionLogger::logToolEvent(const ToolInvocationContext &ctx,
                                       const ToolDecision &decision,
                                       std::chrono::milliseconds duration,
                                       bool success,
                                       std::string_view result_summary,
                                       const SessionInfo &session) {
    auto line = std::format("{} [tool] session={} name={} decision={} "
                            "success={} duration_ms={} summary={}",
                            timestamp_iso_ms(), session.id, ctx.metadata.name,
                            static_cast<int>(decision.action),
                            success ? "true" : "false", duration.count(),
                            result_summary);
    write_line(line);
}
