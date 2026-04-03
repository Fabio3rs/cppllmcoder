// agent_driver_usage.cpp
//
// Exemplo de como main.cpp conecta tudo:
// TUI (main thread) + agente interativo + sub-agentes paralelos

#include "agent_driver.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Esqueleto do loop ReAct que vai dentro do Agent::run()
// (ainda não implementado — placeholder para mostrar como o driver é usado)
// ─────────────────────────────────────────────────────────────────────────────

std::string agent_react_loop(IAgentDriver &driver,
                             const std::string &initial_input) {
    std::string last_response;
    int idle_turns = 0;
    const auto deadline = driver.timeout()
                              ? std::optional(std::chrono::steady_clock::now() +
                                              *driver.timeout())
                              : std::nullopt;

    std::string current_input = initial_input;

    while (true) {
        // 1. Hard stop
        if (driver.stop_requested())
            break;

        // 2. Timeout
        if (deadline && std::chrono::steady_clock::now() > *deadline)
            break;

        // 3. Critério de parada automática
        if (driver.should_finish(idle_turns))
            break;

        // 4. Soft stop / injeção de mensagem
        if (auto injection = driver.next_injection()) {
            current_input = std::move(*injection);
            idle_turns = 0;
        }

        // ── Aqui entra a chamada real ao LLM via openai-cpp (streaming) ──
        // Use llm::ChatStreamer para fazer SSE → driver.on_token().
        // Pseudocódigo:
        //   llm::ChatStreamer streamer(openai_client);
        //   streamer.stream(history_json, driver);
        //   last_response = response_buffer; // preenchido pelo driver
        //
        // ── Extração de <code>...</code> ──
        //   auto code = extract_code_block(response);
        //   if (!code) { idle_turns++; continue; }
        //   idle_turns = 0;
        //
        // ── Execução Lua ──
        //   auto result = lua_context.execute(*code);
        //   driver.on_tool_result("lua", result.has_value(), ...);
        //   current_input = result.value_or(result.error());

        // Placeholder para compilar:
        (void)current_input;
        idle_turns++;
        last_response = "[placeholder]";
    }

    return last_response;
}

// ─────────────────────────────────────────────────────────────────────────────
// main.cpp — wiring completo
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    // Bus compartilhado por todos os agentes e pela TUI
    auto bus = std::make_shared<AgentEventBus>();
    AgentManager manager(bus);

    // ── Agente interativo (responde ao usuário via TUI) ──────────────────────
    auto interactive_driver =
        manager.spawn_interactive("main", [](IAgentDriver &driver) {
            // Agent agent(opts, tools, ...);
            // agent.run(driver, "");  ← loop fica aqui esperando injeções
            agent_react_loop(driver, "");
        });

    // ── Sub-agentes automáticos (spawned pelo pai via Lua) ───────────────────
    std::vector<AgentHandle> sub_agents;

    sub_agents.push_back(manager.spawn_auto(
        "sub-re-iram",
        AutoDriver::Config{.max_idle_turns = 2,
                           .timeout = std::chrono::minutes(10)},
        [](IAgentDriver &driver) -> std::string {
            return agent_react_loop(
                driver, "Analyze IRAM region 0x00-0x7F for sensor mappings");
        }));

    sub_agents.push_back(manager.spawn_auto(
        "sub-re-xram",
        AutoDriver::Config{.max_idle_turns = 2,
                           .timeout = std::chrono::minutes(10)},
        [](IAgentDriver &driver) -> std::string {
            return agent_react_loop(
                driver,
                "Analyze XRAM region 0x8000-0xFFFF for actuator tables");
        }));

    // ── Loop da TUI (main thread — FTXUI exige isso) ─────────────────────────
    //
    // Em vez de polling busy, o FTXUI usa um callback de "on_screen_update"
    // que pode ser trigado pelo bus. Esboço:
    //
    // auto screen = ftxui::ScreenInteractive::Fullscreen();
    //
    // // Thread auxiliar que drena o bus e chama screen.PostEvent()
    // std::thread tui_updater([&] {
    //     while (true) {
    //         auto events = bus->drain();
    //         for (auto& ev : events) {
    //             std::visit([&](auto& e) { handle_event(e, screen); }, ev);
    //         }
    //         std::this_thread::sleep_for(std::chrono::milliseconds(16)); //
    //         ~60fps
    //     }
    // });
    //
    // screen.Loop(component);  ← bloqueia até o usuário sair

    // ── Hard stop em todos os sub-agentes ao sair ────────────────────────────
    for (auto &handle : sub_agents) {
        handle.stop();
    }

    // ── Coleta resultados (opcional — pode ser feito via Lua também) ─────────
    for (auto &handle : sub_agents) {
        if (handle.is_done()) {
            try {
                auto result = handle.collect();
                (void)result;
            } catch (...) {
                // log error
            }
        }
    }

    // ── Stop no agente interativo ────────────────────────────────────────────
    interactive_driver->request_stop();

    return 0;
}
