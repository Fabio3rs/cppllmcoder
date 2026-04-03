# UPDATE: Cockpit TUI Interface

## Status

The main application now features a **professional terminal cockpit interface** designed for technical investigation and reverse engineering, not just chat.

The interface replaces the simple line-by-line REPL with a **multi-pane, tab-based dashboard** that surfaces:

- Task hierarchies and recursive decomposition
- Persistent memory (pointers) with semantic search
- Real-time streaming logs with structured types (LLM, Lua, Tool, DB, Agent, FS, etc.)
- Staged diff viewer for proposed code changes
- Approval center for safety-gated operations
- Git integration (branch, commit, review)
- Sub-agent monitoring

## Running the Cockpit

```bash
cd build/
./cppllmcoder --model qwen2.5-coder:14b --workspace ./firmware_dump
```

The TUI starts immediately. Use Tab to switch abas, `m` to cycle modes, `/help` for commands.

## Interface Modes

| Mode | Color | Use Case |
|------|-------|----------|
| **Ask** | 🔵 | Read-only diagnosis, no side effects |
| **Guide** | 🟡 | Default; proposes changes, waits for approval |
| **Agent** | 🟢 | Auto-executes within policy, no popups |
| **Yolo** | 🔴 | Maximum autonomy inside sandbox |

Toggle with `m` key or `/mode <name>` command.

## 7 Main Tabs

1. **Chat/Plan** – Agent response + execution plan
2. **Tasks** – Hierarchical task tree with status/duration
3. **Pointers** – Memory/evidence browser with relevance scores
4. **Diff** – Staged changes with hunk-level control
5. **Tools** – Available capabilities + risk/policy
6. **Logs** – Timestamped events with type and duration
7. **Review** – Git status, branches, pending commits

## Key Shortcuts

| Key | Action |
|-----|--------|
| Tab | Next tab |
| Shift+Tab | Previous tab |
| `m` | Cycle mode (Ask→Guide→Agent→Yolo) |
| `a` | Approve pending action |
| `r` | Reject pending action |
| Ctrl-I | Toggle inspector panel |
| `/` | Command palette |
| F1 | Help |
| q / Ctrl-C | Quit |

## Slash Commands

```
/model <name>          Switch LLM model
/mode <ask|guide|agent|yolo>  Change operation mode
/approve               Approve pending tool call
/deny                  Reject pending approval
/plan                  Show current execution plan
/agents                Show active subagents
/memory                Show all pointers/vectors
/tools                 List available tools
/mcp                   List MCP endpoints
/diff                  Show staged changes
/review                Show git status
/commit "<msg>"        Commit staged changes
/inject <pointer_id>   Inject evidence into prompt
/help                  Show help
/stats                 Show session stats
```

## Implementation Roadmap

### ✅ Phase 1: Core TUI (Complete)
- Data model (CockpitState, TaskNode, PointerItem, LogLine, etc.)
- 7-tab layout with FTXUI
- Keyboard navigation and shortcuts
- 4 operating modes
- Tab renderers and inspector panel

### ⏳ Phase 2: Agent Integration (Next)
1. **CockpitAgentDriver** – Stream tokens into Chat tab in real-time
2. **Task Tree Sync** – Tasks appear as agent creates/updates them
3. **Structured Logs** – All events logged with timestamp, type, duration
4. **Approval Center** – Safety gates in Guide mode
5. **Diff Viewer** – Proposed edits with hunk staging

### ⏳ Phase 3: Advanced Features
- Pointer injection into prompt context
- Memory browser with semantic search
- Git integration (branch, commit, review)
- Sub-agent dashboard
- Timeline of investigation

### ⏳ Phase 4: Polish
- Mouse support for drag/resize
- Theme customization
- Export logs/pointers
- Session persistence
- Error recovery

## Design Philosophy

The cockpit is inspired by:
- **htop** – hierarchical, real-time monitoring
- **lazygit** – modal, keyboard-driven, purpose-built
- **Codex/Claude Code** – multi-tab, approval center, diff viewer
- **Gemini CLI** – slash commands, mode switching, observability

But optimized for **reverse engineering and technical investigation**, not chat.

### Key Principles
1. **Clarity** – Every pane shows exactly what it's designed for
2. **Observability** – Timestamp, task ID, duration on every event
3. **Agency** – Four modes ensure human control over agent autonomy
4. **Integration** – Git, SQLite brain, Lua, LLM are first-class UI citizens
5. **Investigation** – UX matches reverse engineering, not messaging

## Documentation

- **COCKPIT_SPEC.md** – Complete interface specification
- **IMPLEMENTATION_PLAN.md** – Concrete tasks and roadmap
- **COCKPIT_SESSION_SUMMARY.md** – This session's achievements

## Next Steps

See `IMPLEMENTATION_PLAN.md` for the concrete checklist. The priority order is:

1. CockpitAgentDriver (stream tokens)
2. Task tree sync (real-time updates)
3. Structured logging (timestamp + kind)
4. Approval gate (safety checks)
5. Diff viewer (edit staging)

All infrastructure is in place; remaining work is integration.

---

**Last Updated:** 3 de abril de 2026
