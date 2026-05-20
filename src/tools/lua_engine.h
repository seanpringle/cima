#pragma once

#include "config.h"
#include "tools.h"

extern "C" {
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
}

#include <memory>
#include <mutex>

/// Create an `lua` tool that executes Lua code in a persistent state.
///
/// The tool accepts a single argument `eval` (string of Lua code) and returns
/// a JSON array of return values (strings and numbers only).  Tables cannot
/// be returned and will produce an error.
///
/// @param lua  Shared pointer to the persistent lua_State (one per session).
///             Captured by the execute lambda; state persists across calls
///             and is automatically closed when the last shared_ptr is reset.
/// @param mutex  Shared mutex protecting concurrent access to the lua_State.
/// @param timeout_sec  Maximum execution time per Lua eval call.
Tool make_lua_tool(
    std::shared_ptr<lua_State> lua,
    std::shared_ptr<std::mutex> mutex,
    int timeout_sec);
