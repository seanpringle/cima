#include "inbox.h"

#include <catch2/catch_test_macros.hpp>
#include <thread>

// ===================================================================
// Inbox: send_message / next_message basics
// ===================================================================

TEST_CASE("Inbox send_message delivers to registered agent", "[inbox]") {
    Inbox inbox;

    inbox.register_agent("Agent-Alice");
    inbox.register_agent("Agent-Bob");

    auto result = inbox.send_message("Agent-Alice", "Agent-Bob", "Hello Bob!");
    REQUIRE(result.has_value());
    CHECK(*result == "delivered");

    auto msg = inbox.next_message("Agent-Bob");
    REQUIRE(msg.has_value());
    CHECK(msg->from == "Agent-Alice");
    CHECK(msg->message == "Hello Bob!");
}

TEST_CASE("Inbox send_message fails for unknown recipient", "[inbox]") {
    Inbox inbox;

    inbox.register_agent("Agent-Alice");

    auto result = inbox.send_message("Agent-Alice", "Nonexistent", "Hello?");
    REQUIRE(!result.has_value());
    CHECK(result.error() == "no such recipient");
}

TEST_CASE("Inbox send_message with empty recipient fails", "[inbox]") {
    Inbox inbox;

    inbox.register_agent("Agent-Alice");

    // The Inbox class itself doesn't validate empty — it checks lookups.
    // Empty string won't match any registered agent.
    auto result = inbox.send_message("Agent-Alice", "", "Hello?");
    REQUIRE(!result.has_value());
    CHECK(result.error() == "no such recipient");
}

// ===================================================================
// Inbox: FIFO ordering
// ===================================================================

TEST_CASE("Inbox messages are delivered in FIFO order", "[inbox]") {
    Inbox inbox;

    inbox.register_agent("Agent-Alice");
    inbox.register_agent("Agent-Bob");

    inbox.send_message("Agent-Alice", "Agent-Bob", "first");
    inbox.send_message("Agent-Alice", "Agent-Bob", "second");
    inbox.send_message("Agent-Alice", "Agent-Bob", "third");

    auto m1 = inbox.next_message("Agent-Bob");
    REQUIRE(m1.has_value());
    CHECK(m1->message == "first");

    auto m2 = inbox.next_message("Agent-Bob");
    REQUIRE(m2.has_value());
    CHECK(m2->message == "second");

    auto m3 = inbox.next_message("Agent-Bob");
    REQUIRE(m3.has_value());
    CHECK(m3->message == "third");
}

TEST_CASE("Inbox next_message returns nullopt when queue empty", "[inbox]") {
    Inbox inbox;

    inbox.register_agent("Agent-Alice");

    auto msg = inbox.next_message("Agent-Alice");
    CHECK(!msg.has_value());
}

TEST_CASE("Inbox next_message returns nullopt after consuming all messages", "[inbox]") {
    Inbox inbox;

    inbox.register_agent("Agent-Alice");
    inbox.send_message("Agent-Bob", "Agent-Alice", "hello");

    auto m1 = inbox.next_message("Agent-Alice");
    REQUIRE(m1.has_value());

    auto m2 = inbox.next_message("Agent-Alice");
    CHECK(!m2.has_value());
}

// ===================================================================
// Inbox: multiple agents, independent queues
// ===================================================================

TEST_CASE("Inbox agents have independent queues", "[inbox]") {
    Inbox inbox;

    inbox.register_agent("Agent-Alice");
    inbox.register_agent("Agent-Bob");

    inbox.send_message("user", "Agent-Alice", "for Alice");
    inbox.send_message("user", "Agent-Bob", "for Bob");

    // Alice should only see her message
    auto alice_msg = inbox.next_message("Agent-Alice");
    REQUIRE(alice_msg.has_value());
    CHECK(alice_msg->message == "for Alice");

    // Bob should only see his message
    auto bob_msg = inbox.next_message("Agent-Bob");
    REQUIRE(bob_msg.has_value());
    CHECK(bob_msg->message == "for Bob");

    // Both queues should now be empty
    CHECK(!inbox.next_message("Agent-Alice").has_value());
    CHECK(!inbox.next_message("Agent-Bob").has_value());
}

// ===================================================================
// Inbox: unregister agent
// ===================================================================

TEST_CASE("Inbox unregister_agent discards messages", "[inbox]") {
    Inbox inbox;

    inbox.register_agent("Agent-Alice");
    inbox.send_message("user", "Agent-Alice", "pending message");

    inbox.unregister_agent("Agent-Alice");

    // Agent no longer registered
    auto result = inbox.send_message("user", "Agent-Alice", "hello");
    CHECK(!result.has_value());
    CHECK(result.error() == "no such recipient");

    // Re-register — should start with empty queue
    inbox.register_agent("Agent-Alice");
    CHECK(!inbox.next_message("Agent-Alice").has_value());
}

TEST_CASE("Inbox unregister_agent removes from agent list", "[inbox]") {
    Inbox inbox;

    inbox.register_agent("Agent-Alice");
    CHECK(inbox.is_registered("Agent-Alice"));

    inbox.unregister_agent("Agent-Alice");
    CHECK(!inbox.is_registered("Agent-Alice"));
}

// ===================================================================
// Inbox: all_agent_names and is_registered
// ===================================================================

TEST_CASE("Inbox all_agent_names returns registered names", "[inbox]") {
    Inbox inbox;

    CHECK(inbox.all_agent_names().empty());

    inbox.register_agent("Agent-Alice");
    inbox.register_agent("Agent-Bob");

    auto names = inbox.all_agent_names();
    CHECK(names.size() == 2);
    CHECK(std::find(names.begin(), names.end(), "Agent-Alice") != names.end());
    CHECK(std::find(names.begin(), names.end(), "Agent-Bob") != names.end());
}

TEST_CASE("Inbox is_registered works correctly", "[inbox]") {
    Inbox inbox;

    CHECK(!inbox.is_registered("Agent-Alice"));

    inbox.register_agent("Agent-Alice");
    CHECK(inbox.is_registered("Agent-Alice"));

    CHECK(!inbox.is_registered("Agent-Bob"));
}

// ===================================================================
// Inbox: duplicate registration
// ===================================================================

TEST_CASE("Inbox duplicate registration returns false", "[inbox]") {
    Inbox inbox;

    CHECK(inbox.register_agent("Agent-Alice"));
    CHECK(!inbox.register_agent("Agent-Alice")); // duplicate
}

// ===================================================================
// Inbox: send_message from unregistered sender still works
// ===================================================================

TEST_CASE("Inbox send_message works from any sender string", "[inbox]") {
    Inbox inbox;

    inbox.register_agent("Agent-Bob");

    // Sender doesn't need to be registered
    auto result = inbox.send_message("some-rando", "Agent-Bob", "hello");
    REQUIRE(result.has_value());

    auto msg = inbox.next_message("Agent-Bob");
    REQUIRE(msg.has_value());
    CHECK(msg->from == "some-rando");
}

// ===================================================================
// Inbox: thread safety (basic)
// ===================================================================

TEST_CASE("Inbox concurrent send and receive", "[inbox]") {
    Inbox inbox;

    inbox.register_agent("Agent-Alice");
    inbox.register_agent("Agent-Bob");

    constexpr int N = 100;

    std::thread sender([&]() {
        for (int i = 0; i < N; i++) {
            auto r = inbox.send_message("Agent-Bob", "Agent-Alice", "msg " + std::to_string(i));
            REQUIRE(r.has_value());
        }
    });

    std::thread receiver([&]() {
        int received = 0;
        while (received < N) {
            auto msg = inbox.next_message("Agent-Alice");
            if (msg.has_value()) {
                received++;
            }
        }
    });

    sender.join();
    receiver.join();

    // Queue should be empty
    CHECK(!inbox.next_message("Agent-Alice").has_value());
}
