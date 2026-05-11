# Sub-Agent Parent Architecture Plan

## Summary
The current agent loop already supports queued message injection at turn boundaries, which makes it a good base for parent-child coordination.

The next step is to formalize two distinct parent/child channels:

- `report_parent(...)` for one-way telemetry
- `ask_parent(...)` for blocked handoff that parks the child until a reply arrives

The parent should be treated as a user-like source of injected context, but with orchestration tools that are not available to children.

## Current State
- `Agent::run_step(...)` drains `driver.next_injection()` before each turn.
- `CockpitAgentDriver` already behaves like a user-facing driver with queued injections.
- `AgentManager` already tracks agent identity, parent links, status, and inject/stop control.
- The manager now has a parked-child scaffold:
  - `AgentState::Waiting`
  - `park_agent(...)`
  - `wake_agent(...)`
  - `waiting_reason(...)`

## Planned Behavior
- Parent-only tools:
  - `subagents_status()`
  - `peek_subagent(id)`
  - `inject_subagent(id, message)`
  - `stop_subagent(id)`
- Child-only tools:
  - `report_parent(text)`
  - `ask_parent(question)`
  - `done_task()`

### `report_parent`
- Non-blocking.
- Emits a child-to-parent event.
- Optionally injects a distilled summary into the parent context.
- Does not pause the child loop.

### `ask_parent`
- Blocks the child at a turn boundary, not mid-stream.
- Marks the child as waiting in `AgentManager`.
- Returns control from `run_step()` so the manager can park the child thread or task.
- Resumes only when the parent injects a reply.

### `done_task`
- Remains the normal completion signal.
- Ends the child’s active work loop.

## Implementation Notes
- Keep injections queued, not live-interrupting.
- Reuse the existing `inject()/next_injection()` path for parent replies.
- Avoid a new transport layer; the missing piece is lifecycle state, not messaging.
- Do not make `on_tool_result()` automatically forward raw tool output to the parent.

## Test Plan
- Verify `report_parent(...)` produces a parent-visible event without blocking.
- Verify `ask_parent(...)` parks the child and requires an injected reply to resume.
- Verify parent-only tools are visible only to the parent context.
- Verify child tool results remain local unless explicitly promoted.
- Verify parked children transition back to running when `inject_subagent(...)` or `wake_agent(...)` is called.

## Open Questions
- Whether parked children should be re-run by a manager-owned worker thread or by the parent caller directly.
- Whether `ask_parent` should resume with a plain injected user message or with a typed parent-response event that is rendered as user context.
- Whether `report_parent` should always inject into the parent context or only post an event for the UI/orchestrator to consume.
