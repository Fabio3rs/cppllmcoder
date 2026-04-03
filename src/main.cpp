#include "fs_tools.hpp"
#include "stdafx.hpp"

#include "agent.hpp"
#include "agent_driver.hpp"
#include "cli_options.hpp"
#include "done_task_tool.hpp"
#include "runtime_defaults.hpp"
#include "sqlite3raii.hpp"

#include "getenv.hpp"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <memory>
#include <openai/openai.hpp>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

using namespace ftxui;

// =============================================================================
// DATA MODEL FOR COCKPIT
// =============================================================================

enum class AgentMode {
    Ask,      // Read-only diagnosis
    Guide,    // Propose, seek approval
    Agent,    // Auto-execute approved steps
    Yolo,     // Maximum autonomy (sandbox)
};

enum class LogKind {
    System,   // Runtime, bootstrap
    LLM,      // Model response, streaming
    Lua,      // Lua VM execution, code blocks
    Tool,     // Tool result
    DB,       // Database query/update
    Agent,    // Agent state, task lifecycle
    MCP,      // Model Context Protocol
    FS,       // File system operation
    Warning,  // Policy check, risk flag
    Error,    // Exception, failure
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
    std::string status;  // "pending", "running", "completed", "failed"
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
    std::string source;  // "firmware.bin:0x4F00"
    double relevance_score = 0.0;  // 0.0 to 1.0
    std::vector<std::string> related_pointers;
    std::string last_updated;
};

struct ToolItem {
    std::string name;
    std::string status;     // "ready", "running", "restricted", "failed"
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
    std::vector<std::string> old_lines;
    std::vector<std::string> new_lines;
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
    int selected_tab = 0;  // 0=Chat, 1=Tasks, 2=Pointers, 3=Diff, 4=Tools, 5=Logs, 6=Review
    int selected_task = 0;
    int selected_pointer = 0;
    int selected_tool = 0;
    int selected_log = 0;
    unsigned int selected_approval = 0;
    bool show_inspector = true;
    bool show_help = false;
    
    // Dynamic state
    std::string input_value;
    std::string current_action = "Initializing...";
    std::string mission;
    
    std::vector<std::string> tabs = {
        "Chat/Plan",
        "Tasks",
        "Pointers",
        "Diff",
        "Tools",
        "Logs",
        "Review",
    };
    
    std::vector<std::shared_ptr<TaskNode>> root_tasks;
    std::vector<PointerItem> pointers;
    std::vector<ToolItem> tools;
    std::vector<LogLine> logs;
    std::vector<ApprovalItem> approvals;
    std::vector<DiffHunk> diff_hunks;
    std::vector<std::string> conversation;
    
    // Metadata
    int total_subagents = 0;
    int estimated_tokens = 0;
    double estimated_cost = 0.0;
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

Element inspector_panel(const CockpitState &state) {
    Elements lines;
    
    lines.push_back(text("Inspector") | bold | color(Color::Cyan));
    
    if (state.selected_tab == 1 && !state.root_tasks.empty()) {
        // Task details
        lines.push_back(separator());
        auto &task = state.root_tasks[static_cast<size_t>(state.selected_task)];
        lines.push_back(text("Task: " + task->id) | bold);
        lines.push_back(text("Status: " + task->status) |
                        color(status_color(task->status)));
        lines.push_back(text("Owner: " + task->owner));
        lines.push_back(
            text("Duration: " + std::to_string(task->duration.count()) + "ms"));
    } else if (state.selected_tab == 2 && !state.pointers.empty()) {
        // Pointer details
        lines.push_back(separator());
        auto &ptr = state.pointers[static_cast<size_t>(state.selected_pointer)];
        lines.push_back(text("Pointer: " + ptr.id) | bold | color(Color::Yellow));
        lines.push_back(text("Score: " + std::to_string(ptr.relevance_score)));
        lines.push_back(text("Source: " + ptr.source));
        lines.push_back(paragraph(ptr.summary));
        if (!ptr.related_pointers.empty()) {
            lines.push_back(text("Related:") | dim);
            for (const auto &rp : ptr.related_pointers) {
                lines.push_back(text("  → " + rp));
            }
        }
    } else if (state.selected_tab == 4 && !state.tools.empty()) {
        // Tool details
        lines.push_back(separator());
        auto &tool = state.tools[static_cast<size_t>(state.selected_tool)];
        lines.push_back(text("Tool: " + tool.name) | bold);
        lines.push_back(text("Status: " + tool.status) |
                        color(status_color(tool.status)));
        lines.push_back(text("Risk: " + tool.risk_level));
        lines.push_back(text("Latency: " + std::to_string(tool.avg_latency_ms) +
                             "ms"));
        lines.push_back(paragraph(tool.description));
    }
    
    lines.push_back(separator());
    lines.push_back(text("Current Action") | bold | color(Color::Yellow));
    lines.push_back(paragraph(state.current_action));
    
    return window(text(" Inspector "),
                  vbox(std::move(lines)) | size(WIDTH, GREATER_THAN, 32));
}

// =============================================================================
// OLD CODE (commented, kept for reference)
// =============================================================================

/*static int dry_run() {
    LuaContext engine;

    // Simulando o que o Qwen/Claude enviaria dentro da tag <code>
    std::string mock_llm_code = R"lua(
        -- Demo mínimo sem bindings extras (fs/db/agent não estão expostos aqui)
        local sum = 0
        for i = 1, 5 do
            sum = sum + i
        end
        return "Lua demo ok; sum=" .. sum
    )lua";

    auto res = engine.execute(mock_llm_code);

    if (res) {
        std::cout << "Resultado: " << *res << std::endl;
    } else {
        std::cerr << "Erro: " << res.error() << std::endl;
    }

    return 0;
}

static nlohmann::json::array_t
docs_from_id_vec(const std::span<const std::pair<int, std::string>> vec) {
    nlohmann::json::array_t arr;
    arr.reserve(vec.size());
    for (const auto &[id, content] : vec) {
        arr.push_back(content);
    }
    return arr;
}

static void test_sqlitevec_embd() {
    auto &llmConnection =
        openai::start("ollama", "", true, "http://localhost:11434/v1/");

    sqlite3_db_ptr db;
    check_sqlite_rc(db, sqlite3_open(":memory:", std::out_ptr(db)),
                    "sqlite3_open");

    std::string embedding_model = "qwen3-embedding:8b";

    try {
        check_sqlite_rc(db, sqlite3_enable_load_extension(db.get(), 1),
                        "enable load_extension");

        sqlite3_errmsg_ptr errmsg = nullptr;
        const std::string vec0_path = exe_path_utils::get_vec_extension_path();

        const int load_rc = sqlite3_load_extension(
            db.get(), vec0_path.c_str(), nullptr, std::out_ptr(errmsg));
        if (load_rc != SQLITE_OK) {
            std::string msg = "sqlite3_load_extension: ";
            msg += (errmsg ? errmsg.get() : "unknown error");
            throw std::runtime_error(msg);
        }

        check_sqlite_rc(db, sqlite3_enable_load_extension(db.get(), 0),
                        "disable load_extension");

        exec_or_throw(db, R"sql(
            create table docs(
                id integer primary key,
                content text not null
            );
        )sql");

        exec_or_throw(db, R"sql(
            create virtual table vec_docs using vec0(
                id integer primary key,
                embedding float[4096]
            );
        )sql");

        const std::vector<std::pair<int, std::string>> docs = {
            {1, "C++ templates, concepts and constexpr metaprogramming"},
            {2, "Lua bindings for C++ with sol2 and userdata exposure"},
            {3, "SQLite vector search with embeddings and semantic retrieval"},
            {4, "Rust ownership, borrowing and lifetimes"}};

        auto insert_doc_stmt = prepare_or_throw(db,
                                                R"sql(
                insert into docs(id, content) values(?, ?);
            )sql",
                                                "prepare insert docs");

        auto insert_vec_stmt = prepare_or_throw(db,
                                                R"sql(
                insert into vec_docs(id, embedding) values(?, ?);
            )sql",
                                                "prepare insert vec_docs");

        auto search_stmt = prepare_or_throw(db,
                                            R"sql(
                select d.id, d.content, v.distance
                from vec_docs as v
                join docs as d on d.id = v.id
                where v.embedding match ?
                  and k = 3
                order by v.distance asc;
            )sql",
                                            "prepare search");

        auto embeds = embedding_utils::embed_texts(
            llmConnection, embedding_model, docs_from_id_vec(docs));

        for (const auto &[doc, emb] : std::ranges::zip_view(docs, embeds)) {
            auto &[id, text] = doc;
            std::println("Embedding doc id={} text={}", id, text);

            if (emb.size() != 4096) {
                throw std::runtime_error(
                    "unexpected embedding dimension; expected 4096");
            }

            const auto emb_json = embedding_utils::to_json_array(emb);

            check_sqlite_rc(db, sqlite3_bind_int(insert_doc_stmt.get(), 1, id),
                            "bind doc id");
            check_sqlite_rc(db,
                            sqlite3_bind_text(insert_doc_stmt.get(), 2,
                                              text.c_str(), -1,
                                              SQLITE_TRANSIENT),
                            "bind doc content");
            check_sqlite_rc(db, sqlite3_step(insert_doc_stmt.get()),
                            "step insert docs");
            check_sqlite_rc(db, sqlite3_reset(insert_doc_stmt.get()),
                            "reset insert docs");
            check_sqlite_rc(db, sqlite3_clear_bindings(insert_doc_stmt.get()),
                            "clear insert docs");

            check_sqlite_rc(db, sqlite3_bind_int(insert_vec_stmt.get(), 1, id),
                            "bind vec id");
            check_sqlite_rc(db,
                            sqlite3_bind_text(insert_vec_stmt.get(), 2,
                                              emb_json.c_str(), -1,
                                              SQLITE_TRANSIENT),
                            "bind vec embedding");
            check_sqlite_rc(db, sqlite3_step(insert_vec_stmt.get()),
                            "step insert vec_docs");
            check_sqlite_rc(db, sqlite3_reset(insert_vec_stmt.get()),
                            "reset insert vec_docs");
            check_sqlite_rc(db, sqlite3_clear_bindings(insert_vec_stmt.get()),
                            "clear insert vec_docs");
        }

        const std::string query =
            "How to integrate Lua into a modern C++ project?";
        std::println("\nQuery: {}", query);

        const auto query_emb =
            embedding_utils::embed_text(llmConnection, embedding_model, query);
        if (query_emb.size() != 4096) {
            throw std::runtime_error(
                "unexpected query embedding dimension; expected 4096");
        }

        const auto query_json = embedding_utils::to_json_array(query_emb);

        check_sqlite_rc(db,
                        sqlite3_bind_text(search_stmt.get(), 1,
                                          query_json.c_str(), -1,
                                          SQLITE_TRANSIENT),
                        "bind search query");

        std::println("\nTop matches:");
        while (true) {
            const int rc = sqlite3_step(search_stmt.get());
            if (rc == SQLITE_DONE) {
                break;
            }
            check_sqlite_rc(db, rc, "step search");

            const int id = sqlite3_column_int(search_stmt.get(), 0);
            const char *content = reinterpret_cast<const char *>(
                sqlite3_column_text(search_stmt.get(), 1));
            const double distance = sqlite3_column_double(search_stmt.get(), 2);

            std::println("id={} distance={} text={}", id, distance,
                         content != nullptr ? content : "");
        }

    } catch (...) {
        throw;
    }
}

int main() {
    dry_run();
    test_sqlitevec_embd();
}*/

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
    auto runtime = buildDefaultRuntime(cfg, log_path, /*echo_stdout=*/true);

    // Show available tools in the prompt for debugging
    auto docs = runtime.tools->topKDocs("", 16);

    // Prepare agent
    Agent agent(cfg, runtime.tools, runtime.prompts, runtime.consent,
                runtime.logger, runtime.stats, runtime.done_signal);
    DefaultPromptManager prompt_manager_preview;

    auto prompt = prompt_manager_preview.buildSystemPrompt(agent);
    std::println("Prompt preview:\n{}\n", prompt);

    // Simple driver that echoes streamed tokens and tool results to stdout
    class StdIODriver : public IAgentDriver {
      public:
        void on_token(std::string_view token) override {
            std::cout << token << std::flush;
        }
        void on_turn_complete(std::string_view response) override {
            std::cout << "\n[complete]\n" << response << "\n";
        }
        void on_tool_result(std::string_view tool_name, bool success,
                            std::string_view summary) override {
            std::cout << "\n[tool] " << tool_name
                      << " success=" << (success ? "true" : "false")
                      << " summary=" << summary << "\n";
        }
        bool stop_requested() const override {
            return stop_.load(std::memory_order_relaxed);
        }
        void request_stop() override {
            stop_.store(true, std::memory_order_relaxed);
        }
        std::optional<std::string> next_injection() override { return {}; }
        void inject(std::string) override {}
        std::optional<std::chrono::milliseconds> timeout() const override {
            return std::nullopt;
        }
        bool should_finish(int) const override { return false; }

      private:
        std::atomic<bool> stop_{false};
    };

    StdIODriver driver;

    auto auth_token = getenv_var("OPENAI_API_KEY");

    // OpenAI-compatible client (Ollama default)
    openai::OpenAI openai_client{std::string(auth_token), "", false,
                                 cfg.endpoint};

    // Initialize cockpit state
    CockpitState cockpit;
    cockpit.session_id = "session_" + std::to_string(time(nullptr));
    cockpit.workspace = cfg.workdir;
    cockpit.model = cfg.model;
    cockpit.mission = "Awaiting operator input...";
    cockpit.db_path = ".cppllmcoder/brain.db";
    
    // Bootstrap sample tools
    cockpit.tools = {
        {"fs.read", "ready", 3, "Read file contents from workspace", "low", 0},
        {"vector.search", "ready", 14, "Search semantic memory pointers",
         "low", 0},
        {"rlm.spawn", "ready", 7, "Spawn isolated sub-agent", "medium", 0},
        {"db.query", "ready", 4, "Query structured agent memory", "low", 0},
        {"sh", "restricted", 24, "Sandboxed shell command execution",
         "high", 0},
    };

    // Bootstrap sample task tree
    auto root_task = std::make_shared<TaskNode>();
    root_task->id = "T-001";
    root_task->title = "Root: Analyze workspace";
    root_task->status = "running";
    root_task->owner = "main";
    root_task->summary = "Coordinator initialized";
    cockpit.root_tasks.push_back(root_task);

    // Add initial log
    LogLine boot_log;
    boot_log.timestamp = std::chrono::system_clock::now();
    boot_log.kind = LogKind::System;
    boot_log.task_id = "T-001";
    boot_log.session_id = cockpit.session_id;
    boot_log.text = "CPP-LLM-CODER cockpit initialized";
    cockpit.logs.push_back(boot_log);

    // Launch TUI
    auto screen = ScreenInteractive::Fullscreen();
    screen.TrackMouse(true);

    // Tab headers as tabs
    std::vector<std::string> tab_labels = cockpit.tabs;

    auto tabs_component = Toggle(&tab_labels, &cockpit.selected_tab);

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
                    // TODO: parse slash commands
                    if (cockpit.input_value == "/help") {
                        cockpit.current_action = "Showing help...";
                    } else if (cockpit.input_value == "/mode ask") {
                        cockpit.mode = AgentMode::Ask;
                    } else if (cockpit.input_value == "/mode guide") {
                        cockpit.mode = AgentMode::Guide;
                    } else if (cockpit.input_value == "/mode agent") {
                        cockpit.mode = AgentMode::Agent;
                    } else if (cockpit.input_value == "/mode yolo") {
                        cockpit.mode = AgentMode::Yolo;
                    }
                } else {
                    // Normal user input to agent
                    cockpit.current_action = "Processing user input...";
                    // TODO: call agent.run_step() and update cockpit state
                }

                cockpit.input_value.clear();
                return true;
            }
        }
        return false;
    });

    // Main renderer
    auto main_renderer = Renderer([&] {
        Elements tab_content;

        // Render selected tab
        if (cockpit.selected_tab == 0) {
            // Chat/Plan
            tab_content.push_back(text("Chat/Plan") | bold);
            tab_content.push_back(separator());
            if (!cockpit.conversation.empty()) {
                for (const auto &line : cockpit.conversation) {
                    tab_content.push_back(paragraph(line));
                }
            } else {
                tab_content.push_back(
                    text("No conversation yet.") | color(Color::GrayDark));
            }
        } else if (cockpit.selected_tab == 1) {
            // Tasks tree
            tab_content.push_back(text("Task Tree") | bold);
            tab_content.push_back(separator());
            for (const auto &task : cockpit.root_tasks) {
                tab_content.push_back(
                    hbox({
                        text(task->id + " "),
                        text(task->title) | flex,
                        text(task->status) | color(status_color(task->status)),
                    }));
            }
        } else if (cockpit.selected_tab == 2) {
            // Pointers
            tab_content.push_back(text("Pointers/Memory") | bold);
            tab_content.push_back(separator());
            if (!cockpit.pointers.empty()) {
                for (const auto &ptr : cockpit.pointers) {
                    tab_content.push_back(
                        hbox({
                            text(ptr.id + " ") | bold | color(Color::Yellow),
                            text(ptr.source) | dim,
                        }));
                    tab_content.push_back(paragraph(ptr.summary));
                }
            } else {
                tab_content.push_back(
                    text("No pointers yet.") | color(Color::GrayDark));
            }
        } else if (cockpit.selected_tab == 4) {
            // Tools
            tab_content.push_back(text("Tools") | bold);
            tab_content.push_back(separator());
            for (const auto &tool : cockpit.tools) {
                tab_content.push_back(
                    hbox({
                        text(tool.name + " "),
                        text(tool.status) | color(status_color(tool.status)),
                        filler(),
                        text(std::to_string(tool.avg_latency_ms) + "ms") | dim,
                    }));
            }
        } else if (cockpit.selected_tab == 5) {
            // Logs
            tab_content.push_back(text("Execution Logs") | bold);
            tab_content.push_back(separator());
            for (const auto &log : cockpit.logs) {
                std::string log_line =
                    timestamp_str(log.timestamp) + " [" +
                    log_kind_label(log.kind) + "] " + log.task_id + " " +
                    log.text;
                tab_content.push_back(text(log_line) | color(log_color(log.kind)));
            }
        }

        Element left_panel =
            window(text(" Main "),
                   vbox(std::move(tab_content)) | yframe | flex);

        Element right_panel =
            cockpit.show_inspector ? inspector_panel(cockpit) : text("");

        Element center = right_panel.get() != nullptr
                             ? hbox({
                                   left_panel,
                                   right_panel,
                               }) |
                                   flex
                             : left_panel;

        Element header = header_bar(cockpit);
        Element context = context_bar(cockpit);
        Element tabs = tabs_component->Render() | border;

        Element status_line = hbox({
                                  text(" ◉ ") | color(Color::Green),
                                  paragraph(cockpit.current_action) | flex,
                              }) |
                              border;

        Element input_line = hbox({
                                text(" ❯ ") | bold | color(Color::Magenta),
                                input_component->Render() | flex,
                            }) |
                            border;

        return vbox({
            header,
            context,
            tabs,
            center,
            status_line,
            input_line,
        });
    });

    auto app = CatchEvent(main_renderer, [&](Event event) {
        if (event == Event::Tab) {
            cockpit.selected_tab =
                (cockpit.selected_tab + 1) %
                static_cast<int>(cockpit.tabs.size());
            return true;
        }

        if (event == Event::TabReverse) {
            cockpit.selected_tab =
                (cockpit.selected_tab - 1 +
                 static_cast<int>(cockpit.tabs.size())) %
                static_cast<int>(cockpit.tabs.size());
            return true;
        }

        if (event == Event::F1) {
            cockpit.show_help = !cockpit.show_help;
            return true;
        }

        if (event == Event::CtrlI) {
            cockpit.show_inspector = !cockpit.show_inspector;
            return true;
        }

        if (event == Event::Character('a')) {
            if (!cockpit.approvals.empty()) {
                LogLine approval_log;
                approval_log.timestamp = std::chrono::system_clock::now();
                approval_log.kind = LogKind::Agent;
                approval_log.task_id = "T-001";
                approval_log.session_id = cockpit.session_id;
                approval_log.text =
                    "Approved: " + cockpit.approvals.front().title;
                cockpit.logs.push_back(approval_log);
                cockpit.approvals.erase(cockpit.approvals.begin());
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
            cockpit.current_action =
                "Mode changed to " + mode_to_string(cockpit.mode);
            return true;
        }

        return false;
    });

    screen.Loop(app);
    return 0;
}
