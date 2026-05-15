#include "notes.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

static std::string make_temp_dir() {
    char tmpl[] = "/tmp/cima_test_notes_XXXXXX";
    char* result = mkdtemp(tmpl);
    REQUIRE(result != nullptr);
    return result;
}

TEST_CASE("list_all_notes returns empty initially", "[notes]") {
    Notes notes;
    auto result = notes.list_all_notes();
    REQUIRE(result);
    CHECK(result->empty());
}

TEST_CASE("write_note then list_all_notes shows it", "[notes]") {
    Notes notes;
    auto r = notes.write_note("Hello", "World");
    REQUIRE(r);

    auto result = notes.list_all_notes();
    REQUIRE(result);
    REQUIRE(result->size() == 1);
    CHECK((*result)[0] == "Hello");
}

TEST_CASE("write_note overwrites existing", "[notes]") {
    Notes notes;
    notes.write_note("Hello", "World");
    notes.write_note("Hello", "Updated");

    auto result = notes.read_note("Hello");
    REQUIRE(result);
    CHECK(*result == "Updated");
}

TEST_CASE("read_note returns body", "[notes]") {
    Notes notes;
    notes.write_note("Test", "Body text");

    auto result = notes.read_note("Test");
    REQUIRE(result);
    CHECK(*result == "Body text");
}

TEST_CASE("read_note errors on missing name", "[notes]") {
    Notes notes;
    auto result = notes.read_note("NonExistent");
    CHECK_FALSE(result);
    CHECK(result.error().find("no such note") != std::string::npos);
}

TEST_CASE("delete_note removes it", "[notes]") {
    Notes notes;
    notes.write_note("Hello", "World");

    auto del = notes.delete_note("Hello");
    REQUIRE(del);

    auto list = notes.list_all_notes();
    REQUIRE(list);
    CHECK(list->empty());
}

TEST_CASE("delete_note errors on missing name", "[notes]") {
    Notes notes;
    auto result = notes.delete_note("NonExistent");
    CHECK_FALSE(result);
    CHECK(result.error().find("no such note") != std::string::npos);
}

TEST_CASE("delete_all_notes clears everything", "[notes]") {
    Notes notes;
    notes.write_note("A", "a");
    notes.write_note("B", "b");
    notes.write_note("C", "c");

    auto del = notes.delete_all_notes();
    REQUIRE(del);

    auto list = notes.list_all_notes();
    REQUIRE(list);
    CHECK(list->empty());
}

TEST_CASE("list_all_notes returns alphabetically sorted", "[notes]") {
    Notes notes;
    notes.write_note("Zebra", "z");
    notes.write_note("Alpha", "a");
    notes.write_note("Charlie", "c");

    auto result = notes.list_all_notes();
    REQUIRE(result);
    REQUIRE(result->size() == 3);
    CHECK((*result)[0] == "Alpha");
    CHECK((*result)[1] == "Charlie");
    CHECK((*result)[2] == "Zebra");
}

TEST_CASE("Notes save/load round-trip", "[notes]") {
    auto tmp = make_temp_dir();
    auto path = tmp + "/test_notes.json";

    // Write notes
    {
        Notes notes;
        notes.write_note("A", "apple");
        notes.write_note("B", "banana");
        notes.set_notes_file_path(path);
        auto r = notes.save();
        REQUIRE(r);
    }

    // Read back
    {
        Notes notes;
        auto r = notes.load_from_file(path);
        REQUIRE(r);

        auto list = notes.list_all_notes();
        REQUIRE(list);
        REQUIRE(list->size() == 2);
        CHECK((*list)[0] == "A");
        CHECK((*list)[1] == "B");

        auto body_a = notes.read_note("A");
        REQUIRE(body_a);
        CHECK(*body_a == "apple");

        auto body_b = notes.read_note("B");
        REQUIRE(body_b);
        CHECK(*body_b == "banana");
    }

    fs::remove_all(tmp);
}

TEST_CASE("Notes load_from_file with missing file does not error", "[notes]") {
    auto tmp = make_temp_dir();
    auto path = tmp + "/nonexistent.json";

    Notes notes;
    auto r = notes.load_from_file(path);
    REQUIRE(r);

    auto list = notes.list_all_notes();
    REQUIRE(list);
    CHECK(list->empty());

    fs::remove_all(tmp);
}
