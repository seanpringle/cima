#include "group_channel.h"

#include <catch2/catch_test_macros.hpp>

// ===================================================================
// GroupChannel: @everyone notification delivery
// ===================================================================

TEST_CASE("GroupChannel @everyone notifies all agents", "[group_channel]") {
    GroupChannel ch;

    ch.register_agent(1, "Agent-Alice");
    ch.register_agent(2, "Agent-Bob");

    ch.post_message("user", "@everyone say hi");

    auto alice_notes = ch.consume_notifications("Agent-Alice");
    REQUIRE(alice_notes.size() == 1);
    CHECK(alice_notes[0].from == "user");
    CHECK(alice_notes[0].message == "@everyone say hi");

    auto bob_notes = ch.consume_notifications("Agent-Bob");
    REQUIRE(bob_notes.size() == 1);
    CHECK(bob_notes[0].from == "user");
    CHECK(bob_notes[0].message == "@everyone say hi");

    // Sender ("user") is not an agent, so no self-notification issue
}

TEST_CASE("GroupChannel @everyone excludes sender", "[group_channel]") {
    GroupChannel ch;

    ch.register_agent(1, "Agent-Alice");
    ch.register_agent(2, "Agent-Bob");

    // Alice posts @everyone
    ch.post_message("Agent-Alice", "@everyone check this");

    auto alice_notes = ch.consume_notifications("Agent-Alice");
    CHECK(alice_notes.empty()); // Alice should not get notified

    auto bob_notes = ch.consume_notifications("Agent-Bob");
    REQUIRE(bob_notes.size() == 1);
    CHECK(bob_notes[0].message == "@everyone check this");
}

// ===================================================================
// GroupChannel: @all alias for @everyone
// ===================================================================

TEST_CASE("GroupChannel @all notifies all agents like @everyone", "[group_channel]") {
    GroupChannel ch;

    ch.register_agent(1, "Agent-Alice");
    ch.register_agent(2, "Agent-Bob");
    ch.register_agent(3, "Agent-Charlie");

    ch.post_message("user", "@all team meeting");

    auto alice_notes = ch.consume_notifications("Agent-Alice");
    REQUIRE(alice_notes.size() == 1);
    CHECK(alice_notes[0].message == "@all team meeting");

    auto bob_notes = ch.consume_notifications("Agent-Bob");
    REQUIRE(bob_notes.size() == 1);

    auto charlie_notes = ch.consume_notifications("Agent-Charlie");
    REQUIRE(charlie_notes.size() == 1);
}

TEST_CASE("GroupChannel @all excludes sender", "[group_channel]") {
    GroupChannel ch;

    ch.register_agent(1, "Agent-Alice");
    ch.register_agent(2, "Agent-Bob");

    ch.post_message("Agent-Alice", "@all hello");

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

    ch.post_message("user", "@Agent-Bob help needed");

    auto alice_notes = ch.consume_notifications("Agent-Alice");
    CHECK(alice_notes.empty());

    auto bob_notes = ch.consume_notifications("Agent-Bob");
    REQUIRE(bob_notes.size() == 1);
    CHECK(bob_notes[0].message == "@Agent-Bob help needed");
}

TEST_CASE("GroupChannel @mention case insensitive", "[group_channel]") {
    GroupChannel ch;

    ch.register_agent(1, "Agent-Alice");
    ch.register_agent(2, "Agent-Bob");

    ch.post_message("user", "@agent-alice hi");

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
    ch.post_message("user", "@Very are you there?");

    auto notes = ch.consume_notifications("Very-Little-Gravitas-Indeed");
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].message == "@Very are you there?");
}

TEST_CASE("GroupChannel @mention prefix matching with hyphen", "[group_channel]") {
    GroupChannel ch;

    ch.register_agent(1, "A-Fine-Example");

    // Prefix "Fine" (with "-Fine" in name) should match
    ch.post_message("user", "@Fine test");

    auto notes = ch.consume_notifications("A-Fine-Example");
    REQUIRE(notes.size() == 1);
}

// ===================================================================
// GroupChannel: no notification for unmatched tags
// ===================================================================

TEST_CASE("GroupChannel unmatched @mention does not notify", "[group_channel]") {
    GroupChannel ch;

    ch.register_agent(1, "Agent-Alice");

    ch.post_message("user", "@nonexistent hello");

    auto alice_notes = ch.consume_notifications("Agent-Alice");
    CHECK(alice_notes.empty());
}

TEST_CASE("GroupChannel message with no @ does not notify", "[group_channel]") {
    GroupChannel ch;

    ch.register_agent(1, "Agent-Alice");

    ch.post_message("user", "just a normal message");

    auto alice_notes = ch.consume_notifications("Agent-Alice");
    CHECK(alice_notes.empty());
}

// ===================================================================
// GroupChannel: @user tag does not notify agents
// ===================================================================

TEST_CASE("GroupChannel @user tag does not notify agents", "[group_channel]") {
    GroupChannel ch;

    ch.register_agent(1, "Agent-Alice");

    ch.post_message("Agent-Alice", "@user what do you think?");

    auto alice_notes = ch.consume_notifications("Agent-Alice");
    CHECK(alice_notes.empty()); // @user should not trigger agent notification
}

// ===================================================================
// GroupChannel: pending notification body content
// ===================================================================

TEST_CASE("GroupChannel notification includes body", "[group_channel]") {
    GroupChannel ch;

    ch.register_agent(1, "Agent-Alice");

    ch.post_message("user", "@everyone This is the message body with details.");

    auto notes = ch.consume_notifications("Agent-Alice");
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].message == "@everyone This is the message body with details.");
    CHECK(notes[0].from == "user");
}

// ===================================================================
// GroupChannel: consume_notifications clears the queue
// ===================================================================

TEST_CASE("GroupChannel consume_notifications clears queue", "[group_channel]") {
    GroupChannel ch;

    ch.register_agent(1, "Agent-Alice");

    ch.post_message("user", "@everyone first");
    auto notes1 = ch.consume_notifications("Agent-Alice");
    REQUIRE(notes1.size() == 1);

    // Second consume should return empty
    auto notes2 = ch.consume_notifications("Agent-Alice");
    CHECK(notes2.empty());
}

// ===================================================================
// GroupChannel: tags are present
// ===================================================================

TEST_CASE("GroupChannel tags include agent names for @everyone", "[group_channel]") {
    GroupChannel ch;

    ch.register_agent(1, "Agent-Alice");
    ch.register_agent(2, "Agent-Bob");

    ch.post_message("user", "@everyone hi all");

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
    ch.post_message("user", "Can Agent-Alice help with this?");

    auto alice_notes = ch.consume_notifications("Agent-Alice");
    REQUIRE(alice_notes.size() == 1);

    auto bob_notes = ch.consume_notifications("Agent-Bob");
    CHECK(bob_notes.empty());
}

TEST_CASE("GroupChannel partial word does not trigger notification without @", "[group_channel]") {
    GroupChannel ch;

    ch.register_agent(1, "Agent-Alice");

    // "Alice" is part of "Agent-Alice" but not a whole word match
    ch.post_message("user", "Alice is here");

    auto notes = ch.consume_notifications("Agent-Alice");
    CHECK(notes.empty());
}

// ===================================================================
// GroupChannel: read_new_messages
// ===================================================================

TEST_CASE("read_new_messages returns all messages on first call", "[group_channel]") {
    GroupChannel ch;

    ch.register_agent(1, "Agent-Alice");
    ch.post_message("user", "first");
    ch.post_message("user", "second");

    auto msgs = ch.read_new_messages("Agent-Alice");
    REQUIRE(msgs.size() == 2);
    CHECK(msgs[0].message == "first");
    CHECK(msgs[1].message == "second");
}

TEST_CASE("read_new_messages returns only new messages on subsequent calls", "[group_channel]") {
    GroupChannel ch;

    ch.register_agent(1, "Agent-Alice");
    ch.post_message("user", "first");
    ch.post_message("user", "second");

    // Read initial messages
    auto first_batch = ch.read_new_messages("Agent-Alice");
    REQUIRE(first_batch.size() == 2);

    // Post more messages
    ch.post_message("user", "third");
    ch.post_message("user", "fourth");

    // Second read should only return new messages
    auto second_batch = ch.read_new_messages("Agent-Alice");
    REQUIRE(second_batch.size() == 2);
    CHECK(second_batch[0].message == "third");
    CHECK(second_batch[1].message == "fourth");

    // No more new messages
    auto third_batch = ch.read_new_messages("Agent-Alice");
    CHECK(third_batch.empty());
}

TEST_CASE("read_new_messages is per-agent", "[group_channel]") {
    GroupChannel ch;

    ch.register_agent(1, "Agent-Alice");
    ch.register_agent(2, "Agent-Bob");

    ch.post_message("user", "hello");

    // Alice reads
    auto alice_msgs = ch.read_new_messages("Agent-Alice");
    REQUIRE(alice_msgs.size() == 1);

    // Bob should also see the message (independent cursor)
    auto bob_msgs = ch.read_new_messages("Agent-Bob");
    REQUIRE(bob_msgs.size() == 1);

    // Both read again — nothing new
    CHECK(ch.read_new_messages("Agent-Alice").empty());
    CHECK(ch.read_new_messages("Agent-Bob").empty());
}

TEST_CASE("read_new_messages skips messages that arrived before registration", "[group_channel]") {
    GroupChannel ch;

    ch.post_message("user", "old message");

    ch.register_agent(1, "Agent-Alice");

    // New agent should still see the old message (cursor starts at 0)
    auto msgs = ch.read_new_messages("Agent-Alice");
    REQUIRE(msgs.size() == 1);
    CHECK(msgs[0].message == "old message");
}
