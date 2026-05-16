cmake_minimum_required(VERSION 3.25)
project(cima VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.7
)
FetchContent_MakeAvailable(imgui)

find_package(CURL REQUIRED)
find_package(SDL3 REQUIRED)
find_package(Catch2 REQUIRED)

# ── md4c (Markdown parser) ──
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(MD4C md4c)
endif()
if(MD4C_FOUND)
    message(STATUS "Found md4c: ${MD4C_LIBRARIES}")
    add_library(cima_md4c INTERFACE IMPORTED)
    target_include_directories(cima_md4c INTERFACE ${MD4C_INCLUDE_DIRS})
    target_link_libraries(cima_md4c INTERFACE ${MD4C_LIBRARIES})
else()
    find_library(MD4C_LIB md4c REQUIRED)
    message(STATUS "Found md4c (find_library): ${MD4C_LIB}")
    add_library(cima_md4c INTERFACE IMPORTED)
    target_link_libraries(cima_md4c INTERFACE ${MD4C_LIB})
endif()
# ────────────────────────────────────────────────────────────────────────

# ── libgit2 (try pkg-config first, fall back to FetchContent) ──
# Note: we deliberately avoid find_package(libgit2) because Debian/Ubuntu
# packages ship a broken CMake config that sets INTERFACE_INCLUDE_DIRECTORIES
# to /usr/include/git2 instead of /usr/include, causing conflicts with
# the system <stdint.h>.  pkg-config provides the correct include path.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBGIT2 libgit2)
endif()

if(LIBGIT2_FOUND)
    message(STATUS "Found libgit2 (pkg-config): ${LIBGIT2_LIBRARIES}")
    add_library(cima_git2 INTERFACE IMPORTED)
    target_include_directories(cima_git2 INTERFACE ${LIBGIT2_INCLUDE_DIRS})
    target_link_libraries(cima_git2 INTERFACE ${LIBGIT2_LIBRARIES})
    if(LIBGIT2_LINK_LIBRARIES)
        target_link_libraries(cima_git2 INTERFACE ${LIBGIT2_LINK_LIBRARIES})
    endif()
else()
    include(FetchContent)
    FetchContent_Declare(libgit2
        GIT_REPOSITORY https://github.com/libgit2/libgit2.git
        GIT_TAG v1.8.4
        GIT_SHALLOW TRUE
    )
    # Suppress libgit2's own test/tool builds to speed up the fetch
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_CLAR OFF CACHE BOOL "" FORCE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(REGEX_BACKEND "builtin" CACHE STRING "" FORCE)
    FetchContent_MakeAvailable(libgit2)

    message(STATUS "libgit2 fetched from upstream")
    if(NOT TARGET cima_git2)
        add_library(cima_git2 INTERFACE IMPORTED)
        if(TARGET Git2::git2)
            target_link_libraries(cima_git2 INTERFACE Git2::git2)
        elseif(TARGET libgit2::libgit2)
            target_link_libraries(cima_git2 INTERFACE libgit2::libgit2)
        else()
            message(FATAL_ERROR "libgit2 fetched but no expected target found (Git2::git2 or libgit2::libgit2)")
        endif()
    endif()
endif()
# ────────────────────────────────────────────────────────────────────

add_library(cima_core
    src/config.cpp
    src/types.cpp
    src/ship_name.cpp
    src/tools/path.cpp
    src/tools/git_helpers.cpp
    src/tools/web_helpers.cpp
    src/tools/factories/filesystem.cpp
    src/tools/factories/file_io.cpp
    src/tools/factories/grep.cpp
    src/tools/factories/bash.cpp
    src/tools/factories/web.cpp
    src/tools/factories/git.cpp
    src/tools/registry.cpp
    src/client.cpp
    src/chat.cpp
    src/conversation.cpp
    src/plan.cpp
    src/notes.cpp
    src/app_session.cpp
    src/assistant_data.cpp
    src/wiki.cpp
    src/tools/factories/wiki.cpp
    src/tools/factories/notes.cpp
    src/lsp/json_rpc.cpp
    src/lsp/lsp_client.cpp
    src/tools/factories/lsp.cpp
)
target_include_directories(cima_core PUBLIC src)
target_link_libraries(cima_core PUBLIC
    nlohmann_json::nlohmann_json
    CURL::libcurl
    cima_git2
)

add_executable(cima
    src/main.cpp
    src/gui_app.cpp
    src/gui_chat.cpp
    src/gui_wiki.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
)
target_include_directories(cima PRIVATE ${imgui_SOURCE_DIR})
target_include_directories(cima PRIVATE ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(cima PRIVATE IMGUI_USE_WCHAR32=1)
target_link_libraries(cima PRIVATE cima_core SDL3::SDL3 cima_md4c)

add_executable(test_config tests/test_config.cpp)
target_link_libraries(test_config PRIVATE cima_core Catch2::Catch2WithMain)

add_executable(test_types tests/test_types.cpp)
target_link_libraries(test_types PRIVATE cima_core Catch2::Catch2WithMain)

add_executable(test_tools tests/test_tools.cpp)
target_link_libraries(test_tools PRIVATE cima_core Catch2::Catch2WithMain)

add_executable(test_client tests/test_client.cpp)
target_link_libraries(test_client PRIVATE cima_core Catch2::Catch2WithMain)

add_executable(test_chat tests/test_chat.cpp)
target_link_libraries(test_chat PRIVATE cima_core Catch2::Catch2WithMain)

add_executable(test_wiki tests/test_wiki.cpp)
target_link_libraries(test_wiki PRIVATE cima_core Catch2::Catch2WithMain)

add_executable(test_conversation tests/test_conversation.cpp)
target_link_libraries(test_conversation PRIVATE cima_core Catch2::Catch2WithMain)

add_executable(test_notes tests/test_notes.cpp)
target_link_libraries(test_notes PRIVATE cima_core Catch2::Catch2WithMain)

add_executable(test_json_rpc tests/test_json_rpc.cpp)
target_link_libraries(test_json_rpc PRIVATE cima_core Catch2::Catch2WithMain)

add_executable(test_lsp tests/test_lsp.cpp)
target_link_libraries(test_lsp PRIVATE cima_core Catch2::Catch2WithMain)

add_executable(test_lsp_tools tests/test_lsp_tools.cpp)
target_link_libraries(test_lsp_tools PRIVATE cima_core Catch2::Catch2WithMain)

include(CTest)
include(Catch)
catch_discover_tests(test_config)
catch_discover_tests(test_types)
catch_discover_tests(test_tools)
catch_discover_tests(test_client)
catch_discover_tests(test_chat)
catch_discover_tests(test_wiki)
catch_discover_tests(test_conversation)
catch_discover_tests(test_notes)
catch_discover_tests(test_json_rpc)
catch_discover_tests(test_lsp)
catch_discover_tests(test_lsp_tools)
