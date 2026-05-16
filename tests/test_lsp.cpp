#include "lsp/json_rpc.h"
#include "lsp/lsp_client.h"
#include "mock_lsp_server.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <future>
#include <thread>

using Catch::Matchers::ContainsSubstring;

// ===================================================================
// Helper: ClientMock — LspClient connected to a MockLspServer
// ===================================================================

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

    ~ClientMock() {
        client.shutdown();
    }
};

// ===================================================================
// MockLspServer lifecycle (Ticket 2)
// ===================================================================

TEST_CASE("MockLspServer starts and is running", "[lsp][mock]") {
    MockLspServer mock;
    CHECK(mock.start());
    CHECK(mock.is_running());
}

TEST_CASE("MockLspServer responds to initialize", "[lsp][mock]") {
    MockLspServer mock;
    CHECK(mock.start());

    auto caps = mock.send_initialize();
    REQUIRE(caps);
    CHECK(caps->contains("capabilities"));
    CHECK((*caps)["capabilities"].contains("textDocument"));
}

TEST_CASE("MockLspServer responds to arbitrary request", "[lsp][mock]") {
    MockLspServer mock;
    CHECK(mock.start());

    auto resp = mock.send_request("textDocument/hover",
                                  {{"textDocument", {{"uri", "file:///test.cc"}}},
                                   {"position", {{"line", 10}, {"character", 5}}}});
    REQUIRE(resp);
    CHECK(resp->contains("result"));
}

TEST_CASE("MockLspServer shuts down cleanly", "[lsp][mock]") {
    MockLspServer mock;
    CHECK(mock.start());
    CHECK(mock.is_running());
    mock.shutdown();
    CHECK_FALSE(mock.is_running());
}

TEST_CASE("MockLspServer simulates pull diagnostics", "[lsp][mock]") {
    MockLspServer mock;
    mock.set_diagnostics_response(json::array({
        json::object({
            {"range", json::object({
                {"start", json::object({{"line", 0}, {"character", 0}})},
                {"end", json::object({{"line", 0}, {"character", 5}})}
            })},
            {"severity", 1},
            {"message", "test error"},
            {"source", "clangd"}
        })
    }));
    CHECK(mock.start());

    auto diag = mock.send_pull_diagnostics("file:///test.cc");
    REQUIRE(diag);
    REQUIRE(diag->is_array());
    REQUIRE(diag->size() == 1);
    CHECK((*diag)[0]["message"] == "test error");
    CHECK((*diag)[0]["severity"] == 1);
}

TEST_CASE("MockLspServer pull diagnostics empty", "[lsp][mock]") {
    MockLspServer mock;
    CHECK(mock.start());
    auto diag = mock.send_pull_diagnostics("file:///test.cc");
    REQUIRE(diag);
    CHECK(diag->empty());
}

TEST_CASE("MockLspServer supports configurable delay", "[lsp][mock]") {
    MockLspServer mock;
    mock.set_response_delay(200);
    CHECK(mock.start());

    auto start = std::chrono::steady_clock::now();
    auto resp = mock.send_request("textDocument/hover", {}, 5000);
    auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(resp);
    CHECK(elapsed >= std::chrono::milliseconds(150));
}

TEST_CASE("MockLspServer simulates crash", "[lsp][mock]") {
    MockLspServer mock;
    CHECK(mock.start());
    CHECK(mock.is_running());

    mock.crash();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_FALSE(mock.is_running());
}

TEST_CASE("MockLspServer crash during request", "[lsp][mock]") {
    MockLspServer mock;
    mock.set_response_delay(5000);
    CHECK(mock.start());

    auto fut = std::async(std::launch::async, [&] {
        return mock.send_request("textDocument/hover", {}, 10000);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    mock.crash();

    auto result = fut.get();
    CHECK_FALSE(result);
}

TEST_CASE("MockLspServer handles multiple sequential requests",
          "[lsp][mock]") {
    MockLspServer mock;
    CHECK(mock.start());

    auto r1 = mock.send_request("textDocument/hover", {});
    REQUIRE(r1);
    auto r2 = mock.send_request("textDocument/completion", {});
    REQUIRE(r2);
    CHECK((*r1)["id"].get<int>() != (*r2)["id"].get<int>());
}

TEST_CASE("MockLspServer handles unknown method gracefully",
          "[lsp][mock]") {
    MockLspServer mock;
    CHECK(mock.start());

    auto resp = mock.send_request("some/unknown/method", {});
    REQUIRE(resp);
    CHECK((resp->contains("error") || resp->contains("result")));
}

TEST_CASE("MockLspServer sends push diagnostics notification",
          "[lsp][mock]") {
    MockLspServer mock;
    MockLspServer::PushDiag expected;
    expected.uri = "file:///test.cc";
    expected.diagnostics = json::array({
        json::object({
            {"range", json::object({
                {"start", json::object({{"line", 1}, {"character", 0}})},
                {"end", json::object({{"line", 1}, {"character", 10}})}
            })},
            {"severity", 2},
            {"message", "warning: unused variable"}
        })
    });
    mock.set_push_diagnostics(expected);
    CHECK(mock.start());

    auto caps = mock.send_initialize();
    REQUIRE(caps);

    auto notif = mock.read_push_notification(1000);
    REQUIRE(notif);
    CHECK((*notif)["method"] == "textDocument/publishDiagnostics");
    CHECK((*notif)["params"]["uri"] == "file:///test.cc");
    CHECK((*notif)["params"]["diagnostics"].size() == 1);
    CHECK((*notif)["params"]["diagnostics"][0]["message"] == "warning: unused variable");
}

// ===================================================================
// LspClient lifecycle (Ticket 3)
// ===================================================================

TEST_CASE("LspClient connects and performs initialize handshake",
          "[lsp][client]") {
    ClientMock cm;
    CHECK(cm.start());
    CHECK(cm.client.is_running());
    CHECK(cm.client.server_capabilities().contains("textDocument"));
}

TEST_CASE("LspClient fails cleanly with broken pipe",
          "[lsp][client]") {
    LspClient client;
    int pipes[2];
    CHECK(pipe(pipes) == 0);
    close(pipes[0]);
    close(pipes[1]);
    auto result = client.connect(pipes[0], pipes[1]);
    CHECK_FALSE(result);
}

TEST_CASE("LspClient request/response round-trip", "[lsp][client]") {
    ClientMock cm;
    REQUIRE(cm.start());

    auto resp = cm.client.request("textDocument/hover", {{"uri", "file:///test.cc"}}, 5);
    REQUIRE(resp);
    CHECK((*resp)["jsonrpc"] == "2.0");
    CHECK((*resp).contains("id"));
    CHECK((*resp).contains("result"));
}

TEST_CASE("LspClient multiple sequential requests", "[lsp][client]") {
    ClientMock cm;
    REQUIRE(cm.start());

    auto r1 = cm.client.request("textDocument/hover", {}, 5);
    REQUIRE(r1);
    auto r2 = cm.client.request("textDocument/completion", {}, 5);
    REQUIRE(r2);
    CHECK((*r1)["id"].get<int>() != (*r2)["id"].get<int>());
}

TEST_CASE("LspClient sends notification", "[lsp][client]") {
    ClientMock cm;
    REQUIRE(cm.start());

    auto result = cm.client.notify("textDocument/didOpen", {
        {"textDocument", {{"uri", "file:///test.cc"}, {"languageId", "cpp"}, {"version", 1}, {"text", ""}}}
    });
    CHECK(result);
}

TEST_CASE("LspClient timeout triggers error", "[lsp][client]") {
    ClientMock cm;
    REQUIRE(cm.start(2000));

    auto resp = cm.client.request("textDocument/hover", {}, 1);
    CHECK_FALSE(resp);
    CHECK(resp.error().find("timed out") != std::string::npos);
}

TEST_CASE("LspClient shutdown is clean", "[lsp][client]") {
    ClientMock cm;
    REQUIRE(cm.start());
    CHECK(cm.client.is_running());

    auto result = cm.client.shutdown();
    CHECK(result);
    CHECK_FALSE(cm.client.is_running());
}

TEST_CASE("LspClient handles concurrent requests", "[lsp][client]") {
    ClientMock cm;
    REQUIRE(cm.start());

    auto fut1 = std::async(std::launch::async, [&] {
        return cm.client.request("textDocument/hover", {}, 5);
    });
    auto fut2 = std::async(std::launch::async, [&] {
        return cm.client.request("textDocument/completion", {}, 5);
    });

    auto r1 = fut1.get();
    auto r2 = fut2.get();
    REQUIRE(r1);
    REQUIRE(r2);
    CHECK((*r1)["id"].get<int>() != (*r2)["id"].get<int>());
}

TEST_CASE("LspClient detects server crash during request", "[lsp][client]") {
    MockLspServer mock;
    mock.set_response_delay(5000);
    REQUIRE(mock.start());

    LspClient client;
    CHECK(client.connect(mock.child_stdout(), mock.child_stdin()).has_value());

    std::promise<Result<json>> result_promise;
    auto result_future = result_promise.get_future();
    std::thread worker([&] {
        result_promise.set_value(client.request("textDocument/hover", {}, 30));
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    mock.crash();
    worker.join();

    auto result = result_future.get();
    CHECK_FALSE(result);
    CHECK((result.error().find("closed") != std::string::npos ||
           result.error().find("EOF") != std::string::npos ||
           result.error().find("eof") != std::string::npos));
}

TEST_CASE("LspClient receives error response", "[lsp][client]") {
    ClientMock cm;
    REQUIRE(cm.start());

    auto resp = cm.client.request("nonexistent/method", {}, 5);
    REQUIRE(resp);
    CHECK(resp->contains("error"));
    CHECK((*resp)["error"]["code"] == lsp::ErrorCodes::MethodNotFound);
}

// ===================================================================
// File sync (Ticket 4)
// ===================================================================

TEST_CASE("open_file sends didOpen and tracks state", "[lsp][filesync]") {
    ClientMock cm;
    REQUIRE(cm.start());

    CHECK_FALSE(cm.client.is_file_open("file:///test.cc"));

    auto result = cm.client.open_file("file:///test.cc", "cpp", "int x = 1;");
    CHECK(result);
    CHECK(cm.client.is_file_open("file:///test.cc"));

    auto notifs_opt = cm.mock.get_received_notifications();
    REQUIRE(notifs_opt);
    auto& notifs = *notifs_opt;
    REQUIRE(notifs.size() >= 1);
    CHECK(notifs[0]["method"] == "textDocument/didOpen");
    CHECK(notifs[0]["params"]["textDocument"]["uri"] == "file:///test.cc");
    CHECK(notifs[0]["params"]["textDocument"]["languageId"] == "cpp");
    CHECK(notifs[0]["params"]["textDocument"]["text"] == "int x = 1;");
}

TEST_CASE("open_file idempotent with same content", "[lsp][filesync]") {
    ClientMock cm;
    REQUIRE(cm.start());

    auto r1 = cm.client.open_file("file:///test.cc", "cpp", "int x = 1;");
    REQUIRE(r1);
    auto r2 = cm.client.open_file("file:///test.cc", "cpp", "int x = 1;");
    REQUIRE(r2);

    auto notifs_opt = cm.mock.get_received_notifications();
    REQUIRE(notifs_opt);
    auto& notifs = *notifs_opt;
    int did_open_count = 0;
    for (const auto& n : notifs) {
        if (n["method"] == "textDocument/didOpen")
            did_open_count++;
    }
    CHECK(did_open_count == 1);
}

TEST_CASE("open_file with different content upgrades to didChange",
          "[lsp][filesync]") {
    ClientMock cm;
    REQUIRE(cm.start());

    auto r1 = cm.client.open_file("file:///test.cc", "cpp", "int x = 1;");
    REQUIRE(r1);
    auto r2 = cm.client.open_file("file:///test.cc", "cpp", "int y = 2;");
    REQUIRE(r2);

    auto notifs_opt = cm.mock.get_received_notifications();
    REQUIRE(notifs_opt);
    auto& notifs = *notifs_opt;
    bool saw_open = false, saw_change = false;
    for (const auto& n : notifs) {
        if (n["method"] == "textDocument/didOpen")  saw_open = true;
        if (n["method"] == "textDocument/didChange") saw_change = true;
    }
    CHECK(saw_open);
    CHECK(saw_change);
}

TEST_CASE("change_file sends didChange with correct content",
          "[lsp][filesync]") {
    ClientMock cm;
    REQUIRE(cm.start());
    REQUIRE(cm.client.open_file("file:///test.cc", "cpp", "int x = 1;"));

    auto result = cm.client.change_file("file:///test.cc", "int y = 2;", 2);
    CHECK(result);

    auto notifs_opt = cm.mock.get_received_notifications();
    REQUIRE(notifs_opt);
    auto& notifs = *notifs_opt;
    bool found = false;
    for (const auto& n : notifs) {
        if (n["method"] == "textDocument/didChange") {
            CHECK(n["params"]["textDocument"]["uri"] == "file:///test.cc");
            CHECK(n["params"]["contentChanges"][0]["text"] == "int y = 2;");
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("close_file sends didClose and clears state", "[lsp][filesync]") {
    ClientMock cm;
    REQUIRE(cm.start());
    REQUIRE(cm.client.open_file("file:///test.cc", "cpp", "int x;"));
    CHECK(cm.client.is_file_open("file:///test.cc"));

    cm.mock.clear_notifications();
    auto result = cm.client.close_file("file:///test.cc");
    CHECK(result);
    CHECK_FALSE(cm.client.is_file_open("file:///test.cc"));

    auto notifs_opt = cm.mock.get_received_notifications();
    REQUIRE(notifs_opt);
    auto& notifs = *notifs_opt;
    REQUIRE(notifs.size() >= 1);
    CHECK(notifs[0]["method"] == "textDocument/didClose");
    CHECK(notifs[0]["params"]["textDocument"]["uri"] == "file:///test.cc");
}

TEST_CASE("close_file no-op on unopened file", "[lsp][filesync]") {
    ClientMock cm;
    REQUIRE(cm.start());
    auto result = cm.client.close_file("file:///never_opened.cc");
    CHECK(result);
}

TEST_CASE("ensure_file_synced opens new file", "[lsp][filesync]") {
    ClientMock cm;
    REQUIRE(cm.start());

    auto result = cm.client.ensure_file_synced(
        "file:///test.cc", "cpp", "int x = 1;");
    CHECK(result);
    CHECK(cm.client.is_file_open("file:///test.cc"));
}

TEST_CASE("ensure_file_synced detects stale content", "[lsp][filesync]") {
    ClientMock cm;
    REQUIRE(cm.start());
    REQUIRE(cm.client.open_file("file:///test.cc", "cpp", "int x = 1;"));
    cm.mock.clear_notifications();

    auto result = cm.client.ensure_file_synced(
        "file:///test.cc", "cpp", "int y = 2;");
    CHECK(result);

    auto notifs_opt = cm.mock.get_received_notifications();
    REQUIRE(notifs_opt);
    auto& notifs = *notifs_opt;
    bool saw_change = false;
    for (const auto& n : notifs) {
        if (n["method"] == "textDocument/didChange")
            saw_change = true;
    }
    CHECK(saw_change);
}

TEST_CASE("ensure_file_synced no-op when content matches", "[lsp][filesync]") {
    ClientMock cm;
    REQUIRE(cm.start());
    REQUIRE(cm.client.open_file("file:///test.cc", "cpp", "int x = 1;"));
    cm.mock.clear_notifications();

    auto result = cm.client.ensure_file_synced(
        "file:///test.cc", "cpp", "int x = 1;");
    CHECK(result);

    auto notifs_opt = cm.mock.get_received_notifications();
    REQUIRE(notifs_opt);
    CHECK(notifs_opt->empty());
}

TEST_CASE("language_id_from_extension works correctly",
          "[lsp][filesync]") {
    CHECK(LspClient::language_id_from_extension("/path/to/file.cpp") == "cpp");
    CHECK(LspClient::language_id_from_extension("/path/to/file.h") == "c");
    CHECK(LspClient::language_id_from_extension("/path/to/file.hpp") == "cpp");
    CHECK(LspClient::language_id_from_extension("/path/to/file.c") == "c");
    CHECK(LspClient::language_id_from_extension("/path/to/file.cc") == "cpp");
    CHECK(LspClient::language_id_from_extension("/path/to/file.cxx") == "cpp");
    CHECK(LspClient::language_id_from_extension("/path/to/file.hxx") == "cpp");
    CHECK(LspClient::language_id_from_extension("/path/to/file.unknown") == "");
    CHECK(LspClient::language_id_from_extension("/path/to/Makefile") == "");
}

TEST_CASE("LspClient restart re-opens tracked files", "[lsp][filesync]") {
    MockLspServer mock;
    mock.set_response_delay(200);
    REQUIRE(mock.start());

    LspClient client;
    CHECK(client.connect(mock.child_stdout(), mock.child_stdin()).has_value());

    REQUIRE(client.open_file("file:///test.cc", "cpp", "int x;"));
    REQUIRE(client.open_file("file:///util.h", "cpp", "void foo();"));
    REQUIRE(client.is_file_open("file:///test.cc"));
    REQUIRE(client.is_file_open("file:///util.h"));

    mock.crash();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_FALSE(client.is_running());

    MockLspServer mock2;
    REQUIRE(mock2.start());
    auto conn = client.connect(mock2.child_stdout(), mock2.child_stdin());
    CHECK(conn.has_value());

    CHECK(client.is_file_open("file:///test.cc"));
    CHECK(client.is_file_open("file:///util.h"));

    auto notifs_opt = mock2.get_received_notifications();
    REQUIRE(notifs_opt);
    int open_count = 0;
    for (const auto& n : *notifs_opt) {
        if (n["method"] == "textDocument/didOpen")
            open_count++;
    }
    CHECK(open_count == 2);
}
