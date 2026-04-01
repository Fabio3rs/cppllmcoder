# Design Blueprint: cppllmcoder – A High-Performance Recursive Language Model for Firmware Reverse Engineering

This document synthesizes the concepts from the Recursive Language Models (RLM) paper, the architectural deep dive of Claude Code, and the user’s existing Python prototype into a concrete design for a **C++23 + Lua + SQLite** agent system. The goal is to create a local-first, performant, and sandboxed assistant specialized for analyzing embedded firmware (e.g., 8051 ECU dumps).

---

## 1. Core Insights from RLM and Claude Code

### 1.1 The “Dumb Scaffold, Smart Model” Principle
- Claude Code’s `QueryEngine` is a simple `while(true)` loop that delegates all intelligence to the LLM.
- The harness (C++ in our case) provides tools and context but does *not* reason.
- **Implication:** Our C++ engine should be a minimalistic executor, focusing on speed, safety, and persistence.

### 1.2 Recursive Sub‑Agents (RLM)
- The model can recursively call itself on subtasks by emitting code that spawns a sub‑agent.
- Each sub‑agent has its own context window and returns a summary.
- **Implementation:** Each sub‑agent runs in an isolated Lua VM. The parent receives only the final result (or a pointer to persisted data).

### 1.3 Context Compression via “Pointers” (Micro‑Summaries)
- Long conversations are compressed by replacing old tool outputs with short summaries stored as “pointers” (e.g., `P_42`).
- The model can later “dereference” a pointer to retrieve the full information if needed.
- **SQLite storage:** The summary text is kept in a `pointers` table, and the pointer ID is injected into the prompt. The engine lazily expands pointers when the model references them.

### 1.4 Multi‑Layer Security & Sandboxing
- Claude Code’s permission pipeline and OS‑level sandboxing (macOS Seatbelt, Linux bubblewrap) prevent arbitrary code execution.
- Our C++ engine will run Lua in a restricted environment, intercept all I/O and system calls, and proxy them through a permission‑checking layer.

### 1.5 Asynchronous Streaming & Error Recovery
- Claude Code uses `AsyncGenerator` to stream responses and retry on failures (429, 529, etc.).
- We can emulate this with C++ coroutines (`std::generator` or `cppcoro`) to yield intermediate results while awaiting model responses.

---

## 2. High‑Level Architecture of `cppllmcoder`

```
┌─────────────────────────────────────────────────────────────┐
│                     User / CLI / IDE                         │
└─────────────────────────────────────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────┐
│                     C++ Core Engine                          │
│  • Async I/O (files, network)                               │
│  • Coroutine‑based request loop                              │
│  • SQLite connection pool                                    │
│  • Lua VM pool (isolated per sub‑agent)                      │
│  • Permission & sandbox proxy                                │
└─────────────────────────────────────────────────────────────┘
                               │
                ┌──────────────┼──────────────┐
                ▼              ▼              ▼
┌───────────────────┐ ┌───────────────────┐ ┌───────────────────┐
│   Ollama /        │ │   SQLite          │ │   Filesystem      │
│   llama.cpp       │ │   (brain.db)      │ │   (firmware dumps,│
│   (model calls)   │ │   • tasks         │ │    .asm files)    │
└───────────────────┘ └───────────────────┘ └───────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────┐
│                 Lua VM (per sub‑agent)                       │
│  • Exposes tools as Lua functions                            │
│  • `rlm.spawn()` to create child agents                      │
│  • `db.pointer()` to store micro‑summaries                   │
│  • `vector.search()` for semantic lookup                     │
└─────────────────────────────────────────────────────────────┘
```

---

## 3. Component Breakdown

### 3.1 C++ Core Engine

**Technology:** C++23, using `std::generator` (C++23) for streaming, `std::expected` for error handling, and `std::span` for zero‑copy file I/O. Use `libcurl` or `cpr` for HTTP requests to Ollama.

**Responsibilities:**
- **Bootstrap:** Load configuration, connect to SQLite, initialise Lua VM pool.
- **Task loop:**
  1. Receive user input (or resume a pending task).
  2. Assemble system prompt with current task context (compressed pointers, active sub‑agents).
  3. Stream model response (via coroutine).
  4. Parse `<code>` tags and execute Lua snippets.
  5. Inject tool results back into the conversation.
  6. Repeat until final answer or task termination.
- **State persistence:** All tasks, pointers, and execution logs are stored in SQLite. The engine can crash and resume by reading the last incomplete task from the DB.

### 3.2 Lua Integration (Sol3)

**Why Lua:** Lightweight (sub‑2MB), fast, and easy to sandbox. Sol3 provides zero‑overhead C++/Lua binding.

**Exposed Functions:**
- `fs.read(path, offset, length)` – reads a slice of a file (respects sandbox).
- `fs.grep(pattern, path)` – fast regex search (using PCRE2 via C++).
- `db.pointer(id, summary)` – stores a micro‑summary and returns a pointer ID (or uses the given ID).
- `vector.search(query, limit)` – calls the embedding model (via Ollama) and performs a nearest‑neighbour search in the `vector_index` table.
- `rlm.spawn(description, task_data)` – creates a new child task, runs it in a fresh Lua VM, and returns the result.
- `log.event(message)` – records execution log.

**Isolation:** Each sub‑agent gets its own Lua state. After execution, the state is destroyed; only the result (or pointer) is kept.

### 3.3 SQLite Brain (Schema)

The schema already proposed is excellent. Let's refine it with practical notes:

```sql
-- tasks: root of the recursion tree
CREATE TABLE tasks (
    id TEXT PRIMARY KEY,          -- "T_001", etc.
    parent_task_id TEXT,
    description TEXT,
    status TEXT,                  -- pending, running, completed, failed
    created_at DATETIME,
    updated_at DATETIME,
    FOREIGN KEY (parent_task_id) REFERENCES tasks(id) ON DELETE CASCADE
);

-- pointers: context compression units
CREATE TABLE pointers (
    id TEXT PRIMARY KEY,          -- "P_42"
    task_id TEXT,                 -- which task created it (for cleanup)
    file_path TEXT,               -- source file (optional)
    offset_start INTEGER,         -- byte offset in binary file
    offset_end INTEGER,
    micro_summary TEXT NOT NULL,  -- the text injected into prompts
    created_at DATETIME,
    FOREIGN KEY (task_id) REFERENCES tasks(id) ON DELETE SET NULL
);

-- vector_index: using sqlite-vec (virtual table)
CREATE VIRTUAL TABLE vector_index USING vec0(
    embedding float[768],         -- dimension of nomic-embed-text
    pointer_id TEXT,              -- FK to pointers
    FOREIGN KEY (pointer_id) REFERENCES pointers(id) ON DELETE CASCADE
);

-- knowledge_graph: relationships between pointers
CREATE TABLE knowledge_graph (
    source_pointer_id TEXT,
    target_pointer_id TEXT,
    relationship_type TEXT,
    PRIMARY KEY (source_pointer_id, target_pointer_id, relationship_type),
    FOREIGN KEY (source_pointer_id) REFERENCES pointers(id) ON DELETE CASCADE,
    FOREIGN KEY (target_pointer_id) REFERENCES pointers(id) ON DELETE CASCADE
);

-- execution_logs: audit trail
CREATE TABLE execution_logs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    task_id TEXT,
    lua_script TEXT,              -- exact code emitted in <code> tag
    stdout_output TEXT,
    stderr_hints TEXT,
    tokens_used INTEGER,
    timestamp DATETIME,
    FOREIGN KEY (task_id) REFERENCES tasks(id) ON DELETE CASCADE
);
```

**Key Points:**
- `pointers` store the compressed knowledge. The `micro_summary` is what the model sees in the prompt (e.g., “P_42: function at 0x4F00 initialises K‑Line”).
- The engine injects these summaries into the system prompt at the beginning of each turn.
- When the model references a pointer (e.g., “look at P_42”), the engine can optionally expand it by retrieving the full context (file contents) and injecting it into the conversation. This mimics the “dereferencing” pattern.

### 3.4 Recursive Sub‑Agents (RLM)

**How it works:**
1. Model emits: `<code>rlm.spawn("analyze_tps", {address="0x46"})</code>`
2. C++ engine creates a new task in the `tasks` table with `parent_task_id` set to the current task.
3. A fresh Lua VM is instantiated, with its own copy of the `brain.db` connection (read‑only for safety, but can write to its own task and pointers).
4. The VM runs the provided script, which may perform file reads, grep, vector searches, and even spawn further sub‑agents.
5. When the script finishes, its result is stored (e.g., as a pointer or directly as text).
6. The engine then injects the result back into the parent’s conversation (as an `<subagent-result>` block).

**Isolation:**
- Each sub‑agent runs in its own process (or thread) with a separate Lua state.
- The parent waits for the child to complete (asynchronously) before proceeding.
- If the child fails (e.g., exceeds token limit), the engine can retry or escalate.

### 3.5 Context Compression via Pointers

**Problem:** The conversation grows with every turn, eventually exceeding the model’s context window.

**Solution:**
- After each tool use, the model can call `db.pointer(id, summary)` to create a pointer.
- The engine then replaces the original tool output in the message history with the pointer ID (e.g., “`[Tool result: P_42]`”).
- The full result is stored in the `pointers` table, but the model’s prompt only sees the summary.
- If the model later mentions `P_42`, the engine can inject the full content (or a larger slice) into the conversation again.

**Triggering:** The engine can also automatically compress old tool outputs after a certain number of turns, using a heuristic (e.g., >10K tokens or >5 turns old). It can call the model’s summarisation ability to generate the micro‑summary if the model hasn’t done so explicitly.

### 3.6 Sandbox & Security

**Two‑layer approach:**

1. **Lua sandbox:** Use `sol::state` with a restricted environment. Disable dangerous functions (`os.execute`, `io.popen`, `package.loadlib`). Provide only the whitelisted tools (fs.read, grep, etc.) as Lua functions that call into C++ proxies.
2. **OS‑level sandbox:** On Linux, use `unshare(CLONE_NEWUSER|CLONE_NEWNS)` and `seccomp` to prevent the process from accessing outside a chroot jail. On Windows, use AppContainer or Job Objects. The goal is that even if Lua escapes, the process cannot harm the host.

**Permission model:**
- The user can specify a `project_root` directory. All file access is relative to that root.
- The engine checks every `fs.read` call against a whitelist of allowed paths (e.g., only the firmware dump and its derived files).
- For sub‑agents, the engine can apply stricter permissions (e.g., read‑only, no network).

### 3.7 Vector Search (RAG)

**Why:** The model can ask “where is the K‑Line initialisation?” without knowing exact strings. The engine can convert that query into an embedding, search the `vector_index` for relevant pointers, and inject the top matches into the prompt.

**Implementation:**
- Use **nomic‑embed‑text** via Ollama (`/api/embeddings`) to generate 768‑dim embeddings.
- Store them in `sqlite-vec` virtual table.
- Provide a Lua function `vector.search(query, limit)` that returns a list of pointer IDs and their similarity scores.
- The model can then use `db.pointer` to fetch the full content of those pointers.

### 3.8 LLM Provider Abstraction

**Design:** A simple interface that can talk to:
- **Ollama** (default, local)
- **llama.cpp** (via `llama-server` HTTP API)
- **OpenAI / Anthropic** (for cloud fallback)

**C++ Implementation:** Use a `ModelClient` base class with virtual `stream_complete(prompt, history)` method returning a coroutine. Subclasses handle HTTP requests (using `libcurl` or `cpp-httplib`) and parse streaming JSON.

---

## 4. Workflow Example

**User:** “Analyze the K‑Line protocol in the firmware.”

1. **C++ engine:**
   - Creates a new task `T_001` in SQLite.
   - Assembles system prompt with initial knowledge (empty).
   - Sends prompt to Ollama.

2. **Model (first turn):**
   - Outputs: `<code>local hits = vector.search("K-Line initialisation") for _,p in ipairs(hits) do print(p) end</code>`
   - Engine runs the Lua code:
     - `vector.search` → C++ embeds the query, calls Ollama embeddings, performs SQLite `vec0` search, returns list of pointer IDs (initially empty).
     - Result: empty list.
   - Engine injects: `[Vector search returned 0 results]`.

3. **Model (second turn):**
   - Outputs: `<code>local content = fs.read("FiatTipoIDAFullDump_27c256.asm", 0x4F00, 200) print(content)</code>`
   - Engine checks permissions (allowed), reads the slice, returns it.
   - Model sees the assembly and identifies a pattern: “`mov SBUF, A` appears…”.

4. **Model (third turn):**
   - Outputs: `<code>db.pointer("P_001", "K-Line initialisation found at 0x4F00, uses timer 1 at 4800 baud")</code>`
   - Engine stores the pointer.
   - Now the pointer is available for future queries.

5. **Model (final turn):**
   - Outputs a textual answer summarising the findings.
   - Engine marks task `T_001` as completed.

Later, another user asks: “Where is the K‑Line initialisation?” The engine can inject `P_001`’s summary into the prompt immediately, saving time.

---

## 5. Implementation Roadmap

### Phase 1: Foundation (Weeks 1–2)
- Set up C++23 project with CMake.
- Integrate **SQLite3** (or sqlite_orm) and create the schema.
- Write a minimal **Lua + Sol3** wrapper that can execute a script and capture output.
- Implement a simple **Ollama client** (non‑streaming) and test with a local model.

### Phase 2: Core Loop (Weeks 3–4)
- Build the `while(true)` loop that:
  - Reads user input from stdin (or socket).
  - Sends prompt to model.
  - Parses `<code>` tags.
  - Executes Lua, collects results.
  - Injects results back and continues.
- Implement basic task creation and storage in SQLite.

### Phase 3: Recursion & Sandbox (Weeks 5–6)
- Implement `rlm.spawn` that creates a child task, launches a new Lua VM in a separate thread/process, and waits for its completion.
- Implement sandboxing: restrict Lua’s `os` and `io` libraries, use seccomp on Linux.
- Ensure that sub‑agents can also spawn their own children (recursive).

### Phase 4: Pointers & Compression (Weeks 7–8)
- Implement `db.pointer` and automatic compression of old tool outputs.
- Modify the prompt builder to include all active pointers as compressed context.
- Allow the model to “dereference” a pointer by calling a Lua function that returns the full content.

### Phase 5: Vector Search (Week 9)
- Integrate `sqlite-vec` as a static library or submodule.
- Add `vector.search` Lua function.
- Test with a small corpus of firmware documentation.

### Phase 6: Polish & Performance (Week 10)
- Use `std::generator` for streaming responses.
- Add configuration file (YAML/JSON) for paths, model endpoints, permissions.
- Add logging to SQLite for debugging.
- Write a simple CLI (REPL) for user interaction.

---

## 6. Potential Challenges & Mitigations

| Challenge | Mitigation |
|-----------|------------|
| **C++/Lua interop overhead** | Use Sol3’s “usertype” for efficient binding; batch operations where possible. |
| **SQLite concurrency** | Use a single writer thread; for reads, use `sqlite3_prepare_v3` with `SQLITE_PREPARE_PERSISTENT`. |
| **Embedding latency** | Cache embeddings in SQLite; use async embedding generation while the model is thinking. |
| **Sandbox escape** | Combine Lua restrictions with OS‑level namespaces; regularly audit the exposed API. |
| **Token limits in sub‑agents** | The child VM can also compress its own context using pointers; parent can set a `max_tokens` budget. |
| **Debugging recursive tasks** | Use `execution_logs` table to store every script and its output; provide a `--verbose` flag to replay them. |

---

## 7. Conclusion

The `cppllmcoder` project marries the proven ideas from Claude Code and the RLM paper with a modern C++ foundation, resulting in a fast, safe, and extensible system for reverse engineering. By leveraging Lua for tool calling, SQLite for persistent memory, and local LLMs via Ollama, we achieve a **local‑first, privacy‑respecting** assistant that can recursively decompose complex firmware analysis tasks.

The provided SQLite schema and component architecture give a clear path forward. The next steps are to start with Phase 1 – building the basic C++/Lua/SQLite harness – and iteratively add features while maintaining the core principles: **dumb scaffold, smart model, and rigorous safety**.

*This document serves as the blueprint for the implementation. As development progresses, the design may evolve, but the high‑level concepts remain anchored in the RLM and Claude Code analyses.*
