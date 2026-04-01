# cppllmcoder

**cppllmcoder** is a local-first, high-performance recursive coding and reverse-engineering agent built for large codebases, firmware images, and long-horizon technical analysis.

The project combines a **C++23 orchestration engine**, **embedded Lua tool-calling**, and a **SQLite-based persistent memory** to create an agent that can reason over large contexts without collapsing under prompt bloat.

Its design is inspired by three practical observations:

1. Long-context work breaks when all state is shoved directly into the prompt.
2. Reverse engineering benefits from persistent memory, symbolic pointers, and recursive decomposition.
3. A fast, dumb scaffold with a powerful model often scales better than a complex scaffold with fragile heuristics.

## Vision

The goal of `cppllmcoder` is to become a **"Formula 1" class local agent** for:

* firmware reverse engineering
* binary and disassembly analysis
* large codebase navigation
* recursive decomposition of technical tasks
* persistent agent memory across sessions and crashes
* safe, sandboxed tool execution

It is designed to be **local-first by default**, using Ollama or llama.cpp for privacy-sensitive workloads, while still allowing optional delegation to cloud models when necessary.

## Core Principles

### 1. Fast core, minimal scaffold

The runtime should do as little "thinking" as possible.

The C++ engine is responsible for:

* managing task execution
* streaming model responses
* parsing tool calls
* persisting state
* enforcing sandbox boundaries
* spawning isolated recursive workers

The model remains the primary planner.

### 2. Long-context by indirection

Instead of forcing the model to carry the whole mission history inside the context window, old findings are compressed into durable references called **Pointers**.

A pointer stores:

* where the evidence came from
* a compact summary suitable for prompt injection
* enough metadata to re-expand the underlying evidence later

This keeps the active context small while preserving recoverability.

### 3. Recursive task decomposition

Complex technical work should be decomposed into smaller focused subtasks.

A task can spawn sub-agents that each receive:

* a narrow goal
* a bounded context window
* a limited tool set
* an isolated Lua VM

The parent agent receives only the distilled result.

### 4. Persistent memory as infrastructure

The memory of the system is not an afterthought. It is a first-class component.

SQLite acts as the project brain and stores:

* task trees
* pointer summaries
* vector embeddings
* knowledge graph relations
* execution logs

This allows the agent to survive crashes, resume unfinished work, and maintain mission continuity across sessions.

### 5. Sandboxed capability

The model must not have unrestricted access to the operating system.

All side-effectful operations are mediated by the runtime and validated against policy. The intent is to combine:

* restricted Lua environments
* OS-level sandboxing
* explicit approval or policy gates for dangerous operations

## High-Level Architecture

```text
User / CLI / IDE
      |
      v
C++23 Core Engine
  - orchestration
  - async I/O
  - model streaming
  - task scheduler
  - sandbox / policy layer
  - SQLite persistence
  - Lua VM management
      |
      +--> Ollama / llama.cpp / REST LLM providers
      +--> SQLite brain.db
      +--> filesystem / firmware / codebase inputs
      +--> isolated Lua sub-agent VMs
```

## Main Components

## 1. Core Engine

**Technology:** C++23

The core engine is the runtime heart of the system.

Planned responsibilities:

* high-performance orchestration loop
* zero-copy or low-copy file access where possible
* concurrent execution without unnecessary blocking
* robust error handling with `std::expected`
* coroutine-based streaming and task coordination
* resumable execution after crash or interruption

Important implementation ideas:

* `std::span` for efficient buffer views
* coroutines / generators for streaming workflows
* SIMD-assisted search for large binary scanning
* strict separation between orchestration and reasoning

## 2. Tool-Calling Language

**Technology:** Lua 5.4 + Sol3

Instead of verbose JSON tool calls, the model emits executable logic inside `<code>` tags.

Example shape:

```xml
<code>
local hits = vector.search("K-Line initialization")
local result = rlm.spawn("analyze_hits", hits)
return result
</code>
```

Why Lua:

* extremely lightweight embedding
* simple syntax for models to learn
* excellent C++ interoperability through Sol3
* much lower ceremony than schemas with deeply nested JSON

The intended model is not "function calling" in the usual API sense, but **programmable interaction with a controlled runtime**.

## 3. Persistent Brain

**Technology:** SQLite

The persistent brain lives at:

```text
.cppllmcoder/brain.db
```

This database stores the operational memory of the agent.

Planned contents:

* tasks and subtasks
* compressed findings (Pointers)
* semantic search index
* knowledge graph edges
* execution and audit logs

The purpose is not just storage, but **state continuity**.

## 4. Hybrid Semantic Retrieval

**Technology:** sqlite-vec / sqlite-vss

The system should support both:

* symbolic / lexical lookup
* embedding-based semantic search

This enables queries such as:

* "where is the checksum logic?"
* "find the K-Line initialization path"
* "show summaries related to watchdog reset"

without requiring external vector infrastructure.

## 5. Context Compression

**Pattern:** Pointers + micro-summaries

Long tool outputs and intermediate discoveries should not remain raw in the prompt forever.

Instead, they are compressed into structures like:

* `P_42`: K-Line init routine likely begins near offset `0x4F00`
* `P_91`: checksum validation references table at `0x8C20`

The model can later dereference these IDs when deeper inspection is needed.

This keeps prompt growth under control while preserving traceability.

## 6. Recursive Language Model Execution

**Pattern:** disposable sub-agents

When a task becomes too broad, the coordinator can spawn focused workers.

Example:

* root task: analyze ECU firmware
* child task: identify serial protocol handlers
* grandchild task: inspect candidate function at offset `0x4F00`

Each worker runs with a reduced context and reports back only the essential result.

This is meant to mirror recursive long-context reasoning without forcing a monolithic prompt.

## 7. Sandbox and Safety

**Approach:** runtime mediation + OS isolation

The model should never directly control unrestricted I/O.

Planned safety layers:

* whitelisted filesystem access
* mediated write operations
* controlled process execution
* Linux namespaces / cgroups where available
* Windows AppContainer equivalent when applicable
* IPC boundary between model intent and privileged action

## 8. Flexible Model Endpoints

**Default:** Ollama / llama.cpp

The system should prefer local inference for privacy-sensitive reverse-engineering tasks.

Supported direction:

* Ollama
* llama.cpp server
* optional REST APIs for remote models

Provider selection should be configured in `config.yaml`.

## Example Task Lifecycle

### User request

> Analyze the binary and find the K-Line handlers.

### Coordinator behavior

1. Create or resume a root task.
2. Load compressed project memory from SQLite.
3. Build a compact mission context.
4. Send the task to the selected model.

### Model behavior

The model emits Lua code that searches relevant summaries, inspects candidate regions, and optionally spawns recursive workers.

### Runtime behavior

The C++ engine:

* executes the Lua snippet
* resolves vector or grep queries
* persists findings into SQLite
* records logs
* injects back only the useful result summary

### Final effect

The conversation remains focused, while the full working memory remains durable in the background.

## Initial Database Direction

The current design revolves around five foundational structures:

### Tasks

A tree of root tasks and recursive subtasks.

### Pointers

Compressed references to findings, snippets, offsets, or code regions.

### Vector Index

Embeddings linked to pointers for semantic lookup.

### Knowledge Graph

Relations such as:

* `CALLS_FUNCTION`
* `WRITES_REGISTER`
* `USES_TABLE`
* `DEFINES_STRUCT`

### Execution Logs

A trace of what code the model emitted and what the runtime observed.

## Proposed Early Milestones

### Milestone 1 — brain.db bootstrap

Build the initial SQLite schema and validate the persistence model.

Deliverables:

* schema migration bootstrap
* task creation and recovery
* pointer insertion and lookup
* execution log recording

### Milestone 2 — C++ + Lua Hello World

Embed Lua through Sol3 and execute `<code>` blocks safely.

Deliverables:

* parse `<code>` tags from model output
* run simple Lua snippets
* expose a minimal safe API
* return structured results to the coordinator

### Milestone 3 — first compressed context loop

Introduce pointers and use them in the runtime prompt assembly.

Deliverables:

* pointer creation API
* summary injection into prompt
* dereference flow
* replacement of old tool output with pointer references

### Milestone 4 — first recursive sub-agent

Allow a task to spawn a child task with an isolated Lua VM.

Deliverables:

* `rlm.spawn()` primitive
* child task persistence
* result return path to parent
* failure recovery for child tasks

### Milestone 5 — semantic retrieval

Attach embeddings and hybrid lookup to the persistent brain.

Deliverables:

* embedding generation pipeline
* `sqlite-vec` integration
* `vector.search()` Lua binding
* prompt injection of best matches

## Recommendation for What to Build First

The most strategic starting point is:

### 1. SQLite schema first

Because everything else depends on durable state.

Without the brain, the runtime is only a transient agent loop. With the brain in place, every later subsystem gains continuity.

Suggested order:

1. schema and migrations
2. pointer CRUD and task CRUD in C++
3. minimal Lua embedding
4. `<code>` execution loop
5. recursive task spawning
6. semantic retrieval

If the goal is maximum immediate fun instead of maximum leverage, then the second-best starting point is the **C++23 + Lua Hello World**, because it gives the first visible end-to-end taste of the system.

## Non-Goals for the Earliest Version

To keep the first version sharp, the project should avoid trying to solve everything at once.

Not required on day one:

* full autonomous editing of arbitrary projects
* distributed orchestration
* cloud-native vector databases
* GUI-heavy workflows
* complex multi-user coordination
* perfect decompiler integration

The first version only needs to prove that:

1. the model can emit Lua tool logic reliably
2. the runtime can persist and compress discoveries
3. recursive subtasks improve technical analysis

## Current Status

This repository currently represents the **architectural intent** of the project.

It is a design-first foundation for a future implementation focused on:

* performance
* persistence
* recursive reasoning
* safe tool execution
* reverse engineering workloads

## Future Directions

Planned future exploration may include:

* integration with Capstone / Keystone / Ghidra workflows
* binary structure discovery pipelines
* pointer-aware prompt compilers
* replayable execution traces for debugging model behavior
* symbolic relation extraction for firmware knowledge graphs
* richer planner policies for recursive task budgeting

## Project Philosophy

`cppllmcoder` is not meant to be a generic chat wrapper around an LLM.

It is meant to be a **durable technical agent runtime**:

* fast enough for serious engineering work
* structured enough to survive long investigations
* safe enough to run locally on sensitive artifacts
* flexible enough to let the model program its own investigative path

In short:

> Keep the runtime lean, the memory durable, the tools sharp, and the model focused.
