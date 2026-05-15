#include "wiki.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

Wiki::Wiki(const std::string& wiki_dir) : wiki_dir_(wiki_dir) {
    // Ensure the parent of wiki_dir_ exists (typically the session directory).
    // The wiki_dir_ itself is created lazily on first write.
    std::error_code ec;
    auto parent = std::filesystem::path(wiki_dir_).parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent, ec)) {
        std::filesystem::create_directories(parent, ec);
    }
}

Wiki::~Wiki() = default;

// ── Helper: validate title and return the file path ──

Result<std::filesystem::path> Wiki::page_path(const std::string& title) const {
    if (title.empty()) {
        return std::unexpected("page title must not be empty");
    }
    if (title.find('/') != std::string::npos) {
        return std::unexpected("page title must not contain '/'");
    }
    if (title.find('\0') != std::string::npos) {
        return std::unexpected("page title must not contain null bytes");
    }
    return std::filesystem::path(wiki_dir_) / (title + ".md");
}

// ── list_pages ──

Result<std::vector<std::string>> Wiki::list_pages() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::string> titles;

    std::error_code ec;
    if (!std::filesystem::is_directory(wiki_dir_, ec)) {
        // Directory doesn't exist yet — no pages
        return titles;
    }

    for (const auto& entry : std::filesystem::directory_iterator(wiki_dir_, ec)) {
        if (!entry.is_regular_file()) continue;
        auto path = entry.path();
        if (path.extension() != ".md") continue;

        auto stem = path.filename().string();
        // Remove the trailing .md
        if (stem.size() >= 3) {
            stem.erase(stem.size() - 3);
        }
        titles.push_back(std::move(stem));
    }

    // Sort case-insensitively (same behaviour as the old SQLite COLLATE NOCASE)
    std::sort(titles.begin(), titles.end(), [](const std::string& a, const std::string& b) {
        return std::lexicographical_compare(
            a.begin(), a.end(), b.begin(), b.end(),
            [](unsigned char ca, unsigned char cb) {
                return std::tolower(ca) < std::tolower(cb);
            });
    });

    return titles;
}

// ── read_page ──

Result<std::string> Wiki::read_page(const std::string& title) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto path_res = page_path(title);
    if (!path_res) return std::unexpected(path_res.error());

    const auto& path = *path_res;

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return std::unexpected("no such page: " + title);
    }

    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file.is_open()) {
        return std::unexpected("failed to open page: " + title);
    }

    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

// ── write_page ──

Result<void> Wiki::write_page(const std::string& title, const std::string& body) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto path_res = page_path(title);
    if (!path_res) return std::unexpected(path_res.error());

    const auto& path = *path_res;

    // Ensure wiki directory exists (lazy creation)
    std::error_code ec;
    if (!std::filesystem::is_directory(wiki_dir_, ec)) {
        std::filesystem::create_directories(wiki_dir_, ec);
        if (ec) {
            return std::unexpected("failed to create wiki directory: " + ec.message());
        }
    }

    std::ofstream file(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return std::unexpected("failed to write page: " + title);
    }
    file.write(body.data(), static_cast<std::streamsize>(body.size()));

    if (!file) {
        return std::unexpected("failed to write page (disk full?): " + title);
    }

    return Result<void>();
}

// ── edit_page ──

Result<void> Wiki::edit_page(const std::string& title,
    const std::string& search,
    const std::string& replace) {
    if (search.empty()) {
        return std::unexpected("search string is required");
    }

    std::lock_guard<std::mutex> lock(mutex_);

    auto path_res = page_path(title);
    if (!path_res) return std::unexpected(path_res.error());

    const auto& path = *path_res;

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return std::unexpected("no such page: " + title);
    }

    // Read current body
    std::ifstream infile(path, std::ios::in | std::ios::binary);
    if (!infile.is_open()) {
        return std::unexpected("failed to open page: " + title);
    }
    std::stringstream ss;
    ss << infile.rdbuf();
    std::string body = ss.str();

    // Count occurrences of search string
    size_t count = 0;
    size_t pos = 0;
    while ((pos = body.find(search, pos)) != std::string::npos) {
        count++;
        pos += search.size();
    }

    if (count == 0) {
        return std::unexpected("Search string not found in page (0 matches). "
                               "Use read_wiki_page to verify the page contents.");
    }
    if (count > 1) {
        return std::unexpected("Search string found " + std::to_string(count) +
                               " times in page (expected exactly 1). "
                               "Include more surrounding context in the search string.");
    }

    // Find the unique occurrence and replace
    pos = body.find(search);
    body.replace(pos, search.size(), replace);

    // Write back
    std::ofstream outfile(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!outfile.is_open()) {
        return std::unexpected("failed to write page: " + title);
    }
    outfile.write(body.data(), static_cast<std::streamsize>(body.size()));

    if (!outfile) {
        return std::unexpected("failed to write page (disk full?): " + title);
    }

    return Result<void>();
}

// ── delete_page ──

Result<void> Wiki::delete_page(const std::string& title) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto path_res = page_path(title);
    if (!path_res) return std::unexpected(path_res.error());

    const auto& path = *path_res;

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return std::unexpected("no such page: " + title);
    }

    bool removed = std::filesystem::remove(path, ec);
    if (!removed || ec) {
        return std::unexpected("failed to delete page: " + title);
    }

    return Result<void>();
}
