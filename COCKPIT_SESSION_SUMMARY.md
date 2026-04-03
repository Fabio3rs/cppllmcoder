# Cockpit Redesign - Sumário da Sessão

## 🎯 Objetivo Alcançado

Transformar a interface de um **chat bonitinho** para um **cockpit de investigação técnica**, honrando a verdadeira natureza do projeto: reversa de firmware, memória persistente em SQLite, agentes recursivos, pointers semânticos e observabilidade.

---

## ✅ Deliverables Desta Sessão

### 1. **Data Model Completo** (`src/main.cpp` linhas 30-220)

```cpp
struct CockpitState {
    // Session identity
    std::string session_id, workspace, model, branch, db_path;
    
    // Runtime config
    AgentMode mode;           // Ask, Guide, Agent, Yolo
    bool sandbox_enabled;
    bool auto_approve;
    
    // UI state
    int selected_tab;         // 0-6
    bool show_inspector;
    
    // Dynamic structures
    std::vector<std::shared_ptr<TaskNode>> root_tasks;
    std::vector<PointerItem> pointers;
    std::vector<ToolItem> tools;
    std::vector<LogLine> logs;
    std::vector<ApprovalItem> approvals;
    std::vector<DiffHunk> diff_hunks;
};

enum class AgentMode { Ask, Guide, Agent, Yolo };
enum class LogKind { System, LLM, Lua, Tool, DB, Agent, MCP, FS, Warning, Error };
struct TaskNode { id, title, status, owner, duration, children... };
struct PointerItem { id, summary, source, relevance_score, related_pointers... };
struct LogLine { timestamp, kind, task_id, session_id, text, duration_ms };
```

**Benefício:** Todas as estruturas que precisamos já estão modeladas e prontas para usar. Não há "mágica" na TUI; tudo é explícito.

---

### 2. **Layout FTXUI Base** (`src/main.cpp` linhas 688-932)

```
Header (mission, mode, model)
Context (workspace, branch, session, tokens, sandbox)
Tab bar (Chat/Plan, Tasks, Pointers, Diff, Tools, Logs, Review)
─────────────────────────────────────
[Task Tree] │ [Main Content] │ [Inspector]
[left col]  │   [center]     │ [right col]
─────────────────────────────────────
Status line (current action)
Command bar (input + slash commands)
```

**Benefício:** Estrutura pronta para iteração. Cada componente é renderizável independentemente.

---

### 3. **4 Modos de Operação** com Atalho Rápido

| Modo | Cor | Comportamento | Atalho |
|------|-----|---------------|--------|
| **Ask** | 🔵 Azul | Diagnóstico, sem escrita | `m` → Ask |
| **Guide** | 🟡 Amarelo | Propõe, pede aprovação ← Default | `m` → Guide |
| **Agent** | 🟢 Verde | Auto-executa passos aprovados | `m` → Agent |
| **Yolo** | 🔴 Vermelho | Máxima autonomia (sandbox) | `m` → Yolo |

Ou via slash: `/mode guide`

**Benefício:** Usuário tem controle fino sobre risco vs. autonomia. Psicologicamente claro.

---

### 4. **7 Abas Especializadas** (Implementadas em renderers)

1. **Chat/Plan** – Resposta do agente + plano em passos
2. **Tasks** – Árvore hierárquica com status e duração
3. **Pointers/Memory** – Navegação de evidências com score
4. **Diff/Edits** – Viewer de mudanças propostas
5. **Tools** – Inspector de ferramentas e políticas
6. **Logs/Trace** – Eventos estruturados com timestamp
7. **Review/Git** – Status do workspace

**Benefício:** Organização clara. Cada tarefa tem seu espaço. Não há confusão entre conversa e logs.

---

### 5. **Inspector Colapsável** com Detalhes Contextuais

- Task selecionada → ID, status, duração, owner
- Pointer selecionado → Score, source, relacionamentos
- Tool selecionado → Risk, latência, descrição
- Log selecionado → Tipo, timestamp, duration

Sempre mostra: **"Current Action"** (o que o agente está fazendo agora)

**Benefício:** Inspeção profunda sem poluir main content. Alterna com `Ctrl-I`.

---

### 6. **Command Bar Híbrida**

Aceita tanto:
- **Texto livre:** `Analyze firmware region 0x4F00`
- **Slash commands:** `/mode guide`, `/approve`, `/inject P_42`, `/tools`, `/help`

**Benefício:** Power users podem trabalhar rápido com commands; conversação natural ainda funciona.

---

### 7. **Keyboard Shortcuts** Ergonômicos

| Atalho | Ação |
|--------|------|
| Tab | Próxima aba |
| Shift+Tab | Aba anterior |
| Setas | Navegar |
| Enter | Abrir inspector |
| Space | Expand/collapse tasks, stage diff |
| `a` | Aprovar pending |
| `r` | Rejeitar pending |
| `m` | Cycle mode |
| `/` | Command palette |
| `Ctrl-I` | Toggle inspector |
| `F1` | Help |

**Benefício:** Workflow é mouse-optional, fluente para terminal power users.

---

### 8. **Compilação Limpa**

```
[2/2 100% :: 7.400] Linking CXX executable cppllmcoder
Build succeeded ✅
Executable: /mnt/projects/Projects/cppllmcoder/build/cppllmcoder (42M)
```

**Benefício:** Código pronto para rodar. Não há sintaxe errors. FTXUI integrado.

---

## 📋 Documentação de Referência

### 1. **COCKPIT_SPEC.md** (Nova)
- Especificação completa da interface
- Layout ASCII
- Cada aba detalhada
- Data structures
- Keyboard shortcuts
- Design principles

**Uso:** Referência arquitetural. Leia antes de implementar qualquer aba.

### 2. **IMPLEMENTATION_PLAN.md** (Nova)
- Tarefas concretas em 3 tiers
- Checklist para próxima sessão
- Perguntas abertas
- Success criteria

**Uso:** Roadmap para desenvolvimento. Siga a ordem.

---

## 🔄 Comparação: Antes vs. Depois

### ❌ Antes (Old main.cpp)
```cpp
while (true) {
    std::print("\n[User]> ");
    std::getline(std::cin, user_input);
    agent.run_step(user_input, driver, openai_client);
    // Outputs to stdout
    // No structure
}
```

**Limitações:**
- Chat simples, sem contexto visual
- Logs se perdem no scroll
- Sem aprovações
- Sem observabilidade (timestamps, task tree)
- Mode switch exigia restart
- Não honra a arquitetura real (pointers, brain, subtasks)

### ✅ Depois (New TUI Cockpit)
```cpp
CockpitState cockpit;  // Central state
// TUI event loop
screen.Loop(app);      // Handles all interaction
```

**Ganhos:**
- 7 abas especializadas
- Time-series logs com tipo e duração
- Inspector contextual
- 4 modos operacionais
- Task tree hierarchical
- Pointers com score
- Diff staging
- Mode toggle em tempo real
- Visual observability
- Expressa a alma do projeto (investigação técnica)

---

## 🎮 Próxima Sessão: Integração Agent

### Ordem Recomendada

1. **CockpitAgentDriver** – Streaming tokens em tempo real
2. **Task Tree Sync** – Tasks aparecem conforme criadas
3. **Structured Logs** – Logs com timestamp, kind, duration
4. **Approval Gate** – Safety checks no modo Guide
5. **Diff Viewer** – Edições propostas com staging
6. **Git Integration** – Commits, branch, review

### Verificar Antes

- [ ] `agent.hpp` – Como funciona `run_step()`?
- [ ] `agent_driver.hpp` – Qual interface herdar?
- [ ] `runtime_defaults.hpp` – Setup correto?
- [ ] SQLite brain – Acessível em loop?
- [ ] Existing event system – Task lifecycle events?

---

## 🚀 Visão de Futuro

### Curto Prazo (MVP)
- Agent streaming na TUI
- Tasks em tempo real
- Logs estruturados
- Approvals

### Médio Prazo
- Diff viewer com hunk staging
- Pointer injection em contexto
- Git integration completa
- Sub-agent dashboard

### Longo Prazo
- Memory browser com search semântico
- Timeline visual de investigação
- Multi-file editor integrado
- Benchmark & performance profiling
- Undo/redo granular
- Customização de tema

---

## 📊 Métricas de Sucesso

| Item | Status | Target |
|------|--------|--------|
| Data model | ✅ Done | - |
| Layout FTXUI | ✅ Done | - |
| 7 abas | ✅ Base | Renderers completos |
| Keyboard shortcuts | ✅ Done | - |
| Inspector | ✅ Done | Conteúdo contextual |
| Modes (Ask/Guide/Agent/Yolo) | ✅ Done | Safety gate conectada |
| Agent streaming | ⏳ Todo | Este mês |
| Tasks sync | ⏳ Todo | Este mês |
| Logs estruturados | ⏳ Todo | Este mês |
| Compilação | ✅ Clean | - |

---

## 💡 Insights Principais

1. **Não copie a estética, copie a disciplina.**
   - Claude Code, Gemini CLI, Codex CLI não são especiais por "chat bonito"
   - São especiais porque mostram **intenção, progresso, controle**
   - Nossa TUI faz isso mas para **reversa e investigação técnica**

2. **A arquitetura da TUI deve refletir a arquitetura do projeto.**
   - Persistência → Task tree hierarchical
   - Memória semântica → Pointers com score
   - Observabilidade → Logs com timestamp/duration
   - Segurança → 4 modos + approval gate
   - Tudo explícito, nada mágico

3. **Operador é o agente, não o chat.**
   - Framing: "você governa uma investigação" (não "você conversa")
   - UI reflete isso: cockpit, não messenger
   - Atalhos de power user (vim-like, htop-like)

4. **Modo é força psicológica.**
   - Ask (azul) → diagnóstico seguro
   - Guide (amarelo) → investigação cuidadosa
   - Agent (verde) → autonomia controlada
   - Yolo (vermelho) → ousadia consciente
   - Toggle com `m` transforma a sensação de controle

5. **Timestamp e duração são primeira classe.**
   - Não apenas "logs aparecem"
   - Mas "logs aparecem com [HH:MM:SS.mmm] | 123ms | TIPO | ID"
   - Isso transforma observabilidade em *inteligência* sobre o sistema

---

## 📁 Arquivos Criados/Modificados

```
cppllmcoder/
├── src/main.cpp
│   ├── [30-220] Data model (CockpitState, TaskNode, etc.)
│   ├── [230-380] Helper functions (colors, timestamps, etc.)
│   ├── [385-430] UI components (header_bar, context_bar, inspector_panel)
│   ├── [438-932] Main cockpit loop + event handlers
│   └── ✅ Compilação OK
├── COCKPIT_SPEC.md ✨ NEW
│   └── Especificação completa (layout, abas, data, keyboard, modos)
├── IMPLEMENTATION_PLAN.md ✨ NEW
│   └── Roadmap concreto (tarefas, checklist, perguntas abertas)
└── build/
    └── cppllmcoder (42M) ✅ Executável pronto
```

---

## 🎬 Como Testar

```bash
cd /mnt/projects/Projects/cppllmcoder/build
./cppllmcoder --help            # Ver opções
./cppllmcoder --model qwen:14b  # Abrir cockpit

# Dentro da TUI:
Tab                 # Trocar aba
m                   # Trocar modo (Ask→Guide→Agent→Yolo)
/help               # Ver slash commands
/mode agent         # Modo Agent
Ctrl-I              # Toggle inspector
q ou Ctrl-C         # Sair
```

---

## 🏁 Conclusão

**Sessão foi um sucesso:** Transformamos a interface de um "chat simples" para um **cockpit profissional de investigação técnica**, estruturado, observável e expressivo. O código agora:

✅ Compila limpo  
✅ Modela corretamente o domínio  
✅ Oferece 4 modos de controle  
✅ Estrutura logs, tasks, pointers, diffs como primeira classe  
✅ Está pronto para integração com agent.run_step()  

**Próximo:** Conectar agent, stream tokens, sync tasks, estruturar logs. Tudo preparado para isso.

---

**Autor:** GitHub Copilot  
**Data:** 3 de abril de 2026  
**Status:** ✅ MVP Layout Completo, ⏳ Agent Integration Próximo
