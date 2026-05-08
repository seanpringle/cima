#pragma once

#include <expected>
#include <filesystem>
#include <string>

template <typename T>
using Result = std::expected<T, std::string>;

struct Config {
    std::string api_base = "http://127.0.0.1:11000/v1";
    std::string api_key;
    std::string model = "deepseek-v4-flash";
    std::string system_prompt = "You are a helpful assistant.";
    std::string safe_dir;

    static Config from_env();
    static void load_dotenv(const std::filesystem::path& path);
    static Config from_env_with_dotenv(int argc, char* argv[]);
};
