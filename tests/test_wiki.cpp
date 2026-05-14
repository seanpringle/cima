#include "wiki.h"
#include "tools.h"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

using json = nlohmann::json;

// ========================================================================
// Wiki CRUD basics
// ========================================================================

TEST_CASE("Wiki empty page list on fresh database", "[wiki]") {
    Wiki wiki(":memory:");
    auto pages = wiki.list_pages();
    REQUIRE(pages);
    CHECK(pages->empty());
}

TEST_CASE("Wiki write and list pages", "[wiki]") {
    Wiki wiki(":memory:");

    auto w = wiki.write_page("Home", "Welcome to the wiki");
    REQUIRE(w);

    w = wiki.write_page("About", "This is the about page");
    REQUIRE(w);

    auto pages = wiki.list_pages();
    REQUIRE(pages);
    CHECK(pages->size() == 2);
    CHECK((*pages)[0] == "About");
    CHECK((*pages)[1] == "Home");
}

TEST_CASE("Wiki read_page returns body", "[wiki]") {
    Wiki wiki(":memory:");
    wiki.write_page("TestPage", "Hello, world!");

    auto body = wiki.read_page("TestPage");
    REQUIRE(body);
    CHECK(*body == "Hello, world!");
}

TEST_CASE("Wiki read_page returns error for missing page", "[wiki]") {
    Wiki wiki(":memory:");
    auto body = wiki.read_page("NonExistent");
    CHECK(!body);
    CHECK(body.error().find("no such page") != std::string::npos);
}

TEST_CASE("Wiki write_page overwrites existing page", "[wiki]") {
    Wiki wiki(":memory:");
    wiki.write_page("Page", "version 1");
    wiki.write_page("Page", "version 2");

    auto body = wiki.read_page("Page");
    REQUIRE(body);
    CHECK(*body == "version 2");
}

TEST_CASE("Wiki delete_page removes a page", "[wiki]") {
    Wiki wiki(":memory:");
    wiki.write_page("Page", "some content");

    auto r = wiki.delete_page("Page");
    REQUIRE(r);

    auto pages = wiki.list_pages();
    REQUIRE(pages);
    CHECK(pages->empty());
}

TEST_CASE("Wiki delete_page returns error for missing page", "[wiki]") {
    Wiki wiki(":memory:");
    auto r = wiki.delete_page("NonExistent");
    CHECK(!r);
    CHECK(r.error().find("no such page") != std::string::npos);
}

TEST_CASE("Wiki edit_page exact match once", "[wiki]") {
    Wiki wiki(":memory:");
    wiki.write_page("Page", "Hello world, hello universe");

    auto r = wiki.edit_page("Page", "world", "earth");
    REQUIRE(r);

    auto body = wiki.read_page("Page");
    REQUIRE(body);
    // Only the first occurrence is replaced
    CHECK(*body == "Hello earth, hello universe");
}

TEST_CASE("Wiki edit_page returns error for zero matches", "[wiki]") {
    Wiki wiki(":memory:");
    wiki.write_page("Page", "Hello world");

    auto r = wiki.edit_page("Page", "mars", "venus");
    CHECK(!r);
    CHECK(r.error().find("not found") != std::string::npos);
}

TEST_CASE("Wiki edit_page returns error for multiple matches", "[wiki]") {
    Wiki wiki(":memory:");
    wiki.write_page("Page", "foo foo foo");

    auto r = wiki.edit_page("Page", "foo", "bar");
    CHECK(!r);
    CHECK(r.error().find("3 times") != std::string::npos);
}

TEST_CASE("Wiki edit_page returns error for missing page", "[wiki]") {
    Wiki wiki(":memory:");
    auto r = wiki.edit_page("NonExistent", "foo", "bar");
    CHECK(!r);
    CHECK(r.error().find("no such page") != std::string::npos);
}

TEST_CASE("Wiki empty body allowed", "[wiki]") {
    Wiki wiki(":memory:");
    wiki.write_page("Empty", "");

    auto body = wiki.read_page("Empty");
    REQUIRE(body);
    CHECK(body->empty());
}

TEST_CASE("Wiki page titles are case-sensitive", "[wiki]") {
    Wiki wiki(":memory:");
    wiki.write_page("Page", "content");

    // Different case = different page
    wiki.write_page("page", "other");
    CHECK(wiki.read_page("Page").value() == "content");
    CHECK(wiki.read_page("page").value() == "other");
}

// ========================================================================
// Wiki tools (via Tool objects)
// ========================================================================

TEST_CASE("Wiki tool list_wiki_pages returns JSON array", "[wiki][tools]") {
    Wiki wiki(":memory:");
    wiki.write_page("A", "aaa");
    wiki.write_page("B", "bbb");

    auto tool = make_list_wiki_pages_tool(wiki);
    json args = json::object();
    auto result = tool.execute(args);
    REQUIRE(result);

    auto arr = json::parse(*result);
    REQUIRE(arr.is_array());
    CHECK(arr.size() == 2);
    CHECK(arr[0] == "A");
    CHECK(arr[1] == "B");
}

TEST_CASE("Wiki tool list_wiki_pages empty properties", "[wiki][tools]") {
    Wiki wiki(":memory:");

    auto tool = make_list_wiki_pages_tool(wiki);
    // Verify the parameters schema has an empty properties object (not null)
    CHECK(tool.parameters.contains("properties"));
    CHECK(tool.parameters["properties"].is_object());
    CHECK(tool.parameters["properties"].empty());
    // Verify it's an object, not an array
    CHECK(tool.parameters["properties"].type() == json::value_t::object);

    // Execute with empty args
    json args = json::object();
    auto result = tool.execute(args);
    REQUIRE(result);

    auto arr = json::parse(*result);
    REQUIRE(arr.is_array());
    CHECK(arr.empty());
}

TEST_CASE("Wiki tool read_wiki_page returns page body", "[wiki][tools]") {
    Wiki wiki(":memory:");
    wiki.write_page("Test", "line1\nline2\nline3");

    auto tool = make_read_wiki_page_tool(wiki);
    json args = {{"page_title", "Test"}};
    auto result = tool.execute(args);
    REQUIRE(result);
    CHECK(result->find("line1") != std::string::npos);
    CHECK(result->find("line2") != std::string::npos);
    CHECK(result->find("line3") != std::string::npos);
}

TEST_CASE("Wiki tool read_wiki_page with offset and max_lines", "[wiki][tools]") {
    Wiki wiki(":memory:");
    wiki.write_page("Test", "line1\nline2\nline3\nline4\nline5");

    auto tool = make_read_wiki_page_tool(wiki);
    // offset=2 means skip 2 lines, so start from line 3
    json args = {{"page_title", "Test"}, {"offset", 2}, {"max_lines", 2}};
    auto result = tool.execute(args);
    REQUIRE(result);
    CHECK(result->find("line3") != std::string::npos);
    CHECK(result->find("line4") != std::string::npos);
    CHECK(result->find("line1") == std::string::npos);
    CHECK(result->find("line2") == std::string::npos);
    CHECK(result->find("line5") == std::string::npos);
}

TEST_CASE("Wiki tool write_wiki_page creates and overwrites", "[wiki][tools]") {
    Wiki wiki(":memory:");

    auto tool = make_write_wiki_page_tool(wiki);
    json args = {{"page_title", "Page"}, {"page_body", "hello"}};
    auto result = tool.execute(args);
    REQUIRE(result);
    CHECK(result->find("ok") != std::string::npos);

    // Overwrite
    args["page_body"] = "world";
    result = tool.execute(args);
    REQUIRE(result);
    CHECK(result->find("ok") != std::string::npos);

    auto body = wiki.read_page("Page");
    REQUIRE(body);
    CHECK(*body == "world");
}

TEST_CASE("Wiki tool edit_wiki_page exact match", "[wiki][tools]") {
    Wiki wiki(":memory:");
    wiki.write_page("Page", "hello world");

    auto tool = make_edit_wiki_page_tool(wiki);
    json args = {{"page_title", "Page"}, {"search", "hello"}, {"replace", "goodbye"}};
    auto result = tool.execute(args);
    REQUIRE(result);
    CHECK(result->find("ok") != std::string::npos);

    auto body = wiki.read_page("Page");
    REQUIRE(body);
    CHECK(*body == "goodbye world");
}

TEST_CASE("Wiki tool delete_wiki_page returns ok or no such page", "[wiki][tools]") {
    Wiki wiki(":memory:");
    wiki.write_page("Page", "content");

    auto tool = make_delete_wiki_page_tool(wiki);

    // Delete existing page
    json args = {{"page_title", "Page"}};
    auto result = tool.execute(args);
    REQUIRE(result);
    CHECK(*result == "ok");

    // Delete non-existent page
    result = tool.execute(args);
    CHECK(!result);
    CHECK(result.error().find("no such page") != std::string::npos);
}

TEST_CASE("Wiki tool write_wiki_page requires page_title", "[wiki][tools]") {
    Wiki wiki(":memory:");

    auto tool = make_write_wiki_page_tool(wiki);
    json args = {{"page_body", "hello"}};
    auto result = tool.execute(args);
    CHECK(!result);
}

TEST_CASE("Wiki tool read_wiki_page requires page_title", "[wiki][tools]") {
    Wiki wiki(":memory:");

    auto tool = make_read_wiki_page_tool(wiki);
    json args = json::object();
    auto result = tool.execute(args);
    CHECK(!result);
}

TEST_CASE("Wiki tool delete_wiki_page requires page_title", "[wiki][tools]") {
    Wiki wiki(":memory:");

    auto tool = make_delete_wiki_page_tool(wiki);
    json args = json::object();
    auto result = tool.execute(args);
    CHECK(!result);
}

TEST_CASE("Wiki tool edit_wiki_page requires all params", "[wiki][tools]") {
    Wiki wiki(":memory:");
    wiki.write_page("Page", "content");

    auto tool = make_edit_wiki_page_tool(wiki);

    // Missing search
    json args = {{"page_title", "Page"}, {"replace", "bar"}};
    auto result = tool.execute(args);
    CHECK(!result);

    // Missing page_title
    args = {{"search", "foo"}, {"replace", "bar"}};
    result = tool.execute(args);
    CHECK(!result);
}

TEST_CASE("Wiki tool edit_wiki_page empty search rejected", "[wiki][tools]") {
    Wiki wiki(":memory:");
    wiki.write_page("Page", "content");

    auto tool = make_edit_wiki_page_tool(wiki);
    json args = {{"page_title", "Page"}, {"search", ""}, {"replace", "bar"}};
    auto result = tool.execute(args);
    CHECK(!result);
}
