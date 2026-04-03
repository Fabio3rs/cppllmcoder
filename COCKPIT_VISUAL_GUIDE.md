# CPP-LLM-CODER Cockpit – Visual Guide

## The Full Dashboard (Annotated)

```
╔═══════════════════════════════════════════════════════════════════════════════╗
║         CPP-LLM-CODER mission: Analyze firmware.bin | mode GUIDE              ║
╚═══════════════════════════════════════════════════════════════════════════════╝

╔═══════════════════════════════════════════════════════════════════════════════╗
║ model: qwen2.5:14b | ws: ./firmware | branch: analysis/kline | session: ...  ║
║ sandbox: ON | auto-approve: OFF                                              ║
╚═══════════════════════════════════════════════════════════════════════════════╝

╔════════════════════════════════════════════════════════════════════════════════╗
║ [Chat/Plan]  [Tasks]  [Pointers]  [Diff]  [Tools]  [Logs]  [Review]           ║
╚════════════════════════════════════════════════════════════════════════════════╝

┌─────────────────┬──────────────────────────────────┬─────────────────────┐
│  TASK TREE      │  MAIN CONTENT                    │  INSPECTOR          │
│  (Left Panel)   │  (Center – Tab dependent)        │  (Right Panel)      │
├─────────────────┼──────────────────────────────────┼─────────────────────┤
│ T-001 (running) │ >> Agent Response Streaming      │ Task: T-001         │
│   T-002 (pend.) │ "I found K-Line init at 0x4F00" │ Status: running     │
│   T-003 (done)  │                                  │ Owner: main         │
│ T-004 (pend.)   │ Plan:                            │ Duration: 45ms      │
│                 │ 1. Search vector DB              │ ───────────────────│
│                 │ 2. Spawn sub-agent              │ Current Action:     │
│                 │ 3. Validate candidate           │ Spawning subagent   │
│                 │                                  │ sub-re-kline        │
└─────────────────┴──────────────────────────────────┴─────────────────────┘

╔════════════════════════════════════════════════════════════════════════════════╗
║ ◉ Running: vector.search · 3 subagents active · 12 pointers · 189 tokens     ║
╚════════════════════════════════════════════════════════════════════════════════╝

╔════════════════════════════════════════════════════════════════════════════════╗
║ ❯ /inject P_42  [Tab to autocomplete]  or type /help for slash commands       ║
╚════════════════════════════════════════════════════════════════════════════════╝
```

---

## Tab Content Examples

### 1. Chat/Plan Tab

```
>> Agent is thinking...

Agent Response (streaming):
"I'll search the vector database for K-Line initialization patterns
 and cross-reference with your recent pointers. Let me also spawn a 
 focused worker to analyze candidate functions..."

Plan (derived from ReAct loop):
 1. vector.search('K-Line initialization') → 0.87 match
 2. rlm.spawn('Analyze candidate at 0x4F00') → sub-re-kline
 3. compare_with(P_42, P_91) → assess correlation
 4. propose_edit_range(0x4F00 - 0x4F80) → staging area

Current Hypothesis:
K-Line handler is at firmware.bin:0x4F00, linked to checksum
validation via pointer P_91. Sub-agent validating.
```

### 2. Tasks Tab (Tree View)

```
T-001 (running)  [20 sec]  main  "Root: Analyze firmware"
 ├─ T-002 (running)  [15 sec]  main  "Load memory + search vectors"
 │  └─ T-003 (done)  [4 sec]   main  "Vector DB query K-Line" ✓
 │
 ├─ T-004 (pending)  [--]     sub-re-kline  "Spawn candidate analysis"
 │  └─ (not yet created)
 │
 └─ T-005 (failed)  [3 sec]   sub-re-xram  "Scan checksum writers"
    Error: Policy denial (high-risk shell command)

Legend: ◉ = running, ◯ = pending, ✓ = done, ✗ = failed
```

### 3. Pointers Tab (Memory Browser)

```
P_42 (score: 0.92) [firmware.bin:0x4F00]
  K-Line initialization routine begins near offset 0x4F00.
  Linkage: USES_TABLE → P_91, CALLS_FUNCTION → P_114
  Updated: 2min ago

P_91 (score: 0.87) [firmware.bin:0x8C20]
  Checksum validation references table. Likely init sequence.
  Linkage: TARGET_OF ← P_42
  Updated: 5min ago

P_114 (score: 0.64) [firmware.bin:0x19A0]
  Watchdog reset path touches IO register 0x11.
  Status: Candidate (low confidence)
  Updated: 8min ago

Commands:
  [E] Edit | [I] Inject | [L] Link to task | [D] Delete | [C] Compare
```

### 4. Diff Tab (Staged Changes)

```
File: src/handler_kline.cpp

Hunk 1/3 [STAGED]
═════════════════════════════════════════════
  139    bool init_kline() {
- 140        uint8_t buffer[64];
+ 140        uint8_t buffer[128];  // Increased size per analysis
  141        // Original init call
  142        return send_init_sequence(buffer);

Hunk 2/3 [UNSTAGED]
═════════════════════════════════════════════
  201    void handle_rx() {
- 202        if (checksum_validate(rx_data, rx_len)) {
+ 202        if (validate_checksum_crc16(rx_data, rx_len, 0x8C20)) {
  203            process_command();
  204        }
  205    }

Hunk 3/3 [UNSTAGED]
═════════════════════════════════════════════
+ 310    // New: K-Line state machine (proposed by agent)
+ 311    static kline_state_t current_state = KLINE_IDLE;
+ 312

Commands:
  [S] Stage hunk | [R] Reject hunk | [P] Preview | [A] Apply all | [C] Commit
```

### 5. Tools Tab (Capabilities)

```
fs.read
  Status: ready | Risk: low | Latency: 3ms
  Desc: Read file contents from workspace
  Last call: 45sec ago (success)

vector.search
  Status: ready | Risk: low | Latency: 14ms
  Desc: Search semantic memory pointers in SQLite vec DB
  Last call: 2sec ago (success)

rlm.spawn
  Status: ready | Risk: medium | Latency: 7ms
  Desc: Spawn isolated sub-agent with Lua VM
  Last call: 18sec ago (success)

db.query
  Status: ready | Risk: low | Latency: 4ms
  Desc: Query structured agent memory
  Last call: 30sec ago (success)

sh
  Status: restricted | Risk: high | Latency: 24ms
  Desc: Sandboxed shell command execution
  Last call: 5min ago (denied by policy)
  Policy: Only if explicit approval in Guide mode

Commands:
  [I] Inspect | [T] Test | [L] Logs for this tool | [P] Policy
```

### 6. Logs Tab (Execution Trace)

```
12:14:01.042 [SYS]   T-001  Runtime initialized with SQLite brain and Lua VM
12:14:01.057 [DB]    T-001  Connected to .cppllmcoder/brain.db (3 pointers loaded)
12:14:01.212 [AGENT] T-001  Loaded 17 recent pointers into compact mission context
12:14:02.005 [LLM]   T-001  002ms Streaming response from qwen2.5-coder:14b
12:14:03.129 [LUA]   T-001  124ms Extracted <code> block with vector.search and rlm.spawn
12:14:03.144 [TOOL]  T-001  014ms vector.search('K-Line initialization') → 0.87 match
12:14:03.211 [TOOL]  T-001  007ms rlm.spawn('sub-re-kline', goal='Inspect 0x4F00') → success
12:14:03.987 [AGENT] T-003  ✓ Child agent created pointer P_42 from candidate function
12:14:04.014 [WARN]  T-004  ⚠ Checksum scan queued (max parallel agents reached)
12:14:04.102 [FS]    T-005  ✗ Policy denied: fs.write outside workspace boundary

Legend:
  SYS=System, LLM=Model, LUA=Lua VM, TOOL=Tool call, DB=Database,
  AGENT=Agent state, MCP=Model Context Protocol, FS=File system,
  WARN=Warning, ERR=Error
  
  Duration on left (e.g., "002ms")
  Task ID on right (e.g., "T-001")

Commands:
  [F] Filter by kind | [S] Search text | [E] Export | [C] Clear
```

### 7. Review Tab (Git + Workspace)

```
Branch: analysis/kline (ahead of origin/main by 2 commits)
Sandbox: ON (workspace isolated to ./firmware_dump)
Staged changes: 3 files, 47 insertions, 12 deletions

Recent commits:
  373c098 Redesign main interface as professional cockpit (2min ago)
  a9f42d1 Add observability with structured logging (4min ago)
  e2c3d0e Pointer injection into prompt context (12min ago)

Proposed by agent (in staging area):
  M src/handler_kline.cpp  (+8 -2)
  M include/handler_kline.hpp  (+3 -1)
  ? analysis/notes.md (new file)

Approval needed:
  ☐ Commit to branch analysis/kline?
  ☐ Consider PR to main?

Commands:
  [S] Show staged diff | [U] Unstage all | [C] Commit | [P] Push | [R] Reset
```

---

## Keyboard Flow Example

### Starting a Session

```
$ ./cppllmcoder --model qwen2.5:14b --workspace ./firmware_dump

[TUI opens, Tab 0: Chat/Plan]

User types:    "Analyze this firmware, find K-Line handlers"
User presses:  Enter

[Agent starts running]
Tokens stream:  "I'll search the vector database..."
Inspector shows: "Current Action: vector.search running"

[After 5 seconds]
Tab 1 (Tasks) updates: T-002 created and appears
Inspector updates: Shows latest task details

[After 10 seconds]
Tab 5 (Logs) shows: Tool success, pointer created, etc.
Approvals popup: "Execute rlm.spawn with risk=medium?"

User presses:  a  (approve)
TUI shows:     [Tool] rlm.spawn success
               Sub-agent sub-re-kline started

[Investigation continues...]
User presses:  Tab  (switch to Pointers tab)
Shows:         New pointer P_42 and related findings

User types:    /inject P_42
Response:      "Pointer P_42 injected into prompt"

[Agent continues with injected context...]
User presses:  Tab → Tab → Tab  (switch to Diff tab)
Shows:         3 hunks of proposed changes

User presses:  Space  (on hunk 1)
Hunk 1 staged: ✓

User types:    /commit "Add K-Line handler analysis"
Response:      "Committed to analysis/kline branch"

User presses:  m  (toggle mode)
Mode changes:  GUIDE → AGENT
Status bar:    Mode now AGENT (green)

[Agent now auto-executes within policy...]
```

---

## Command Palette (Slash Commands)

Press `/` to open command palette:

```
/ [█████████████████████████] ← type to filter

/approve ...................... Approve first pending action
/commit "<msg>" ................ Commit staged changes
/deny ......................... Reject pending approval
/diff ......................... Show staged changes
/export <format> .............. Export logs/pointers
/focus <task_id> .............. Focus on specific task
/help ......................... Show this help
/inject <pointer_id> .......... Inject evidence into prompt
/memory ....................... Show all pointers/vectors
/mcp .......................... List MCP endpoints
/mode <ask|guide|agent|yolo> .. Change operation mode
/model <name> ................. Switch LLM model
/plan ......................... Show current execution plan
/replay <task_id> ............. Replay a task
/review ....................... Show git status
/stats ........................ Show session stats
/tools ........................ List available tools
```

Press Enter to execute, or Escape to close.

---

## Mode Visual States

```
┌─────────────────────┐
│ Mode: ASK (BLUE)    │  🔵 Diagnostic, no modifications
│                     │  Read-only safe mode
└─────────────────────┘

┌─────────────────────┐
│ Mode: GUIDE (YELLOW)│ 🟡 Default; proposes + awaits approval
│ ⚠ Approval pending  │  Green "Approve" button in inspector
└─────────────────────┘

┌─────────────────────┐
│ Mode: AGENT (GREEN) │ 🟢 Auto-executes within policy
│ ◉ Running           │  No popups; policy-gated
└─────────────────────┘

┌─────────────────────┐
│ Mode: YOLO (RED)    │ 🔴 Maximum autonomy (sandboxed)
│ ⚡ High autonomy    │  Use with caution
└─────────────────────┘
```

Toggle with:
- Keyboard: `m` key cycles through modes
- Command: `/mode <name>`

---

## Inspector Panel States

### When Task Selected

```
╔══════════════════════════════╗
│ Inspector                    │
├──────────────────────────────┤
│ Task: T-002                  │
│ Status: running              │
│ Owner: main                  │
│ Duration: 15 sec             │
│                              │
│ Summary:                     │
│ Load memory + search         │
│ vectors for K-Line patterns  │
│                              │
│ [Focus on this task]         │
├──────────────────────────────┤
│ Current Action:              │
│ Querying SQLite vector DB    │
╚══════════════════════════════╝
```

### When Pointer Selected

```
╔══════════════════════════════╗
│ Inspector                    │
├──────────────────────────────┤
│ Pointer: P_42                │
│ Score: 0.92                  │
│ Source: firmware.bin:0x4F00  │
│                              │
│ K-Line initialization        │
│ routine begins near offset   │
│ 0x4F00. Linkage to checksum  │
│ validation via P_91.         │
│                              │
│ Related:                     │
│   → P_91 (checksum)          │
│   → P_114 (watchdog)         │
│                              │
│ [Inject] [Link] [Compare]    │
├──────────────────────────────┤
│ Current Action:              │
│ Injected P_42 into prompt    │
╚══════════════════════════════╝
```

### When Tool Selected

```
╔══════════════════════════════╗
│ Inspector                    │
├──────────────────────────────┤
│ Tool: rlm.spawn              │
│ Status: ready                │
│ Risk: medium                 │
│ Latency: 7 ms (avg)          │
│                              │
│ Spawn isolated sub-agent     │
│ with Lua VM and bounded      │
│ context window               │
│                              │
│ Policy: Requires manual      │
│ approval in Guide mode       │
│                              │
│ [Test manually] [Logs]       │
├──────────────────────────────┤
│ Current Action:              │
│ Waiting for approval         │
╚══════════════════════════════╝
```

---

## Mouse Support (FTXUI Capable)

- **Scroll wheel** in any tab scrolls content
- **Click on tab name** switches tab
- **Click on task** selects it, updates inspector
- **Click `[Approve]` button** approves pending action
- **Click `[Inject]` button** injects pointer
- **Drag inspector edge** resizes panel (future)

---

## Error/Warning States

```
╔════════════════════════════════════════════════════════════════════╗
║ ⚠ Policy denial: fs.write outside workspace boundary               ║
║   (Tool: sh | Risk: high | Mode: GUIDE)                           ║
║   Action: None. Awaiting user instruction.                        ║
╚════════════════════════════════════════════════════════════════════╝

Tab: Logs
  12:14:04.102 [ERR]  T-005  Policy denied: fs.write outside workspace

Tab: Tools
  sh: Status → restricted (denial count: 1)
```

---

## Performance Indicators

```
Status Line Updates:
- ◉ Running       [agent is executing]
- ◉ Paused        [awaiting approval]
- ○ Idle          [awaiting user input]

Token Counter:
- "189 tokens" → Running total for session
- Updates as LLM streams

Cost Estimator:
- "$0.0023" → Cumulative API cost (if cloud model)

Subagent Counter:
- "3 subagents" → Active parallel workers
```

---

## Customization & Themes

Currently:
- Dark theme (hardcoded)
- Color palette fixed (Green, Yellow, Blue, Red, Cyan, Magenta)

Future:
- `/theme dark` / `/theme light`
- Custom color schemes in config file
- Font/layout preferences

---

This guide covers the **visual interface and interactive patterns**. For implementation details, see `IMPLEMENTATION_PLAN.md`.
