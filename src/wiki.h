#pragma once

#include "config.h"
#include "types.h"

#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

/// Persistent, thread-safe wiki store backed by a single SQLite database file.
/// Shared across all ChatSession instances so that all agents see the same pages.
class Wiki {
  public:
    explicit Wiki(const std::string& db_path);
    ~Wiki();

    Wiki(const Wiki&) = delete;
    Wiki& operator=(const Wiki&) = delete;
    Wiki(Wiki&&) = delete;
    Wiki& operator=(Wiki&&) = delete;

    /// Return the titles of all wiki pages, sorted alphabetically.
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
    sqlite3* db_ = nullptr;
    mutable std::mutex mutex_;
    std::string db_path_;

    void init_tables();
};


