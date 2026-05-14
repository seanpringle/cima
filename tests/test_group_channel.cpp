#include "group_channel.h"

#include <catch2/catch_test_macros.hpp>

// ===================================================================
// GroupChannel: @everyone notification delivery
// ===================================================================

TEST_CASE("GroupChannel @everyone notifies all agents", "[group_channel]") {
    GroupChannel ch;

    ch.register_agent(1, "Agent-Alice");
    ch.register_agent(2, "Agent-Bob");

    ch.post_message("user", "@everyone say hi", "@everyone say hi");

    auto alice_notes = ch.consume_notifications("Agent-Alice");
    REQUIRE(alice_notes.size() == 1);
    CHECK(alice_notes[0].from == "user");
    CHECK(alice_notes[0].summary == "@everyone say hi");

    auto bob_notes = ch.consume_notifications("Agent-Bob");
    REQUIRE(bob_notes.size() == 1);
    CHECK(bob_notes[0].from == "user");
    CHECK(bob_notes[0].summary == "@everyone say hi");

    // Sender ("user") is not an agent, so no self-notification issue
}

TEST_CASE("GroupChannel @everyone excludes sender", "[group_channel]") {
    GroupChannel ch;

    ch.register_agent(1, "Agent-Alice");
    ch.register_agent(2, "Agent-Bob");

    // Alice posts @everyone
    ch.post_message("Agent-Alice", "@everyone check this", "details");

    auto alice_notes = ch.consume_notifications("Agent-Alice");
    CHECK(alice_notes.empty()); // Alice should not get notified

    auto bob_notes = ch.consume_notifications("Agent-Bob");
    REQUIRE(bob_notes.size() == 1);
    CHECK(bob_notes[0].summary == "@everyone check this");
}

// ===================================================================
// GroupChannel: @all alias for @everyone
// ===================================================================

TEST_CASE("GroupChannel @all notifies all agents like @everyone", "[group_channel]") {
    GroupChannel ch;

    ch.register_agent(1, "Agent-Alice");
    ch.register_agent(2, "Agent-Bob");
    ch.register_agent(3, "Agent-Charlie");

    ch.post_message("user", "@all team meeting", "@all team meeting");

    auto alice_notes = ch.consume_notifications("Agent-Alice");
    REQUIRE(alice_notes.size() == 1);
    CHECK(alice_notes[0].summary == "@all team meeting");

    auto bob_notes = ch.consume_notifications("Agent-Bob");
    REQUIRE(bob_notes.size() == 1);

    auto charlie_notes = ch.consume_notifications("Agent-Charlie");
    REQUIRE(charlie_notes.size() == 1);
}

TEST_CASE("GroupChannel @all excludes sender", "[group_channel]") {
    GroupChannel ch;

    ch.register_agent(1, "Agent-Alice");
    ch.register_agent(2, "Agent-Bob");

    ch.post_message("Agent-Alice", "@all hello", "@all hello");

    auto alice_notes = ch.consume_notifications("Agent-Alice");
    CHECK(alice_notes.empty());

    auto bob_notes = ch.consume_notifications("Agent-Bob");
    REQUIRE(bob_notes.size() == 1);
}

// ===================================================================
// GroupChannel: individual @name notification
// ===================================================================

TEST_CASE("GroupChannel @mention notifies specific agent", "[group_channel]") {
    GroupChannel ch;

    ch.register_agent(1, "Agent-Alice");
    ch.register_agent(2, "Agent-Bob");

    ch.post_message("user", "@Agent-Bob help needed", "@Agent-Bob help needed");

    auto alice_notes = ch.consume_notifications("Agent-Alice");
    CHECK(alice_notes.empty());

    auto bob_notes = ch.consume_notifications("Agent-Bob");
    REQUIRE(bob_notes.size() == 1);
    CHECK(bob_notes[0].summary == "@Agent-Bob help needed");
}

TEST_CASE("GroupChannel @mention case insensitive", "[group_channel]") {
    GroupChannel ch;

    ch.register_agent(1, "Agent-Alice");
    ch.register_agent(2, "Agent-Bob");

    ch.post_message("user", "@agent-alice hi", "@agent-alice hi");

    auto alice_notes = ch.consume_notifications("Agent-Alice");
    REQUIRE(alice_notes.size() == 1);
}

// ===================================================================
// GroupChannel: prefix matching
// ===================================================================

TEST_CASE("GroupChannel @mention prefix matching", "[group_channel]") {
    GroupChannel ch;

    ch.register_agent(1, "Very-Little-Gravitas-Indeed");

    // Prefix "Very" should match "Very-Little-Gravitas-Indeed"
    ch.post_message("user", "@Very are you there?", "test");

    auto notes = ch.consume_notifications("Very-Little-Gravitas-Indeed");
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].summary == "@Very are you there?");
}

TEST_CASE("GroupChannel @mention prefix matching with hyphen", "[group_channel]") {
    GroupChannel ch;

    ch.register_agent(1, "A-Fine-Example");

    // Prefix "Fine" (with "-Fine" in name) should match
    ch.post_message("user", "@Fine test", "test");

    auto notes = ch.consume_notifications("A-Fine-Example");
    REQUIRE(notes.size() == 1);
}

// ===================================================================
// GroupChannel: no notification for unmatched tags
// ===================================================================

TEST_CASE("GroupChannel unmatched @mention does not notify", "[group_channel]") {
    GroupChannel ch;

    ch.register_agent(1, "Agent-Alice");

    ch.post_message("user", "@nonexistent hello", "test");

    auto alice_notes = ch.consume_notifications("Agent-Alice");
    CHECK(alice_notes.empty());
}

TEST_CASE("GroupChannel message with no @ does not notify", "[group_channel]") {
    GroupChannel ch;

    ch.register_agent(1, "Agent-Alice");

    ch.post_message("user", "just a normal message", "test");

    auto alice_notes = ch.consume_notifications("Agent-Alice");
    CHECK(alice_notes.empty());
}

// ===================================================================
// GroupChannel: @user tag does not notify agents
// ===================================================================

TEST_CASE("GroupChannel @user tag does not notify agents", "[group_channel]") {
    GroupChannel ch;

    ch.register_agent(1, "Agent-Alice");

    ch.post_message("Agent-Alice", "@user what do you think?", "test");

    auto alice_notes = ch.consume_notifications("Agent-Alice");
    CHECK(alice_notes.empty()); // @user should not trigger agent notification
}

// ===================================================================
// GroupChannel: pending notification body content
// ===================================================================

TEST_CASE("GroupChannel notification includes body", "[group_channel]") {
    GroupChannel ch;

    ch.register_agent(1, "Agent-Alice");

    ch.post_message("user", "@everyone short summary", "This is the long body with details.");

    auto notes = ch.consume_notifications("Agent-Alice");
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].summary == "@everyone short summary");
    CHECK(notes[0].body == "This is the long body with details.");
    CHECK(notes[0].from == "user");
}

// ===================================================================
// GroupChannel: consume_notifications clears the queue
// ===================================================================

TEST_CASE("GroupChannel consume_notifications clears queue", "[group_channel]") {
    GroupChannel ch;

    ch.register_agent(1, "Agent-Alice");

    ch.post_message("user", "@everyone first", "first");
    auto notes1 = ch.consume_notifications("Agent-Alice");
    REQUIRE(notes1.size() == 1);

    // Second consume should return empty
    auto notes2 = ch.consume_notifications("Agent-Alice");
    CHECK(notes2.empty());
}

// ===================================================================
// GroupChannel: pending_body_tags are present
// ===================================================================

TEST_CASE("GroupChannel tags include agent names for @everyone", "[group_channel]") {
    GroupChannel ch;

    ch.register_agent(1, "Agent-Alice");
    ch.register_agent(2, "Agent-Bob");

    ch.post_message("user", "@everyone hi all", "test");

    auto msgs = ch.list_all_messages();
    REQUIRE(msgs.size() == 1);

    // Tags should include all agent names (for display)
    bool has_alice = false, has_bob = false;
    for (const auto& tag : msgs[0].tags) {
        if (tag == "Agent-Alice") has_alice = true;
        if (tag == "Agent-Bob") has_bob = true;
    }
    CHECK(has_alice);
    CHECK(has_bob);
}

// ===================================================================
// GroupChannel: whole-word matching without @ prefix
// ===================================================================

TEST_CASE("GroupChannel whole-word agent name without @ triggers notification", "[group_channel]") {
    GroupChannel ch;

    ch.register_agent(1, "Agent-Alice");
    ch.register_agent(2, "Agent-Bob");

    // "Agent-Alice" appears in the body without @ prefix
    ch.post_message("user", "Can Agent-Alice help with this?", "details");

    auto alice_notes = ch.consume_notifications("Agent-Alice");
    REQUIRE(alice_notes.size() == 1);

    auto bob_notes = ch.consume_notifications("Agent-Bob");
    CHECK(bob_notes.empty());
}

TEST_CASE("GroupChannel partial word does not trigger notification without @", "[group_channel]") {
    GroupChannel ch;

    ch.register_agent(1, "Agent-Alice");

    // "Alice" is part of "Agent-Alice" but not a whole word match
    ch.post_message("user", "Alice is here", "test");

    auto notes = ch.consume_notifications("Agent-Alice");
    CHECK(notes.empty());
}
