# cima

An LLM-powered coding assistant with a native desktop GUI.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

**Dependencies:** SDL3, libcurl, libgit2, libxml2, nlohmann-json, imgui, md4c, fontconfig (Linux)

## Usage

```bash
./build/cima [session]    # session defaults to "default"
```

Configuration: `~/.config/cima/cima.json` — add your provider(s) on first run.
Sessions stored in `~/.local/state/cima/<session>.json` (single JSON file per session).

## Features

- Multi-tab chat with subagents
- Tool execution: filesystem read/write/edit, bash sandbox, grep/find, web search/fetch
- MCP server integration (stdio & HTTP transports)
- Auto-compaction for long conversations
