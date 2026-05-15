#include "session_db.h"
#include "types.h"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

using json = nlohmann::json;

// ========================================================================
// SessionDB scratch space tests
// ========================================================================

TEST_CASE("SessionDB empty database has no tables", "[session_db]") {
    SessionDB db;

    auto result = db.execute("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name");
    REQUIRE(result);
    auto tables = json::parse(*result);
    REQUIRE(tables.is_array());
    CHECK(tables.empty());
}

TEST_CASE("SessionDB create table and insert", "[session_db]") {
    SessionDB db;

    auto r = db.execute("CREATE TABLE t (a INTEGER, b TEXT)");
    REQUIRE(r);

    auto r2 = db.execute("INSERT INTO t VALUES (1, 'hello')");
    REQUIRE(r2);
    auto parsed = json::parse(*r2);
    CHECK(parsed["rows_affected"] == 1);
}

TEST_CASE("SessionDB select returns rows", "[session_db]") {
    SessionDB db;

    db.execute("CREATE TABLE t (a INTEGER, b TEXT)");
    db.execute("INSERT INTO t VALUES (1, 'hello')");
    db.execute("INSERT INTO t VALUES (2, 'world')");

    auto r = db.execute("SELECT * FROM t ORDER BY a");
    REQUIRE(r);
    auto parsed = json::parse(*r);
    REQUIRE(parsed.is_array());
    REQUIRE(parsed.size() == 2);
    CHECK(parsed[0]["a"] == 1);
    CHECK(parsed[0]["b"] == "hello");
    CHECK(parsed[1]["a"] == 2);
    CHECK(parsed[1]["b"] == "world");
}

TEST_CASE("SessionDB null values round-trip", "[session_db]") {
    SessionDB db;

    db.execute("CREATE TABLE t (a INTEGER, b TEXT)");
    db.execute("INSERT INTO t (a, b) VALUES (NULL, 'text')");
    db.execute("INSERT INTO t (a, b) VALUES (42, NULL)");

    auto r = db.execute("SELECT * FROM t ORDER BY a");
    REQUIRE(r);
    auto parsed = json::parse(*r);
    REQUIRE(parsed.is_array());
    REQUIRE(parsed.size() == 2);

    CHECK(parsed[0]["a"].is_null());
    CHECK(parsed[0]["b"] == "text");
    CHECK(parsed[1]["a"] == 42);
    CHECK(parsed[1]["b"].is_null());
}

TEST_CASE("SessionDB UPDATE returns affected rows", "[session_db]") {
    SessionDB db;

    db.execute("CREATE TABLE t (a INTEGER)");
    db.execute("INSERT INTO t VALUES (1)");
    db.execute("INSERT INTO t VALUES (2)");
    db.execute("INSERT INTO t VALUES (3)");

    auto r = db.execute("UPDATE t SET a = a + 10 WHERE a >= 2");
    REQUIRE(r);
    auto parsed = json::parse(*r);
    CHECK(parsed["rows_affected"] == 2);
}

TEST_CASE("SessionDB DELETE returns affected rows", "[session_db]") {
    SessionDB db;

    db.execute("CREATE TABLE t (a INTEGER)");
    db.execute("INSERT INTO t VALUES (1)");
    db.execute("INSERT INTO t VALUES (2)");

    auto r = db.execute("DELETE FROM t WHERE a = 1");
    REQUIRE(r);
    auto parsed = json::parse(*r);
    CHECK(parsed["rows_affected"] == 1);

    auto r2 = db.execute("SELECT COUNT(*) AS cnt FROM t");
    REQUIRE(r2);
    auto parsed2 = json::parse(*r2);
    REQUIRE(parsed2.is_array());
    REQUIRE(parsed2.size() == 1);
    CHECK(parsed2[0]["cnt"] == 1);
}

TEST_CASE("SessionDB multiple statements not allowed", "[session_db]") {
    SessionDB db;

    auto r = db.execute("CREATE TABLE t (a INTEGER); INSERT INTO t VALUES (1)");
    REQUIRE(r);
    auto r2 = db.execute("SELECT COUNT(*) AS cnt FROM t");
    REQUIRE(r2);
    auto parsed = json::parse(*r2);
    REQUIRE(parsed.is_array());
    CHECK(parsed[0]["cnt"] == 0);
}

TEST_CASE("SessionDB empty SQL returns ok", "[session_db]") {
    SessionDB db;

    auto r = db.execute("");
    REQUIRE(r);
    CHECK(*r == R"({"ok": true})");

    auto r2 = db.execute("   ");
    REQUIRE(r2);
    CHECK(*r2 == R"({"ok": true})");
}
