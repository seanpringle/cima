# cima

An LLM-powered coding assistant with a native desktop GUI.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

**Dependencies:** SDL3, libcurl, libgit2, libxml2, nlohmann-json, imgui, md4c, html2md, fontconfig (Linux)

## Usage

```bash
./build/cima [session]    # session defaults to "default"
```

Configuration: `~/.config/cima/cima.json` — add your provider(s) on first run.
Sessions stored in `~/.local/state/cima/<session>.json` (single JSON file per session).

## Features

- Multi-tab chat with subagents (configurable in `cima.json`)
- Tool execution: filesystem read/write/edit, bash sandbox, grep/find, web search/fetch
- Plan tools (`write_plan` / `read_plan`) for structured problem-solving
- Skills system (`load_skill` tool, `~/.agents/skills/<name>/SKILL.md`)
- MCP server integration (stdio & HTTP transports, per-session enable/disable)
- Supports OpenAI-compatible and Anthropic API providers
- Auto-compaction for long conversations
