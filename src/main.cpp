#include "fs_tools.hpp"
#include "stdafx.hpp"

#include "agent.hpp"
#include "agent_driver.hpp"
#include "cli_options.hpp"
#include "cockpit_agent_driver.hpp"
#include "cockpit_consent.hpp"
#include "done_task_tool.hpp"
#include "runtime_defaults.hpp"
#include "sqlite3raii.hpp"

#include "getenv.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <openai/openai.hpp>
#include <optional>
#include <print>
#include <queue>
#include <ranges>
#include <span>
#include <sqlite3.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

using namespace ftxui;

// Small helper for std::visit
template <class... Ts> struct overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

// =============================================================================
// DATA MODEL FOR COCKPIT
// =============================================================================

enum class AgentMode {
    Ask,   // Read-only diagnosis
    Guide, // Propose, seek approval
    Agent, // Auto-execute approved steps
    Yolo,  // Maximum autonomy (sandbox)
};

enum class LogKind {
    System,  // Runtime, bootstrap
    LLM,     // Model response, streaming
    Lua,     // Lua VM execution, code blocks
    Tool,    // Tool result
    DB,      // Database query/update
    Agent,   // Agent state, task lifecycle
    MCP,     // Model Context Protocol
    FS,      // File system operation
    Warning, // Policy check, risk flag
    Error,   // Exception, failure
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
    std::string status; // "pending", "running", "completed", "failed"
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
    std::string source;           // "firmware.bin:0x4F00"
    double relevance_score = 0.0; // 0.0 to 1.0
    std::vector<std::string> related_pointers;
    std::string last_updated;
};

struct ToolItem {
    std::string name;
    std::string status; // "ready", "running", "restricted", "failed"
    int avg_latency_ms = 0;
    std::string description;
    std::string risk_level; // "low", "medium", "high"
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
    std::vector<std::string> lines; // raw diff lines starting with + - or space
    bool staged = false;
};

struct CockpitState {
    // Session identity
    std::string session_id;
    std::string workspace;
    std::string model;
    std::string branch;
    std::string db_path;

    // Runtime config
    AgentMode mode = AgentMode::Guide;
    bool sandbox_enabled = true;
    bool auto_approve = false;

    // UI state
    int selected_tab =
        0; // 0=Chat, 1=Tasks, 2=Pointers, 3=Diff, 4=Tools, 5=Logs, 6=Review
    int selected_task = 0;
    int selected_pointer = 0;
    int selected_tool = 0;
    int selected_log = 0;
    unsigned int selected_approval = 0;
    int selected_diff = 0;
    bool show_inspector = true;
    bool show_help = false;
    bool chat_auto_scroll = true; // auto-scroll chat to bottom
    float chat_scroll_position = 1.0F; // 0.0 = top, 1.0 = bottom
    std::optional<std::string> next_injection;

    // Dynamic state
    std::string input_value;
    std::string current_action = "Initializing...";
    std::string mission;

    std::vector<std::string> tabs = {
        "Chat/Plan", "Tasks", "Pointers", "Diff", "Tools", "Logs", "Review",
    };

    std::vector<std::shared_ptr<TaskNode>> root_tasks;
    std::vector<std::shared_ptr<TaskNode>>
        flat_tasks; // pre-order for menus/inspector
    std::vector<PointerItem> pointers;
    std::vector<ToolItem> tools;
    std::vector<LogLine> logs;
    std::vector<LogLine> db_logs;
    std::vector<ApprovalItem> approvals;
    std::deque<std::pair<std::string, std::string>>
        pending_turns; // action_id, text
    std::vector<DiffHunk> diff_hunks;
    std::vector<std::string> conversation;

    // Metadata
    int total_subagents = 0;
    int estimated_tokens = 0;
    double estimated_cost = 0.0;
    int approval_counter = 0;
};

// =============================================================================
// HELPER FUNCTIONS
// =============================================================================

std::string mode_to_string(AgentMode mode) {
    switch (mode) {
    case AgentMode::Ask:
        return "ASK";
    case AgentMode::Guide:
        return "GUIDE";
    case AgentMode::Agent:
        return "AGENT";
    case AgentMode::Yolo:
        return "YOLO";
    }
    return "UNKNOWN";
}

Color mode_to_color(AgentMode mode) {
    switch (mode) {
    case AgentMode::Ask:
        return Color::Blue;
    case AgentMode::Guide:
        return Color::Yellow;
    case AgentMode::Agent:
        return Color::Green;
    case AgentMode::Yolo:
        return Color::Red;
    }
    return Color::White;
}

Color status_color(const std::string &status) {
    if (status == "completed" || status == "ready")
        return Color::Green;
    if (status == "running")
        return Color::Yellow;
    if (status == "queued" || status == "restricted")
        return Color::Magenta;
    if (status == "failed")
        return Color::Red;
    return Color::White;
}

Color log_color(LogKind kind) {
    switch (kind) {
    case LogKind::System:
        return Color::Cyan;
    case LogKind::LLM:
        return Color::Blue;
    case LogKind::Lua:
        return Color::Yellow;
    case LogKind::Tool:
        return Color::Green;
    case LogKind::DB:
        return Color::Magenta;
    case LogKind::Agent:
        return Color::White;
    case LogKind::MCP:
        return Color::Cyan;
    case LogKind::FS:
        return Color::Blue;
    case LogKind::Warning:
        return Color::YellowLight;
    case LogKind::Error:
        return Color::Red;
    }
    return Color::White;
}

std::string log_kind_label(LogKind kind) {
    switch (kind) {
    case LogKind::System:
        return "SYS";
    case LogKind::LLM:
        return "LLM";
    case LogKind::Lua:
        return "LUA";
    case LogKind::Tool:
        return "TOOL";
    case LogKind::DB:
        return "DB";
    case LogKind::Agent:
        return "AGENT";
    case LogKind::MCP:
        return "MCP";
    case LogKind::FS:
        return "FS";
    case LogKind::Warning:
        return "WARN";
    case LogKind::Error:
        return "ERR";
    }
    return "?";
}

std::string timestamp_str(const std::chrono::system_clock::time_point &tp) {
    auto time = std::chrono::system_clock::to_time_t(tp);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  tp.time_since_epoch()) %
              1000;
    struct tm *tm_info = std::localtime(&time);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", tm_info);
    return std::string(buf) + "." + std::to_string(ms.count());
}

std::string indent_for_depth(int depth) {
    return std::string(static_cast<size_t>(depth) * 2, ' ');
}

std::string detect_git_branch() {
    FILE *pipe = popen("git rev-parse --abbrev-ref HEAD 2>/dev/null", "r");
    if (!pipe) {
        return "(unknown)";
    }
    std::array<char, 256> buf{};
    std::string result;
    while (fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
        result += buf.data();
    }
    pclose(pipe);
    // Trim trailing newline
    while (!result.empty() &&
           (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }
    return result.empty() ? "(detached)" : result;
}

std::chrono::system_clock::time_point
parse_sqlite_timestamp(std::string_view ts) {
    // Expected format: YYYY-MM-DD HH:MM:SS.sss
    std::tm tm{};
    std::istringstream ss{std::string(ts)};
    ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
    if (ss.fail()) {
        return std::chrono::system_clock::now();
    }
    auto time_c = std::chrono::system_clock::from_time_t(std::mktime(&tm));
    // Parse fractional seconds if present
    auto dot_pos = ts.find('.');
    if (dot_pos != std::string_view::npos && dot_pos + 1 < ts.size()) {
        std::string_view frac = ts.substr(dot_pos + 1);
        int ms = 0;
        for (size_t i = 0; i < std::min<size_t>(3, frac.size()); ++i) {
            if (std::isdigit(static_cast<unsigned char>(frac[i]))) {
                ms = ms * 10 + (frac[i] - '0');
            }
        }
        time_c += std::chrono::milliseconds(ms);
    }
    return time_c;
}

void flatten_tasks(const std::vector<std::shared_ptr<TaskNode>> &roots,
                   std::vector<std::shared_ptr<TaskNode>> &out) {
    out.clear();
    std::function<void(const std::shared_ptr<TaskNode> &)> dfs =
        [&](const std::shared_ptr<TaskNode> &n) {
            out.push_back(n);
            for (const auto &c : n->children) {
                dfs(c);
            }
        };
    for (const auto &r : roots) {
        dfs(r);
    }
}

// Load task hierarchy from SQLite brain DB.
std::vector<std::shared_ptr<TaskNode>>
load_task_tree_from_db(const std::string &db_path) {
    namespace fs = std::filesystem;
    if (!fs::exists(db_path)) {
        return {};
    }

    SqliteDb db(db_path);
    db.exec("PRAGMA foreign_keys = ON;");

    auto stmt = db.prepare(
        R"sql(
        SELECT id, parent_task_id, description, status
        FROM tasks
        ORDER BY created_at ASC;
    )sql",
        "select tasks");

    struct Row {
        std::string id;
        std::string parent;
        std::string desc;
        std::string status;
    };

    std::vector<Row> rows;
    while (true) {
        const int rc = stmt.step();
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            throw std::runtime_error("failed to step tasks query");
        }
        auto cstr = [&](int idx) -> std::string {
            const unsigned char *txt = sqlite3_column_text(stmt.get(), idx);
            return txt ? reinterpret_cast<const char *>(txt) : "";
        };
        rows.push_back(Row{
            .id = cstr(0),
            .parent = cstr(1),
            .desc = cstr(2),
            .status = cstr(3),
        });
    }

    std::unordered_map<std::string, std::shared_ptr<TaskNode>> map;
    map.reserve(rows.size());
    for (const auto &row : rows) {
        auto node = std::make_shared<TaskNode>();
        node->id = row.id;
        node->title = row.desc;
        node->status = row.status;
        node->owner = "agent";
        node->summary = row.desc;
        map.emplace(row.id, std::move(node));
    }

    std::vector<std::shared_ptr<TaskNode>> roots;
    for (const auto &row : rows) {
        auto it = map.find(row.id);
        if (it == map.end())
            continue;
        auto node = it->second;
        if (!row.parent.empty()) {
            auto parent_it = map.find(row.parent);
            if (parent_it != map.end()) {
                parent_it->second->children.push_back(node);
                continue;
            }
        }
        roots.push_back(node);
    }

    // Assign depths with DFS so nested children are correct.
    std::function<void(const std::shared_ptr<TaskNode> &, int)> set_depth =
        [&](const std::shared_ptr<TaskNode> &n, int depth) {
            n->depth = depth;
            for (auto &child : n->children) {
                set_depth(child, depth + 1);
            }
        };
    for (auto &root : roots) {
        set_depth(root, 0);
    }

    return roots;
}

// Load pointers from SQLite brain DB.
std::vector<PointerItem> load_pointers_from_db(const std::string &db_path) {
    namespace fs = std::filesystem;
    if (!fs::exists(db_path)) {
        return {};
    }

    SqliteDb db(db_path);
    db.exec("PRAGMA foreign_keys = ON;");

    auto stmt = db.prepare(
        R"sql(
        SELECT p.id, p.micro_summary, p.file_path,
               p.offset_start, p.offset_end, p.updated_at
        FROM pointers p
        ORDER BY p.updated_at DESC
        LIMIT 200;
    )sql",
        "select pointers");

    std::vector<PointerItem> pointers;
    while (true) {
        const int rc = stmt.step();
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            break;
        }
        auto cstr = [&](int idx) -> std::string {
            const unsigned char *txt = sqlite3_column_text(stmt.get(), idx);
            return txt ? reinterpret_cast<const char *>(txt) : "";
        };

        PointerItem ptr;
        ptr.id = cstr(0);
        ptr.summary = cstr(1);
        std::string file_path = cstr(2);
        int offset_start = sqlite3_column_int(stmt.get(), 3);
        ptr.source = file_path + ":0x" + ([](int v) {
                         std::ostringstream ss;
                         ss << std::hex << std::uppercase << v;
                         return ss.str();
                     })(offset_start);
        ptr.last_updated = cstr(5);
        pointers.push_back(std::move(ptr));
    }

    // Load relationships from knowledge_graph
    if (!pointers.empty()) {
        try {
            auto kg_stmt = db.prepare(
                R"sql(
                SELECT source_pointer_id, target_pointer_id, relationship_type
                FROM knowledge_graph
                ORDER BY created_at DESC
                LIMIT 500;
            )sql",
                "select knowledge_graph");

            std::unordered_map<std::string, std::vector<std::string>> rels;
            while (true) {
                const int rc = kg_stmt.step();
                if (rc == SQLITE_DONE) {
                    break;
                }
                if (rc != SQLITE_ROW) {
                    break;
                }
                auto cstr = [&](int idx) -> std::string {
                    const unsigned char *txt =
                        sqlite3_column_text(kg_stmt.get(), idx);
                    return txt ? reinterpret_cast<const char *>(txt) : "";
                };
                std::string src = cstr(0);
                std::string tgt = cstr(1);
                std::string rel = cstr(2);
                rels[src].push_back(rel + " → " + tgt);
            }

            for (auto &ptr : pointers) {
                auto it = rels.find(ptr.id);
                if (it != rels.end()) {
                    ptr.related_pointers = it->second;
                }
            }
        } catch (...) {
            // Knowledge graph may not exist yet
        }
    }

    return pointers;
}

// Build ToolItem list from the actual ToolRegistry.
std::vector<ToolItem>
build_tool_items_from_registry(const ToolRegistry &registry) {
    std::vector<ToolItem> items;
    registry.forEach([&](const ToolMetadata &meta, const ITool &) {
        ToolItem item;
        item.name = meta.name;
        item.status = "ready";
        item.description = meta.description;
        item.risk_level = meta.is_sensitive ? "high" : "low";
        if (!meta.danger_tags.empty()) {
            item.risk_level = "medium";
            for (const auto &tag : meta.danger_tags) {
                if (tag == "write" || tag == "exec" || tag == "shell" ||
                    tag == "delete") {
                    item.risk_level = "high";
                    break;
                }
            }
        }
        item.avg_latency_ms = 0;
        item.last_call_ms = 0;
        items.push_back(std::move(item));
    });
    return items;
}

std::vector<DiffHunk> load_diff_from_git() {
    std::vector<DiffHunk> hunks;
    FILE *pipe = popen("git diff --unified=3 --no-color", "r");
    if (!pipe) {
        return hunks;
    }
    std::string output;
    std::array<char, 512> buffer{};
    while (true) {
        size_t n = std::fread(buffer.data(), 1, buffer.size(), pipe);
        if (n == 0)
            break;
        output.append(buffer.data(), n);
    }
    pclose(pipe);

    std::istringstream iss(output);
    std::string line;
    DiffHunk current;
    bool in_hunk = false;
    while (std::getline(iss, line)) {
        if (line.starts_with("diff --git ")) {
            if (in_hunk && !current.file_path.empty()) {
                hunks.push_back(current);
            }
            current = DiffHunk{};
            in_hunk = true;
            // extract file path after b/
            auto pos = line.find(" b/");
            if (pos != std::string::npos) {
                current.file_path = line.substr(pos + 3);
            } else {
                current.file_path = line.substr(11);
            }
        } else if (line.starts_with("@@")) {
            current.lines.push_back(line);
        } else if (in_hunk) {
            current.lines.push_back(line);
        }
    }
    if (in_hunk && !current.file_path.empty()) {
        hunks.push_back(current);
    }
    return hunks;
}

// Load recent logs from DB for the active session.
std::vector<LogLine> load_logs_from_db(const std::string &db_path,
                                       const std::string &session_id,
                                       int limit = 200) {
    namespace fs = std::filesystem;
    if (!fs::exists(db_path)) {
        return {};
    }

    SqliteDb db(db_path);
    db.exec("PRAGMA foreign_keys = ON;");

    auto stmt = db.prepare(
        R"sql(
        SELECT created_at, task_id, role, content, duration_ms
        FROM messages
        WHERE session_id = ?
        ORDER BY created_at DESC
        LIMIT ?
    )sql",
        "select messages");

    stmt.bind(1, session_id).bind(2, limit);

    std::vector<LogLine> logs;
    while (true) {
        const int rc = stmt.step();
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            throw std::runtime_error("failed to step messages query");
        }
        auto cstr = [&](int idx) -> std::string {
            const unsigned char *txt = sqlite3_column_text(stmt.get(), idx);
            return txt ? reinterpret_cast<const char *>(txt) : "";
        };

        LogLine log;
        log.timestamp = parse_sqlite_timestamp(cstr(0));
        log.task_id = cstr(1);
        log.session_id = session_id;
        log.kind = LogKind::LLM;
        log.text = cstr(2) + ": " + cstr(3);
        int duration = sqlite3_column_int(stmt.get(), 4);
        if (duration > 0)
            log.duration_ms = duration;
        logs.push_back(std::move(log));
    }

    auto tool_stmt = db.prepare(
        R"sql(
        SELECT created_at, task_id, tool_name, status, result_summary, duration_ms
        FROM tool_invocations
        WHERE session_id = ?
        ORDER BY created_at DESC
        LIMIT ?
    )sql",
        "select tool invocations");
    tool_stmt.bind(1, session_id).bind(2, limit);
    while (true) {
        const int rc = tool_stmt.step();
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            throw std::runtime_error("failed to step tool_invocations");
        }
        auto cstr = [&](int idx) -> std::string {
            const unsigned char *txt =
                sqlite3_column_text(tool_stmt.get(), idx);
            return txt ? reinterpret_cast<const char *>(txt) : "";
        };

        LogLine log;
        log.timestamp = parse_sqlite_timestamp(cstr(0));
        log.task_id = cstr(1);
        log.session_id = session_id;
        log.kind = LogKind::Tool;
        log.text = cstr(2) + " (" + cstr(3) + "): " + cstr(4);
        int duration = sqlite3_column_int(tool_stmt.get(), 5);
        if (duration > 0)
            log.duration_ms = duration;
        logs.push_back(std::move(log));
    }

    auto exec_stmt = db.prepare(
        R"sql(
        SELECT created_at, task_id, lua_script, duration_ms
        FROM execution_logs
        WHERE session_id = ?
        ORDER BY created_at DESC
        LIMIT ?
    )sql",
        "select execution logs");
    exec_stmt.bind(1, session_id).bind(2, limit);
    while (true) {
        const int rc = exec_stmt.step();
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            throw std::runtime_error("failed to step execution_logs");
        }
        auto cstr = [&](int idx) -> std::string {
            const unsigned char *txt =
                sqlite3_column_text(exec_stmt.get(), idx);
            return txt ? reinterpret_cast<const char *>(txt) : "";
        };

        LogLine log;
        log.timestamp = parse_sqlite_timestamp(cstr(0));
        log.task_id = cstr(1);
        log.session_id = session_id;
        log.kind = LogKind::Lua;
        log.text = "lua: " + cstr(2);
        int duration = sqlite3_column_int(exec_stmt.get(), 3);
        if (duration > 0)
            log.duration_ms = duration;
        logs.push_back(std::move(log));
    }

    std::sort(logs.begin(), logs.end(), [](const LogLine &a, const LogLine &b) {
        return a.timestamp < b.timestamp;
    });

    if (static_cast<int>(logs.size()) > limit) {
        logs.erase(
            logs.begin(),
            logs.begin() +
                static_cast<long>(logs.size() - static_cast<size_t>(limit)));
    }

    return logs;
}

// =============================================================================
// UI COMPONENTS
// =============================================================================

Element header_bar(const CockpitState &state) {
    return hbox({
               text("   CPP-LLM-CODER ") | bold | color(Color::Cyan),
               text(" mission: ") | dim,
               text(state.mission) | flex,
               text(" mode ") | dim,
               text(" " + mode_to_string(state.mode) + " ") | bold |
                   color(mode_to_color(state.mode)),
               text(" "),
           }) |
           border;
}

Element context_bar(const CockpitState &state) {
    return hbox({
               text(" model: " + state.model + " ") | color(Color::Green),
               separatorEmpty(),
               text(" ws: " + state.workspace + " ") | dim,
               separatorEmpty(),
               text(" branch: " + state.branch + " ") | dim,
               separatorEmpty(),
               text(" session: " + state.session_id.substr(0, 12) + " ") | dim,
               separatorEmpty(),
               text(" tokens: " + std::to_string(state.estimated_tokens) +
                    " ") |
                   dim,
               separatorEmpty(),
               text(" sandbox: " +
                    std::string(state.sandbox_enabled ? "ON" : "OFF") + " ") |
                   color(state.sandbox_enabled ? Color::Green : Color::Red),
               separatorEmpty(),
               text(" auto-approve: " +
                    std::string(state.auto_approve ? "ON" : "OFF") + " ") |
                   color(state.auto_approve ? Color::Red : Color::Yellow),
           }) |
           border;
}

Element inspector_panel(const CockpitState &state,
                        const std::vector<LogLine> &merged_logs) {
    Elements lines;

    lines.push_back(text("Inspector") | bold | color(Color::Cyan));

    if (state.selected_tab == 1 && !state.flat_tasks.empty() &&
        state.selected_task < static_cast<int>(state.flat_tasks.size())) {
        // Task details
        lines.push_back(separator());
        auto &task = state.flat_tasks[static_cast<size_t>(state.selected_task)];
        lines.push_back(text("Task: " + task->id) | bold);
        lines.push_back(text("Title: " + task->title));
        lines.push_back(text("Status: " + task->status) |
                        color(status_color(task->status)));
        lines.push_back(text("Owner: " + task->owner));
        lines.push_back(
            text("Duration: " + std::to_string(task->duration.count()) + "ms"));
        lines.push_back(
            text("Children: " + std::to_string(task->children.size())));
        if (!task->summary.empty()) {
            lines.push_back(separator());
            lines.push_back(text("Summary:") | dim);
            lines.push_back(paragraph(task->summary));
        }
    } else if (state.selected_tab == 2 && !state.pointers.empty() &&
               state.selected_pointer <
                   static_cast<int>(state.pointers.size())) {
        // Pointer details
        lines.push_back(separator());
        auto &ptr = state.pointers[static_cast<size_t>(state.selected_pointer)];
        lines.push_back(text("Pointer: " + ptr.id) | bold |
                        color(Color::Yellow));
        lines.push_back(text("Score: " + std::to_string(ptr.relevance_score)));
        lines.push_back(text("Source: " + ptr.source));
        if (!ptr.last_updated.empty()) {
            lines.push_back(text("Updated: " + ptr.last_updated) | dim);
        }
        lines.push_back(separator());
        lines.push_back(paragraph(ptr.summary));
        if (!ptr.related_pointers.empty()) {
            lines.push_back(separator());
            lines.push_back(text("Related:") | dim);
            for (const auto &rp : ptr.related_pointers) {
                lines.push_back(text("  → " + rp));
            }
        }
        lines.push_back(separator());
        lines.push_back(text("/inject " + ptr.id + " to inject into context") |
                        dim);
    } else if (state.selected_tab == 4 && !state.tools.empty() &&
               state.selected_tool < static_cast<int>(state.tools.size())) {
        // Tool details
        lines.push_back(separator());
        auto &tool = state.tools[static_cast<size_t>(state.selected_tool)];
        lines.push_back(text("Tool: " + tool.name) | bold);
        lines.push_back(text("Status: " + tool.status) |
                        color(status_color(tool.status)));
        lines.push_back(text("Risk: " + tool.risk_level) |
                        color(tool.risk_level == "high"     ? Color::Red
                              : tool.risk_level == "medium" ? Color::Yellow
                                                            : Color::Green));
        lines.push_back(
            text("Avg Latency: " + std::to_string(tool.avg_latency_ms) + "ms"));
        if (tool.last_call_ms > 0) {
            lines.push_back(
                text("Last Call: " + std::to_string(tool.last_call_ms) + "ms"));
        }
        lines.push_back(separator());
        lines.push_back(paragraph(tool.description));
    } else if (state.selected_tab == 5 && !merged_logs.empty() &&
               state.selected_log < static_cast<int>(merged_logs.size())) {
        // Log details
        lines.push_back(separator());
        auto &log = merged_logs[static_cast<size_t>(state.selected_log)];
        lines.push_back(text("Log Entry") | bold);
        lines.push_back(text("Time: " + timestamp_str(log.timestamp)));
        lines.push_back(text("Kind: " + log_kind_label(log.kind)) |
                        color(log_color(log.kind)));
        lines.push_back(text("Task: " + log.task_id));
        lines.push_back(text("Session: " + log.session_id));
        if (log.duration_ms.has_value()) {
            lines.push_back(
                text("Duration: " + std::to_string(*log.duration_ms) + "ms"));
        }
        lines.push_back(separator());
        lines.push_back(paragraph(log.text));
    }

    // Always show approval queue if non-empty
    if (!state.approvals.empty()) {
        lines.push_back(separator());
        lines.push_back(text("⏳ Pending Approvals (" +
                             std::to_string(state.approvals.size()) + ")") |
                        bold | color(Color::YellowLight));
        for (size_t i = 0; i < std::min<size_t>(state.approvals.size(), 5);
             ++i) {
            auto &a = state.approvals[i];
            lines.push_back(hbox({
                text(" " + a.action_id + " ") | bold,
                text("[" + a.risk + "] ") |
                    color(a.risk == "high"     ? Color::Red
                          : a.risk == "medium" ? Color::Yellow
                                               : Color::Green),
                text(a.title),
            }));
            if (!a.details.empty() && a.details.size() <= 80) {
                lines.push_back(text("  " + a.details) | dim);
            }
        }
        lines.push_back(text("Press 'a' to approve or /deny <id>") | dim);
    }

    lines.push_back(separator());
    lines.push_back(text("Current Action") | bold | color(Color::Yellow));
    lines.push_back(paragraph(state.current_action));

    return window(text(" Inspector "),
                  vbox(std::move(lines)) | size(WIDTH, GREATER_THAN, 32));
}

// =============================================================================
// MAIN COCKPIT LOOP
// =============================================================================

int main(int argc, char *argv[]) {
    // 1. Parse CLI Options
    auto parser = app::create_parser();
    auto result = parser.parse(argc, argv);

    switch (result.status) {
    case cli::ParseStatus::ShowHelp:
        std::cout << parser.generate_help(argv[0]);
        return 0;

    case cli::ParseStatus::ShowHelpVerbose:
        std::cout << parser.generate_help_verbose(argv[0]);
        return 0;

    case cli::ParseStatus::ShowVersion:
        std::print("CPP-LLM-CODER v0.1.0-alpha\n");
        return 0;

    case cli::ParseStatus::ShowCompletion:
        // Completion já foi tratada internamente, apenas sai
        return 0;

    case cli::ParseStatus::Error:
        std::cerr << "Erro: " << result.error_message << "\n";
        std::cerr << "Use --help para ver as opções disponíveis.\n";
        return 1;

    case cli::ParseStatus::Ok:
        break;
    }

    auto &cfg = *result.config;

    if (cfg.workdir.empty()) {
        cfg.workdir =
            std::filesystem::absolute(std::filesystem::current_path()).string();
    }

    // Build runtime defaults (tools, prompt manager, consent, logger, stats)
    const std::string log_path = ".cppllmcoder/agent.log";
    auto runtime = buildDefaultRuntime(cfg, log_path, /*echo_stdout=*/false);

    // Replace the stdin-based consent provider with a TUI-aware one
    auto cockpit_consent =
        std::make_shared<CockpitConsentProvider>(cfg.auto_approve);
    runtime.consent = cockpit_consent;

    // Prepare agent
    Agent agent(cfg, runtime.tools, runtime.prompts, runtime.consent,
                runtime.logger, runtime.stats, runtime.done_signal);

    auto auth_token = getenv_var("OPENAI_API_KEY");

    // OpenAI-compatible client (Ollama default)
    openai::OpenAI openai_client{std::string(auth_token), "", false,
                                 cfg.endpoint};

    // Disable the total request timeout for streaming — local models (Ollama)
    // can take minutes per turn. Keep a reasonable connect timeout so we
    // fail fast when the server is unreachable.
    openai_client.setTimeouts(
        std::chrono::milliseconds{10000}, // connect: 10s
        std::chrono::milliseconds{0});    // total: 0 = no limit

    // Event bus between agent threads and the TUI
    auto event_bus = std::make_shared<AgentEventBus>();
    std::mutex event_mutex;
    std::vector<AgentEvent> pending_events;
    std::atomic<bool> running{true};
    std::atomic<bool> agent_busy{false};
    // Persistent driver shared between TUI and agent thread so that
    // request_stop() / inject() can reach the active agent turn.
    std::shared_ptr<CockpitAgentDriver> active_driver;
    std::mutex driver_mutex;
    std::chrono::steady_clock::time_point last_task_refresh =
        std::chrono::steady_clock::now();
    constexpr std::chrono::milliseconds task_refresh_interval{750};
    std::chrono::steady_clock::time_point last_log_refresh =
        std::chrono::steady_clock::now();
    constexpr std::chrono::milliseconds log_refresh_interval{1000};
    std::chrono::steady_clock::time_point last_pointer_refresh =
        std::chrono::steady_clock::now();
    constexpr std::chrono::milliseconds pointer_refresh_interval{5000};

    // Initialize cockpit state with real data from the engine
    CockpitState cockpit;
    cockpit.session_id = agent.get_session_info().id;
    cockpit.workspace = cfg.workdir;
    cockpit.model = cfg.model;
    cockpit.branch = detect_git_branch();
    cockpit.mission = "Awaiting operator input...";
    cockpit.db_path = cfg.db_path;
    cockpit.mode = cfg.auto_approve ? AgentMode::Agent : AgentMode::Guide;
    cockpit.auto_approve = cfg.auto_approve;

    // Populate tools from the real ToolRegistry
    if (runtime.tools) {
        cockpit.tools = build_tool_items_from_registry(*runtime.tools);
    }

    // Wire the consent provider to post approval requests to cockpit state
    cockpit_consent->set_request_callback(
        [&](const ToolInvocationContext &ctx, const std::string &approval_id) {
            // This runs on the agent thread — post to cockpit + wake TUI
            ApprovalItem approval;
            approval.title = "Tool: " + ctx.metadata.name;
            approval.risk = ctx.metadata.is_sensitive ? "high" : "low";
            if (!ctx.metadata.danger_tags.empty()) {
                approval.risk = "medium";
            }
            approval.details = ctx.json_args.substr(
                0, std::min<size_t>(ctx.json_args.size(), 120));
            approval.action_id = approval_id;
            {
                std::lock_guard lock(event_mutex);
                cockpit.approvals.push_back(std::move(approval));
            }
            // Wake TUI so it renders the approval
            // (screen will be available by the time agent runs)
        });

    // Add initial log
    LogLine boot_log;
    boot_log.timestamp = std::chrono::system_clock::now();
    boot_log.kind = LogKind::System;
    boot_log.task_id = "T-001";
    boot_log.session_id = cockpit.session_id;
    boot_log.text = "CPP-LLM-CODER cockpit initialized";
    cockpit.logs.push_back(boot_log);

    auto start_agent_turn = [&](const std::string &user_msg) {
        if (agent_busy.load(std::memory_order_relaxed)) {
            cockpit.current_action =
                "Agent busy, wait for current turn to finish.";
            return false;
        }
        agent_busy.store(true, std::memory_order_relaxed);
        cockpit.current_action = "Running agent turn...";
        cockpit.conversation.push_back("user: " + user_msg);
        cockpit.conversation.push_back("assistant: ");

        // Create a shared driver accessible from the TUI thread for
        // stop/inject while the agent turn is running.
        auto driver = std::make_shared<CockpitAgentDriver>(event_bus, "main");
        if (cockpit.next_injection) {
            driver->inject(*cockpit.next_injection);
            cockpit.next_injection.reset();
        }
        {
            std::lock_guard lock(driver_mutex);
            active_driver = driver;
        }

        std::thread([&, user_msg, driver]() {
            try {
                agent.run_step(user_msg, *driver, openai_client);
            } catch (const std::exception &e) {
                event_bus->post(EvAgentError{"main", e.what()});
            }
            {
                std::lock_guard lock(driver_mutex);
                active_driver.reset();
            }
            agent_busy.store(false, std::memory_order_relaxed);
        }).detach();
        return true;
    };

    // Initial task tree load (if DB exists)
    try {
        auto db_tasks = load_task_tree_from_db(cockpit.db_path);
        if (!db_tasks.empty()) {
            cockpit.root_tasks = std::move(db_tasks);
            flatten_tasks(cockpit.root_tasks, cockpit.flat_tasks);
        }
    } catch (const std::exception &e) {
        LogLine log;
        log.timestamp = std::chrono::system_clock::now();
        log.kind = LogKind::Warning;
        log.task_id = "T-001";
        log.session_id = cockpit.session_id;
        log.text = std::string("Task sync failed: ") + e.what();
        cockpit.logs.push_back(std::move(log));
    }

    // Initial pointer load from DB
    try {
        cockpit.pointers = load_pointers_from_db(cockpit.db_path);
    } catch (const std::exception &e) {
        LogLine log;
        log.timestamp = std::chrono::system_clock::now();
        log.kind = LogKind::Warning;
        log.task_id = "T-001";
        log.session_id = cockpit.session_id;
        log.text = std::string("Pointer load failed: ") + e.what();
        cockpit.logs.push_back(std::move(log));
    }

    // Initial diff load
    cockpit.diff_hunks = load_diff_from_git();

    // Launch TUI
    auto screen = ScreenInteractive::Fullscreen();
    screen.TrackMouse(true);

    // Thread that drains the agent event bus and notifies the TUI
    std::thread bus_thread([&] {
        while (running.load(std::memory_order_relaxed)) {
            auto ev = event_bus->wait_next();
            {
                std::lock_guard lock(event_mutex);
                pending_events.push_back(std::move(ev));
            }
            screen.PostEvent(Event::Custom);
        }
    });

    // Periodic tick to refresh DB-backed panels even when agent is idle
    std::thread tick_thread([&] {
        while (running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            screen.PostEvent(Event::Custom);
        }
    });

    // Tab headers as tabs
    std::vector<std::string> tab_labels = cockpit.tabs;
    auto tabs_component = Toggle(&tab_labels, &cockpit.selected_tab);

    // Menu entry buffers
    std::vector<std::string> task_entries;
    std::vector<std::string> pointer_entries;
    std::vector<std::string> tool_entries;
    std::vector<std::string> diff_entries;
    std::vector<std::string> log_entries;
    std::vector<LogLine> merged_logs;

    auto clamp_index = [](int &idx, size_t size) {
        if (size == 0) {
            idx = 0;
            return;
        }
        if (idx < 0)
            idx = 0;
        if (idx >= static_cast<int>(size))
            idx = static_cast<int>(size) - 1;
    };

    auto rebuild_task_entries = [&]() {
        task_entries.clear();
        if (cockpit.flat_tasks.empty() && !cockpit.root_tasks.empty()) {
            flatten_tasks(cockpit.root_tasks, cockpit.flat_tasks);
        }
        for (const auto &t : cockpit.flat_tasks) {
            std::string label =
                indent_for_depth(t->depth) + t->title + " [" + t->status + "]";
            task_entries.push_back(label);
        }
        clamp_index(cockpit.selected_task, task_entries.size());
    };

    auto rebuild_pointer_entries = [&]() {
        pointer_entries.clear();
        for (const auto &p : cockpit.pointers) {
            pointer_entries.push_back(p.id + "  " + p.source);
        }
        clamp_index(cockpit.selected_pointer, pointer_entries.size());
    };

    auto rebuild_tool_entries = [&]() {
        tool_entries.clear();
        for (const auto &t : cockpit.tools) {
            tool_entries.push_back(t.name + " [" + t.status + "]");
        }
        clamp_index(cockpit.selected_tool, tool_entries.size());
    };

    auto rebuild_diff_entries = [&]() {
        diff_entries.clear();
        for (const auto &d : cockpit.diff_hunks) {
            diff_entries.push_back(std::string(d.staged ? "[✔] " : "[ ] ") +
                                   d.file_path);
        }
        clamp_index(cockpit.selected_diff, diff_entries.size());
    };

    auto rebuild_log_entries = [&]() {
        merged_logs = cockpit.logs;
        merged_logs.insert(merged_logs.end(), cockpit.db_logs.begin(),
                           cockpit.db_logs.end());
        std::sort(merged_logs.begin(), merged_logs.end(),
                  [](const LogLine &a, const LogLine &b) {
                      return a.timestamp < b.timestamp;
                  });
        log_entries.clear();
        for (const auto &log : merged_logs) {
            log_entries.push_back(timestamp_str(log.timestamp) + " [" +
                                  log_kind_label(log.kind) + "] " +
                                  log.task_id + " " + log.text);
        }
        clamp_index(cockpit.selected_log, log_entries.size());
    };

    rebuild_task_entries();
    rebuild_pointer_entries();
    rebuild_tool_entries();
    rebuild_diff_entries();
    rebuild_log_entries();

    // Components per tab

    // Helper: determine role prefix color for chat lines
    auto chat_line_elements = [&](const std::string &line) -> Element {
        // Detect role from prefix
        if (line.starts_with("user: ")) {
            auto label = text("user: ") | bold | color(Color::Green);
            auto body = paragraph(line.substr(6));
            return hbox({label, body});
        }
        if (line.starts_with("assistant: ")) {
            auto label = text("assistant: ") | bold | color(Color::Cyan);
            auto body = paragraph(line.substr(11));
            return hbox({label, body});
        }
        if (line.starts_with("system: ")) {
            auto label = text("system: ") | bold | color(Color::Yellow);
            auto body = paragraph(line.substr(8));
            return hbox({label, body});
        }
        if (line.starts_with("tool: ")) {
            auto label = text("tool: ") | bold | color(Color::Yellow);
            // Tools tend to produce very long output — show only a
            // preview so the chat stays readable.
            constexpr size_t kToolPreviewLen = 200;
            std::string raw = line.substr(6);
            std::string preview;
            if (raw.size() > kToolPreviewLen) {
                // Find the last newline within the preview window so we
                // don't cut mid-line.
                auto cut = raw.rfind('\n', kToolPreviewLen);
                if (cut == std::string::npos || cut == 0) {
                    cut = kToolPreviewLen;
                }
                preview = raw.substr(0, cut) + "\n  … (" +
                          std::to_string(raw.size() - cut) +
                          " chars truncated)";
            } else {
                preview = raw;
            }
            auto body = paragraph(preview) | color(Color::GrayLight);
            return hbox({label, body});
        }
        // Continuation of previous message (no prefix)
        return paragraph(line);
    };

    Component chat_tab = Renderer([&] {
        Elements lines;
        if (!cockpit.conversation.empty()) {
            for (const auto &line : cockpit.conversation) {
                lines.push_back(chat_line_elements(line));
            }
        } else {
            lines.push_back(text("No conversation yet.") |
                            color(Color::GrayDark));
        }

        // Auto-scroll indicator
        if (!cockpit.chat_auto_scroll) {
            lines.push_back(separator());
            lines.push_back(
                text(" ↓ Auto-scroll paused — press End to resume ") | dim |
                color(Color::YellowLight));
        }

        auto content = vbox(std::move(lines));

        // Position the scroll view according to the current scroll offset.
        // When auto-scroll is on the position is always pinned to the bottom.
        if (cockpit.chat_auto_scroll) {
            cockpit.chat_scroll_position = 1.0F;
        }
        content =
            content | focusPositionRelative(0, cockpit.chat_scroll_position);

        return window(text(" Chat/Plan "),
                      content | frame | vscroll_indicator | yframe | flex);
    });

    // Scroll control: PageUp/PageDown/Arrows adjust position, End resumes
    // auto-scroll, Home jumps to top.
    chat_tab = CatchEvent(chat_tab, [&](Event event) {
        if (cockpit.selected_tab != 0) {
            return false;
        }
        constexpr float kLineStep = 0.03F;  // ~3 % per arrow press
        constexpr float kPageStep = 0.25F;  // ~25 % per PageUp/Down

        if (event == Event::ArrowUp) {
            cockpit.chat_auto_scroll = false;
            cockpit.chat_scroll_position =
                std::max(0.0F, cockpit.chat_scroll_position - kLineStep);
            return true;
        }
        if (event == Event::ArrowDown) {
            cockpit.chat_scroll_position =
                std::min(1.0F, cockpit.chat_scroll_position + kLineStep);
            if (cockpit.chat_scroll_position >= 1.0F) {
                cockpit.chat_auto_scroll = true;
            }
            return true;
        }
        if (event == Event::PageUp) {
            cockpit.chat_auto_scroll = false;
            cockpit.chat_scroll_position =
                std::max(0.0F, cockpit.chat_scroll_position - kPageStep);
            return true;
        }
        if (event == Event::PageDown) {
            cockpit.chat_scroll_position =
                std::min(1.0F, cockpit.chat_scroll_position + kPageStep);
            if (cockpit.chat_scroll_position >= 1.0F) {
                cockpit.chat_auto_scroll = true;
            }
            return true;
        }
        if (event == Event::Home) {
            cockpit.chat_auto_scroll = false;
            cockpit.chat_scroll_position = 0.0F;
            return true;
        }
        if (event == Event::End) {
            cockpit.chat_auto_scroll = true;
            cockpit.chat_scroll_position = 1.0F;
            return true;
        }
        return false;
    });

    Component tasks_menu = Menu(&task_entries, &cockpit.selected_task);
    Component tasks_tab = Renderer(tasks_menu, [&] {
        if (task_entries.empty()) {
            return window(text(" Tasks "),
                          text("No tasks yet.") | color(Color::GrayDark));
        }
        return window(text(" Tasks "), tasks_menu->Render() | frame |
                                           vscroll_indicator | yframe | flex);
    });

    Component pointer_menu = Menu(&pointer_entries, &cockpit.selected_pointer);
    Component pointers_tab = Renderer(pointer_menu, [&] {
        if (pointer_entries.empty()) {
            return window(text(" Pointers "),
                          text("No pointers yet.") | color(Color::GrayDark));
        }
        return window(text(" Pointers "), pointer_menu->Render() | frame |
                                              vscroll_indicator | yframe |
                                              flex);
    });

    Component tool_menu = Menu(&tool_entries, &cockpit.selected_tool);
    Component tools_tab = Renderer(tool_menu, [&] {
        if (tool_entries.empty()) {
            return window(text(" Tools "),
                          text("No tools yet.") | color(Color::GrayDark));
        }
        return window(text(" Tools "), tool_menu->Render() | frame |
                                           vscroll_indicator | yframe | flex);
    });

    Component log_menu = Menu(&log_entries, &cockpit.selected_log);
    Component logs_tab = Renderer(log_menu, [&] {
        if (log_entries.empty()) {
            return window(text(" Logs "),
                          text("No logs yet.") | color(Color::GrayDark));
        }
        return window(text(" Logs "), log_menu->Render() | frame |
                                          vscroll_indicator | yframe | flex);
    });

    Component diff_menu = Menu(&diff_entries, &cockpit.selected_diff);
    Component diff_detail = Renderer([&] {
        if (cockpit.diff_hunks.empty()) {
            return text("No diff selected.") | color(Color::GrayDark);
        }
        const auto &h =
            cockpit.diff_hunks[static_cast<size_t>(cockpit.selected_diff)];
        Elements lines;
        for (const auto &line : h.lines | std::views::take(80)) {
            Color c = Color::White;
            if (!line.empty()) {
                if (line[0] == '+')
                    c = Color::Green;
                else if (line[0] == '-')
                    c = Color::Red;
            }
            lines.push_back(text(line) | color(c));
        }
        if (h.lines.size() > 80) {
            lines.push_back(text("...") | color(Color::GrayDark));
        }
        return vbox(std::move(lines)) | frame | yframe;
    });

    Component diff_tab = Container::Vertical({diff_menu, diff_detail});
    diff_tab = CatchEvent(diff_tab, [&](Event event) {
        if ((event == Event::Return || event == Event::Character(' ')) &&
            !cockpit.diff_hunks.empty()) {
            auto &h =
                cockpit.diff_hunks[static_cast<size_t>(cockpit.selected_diff)];
            h.staged = !h.staged;
            rebuild_diff_entries();
            return true;
        }
        return false;
    });
    diff_tab = Renderer(diff_tab, [&] {
        if (diff_entries.empty()) {
            return window(text(" Diff / Staging "),
                          text("No local changes.") | color(Color::GrayDark));
        }
        return window(
            text(" Diff / Staging "),
            vbox({
                diff_menu->Render() | frame | vscroll_indicator | yframe,
                separator(),
                diff_detail->Render() | flex,
            }) | flex);
    });

    Component review_tab = Renderer([&] {
        Elements lines;
        lines.push_back(text("Branch: " + cockpit.branch) | bold |
                        color(Color::Cyan));
        lines.push_back(text("Session: " + cockpit.session_id) | dim);
        lines.push_back(text("Workspace: " + cockpit.workspace) | dim);
        lines.push_back(separator());

        if (cockpit.diff_hunks.empty()) {
            lines.push_back(text("No local changes detected.") |
                            color(Color::GrayDark));
        } else {
            int staged_count = 0;
            int unstaged_count = 0;
            for (const auto &h : cockpit.diff_hunks) {
                if (h.staged) {
                    staged_count++;
                } else {
                    unstaged_count++;
                }
            }
            lines.push_back(
                text("Changes: " + std::to_string(cockpit.diff_hunks.size()) +
                     " files"));
            lines.push_back(text("  Staged: " + std::to_string(staged_count)) |
                            color(Color::Green));
            lines.push_back(
                text("  Unstaged: " + std::to_string(unstaged_count)) |
                color(Color::Yellow));
            lines.push_back(separator());
            for (const auto &h : cockpit.diff_hunks) {
                Color c = h.staged ? Color::Green : Color::Yellow;
                std::string prefix = h.staged ? "✔ " : "  ";
                lines.push_back(text(prefix + h.file_path + " (" +
                                     std::to_string(h.lines.size()) +
                                     " lines)") |
                                color(c));
            }
        }
        lines.push_back(separator());
        lines.push_back(text("Use /diff to reload changes from git") | dim);
        return window(text(" Review / Git "),
                      vbox(std::move(lines)) | frame | yframe | flex);
    });

    // Content tab container
    Component content_tab = Container::Tab(
        {
            chat_tab,
            tasks_tab,
            pointers_tab,
            diff_tab,
            tools_tab,
            logs_tab,
            review_tab,
        },
        &cockpit.selected_tab);

    // Input field
    Component input_component =
        Input(&cockpit.input_value, " Type command or /help ...");

    input_component = CatchEvent(input_component, [&](Event event) {
        if (event == Event::Return) {
            if (!cockpit.input_value.empty()) {
                // Log user input
                LogLine user_log;
                user_log.timestamp = std::chrono::system_clock::now();
                user_log.kind = LogKind::Agent;
                user_log.task_id = "T-001";
                user_log.session_id = cockpit.session_id;
                user_log.text = "User: " + cockpit.input_value;
                cockpit.logs.push_back(user_log);

                // Handle slash commands
                if (cockpit.input_value[0] == '/') {
                    if (cockpit.input_value == "/help") {
                        cockpit.show_help = true;
                        cockpit.current_action = "Showing help overlay";
                    } else if (cockpit.input_value == "/mode ask") {
                        cockpit.mode = AgentMode::Ask;
                    } else if (cockpit.input_value == "/mode guide") {
                        cockpit.mode = AgentMode::Guide;
                    } else if (cockpit.input_value == "/mode agent") {
                        cockpit.mode = AgentMode::Agent;
                    } else if (cockpit.input_value == "/mode yolo") {
                        cockpit.mode = AgentMode::Yolo;
                    } else if (cockpit.input_value == "/stop") {
                        // Request agent to stop via the shared driver
                        {
                            std::lock_guard lock(driver_mutex);
                            if (active_driver) {
                                active_driver->request_stop();
                            }
                        }
                        cockpit.current_action = "Stop requested";
                        LogLine slog;
                        slog.timestamp = std::chrono::system_clock::now();
                        slog.kind = LogKind::Agent;
                        slog.session_id = cockpit.session_id;
                        slog.text = "User requested agent stop";
                        cockpit.logs.push_back(std::move(slog));
                    } else if (cockpit.input_value == "/stats") {
                        std::string stats_text =
                            "Session: " + cockpit.session_id + "\n" +
                            "Model: " + cockpit.model + "\n" + "Tokens: " +
                            std::to_string(cockpit.estimated_tokens) + "\n" +
                            "Tasks: " +
                            std::to_string(cockpit.flat_tasks.size()) + "\n" +
                            "Pointers: " +
                            std::to_string(cockpit.pointers.size()) + "\n" +
                            "Tools: " + std::to_string(cockpit.tools.size()) +
                            "\n" + "Approvals pending: " +
                            std::to_string(cockpit.approvals.size()) + "\n" +
                            "Mode: " + mode_to_string(cockpit.mode);
                        cockpit.conversation.push_back("system: " + stats_text);
                        cockpit.selected_tab = 0;
                        cockpit.current_action = "Showing stats";
                    } else if (cockpit.input_value == "/diff") {
                        cockpit.diff_hunks = load_diff_from_git();
                        rebuild_diff_entries();
                        cockpit.selected_tab = 3;
                        cockpit.current_action = "Diff reloaded from git";
                    } else if (cockpit.input_value.starts_with("/inject ")) {
                        std::string payload = cockpit.input_value.substr(
                            std::string("/inject ").size());
                        // Check if payload matches a pointer ID
                        std::string injection_text;
                        bool found_pointer = false;
                        for (const auto &ptr : cockpit.pointers) {
                            if (ptr.id == payload) {
                                injection_text = "Evidence from " + ptr.source +
                                                 ": " + ptr.summary;
                                cockpit.current_action =
                                    "Injected pointer " + ptr.id;
                                found_pointer = true;
                                break;
                            }
                        }
                        if (!found_pointer) {
                            injection_text = payload;
                        }
                        // If agent is running, inject directly into the
                        // active driver so it takes effect in the current
                        // turn. Otherwise, queue for the next turn.
                        {
                            std::lock_guard lock(driver_mutex);
                            if (active_driver) {
                                active_driver->inject(injection_text);
                                cockpit.current_action =
                                    found_pointer
                                        ? "Injected pointer into active turn"
                                        : "Injected into active turn";
                            } else {
                                cockpit.next_injection = injection_text;
                                if (!found_pointer) {
                                    cockpit.current_action =
                                        "Prepared injection for next turn";
                                }
                            }
                        }
                    } else if (cockpit.input_value == "/pointers" ||
                               cockpit.input_value == "/memory") {
                        cockpit.selected_tab = 2;
                        cockpit.current_action = "Pointers tab";
                    } else if (cockpit.input_value == "/plan") {
                        cockpit.selected_tab = 0;
                        cockpit.current_action = "Chat/Plan tab";
                    } else if (cockpit.input_value == "/review") {
                        cockpit.selected_tab = 6;
                        cockpit.current_action = "Review tab (stub)";
                    } else if (cockpit.input_value == "/tools") {
                        cockpit.selected_tab = 4;
                        cockpit.current_action = "Tools tab";
                    } else if (cockpit.input_value == "/logs") {
                        cockpit.selected_tab = 5;
                        cockpit.current_action = "Logs tab";
                    } else if (cockpit.input_value == "/tasks") {
                        cockpit.selected_tab = 1;
                        cockpit.current_action = "Tasks tab";
                    } else if (cockpit.input_value.starts_with("/approve")) {
                        auto pos = cockpit.input_value.find(' ');
                        if (pos != std::string::npos) {
                            std::string action_id =
                                cockpit.input_value.substr(pos + 1);

                            // Check if it's a consent approval (tool call)
                            if (action_id.starts_with("consent-")) {
                                cockpit_consent->resolve(
                                    action_id,
                                    ToolDecision{ToolDecisionKind::Allow,
                                                 "approved via cockpit",
                                                 {}});
                                cockpit.approvals.erase(
                                    std::remove_if(cockpit.approvals.begin(),
                                                   cockpit.approvals.end(),
                                                   [&](const ApprovalItem &a) {
                                                       return a.action_id ==
                                                              action_id;
                                                   }),
                                    cockpit.approvals.end());
                                cockpit.current_action =
                                    "Tool approved: " + action_id;
                            } else {
                                // Turn approval
                                bool started = false;
                                for (const auto &p : cockpit.pending_turns) {
                                    if (p.first == action_id) {
                                        started = start_agent_turn(p.second);
                                        break;
                                    }
                                }
                                cockpit.pending_turns.erase(
                                    std::remove_if(
                                        cockpit.pending_turns.begin(),
                                        cockpit.pending_turns.end(),
                                        [&](const auto &p) {
                                            return p.first == action_id &&
                                                   started;
                                        }),
                                    cockpit.pending_turns.end());
                                cockpit.approvals.erase(
                                    std::remove_if(cockpit.approvals.begin(),
                                                   cockpit.approvals.end(),
                                                   [&](const ApprovalItem &a) {
                                                       return a.action_id ==
                                                                  action_id &&
                                                              started;
                                                   }),
                                    cockpit.approvals.end());
                                if (started) {
                                    cockpit.current_action =
                                        "Approved and running: " + action_id;
                                } else {
                                    cockpit.current_action =
                                        "Approve failed (busy or not found)";
                                }
                            }
                        }
                    } else if (cockpit.input_value.starts_with("/deny")) {
                        auto pos = cockpit.input_value.find(' ');
                        if (pos != std::string::npos) {
                            std::string action_id =
                                cockpit.input_value.substr(pos + 1);

                            // Check if it's a consent denial (tool call)
                            if (action_id.starts_with("consent-")) {
                                cockpit_consent->resolve(
                                    action_id,
                                    ToolDecision{ToolDecisionKind::Deny,
                                                 "denied via cockpit",
                                                 {}});
                            }

                            cockpit.pending_turns.erase(
                                std::remove_if(cockpit.pending_turns.begin(),
                                               cockpit.pending_turns.end(),
                                               [&](const auto &p) {
                                                   return p.first == action_id;
                                               }),
                                cockpit.pending_turns.end());
                            cockpit.approvals.erase(
                                std::remove_if(cockpit.approvals.begin(),
                                               cockpit.approvals.end(),
                                               [&](const ApprovalItem &a) {
                                                   return a.action_id ==
                                                          action_id;
                                               }),
                                cockpit.approvals.end());
                            cockpit.current_action =
                                "Denied action " + action_id;
                        }
                    }
                } else {
                    const std::string user_msg = cockpit.input_value;
                    // In Ask mode, every turn requires explicit approval.
                    // In Guide mode (and above), the operator's Enter is
                    // sufficient intent — tool consent handles risky actions
                    // separately via CockpitConsentProvider.
                    if (cockpit.mode == AgentMode::Ask) {
                        const std::string action_id =
                            "turn-" +
                            std::to_string(++cockpit.approval_counter);
                        cockpit.approvals.push_back(
                            {"Run agent turn", "medium", user_msg, action_id});
                        cockpit.pending_turns.push_back(
                            std::make_pair(action_id, user_msg));
                        cockpit.current_action =
                            "Awaiting approval: " + action_id;
                    } else {
                        start_agent_turn(user_msg);
                    }
                }

                cockpit.input_value.clear();
                cockpit.chat_auto_scroll = true; // resume on new input
                rebuild_log_entries();
                return true;
            }
        }
        return false;
    });

    Component content_with_inspector = Renderer(content_tab, [&] {
        Element main_el = content_tab->Render() | flex;
        if (cockpit.show_inspector) {
            return hbox({
                       main_el,
                       inspector_panel(cockpit, merged_logs),
                   }) |
                   flex;
        }
        return main_el;
    });

    Component root = Container::Vertical(
        {tabs_component, content_with_inspector, input_component});

    // Global renderer with status/context/help overlay
    auto root_renderer = Renderer(root, [&] {
        // Update token estimate from the real stats recorder
        if (runtime.stats) {
            cockpit.estimated_tokens =
                static_cast<int>(runtime.stats->totalTokens());
        }

        Element header = header_bar(cockpit);
        Element context = context_bar(cockpit);
        Element tabs = tabs_component->Render() | border;

        // Enhanced status line with real counters
        Color status_dot_color = agent_busy.load(std::memory_order_relaxed)
                                     ? Color::Yellow
                                     : Color::Green;
        std::string status_dot =
            agent_busy.load(std::memory_order_relaxed) ? " ◉ " : " ○ ";
        Element status_line =
            hbox({
                text(status_dot) | color(status_dot_color),
                paragraph(cockpit.current_action) | flex,
                text(" ptrs:" + std::to_string(cockpit.pointers.size())) | dim,
                text(" "),
                text(" tools:" + std::to_string(cockpit.tools.size())) | dim,
                text(" "),
                text(" tokens:" + std::to_string(cockpit.estimated_tokens)) |
                    dim,
                text(" "),
                text(" pending:" + std::to_string(cockpit.approvals.size())) |
                    color(cockpit.approvals.empty() ? Color::GrayDark
                                                    : Color::YellowLight),
                text(" "),
                text(" focus: " +
                     tab_labels[static_cast<size_t>(cockpit.selected_tab)]) |
                    dim,
            }) |
            border;
        Element input_line = hbox({
                                 text(" ❯ ") | bold | color(Color::Magenta),
                                 input_component->Render() | flex,
                             }) |
                             border;

        Element body = vbox({
            header,
            context,
            tabs,
            content_with_inspector->Render(),
            status_line,
            input_line,
        });

        if (!cockpit.show_help) {
            return body;
        }

        Elements help_lines = {
            text("── Keyboard Shortcuts ──") | bold | color(Color::Cyan),
            text("  m          Cycle mode (Ask→Guide→Agent→Yolo)"),
            text("  a          Approve next pending action"),
            text("  Ctrl+I     Toggle inspector panel"),
            text("  F1 / ?     Toggle this help"),
            text("  Esc        Close help / cancel"),
            text(""),
            text("── Navigation ──") | bold | color(Color::Cyan),
            text("  Tab/Shift+Tab    Move focus between components"),
            text("  Arrow keys       Navigate lists"),
            text("  Space/Enter      Toggle staged (Diff tab)"),
            text(""),
            text("── Slash Commands ──") | bold | color(Color::Cyan),
            text("  /help                Show this help"),
            text("  /mode <ask|guide|agent|yolo>"),
            text("  /approve <id>        Approve pending action"),
            text("  /deny <id>           Reject pending action"),
            text("  /inject <text>       Inject into next agent turn"),
            text("  /stop                Stop running agent"),
            text("  /stats               Show session statistics"),
            text("  /diff                Reload diff from git"),
            text("  /tasks               Switch to Tasks tab"),
            text("  /tools               Switch to Tools tab"),
            text("  /logs                Switch to Logs tab"),
            text("  /pointers            Switch to Pointers tab"),
            text("  /review              Switch to Review tab"),
        };
        Element help_window =
            window(text(" Help / Shortcuts "),
                   vbox(help_lines) | size(WIDTH, GREATER_THAN, 52));
        return dbox({
            body,
            help_window | center,
        });
    });

    auto app = CatchEvent(root_renderer, [&](Event event) {
        // Global help toggle first so it works even when input is focused
        if (event == Event::F1 || event == Event::Character('?')) {
            cockpit.show_help = !cockpit.show_help;
            return true;
        }
        if (event == Event::Escape && cockpit.show_help) {
            cockpit.show_help = false;
            return true;
        }

        // Forward to root (tabs/content/input) next
        if (root->OnEvent(event)) {
            return true;
        }

        if (event == Event::Custom) {
            auto push_log = [&](LogKind kind, std::string text) {
                LogLine log;
                log.timestamp = std::chrono::system_clock::now();
                log.kind = kind;
                log.task_id = "T-001";
                log.session_id = cockpit.session_id;
                log.text = std::move(text);
                cockpit.logs.push_back(std::move(log));
            };

            std::vector<AgentEvent> drained;
            {
                std::lock_guard lock(event_mutex);
                drained.swap(pending_events);
            }

            bool updated_logs = false;
            bool updated_tasks = false;

            for (auto &ev : drained) {
                std::visit(
                    overloaded{
                        [&](const EvToken &tok) {
                            if (cockpit.conversation.empty() ||
                                cockpit.conversation.back().rfind("assistant:",
                                                                  0) != 0) {
                                cockpit.conversation.push_back("assistant: ");
                            }
                            cockpit.conversation.back() += tok.text;
                            cockpit.current_action = "Streaming response...";
                        },
                        [&](const EvTurnComplete &done) {
                            if (!cockpit.conversation.empty()) {
                                cockpit.conversation.back() += "\n";
                            }
                            cockpit.current_action = "Turn complete";
                            push_log(LogKind::LLM, "Agent turn complete (" +
                                                       done.agent_id + ")");
                            updated_logs = true;
                        },
                        [&](const EvToolCall &tool) {
                            // Show tool result in conversation
                            cockpit.conversation.push_back(
                                "tool: [" + tool.tool_name + "] " +
                                (tool.success ? "✓ " : "✗ ") + tool.summary);
                            push_log(LogKind::Tool,
                                     tool.agent_id + " tool " + tool.tool_name +
                                         " success=" +
                                         (tool.success ? "true" : "false") +
                                         " " + tool.summary);
                            // Update tool status in cockpit
                            for (auto &ti : cockpit.tools) {
                                if (ti.name == tool.tool_name) {
                                    ti.status =
                                        tool.success ? "ready" : "failed";
                                    break;
                                }
                            }
                            updated_logs = true;
                        },
                        [&](const EvAgentFinished &fin) {
                            cockpit.current_action =
                                "Agent finished: " + fin.agent_id;
                            agent_busy.store(false, std::memory_order_relaxed);
                            push_log(LogKind::Agent,
                                     "Agent finished: " + fin.agent_id);
                            updated_logs = true;
                        },
                        [&](const EvAgentError &err) {
                            cockpit.current_action =
                                "Agent error: " + err.error;
                            agent_busy.store(false, std::memory_order_relaxed);
                            push_log(LogKind::Error,
                                     "Agent error: " + err.error);
                            updated_logs = true;
                        }},
                    ev);
            }

            // Periodic task refresh from DB
            const auto now = std::chrono::steady_clock::now();
            if (now - last_task_refresh >= task_refresh_interval) {
                last_task_refresh = now;
                try {
                    auto db_tasks = load_task_tree_from_db(cockpit.db_path);
                    if (!db_tasks.empty()) {
                        cockpit.root_tasks = std::move(db_tasks);
                        flatten_tasks(cockpit.root_tasks, cockpit.flat_tasks);
                        updated_tasks = true;
                    }
                } catch (const std::exception &e) {
                    push_log(LogKind::Warning,
                             std::string("Task sync failed: ") + e.what());
                    updated_logs = true;
                }
            }

            if (now - last_log_refresh >= log_refresh_interval) {
                last_log_refresh = now;
                try {
                    cockpit.db_logs =
                        load_logs_from_db(cockpit.db_path, cockpit.session_id);
                    updated_logs = true;
                } catch (const std::exception &e) {
                    push_log(LogKind::Warning,
                             std::string("Log sync failed: ") + e.what());
                    updated_logs = true;
                }
            }

            // Periodic pointer refresh from DB
            if (now - last_pointer_refresh >= pointer_refresh_interval) {
                last_pointer_refresh = now;
                try {
                    auto new_pointers = load_pointers_from_db(cockpit.db_path);
                    if (!new_pointers.empty()) {
                        cockpit.pointers = std::move(new_pointers);
                        rebuild_pointer_entries();
                    }
                } catch (const std::exception &e) {
                    push_log(LogKind::Warning,
                             std::string("Pointer sync failed: ") + e.what());
                    updated_logs = true;
                }
            }

            if (updated_tasks) {
                rebuild_task_entries();
            }
            if (updated_logs) {
                rebuild_log_entries();
            }
            return true;
        }

        if (event == Event::CtrlI) {
            cockpit.show_inspector = !cockpit.show_inspector;
            return true;
        }

        if (event == Event::Character('a')) {
            if (!cockpit.approvals.empty()) {
                auto approval = cockpit.approvals.front();

                bool handled = false;
                // Check if this is a consent (tool) approval
                if (approval.action_id.starts_with("consent-")) {
                    cockpit_consent->resolve(
                        approval.action_id,
                        ToolDecision{ToolDecisionKind::Allow,
                                     "quick-approved via 'a' key",
                                     {}});
                    cockpit.approvals.erase(cockpit.approvals.begin());
                    handled = true;

                    LogLine approval_log;
                    approval_log.timestamp = std::chrono::system_clock::now();
                    approval_log.kind = LogKind::Agent;
                    approval_log.task_id = "T-001";
                    approval_log.session_id = cockpit.session_id;
                    approval_log.text = "Tool approved: " + approval.title;
                    cockpit.logs.push_back(std::move(approval_log));
                } else {
                    // Turn approval
                    bool started = false;
                    for (const auto &p : cockpit.pending_turns) {
                        if (p.first == approval.action_id) {
                            started = start_agent_turn(p.second);
                            break;
                        }
                    }
                    if (started) {
                        cockpit.pending_turns.pop_front();
                    }
                    cockpit.approvals.erase(cockpit.approvals.begin());
                    handled = true;

                    LogLine approval_log;
                    approval_log.timestamp = std::chrono::system_clock::now();
                    approval_log.kind =
                        started ? LogKind::Agent : LogKind::Warning;
                    approval_log.task_id = "T-001";
                    approval_log.session_id = cockpit.session_id;
                    approval_log.text =
                        (started ? "Approved: " : "Skipped: ") + approval.title;
                    cockpit.logs.push_back(std::move(approval_log));
                }

                if (handled) {
                    rebuild_log_entries();
                }
            }
            return true;
        }

        if (event == Event::Character('m')) {
            switch (cockpit.mode) {
            case AgentMode::Ask:
                cockpit.mode = AgentMode::Guide;
                break;
            case AgentMode::Guide:
                cockpit.mode = AgentMode::Agent;
                break;
            case AgentMode::Agent:
                cockpit.mode = AgentMode::Yolo;
                break;
            case AgentMode::Yolo:
                cockpit.mode = AgentMode::Ask;
                break;
            }
            // Sync auto-approve to consent provider
            cockpit.auto_approve = (cockpit.mode == AgentMode::Agent ||
                                    cockpit.mode == AgentMode::Yolo);
            cockpit_consent->set_auto_approve(cockpit.auto_approve);
            cockpit.current_action =
                "Mode changed to " + mode_to_string(cockpit.mode);
            return true;
        }

        return false;
    });

    screen.Loop(app);
    running.store(false, std::memory_order_relaxed);
    event_bus->post(EvAgentFinished{"__quit", ""});
    if (bus_thread.joinable()) {
        bus_thread.join();
    }
    if (tick_thread.joinable()) {
        tick_thread.join();
    }
    return 0;
}
