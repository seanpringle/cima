#include "lsp/json_rpc.h"
#include "lsp/lsp_client.h"
#include "mock_lsp_server.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <fstream>
#include <future>
#include <thread>

using Catch::Matchers::ContainsSubstring;

namespace fs = std::filesystem;

// Helper: create a temporary directory for file tests.
static std::string make_temp_dir() {
    char tmpl[] = "/tmp/cima_test_lsp_XXXXXX";
    char* result = mkdtemp(tmpl);
    REQUIRE(result != nullptr);
    return result;
}

// Helper: connect LspClient to MockLspServer and return the connected client.
// The caller must keep `mock` alive (it destructs before client).
struct ClientMock {
    MockLspServer mock;
    LspClient client;

    bool start(int delay_ms = 0) {
        if (delay_ms > 0)
            mock.set_response_delay(delay_ms);
        if (!mock.start())
            return false;
        auto conn = client.connect(mock.child_stdout(), mock.child_stdin());
        return conn.has_value();
    }

    ~ClientMock() { client.shutdown(); }
};

// Helper: query the mock for its last received notification.
static json mock_last_notif(LspClient& client) {
    auto resp = client.request("_mock_getLastNotification", {}, 5);
    REQUIRE(resp);
    return (*resp)["result"];
}

// Helper: query the mock for all notifications received so far.
static json mock_all_notifs(LspClient& client) {
    auto resp = client.request("_mock_getAllNotifications", {}, 5);
    REQUIRE(resp);
    return (*resp)["result"];
}

// Helper: clear mock's notification log.
static void mock_clear_notifs(LspClient& client) {
    client.request("_mock_clearNotifications", {}, 5);
}

// ===================================================================
// open_file
// ===================================================================

TEST_CASE("open_file sends textDocument/didOpen", "[lsp][filesync]") {
    ClientMock cm;
    REQUIRE(cm.start());

    cm.client.open_file("file:///test.cc", "cpp", "int x = 1;");

    auto last = mock_last_notif(cm.client);
    CHECK(last["method"] == "textDocument/didOpen");
    CHECK(last["params"]["textDocument"]["uri"] == "file:///test.cc");
    CHECK(last["params"]["textDocument"]["languageId"] == "cpp");
    CHECK(last["params"]["textDocument"]["text"] == "int x = 1;");
    CHECK(last["params"]["textDocument"]["version"] == 1);
}

TEST_CASE("open_file is idempotent with same content", "[lsp][filesync]") {
    ClientMock cm;
    REQUIRE(cm.start());

    cm.client.open_file("file:///test.cc", "cpp", "int x = 1;");
    mock_clear_notifs(cm.client);

    // Open again with identical content — should NOT send another didOpen
    cm.client.open_file("file:///test.cc", "cpp", "int x = 1;");

    auto all = mock_all_notifs(cm.client);
    CHECK(all.size() == 0); // no new notifications
}

TEST_CASE("open_file upgrades to didChange when content differs",
          "[lsp][filesync]") {
    ClientMock cm;
    REQUIRE(cm.start());

    cm.client.open_file("file:///test.cc", "cpp", "int x = 1;");
    mock_clear_notifs(cm.client);

    // Open again with different content — should send didChange, not didOpen
    cm.client.open_file("file:///test.cc", "cpp", "int x = 2;");

    auto last = mock_last_notif(cm.client);
    CHECK(last["method"] == "textDocument/didChange");
    CHECK(last["params"]["textDocument"]["uri"] == "file:///test.cc");
    CHECK(last["params"]["contentChanges"][0]["text"] == "int x = 2;");
    CHECK(last["params"]["textDocument"]["version"] == 2);
}

TEST_CASE("open_file tracks version numbers", "[lsp][filesync]") {
    ClientMock cm;
    REQUIRE(cm.start());

    cm.client.open_file("file:///test.cc", "cpp", "v1");
    CHECK(mock_last_notif(cm.client)["params"]["textDocument"]["version"] == 1);

    cm.client.open_file("file:///test.cc", "cpp", "v2");
    CHECK(mock_last_notif(cm.client)["params"]["textDocument"]["version"] == 2);

    cm.client.open_file("file:///test.cc", "cpp", "v3");
    CHECK(mock_last_notif(cm.client)["params"]["textDocument"]["version"] == 3);
}

// ===================================================================
// change_file
// ===================================================================

TEST_CASE("change_file sends textDocument/didChange", "[lsp][filesync]") {
    ClientMock cm;
    REQUIRE(cm.start());

    cm.client.open_file("file:///test.cc", "cpp", "int x = 1;");
    mock_clear_notifs(cm.client);

    cm.client.change_file("file:///test.cc", "int y = 2;", 2);

    auto last = mock_last_notif(cm.client);
    CHECK(last["method"] == "textDocument/didChange");
    CHECK(last["params"]["textDocument"]["uri"] == "file:///test.cc");
    CHECK(last["params"]["textDocument"]["version"] == 2);
    CHECK(last["params"]["contentChanges"][0]["text"] == "int y = 2;");
}

TEST_CASE("change_file errors if file not open", "[lsp][filesync]") {
    ClientMock cm;
    REQUIRE(cm.start());

    auto result = cm.client.change_file("file:///never_opened.cc", "content", 1);
    CHECK_FALSE(result);
}

// ===================================================================
// close_file
// ===================================================================

TEST_CASE("close_file sends textDocument/didClose", "[lsp][filesync]") {
    ClientMock cm;
    REQUIRE(cm.start());

    cm.client.open_file("file:///test.cc", "cpp", "int x = 1;");
    mock_clear_notifs(cm.client);

    cm.client.close_file("file:///test.cc");

    auto last = mock_last_notif(cm.client);
    CHECK(last["method"] == "textDocument/didClose");
    CHECK(last["params"]["textDocument"]["uri"] == "file:///test.cc");
}

TEST_CASE("close_file errors if file not open", "[lsp][filesync]") {
    ClientMock cm;
    REQUIRE(cm.start());

    auto result = cm.client.close_file("file:///never_opened.cc");
    CHECK_FALSE(result);
}

// ===================================================================
// ensure_file_synced
// ===================================================================

TEST_CASE("ensure_file_synced sends didChange when file changed on disk",
          "[lsp][filesync]") {
    ClientMock cm;
    REQUIRE(cm.start());

    // Create a temp file
    auto tmp_dir = fs::temp_directory_path() / "cima_test_lsp_XXXXXX";
    auto dir = make_temp_dir();
    auto path = dir + "/test.cc";
    std::ofstream(path) << "int original;";

    cm.client.open_file(lsp::path_to_uri(path), "cpp", "int original;");
    mock_clear_notifs(cm.client);

    // Modify file on disk
    std::ofstream(path) << "int modified;";

    // Call ensure_file_synced — should detect difference and send didChange
    auto result = cm.client.ensure_file_synced(
        lsp::path_to_uri(path), "cpp", "int modified;");
    CHECK(result);

    auto last = mock_last_notif(cm.client);
    CHECK(last["method"] == "textDocument/didChange");
    CHECK(last["params"]["contentChanges"][0]["text"] == "int modified;");

    fs::remove_all(dir);
}

TEST_CASE("ensure_file_synced is no-op when content matches",
          "[lsp][filesync]") {
    ClientMock cm;
    REQUIRE(cm.start());

    auto dir = make_temp_dir();
    auto path = dir + "/test.cc";
    std::ofstream(path) << "int x;";

    cm.client.open_file(lsp::path_to_uri(path), "cpp", "int x;");
    mock_clear_notifs(cm.client);

    // File unchanged — ensure_file_synced should do nothing
    auto result = cm.client.ensure_file_synced(
        lsp::path_to_uri(path), "cpp", "int x;");
    CHECK(result);

    auto all = mock_all_notifs(cm.client);
    CHECK(all.size() == 0);

    fs::remove_all(dir);
}

TEST_CASE("ensure_file_synced opens a not-yet-open file",
          "[lsp][filesync]") {
    ClientMock cm;
    REQUIRE(cm.start());

    // File not tracked yet — ensure_file_synced should open it
    auto result = cm.client.ensure_file_synced(
        "file:///newfile.cc", "cpp", "int fresh;");
    CHECK(result);
    CHECK(cm.client.is_file_open("file:///newfile.cc"));

    // Verify the mock received a didOpen for it
    auto last = mock_last_notif(cm.client);
    CHECK(last["method"] == "textDocument/didOpen");
    CHECK(last["params"]["textDocument"]["uri"] == "file:///newfile.cc");
}

// ===================================================================
// is_file_open
// ===================================================================

TEST_CASE("is_file_open tracks state correctly", "[lsp][filesync]") {
    ClientMock cm;
    REQUIRE(cm.start());

    CHECK_FALSE(cm.client.is_file_open("file:///test.cc"));

    cm.client.open_file("file:///test.cc", "cpp", "hello");
    CHECK(cm.client.is_file_open("file:///test.cc"));

    cm.client.close_file("file:///test.cc");
    CHECK_FALSE(cm.client.is_file_open("file:///test.cc"));
}

// ===================================================================
// Crash recovery — re-opens tracked files
// ===================================================================

TEST_CASE("LspClient restart re-opens previously open files",
          "[lsp][filesync]") {
    // This test validates crash recovery logic:
    // 1. Open two files in LspClient
    // 2. Simulate server crash
    // 3. Reconnect to a fresh mock
    // 4. Verify both files get didOpen again

    MockLspServer mock;
    REQUIRE(mock.start());

    LspClient client;
    REQUIRE(client.connect(mock.child_stdout(), mock.child_stdin()));

    client.open_file("file:///alpha.cc", "cpp", "int a;");
    client.open_file("file:///beta.cc", "cpp", "int b;");

    // Clear the notification log
    client.request("_mock_clearNotifications", {}, 5);

    // Simulate server restart: crash + reconnect to fresh mock
    mock.crash();

    // Start a fresh mock and reconnect
    MockLspServer mock2;
    REQUIRE(mock2.start());
    REQUIRE(client.connect(mock2.child_stdout(), mock2.child_stdin()));

    // Wait for re-open notifications to be sent
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Query mock2 for received notifications
    auto all = client.request("_mock_getAllNotifications", {}, 5);
    REQUIRE(all);
    auto notifs = (*all)["result"];

    // Should have received didOpen for alpha.cc and beta.cc
    bool found_alpha = false, found_beta = false;
    for (const auto& n : notifs) {
        if (n["method"] == "textDocument/didOpen") {
            auto uri = n["params"]["textDocument"]["uri"].get<std::string>();
            if (uri == "file:///alpha.cc") found_alpha = true;
            if (uri == "file:///beta.cc") found_beta = true;
        }
    }
    CHECK(found_alpha);
    CHECK(found_beta);

    client.shutdown();
}
