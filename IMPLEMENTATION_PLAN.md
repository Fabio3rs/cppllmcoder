# Próximos Passos: Integração Cockpit-Agent

## Status Atual

✅ **main.cpp restructured:**
- Data model completo (CockpitState, TaskNode, PointerItem, ToolItem, LogLine)
- 7 abas definidas
- Layout base com header, tabs, footer
- Input com suporte básico a slash commands
- Inspector colapsável
- 4 modos de operação (Ask, Guide, Agent, Yolo)
- Compilação OK

⏳ **Próximo:** Integração com agent.run_step() e observabilidade em tempo real

---

## Tarefas Imediatas

### 1. **Conectar Agent Streaming ao Chat/Plan Tab**

**O quê:**
- Capturar tokens sendo emitidos pelo LLM (streaming)
- Exibir em tempo real na aba Chat/Plan
- Mostrar plano extraído do loop ReAct

**Como:**
1. Criar uma nova classe `CockpitAgentDriver : public IAgentDriver`
   - Herda de `IAgentDriver` (já na codebase)
   - Override `on_token()` → acumula tokens + reflete no cockpit
   - Override `on_turn_complete()` → marca fim da resposta
   - Override `on_tool_result()` → registra em cockpit.logs + cockpit.approvals

2. Instanciar na main loop:
   ```cpp
   CockpitAgentDriver driver(cockpit);
   // Em vez de StdIODriver
   agent.run_step(user_input, driver, openai_client);
   ```

3. Update UI em background thread (ou callback)
   - Cada token atualiza `cockpit.conversation` e redraws a aba Chat
   - O FTXUI redraws a cada frame anyway

**Arquivo:** `include/cockpit_agent_driver.hpp` + `src/cockpit_agent_driver.cpp`

---

### 2. **Task Tree Lifecycle Events**

**O quê:**
- Quando agente cria/atualiza/completa uma task, refletir em cockpit.root_tasks
- Exibir hierarquia em tempo real na aba Tasks

**Como:**
1. Hook into `Agent` lifecycle:
   - `task_created(task_id, parent_id, title)` event
   - `task_status_changed(task_id, status)` event
   - `task_completed(task_id, result)` event

2. Ou: Query do SQLite brain periodicamente
   - A cada 500ms, refetch task tree do DB
   - Compara com estado anterior, atualiza cockpit.root_tasks

**Arquivo:** Eventos já existem? Ou criar `task_event_bus.hpp`?

---

### 3. **Logs Estruturados**

**O quê:**
- Converter todos os `std::cout` / `std::cerr` / `std::println` em LogLine tipadas
- Exibir em aba Logs com timestamp, kind, duration

**Como:**
1. Criar `logging_sink.hpp` que captura:
   - Saída de stderr (LLM, Lua, TOOL, etc.)
   - Agent state changes
   - DB queries
   - File operations

2. Cada evento ⟶ `LogLine { timestamp, kind, task_id, session_id, text, duration_ms }`

3. Push para `cockpit.logs` (thread-safe)

4. Renderizar com cores semânticas na aba Logs

**Arquivo:** `include/logging_sink.hpp`

---

### 4. **Approvals & Safety Gate**

**O quê:**
- Quando modo == Guide ou Ask, pedir aprovação antes de:
  - Executar tool com risk_level > 0
  - Escrever arquivo
  - Rodar shell command
  - Fazer git commit

**Como:**
1. Criar `approval_gate.hpp`
   - `request_approval(action_title, details, risk_level)`
   - Retorna `bool` (approved or not)

2. Em `CockpitAgentDriver::on_tool_result()`:
   - Verificar modo
   - Se Guide/Ask: popula `cockpit.approvals` e aguarda
   - Usuário aperta `a` para aprovar ou entra `/deny`

3. Driver bloqueia até aprovação

**Arquivo:** `include/approval_gate.hpp`

---

### 5. **Pointer Injection & Memory Management**

**O quê:**
- Carregar pointers do SQLite brain na startup
- Permitir `/inject <pointer_id>` para injetar evidência no contexto ativo
- Mostrar score e relacionamentos na aba Pointers

**Como:**
1. Na startup (antes do loop TUI):
   ```cpp
   auto pointers_from_db = load_pointers_from_sqlite(cockpit.db_path);
   cockpit.pointers = pointers_from_db;
   ```

2. Slash command handler:
   ```cpp
   if (cmd == "/inject") {
       auto ptr = find_pointer(args[0]);
       cockpit.next_injection = ptr.summary;  // Injeta no próximo agent step
       cockpit.logs.push_back({..., "Injected pointer " + ptr.id});
   }
   ```

3. Em CockpitAgentDriver, checar se `cockpit.next_injection` e passar para prompt

**Arquivo:** `include/pointer_store.hpp`

---

### 6. **Diff Viewer & Staging**

**O quê:**
- Quando agente propõe edições, mostrar na aba Diff como hunks navegáveis
- Usuário pode stage/reject bloco por bloco
- Preview antes de aplicar

**Como:**
1. Criar `diff_manager.hpp`
   - Modela mudanças de arquivo como hunks
   - Tracks staged/unstaged status

2. Em `CockpitAgentDriver::on_turn_complete()`:
   - Parse mudanças propostas
   - Popula `cockpit.diff_hunks`

3. Aba Diff renderiza com cores:
   - Verde = new lines
   - Vermelho = old lines
   - Amarelo = context

4. Usuário aperta Space para stage hunk
   - `/review` mostra resumo
   - `/commit` aplica e faz git commit

**Arquivo:** `include/diff_manager.hpp`

---

### 7. **Sub-Agent Dashboard**

**O quê:**
- Visualizar subagentes ativos, tarefas paralelas, hierarquia
- Mostrar em aba Tasks com indicador visual

**Como:**
1. Query SQLite: `SELECT * FROM tasks WHERE parent_id = ? ORDER BY status`
2. Renderizar com `depth` como indentação
3. Status colors por estado: ◉ (running), ◯ (pending), ✓ (done), ✗ (failed)

**Arquivo:** Parte de `Tasks` tab renderer

---

## Implementação Prioritária

### Tier 1 (Essencial para MVP)
1. ✅ Data model
2. ⏳ **CockpitAgentDriver** – streaming tokens
3. ⏳ **Task tree sync** – tasks aparecem em tempo real
4. ⏳ **Logs estruturados** – observabilidade

### Tier 2 (Completa o cockpit)
5. ⏳ **Approvals gate** – safety + interactividade
6. ⏳ **Diff viewer** – edições com preview
7. ⏳ **Git integration** – branch, commit, review

### Tier 3 (Diferencial)
8. ⏳ **Pointer injection** – evidência em contexto
9. ⏳ **Memory browser** – search/filter de pointers
10. ⏳ **Timeline visual** – chronology de investigação

---

## Checklist para Próxima Sessão

- [ ] Criar `cockpit_agent_driver.hpp` + `.cpp`
  - [ ] Herdar de `IAgentDriver`
  - [ ] Override `on_token()`, `on_turn_complete()`, `on_tool_result()`
  - [ ] Atualizar `cockpit` state (thread-safe)

- [ ] Integrar na main loop
  - [ ] Remover `StdIODriver`
  - [ ] Instanciar `CockpitAgentDriver driver(cockpit)`
  - [ ] Passar para `agent.run_step()`

- [ ] Criar `logging_sink.hpp`
  - [ ] Hook em stderr/stdout
  - [ ] Produz `LogLine` tipadas
  - [ ] Acumula em `cockpit.logs`

- [ ] Atualizar Chat tab renderer
  - [ ] Mostra `cockpit.conversation` (streamed)
  - [ ] Mostra `cockpit.current_action`

- [ ] Atualizar Tasks tab renderer
  - [ ] Renderiza `cockpit.root_tasks` como árvore
  - [ ] Cores por status
  - [ ] Expand/collapse com Space

- [ ] Testar integração end-to-end
  - [ ] Compilar com novo code
  - [ ] Rodar agent em cockpit
  - [ ] Verificar streaming, logs, tasks appearing

---

## Notas Técnicas

### Thread Safety
- `CockpitState` será acessado de múltiplas threads:
  - Main thread (FTXUI rendering)
  - Agent thread (agent.run_step)
  - Background tasks thread (logs, monitoring)

**Solução:** Usar `std::mutex` ou melhor: `std::atomic` para fields simples, `std::shared_ptr<T>` com mutex para estruturas complexas.

### Rendering Performance
- FTXUI redraw a cada frame (~60fps por padrão)
- Se `cockpit.logs` crescer muito, renderizar último N logs (ex: últimos 100)
- Scroll/pagination para logs muito grandes

### Agent Integration
- `Agent::run_step()` já retorna?
- Precisa ser async/callback?
- Ou blocking (com UI updates durante blocking)?

→ **Verificar:** `agent_driver.hpp` e `agent.hpp` para entender o flow

---

## Arquivos a Criar/Modificar

| Arquivo | Tipo | Descrição |
|---------|------|-----------|
| `include/cockpit_agent_driver.hpp` | Novo | Bridge agent → cockpit |
| `src/cockpit_agent_driver.cpp` | Novo | Impl |
| `include/logging_sink.hpp` | Novo | Obs estruturada |
| `include/approval_gate.hpp` | Novo | Safety gate |
| `include/diff_manager.hpp` | Novo | Diff viewer |
| `include/pointer_store.hpp` | Novo | Memory browser |
| `src/main.cpp` | Modify | Integração |
| `COCKPIT_SPEC.md` | Novo | ✅ Feito |
| `IMPLEMENTATION_PLAN.md` | Novo | ✅ Este arquivo |

---

## Perguntas Abertas

1. **Agent** é síncrono ou assíncrono?
   - Preciso fazer `std::thread` para não bloquear FTXUI?
   
2. **SQLite brain** é acessível durante investigação?
   - Posso fazer queries em background sem deadlock?

3. **Existing event bus** para task lifecycle?
   - Ou criar do zero?

4. **Modo "headless"** sem TUI?
   - Manter `--headless` flag? Ou TUI is mandatório agora?

→ **Verificar:** README, CMakeLists, agent.hpp, runtime_defaults.hpp

---

## Referências Internas

- `include/agent_driver.hpp` – Interface a implementar
- `include/agent.hpp` – Main agent class
- `include/runtime_defaults.hpp` – Runtime setup
- `include/tool_registry.hpp` – Tools + policies
- `tests/test_agent_run_step.cpp` – Como usar agent
- `docs/SUMMARIES.md` – Design overview

---

## Success Criteria

✅ **MVP Cockpit Loop:**
1. Usuário digita prompt
2. Agent inicia `run_step()`
3. Tokens aparecem em Chat tab em tempo real
4. Tasks aparecem em Tasks tab conforme criadas
5. Logs aparecem em Logs tab com timestamp e tipo
6. Approvals popup quando modo == Guide
7. Loop fecha quando agent completa ou usuário aperta `q`

✅ **Compilação limpa** (sem warnings)

✅ **Sem deadlocks ou race conditions**

✅ **TUI responsivo** (não congela durante agent execution)
