cmake_minimum_required(VERSION 3.25)
project(llm-chat VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

find_package(CURL REQUIRED)
find_package(Readline QUIET)
find_package(Catch2 REQUIRED)

add_library(llmchat
    src/config.cpp
    src/types.cpp
)
target_include_directories(llmchat PUBLIC src)
target_link_libraries(llmchat PUBLIC
    nlohmann_json::nlohmann_json
    CURL::libcurl
)

if(Readline_FOUND)
    target_compile_definitions(llmchat PRIVATE USE_READLINE=1)
    target_link_libraries(llmchat PRIVATE Readline::Readline)
endif()

add_executable(test_config tests/test_config.cpp)
target_link_libraries(test_config PRIVATE llmchat Catch2::Catch2WithMain)

add_executable(test_types tests/test_types.cpp)
target_link_libraries(test_types PRIVATE llmchat Catch2::Catch2WithMain)

include(CTest)
include(Catch)
catch_discover_tests(test_config)
catch_discover_tests(test_types)
