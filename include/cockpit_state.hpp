#pragma once

#include "agent_driver.hpp"
#include "brain_store.hpp"
#include "cockpit_chat.hpp"

#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>

enum class AgentMode {
    Ask,
    Guide,
    Agent,
    Yolo,
};

enum class LogKind {
    System,
    LLM,
    Lua,
    Tool,
    DB,
    Agent,
    MCP,
    FS,
    Warning,
    Error,
};

struct LogLine {
    std::chrono::system_clock::time_point timestamp;
    LogKind kind;
    std::string task_id;
    std::string session_id;
    std::string text;
    std::optional<int> duration_ms;
};

struct TaskNode {
    std::string id;
    std::string title;
    std::string status;
    int depth = 0;
    bool expanded = true;
    std::string owner;
    std::string summary;
    std::chrono::milliseconds duration{0};
    std::vector<std::shared_ptr<TaskNode>> children;
};

struct PointerItem {
    std::string id;
    std::string summary;
    std::string source;
    double relevance_score = 0.0;
    std::vector<std::string> related_pointers;
    std::string last_updated;
};

struct ToolItem {
    std::string name;
    std::string status;
    int avg_latency_ms = 0;
    std::string description;
    std::string risk_level;
    int last_call_ms = 0;
};

struct ApprovalItem {
    std::string title;
    std::string risk;
    std::string details;
    std::string action_id;
};

struct DiffHunk {
    std::string file_path;
    int line_start = 0;
    int line_count = 0;
    std::vector<std::string> lines;
    bool staged = false;
};

struct CockpitState {
    std::string session_id;
    std::string workspace;
    std::string model;
    std::string branch;
    std::string db_path;

    AgentMode mode = AgentMode::Guide;
    bool sandbox_enabled = true;
    bool auto_approve = false;

    int selected_tab = 0;
    int selected_task = 0;
    int selected_pointer = 0;
    int selected_tool = 0;
    int selected_log = 0;
    unsigned int selected_approval = 0;
    int selected_diff = 0;
    bool show_inspector = true;
    bool show_help = false;
    bool chat_auto_scroll = true;
    float chat_scroll_position = 1.0F;
    std::optional<std::string> next_injection;

    std::string input_value;
    std::string current_action = "Initializing...";
    std::string mission;

    std::vector<std::string> tabs = {
        "Chat/Plan", "Tasks", "Pointers", "Diff", "Tools", "Logs", "Review",
    };

    std::vector<std::shared_ptr<TaskNode>> root_tasks;
    std::vector<std::shared_ptr<TaskNode>> flat_tasks;
    std::vector<PointerItem> pointers;
    std::vector<ToolItem> tools;
    std::vector<LogLine> logs;
    std::vector<LogLine> db_logs;
    std::vector<ApprovalItem> approvals;
    std::deque<std::pair<std::string, std::string>> pending_turns;
    std::vector<DiffHunk> diff_hunks;
    std::vector<ChatItem> conversation;
    int selected_chat = 0;

    int total_subagents = 0;
    int estimated_tokens = 0;
    double estimated_cost = 0.0;
    int approval_counter = 0;
};

std::string modeToString(AgentMode mode);
ftxui::Color modeToColor(AgentMode mode);
ftxui::Color statusColor(const std::string &status);
ftxui::Color logColor(LogKind kind);
std::string logKindLabel(LogKind kind);
std::string timestampStr(const std::chrono::system_clock::time_point &tp);
std::string indentForDepth(int depth);
std::string detectGitBranch();
std::chrono::system_clock::time_point parseSqliteTimestamp(std::string_view ts);
void flattenTasks(const std::vector<std::shared_ptr<TaskNode>> &roots,
                  std::vector<std::shared_ptr<TaskNode>> &out);
void printSessionTable(const std::vector<SessionSummary> &sessions);
