# Project rules

[2026-10-01 12:35:20] - Build/test this fork against its repository binaries, never silently fall back to `/Applications/plugdata.app`. After submodule pointer changes run `git submodule update --init` before compiling. Do not commit `.omo/`, build output, logs, caches or unrelated `Libraries/Gem` changes. Keep the original installed application and plugins untouched.
