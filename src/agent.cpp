#include "agent.hpp"

#include "agent_driver.hpp"
#include "agents/agent_action.hpp"
#include "brain_store.hpp"
#include "json_utils.hpp"
#include "llm_chat_streamer.hpp"
#include "time_utils.hpp"
#include <nlohmann/json.hpp>
#include <openai/openai.hpp>
#include <random>
#include <sstream>
#include <utility>

namespace {
std::string generate_uuid_v4() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    auto rand64 = [&]() { return gen(); };

    auto to_hex = [](uint64_t value, size_t width) {
        std::ostringstream oss;
        oss << std::hex;
        oss.width(static_cast<std::streamsize>(width));
        oss.fill('0');
        oss << value;
        return oss.str();
    };

    // Build 128 bits
    uint64_t high = rand64();
    uint64_t low = rand64();

    // Set version (4) and variant (10xx)
    high &= 0xFFFFFFFFFFFF0FFFULL;
    high |= 0x0000000000004000ULL;
    low &= 0x3FFFFFFFFFFFFFFFULL;
    low |= 0x8000000000000000ULL;

    std::ostringstream uuid;
    uuid << to_hex((high >> 32) & 0xFFFFFFFFULL, 8) << "-"
         << to_hex((high >> 16) & 0xFFFFULL, 4) << "-"
         << to_hex(high & 0xFFFFULL, 4) << "-"
         << to_hex((low >> 48) & 0xFFFFULL, 4) << "-"
         << to_hex(low & 0xFFFFFFFFFFFFULL, 12);
    return uuid.str();
}

std::string build_params_json(const SessionInfo &s) {
    std::ostringstream oss;
    oss << "{" << "\"model\":\"" << json_utils::escapeJson(s.model) << "\","
        << "\"model_version\":\"" << json_utils::escapeJson(s.model_version)
        << "\"," << "\"endpoint\":\"" << json_utils::escapeJson(s.endpoint)
        << "\"," << "\"temperature\":" << s.temperature << ","
        << "\"top_p\":" << s.top_p << "," << "\"top_k\":" << s.top_k << ","
        << "\"max_tokens\":" << s.max_tokens << "," << "\"seed\":" << s.seed
        << "}";
    return oss.str();
}
} // namespace

Agent::Agent(const app::Options &opts, std::shared_ptr<ToolRegistry> tools,
             std::shared_ptr<IPromptManager> prompts,
             std::shared_ptr<IToolConsentProvider> consent,
             std::shared_ptr<IExecutionLogger> logger,
             std::shared_ptr<IStatsRecorder> stats)
    : options(opts), tool_registry(std::move(tools)),
      prompt_manager(std::move(prompts)), consent_provider(std::move(consent)),
      execution_logger(std::move(logger)), stats_recorder(std::move(stats)) {
    session_info.id = options.session_id_override.empty()
                          ? generate_uuid_v4()
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

    if (tool_registry) {
        auto toolInvocationHandler = [this](const ToolMetadata &meta,
                                            const ITool &tool,
                                            const sol::object &lua_args)
            -> std::expected<sol::object, std::string> {
            // Opcional: geramos uma prévia em JSON só para consent/log.
            auto preview = LuaContext::luaObjectToJson(lua_args).value_or(
                std::string{"<lua-object>"});
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
            auto result = tool.invoke(lua_args);
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
                    summary.resize(200);
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

        auto system_prompt = prompt_manager->buildSystemPrompt(history, tools);

        Message system_msg{.id = 0,
                           .role = MessageRole::System,
                           .content = system_prompt,
                           .session_id = session_info.id,
                           .created_at = to_iso8601_ms(now),
                           .updated_at = to_iso8601_ms(now),
                           .duration = std::chrono::milliseconds{0},
                           .token_count = 0};
        history.push_back(system_msg);
        persist_message(history.back());
    }

    Message user_msg{.id = 0,
                     .role = MessageRole::User,
                     .content = std::string(input),
                     .session_id = session_info.id,
                     .created_at = to_iso8601_ms(now),
                     .updated_at = to_iso8601_ms(now),
                     .duration = std::chrono::milliseconds{0},
                     .token_count = 0};
    history.push_back(user_msg);
    persist_message(history.back());

    nlohmann::json req;
    req["model"] = options.model;
    req["messages"] = nlohmann::json::array();
    for (const auto &msg : history) {
        req["messages"].push_back(
            {{"role", msg.role_to_string()}, {"content", msg.content}});
    }
    req["stream"] = true;
    req["stream_options"] = {{"include_usage", true}};

    llm::ChatStreamer streamer(openai_client);
    const auto start = now_steady();
    llm::Usage usage{};
    const std::string reply = streamer.stream(std::move(req), driver, &usage);
    const auto end = now_steady();
    const auto duration = elapsed_ms(start, end);

    const auto now_after = now_system();
    Message assistant_msg{.id = 0,
                          .role = MessageRole::Assistant,
                          .content = reply,
                          .session_id = session_info.id,
                          .created_at = to_iso8601_ms(now_after),
                          .updated_at = to_iso8601_ms(now_after),
                          .duration = duration,
                          .token_count =
                              usage.total_tokens >= 0 ? usage.total_tokens : 0};
    history.push_back(assistant_msg);
    persist_message(history.back());

    if (stats_recorder && usage.total_tokens >= 0) {
        stats_recorder->incrementTokenCount(usage.total_tokens);
    }

    AgentAction action = AgentAction::parse(reply);
    if (action.has_code) {
        const auto lua_start = now_steady();
        auto lua_result = luaContext.execute(action.lua_code);
        const auto lua_end = now_steady();
        const auto lua_duration = elapsed_ms(lua_start, lua_end);

        const bool success = lua_result.has_value();
        const std::string summary = success ? *lua_result : lua_result.error();
        driver.on_tool_result("lua", success, summary);

        // Persist tool invocation-style log for Lua (and mirror to execution
        // log)
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
                brain_store->insertToolInvocation(ctx, decision, lua_duration,
                                                  std::chrono::milliseconds{0},
                                                  success, summary);
                brain_store->insertExecutionLog(
                    "", session_info.id, action.lua_code, summary,
                    success ? "" : summary, 0, lua_duration);
            }

            if (execution_logger) {
                execution_logger->logToolEvent(ctx, decision, lua_duration,
                                               success, summary, session_info);
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
        history.push_back(tool_msg);
        persist_message(history.back());
    }

    return reply;
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

void Agent::prune_context() {}
