#pragma once

#include "config.h"

struct ImFont;
class Wiki;

/// Render the read-only Wiki tab in the main tab bar.
/// Left panel (40%): selectable page titles.
/// Right panel (60%): rendered markdown of the selected page.
void render_wiki_tab(Wiki& wiki, ImFont* mono_font);
