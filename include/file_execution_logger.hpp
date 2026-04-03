#pragma once

#include "agent.hpp"

#include <fstream>
#include <mutex>
#include <string>

// Simple file-based execution logger for the PoC.
class FileExecutionLogger : public IExecutionLogger {
  public:
    explicit FileExecutionLogger(std::string log_path, bool echo_stdout = true);

    void logMessage(const Message &msg, const SessionInfo &session) override;
    void logToolEvent(const ToolInvocationContext &ctx,
                      const ToolDecision &decision,
                      std::chrono::milliseconds duration, bool success,
                      std::string_view result_summary,
                      const SessionInfo &session) override;

  private:
    std::string timestamp_iso_ms() const;
    void write_line(const std::string &line);

    std::string log_path_;
    bool echo_stdout_;
    mutable std::mutex mutex_;
    std::ofstream file_;
};
