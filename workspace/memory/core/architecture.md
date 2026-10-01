# Architecture

[2026-10-01 12:35:20] - `Source/Pd/Instance.cpp` receives base64 JSON on `__pd_mcp_debug` and replies on `__pd_mcp_debug_reply`. `read_canvas` returns bounded pages and a root-wide content-derived revision. `edit_canvas` binds generation/revision, preflights ordered edits under the Pd lock, applies scoped Pd mutations, then checks displayed JUCE canvases on the message thread before reporting `applied`. Batch-local refs address earlier additions; mappings describe surviving and invalidated identities. A failure after dispatch is `PartialOrUncertain`, not rollback.
