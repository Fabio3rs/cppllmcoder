#pragma once

// agent_driver.hpp
//
// Protótipo de controle thread-safe para múltiplos agentes + TUI.
//
// Modelo de threading:
//   - Cada Agent roda em sua própria thread (AgentThread)
//   - A TUI roda na main thread (FTXUI exige isso)
//   - Comunicação Agent → TUI via AgentEventBus (fila lock-free por agente)
//   - Comunicação TUI → Agent via IAgentDriver (atomic stop + injeção mutex)
//
// Fluxo de dados:
//
//   [AgentThread N]
//       run_loop()
//           ├─ checa driver.stop_requested()       ← atomic, sem lock
//           ├─ checa driver.next_injection()        ← mutex leve
//           ├─ stream tokens → driver.on_token()   ← posta em EventBus
//           └─ driver.on_turn_complete()            ← posta em EventBus
//
//   [MainThread / TUI]
//       loop FTXUI
//           └─ drena EventBus → atualiza componentes FTXUI
//
// Nenhum dado de UI é tocado pelas threads de agente diretamente.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Eventos que o agente emite para a TUI (ou outro consumidor)
// ─────────────────────────────────────────────────────────────────────────────

struct EvToken {
    std::string agent_id;
    std::string text; // chunk SSE
};

struct EvTurnComplete {
    std::string agent_id;
    std::string full_response;
};

struct EvToolCall {
    std::string agent_id;
    std::string tool_name;
    std::string args_json;
    bool success = false;
    std::string summary;
};

struct EvAgentFinished {
    std::string agent_id;
    std::string final_output;
};

struct EvAgentError {
    std::string agent_id;
    std::string error;
};

using AgentEvent = std::variant<EvToken, EvTurnComplete, EvToolCall,
                                EvAgentFinished, EvAgentError>;

// ─────────────────────────────────────────────────────────────────────────────
// Bus de eventos: thread-safe, drenado pela TUI na main thread
// ─────────────────────────────────────────────────────────────────────────────

class AgentEventBus {
  public:
    void post(AgentEvent ev) {
        {
            std::lock_guard lock(mutex_);
            queue_.push(std::move(ev));
        }
        cv_.notify_one();
    }

    // Drena todos os eventos disponíveis agora (sem bloquear).
    // Chamado pelo loop da TUI a cada frame ou via callback.
    std::vector<AgentEvent> drain() {
        std::vector<AgentEvent> out;
        std::lock_guard lock(mutex_);
        while (!queue_.empty()) {
            out.push_back(std::move(queue_.front()));
            queue_.pop();
        }
        return out;
    }

    // Versão bloqueante — útil para AutoDriver sem TUI.
    AgentEvent wait_next() {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] { return !queue_.empty(); });
        AgentEvent ev = std::move(queue_.front());
        queue_.pop();
        return ev;
    }

  private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<AgentEvent> queue_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Interface que o Agent::run_loop() usa para controle e I/O
// ─────────────────────────────────────────────────────────────────────────────

class IAgentDriver {
  public:
    virtual ~IAgentDriver() = default;

    // Chamados pelo agente durante o loop ReAct ──────────────────────────────

    // Chunk de token do stream SSE
    virtual void on_token(std::string_view token) = 0;

    // Resposta completa de um turno (após acumular todos os chunks)
    virtual void on_turn_complete(std::string_view response) = 0;

    // Resultado de uma tool call (para telemetria/TUI)
    virtual void on_tool_result(std::string_view tool_name, bool success,
                                std::string_view summary) = 0;

    // Controle ───────────────────────────────────────────────────────────────

    // Hard stop: o loop checa isso entre rodadas do ReAct.
    // Implementado com atomic — sem lock, seguro chamar de qualquer thread.
    virtual bool stop_requested() const = 0;
    virtual void request_stop() = 0;

    // Soft stop / message injection: injeta input no próximo turno.
    // Retorna nullopt se não há injeção pendente.
    virtual std::optional<std::string> next_injection() = 0;
    virtual void inject(std::string message) = 0;

    // Timeout opcional para sub-agentes automáticos.
    virtual std::optional<std::chrono::milliseconds> timeout() const = 0;

    // Critério de parada para AutoDriver (ex: N turnos sem <code>).
    // Retorna true se o agente deve encerrar normalmente.
    virtual bool should_finish(int idle_turns) const = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// Driver para agente interativo (TUI / usuário humano)
// ─────────────────────────────────────────────────────────────────────────────

class InteractiveDriver final : public IAgentDriver {
  public:
    explicit InteractiveDriver(std::shared_ptr<AgentEventBus> bus,
                               std::string agent_id)
        : bus_(std::move(bus)), agent_id_(std::move(agent_id)) {}

    void on_token(std::string_view token) override {
        bus_->post(EvToken{agent_id_, std::string(token)});
    }

    void on_turn_complete(std::string_view response) override {
        bus_->post(EvTurnComplete{agent_id_, std::string(response)});
    }

    void on_tool_result(std::string_view tool_name, bool success,
                        std::string_view summary) override {
        bus_->post(EvToolCall{agent_id_,
                              std::string(tool_name),
                              {},
                              success,
                              std::string(summary)});
    }

    bool stop_requested() const override {
        return stop_.load(std::memory_order_relaxed);
    }

    void request_stop() override {
        stop_.store(true, std::memory_order_relaxed);
    }

    std::optional<std::string> next_injection() override {
        std::lock_guard lock(inject_mutex_);
        if (inject_queue_.empty())
            return std::nullopt;
        auto msg = std::move(inject_queue_.front());
        inject_queue_.pop();
        return msg;
    }

    void inject(std::string message) override {
        {
            std::lock_guard lock(inject_mutex_);
            inject_queue_.push(std::move(message));
        }
    }

    // Interativo: sem timeout, sem critério automático de parada
    std::optional<std::chrono::milliseconds> timeout() const override {
        return std::nullopt;
    }

    bool should_finish(int /*idle_turns*/) const override { return false; }

  private:
    std::shared_ptr<AgentEventBus> bus_;
    std::string agent_id_;
    std::atomic<bool> stop_{false};
    std::mutex inject_mutex_;
    std::queue<std::string> inject_queue_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Driver para sub-agente automático (sem TUI, roda até conclusão)
// ─────────────────────────────────────────────────────────────────────────────

class AutoDriver final : public IAgentDriver {
  public:
    struct Config {
        int max_idle_turns = 3; // Para após N turnos sem <code>
        std::optional<std::chrono::milliseconds> timeout = std::nullopt;
    };

    explicit AutoDriver(std::shared_ptr<AgentEventBus> bus,
                        std::string agent_id, Config cfg);

    void on_token(std::string_view token) override {
        // Sub-agentes também postam tokens — a TUI pode mostrar numa aba
        bus_->post(EvToken{agent_id_, std::string(token)});
    }

    void on_turn_complete(std::string_view response) override {
        bus_->post(EvTurnComplete{agent_id_, std::string(response)});
    }

    void on_tool_result(std::string_view tool_name, bool success,
                        std::string_view summary) override {
        bus_->post(EvToolCall{agent_id_,
                              std::string(tool_name),
                              {},
                              success,
                              std::string(summary)});
    }

    bool stop_requested() const override {
        return stop_.load(std::memory_order_relaxed);
    }

    void request_stop() override {
        stop_.store(true, std::memory_order_relaxed);
    }

    std::optional<std::string> next_injection() override {
        std::lock_guard lock(inject_mutex_);
        if (inject_queue_.empty())
            return std::nullopt;
        auto msg = std::move(inject_queue_.front());
        inject_queue_.pop();
        return msg;
    }

    void inject(std::string message) override {
        {
            std::lock_guard lock(inject_mutex_);
            inject_queue_.push(std::move(message));
        }
    }

    std::optional<std::chrono::milliseconds> timeout() const override {
        return cfg_.timeout;
    }

    bool should_finish(int idle_turns) const override {
        return idle_turns >= cfg_.max_idle_turns;
    }

  private:
    std::shared_ptr<AgentEventBus> bus_;
    std::string agent_id_;
    Config cfg_;
    std::atomic<bool> stop_{false};
    std::mutex inject_mutex_;
    std::queue<std::string> inject_queue_;
};

inline AutoDriver::AutoDriver(std::shared_ptr<AgentEventBus> bus,
                              std::string agent_id, Config cfg)
    : bus_(std::move(bus)), agent_id_(std::move(agent_id)),
      cfg_(std::move(cfg)) {}

// ─────────────────────────────────────────────────────────────────────────────
// AgentHandle: representa um agente rodando em background
// Retornado pelo AgentManager ao spawnar
// ─────────────────────────────────────────────────────────────────────────────

class AgentHandle {
  public:
    AgentHandle(std::string id, std::shared_ptr<IAgentDriver> driver,
                std::future<std::string> result)
        : id_(std::move(id)), driver_(std::move(driver)),
          result_(std::move(result)) {}

    // Non-copyable, movable
    AgentHandle(const AgentHandle &) = delete;
    AgentHandle &operator=(const AgentHandle &) = delete;
    AgentHandle(AgentHandle &&) = default;
    AgentHandle &operator=(AgentHandle &&) = default;

    const std::string &id() const { return id_; }

    void stop() { driver_->request_stop(); }

    void inject(std::string msg) { driver_->inject(std::move(msg)); }

    // Bloqueia até o agente terminar e retorna o output final.
    // Lança se o agente terminou com erro.
    std::string collect() { return result_.get(); }

    bool is_done() const {
        return result_.wait_for(std::chrono::seconds(0)) ==
               std::future_status::ready;
    }

  private:
    std::string id_;
    std::shared_ptr<IAgentDriver> driver_;
    std::future<std::string> result_;
};

// ─────────────────────────────────────────────────────────────────────────────
// AgentManager: spawna e rastreia agentes
// ─────────────────────────────────────────────────────────────────────────────

class AgentManager {
  public:
    explicit AgentManager(std::shared_ptr<AgentEventBus> bus)
        : bus_(std::move(bus)) {}

    // Spawna um sub-agente automático.
    // `task_fn` é o callable que roda o loop do agente — tipicamente
    // uma lambda que cria um Agent e chama agent.run(driver, initial_task).
    AgentHandle spawn_auto(std::string agent_id, AutoDriver::Config cfg,
                           std::function<std::string(IAgentDriver &)> task_fn) {

        auto driver = std::make_shared<AutoDriver>(bus_, agent_id, cfg);

        auto future =
            std::async(std::launch::async,
                       [driver, fn = std::move(task_fn), id = agent_id,
                        bus = bus_]() mutable -> std::string {
                           try {
                               auto result = fn(*driver);
                               bus->post(EvAgentFinished{id, result});
                               return result;
                           } catch (const std::exception &e) {
                               bus->post(EvAgentError{id, e.what()});
                               throw;
                           }
                       });

        return AgentHandle{std::move(agent_id), std::move(driver),
                           std::move(future)};
    }

    // Spawna o agente interativo (apenas um por vez normalmente).
    // Retorna o driver para que a TUI possa injetar mensagens e tokens.
    std::shared_ptr<InteractiveDriver>
    spawn_interactive(std::string agent_id,
                      std::function<void(IAgentDriver &)> task_fn) {

        auto driver = std::make_shared<InteractiveDriver>(bus_, agent_id);

        std::thread([driver, fn = std::move(task_fn), id = agent_id,
                     bus = bus_]() mutable {
            try {
                fn(*driver);
                bus->post(EvAgentFinished{id, {}});
            } catch (const std::exception &e) {
                bus->post(EvAgentError{id, e.what()});
            }
        }).detach();

        return driver;
    }

  private:
    std::shared_ptr<AgentEventBus> bus_;
};
