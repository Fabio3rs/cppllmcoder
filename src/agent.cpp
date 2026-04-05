#include "agent.hpp"

#include "agent_driver.hpp"
#include "agents/agent_action.hpp"
#include "brain_store.hpp"
#include "done_task_signal.hpp"
#include "json_utils.hpp"
#include "llm_chat_streamer.hpp"
#include "time_utils.hpp"
#include "utils/utf8_text.hpp"
#include "uuid_utils.hpp"
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <format>
#include <nlohmann/json.hpp>
#include <openai/openai.hpp>
#include <print>
#include <string_view>
#include <utility>

namespace {
std::string build_params_json(const SessionInfo &s) {
    nlohmann::json j;
    j["model"] = s.model;

    if (!s.model_version.empty()) {
        j["model_version"] = s.model_version;
    }

    j["endpoint"] = s.endpoint;
    j["temperature"] = s.temperature;
    j["top_p"] = s.top_p;

    if (s.top_k > 0) {
        j["top_k"] = s.top_k;
    }

    if (s.max_tokens > 0) {
        j["max_tokens"] = s.max_tokens;
    }
    j["seed"] = s.seed;
    return j.dump();
}
} // namespace

Agent::Agent(const app::Options &opts, std::shared_ptr<ToolRegistry> tools,
             std::shared_ptr<IPromptManager> prompts,
             std::shared_ptr<IToolConsentProvider> consent,
             std::shared_ptr<IExecutionLogger> logger,
             std::shared_ptr<IStatsRecorder> stats,
             std::shared_ptr<DoneTaskSignal> done)
    : options(opts), tool_registry(std::move(tools)),
      prompt_manager(std::move(prompts)), consent_provider(std::move(consent)),
      execution_logger(std::move(logger)), stats_recorder(std::move(stats)),
      done_task_signal(std::move(done)) {
    session_info.id = options.session_id_override.empty()
                          ? utils::generate_uuid_v4()
                          : options.session_id_override;
    session_info.model = options.model;
    session_info.model_version = options.model_version;
    session_info.endpoint = options.endpoint;
    session_info.temperature = options.temperature;
    session_info.top_p = options.top_p;
    session_info.top_k = options.top_k;
    session_info.max_tokens = options.max_tokens;
    session_info.seed = options.seed;
    session_info.params_json = build_params_json(session_info);

    brain_store =
        std::make_unique<BrainStore>(BrainStore::open(options.db_path));
    brain_store->ensureSession(session_info);

    if (options.restore_history && brain_store) {
        try {
            session_info = brain_store->loadSession(session_info.id);
            history = brain_store->loadMessages(session_info.id);
            messages_cache.clear();
            total_tokens = 0;
            for (const auto &msg : history) {
                append_to_cache(msg);
                total_tokens += static_cast<size_t>(msg.token_count);
            }
            if (stats_recorder && total_tokens > 0) {
                stats_recorder->incrementTokenCount(
                    static_cast<int>(total_tokens));
            }
        } catch (const std::exception &ex) {
            std::fprintf(stderr, "[agent] failed to restore session '%s': %s\n",
                         session_info.id.c_str(), ex.what());
        }
    }

    if (tool_registry) {
        auto toolInvocationHandler =
            [this](
                const ToolMetadata &meta, const ITool &tool,
                sol::variadic_args va,
                sol::this_state s) -> std::expected<sol::object, std::string> {
            // Opcional: geramos uma prévia em JSON só para consent/log.
            auto preview = LuaContext::luaVariadicToJson(va, s).value_or(
                std::string{"<lua-args>"});
            ToolInvocationContext ctx{meta, std::move(preview), 0,
                                      std::chrono::milliseconds{0},
                                      session_info};
            const auto consent_start = std::chrono::steady_clock::now();
            auto decision = evaluate_tool_consent(ctx);
            const auto consent_end = std::chrono::steady_clock::now();
            const auto consent_latency =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    consent_end - consent_start);
            if (decision.action == ToolDecisionKind::Deny ||
                decision.action == ToolDecisionKind::AbortConversation) {
                return std::unexpected("denied by consent provider");
            }
            // Se o provedor ajustou os argumentos, tentamos interpretá-los
            // como JSON e re-hidratar para Lua? Por ora, priorizamos
            // segurança e retornamos erro.
            if (decision.action == ToolDecisionKind::ModifyArgs) {
                return std::unexpected(
                    "argument modification not supported with lua args");
            }
            const auto start = std::chrono::steady_clock::now();
            auto result = tool.invoke(va, s);
            const auto end = std::chrono::steady_clock::now();
            const auto duration =
                std::chrono::duration_cast<std::chrono::milliseconds>(end -
                                                                      start);

            const bool success = result.has_value();
            std::string summary;
            if (success) {
                const sol::object &val = *result;
                switch (val.get_type()) {
                case sol::type::string:
                    summary = val.as<std::string>();
                    break;
                case sol::type::number:
                    summary = std::to_string(val.as<double>());
                    break;
                case sol::type::boolean:
                    summary = val.as<bool>() ? "true" : "false";
                    break;
                case sol::type::nil:
                    summary = "null";
                    break;
                default: {
                    auto json_summary = LuaContext::luaObjectToJson(val);
                    summary = json_summary.value_or("<unserializable result>");
                    break;
                }
                }
                if (summary.size() > 200) {
                    summary = std::string(utf8::prefix_by_bytes(summary, 200));
                }
            } else {
                summary = result.error();
            }

            if (brain_store) {
                brain_store->insertToolInvocation(
                    ctx, decision, duration, consent_latency, success, summary);
            }
            if (execution_logger) {
                execution_logger->logToolEvent(ctx, decision, duration, success,
                                               summary, session_info);
            }

            return result;
        };
        luaContext.bindTools(*tool_registry, toolInvocationHandler);
    }
}

Agent::~Agent() = default;

std::string Agent::run_step(std::string_view input, IAgentDriver &driver,
                            openai::OpenAI &openai_client) {
    using time_utils::elapsed_ms;
    using time_utils::now_steady;
    using time_utils::now_system;
    using time_utils::to_iso8601_ms;

    const auto now = now_system();

    if (history.empty()) {
        // System prompt é o primeiro
        auto tools = tool_registry->topKDocs("", 16);

        auto system_prompt = prompt_manager->buildSystemPrompt(*this);

        Message system_msg{.id = 0,
                           .role = MessageRole::System,
                           .content = system_prompt,
                           .session_id = session_info.id,
                           .created_at = to_iso8601_ms(now),
                           .updated_at = to_iso8601_ms(now),
                           .duration = std::chrono::milliseconds{0},
                           .token_count = 0};
        add_to_history(system_msg);
    }

    Message user_msg{.id = 0,
                     .role = MessageRole::User,
                     .content = std::string(input),
                     .session_id = session_info.id,
                     .created_at = to_iso8601_ms(now),
                     .updated_at = to_iso8601_ms(now),
                     .duration = std::chrono::milliseconds{0},
                     .token_count = 0};
    add_to_history(user_msg);

    if (done_task_signal) {
        done_task_signal->reset();
    }

    std::string full_reply;
    int iterations = 0;
    int idle_turns = 0;

    const auto timeout = driver.timeout();
    const auto deadline =
        timeout ? std::optional<
                      std::chrono::steady_clock::time_point>{now_steady() +
                                                             *timeout}
                : std::nullopt;

    while (true) {
        if (driver.stop_requested()) {
            break;
        }

        if (deadline && now_steady() > *deadline) {
            break;
        }

        if (!options.auto_approve && iterations >= options.max_iterations) {
            break;
        }

        if (driver.should_finish(idle_turns)) {
            break;
        }

        // Processa injeções pendentes vindas da TUI (soft stop).
        while (auto injection = driver.next_injection()) {
            const auto now_injection = now_system();
            Message injected_msg{.id = 0,
                                 .role = MessageRole::User,
                                 .content = std::move(*injection),
                                 .session_id = session_info.id,
                                 .created_at = to_iso8601_ms(now_injection),
                                 .updated_at = to_iso8601_ms(now_injection),
                                 .duration = std::chrono::milliseconds{0},
                                 .token_count = 0};
            add_to_history(injected_msg);
        }

        iterations++;

        prune_context(); // Placeholder for context compression/sliding window

        nlohmann::json req;
        req["model"] = options.model;
        req["messages"] = messages_cache;
        req["stream"] = true;
        req["stream_options"] = {{"include_usage", true}};

        llm::ChatStreamer streamer(openai_client);
        const auto start = now_steady();
        llm::Usage usage{};
        const std::string reply =
            streamer.stream(std::move(req), driver, &usage);
        const auto end = now_steady();
        const auto duration = elapsed_ms(start, end);

        full_reply += reply;

        const auto now_after = now_system();
        Message assistant_msg{
            .id = 0,
            .role = MessageRole::Assistant,
            .content = reply,
            .session_id = session_info.id,
            .created_at = to_iso8601_ms(now_after),
            .updated_at = to_iso8601_ms(now_after),
            .duration = duration,
            .token_count = static_cast<unsigned int>(
                usage.total_tokens >= 0 ? usage.total_tokens : 0)};
        add_to_history(assistant_msg);

        if (stats_recorder && usage.total_tokens >= 0) {
            stats_recorder->incrementTokenCount(usage.total_tokens);
        }

        AgentAction action = AgentAction::parse(reply);

        if (!action.has_code) {
            idle_turns++;
            break;
        }
        idle_turns = 0;

        {
            const auto lua_start = now_steady();
            auto lua_result = luaContext.execute(action.lua_code);
            const auto lua_end = now_steady();
            const auto lua_duration = elapsed_ms(lua_start, lua_end);

            const bool success = lua_result.has_value();
            const std::string summary =
                success ? *lua_result : lua_result.error();
            driver.on_tool_result("lua", success, summary);

            // Persist tool invocation-style log for Lua (and mirror to
            // execution log)
            if (brain_store || execution_logger) {
                ToolMetadata meta{.name = "lua",
                                  .description = "inline lua execution",
                                  .arguments = {},
                                  .usage_example = "",
                                  .returns = "",
                                  .danger_tags = {},
                                  .is_sensitive = false,
                                  .always_show_in_prompt = false};
                ToolInvocationContext ctx{
                    meta,                         // metadata
                    std::string(action.lua_code), // store script
                    0,                            // estimated_token_cost
                    lua_duration,                 // estimated_latency
                    session_info};
                ToolDecision decision{}; // default allow

                if (brain_store) {
                    brain_store->insertToolInvocation(
                        ctx, decision, lua_duration,
                        std::chrono::milliseconds{0}, success, summary);
                    brain_store->insertExecutionLog(
                        "", session_info.id, action.lua_code, summary,
                        success ? "" : summary, 0, lua_duration);
                }

                if (execution_logger) {
                    execution_logger->logToolEvent(ctx, decision, lua_duration,
                                                   success, summary,
                                                   session_info);
                }
            }

            const auto now_tool = now_system();
            Message tool_msg{.id = 0,
                             .role = MessageRole::Tool,
                             .content = summary,
                             .session_id = session_info.id,
                             .created_at = to_iso8601_ms(now_tool),
                             .updated_at = to_iso8601_ms(now_tool),
                             .duration = lua_duration,
                             .token_count = 0};
            add_to_history(tool_msg);
        }

        if (done_task_signal && done_task_signal->consume()) {
            break;
        }
    }

    return full_reply;
}

ToolDecision Agent::evaluate_tool_consent(const ToolInvocationContext &ctx) {
    if (!consent_provider) {
        return {};
    }
    return consent_provider->requestToolUse(ctx);
}

std::shared_ptr<ToolRegistry> Agent::get_tool_registry() const {
    return tool_registry;
}

std::shared_ptr<IPromptManager> Agent::get_prompt_manager() const {
    return prompt_manager;
}

std::shared_ptr<IToolConsentProvider> Agent::get_consent_provider() const {
    return consent_provider;
}

const SessionInfo &Agent::get_session_info() const { return session_info; }

void Agent::persist_message(Message &msg) {
    if (brain_store) {
        brain_store->insertMessage(msg);
    }
    if (execution_logger) {
        execution_logger->logMessage(msg, session_info);
    }
}

void Agent::add_to_history(Message msg) {
    history.emplace_back(std::move(msg));
    persist_message(history.back()); // Updates the ID in history.back()
    append_to_cache(history.back());
}

void Agent::prune_context() {
    // When implementing pruning, remember to:
    // messages_cache.erase(messages_cache.begin(), messages_cache.begin() + N);

    [[maybe_unused]] constexpr std::string_view prompt_for_compression =
        R"md(### Context compression instructions
            ....
)md";
}

std::string Agent::prune_message(const Message &msg_ref) const {
    if (msg_ref.content.size() <= options.max_tool_out_bytes) {
        return msg_ref.content;
    }

    std::string_view TAG_OPEN = "<truncated>";
    std::string_view TAG_CLOSE = "</truncated>";

    if (msg_ref.id == 0) {
        // Não tem ID?!
        size_t total = TAG_OPEN.size() + TAG_CLOSE.size();
        if (total > options.max_tool_out_bytes) {
            total = 0;
        } else {
            total = options.max_tool_out_bytes - total;
        }
        const auto safe_prefix =
            utf8::prefix_by_bytes(std::string_view(msg_ref.content), total);
        return std::format("{}{}{}", TAG_OPEN, safe_prefix, TAG_CLOSE);
    }

    /**
     * TODO: deixar dinâmico a seleção de tools
     */
    std::string dbtools =
        std::format("TOTAL IN BYTES ORIGINAL: {}\n READ IT IN BLOCKS WITH THE "
                    "TOOL(S):\ndb.head('messages', {}, 'content', "
                    "offset_bytes, bytes_to_read)\n",
                    msg_ref.content.size(), msg_ref.id);

    size_t overhead = dbtools.size() + TAG_OPEN.size() + TAG_CLOSE.size() + 5;

    if (overhead > options.max_tool_out_bytes) {
        return std::format("{}...{}\n{}", TAG_OPEN, TAG_CLOSE, dbtools);
    }

    size_t msgsize = options.max_tool_out_bytes - overhead;

    const auto safe_prefix =
        utf8::prefix_by_bytes(std::string_view(msg_ref.content), msgsize);
    return std::format("{}{}{}\n{}", TAG_OPEN, safe_prefix, TAG_CLOSE, dbtools);
}

void Agent::append_to_cache(const Message &msg_ref) {
    std::string add_id;

    if (msg_ref.id != 0) {
        add_id = std::format("\n<id>{}</id>\n", msg_ref.id);
    }

    std::string message = prune_message(msg_ref);

    if (!options.supports_tool_role && msg_ref.role == MessageRole::Tool) {
        auto content =
            std::format("{}\n{}\n{}{}", TOOL_RESPONSE_TAG_OPEN,
                        std::move(message), TOOL_RESPONSE_TAG_CLOSE, add_id);
        messages_cache.push_back({{"role", "user"}, {"content", content}});
    } else {
        messages_cache.push_back(
            {{"role", msg_ref.role_to_string()},
             {"content", (add_id.empty() ? std::move(message)
                                         : std::move(message) + add_id)}});
    }
}
