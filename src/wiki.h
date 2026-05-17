#pragma once

#include "config.h"

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

/// Persistent, thread-safe wiki store backed by a folder of markdown files.
/// Each page is stored as <title>.md under the wiki directory.
/// Shared across all ChatSession instances so that all agents see the same pages.
class Wiki {
  public:
    /// @param wiki_dir  Path to the wiki/ directory.  The directory is created
    ///                  lazily on the first write operation.
    explicit Wiki(const std::string& wiki_dir);
    ~Wiki();

    Wiki(const Wiki&) = delete;
    Wiki& operator=(const Wiki&) = delete;
    Wiki(Wiki&&) = delete;
    Wiki& operator=(Wiki&&) = delete;

    /// Return the titles of all wiki pages, sorted alphabetically (case-insensitive).
    Result<std::vector<std::string>> list_pages();

    /// Read the full body of a page.
    /// Returns an error if the page does not exist.
    Result<std::string> read_page(const std::string& title);

    /// Write (create or overwrite) a page.
    Result<void> write_page(const std::string& title, const std::string& body);

    /// Edit a page by searching for an exact string and replacing it.
    /// The search string must match exactly once in the page body.
    /// Returns an error if the page doesn't exist, search is empty,
    /// or search doesn't match exactly once.
    Result<void> edit_page(const std::string& title,
        const std::string& search,
        const std::string& replace);

    /// Delete a page. Returns "ok" on success, "no such page" if the
    /// title doesn't exist.
    Result<void> delete_page(const std::string& title);

  private:
    std::string wiki_dir_;
    mutable std::mutex mutex_;

    /// Return the full file path for a given page title.
    /// Validates the title (rejects '/' and '\0').
    Result<std::filesystem::path> page_path(const std::string& title) const;
};
