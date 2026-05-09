cmake_minimum_required(VERSION 3.25)
project(llm-chat VERSION 0.1.0 LANGUAGES CXX)

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

add_library(llmchat
    src/config.cpp
    src/types.cpp
    src/tools.cpp
    src/client.cpp
    src/chat.cpp
    src/renderer.cpp
)
target_include_directories(llmchat PUBLIC src)
target_link_libraries(llmchat PUBLIC
    nlohmann_json::nlohmann_json
    CURL::libcurl
)

add_executable(llm-chat
    src/main.cpp
    src/gui_app.cpp
    src/gui_chat.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
)
target_include_directories(llm-chat PRIVATE ${imgui_SOURCE_DIR})
target_include_directories(llm-chat PRIVATE ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(llm-chat PRIVATE IMGUI_USE_WCHAR32=1)
target_link_libraries(llm-chat PRIVATE llmchat SDL3::SDL3)

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

add_executable(test_renderer tests/test_renderer.cpp)
target_link_libraries(test_renderer PRIVATE llmchat Catch2::Catch2WithMain)

include(CTest)
include(Catch)
catch_discover_tests(test_config)
catch_discover_tests(test_types)
catch_discover_tests(test_tools)
catch_discover_tests(test_client)
catch_discover_tests(test_chat)
catch_discover_tests(test_renderer)
