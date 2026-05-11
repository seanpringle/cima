#include "tools.h"

#include <filesystem>

// ===================================================================
// Path sandbox
// ===================================================================

Result<std::string> resolve_path(const std::string& raw_path,
    const std::string& safe_dir,
    const std::vector<std::string>& extra_allowed) {
    if (raw_path.empty()) {
        return std::unexpected(std::string("path is required"));
    }

    std::error_code ec;
    std::filesystem::path p(raw_path);

    // For relative paths, resolve against safe_dir first, then normalize
    if (p.is_relative()) {
        p = std::filesystem::path(safe_dir) / p;
    }

    p = std::filesystem::weakly_canonical(p, ec);
    if (ec) {
        p = std::filesystem::path(raw_path).lexically_normal();
        if (p.is_relative()) {
            p = std::filesystem::path(safe_dir) / p;
        }
    }

    std::string resolved = p.string();

    // Normalize safe_dir (no trailing slash)
    auto sd_path = std::filesystem::weakly_canonical(std::filesystem::path(safe_dir), ec);
    if (ec) {
        sd_path = std::filesystem::path(safe_dir).lexically_normal();
    }
    std::string sd = sd_path.string();
    while (!sd.empty() && sd.back() == '/') {
        sd.pop_back();
    }

    if (resolved == sd || resolved.starts_with(sd + "/")) {
        return resolved;
    }

    // Check extra_allowed paths (for read-only tools)
    for (const auto& allowed : extra_allowed) {
        std::string allowed_norm = allowed;
        while (!allowed_norm.empty() && allowed_norm.back() == '/') {
            allowed_norm.pop_back();
        }
        if (resolved == allowed_norm || resolved.starts_with(allowed_norm + "/")) {
            return resolved;
        }
    }

    return std::unexpected("path must be under " + sd);
}
