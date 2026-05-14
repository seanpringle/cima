#pragma once

#include <string>

/// Generate a random Culture-series-style ship name.
/// Examples: "Very-Little-Gravitas-Indeed", "No-More-Mr-Nice-Guy",
///           "Just-Another-Victim-Of-The-Ambient-Morality"
std::string generate_culture_ship_name();

/// Return all registered ship name part lists (useful for the UI if needed).
int culture_ship_name_count();
