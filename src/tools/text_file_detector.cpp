#include "tools/text_file_detector.h"

#include <cstring>
#include <fstream>
#include <vector>

// UTF-16/32 BOMs that contain NUL bytes — detect these as text, not binary.
static const struct { unsigned char bytes[4]; std::size_t n; } boms[] = {
    {{0xFF, 0xFE, 0x00, 0x00}, 4}, // UTF-32LE
    {{0x00, 0x00, 0xFE, 0xFF}, 4}, // UTF-32BE
    {{0xFF, 0xFE},               2}, // UTF-16LE
    {{0xFE, 0xFF},               2}, // UTF-16BE
    {{0xEF, 0xBB, 0xBF},         3}, // UTF-8 BOM
};

FileKind detect_text_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return FileKind::Text; // cannot open — treat as text (caller handles error)

    constexpr std::size_t scan_size = 8192;
    std::vector<unsigned char> buf(scan_size);
    file.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(scan_size));
    std::size_t n = static_cast<std::size_t>(file.gcount());

    if (n == 0)
        return FileKind::Text; // empty file → text

    // Check for UTF-16/32 BOMs before NUL scan.
    // These files contain NUL bytes but are valid text.
    for (auto& bom : boms) {
        if (n >= bom.n && std::memcmp(buf.data(), bom.bytes, bom.n) == 0)
            return FileKind::Text;
    }

    // Scan first 8 KB for NUL bytes — the industry-standard binary detection.
    // Used by GNU grep, ripgrep, git, and others.
    if (std::memchr(buf.data(), 0, n) != nullptr)
        return FileKind::Binary;

    return FileKind::Text;
}
