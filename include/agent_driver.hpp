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
#include <unordered_map>
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

struct EvRetry {
    std::string agent_id;
    int attempt = 0;
};

using AgentEvent = std::variant<EvToken, EvTurnComplete, EvToolCall,
                                EvAgentFinished, EvAgentError, EvRetry>;

enum class AgentState { Running, Finished, Errored };

struct AgentStatus {
    std::string agent_id;
    std::optional<std::string> parent_id;
    AgentState state = AgentState::Running;
    std::string last_error{};
    std::chrono::milliseconds runtime_ms{0}; // steady-derived, monotonic
    std::chrono::system_clock::time_point started_at{};
    std::optional<std::chrono::system_clock::time_point> finished_at{};
};

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

    // Chamado pelo ChatStreamer *antes* de cada nova tentativa após falha.
    // `attempt` é o índice 0-based da tentativa *que está prestes a começar*
    // (portanto 1 na primeira retry, 2 na segunda, …).
    //
    // Contrato importante: tokens parciais de tentativas anteriores podem ter
    // sido entregues via on_token() antes que o erro fosse detectado
    // (tipicamente em falhas de rede mid-stream). O driver é responsável por
    // descartar/sobrescrever esse conteúdo parcial se necessário.
    // Drivers simples (CLI, log) podem ignorar; drivers TUI devem limpar o
    // buffer de streaming do turno atual.
    //
    // Implementação padrão: no-op (não quebra implementações existentes).
    virtual void on_retry(int /*attempt*/) {}

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
                std::shared_future<std::string> result)
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
    std::shared_future<std::string> result_;
};

// ─────────────────────────────────────────────────────────────────────────────
// AgentManager: spawna e rastreia agentes
// ─────────────────────────────────────────────────────────────────────────────

class AgentManager {
  private:
    struct ManagedAgentRecord {
        std::string id;
        std::optional<std::string> parent_id;
        std::shared_ptr<IAgentDriver> driver;
        std::shared_future<std::string> result;
        AgentState state = AgentState::Running;
        std::chrono::steady_clock::time_point started_steady{};
        std::optional<std::chrono::steady_clock::time_point> finished_steady{};
        std::chrono::system_clock::time_point started_at{};
        std::optional<std::chrono::system_clock::time_point> finished_at{};
        std::string last_error{};
    };

    AgentStatus to_status(const ManagedAgentRecord &rec) const {
        AgentStatus st;
        st.agent_id = rec.id;
        st.parent_id = rec.parent_id;
        st.state = rec.state;
        st.last_error = rec.last_error;
        const auto end_steady =
            rec.finished_steady.value_or(std::chrono::steady_clock::now());
        st.runtime_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_steady - rec.started_steady);
        st.started_at = rec.started_at;
        st.finished_at = rec.finished_at;
        return st;
    }

  public:
    explicit AgentManager(std::shared_ptr<AgentEventBus> bus)
        : bus_(std::move(bus)) {}

    // Spawna um sub-agente automático.
    // `task_fn` é o callable que roda o loop do agente — tipicamente
    // uma lambda que cria um Agent e chama agent.run(driver, initial_task).
    AgentHandle
    spawn_auto(std::string agent_id, AutoDriver::Config cfg,
               std::function<std::string(IAgentDriver &)> task_fn,
               std::optional<std::string> parent_id = std::nullopt) {

        auto driver = std::make_shared<AutoDriver>(bus_, agent_id, cfg);

        auto record = std::make_shared<ManagedAgentRecord>();
        record->id = agent_id;
        record->parent_id = std::move(parent_id);
        record->driver = driver;
        record->state = AgentState::Running;
        record->started_steady = std::chrono::steady_clock::now();
        record->started_at = std::chrono::system_clock::now();

        std::weak_ptr<ManagedAgentRecord> weak_record = record;

        auto future = std::async(
            std::launch::async,
            [driver, fn = std::move(task_fn), id = agent_id, bus = bus_,
             weak_record]() mutable -> std::string {
                try {
                    auto result = fn(*driver);
                    if (auto rec = weak_record.lock()) {
                        rec->state = AgentState::Finished;
                        rec->finished_steady = std::chrono::steady_clock::now();
                        rec->finished_at = std::chrono::system_clock::now();
                        rec->last_error.clear();
                    }
                    bus->post(EvAgentFinished{id, result});
                    return result;
                } catch (const std::exception &e) {
                    if (auto rec = weak_record.lock()) {
                        rec->state = AgentState::Errored;
                        rec->finished_steady = std::chrono::steady_clock::now();
                        rec->finished_at = std::chrono::system_clock::now();
                        rec->last_error = e.what();
                    }
                    bus->post(EvAgentError{id, e.what()});
                    throw;
                }
            });

        auto shared_future = future.share();
        record->result = shared_future;

        {
            std::lock_guard lock(registry_mutex_);
            registry_[record->id] = record;
        }

        return AgentHandle{std::move(agent_id), std::move(driver),
                           std::move(shared_future)};
    }

    // Spawna o agente interativo (apenas um por vez normalmente).
    // Retorna o driver para que a TUI possa injetar mensagens e tokens.
    std::shared_ptr<InteractiveDriver>
    spawn_interactive(std::string agent_id,
                      std::function<void(IAgentDriver &)> task_fn,
                      std::optional<std::string> parent_id = std::nullopt) {

        auto driver = std::make_shared<InteractiveDriver>(bus_, agent_id);

        auto promise = std::make_shared<std::promise<std::string>>();
        auto shared_future = promise->get_future().share();

        auto record = std::make_shared<ManagedAgentRecord>();
        record->id = agent_id;
        record->parent_id = std::move(parent_id);
        record->driver = driver;
        record->result = shared_future;
        record->state = AgentState::Running;
        record->started_steady = std::chrono::steady_clock::now();
        record->started_at = std::chrono::system_clock::now();

        {
            std::lock_guard lock(registry_mutex_);
            registry_[record->id] = record;
        }

        std::thread([driver, fn = std::move(task_fn), id = agent_id, bus = bus_,
                     promise,
                     weak_record =
                         std::weak_ptr<ManagedAgentRecord>(record)]() mutable {
            try {
                fn(*driver);
                promise->set_value({});
                if (auto rec = weak_record.lock()) {
                    rec->state = AgentState::Finished;
                    rec->finished_steady = std::chrono::steady_clock::now();
                    rec->finished_at = std::chrono::system_clock::now();
                    rec->last_error.clear();
                }
                bus->post(EvAgentFinished{id, {}});
            } catch (const std::exception &e) {
                promise->set_exception(std::current_exception());
                if (auto rec = weak_record.lock()) {
                    rec->state = AgentState::Errored;
                    rec->finished_steady = std::chrono::steady_clock::now();
                    rec->finished_at = std::chrono::system_clock::now();
                    rec->last_error = e.what();
                }
                bus->post(EvAgentError{id, e.what()});
            }
        }).detach();

        return driver;
    }

    // Consulta status agregado de um agente
    std::optional<AgentStatus> status(std::string_view agent_id) const {
        std::lock_guard lock(registry_mutex_);
        auto it = registry_.find(std::string(agent_id));
        if (it == registry_.end())
            return std::nullopt;
        return to_status(*it->second);
    }

    // Lista filhos diretos de um pai
    std::vector<std::string> children_of(std::string_view parent_id) const {
        std::vector<std::string> out;
        std::lock_guard lock(registry_mutex_);
        for (const auto &[id, rec] : registry_) {
            if (rec->parent_id && *rec->parent_id == parent_id) {
                out.push_back(id);
            }
        }
        return out;
    }

    // Snapshot de todos os agentes conhecidos
    std::vector<AgentStatus> list() const {
        std::vector<AgentStatus> out;
        std::lock_guard lock(registry_mutex_);
        out.reserve(registry_.size());
        for (const auto &[_, rec] : registry_) {
            out.push_back(to_status(*rec));
        }
        return out;
    }

    // Injeta mensagem em um agente existente (bidirecional via driver)
    bool inject_into(std::string_view agent_id, std::string message) const {
        std::shared_ptr<IAgentDriver> driver;
        {
            std::lock_guard lock(registry_mutex_);
            auto it = registry_.find(std::string(agent_id));
            if (it == registry_.end())
                return false;
            driver = it->second->driver;
        }
        if (!driver)
            return false;
        driver->inject(std::move(message));
        return true;
    }

    bool request_stop(std::string_view agent_id) const {
        std::shared_ptr<IAgentDriver> driver;
        {
            std::lock_guard lock(registry_mutex_);
            auto it = registry_.find(std::string(agent_id));
            if (it == registry_.end())
                return false;
            driver = it->second->driver;
        }
        if (!driver)
            return false;
        driver->request_stop();
        return true;
    }

  private:
    std::shared_ptr<AgentEventBus> bus_;
    mutable std::mutex registry_mutex_;
    std::unordered_map<std::string, std::shared_ptr<ManagedAgentRecord>>
        registry_;
};
