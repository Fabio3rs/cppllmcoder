# CPP-LLM-CODER Cockpit Specification

## Overview

Transformação da interface de um "chat assistente" para um **cockpit de investigação técnica**, honrando a arquitetura real do projeto: execução persistente, memória em SQLite, subtasks recursivas, pointers semânticos e observabilidade com timestamps.

A interface não é para conversar com o agente; é para **governar uma investigação** conduzida pelo agente.

---

## Layout da TUI

```
┌─ Header Bar ───────────────────────────────────────────────────────────────┐
│ CPP-LLM-CODER mission: Analyze firmware · mode GUIDE · sandbox ON         │
└────────────────────────────────────────────────────────────────────────────┘

┌─ Context Bar ──────────────────────────────────────────────────────────────┐
│ model: qwen · ws: ./firmware · branch: analysis/kline · session: sess_... │
└────────────────────────────────────────────────────────────────────────────┘

┌─ Tab Bar ──────────────────────────────────────────────────────────────────┐
│ [Chat/Plan] [Tasks] [Pointers] [Diff] [Tools] [Logs] [Review]            │
└────────────────────────────────────────────────────────────────────────────┘

┌─ Task Tree (Left)  │ ┌─ Main Content (Center) ────────┐ ┌─ Inspector ────┐
│ T-001 (running)    │ │ [Selected tab content]          │ │ Task: T-001    │
│   T-002 (pending)  │ │                                 │ │ Status: running│
│   T-003 (done)     │ │ [Rendered output, logs, etc]    │ │ Duration: 45ms │
│                    │ │                                 │ │                │
│                    │ │                                 │ │ Current action:│
│                    │ │                                 │ │ Spawning sub..  │
└────────────────────┴─┴─────────────────────────────────┴─┴────────────────┘

┌─ Status Line ──────────────────────────────────────────────────────────────┐
│ ◉ Processing user input · 3 subagents · 12 pointers loaded                │
└────────────────────────────────────────────────────────────────────────────┘

┌─ Command Bar ──────────────────────────────────────────────────────────────┐
│ ❯ /model qwen2.5:14b [Tab to autocomplete] | Type /help for slash cmds   │
└────────────────────────────────────────────────────────────────────────────┘
```

---

## Abas Principais (7 tabs)

### 1. **Chat/Plan** (Tab 0)
- **Propósito:** Resposta conversacional do agente + plano em passos
- **Conteúdo:**
  - Última mensagem do agente (streaming)
  - Plano derivado do loop ReAct: ["ler símbolo X", "buscar pointer", "spawn subagent"]
  - Hipótese atual
- **Interação:**
  - Scroll com roda do mouse
  - Enter para enviar próxima instrução
  - `/inject_pointer P_42` para injetar evidência no prompt

### 2. **Tasks** (Tab 1)
- **Propósito:** Árvore hierárquica de tasks e subtasks
- **Conteúdo:**
  - Raiz, nós filhos, netos, etc.
  - Status: pending, running, completed, failed
  - Duração, owner (qual subagent), último evento
  - Expandir/colapsar com `[+] / [-]`
- **Interação:**
  - Clique para selecionar (reflete no inspector)
  - Space para expandir/colapsar
  - Scroll vertical
  - Cor por status: verde (done), amarelo (running), magenta (queued), vermelho (failed)

### 3. **Pointers/Memory** (Tab 2)
- **Propósito:** Navegação de evidências semânticas
- **Conteúdo:**
  - ID (P_42, P_91, etc.)
  - Score de relevância (0.0-1.0)
  - Origem (firmware.bin:0x4F00)
  - Resumo da evidência
  - Relacionamentos (USES_TABLE, CALLS_FUNCTION, etc.)
- **Interação:**
  - Clique para inspecionar em painel lateral
  - Botão/comando `/inject P_42` para injetar no contexto ativo
  - `/link P_42 T-003` para vincular a task
  - Filtro por score ou tipo
  - Busca incremental

### 4. **Diff/Edits** (Tab 3)
- **Propósito:** Viewer de mudanças propostas, staging por hunk
- **Conteúdo:**
  - Arquivo → lista de hunks
  - Cada hunk mostra old/new lines em cores (vermelho/verde)
  - `[Stage] [Reject] [Preview]` botões
- **Interação:**
  - Clique em hunk para expandir/colapsar
  - Space para stagear/unstage
  - Enter para preview detalhado
  - Diferencial final antes de aplicar
  - Git integration: `git add`, `git commit`

### 5. **Tools** (Tab 4)
- **Propósito:** Inspector de ferramentas e políticas
- **Conteúdo:**
  - Nome, status (ready/running/restricted/failed)
  - Risk level: low/medium/high
  - Latência média (ms)
  - Descrição
  - Último argumento chamado
  - Taxa de sucesso/falha
- **Interação:**
  - Clique para detalhe em inspector
  - `/tool fs.read --dry-run` para testar
  - `/policy` para ver matrix de risco
  - Botão para rescan/reload

### 6. **Logs/Trace** (Tab 5)
- **Propósito:** Observabilidade estruturada com timestamp e duração
- **Conteúdo:**
  - Cada linha: `[HH:MM:SS.mmm] [TIPO] [TASK_ID] duracao_ms text`
  - Tipos: SYS, LLM, LUA, TOOL, DB, AGENT, MCP, FS, WARN, ERR
  - Cor semântica por tipo
  - Filtro + busca incremental
- **Interação:**
  - Scroll com roda
  - Filtro por tipo: `/log kind:LLM`, `/log task:T-002`
  - Clique para expandir linha longa
  - Export para arquivo

### 7. **Review/Git** (Tab 6)
- **Propósito:** Status do workspace e control de versão
- **Conteúdo:**
  - Branch ativo
  - Arquivos staged/unstaged
  - Commits sugeridos pelo agente
  - Diff preview
  - Remote status
  - Rollback/stash options
- **Interação:**
  - `/git commit "msg"` para fazer commit
  - `/git diff` para ver mudanças
  - `/git revert` para desfazer
  - Clique em arquivo para abrir diff

---

## Painel Lateral (Inspector)

**Colapsável via `Ctrl-I`**. Mostra details contextuais do item selecionado:

- **Se Task selecionada:**
  - ID, status, owner, duração
  - Último log relevante
  - Botão: "Focus on this task"
  
- **Se Pointer selecionado:**
  - ID, score, source
  - Resumo completo
  - Relacionamentos (setas)
  - Botões: "Inject", "Link to task", "Compare"
  
- **Se Tool selecionado:**
  - Nome, status, risk
  - Latência, taxa de sucesso
  - Descrição completa
  - Botão: "Test manually"
  
- **Se Log selecionado:**
  - Timestamp, kind, duration
  - Texto completo
  - Task/session ID
  - Botão: "Open related artifacts"

**Current Action** sempre visível no rodapé do inspector:
- "Running Lua block"
- "Waiting for approval"
- "Spawning subagent sub-re-iram"
- "Streaming from qwen:14b"

---

## Status Line (Rodapé 1)

Exibe estado operacional em tempo real:

```
◉ Processing: Running vector.search · 3 subagents active · 12 pointers · 157 tokens · cost $0.0023
```

Elementos:
- **◉ Status:** ◉ (running), ◉ (paused), ○ (idle)
- **Current op:** Descrição breve do que está acontecendo
- **Subagents:** Número de workers em paralelo
- **Pointers:** Quantidade carregada em memória
- **Tokens:** Estimado até agora
- **Cost:** Custo estimado

---

## Command Bar (Rodapé 2)

**Hybrid:** Texto livre + slash commands

### Slash Commands

- `/model <name>` – Switch LLM model
- `/mode <ask|guide|agent|yolo>` – Change operation mode
- `/approve` – Approve pending tool call
- `/deny` – Reject pending approval
- `/plan` – Show current execution plan
- `/agents` – Show active subagents
- `/memory` – Show all pointers/vectors
- `/pointers` – Same as /memory
- `/tools` – List available tools
- `/mcp` – List MCP endpoints
- `/diff` – Show staged changes
- `/review` – Show git status
- `/commit <msg>` – Commit staged changes
- `/replay <task_id>` – Replay a task
- `/focus <task_id>` – Focus on specific task
- `/inject <pointer_id>` – Inject evidence into prompt
- `/help` – Show help
- `/stats` – Show session stats
- `/export <format>` – Export logs/pointers
- `/theme <dark|light>` – Switch theme
- `:command` – Vim-style mode (future)

**Auto-complete** on Tab key

---

## Operating Modes

### Ask (Blue)
- **Behavior:** Read-only diagnosis
- **Tools:** Allowed only if zero-risk (read, search, list)
- **Approvals:** Not needed
- **Writing:** No file writes, no git commits
- **Use:** Initial investigation, "what would happen if?"

### Guide (Yellow) ← **Default**
- **Behavior:** Propose and seek approval
- **Tools:** All, but each execution needs approval
- **Approvals:** Popup for tool calls, file writes, git ops
- **Writing:** Staged edits need preview + approval
- **Use:** Careful investigation with human oversight

### Agent (Green)
- **Behavior:** Auto-execute approved steps
- **Tools:** All, auto-execute within approved policy
- **Approvals:** No popup; policy-gated
- **Writing:** Auto-apply staged edits if policy allows
- **Use:** Autonomous but sandboxed investigation

### Yolo (Red)
- **Behavior:** Maximum autonomy inside sandbox
- **Tools:** All, immediate execution
- **Approvals:** None
- **Writing:** Direct writes, auto-commit
- **Use:** Fast iteration in isolated project

**Toggle:** `m` key or `/mode <name>`

---

## Data Structures (Recap)

```cpp
struct CockpitState {
    // Session
    std::string session_id;
    std::string workspace;
    std::string model;
    std::string branch;
    
    // Config
    AgentMode mode;
    bool sandbox_enabled;
    bool auto_approve;
    
    // UI State
    int selected_tab;
    int selected_task, selected_pointer, selected_tool, selected_log;
    bool show_inspector;
    
    // Dynamic
    std::string current_action;
    std::vector<std::shared_ptr<TaskNode>> root_tasks;
    std::vector<PointerItem> pointers;
    std::vector<ToolItem> tools;
    std::vector<LogLine> logs;
    std::vector<ApprovalItem> approvals;
    std::vector<DiffHunk> diff_hunks;
    std::vector<std::string> conversation;
};

struct TaskNode {
    std::string id, title, status, owner, summary;
    int depth;
    bool expanded;
    std::chrono::milliseconds duration;
    std::vector<std::shared_ptr<TaskNode>> children;
};

struct PointerItem {
    std::string id, summary, source;
    double relevance_score;
    std::vector<std::string> related_pointers;
};

struct LogLine {
    std::chrono::system_clock::time_point timestamp;
    LogKind kind;
    std::string task_id, session_id, text;
    std::optional<int> duration_ms;
};
```

---

## Keyboard Shortcuts

| Key | Action |
|-----|--------|
| Tab | Next tab |
| Shift+Tab | Prev tab |
| Arrow keys | Navigate (task tree, logs, pointers) |
| Enter | Open inspector / Confirm action |
| Space | Expand/collapse (tasks), Stage (diff) |
| Ctrl-I | Toggle inspector |
| F1 | Toggle help |
| `a` | Approve pending |
| `r` | Reject pending |
| `m` | Cycle mode (Ask→Guide→Agent→Yolo) |
| `/` | Command palette |
| `q` / Ctrl-C | Quit |

---

## Mouse Support

- **Scroll wheel:** Vertical scroll in logs/tasks/pointers
- **Click:** Select item, toggle tab, trigger approval
- **Drag:** Resize inspector panel (future)

---

## Next Steps (Implementation Roadmap)

### Phase 1: Core TUI (Now)
✅ Data model defined (CockpitState, TaskNode, etc.)
✅ Basic layout (header, tabs, footer)
✅ Tab navigation  
✅ Input bar with slash command parsing
⏳ Tab renderers (Chat, Tasks, Pointers, Diff, Tools, Logs, Review)
⏳ Inspector panel details

### Phase 2: Agent Integration
⏳ Connect `agent.run_step()` to UI
⏳ Stream LLM tokens into Chat tab
⏳ Update tasks tree on task lifecycle events
⏳ Append logs in real-time (Logs tab)
⏳ Handle approvals (popup, list in UI)

### Phase 3: Advanced Features
⏳ Diff viewer with hunk staging
⏳ Pointers injection into prompt
⏳ Git integration (commit, branch, status)
⏳ Sub-agent dashboard
⏳ Memory browser with search

### Phase 4: Polish
⏳ Mouse support for resize/select
⏳ Theme customization
⏳ Export logs/pointers
⏳ Session persistence
⏳ Error recovery

---

## Design Principles

1. **Clarity:** Every pane shows exactly what it's designed for. No clutter.
2. **Observability:** Timestamp, task ID, duration on every significant event.
3. **Agency:** Four modes ensure the human has the control they need.
4. **Integration:** Git, SQLite brain, Lua, LLM are all first-class citizens in the UI.
5. **Investigation:** The UX matches reverse-engineering, not chat assistants.

---

## References

Inspired by:
- **htop:** hierarchical, scrollable, real-time monitoring
- **lazygit:** modal, keyboard-driven, git-aware
- **Codex CLI / Claude Code:** multi-tab, approval center, diff viewer
- **Gemini CLI:** slash commands, mode switching, observability

But optimized for **technical investigation with persistent agent memory**.
