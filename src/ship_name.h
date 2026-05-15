#pragma once

#include <string>

/// Generate a Lord of the Rings character name not currently in use.
/// Maintains an internal pool with a shuffled circular queue.
/// Call free_lotr_name() when the name is released.
std::string generate_lotr_name();

/// Return a name to the pool so it can be reused by another session.
void free_lotr_name(const std::string& name);

/// Reserve a specific name so generate_lotr_name() will not return it.
/// Returns true if the name is a known LOTR name, false otherwise
/// (non-LOTR names are still tracked to avoid duplicates during a session).
bool reserve_lotr_name(const std::string& name);

/// Number of available LOTR names in the pool.
int lotr_name_count();
