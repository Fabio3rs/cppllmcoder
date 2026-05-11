#include "cockpit_ui.hpp"

#include "cockpit_agent_driver.hpp"
#include "getenv.hpp"
#include "utils/utf8_text.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <format>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

using namespace ftxui;

template <class... Ts> struct overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

static Element headerBar(const CockpitState &state) {
    return hbox(Elements{text("   CPP-LLM-CODER ") | bold | color(Color::Cyan),
                         text(" mission: ") | dim, text(state.mission) | flex,
                         text(" mode ") | dim,
                         text(" " + modeToString(state.mode) + " ") | bold |
                             color(modeToColor(state.mode)),
                         text(" ")}) |
           border;
}

static Element contextBar(const CockpitState &state) {
    return hbox(Elements{
               text(" model: " + state.model + " ") | color(Color::Green),
               separatorEmpty(), text(" ws: " + state.workspace + " ") | dim,
               separatorEmpty(), text(" branch: " + state.branch + " ") | dim,
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
                   color(state.auto_approve ? Color::Red : Color::Yellow)}) |
           border;
}

static Element inspectorPanel(const CockpitState &state,
                              const std::vector<LogLine> &merged_logs) {
    Elements lines;
    lines.push_back(text("Inspector") | bold | color(Color::Cyan));

    if (state.selected_tab == 1 && !state.flat_tasks.empty() &&
        state.selected_task < static_cast<int>(state.flat_tasks.size())) {
        auto &task = state.flat_tasks[static_cast<size_t>(state.selected_task)];
        lines.push_back(separator());
        lines.push_back(text("Task: " + task->id) | bold);
        lines.push_back(text("Title: " + task->title));
        lines.push_back(text("Status: " + task->status) |
                        color(statusColor(task->status)));
        lines.push_back(text("Owner: " + task->owner));
        lines.push_back(
            text("Children: " + std::to_string(task->children.size())));
        if (!task->summary.empty()) {
            lines.push_back(separator());
            lines.push_back(paragraph(task->summary));
        }
    } else if (state.selected_tab == 2 && !state.pointers.empty() &&
               state.selected_pointer <
                   static_cast<int>(state.pointers.size())) {
        auto &ptr = state.pointers[static_cast<size_t>(state.selected_pointer)];
        lines.push_back(separator());
        lines.push_back(text("Pointer: " + ptr.id) | bold |
                        color(Color::Yellow));
        lines.push_back(text("Source: " + ptr.source));
        lines.push_back(paragraph(ptr.summary));
    } else if (state.selected_tab == 4 && !state.tools.empty() &&
               state.selected_tool < static_cast<int>(state.tools.size())) {
        auto &tool = state.tools[static_cast<size_t>(state.selected_tool)];
        lines.push_back(separator());
        lines.push_back(text("Tool: " + tool.name) | bold);
        lines.push_back(text("Status: " + tool.status) |
                        color(statusColor(tool.status)));
        lines.push_back(paragraph(tool.description));
    } else if (state.selected_tab == 5 && !merged_logs.empty() &&
               state.selected_log < static_cast<int>(merged_logs.size())) {
        auto &log = merged_logs[static_cast<size_t>(state.selected_log)];
        lines.push_back(separator());
        lines.push_back(text("Log Entry") | bold);
        lines.push_back(text("Kind: " + logKindLabel(log.kind)) |
                        color(logColor(log.kind)));
        lines.push_back(paragraph(log.text));
    }

    if (!state.approvals.empty()) {
        lines.push_back(separator());
        lines.push_back(text("Pending Approvals (" +
                             std::to_string(state.approvals.size()) + ")") |
                        bold | color(Color::YellowLight));
        for (size_t i = 0; i < std::min<size_t>(state.approvals.size(), 5);
             ++i) {
            const auto &a = state.approvals[i];
            lines.push_back(text(" " + a.action_id + " " + a.title));
        }
    }

    lines.push_back(separator());
    lines.push_back(text("Current Action") | bold | color(Color::Yellow));
    lines.push_back(paragraph(state.current_action));
    return window(text(" Inspector "),
                  vbox(std::move(lines)) | size(WIDTH, GREATER_THAN, 32));
}

static std::string chatRoleLabel(ChatRole role) {
    switch (role) {
    case ChatRole::User:
        return "user";
    case ChatRole::Assistant:
        return "assistant";
    case ChatRole::System:
        return "system";
    case ChatRole::Tool:
        return "tool";
    }
    return "";
}

static Element renderChatItem(const ChatItem &item, bool focused) {
    auto role_label = text(chatRoleLabel(item.role) + ": ") | bold |
                      color(item.role == ChatRole::User        ? Color::Green
                            : item.role == ChatRole::Assistant ? Color::Cyan
                            : item.role == ChatRole::System    ? Color::Yellow
                                                               : Color::Yellow);
    Element out;
    if (!item.collapsible) {
        out = hbox({role_label, paragraph(item.text) | flex});
    } else {
        std::string header_text =
            item.title.empty() ? (std::to_string(item.text.size()) + " bytes")
                               : item.title;
        std::string marker = item.expanded ? "[-] " : "[+] ";
        Element body =
            item.expanded
                ? paragraph(item.text) | color(Color::GrayLight)
                : paragraph(std::string(utf8::prefix_by_bytes(
                      item.text, static_cast<size_t>(item.preview_len)))) |
                      color(Color::GrayDark);
        out = vbox({hbox({text(marker) | bold | color(Color::Magenta),
                          role_label, text(header_text) | dim}),
                    body});
    }
    if (focused) {
        out = out | inverted;
    }
    return out;
}

struct BracketedPasteGuard {
    BracketedPasteGuard() {
        std::fputs("\033[?2004h", stdout);
        std::fflush(stdout);
    }

    ~BracketedPasteGuard() {
        std::fputs("\033[?2004l", stdout);
        std::fflush(stdout);
    }
};

int runCockpitUi(CockpitAppDeps deps) {
    BracketedPasteGuard bracketed_paste_guard;
    auto &cfg = deps.options;
    auto &runtime = deps.runtime;
    auto &agent = *deps.agent;
    auto cockpit_consent = deps.consent;

    auto auth_token = getenv_var("OPENAI_API_KEY");
    openai::OpenAI openai_client{std::string(auth_token), "", false,
                                 cfg.endpoint};
    openai_client.setTimeouts(std::chrono::milliseconds{10000},
                              std::chrono::milliseconds{0});

    auto event_bus = std::make_shared<AgentEventBus>();
    std::mutex event_mutex;
    std::vector<AgentEvent> pending_events;
    std::atomic<bool> running{true};
    std::atomic<bool> agent_busy{false};
    std::shared_ptr<CockpitAgentDriver> active_driver;
    std::mutex driver_mutex;
    std::optional<std::thread> agent_thread;
    std::chrono::steady_clock::time_point last_task_refresh =
        std::chrono::steady_clock::now();
    constexpr std::chrono::milliseconds task_refresh_interval{750};
    std::chrono::steady_clock::time_point last_log_refresh =
        std::chrono::steady_clock::now();
    constexpr std::chrono::milliseconds log_refresh_interval{1000};
    std::chrono::steady_clock::time_point last_pointer_refresh =
        std::chrono::steady_clock::now();
    constexpr std::chrono::milliseconds pointer_refresh_interval{5000};

    CockpitState cockpit;
    cockpit.session_id = agent.get_session_info().id;
    cockpit.workspace = cfg.workdir;
    cockpit.model = cfg.model;
    cockpit.branch = detectGitBranch();
    cockpit.mission = "Awaiting operator input...";
    cockpit.db_path = cfg.db_path;
    cockpit.mode = cfg.auto_approve ? AgentMode::Agent : AgentMode::Guide;
    cockpit.auto_approve = cfg.auto_approve;
    if (runtime.tools) {
        cockpit.tools = buildToolItemsFromRegistry(*runtime.tools);
    }

    cockpit_consent->set_request_callback(
        [&](const ToolInvocationContext &ctx, const std::string &approval_id) {
            ApprovalItem approval;
            approval.title = "Tool: " + ctx.metadata.name;
            approval.risk = ctx.metadata.is_sensitive ? "high" : "low";
            if (!ctx.metadata.danger_tags.empty()) {
                approval.risk = "medium";
            }
            approval.details =
                std::string(utf8::prefix_by_bytes(ctx.json_args, 120));
            approval.action_id = approval_id;
            std::lock_guard lock(event_mutex);
            cockpit.approvals.push_back(std::move(approval));
        });

    LogLine boot_log{};
    boot_log.timestamp = std::chrono::system_clock::now();
    boot_log.kind = LogKind::System;
    boot_log.task_id = "T-001";
    boot_log.session_id = cockpit.session_id;
    boot_log.text = "CPP-LLM-CODER cockpit initialized";
    cockpit.logs.push_back(std::move(boot_log));

    if (cfg.restore_history) {
        const auto &restored = agent.get_history();
        for (const auto &msg : restored) {
            if (msg.role == MessageRole::System) {
                continue;
            }
            ChatItem item{};
            item.role = msg.role == MessageRole::User ? ChatRole::User
                        : msg.role == MessageRole::Assistant
                            ? ChatRole::Assistant
                        : msg.role == MessageRole::System ? ChatRole::System
                                                          : ChatRole::Tool;
            item.text = msg.content;
            if (msg.role == MessageRole::Tool) {
                item.collapsible = true;
                item.expanded = false;
                item.preview_len = 200;
                item.title = std::format("[tool] {} bytes", msg.content.size());
            }
            cockpit.conversation.push_back(std::move(item));
        }
        if (!cockpit.conversation.empty()) {
            cockpit.selected_chat =
                static_cast<int>(cockpit.conversation.size()) - 1;
            cockpit.chat_scroll_position = 1.0F;
            cockpit.chat_auto_scroll = true;
            cockpit.current_action = "Session restored from history";
        }
    }

    auto start_agent_turn = [&](const std::string &user_msg) {
        if (agent_busy.load(std::memory_order_relaxed)) {
            cockpit.current_action =
                "Agent busy, wait for current turn to finish.";
            return false;
        }
        if (agent_thread && agent_thread->joinable()) {
            agent_thread->join();
            agent_thread.reset();
        }
        agent_busy.store(true, std::memory_order_relaxed);
        cockpit.current_action = "Running agent turn...";
        cockpit.conversation.push_back(
            ChatItem{ChatRole::User, "", user_msg, {}});
        cockpit.conversation.push_back(
            ChatItem{ChatRole::Assistant, "", "", {}});
        auto driver = std::make_shared<CockpitAgentDriver>(event_bus, "main");
        if (cockpit.next_injection) {
            driver->inject(*cockpit.next_injection);
            cockpit.next_injection.reset();
        }
        {
            std::lock_guard lock(driver_mutex);
            active_driver = driver;
        }
        agent_thread.emplace([&, user_msg, driver]() {
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
        });
        return true;
    };

    std::function<void()> rebuild_diff_entries;
    std::function<void()> rebuild_log_entries;
    bool composer_multiline = false;
    bool in_bracketed_paste = false;
    std::string paste_buffer;

    auto submit_input = [&]() {
        const std::string submitted = cockpit.input_value;
        if (submitted.empty()) {
            return;
        }
        if (submitted == "/help") {
            cockpit.show_help = true;
        } else if (submitted == "/tasks") {
            cockpit.selected_tab = 1;
            cockpit.current_action = "Tasks tab";
        } else if (submitted == "/pointers") {
            cockpit.selected_tab = 2;
            cockpit.current_action = "Pointers tab";
        } else if (submitted == "/diff") {
            cockpit.selected_tab = 3;
            cockpit.current_action = "Diff tab";
            cockpit.diff_hunks = loadDiffFromGit();
            rebuild_diff_entries();
        } else if (submitted == "/tools") {
            cockpit.selected_tab = 4;
            cockpit.current_action = "Tools tab";
        } else if (submitted == "/logs") {
            cockpit.selected_tab = 5;
            cockpit.current_action = "Logs tab";
        } else if (submitted == "/review") {
            cockpit.selected_tab = 6;
            cockpit.current_action = "Review tab";
        } else if (submitted == "/mode ask") {
            cockpit.mode = AgentMode::Ask;
            cockpit_consent->set_auto_approve(false);
        } else if (submitted == "/mode guide") {
            cockpit.mode = AgentMode::Guide;
            cockpit_consent->set_auto_approve(false);
        } else if (submitted == "/mode agent") {
            cockpit.mode = AgentMode::Agent;
            cockpit_consent->set_auto_approve(true);
        } else if (submitted == "/mode yolo") {
            cockpit.mode = AgentMode::Yolo;
            cockpit_consent->set_auto_approve(true);
        } else {
            start_agent_turn(submitted);
        }
        cockpit.input_value.clear();
        cockpit.chat_auto_scroll = true;
        rebuild_log_entries();
    };

    auto insert_newline = [&]() { cockpit.input_value.push_back('\n'); };
    auto flush_paste_buffer = [&]() {
        cockpit.input_value += paste_buffer;
        paste_buffer.clear();
    };

    try {
        auto db_tasks = loadTaskTreeFromDb(cockpit.db_path);
        if (!db_tasks.empty()) {
            cockpit.root_tasks = std::move(db_tasks);
            flattenTasks(cockpit.root_tasks, cockpit.flat_tasks);
        }
    } catch (const std::exception &e) {
        LogLine log{};
        log.timestamp = std::chrono::system_clock::now();
        log.kind = LogKind::Warning;
        log.task_id = "T-001";
        log.session_id = cockpit.session_id;
        log.text = std::string("Task sync failed: ") + e.what();
        cockpit.logs.push_back(std::move(log));
    }

    try {
        cockpit.pointers = loadPointersFromDb(cockpit.db_path);
    } catch (const std::exception &e) {
        LogLine log{};
        log.timestamp = std::chrono::system_clock::now();
        log.kind = LogKind::Warning;
        log.task_id = "T-001";
        log.session_id = cockpit.session_id;
        log.text = std::string("Pointer load failed: ") + e.what();
        cockpit.logs.push_back(std::move(log));
    }

    cockpit.diff_hunks = loadDiffFromGit();

    auto screen = ScreenInteractive::Fullscreen();
    screen.TrackMouse(true);

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

    std::thread tick_thread([&] {
        while (running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            screen.PostEvent(Event::Custom);
        }
    });

    std::vector<std::string> tab_labels = cockpit.tabs;
    auto tabs_component = Toggle(&tab_labels, &cockpit.selected_tab);
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
    auto rebuild_task_entries = [&] {
        task_entries.clear();
        if (cockpit.flat_tasks.empty() && !cockpit.root_tasks.empty()) {
            flattenTasks(cockpit.root_tasks, cockpit.flat_tasks);
        }
        for (const auto &t : cockpit.flat_tasks) {
            task_entries.push_back(indentForDepth(t->depth) + t->title + " [" +
                                   t->status + "]");
        }
        clamp_index(cockpit.selected_task, task_entries.size());
    };
    auto rebuild_pointer_entries = [&] {
        pointer_entries.clear();
        for (const auto &p : cockpit.pointers) {
            pointer_entries.push_back(p.id + "  " + p.source);
        }
        clamp_index(cockpit.selected_pointer, pointer_entries.size());
    };
    auto rebuild_tool_entries = [&] {
        tool_entries.clear();
        for (const auto &t : cockpit.tools) {
            tool_entries.push_back(t.name + " [" + t.status + "]");
        }
        clamp_index(cockpit.selected_tool, tool_entries.size());
    };
    rebuild_diff_entries = [&] {
        diff_entries.clear();
        for (const auto &d : cockpit.diff_hunks) {
            diff_entries.push_back(std::string(d.staged ? "[✔] " : "[ ] ") +
                                   d.file_path);
        }
        clamp_index(cockpit.selected_diff, diff_entries.size());
    };
    rebuild_log_entries = [&] {
        merged_logs = cockpit.logs;
        merged_logs.insert(merged_logs.end(), cockpit.db_logs.begin(),
                           cockpit.db_logs.end());
        std::sort(merged_logs.begin(), merged_logs.end(),
                  [](const LogLine &a, const LogLine &b) {
                      return a.timestamp < b.timestamp;
                  });
        log_entries.clear();
        for (const auto &log : merged_logs) {
            log_entries.push_back(timestampStr(log.timestamp) + " [" +
                                  logKindLabel(log.kind) + "] " + log.task_id +
                                  " " + log.text);
        }
        clamp_index(cockpit.selected_log, log_entries.size());
    };
    rebuild_task_entries();
    rebuild_pointer_entries();
    rebuild_tool_entries();
    rebuild_diff_entries();
    rebuild_log_entries();

    Component chat_tab = Renderer([&] {
        Elements lines;
        if (!cockpit.conversation.empty()) {
            for (int i = 0; i < static_cast<int>(cockpit.conversation.size());
                 ++i) {
                lines.push_back(
                    renderChatItem(cockpit.conversation[static_cast<size_t>(i)],
                                   i == cockpit.selected_chat));
                lines.push_back(separatorEmpty());
            }
        } else {
            lines.push_back(text("No conversation yet.") |
                            color(Color::GrayDark));
        }
        if (!cockpit.chat_auto_scroll) {
            lines.push_back(separator());
            lines.push_back(
                text(" ↓ Auto-scroll paused — press End to resume ") | dim |
                color(Color::YellowLight));
        }
        auto content = vbox(std::move(lines));
        if (cockpit.chat_auto_scroll) {
            cockpit.chat_scroll_position = 1.0F;
        }
        content =
            content | focusPositionRelative(0, cockpit.chat_scroll_position);
        return window(text(" Chat/Plan "),
                      content | frame | vscroll_indicator | yframe | flex);
    });
    chat_tab = CatchEvent(chat_tab, [&](Event event) {
        if (cockpit.selected_tab != 0) {
            return false;
        }
        constexpr float kPageStep = 0.25F;
        constexpr float kWheelStep = 0.06F;
        if (event.is_mouse()) {
            auto &m = event.mouse();
            if (m.button == Mouse::WheelUp) {
                cockpit.chat_auto_scroll = false;
                cockpit.chat_scroll_position =
                    std::max(0.0F, cockpit.chat_scroll_position - kWheelStep);
                return true;
            }
            if (m.button == Mouse::WheelDown) {
                cockpit.chat_scroll_position =
                    std::min(1.0F, cockpit.chat_scroll_position + kWheelStep);
                if (cockpit.chat_scroll_position >= 1.0F) {
                    cockpit.chat_auto_scroll = true;
                }
                return true;
            }
        }
        if (event == Event::ArrowUp) {
            if (cockpit.selected_chat > 0) {
                cockpit.selected_chat--;
                cockpit.chat_auto_scroll = false;
            }
            return true;
        }
        if (event == Event::ArrowDown) {
            if (cockpit.selected_chat + 1 <
                static_cast<int>(cockpit.conversation.size())) {
                cockpit.selected_chat++;
                if (cockpit.selected_chat + 1 >=
                    static_cast<int>(cockpit.conversation.size())) {
                    cockpit.chat_auto_scroll = true;
                    cockpit.chat_scroll_position = 1.0F;
                }
            }
            return true;
        }
        if ((event == Event::Return || event == Event::Character(' ')) &&
            cockpit.selected_chat >= 0 &&
            cockpit.selected_chat <
                static_cast<int>(cockpit.conversation.size())) {
            auto &item =
                cockpit
                    .conversation[static_cast<size_t>(cockpit.selected_chat)];
            if (item.collapsible) {
                item.expanded = !item.expanded;
                return true;
            }
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
        return vbox(std::move(lines)) | frame | flex;
    });
    Component diff_tab =
        Renderer(Container::Vertical({diff_menu, diff_detail}), [&] {
            if (diff_entries.empty()) {
                return window(text(" Diff "),
                              text("No diff yet.") | color(Color::GrayDark));
            }
            return window(text(" Diff "),
                          vbox({diff_menu->Render() | frame |
                                    vscroll_indicator | yframe | flex,
                                diff_detail->Render() | flex}));
        });
    Component review_tab = Renderer([&] {
        Elements lines;
        lines.push_back(text("Review") | bold);
        if (cockpit.approvals.empty()) {
            lines.push_back(text("No pending approvals.") |
                            color(Color::GrayDark));
        } else {
            for (const auto &a : cockpit.approvals) {
                lines.push_back(text(a.action_id + " " + a.title));
            }
        }
        return window(text(" Review "), vbox(std::move(lines)) | flex);
    });
    Component content_tab =
        Container::Tab({chat_tab, tasks_tab, pointers_tab, diff_tab, tools_tab,
                        logs_tab, review_tab},
                       &cockpit.selected_tab);

    InputOption input_option = InputOption::Default();
    input_option.content = &cockpit.input_value;
    input_option.placeholder = "";
    input_option.multiline = true;
    input_option.on_enter = submit_input;
    Component input_component = Input(input_option);
    input_component = CatchEvent(input_component, [&](Event event) {
        if (event == Event::Special("\033[200~")) {
            in_bracketed_paste = true;
            paste_buffer.clear();
            return true;
        }
        if (event == Event::Special("\033[201~")) {
            in_bracketed_paste = false;
            flush_paste_buffer();
            return true;
        }
        if (in_bracketed_paste) {
            if (event == Event::Return || event == Event::CtrlM ||
                event == Event::CtrlJ) {
                paste_buffer.push_back('\n');
                return true;
            }
            if (event.is_character()) {
                paste_buffer += event.character();
                return true;
            }
            return true;
        }
        if (event == Event::CtrlJ || event == Event::CtrlM) {
            if (composer_multiline) {
                insert_newline();
                return true;
            }
            return false;
        }
        if (event == Event::Return) {
            if (composer_multiline) {
                insert_newline();
            } else {
                submit_input();
            }
            return true;
        }
        return false;
    });
    Component multiline_toggle = Checkbox("Multiline", &composer_multiline);
    Component send_button = Button("Send", submit_input);
    auto handle_event = [&](Event event) {
        if (event == Event::Escape && cockpit.show_help) {
            cockpit.show_help = false;
            return true;
        }
        if (event == Event::F1 || event == Event::Character('?')) {
            cockpit.show_help = !cockpit.show_help;
            return true;
        }
        if (event == Event::Custom) {
            auto push_log = [&](LogKind kind, std::string text) {
                LogLine log{};
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
                                cockpit.conversation.back().role !=
                                    ChatRole::Assistant) {
                                ChatItem item{};
                                item.role = ChatRole::Assistant;
                                item.agent_id = tok.agent_id;
                                cockpit.conversation.push_back(std::move(item));
                            }
                            cockpit.conversation.back().agent_id = tok.agent_id;
                            cockpit.conversation.back().text += tok.text;
                            cockpit.current_action = "Streaming response...";
                            cockpit.selected_chat = static_cast<int>(
                                cockpit.conversation.size() - 1);
                        },
                        [&](const EvTurnComplete &done) {
                            if (!cockpit.conversation.empty()) {
                                cockpit.conversation.back().text += "\n";
                            }
                            cockpit.current_action = "Turn complete";
                            push_log(LogKind::LLM, "Agent turn complete (" +
                                                       done.agent_id + ")");
                            updated_logs = true;
                        },
                        [&](const EvToolCall &tool) {
                            ChatItem item{};
                            item.role = ChatRole::Tool;
                            item.title = "[" + tool.tool_name + "] " +
                                         (tool.success ? "✓" : "✗") + " " +
                                         std::to_string(tool.summary.size()) +
                                         " bytes";
                            item.text = tool.summary;
                            item.collapsible = true;
                            item.expanded = false;
                            item.preview_len = 200;
                            cockpit.conversation.push_back(std::move(item));
                            push_log(LogKind::Tool,
                                     tool.agent_id + " tool " + tool.tool_name +
                                         " success=" +
                                         (tool.success ? "true" : "false") +
                                         " " + tool.summary);
                            for (auto &ti : cockpit.tools) {
                                if (ti.name == tool.tool_name) {
                                    ti.status =
                                        tool.success ? "ready" : "failed";
                                    break;
                                }
                            }
                            updated_logs = true;
                        },
                        [&](const EvRetry &retry) {
                            clearRetryTarget(cockpit.conversation,
                                             retry.agent_id,
                                             &cockpit.selected_chat);
                            cockpit.current_action =
                                "Retrying (attempt " +
                                std::to_string(retry.attempt) + ")…";
                            push_log(LogKind::LLM,
                                     "Retry attempt " +
                                         std::to_string(retry.attempt) +
                                         " for agent " + retry.agent_id);
                            updated_logs = true;
                        },
                        [&](const EvSubAgentFeedback &feedback) {
                            cockpit.current_action =
                                "Sub-agent feedback from " + feedback.agent_id;
                            ChatItem item{};
                            item.role = ChatRole::Assistant;
                            item.agent_id = feedback.agent_id;
                            item.title = feedback.parent_id.empty()
                                             ? "[subagent]"
                                             : "[subagent -> parent]";
                            item.text = feedback.text;
                            item.collapsible = true;
                            item.expanded = false;
                            item.preview_len = 240;
                            cockpit.conversation.push_back(std::move(item));
                            push_log(
                                LogKind::Agent,
                                "Sub-agent feedback from " + feedback.agent_id +
                                    (feedback.parent_id.empty()
                                         ? ""
                                         : " to parent " + feedback.parent_id));
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

            const auto now = std::chrono::steady_clock::now();
            if (now - last_task_refresh >= task_refresh_interval) {
                last_task_refresh = now;
                try {
                    auto db_tasks = loadTaskTreeFromDb(cockpit.db_path);
                    if (!db_tasks.empty()) {
                        cockpit.root_tasks = std::move(db_tasks);
                        flattenTasks(cockpit.root_tasks, cockpit.flat_tasks);
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
                        loadLogsFromDb(cockpit.db_path, cockpit.session_id);
                    updated_logs = true;
                } catch (const std::exception &e) {
                    push_log(LogKind::Warning,
                             std::string("Log sync failed: ") + e.what());
                    updated_logs = true;
                }
            }
            if (now - last_pointer_refresh >= pointer_refresh_interval) {
                last_pointer_refresh = now;
                try {
                    auto new_pointers = loadPointersFromDb(cockpit.db_path);
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
            if (updated_tasks)
                rebuild_task_entries();
            if (updated_logs)
                rebuild_log_entries();
            return true;
        }
        if (event == Event::CtrlI) {
            cockpit.show_inspector = !cockpit.show_inspector;
            return true;
        }
        if (event == Event::Character('a')) {
            if (!cockpit.approvals.empty()) {
                auto approval = cockpit.approvals.front();
                if (approval.action_id.starts_with("consent-")) {
                    cockpit_consent->resolve(
                        approval.action_id,
                        ToolDecision{ToolDecisionKind::Allow,
                                     "quick-approved via 'a' key",
                                     {}});
                    cockpit.approvals.erase(cockpit.approvals.begin());
                    LogLine approval_log{};
                    approval_log.timestamp = std::chrono::system_clock::now();
                    approval_log.kind = LogKind::Agent;
                    approval_log.task_id = "T-001";
                    approval_log.session_id = cockpit.session_id;
                    approval_log.text = "Tool approved: " + approval.title;
                    cockpit.logs.push_back(std::move(approval_log));
                } else {
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
                }
                rebuild_log_entries();
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
            cockpit.auto_approve = (cockpit.mode == AgentMode::Agent ||
                                    cockpit.mode == AgentMode::Yolo);
            cockpit_consent->set_auto_approve(cockpit.auto_approve);
            cockpit.current_action =
                "Mode changed to " + modeToString(cockpit.mode);
            return true;
        }
        return false;
    };

    Component content =
        Container::Tab({chat_tab, tasks_tab, pointers_tab, diff_tab, tools_tab,
                        logs_tab, review_tab},
                       &cockpit.selected_tab);
    Component content_with_inspector = Renderer(content, [&] {
        Element main_el = content->Render() | flex;
        if (cockpit.show_inspector) {
            return hbox(Elements{main_el,
                                 inspectorPanel(cockpit, merged_logs)}) |
                   flex;
        }
        return main_el;
    });
    Component composer_row =
        Container::Horizontal({multiline_toggle, input_component, send_button});
    Component root = Container::Vertical(
        {tabs_component, content_with_inspector, composer_row});

    auto root_renderer = Renderer(root, [&] {
        if (runtime.stats) {
            cockpit.estimated_tokens =
                static_cast<int>(runtime.stats->totalTokens());
        }
        Element header = headerBar(cockpit);
        Element context = contextBar(cockpit);
        Element tabs = tabs_component->Render() | border;
        Element status_line =
            hbox({
                text(agent_busy.load(std::memory_order_relaxed) ? " ◉ "
                                                                : " ○ ") |
                    color(agent_busy.load(std::memory_order_relaxed)
                              ? Color::Yellow
                              : Color::Green),
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
        Element input_line = hbox(Elements{
                                 text(" ❯ ") | bold | color(Color::Magenta),
                                 multiline_toggle->Render(),
                                 separatorEmpty(),
                                 input_component->Render() | flex,
                                 separatorEmpty(),
                                 send_button->Render(),
                             }) |
                             border;
        Element body = vbox(Elements{header, context, tabs,
                                     content_with_inspector->Render(),
                                     status_line, input_line});
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
            text("  Arrow keys       Navigate lists / chat items"),
            text("  Space/Enter      Toggle expand/collapse"),
            text("  PageUp/Down      Scroll chat / logs"),
            text("  Home/End         Jump to top / resume auto-scroll"),
            text(""),
            text("── Slash Commands ──") | bold | color(Color::Cyan),
            text("  /help /mode /approve /deny /inject /stop /stats /diff"),
            text("  /tasks /tools /logs /pointers /review"),
        };
        Element help_window =
            window(text(" Help / Shortcuts "),
                   vbox(help_lines) | size(WIDTH, GREATER_THAN, 52));
        return dbox({body, help_window | center});
    });

    auto root_event = [&](Event event) {
        if (root->OnEvent(event)) {
            return true;
        }
        return handle_event(event);
    };

    auto exit_loop = screen.ExitLoopClosure();
    auto request_shutdown = [&] {
        if (!running.exchange(false, std::memory_order_relaxed)) {
            return;
        }
        std::shared_ptr<CockpitAgentDriver> driver;
        {
            std::lock_guard lock(driver_mutex);
            driver = active_driver;
        }
        if (driver) {
            driver->request_stop();
        }
        exit_loop();
    };

    screen.Loop(CatchEvent(root_renderer, [&](Event event) {
        if (event == Event::CtrlC) {
            request_shutdown();
            return true;
        }
        return root_event(event);
    }));
    running.store(false, std::memory_order_relaxed);
    request_shutdown();
    if (agent_thread && agent_thread->joinable()) {
        agent_thread->join();
        agent_thread.reset();
    }
    event_bus->post(EvAgentFinished{"__quit", ""});
    if (bus_thread.joinable())
        bus_thread.join();
    if (tick_thread.joinable())
        tick_thread.join();
    return 0;
}
