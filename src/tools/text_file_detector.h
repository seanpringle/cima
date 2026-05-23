#pragma once

#include <string>

enum class FileKind { Text, Binary };

/// Detect whether a file is text or binary by scanning the first 8 KB.
/// Returns FileKind::Text for text files (including UTF-16/32 with BOM),
/// FileKind::Binary for files containing NUL bytes (compiled objects, ELF, etc.).
FileKind detect_text_file(const std::string& path);
