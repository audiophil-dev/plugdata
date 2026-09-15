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
  that object's normal left inlet. The reply's `status: "invoked"`
  acknowledges only that the message was delivered - it makes no claim
  about the object's resulting state or DSP output.
- `get_console` returns the newest `max_entries` console entries (a
  required integer field, 1-200; missing or out-of-range values reply
  `InvalidRequest`), oldest to newest, each with its
  text, `message`/`warning`/`error` severity, repeat count, a stable
  monotonic `id`, and whether it came from the visible buffer or (when
  `include_history` is set) history. Entries never include origin pointers.
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
- `export_canvas` renders the registered root canvas at its current zoom
  and scroll position and writes the visible content region to `path`, an
  absolute file path ending in `.png` (anything else replies
  `InvalidRequest`). It presents the same `generation` token as
  `send_object`: the generation must still be registered, and the canvas
  must be currently displayed, otherwise the reply is `StaleGeneration` or
  `CanvasNotFound`. v1 is root-only - there is no `canvas_path`, and no
  `scale` or `fit` parameter. The capture is the canvas content region (the
  union of all object bounds plus a margin) mapped through the current view
  and clipped to the visible editor area, so offscreen content is excluded;
  the reply carries only `status: "saved"`
  and `clipped: true` when the content extent reaches past the captured
  region (`false` otherwise), and never carries image bytes. An existing
  file at `path` is overwritten. A canvas with no objects, content that
  lies entirely outside the viewport, or a target file that cannot be
  opened or written replies `ExportFailed`. The plugin process writes the
  requested path, so this operation is intended for the local managed-tool
  trust boundary.

### Stable errors

`InvalidEnvelope`, `UnsupportedProtocolVersion`, `PayloadTooLarge`,
`InvalidRequest`, `UnknownOperation`, `StaleGeneration`, `CanvasNotFound`,
`CanvasTypeMismatch`, `ObjectNotFound`, `ObjectTypeMismatch`,
`InvalidSelector`, `InvalidAtoms`, `ResponseTooLarge`, `ExportFailed`.

### Limits

Encoded request 64 KiB, decoded JSON 48 KiB, encoded response 60 KiB,
generation/root-receiver tokens 128 UTF-8 bytes, canvas path depth 32,
selector 1,024 UTF-8 bytes, atom count 256, string atom 2,048 UTF-8 bytes,
console entries 1-200. Oversized console snapshots omit whole entries and
set `truncated: true` rather than splitting a message.

### Threading

Requests are parsed and validated on the JUCE message thread before the Pd
lock is taken; `send_object` resolves its path and dispatches in a single
lock acquisition. Console pending-message enqueue, drain, and hard clear
share one small mutex that is never held while acquiring the Pd lock.

### Scope

No new network listener is added by this fork. A separate, independently
reviewed `pd-mcp` integration plan may later route external tool traffic
through these receivers; that integration is out of scope here.

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
