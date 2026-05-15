#include "ship_name.h"

#include <algorithm>
#include <numeric>
#include <random>
#include <set>
#include <string>
#include <vector>

// ── Lord of the Rings character names ────────────────────────────────

static const std::vector<std::string> kLotrNames = {
    "Gandalf",
    "Frodo",
    "Aragorn",
    "Legolas",
    "Gimli",
    "Boromir",
    "Samwise",
    "Merry",
    "Pippin",
    "Galadriel",
    "Elrond",
    "Arwen",
    "Eowyn",
    "Faramir",
    "Treebeard",
    "Bilbo",
    "Gollum",
    "Saruman",
    "Sauron",
    "Theoden",
    "Eomer",
    "Denethor",
    "Celeborn",
    "Thranduil",
    "Radagast",
};

// ── Shuffled circular queue ──────────────────────────────────────────

static const std::vector<std::string>& name_pool() {
    return kLotrNames;
}

static std::vector<size_t>& shuffled_queue() {
    static std::vector<size_t> indices = [] {
        std::vector<size_t> v(kLotrNames.size());
        std::iota(v.begin(), v.end(), size_t{0});
        std::mt19937 rng{std::random_device{}()};
        std::shuffle(v.begin(), v.end(), rng);
        return v;
    }();
    return indices;
}

static size_t& queue_index() {
    static size_t idx = 0;
    return idx;
}

static std::set<std::string>& in_use() {
    static std::set<std::string> used;
    return used;
}

// ── Public API ───────────────────────────────────────────────────────

std::string generate_lotr_name() {
    auto& queue = shuffled_queue();
    auto& pos = queue_index();
    auto& used = in_use();

    for (size_t attempt = 0; attempt < queue.size(); ++attempt) {
        size_t candidate = queue[pos];
        pos = (pos + 1) % queue.size();
        const auto& name = kLotrNames[candidate];
        if (used.find(name) == used.end()) {
            used.insert(name);
            return name;
        }
    }

    // Fallback — all names in use (extremely unlikely).
    return "Hobbit";
}

void free_lotr_name(const std::string& name) {
    in_use().erase(name);
}

bool reserve_lotr_name(const std::string& name) {
    in_use().insert(name);
    // Check if it's a known LOTR name (informational for callers)
    for (const auto& n : kLotrNames) {
        if (n == name) return true;
    }
    return false;
}

int lotr_name_count() {
    return static_cast<int>(kLotrNames.size());
}
