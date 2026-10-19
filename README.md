<p align="center">
  <a href="https://github.com/plugdata-team/plugdata/wiki">
    <img src="https://github.com/plugdata-team/plugdata/assets/44585538/17062f59-bb1b-442e-a0ba-a96a74327460" alt="Logo" width=130 height=130>
  </a>
  <h1 align="center">plugdata</h1>
  <p align="center">
    Plugin wrapper around Pure Data to allow patching in a wide selection of DAWs.
  </p>
  <p align="center">
    <a href="https://github.com/plugdata-team/plugdata/actions/workflows/cmake.yml"><img src="https://github.com/plugdata-team/plugdata/actions/workflows/cmake.yml/badge.svg" alt="Workflows"></a>
  </p>
  <p align="center">
    <a href="https://github.com/plugdata-team/plugdata/releases/latest"><img src="https://img.shields.io/github/downloads/plugdata-team/plugdata/total.svg?colorB=007ec6" alt="Downloads"></a>
    <a href="https://github.com/plugdata-team/plugdata/releases/latest"><img src="https://img.shields.io/github/release/plugdata-team/plugdata.svg?include_prereleases" alt="Release"></a>
    <a href="https://github.com/plugdata-team/plugdata/blob/main/LICENSE"><img src="https://img.shields.io/badge/license-GPL--v3-blue.svg" alt="License"></a>
  </p>
  <p align="center">
    <a href="https://discord.gg/eT2RxdF9Nq"><img src="https://img.shields.io/discord/993531159956955256?label=discord&logo=discord" alt="Discord"></a>
  </p>
</p>

<p align="middle">
<img width="1093" alt="app" src="https://github.com/user-attachments/assets/19f948b1-cd17-4f84-ae70-baf89707f749">
<img width="1093" alt="darkmode" src="https://github.com/user-attachments/assets/605a74fc-34bb-48f5-a48e-153ffc287141">
</p>

plugdata is a plugin wrapper for Pure Data, featuring a new GUI made with JUCE. This is still a WIP, and there are probably still some bugs. By default, it ships with the ELSE collection of externals and abstractions. The aim is to provide a more comfortable patching experience for a large selection of DAWs. It can also be used as a standalone replacement for pure-data.

Join the Discord here, for sharing patches, reporting issues or requesting features: https://discord.gg/eT2RxdF9Nq

<p align="middle">
<img width="431" alt="LIRA-8" src="https://github.com/plugdata-team/plugdata/assets/44585538/1eb2e36f-6552-4caa-8e7b-2cd6adf03917">
</p>

## Installation

**-Windows:**
- Get the installer from the latest [official release](https://github.com/plugdata-team/plugdata/tags)

**-MacOS:**
- Option 1: Get the installer from the latest [official release](https://github.com/plugdata-team/plugdata/tags)
- Option 2: Install the [homebrew cask](https://formulae.brew.sh/cask/plugdata). I do not maintain this so it may be outdated.

**-Linux:**
- Option 1: [OBS repository/packages](https://software.opensuse.org//download.html?project=home%3Aplugdata&package=plugdata)
  The repository contains the packages plugdata, plugdata-vst3 and plugdata-lv2 (lv2-plugdata and vst3-plugdata on some distros). If you get a package without the repo, it will only contain the standalone version.
    - Plugin versions can be found here:
      - Deb/Ubuntu: https://software.opensuse.org/package/plugdata-vst3 and https://software.opensuse.org/package/plugdata-lv2
      - RPM based distros: https://software.opensuse.org/package/vst3-plugdata and https://software.opensuse.org/package/lv2-plugdata
- Option 2: Arch User Repository (Arch only).
  Has the [plugdata-bin](https://aur.archlinux.org/packages/plugdata-bin) package for the latest stable version, or the [plugdata-git](https://aur.archlinux.org/packages/plugdata-git) package for the latest experimental build.

- Option 3: Get the binaries from the latest [official release](https://github.com/plugdata-team/plugdata/tags)

You can also download a recent experimental build from our [website](https://plugdata.org/download.html)

## Build

```
git clone --recursive https://github.com/plugdata-team/plugdata.git
cd plugdata
mkdir build && cd build
cmake .. (the generator can be specified using -G"Unix Makefiles", -G"Xcode" or -G"Visual Studio 16 2019" -A x64)
cmake --build .
```

**Important:**
- plugdata requires cmake 3.21 or later to build CLAP plugins. If you use an older version of cmake, CLAP plugin builds will be disabled.
- Ensure that the git submodules are initialized and updated! You can use the `--recursive` option while cloning or `git submodule update --init --recursive` in the plugdata repository .
- On Linux, Juce framework requires to install dependencies, please refer to [Linux Dependencies.md](https://github.com/juce-framework/JUCE/blob/master/docs/Linux%20Dependencies.md) and use the full command.
- The CMake build system has been tested with *Unix Makefiles*, *XCode*, *Visual Studio 17 2022* and *Visual Studio 16 2019*
- Extra build options:
  - -DQUICK_BUILD=1 will skip objects that take a long time to compile (All Gem objects, sfz~ and ffmpeg based audio players)
  - Gem, sfz~ and ffmpeg can also be disabled separately by passing "-DENABLE_GEM=0", "-DENABLE_SFIZZ=0", "-DENABLE_FFMPEG=0"
  - You can enable Google [Perfetto](https://perfetto.dev/) performance tracing library by passing -DENABLE_PERFETTO=1. This [blog post](https://melatonin.dev/blog/using-perfetto-with-juce-for-dsp-and-ui-performance-tuning/) gives more insight on how to use it.
    - To trace function calls and get performance information, add `TRACE_COMPONENT()` to function bodies you want.
    - A faster way is a Python 3 script `Resources/Scripts/add_perfetto_tracepoints.py` that will add `TRACE_COMPONENT()` calls to matched function bodies in provided `.cpp` files. This means you don't have to add `TRACE_COMPONENT()` everywhere by hand. To use the script you must have a recent `llvm` (>= 19.1.3) and compatible [`libclang`](https://pypi.org/project/libclang/) package installed.
    - Recommended compiling with `Release` mode on for most accurate profiling information.

## Adding your own externals
You can use externals inside plugdata's plugin version by recompiling the externals along with plugdata. This can be achieved by making the following modification to plugdata:

-  Add your sources to the "externals" target inside Libraries/CMakeLists.txt. Alternatively the source files can be placed inside the Libraries/ELSE/Source folder, as all .c files in that folder will be compiled automatically.
-  In Source/Pd/Setup.cpp, add the setup function for your external. The best place to call your setup function is inside libpd_init_pdlua. initialiseELSE and initialiseCyclone will also work, but it has the side-effect that the externals will also be available under the else/* and cyclone/* prefix.

## Direct Debug API

This fork adds an internal debug API for driving a live patch and reading the
console from an external tool while plugdata is running. It is a receiver
pair, not a network service: no socket, port, or authentication is added.

### Receivers

Requests are sent to a fixed Pd receiver and replies arrive on a fixed reply
receiver, both bound once per `Instance`:

```text
__pd_mcp_debug request <base64-json>
__pd_mcp_debug_reply response <base64-json>
```

The payload is JSON, base64-encoded, because raw JSON cannot cross Pd's FUDI
message framing (commas, semicolons, newlines, and backslashes are message
syntax in Pd). Every structurally readable request produces exactly one
correlated reply with the same `request_id`; an unreadable payload replies
with `request_id: 0`.

### Envelope

```json
{"version": 1, "request_id": 1, "operation": "set_generation"}
```

```json
{"version": 1, "request_id": 1, "ok": true, "data": {}}
{"version": 1, "request_id": 1, "ok": false, "error": {"code": "InvalidRequest", "message": "..."}}
```

### Operations

- `set_generation` binds an opaque `generation` token to a root canvas
  identified by `root_receiver`. All later `send_object` calls must present
  a matching, still-registered `generation`; there is no persisted raw
  pointer identity, so a deleted or rebuilt root always invalidates the old
  token.
- `send_object` resolves `canvas_path` (a zero-based sequence of `gl_list`
  ordinals from the registered root) plus `object_ordinal`, then sends
  `bang`, `float`, `symbol`, `list`, or an arbitrary selector with atoms to
  that entry's normal left inlet. Valid targets are object boxes and
  message boxes: banging a message box outputs its stored content.
  Comments and canvases are refused with `ObjectTypeMismatch` or
  `CanvasTypeMismatch`. The reply's `status: "invoked"`
  acknowledges only that the message was delivered - it makes no claim
  about the object's resulting state or DSP output.
- `get_console` returns the newest `max_entries` console entries (a
  required integer field, 1-200; missing or out-of-range values reply
  `InvalidRequest`), oldest to newest, each with its
  text, `message`/`warning`/`error` severity, repeat count, a stable
  monotonic `id`, and whether it came from the visible buffer or (when
  `include_history` is true) history. `include_history` is a required
  boolean field; requests missing it, or carrying any field outside the
  set above, reply `InvalidRequest`. Entries never include origin pointers.
  An optional `since_id` field (a non-negative integer; any other type or a
  negative value replies `InvalidRequest`) selects only entries with
  `id` greater than `since_id`, after which the newest-`max_entries` suffix
  is applied to that filtered set. Every reply also carries a top-level
  `cursor`: the `id` of the most recently stored entry (`0` before any
  entry is stored), returned even when no entries match or both stores are
  empty, and never reset by GUI clear, GUI restore, or `clear_console`.
  Console storage is a bounded window: the visible buffer retains only its
  newest 800 entries, `include_history` extends reach to the history
  buffer, and `clear_console` discards both. Poll using the previous
  reply's `cursor` as the next `since_id`; a poll interval slower than
  eviction can leave entries that no longer match any reachable window
  permanently unreachable.
- `clear_console` deterministically discards pending, visible, and
  historical console state. This hard clear is a separate, non-reversible
  operation from the sidebar's own "clear" button, which still just moves
  visible entries into history for later restore.
- `export_canvas` renders the whole authored content of the registered root
  canvas, or of an already-open child canvas addressed by an optional
  `canvas_path`, and writes it to `path`, an absolute file path ending in
  `.png` (anything else replies `InvalidRequest`). It presents the same
  `generation` token as `send_object`: the generation must still be
  registered, otherwise the reply is `StaleGeneration`. The target canvas is
  resolved under the Pd lock from the registered root plus the optional
  `canvas_path` (an ordinal-only array, at most 32 nonnegative integers,
  default empty for the root); a path component that is not a canvas replies
  `CanvasTypeMismatch`. The resolved canvas must have an existing editor
  Canvas - an inactive tab, a plugin-mode canvas, a graph-on-parent child and
  a hidden-but-open tab all qualify, and display state is irrelevant. A child
  that has never been instantiated has no Canvas and replies `CanvasNotFound`.
  The capture is the full authored extent in logical canvas coordinates: the
  union of all visible object bodies, their labels, connection paths
  (including bends) and bounded drawables, plus a 24-pixel margin, with
  window chrome, scrollbars, selection, lasso and search overlays excluded.
  Offscreen content is included; the image does not depend on the editor's
  scroll position, zoom level or desktop DPI. An optional integer `scale`
  (1 through 4, default 1) multiplies the output dimensions; output is 1
  pixel per logical canvas unit per scale step, by exact integer
  multiplication. The reply carries only `status: "saved"` and `clipped: false`,
  and never carries image bytes. Size caps are checked before any
  allocation: an axis over 8192 pixels, more than 16,777,216 total pixels,
  or a smaller backend 2D texture limit replies `ExportTooLarge`. Publication is atomic: the PNG is staged in the
  destination directory and replaces the target only after a completely
  written image, so a failed export preserves the previous file byte-for-byte
  and leaves no temporary artifact. A canvas with no visible content, a
  drawable whose extent cannot be determined, or a target file that cannot be
  written replies `ExportFailed`. A subpatch
  that has never been opened cannot be exported. The backend texture limit is
  the device's reported maximum 2D texture dimension (for example a Metal
  device's `maxTextureDimension2D`). The plugin process writes
  the requested path, so this operation is intended for the local
  managed-tool trust boundary.

### Canvas read/edit wire contract (v1)

`read_canvas` returns live structural records. `edit_canvas` adds objects,
comments and empty subpatches, moves or removes records, and connects or
disconnects cords at root or nested depth without clearing or reopening the
root. `edit_canvas` preflights its generation and root-wide revision under the
Pd lock, then dispatches the ordered batch while holding that lock. When
deleting an open subpatch, its tabs are closed on the JUCE message thread
between preflight and dispatch, then the root and revision are checked again
under the Pd lock. A successful `status: "applied"` means Pd and all displayed
affected GUI models have been reconciled and checked before the reply. It does
not promise atomic rollback if dispatch fails. An unchanged preflight rejection has
`ok:false`; a post-dispatch failure has `PartialOrUncertain` and requires
authoritative readback before planning another edit.

```json
{"version":1,"request_id":101,"operation":"read_canvas","generation":"session-1","canvas_path":[]}
{"version":1,"request_id":102,"operation":"read_canvas","generation":"session-1","canvas_path":[],"cursor":"<next_cursor from 101>","expected_revision":"<revision from 101>"}
{"version":1,"request_id":103,"operation":"edit_canvas","generation":"session-1","expected_revision":"<current revision>","edits":[{"op":"move","canvas_path":[2],"target":0,"x":45,"y":50},{"op":"add","canvas_path":[],"ref":"new","type":"object","x":210,"y":45,"text":"print result"},{"op":"connect","canvas_path":[],"source":0,"outlet":0,"target":{"ref":"new"},"inlet":0}]}
```

`read_canvas` requires `generation` and an ordinal-only `canvas_path` (empty
for the root); `cursor` and `expected_revision` are optional nonempty opaque
strings, but `cursor` requires `expected_revision`. `edit_canvas` requires `generation`, a nonempty `expected_revision`,
and 1-100 ordered `edits`. Each edit requires `op` and `canvas_path`. Path
components are nonnegative pre-transaction ordinals or exactly `{ "ref":
"id" }` identifying a subpatch added earlier in this batch. `add` requires
a unique batch-local `ref`, `type` (`object`, `comment`, `subpatch`), integer
`x`,`y`, and either `text` for object/comment or `name` for subpatch. `move`
requires `target`, `x`,`y`; `remove` requires `target`; `connect` and
`disconnect` require `source`,`outlet`,`target`,`inlet`. Targets are
nonnegative pre-state ordinals or exactly `{ "ref": "id" }` for an earlier
added record. Refs cannot be reused, referenced before creation, or used
after removal; removing a created subpatch invalidates its ref path.
Before dispatch, live preflight checks generation and the
root-wide revision under one Pd lock, resolves pre-state ordinals and earlier
subpatch refs against pinned identities, projects adds/removes and edge
changes, and validates existing-object port bounds, signal compatibility,
same-canvas endpoints, and edge presence. A class added earlier in the batch
has no known runtime ports until it is instantiated; those checks remain for
the dispatch stage. No edit is dispatched by preflight.

Unknown or missing fields at any level, noninteger ordinals/coordinates/ports,
paths deeper than 32, coordinates outside -1000000..1000000, empty or
oversized strings (generation/ref/name/revision/cursor: 128 UTF-8 bytes;
text: 2048 UTF-8 bytes), and batches over 100 edits are `InvalidRequest`.
The shared 64 KiB encoded / 48 KiB decoded request limits still apply.

`read_canvas` replies return a root-wide content-derived opaque SHA-256
`revision`, an ordered bounded `records` page (canvas/object/connection with
ordinal paths including descendants of the requested canvas), and
`next_cursor` (null at the last page). A continuation
must supply its `cursor` and the page's `expected_revision`; an old token
fails `StaleRevision`, never mixing pages. The cursor is bound to both token
and requested path. An invalid cursor returns `InvalidRequest`; a record
larger than the reply limit returns `RecordTooLarge` rather than being skipped.
Text objects and subpatch boxes carry canvas-local `x`/`y` positions. Pd
scalars carry their `template` name and `x`/`y` in template data coordinates with
`coordinate_space: "data"`. Other non-text Pd objects have no common
side-effect-free position primitive; their record states
`position_available: false` rather than inventing coordinates from GUI bounds.
An applied edit replies with `pre_revision`, the new `revision`, and mappings
for pre/post canvas and object addresses, created/invalidated cords, rebased
canvases, invalidated descendants and batch refs. If mappings exceed one reply,
the edit reply has an empty `mappings` object and `next_cursor` beginning `m:`;
read its pages using `read_canvas` at root with that cursor and the new revision
until `next_cursor` is null. These mapping pages contain records with `kind`
(`ref`, `canvases`, `survivors`, etc.) and `value`; a later edit invalidates the
cursor. A preflight rejection uses the ordinary `ok:false,error:{code,message}`
envelope without changing Pd. A post-dispatch failure includes revision-bound
recovery instructions but makes no rollback claim.

Example read and edit `data` / `error` shapes are:

```json
{"version":1,"request_id":101,"ok":true,"data":{"revision":"opaque-root-token","records":[{"kind":"canvas","canvas_path":[],"name":"root"},{"kind":"object","canvas_path":[],"ordinal":0,"type":"object","x":20,"y":20,"text":"t b b"},{"kind":"object","canvas_path":[],"ordinal":1,"type":"object","x":120,"y":20,"text":"print result"},{"kind":"connection","canvas_path":[],"source":0,"outlet":0,"target":1,"inlet":0}],"next_cursor":null}}
{"version":1,"request_id":103,"ok":true,"data":{"status":"applied","revision":"new-root-token","pre_revision":"old-root-token","mappings":{"canvases":[{"before":[],"after":[]},{"before":[2],"after":[2]}],"created_canvases":[],"connections":[],"created_connections":[{"canvas_path":[],"source":0,"outlet":0,"target":3,"inlet":0}],"invalidated_connections":[],"survivors":[{"before":[2,0],"after":[2,0],"moved":true}],"invalidated":[],"rebased_canvases":[],"refs":{"new":{"canvas_path":[],"ordinal":3}}},"next_cursor":null}}
{"version":1,"request_id":103,"ok":false,"error":{"code":"StaleRevision","message":"Root structure changed"}}
{"version":1,"request_id":103,"ok":false,"error":{"code":"PartialOrUncertain","message":"Inspect current structure before any retry","revision":"current-root-token","recovery":{"operation":"read_canvas","canvas_path":[],"expected_revision":"current-root-token"},"refs":{"new":{"canvas_path":[],"ordinal":3}}}}
```

### Stable errors

`InvalidEnvelope`, `UnsupportedProtocolVersion`, `PayloadTooLarge`,
`InvalidRequest`, `UnknownOperation`, `StaleGeneration`, `CanvasNotFound`,
`CanvasTypeMismatch`, `ObjectNotFound`, `ObjectTypeMismatch`,
`InvalidSelector`, `InvalidAtoms`, `ResponseTooLarge`, `ExportFailed`.
Canvas operations also return `StaleRevision`, `RecordTooLarge`, `InvalidPort`,
`InvalidConnection`, and `PartialOrUncertain`. `NotYetAvailable` can report a
batch that could not dispatch (for example, failure to close a selected child
tab before deletion); it is not the normal response for a supported edit.

### Recovery and relay

For an unchanged `StaleRevision` or `StaleGeneration`, obtain a fresh binding
and full `read_canvas` snapshot, then plan against its current revision and
addresses. For `PartialOrUncertain`, read the reported recovery revision and
all pages before deciding what remains to do. A timeout or broken reply has
**unknown outcome**: do not automatically resend the edit, even with the same
`request_id`. Re-establish the reply channel, bind the live root again if
necessary, and compare an authoritative root snapshot with the intended edit.
Save/close/reopen is an explicit persistence action, never an edit fallback.

The existing pd-mcp PlugData wrapper relays `request <base64-json>` through
its edit port and returns `response <base64-json>` via `[netsend -b]`. The
September 2026 report's earlier ~999-character text-mode truncation was fixed
by binary relay; do not impose that obsolete limit on the fork. Measure the
complete base64 frame on the actual managed path, check its correlated
`request_id`, and cap it at 60 KiB. The relay does not expose canvas editing
through pd-mcp's public live-edit tools; those still reject unsupported
mutations. Use a managed `ENABLE_TESTING=0` fork binary pinned via
`PD_MCP_BINARY`, not an installed upstream application or a test-suite binary.

### Limits

Encoded request 64 KiB, decoded JSON 48 KiB, encoded response 60 KiB,
generation/root-receiver tokens 128 UTF-8 bytes, canvas path depth 32,
selector 1,024 UTF-8 bytes, atom count 256, string atom 2,048 UTF-8 bytes,
console entries 1-200. Oversized console snapshots omit whole entries and
set `truncated: true` rather than splitting a message.

### Threading

Requests are parsed and validated on the JUCE message thread before the Pd
lock is taken. `send_object` resolves its path and dispatches in a single lock
acquisition. `edit_canvas` checks the registered generation and root-wide
revision and preflights its ordered edits under the Pd lock, then dispatches
the batch under that lock. If deleting an open subpatch requires closing its
tab, the tab close happens on the JUCE message thread outside the lock, and
generation and revision are checked again before dispatch. After dispatch,
the affected displayed canvas models are synchronized and checked before an
`applied` reply is sent. This acknowledgment does not promise rollback on a
post-dispatch failure. Console pending-message enqueue, drain, and hard clear
share one small mutex that is never held while acquiring the Pd lock.

### Scope

No new network listener is added by this fork. A separate, independently
reviewed `pd-mcp` integration plan may later route external tool traffic
through these receivers; that integration is out of scope here.

### Build flavors and managed use

Two build flavors serve different purposes:

- `ENABLE_TESTING=0` (default): normal application behavior. Settings persist
  in `Documents/plugdata/settings.json`, so state such as `onboarding_completed`
  survives across launches.
- `ENABLE_TESTING=1`: use for running the test suite with
  `PLUGDATA_RUN_TESTS=1`. Without that variable the binary starts as a normal
  application. In this flavor, `SettingsFile` substitutes a
  fresh temporary file for `settings.json` on every launch, so every settings
  write is discarded when the process exits.

From the fork repository root on macOS, configure and build the managed
debug-API bundle, then copy/sign it before building another flavor:

```sh
cmake -B build-managed -GNinja -DENABLE_TESTING=0
cmake --build build-managed
mkdir -p Plugins/Standalone-managed
ditto Plugins/Standalone/plugdata.app Plugins/Standalone-managed/plugdata.app
codesign --force --deep -s - Plugins/Standalone-managed/plugdata.app
codesign --verify --deep --strict Plugins/Standalone-managed/plugdata.app
```

Both CMake build directories write `Plugins/Standalone/plugdata.app`; building
one flavor replaces that shared bundle.
`ENABLE_TESTING=1` uses a fresh temporary settings file each launch, so settings
such as `onboarding_completed` do not persist and onboarding can reappear.
A managed launcher that redirects `HOME` should also account for plugdata's
app data directory being `$HOME/Documents/plugdata`.

To verify the managed fork without joining an existing pd-mcp session, run:

```sh
node Tests/canvas-edit-managed-probe.mjs --binary Plugins/Standalone-managed/plugdata.app/Contents/MacOS/plugdata --output .omo/evidence/canvas-edit-managed.json
```

The probe copies the sibling pd-mcp wrapper and bridge into a disposable
directory, changes only their local listener ports, verifies both listeners
belong to its own child PID, and removes its home and patches afterward. It
never connects to another process's fixed ports 3011/3013. Its JSON evidence
includes the correlated decoded replies, encoded byte counts, saved Pd source,
and process cleanup. Its default request timeout is 10 seconds; a timeout is
recorded as an unknown outcome, not retried, and followed by authoritative
readback in the timeout case. The probe's binary relay accepts encoded replies
up to 60 KiB. A timeout or broken reply in other clients likewise has unknown
outcome; don't automatically resend an edit, and inspect the live canvas first.

On Linux, build the normal flavor, copy its standalone executable to a separate
managed path before building the test flavor, and point the probe at the relay
patches from a local pd-mcp checkout:

```sh
cmake -B build-managed -GNinja -DENABLE_TESTING=0
cmake --build build-managed --target plugdata_standalone
mkdir -p Plugins/Standalone-managed
cp Plugins/Standalone/plugdata Plugins/Standalone/plugdata-resources.bin Plugins/Standalone-managed/
PD_MCP_PATCH_DIR=/path/to/pd-mcp/pd node Tests/canvas-edit-managed-probe.mjs --binary Plugins/Standalone-managed/plugdata --output /tmp/canvas-edit-managed.json
```

For the in-app Direct Debug API and offscreen OpenGL tests without the full
helpfile fuzz suite, build with `ENABLE_TESTING=1` and launch with
`PLUGDATA_RUN_TESTS=1 PLUGDATA_TEST_FILTER=direct-debug-api`.

### Licensing note for this fork

This repository is a fork of plugdata with source-level modifications
(this Direct Debug API among them). If you distribute a compiled binary
built from this fork, the GPL-3.0 (and, via the bundled JUCE framework,
AGPL-3.0) terms described below still apply: you must make the
corresponding modified source available under the same license. This fork
does not change plugdata's general licensing; see the section below.

## Corporate sponsors
<p align="center" style="display: flex; flex-direction: column; align-items: center; justify-content: center;">
  <a href="https://gigperformer.com/">
    <img src="https://github.com/plugdata-team/plugdata/assets/44585538/e0c37c87-05cc-4766-9dc4-a42ddbb8784f">
  </a>
  <a href="https://jb.gg/OpenSourceSupport">
    <img src="https://github.com/plugdata-team/plugdata/assets/44585538/fd9ae433-4913-411f-8ae0-c1a7c5f28c72">
  </a>
</p>

## Licensing
This project uses the JUCE framework, which is licensed under the [GNU Affero General Public License (AGPL-3.0)](https://www.gnu.org/licenses/agpl-3.0.en.html). Because of this, the compiled application (the full program built with JUCE) is distributed under the terms of the AGPL-3.0.

The plugdata source code that is directly included in this repository (not in any third-party submodule) is licensed under the regular [GPL-3.0](https://www.gnu.org/licenses/gpl-3.0.en.html)

Third-party components (pure-data, pd-else, pd-cyclone, pd-lua, libwebp, liblzma, heavylib, fftw3, ffmpeg, sfz~ and many more) included with plugdata are licensed under their own respective licenses.

## Credits
- Logo designed by [Joshua A.C. Newman](https://glyphpress.com/talk/), executed by [Bas de Bruin](https://www.bdebruin.nl/)
- [Camomile](https://github.com/pierreguillot/Camomile) by Pierre Guillot
- [ELSE](https://github.com/porres/pd-else) by Alexandre Torres Porres
- [cyclone](https://github.com/porres/pd-cyclone) by Krzysztof Czaja, Hans-Christoph Steiner, Fred Jan Kraan, Alexandre Torres Porres, Derek Kwan, Matt Barber and others (note: Cyclone is included to offer an easy entry point for Max users but ELSE contains several alternatives to objects in Cyclone and Pure Data Vanilla itself also has some alternatives. Not only that, but objects that weren't cloned into Cyclone also have alternatives in ELSE, see: [this](https://github.com/porres/pd-else/wiki/Cyclone-alternatives))
- [pd-lua](https://github.com/agraef/pd-lua) by Claude Heiland-Allen, Albert Graef, and others
- [Pure Data](https://puredata.info) by Miller Puckette and others
- [libpd](https://github.com/libpd/libpd) by the Peter Brinkmann, Dan Wilcox and others
- [Heavy/hvcc](https://github.com/Wasted-Audio/hvcc) originally by Enzien Audio, maintained and modernised by Wasted Audio
- [Juce](https://github.com/juce-framework/JUCE) by Raw Material Software Ltd.
- [Inter](https://rsms.me/inter/) font by Rasmus Andersson
