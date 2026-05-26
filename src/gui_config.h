#pragma once

#include "agent.h"

/// Render the Config tab (left panel): provider/model combos, tool gate table,
/// MCP server management and snippets.
void render_config_tab(PrimaryAgent& tab);

/// Render the Model sub-tab inside Config: provider/model/reasoning-effort for
/// Primary agent and all subagents.
void render_model_tab(PrimaryAgent& tab);
