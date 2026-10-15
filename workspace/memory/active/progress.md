# Progress

## Done

[2026-10-02 09:40:00] - Resolved the fork ml.lib load failure: copied the library into the fork's own version tree, identified the no-recursion search-path rule and the 0.9.3 tree's app-bound ACL, and wrote the pd-mcp maintainer handover for live-edit availability and testing.

[2026-10-01 12:35:20] - Completed generation/revision-bound canvas readback, ordered in-place add/move/remove/wire edits, GUI acknowledgment, survivor mapping, and native child-port routing after save/reopen. Nine implementation tasks and four final reviews passed; the owner confirmed valid manual GUI connections save and load.

## Doing

- [2026-10-12 18:01:52] - Read-only startup investigation identified LaunchServices sandbox denial followed by application-registration abort for PID 22733 at 16:06:33 CEST; PID 22760 repeats at 16:06:47. Repair remains pending exact-policy attribution. Earlier resource-link warning is separate; permission remediation/relaunch remains deferred because the owner cannot restart now.

## Next

- Give the sandbox maintainer the retained LaunchServices denial/abort evidence and establish the applied profile before proposing scoped GUI-child support. Improve pd-mcp exit receipts (child PID, signal/code, bounded stderr) separately; existing readiness errors already include non-null exitCode. Preserve the Manual-only checkpoint; see `workspace/docs/tracking/managed-startup-diagnostics.md`. No runtime changes performed.
- Reassess remaining pd-mcp integration coverage after startup is unblocked. The October 12 handover exercises public live-mode tools, so the earlier blanket statement that integration is unstarted is no longer current; end-to-end coverage was not established in this investigation.

[2026-10-06 15:10:00] - Completed full-canvas offscreen export: 7 implementation tasks + 4 final gates APPROVE (96h 35m). Dedicated offscreen NVG render target with async-layer primitives, authored visual extent + chrome-free draw, scale/nested targeting, atomic PNG publication, managed probe 164/164 checks. Upstreamed the nanovg delta as audiophil-dev/nanovg async-offscreen-passes and opened the upstream PR.

## Diagnostic evidence recorded

[2026-10-12 16:33:02] - Recorded host-log evidence from the 13:58:18 CEST managed launch: PID 77158 denied file-write-unlink on Abstractions, Documentation and Extra with retries matching initialiseFilesystem(); directory reads were denied too. No source change, rebuild or permission change was needed to identify the warning's failing operation. Read and recorded the later process-exit handover separately; its claims have not yet been independently reproduced.

[2026-10-12 18:01:52] - Retrieved matching host logs for the later exit: launchd rejected LaunchServices lookup (error 159, Sandbox restriction); PlugData logged _RegisterApplication abort. ReportCrash declined an invalid source; no PlugData-named crash report was found. Source inspection confirms stderr forwarding without local buffering and cleanup that loses PID tracking. No exact Seatbelt launch record found; current dist mtime postdates incident. Evidence is documented; no reproduction, policy change, restart or rebuild performed.

[2026-10-15 09:42:13] - Reviewed the diagnostic handover and memory for publication at the owner's request. Kept confirmed October 12 findings separate from reported environment and unverified remediation; corrected stale integration status. This is a documentation-only update, not evidence of a repaired startup or a completed permission/relaunch test.
