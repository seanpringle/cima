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
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(READLINE readline)
endif()
find_package(Catch2 REQUIRED)

if(READLINE_FOUND)
    message(STATUS "Readline found — REPL will have history & tab-completion")
endif()

if(Readline_FOUND)
    message(STATUS "Readline found — REPL will have history & tab-completion")
endif()

add_library(llmchat
    src/config.cpp
    src/types.cpp
    src/tools.cpp
    src/client.cpp
    src/chat.cpp
)
target_include_directories(llmchat PUBLIC src)
target_link_libraries(llmchat PUBLIC
    nlohmann_json::nlohmann_json
    CURL::libcurl
)

add_executable(llm-chat src/main.cpp)
target_link_libraries(llm-chat PRIVATE llmchat)
if(READLINE_FOUND)
    target_compile_definitions(llm-chat PRIVATE USE_READLINE=1)
    target_include_directories(llm-chat PRIVATE ${READLINE_INCLUDE_DIRS})
    target_link_libraries(llm-chat PRIVATE ${READLINE_LIBRARIES})
endif()

add_executable(test_config tests/test_config.cpp)
target_link_libraries(test_config PRIVATE llmchat Catch2::Catch2WithMain)

add_executable(test_types tests/test_types.cpp)
target_link_libraries(test_types PRIVATE llmchat Catch2::Catch2WithMain)

add_executable(test_tools tests/test_tools.cpp)
target_link_libraries(test_tools PRIVATE llmchat Catch2::Catch2WithMain)

add_executable(test_client tests/test_client.cpp)
target_link_libraries(test_client PRIVATE llmchat Catch2::Catch2WithMain)

add_executable(test_chat tests/test_chat.cpp)
target_link_libraries(test_chat PRIVATE llmchat Catch2::Catch2WithMain)

include(CTest)
include(Catch)
catch_discover_tests(test_config)
catch_discover_tests(test_types)
catch_discover_tests(test_tools)
catch_discover_tests(test_client)
catch_discover_tests(test_chat)
