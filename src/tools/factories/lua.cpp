#include "tools/lua_engine.h"

#include <chrono>
#include <mutex>
#include <sstream>
#include <string>

// ── Timeout hook ──────────────────────────────────────────────────────
//
// Installed via lua_sethook before each luaL_dostring call.  The deadline
// is stored in the lua_State's extraspace (a single void* slot) as a
// pointer to a stack-local DeadlineCtx.
//
// If the deadline passes, the hook calls luaL_error() which causes the
// enclosing lua_pcall (inside luaL_dostring) to return LUA_ERRRUN.

struct DeadlineCtx {
    std::chrono::steady_clock::time_point deadline;
};

static void timeout_hook(lua_State* L, lua_Debug* /*ar*/) {
    // extraspace holds a void* pointing to a stack-local DeadlineCtx
    auto* ctx = static_cast<DeadlineCtx*>(*(void**)lua_getextraspace(L));
    if (std::chrono::steady_clock::now() >= ctx->deadline) {
        luaL_error(L, "lua: execution timed out");
    }
}

// ── make_lua_tool ─────────────────────────────────────────────────────

Tool make_lua_tool(
    std::shared_ptr<lua_State> lua,
    std::shared_ptr<std::mutex> mutex,
    int timeout_sec) {
    Tool t;
    t.name = "lua";
    t.description =
        "Execute Lua code. The Lua state persists across calls "
        "(global variables carry over). Returns a JSON array of return "
        "values (strings and numbers only). Tables cannot be returned.";
    t.permission = ToolPermission::Write;
    // We handle timeout internally via lua_sethook, so the generic timeout
    // in ToolRegistry::execute() is disabled (set to 0).
    t.timeout_sec = 0;
    t.parameters = {
        {"type", "object"},
        {"properties",
            {{"eval",
                {{"type", "string"},
                    {"description",
                        "Lua code to evaluate. Return values are collected "
                        "and returned as a JSON array of scalars."}}}}},
        {"required", {"eval"}}};

    t.execute = [lua, mutex, timeout_sec](const json& args) mutable -> Result<std::string> {
        // Extract the eval string
        auto it = args.find("eval");
        if (it == args.end() || !it->is_string()) {
            return std::unexpected("lua: missing or non-string 'eval' argument");
        }
        std::string code = it->get<std::string>();

        std::lock_guard<std::mutex> lock(*mutex);

        // If the Lua state is in an error state (e.g. from a previous timeout),
        // close it and create a fresh one.
        int status = lua_status(lua.get());
        if (status != LUA_OK && status != LUA_YIELD) {
            // reset() calls lua_close on the old managed pointer, then
            // takes ownership of the new state — do NOT call lua_close
            // explicitly beforehand (double-free segfault).
            lua.reset(luaL_newstate(), lua_close);
            if (!lua) {
                return std::unexpected(
                    "lua: failed to re-create Lua state after error");
            }
            luaL_openlibs(lua.get());
        }

        // Save stack top to later count return values
        int saved_top = lua_gettop(lua.get());

        // ── Install timeout hook ──────────────────────────────────────
        DeadlineCtx ctx{
            std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec)};
        // Store ptr-to-ctx in the Lua state's extraspace (a void* slot).
        // This is safe because ctx lives on this stack frame and the hook
        // is only called while luaL_dostring is executing below.
        *(void**)lua_getextraspace(lua.get()) = &ctx;
        lua_sethook(lua.get(), timeout_hook, LUA_MASKCOUNT, 1000);

        // Run the code
        int result = luaL_dostring(lua.get(), code.c_str());

        // ── Remove timeout hook ───────────────────────────────────────
        lua_sethook(lua.get(), nullptr, 0, 0);
        // Clear extraspace to avoid stale pointers.
        *(void**)lua_getextraspace(lua.get()) = nullptr;

        if (result != LUA_OK) {
            // Error — get the error message from the stack
            std::string err_msg;
            if (const char* msg = lua_tostring(lua.get(), -1)) {
                err_msg = msg;
            } else {
                err_msg = "unknown Lua error";
            }
            lua_settop(lua.get(), saved_top); // pop error, restore stack

            // Close and recreate the state so future calls can succeed.
            // reset() calls lua_close on the old managed pointer, then
            // takes ownership of the new state — do NOT call lua_close
            // explicitly beforehand (double-free segfault).
            lua.reset(luaL_newstate(), lua_close);
            if (lua) {
                luaL_openlibs(lua.get());
            }

            return std::unexpected("Lua error: " + err_msg);
        }

        // Collect return values
        int n_results = lua_gettop(lua.get()) - saved_top;
        json arr = json::array();

        for (int i = 1; i <= n_results; i++) {
            int idx = saved_top + i; // 1-based Lua stack index

            if (lua_isnumber(lua.get(), idx)) {
                // Get as a double (handles both integers and floats)
                arr.push_back(lua_tonumber(lua.get(), idx));
            } else if (lua_isstring(lua.get(), idx)) {
                arr.push_back(lua_tostring(lua.get(), idx));
            } else if (lua_istable(lua.get(), idx)) {
                // Tables are not allowed — error
                lua_settop(lua.get(), saved_top); // pop results
                return std::unexpected(
                    "Lua error: cannot return tables, only scalars "
                    "(strings and numbers)");
            } else {
                lua_settop(lua.get(), saved_top); // pop results
                return std::unexpected(
                    "Lua error: unsupported return type (only strings "
                    "and numbers are allowed)");
            }
        }

        // Pop the return values from the stack
        lua_settop(lua.get(), saved_top);

        return arr.dump();
    };

    return t;
}
