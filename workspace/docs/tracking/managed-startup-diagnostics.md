# Managed PlugData startup diagnostics

Updated: 2026-10-12 18:01 CEST. For the PlugData/pd-mcp fork maintainer and OpenCode sandbox maintainer.

Publication review: 2026-10-15. Findings below describe the October 12 investigation, not a fresh runtime check. No subsequent repair or successful relaunch is verified here.

## Status and boundaries

Two separate incidents are tracked below. The earlier filesystem warning did not prevent bridge readiness or patch loading. Host logs now identify the later process exit mechanism: a sandbox denial of LaunchServices lookup followed by application-registration abort. The exact applied policy and process ancestry of that departed child still require correlation; no repair has been applied. Do not conflate this abort with the earlier Documents resource-link denials.

The owner cannot restart now. Permission remediation and a clean VS Code/pd-mcp/PlugData relaunch remain deferred and unverified. Investigation uses retained host logs and source reads only: no permission widening, unsandboxed fallback, process killing, app-data deletion, deployment, rebuild or production-patch editing.

## Earlier filesystem warning: operation identified

Warning: `Failed to initialise filesystem. Is the disk full?`

The earlier handover reported 172 GiB free, existing bundled/extracted resources, resolving resource links and readable known files despite directory-enumeration EPERM. Host unified logs were subsequently inspected in this session for the managed launch at 2026-10-12 13:58:18 CEST (PID 77158).

Kernel evidence at 13:58:19 shows:

```text
System Policy: plugdata(77158) deny(1) file-write-unlink /Users/audiophilair/Documents/plugdata/Abstractions
System Policy: plugdata(77158) deny(1) file-write-unlink /Users/audiophilair/Documents/plugdata/Documentation
System Policy: plugdata(77158) deny(1) file-write-unlink /Users/audiophilair/Documents/plugdata/Extra
```

Each link has retries roughly 100 ms apart. `Source/PluginProcessor.cpp:432` defines `createLinkWithRetry`; it attempts removal/replacement even when a link already resolves correctly, and exhausted retries set `extractionCompleted = false`. The macOS call sites are at lines 513-515, with the failure returned at line 519. This identifies the resource-link refresh failure behind the generic warning, rather than establishing a disk-space or extraction failure.

The same child also has unlink denials for `testtone.pd` and `load-meter.pd`, plus `file-read-data` denials on `~/Documents/plugdata` and `Externals`. Reading a known file is not proof of permission to enumerate its parent or replace a link.

TCC attribution identifies requesting/accessing `com.plugdata.plugdata` PID 77158 and responsible `com.microsoft.VSCode` PID 1165, running from an App Translocation path. Captured authorization results concern other services, not `SystemPolicyDocumentsFolder`; the exact stored permission causing the filesystem denials remains unproven. Neither App Translocation nor a disabled Documents toggle is established as the cause.

The next narrow permission check is VS Code's Documents Folder entry under Privacy & Security > Files & Folders. When the owner can restart, save work and relaunch the whole chain to retest initialization. Do not claim a permission change or successful retest until observed. A temporary HOME must not be assumed to redirect JUCE/macOS app-data access.

The incoming handover's statement that the earlier warning's precise failed operation is unknown is superseded by this kernel evidence. Its warning not to conflate the two incidents remains valid.

## New blocker: child exits before bridge readiness

The request and environment below were reported in the owner's handover. The subsequent host-log findings independently establish the termination mechanism at the matching timestamp without launching another process.

At **2026-10-12 16:06:33 CEST / 14:06:33 UTC**, the first R2 authoring operation failed:

```json
{
  "tool": "plugdata_open_patch",
  "arguments": {
    "path": ".omo/runtime/pd-prerequisites/view/bela/_common/sensor-source-selector.pd",
    "save_mode": "manual",
    "create": true,
    "mode": "live"
  }
}
```

Returned error: `LiveCanvasUnavailable: Pd exited before its bridge became reachable.` The response reported `source_written:false`; follow-up context reported `scratch_clean`, no managed child and no target. No helper file was created. The approved Manual-only `sensor-input.pd` checkpoint remains unchanged according to the handover.

### Reported environment

- Project: `/Users/audiophilair/Desktop/Pascal/she_brings_the_rain`.
- Managed executable: `/Users/audiophilair/Desktop/Pascal/plugdata/Plugins/Standalone-managed/plugdata.app/Contents/MacOS/plugdata`.
- pd-mcp PID 15773; command port 3011; edit port 3013.
- Child-to-ancestor chain: pd-mcp node 15773 -> opencode 8399 -> bash 8356 -> zsh 8203 -> VS Code Helper 1184.
- OpenCode PID 8399 serves `127.0.0.1:4096`; health returned `{"healthy":true,"version":"1.18.35"}`.
- Tool shell: `ps` returns operation not permitted; `log show` returns `Cannot run while sandboxed`.
- At handover time, exact applied Seatbelt profile and departed PlugData child's sandbox status were not established. Subsequent host logs confirm enforcement on the child; the exact profile remains unknown.
- Managed app passed `codesign --verify --deep --strict`; executable has arm64 and x86_64 slices; inspected dependencies showed no obvious missing library. These checks do not establish successful runtime initialization.
- Binary mtime: October 6, 13:20:59 CEST. `pd-mcp/dist/pd/process.js` mtime: October 12, 15:43:45 CEST. Mtimes do not establish a source commit or source/dist equivalence.
- Reported observability gap: `pd-mcp/src/pd/process.ts` around line 930 detects a dead child during readiness but does not expose its complete termination details. Source inspection below corrects the original report: non-null exitCode is included; signal, child PID and captured stderr are missing.

### Evidence to preserve

Paths below are relative to `she_brings_the_rain`, unless absolute:

- R2 evidence: `.omo/runtime/sensor-input-r2/evidence.md`.
- Earlier startup evidence: `.omo/runtime/pd-prerequisites/evidence.md`.
- OpenCode log: `~/.local/share/opencode/log/opencode.log`, around line 319686.
- Approved stage/product SHA: `5a7201810ddf3564489f6d8e7674c212a234744fcc359b5dc29dbca2061a9379` (provided by the handover; not recomputed here).

## Host-log findings: LaunchServices denial and abort

Read-only investigation on October 12 retrieved the 16:06:25-16:06:50 CEST window using `/usr/bin/log show --style compact --info --debug`. The predicate selected PlugData messages plus sandboxd, ReportCrash and amfid. The complete captured output is at `/Users/audiophilair/.local/share/opencode/tool-output/tool_130d84496001XqFdw8RdHNBNXV`; the causal sequence is preserved below so the finding does not depend on that tool-output file's retention.

```text
2026-10-12 16:06:33.525 kernel: Sandbox: plugdata(22733) deny(1) file-issue-extension target:/Users/audiophilair/Desktop/Pascal/plugdata/Plugins/Standalone-managed/plugdata.app extension-class:com.apple.app-sandbox.read
2026-10-12 16:06:33.527 kernel: Sandbox: plugdata(22733) deny(1) mach-lookup com.apple.coreservices.launchservicesd
2026-10-12 16:06:33.527 launchd: denied lookup: name = com.apple.coreservices.launchservicesd, flags = 0x8, requestor = plugdata[22733], error = 159: Sandbox restriction
2026-10-12 16:06:33.527 plugdata[22733]: Process unable to create connection because the sandbox denied the right to lookup com.apple.coreservices.launchservicesd and so this process cannot talk to launchservicesd.
2026-10-12 16:06:33.532 plugdata[22733]: _RegisterApplication(), unable to get application ASN from launchservicesd, and this application requires an ASN, so aborting. error=#-1.
2026-10-12 16:06:33.532 kernel: AMFI: Denying core dump for pid 22733 (plugdata)
2026-10-12 16:06:33.909 ReportCrash[22734]: Will not report from invalid source
```

Excerpt formatting omits trace IDs/subsystem decorations; messages and timestamps retain the observed values. This identifies sandbox enforcement on the PlugData process itself, not merely on the diagnostic shell. It is an explicit application-registration abort, not evidence of a missing library, invalid code signature, disk-space problem or loopback-port failure.

PID 22733 also received `mach-lookup` denials for `com.apple.windowserver.active`, `com.apple.pasteboard.1`, `com.apple.tccd.system` and `com.apple.distributed_notifications@Uv3`, and `user-preference-read` denials. These establish additional blocked desktop services, not a sufficient allowlist for a repair.

A second PlugData process, PID 22760, shows the same LaunchServices denial at 16:06:47.225 and registration abort at 16:06:47.230. Its relation to the first request (retry versus another launch) is not established by these logs. The matching timestamp and managed bundle path strongly correlate PID 22733 with the reported failure; the OS excerpt does not independently establish parent PID 15773.

No PlugData-named report was found by the filename search of the user's and system's `Library/Logs/DiagnosticReports` directories. ReportCrash's invalid-source message explains why lack of a standalone report cannot be treated as absence of a crash. The excerpt does not supply the child exit code or signal; do not label it SIGABRT without that evidence.

The existing VS Code Documents permission advice addresses the earlier warning, not this LaunchServices denial. Do not widen Documents access or disable the sandbox as a proposed repair for this abort. The sandbox maintainer must inspect the applied policy and intended support for managed GUI children before choosing a scoped remedy.

## Diagnostic ownership and next evidence

### Source and policy-record checks

Read-only inspection of `pd-mcp/src/pd/process.ts` found:

- Lines 865-869 spawn the binary with `detached: true` and piped stdout/stderr. Detached process grouping is not evidence of sandbox exemption.
- Lines 880-887 handle exit without retaining callback code/signal, clear active tracking and remove the pidfile/runtime directory. The pidfile initially contains both child and server PID (lines 890-898), but is not a durable exit receipt.
- Lines 900-902 forward stderr as `[pd:<child pid>]` to the MCP server's stderr, without a local bounded buffer. A search of the named OpenCode log found no historical receipt for PID 22733 or 22760; only the diagnostic search's own later log entry matched.
- Lines 930-938 include `child.exitCode` in the readiness error when non-null. Thus the original handover's blanket statement that exit code is never exposed is too broad. Signal, child PID and captured startup stderr are not included in this error; a signal exit can retain the generic wording. `getManagedProcessStatus()` reports no child once it has exited.
- The source reviewer found matching relevant behavior in the current `dist/pd/process.js`, but its mtime is now 17:07:47 CEST, later than the incident and the handover's 15:43:45 value. Current source/dist inspection does not prove the bytes loaded by the already-running MCP at 16:06.

`opencode.log:319686` records `plugdata_open_patch` allowed at 14:06:33.460Z, immediately preceding the OS denial/abort sequence. That is an application-level tool permission, not a Seatbelt grant. Lines 319701-319702 record context/console requests at 14:06:47.138Z and .159Z, near the second process launch; this is temporal correlation, not proof of which request spawned it.

No launch record tying OpenCode PID 8399 or pd-mcp PID 15773 to an exact Seatbelt profile was found in the inspected retained logs. Unrelated Linux Firejail configuration is not evidence for this macOS process. The child's sandbox denial is proven by host logs; the profile's origin and inheritance chain remain a separate evidence gap for the sandbox maintainer.

### OpenCode sandbox maintainer

1. Establish the exact policy applied to OpenCode PID 8399 and whether/how it was inherited by the managed child. Do not assume the shell's restrictions establish the child's policy.
2. Use the host-log findings above for **16:06:25-16:06:50 CEST / 14:06:25-14:06:50 UTC**. Absence of logs accessible inside the originating sandbox is not evidence that no denial occurred.
3. Correlate PID 22733 (and separately 22760) to the launcher and applied policy. Do not reuse PID 77158 from the earlier incident as the new child's identity. Review GUI-service access, not just filesystem roots or ports.

### PlugData/pd-mcp maintainer

1. Inspect matching PlugData crash/dyld reports for the same window.
2. Preserve the child PID, exit code/signal and bounded startup stderr through the bridge-readiness failure path. Keep captured output limited and avoid exposing secrets. Verify source/dist correspondence before interpreting a code change as deployed.
3. Distinguish a spawn failure, normal/nonzero exit, signal/crash or sandbox denial using actual evidence before proposing a repair. The generic readiness error alone cannot distinguish them.

No runtime repair or telemetry implementation has been made. Host logs identify the immediate abort mechanism; exact-policy attribution and startup-exit telemetry remain pending. No restart was required to inspect this retained evidence from the host context.
