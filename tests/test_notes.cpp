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

    auto result = notes.list_notes();
    REQUIRE(result);
    REQUIRE(result->size() == 1);
    CHECK((*result)[0] == 0);
}

TEST_CASE("write_note auto-assigns incrementing IDs", "[notes]") {
    Notes notes;
    notes.write_note("First");
    notes.write_note("Second");
    notes.write_note("Third");

    auto result = notes.list_notes();
    REQUIRE(result);
    REQUIRE(result->size() == 3);
    CHECK((*result)[0] == 0);
    CHECK((*result)[1] == 1);
    CHECK((*result)[2] == 2);
}

TEST_CASE("read_note returns body", "[notes]") {
    Notes notes;
    notes.write_note("Body text");

    auto result = notes.read_note(0);
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

    auto del = notes.delete_note(0);
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
    CHECK((*result)[0] == 0);
    CHECK((*result)[1] == 1);
    CHECK((*result)[2] == 2);
}

TEST_CASE("Notes serialization round-trip", "[notes]") {
    Notes notes;
    notes.write_note("apple");
    notes.write_note("banana");

    auto j = notes.to_json();
    REQUIRE(j.is_object());
    CHECK(j["0"] == "apple");
    CHECK(j["1"] == "banana");

    Notes notes2;
    notes2.from_json(j);

    auto list = notes2.list_notes();
    REQUIRE(list);
    REQUIRE(list->size() == 2);
    CHECK((*list)[0] == 0);
    CHECK((*list)[1] == 1);

    auto body_a = notes2.read_note(0);
    REQUIRE(body_a);
    CHECK(*body_a == "apple");

    auto body_b = notes2.read_note(1);
    REQUIRE(body_b);
    CHECK(*body_b == "banana");
}

TEST_CASE("from_json skips non-integer keys (backward compat)", "[notes]") {
    json j;
    j["0"] = "first";
    j["old_name"] = "skip me"; // non-integer key — should be ignored
    j["1"] = "second";

    Notes notes;
    notes.from_json(j);

    auto list = notes.list_notes();
    REQUIRE(list);
    CHECK(list->size() == 2);
    CHECK(notes.read_note(0) == "first");
    CHECK(notes.read_note(1) == "second");
}
