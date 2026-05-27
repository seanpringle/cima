#pragma once

#include "agent.h"

/// Render the Config tab (left panel): tool gate table,
/// MCP server management and snippets.
void render_config_tab(PrimaryAgent& tab);

/// Render the provider/model/reasoning-effort inline selectors.
/// Called at the top of each agent chat tab.
void render_provider_model_inline(Agent& tab, ChatSession& session);
