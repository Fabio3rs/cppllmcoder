#include "cockpit_runtime.hpp"

#include "fs_tools.hpp"
#include "sqlite3raii.hpp"
#include "utils/utf8_text.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <format>
#include <functional>
#include <ranges>
#include <stdexcept>
#include <unordered_map>

using namespace ftxui;

std::string modeToString(AgentMode mode) {
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

Color modeToColor(AgentMode mode) {
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

Color statusColor(const std::string &status) {
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

Color logColor(LogKind kind) {
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

std::string logKindLabel(LogKind kind) {
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

std::string timestampStr(const std::chrono::system_clock::time_point &tp) {
    auto time = std::chrono::system_clock::to_time_t(tp);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  tp.time_since_epoch()) %
              1000;
    struct tm *tm_info = std::localtime(&time);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", tm_info);
    return std::string(buf) + "." + std::to_string(ms.count());
}

std::string indentForDepth(int depth) {
    return std::string(static_cast<size_t>(depth) * 2, ' ');
}

std::string detectGitBranch() {
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
    while (!result.empty() &&
           (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }
    return result.empty() ? "(detached)" : result;
}

std::chrono::system_clock::time_point
parseSqliteTimestamp(std::string_view ts) {
    constexpr size_t base_len = 19;
    auto now = std::chrono::system_clock::now();
    auto parse_int = [](std::string_view v) -> std::optional<int> {
        int value = 0;
        auto res = std::from_chars(v.data(), v.data() + v.size(), value);
        if (res.ec != std::errc{}) {
            return std::nullopt;
        }
        return value;
    };
    if (ts.size() < base_len) {
        return now;
    }
    if (!(ts[4] == '-' && ts[7] == '-' && ts[10] == ' ' && ts[13] == ':' &&
          ts[16] == ':')) {
        return now;
    }
    auto year = parse_int(ts.substr(0, 4));
    auto month = parse_int(ts.substr(5, 2));
    auto day = parse_int(ts.substr(8, 2));
    auto hour = parse_int(ts.substr(11, 2));
    auto minute = parse_int(ts.substr(14, 2));
    auto second = parse_int(ts.substr(17, 2));
    if (!year || !month || !day || !hour || !minute || !second) {
        return now;
    }
    std::tm tm{};
    tm.tm_year = *year - 1900;
    tm.tm_mon = *month - 1;
    tm.tm_mday = *day;
    tm.tm_hour = *hour;
    tm.tm_min = *minute;
    tm.tm_sec = *second;
    auto time_c = std::chrono::system_clock::from_time_t(std::mktime(&tm));
    if (ts.size() > base_len && ts[base_len] == '.') {
        const size_t frac_start = base_len + 1;
        const size_t frac_len = std::min<size_t>(3, ts.size() - frac_start);
        auto frac = ts.substr(frac_start, frac_len);
        int ms = 0;
        if (auto parsed = parse_int(frac)) {
            ms = *parsed;
            if (frac_len == 1) {
                ms *= 100;
            } else if (frac_len == 2) {
                ms *= 10;
            }
        }
        time_c += std::chrono::milliseconds(ms);
    }
    return time_c;
}

void flattenTasks(const std::vector<std::shared_ptr<TaskNode>> &roots,
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

void printSessionTable(const std::vector<SessionSummary> &sessions) {
    if (sessions.empty()) {
        std::print("no sessions found\n");
        return;
    }
    std::print("{:<38}{:<24}{:<18}{}\n", "id", "updated_at", "model",
               "messages");
    for (const auto &s : sessions) {
        std::print("{:<38}{:<24}{:<18}{}\n", s.id, s.updated_at, s.model,
                   s.message_count);
    }
}

std::vector<std::shared_ptr<TaskNode>>
loadTaskTreeFromDb(const std::string &db_path) {
    namespace fs = std::filesystem;
    if (!fs::exists(db_path)) {
        return {};
    }
    SqliteDb db(db_path);
    db.exec("PRAGMA foreign_keys = ON;");
    auto stmt = db.prepare(R"sql(
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
        if (rc == SQLITE_DONE)
            break;
        if (rc != SQLITE_ROW)
            throw std::runtime_error("failed to step tasks query");
        auto cstr = [&](int idx) -> std::string {
            const unsigned char *txt = sqlite3_column_text(stmt.get(), idx);
            return txt ? reinterpret_cast<const char *>(txt) : "";
        };
        rows.push_back(Row{.id = cstr(0),
                           .parent = cstr(1),
                           .desc = cstr(2),
                           .status = cstr(3)});
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

std::vector<PointerItem> loadPointersFromDb(const std::string &db_path) {
    namespace fs = std::filesystem;
    if (!fs::exists(db_path)) {
        return {};
    }
    SqliteDb db(db_path);
    db.exec("PRAGMA foreign_keys = ON;");
    auto stmt = db.prepare(R"sql(
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
        if (rc == SQLITE_DONE)
            break;
        if (rc != SQLITE_ROW)
            break;
        auto cstr = [&](int idx) -> std::string {
            const unsigned char *txt = sqlite3_column_text(stmt.get(), idx);
            return txt ? reinterpret_cast<const char *>(txt) : "";
        };
        PointerItem ptr;
        ptr.id = cstr(0);
        ptr.summary = cstr(1);
        std::string file_path = cstr(2);
        int offset_start = sqlite3_column_int(stmt.get(), 3);
        ptr.source = std::format("{}:0x{:X}", file_path, offset_start);
        ptr.last_updated = cstr(5);
        pointers.push_back(std::move(ptr));
    }
    if (!pointers.empty()) {
        try {
            auto kg_stmt = db.prepare(R"sql(
                SELECT source_pointer_id, target_pointer_id, relationship_type
                FROM knowledge_graph
                ORDER BY created_at DESC
                LIMIT 500;
            )sql",
                                      "select knowledge_graph");
            std::unordered_map<std::string, std::vector<std::string>> rels;
            while (true) {
                const int rc = kg_stmt.step();
                if (rc == SQLITE_DONE)
                    break;
                if (rc != SQLITE_ROW)
                    break;
                auto cstr = [&](int idx) -> std::string {
                    const unsigned char *txt =
                        sqlite3_column_text(kg_stmt.get(), idx);
                    return txt ? reinterpret_cast<const char *>(txt) : "";
                };
                rels[cstr(0)].push_back(cstr(2) + " → " + cstr(1));
            }
            for (auto &ptr : pointers) {
                auto it = rels.find(ptr.id);
                if (it != rels.end()) {
                    ptr.related_pointers = it->second;
                }
            }
        } catch (...) {
        }
    }
    return pointers;
}

std::vector<ToolItem> buildToolItemsFromRegistry(const ToolRegistry &registry) {
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
        items.push_back(std::move(item));
    });
    return items;
}

std::vector<DiffHunk> loadDiffFromGit() {
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
    DiffHunk current;
    bool in_hunk = false;
    std::string_view content{output};
    size_t pos = 0;
    auto push_current = [&]() {
        if (in_hunk && !current.file_path.empty()) {
            hunks.push_back(current);
        }
    };
    while (pos <= content.size()) {
        size_t next = content.find('\n', pos);
        std::string_view line_view = next == std::string_view::npos
                                         ? content.substr(pos)
                                         : content.substr(pos, next - pos);
        std::string line{line_view};
        if (line.starts_with("diff --git ")) {
            push_current();
            current = DiffHunk{};
            in_hunk = true;
            auto pos = line.find(" b/");
            current.file_path = pos != std::string::npos ? line.substr(pos + 3)
                                                         : line.substr(11);
        } else if (line.starts_with("@@")) {
            current.lines.push_back(line);
        } else if (in_hunk) {
            current.lines.push_back(line);
        }
        if (next == std::string_view::npos)
            break;
        pos = next + 1;
    }
    push_current();
    return hunks;
}

std::vector<LogLine> loadLogsFromDb(const std::string &db_path,
                                    const std::string &session_id, int limit) {
    namespace fs = std::filesystem;
    if (!fs::exists(db_path)) {
        return {};
    }
    SqliteDb db(db_path);
    db.exec("PRAGMA foreign_keys = ON;");
    auto stmt = db.prepare(R"sql(
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
        if (rc == SQLITE_DONE)
            break;
        if (rc != SQLITE_ROW)
            throw std::runtime_error("failed to step messages query");
        auto cstr = [&](int idx) -> std::string {
            const unsigned char *txt = sqlite3_column_text(stmt.get(), idx);
            return txt ? reinterpret_cast<const char *>(txt) : "";
        };
        LogLine log;
        log.timestamp = parseSqliteTimestamp(cstr(0));
        log.task_id = cstr(1);
        log.session_id = session_id;
        log.kind = LogKind::LLM;
        log.text = cstr(2) + ": " + cstr(3);
        int duration = sqlite3_column_int(stmt.get(), 4);
        if (duration > 0)
            log.duration_ms = duration;
        logs.push_back(std::move(log));
    }
    auto tool_stmt = db.prepare(R"sql(
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
        if (rc == SQLITE_DONE)
            break;
        if (rc != SQLITE_ROW)
            throw std::runtime_error("failed to step tool_invocations");
        auto cstr = [&](int idx) -> std::string {
            const unsigned char *txt =
                sqlite3_column_text(tool_stmt.get(), idx);
            return txt ? reinterpret_cast<const char *>(txt) : "";
        };
        LogLine log;
        log.timestamp = parseSqliteTimestamp(cstr(0));
        log.task_id = cstr(1);
        log.session_id = session_id;
        log.kind = LogKind::Tool;
        log.text = cstr(2) + " (" + cstr(3) + "): " + cstr(4);
        int duration = sqlite3_column_int(tool_stmt.get(), 5);
        if (duration > 0)
            log.duration_ms = duration;
        logs.push_back(std::move(log));
    }
    auto exec_stmt = db.prepare(R"sql(
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
        if (rc == SQLITE_DONE)
            break;
        if (rc != SQLITE_ROW)
            throw std::runtime_error("failed to step execution_logs");
        auto cstr = [&](int idx) -> std::string {
            const unsigned char *txt =
                sqlite3_column_text(exec_stmt.get(), idx);
            return txt ? reinterpret_cast<const char *>(txt) : "";
        };
        LogLine log;
        log.timestamp = parseSqliteTimestamp(cstr(0));
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
