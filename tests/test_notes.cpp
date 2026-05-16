#include "notes.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("list_notes returns empty initially", "[notes]") {
    Notes notes;
    auto result = notes.list_notes();
    REQUIRE(result);
    CHECK(result->empty());
}

TEST_CASE("write_note then list_notes shows it", "[notes]") {
    Notes notes;
    auto r = notes.write_note("World");
    REQUIRE(r);
    CHECK(*r == 1); // first note gets ID 1

    auto result = notes.list_notes();
    REQUIRE(result);
    REQUIRE(result->size() == 1);
    CHECK((*result)[0] == 1);
}

TEST_CASE("write_note auto-assigns incrementing IDs", "[notes]") {
    Notes notes;
    notes.write_note("First");
    notes.write_note("Second");
    notes.write_note("Third");

    auto result = notes.list_notes();
    REQUIRE(result);
    REQUIRE(result->size() == 3);
    CHECK((*result)[0] == 1);
    CHECK((*result)[1] == 2);
    CHECK((*result)[2] == 3);
}

TEST_CASE("read_note returns body", "[notes]") {
    Notes notes;
    auto id = notes.write_note("Body text");
    REQUIRE(id);
    CHECK(*id == 1);

    auto result = notes.read_note(1);
    REQUIRE(result);
    CHECK(*result == "Body text");
}

TEST_CASE("read_note errors on missing ID", "[notes]") {
    Notes notes;
    auto result = notes.read_note(99);
    CHECK_FALSE(result);
    CHECK(result.error().find("no such note") != std::string::npos);
}

TEST_CASE("delete_note removes it", "[notes]") {
    Notes notes;
    notes.write_note("Body");

    auto del = notes.delete_note(1);
    REQUIRE(del);

    auto list = notes.list_notes();
    REQUIRE(list);
    CHECK(list->empty());
}

TEST_CASE("delete_note errors on missing ID", "[notes]") {
    Notes notes;
    auto result = notes.delete_note(42);
    CHECK_FALSE(result);
    CHECK(result.error().find("no such note") != std::string::npos);
}

TEST_CASE("list_notes returns IDs sorted ascending", "[notes]") {
    Notes notes;
    notes.write_note("z");
    notes.write_note("a");
    notes.write_note("c");

    auto result = notes.list_notes();
    REQUIRE(result);
    REQUIRE(result->size() == 3);
    CHECK((*result)[0] == 1);
    CHECK((*result)[1] == 2);
    CHECK((*result)[2] == 3);
}

TEST_CASE("Notes serialization round-trip", "[notes]") {
    Notes notes;
    notes.write_note("apple");
    notes.write_note("banana");

    auto j = notes.to_json();
    REQUIRE(j.is_object());
    CHECK(j["1"] == "apple");
    CHECK(j["2"] == "banana");
    CHECK(j["_next_id"] == 3);

    Notes notes2;
    notes2.from_json(j);

    auto list = notes2.list_notes();
    REQUIRE(list);
    REQUIRE(list->size() == 2);
    CHECK((*list)[0] == 1);
    CHECK((*list)[1] == 2);

    auto body_a = notes2.read_note(1);
    REQUIRE(body_a);
    CHECK(*body_a == "apple");

    auto body_b = notes2.read_note(2);
    REQUIRE(body_b);
    CHECK(*body_b == "banana");

    // Next auto-assigned ID continues correctly after reload
    auto r = notes2.write_note("cherry");
    REQUIRE(r);
    CHECK(*r == 3);
    CHECK(notes2.list_notes()->size() == 3);
}

TEST_CASE("from_json computes next_id from max when _next_id missing", "[notes]") {
    json j;
    j["1"] = "alpha";
    j["3"] = "gamma"; // gap at 2

    Notes notes;
    notes.from_json(j);

    CHECK(notes.list_notes()->size() == 2);
    CHECK(notes.read_note(1) == "alpha");
    CHECK(notes.read_note(3) == "gamma");

    // next_id_ should be max + 1 = 4
    auto r = notes.write_note("delta");
    REQUIRE(r);
    CHECK(*r == 4);
}

TEST_CASE("from_json handles empty object", "[notes]") {
    Notes notes;
    notes.from_json(json::object());
    CHECK(notes.list_notes()->empty());

    // next_id_ starts at 1
    auto r = notes.write_note("first");
    REQUIRE(r);
    CHECK(*r == 1);
}

TEST_CASE("write_note with explicit ID", "[notes]") {
    Notes notes;
    auto id = notes.write_note("first", 10);
    REQUIRE(id);
    CHECK(*id == 10);
    CHECK(notes.read_note(10) == "first");

    // Next auto-assigned ID advances past the explicit ID
    auto id2 = notes.write_note("second");
    REQUIRE(id2);
    CHECK(*id2 == 11);

    // Explicit overwrite of existing note
    auto id3 = notes.write_note("updated first", 10);
    REQUIRE(id3);
    CHECK(*id3 == 10);
    CHECK(notes.read_note(10) == "updated first");
}

TEST_CASE("from_json skips non-integer keys (backward compat)", "[notes]") {
    json j;
    j["1"] = "first";
    j["old_name"] = "skip me"; // non-integer key — should be ignored
    j["2"] = "second";

    Notes notes;
    notes.from_json(j);

    auto list = notes.list_notes();
    REQUIRE(list);
    CHECK(list->size() == 2);
    CHECK(notes.read_note(1) == "first");
    CHECK(notes.read_note(2) == "second");
}
