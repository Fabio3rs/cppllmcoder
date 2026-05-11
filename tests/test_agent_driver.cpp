#include <gtest/gtest.h>

#include "agent_driver.hpp"

#include <memory>
#include <string>
#include <vector>

TEST(SubAgentDriver, ForwardsCompletionFeedbackToParentAndBus) {
    auto bus = std::make_shared<AgentEventBus>();
    std::vector<std::string> injected;

    SubAgentDriver::Config cfg;
    cfg.parent_id = "parent-1";
    cfg.parent_inject = [&](std::string_view text) {
        injected.emplace_back(text);
    };
    cfg.feedback_formatter = [](std::string_view response) {
        return std::string{"<subagent-result>"} + std::string(response) +
               "</subagent-result>";
    };

    SubAgentDriver driver(bus, "child-1", cfg);
    driver.on_turn_complete("done");

    ASSERT_EQ(injected.size(), 1u);
    EXPECT_EQ(injected[0], "<subagent-result>done</subagent-result>");

    const auto events = bus->drain();
    ASSERT_EQ(events.size(), 2u);
    EXPECT_TRUE(std::holds_alternative<EvTurnComplete>(events[0]));
    EXPECT_TRUE(std::holds_alternative<EvSubAgentFeedback>(events[1]));

    const auto &feedback = std::get<EvSubAgentFeedback>(events[1]);
    EXPECT_EQ(feedback.agent_id, "child-1");
    EXPECT_EQ(feedback.parent_id, "parent-1");
    EXPECT_EQ(feedback.text, "<subagent-result>done</subagent-result>");
    EXPECT_TRUE(feedback.inject_parent);
}

TEST(SubAgentDriver, QueuesInjectedMessagesUntilNextTurn) {
    auto bus = std::make_shared<AgentEventBus>();
    SubAgentDriver::Config cfg;
    cfg.parent_id = "parent-1";

    SubAgentDriver driver(bus, "child-1", cfg);
    driver.inject("ping");

    auto injection = driver.next_injection();
    ASSERT_TRUE(injection.has_value());
    EXPECT_EQ(*injection, "ping");
    EXPECT_FALSE(driver.next_injection().has_value());
}

TEST(AgentManager, SpawnsSubagentWithFeedbackEnabled) {
    auto bus = std::make_shared<AgentEventBus>();
    AgentManager manager(bus);

    std::vector<std::string> parent_injections;
    SubAgentDriver::Config cfg;
    cfg.parent_id = "parent-7";
    cfg.parent_inject = [&](std::string_view text) {
        parent_injections.emplace_back(text);
    };
    cfg.feedback_formatter = [](std::string_view response) {
        return std::string{"result:"} + std::string(response);
    };

    auto handle = manager.spawn_subagent(
        "child-7", cfg,
        [](IAgentDriver &driver) -> std::string {
            driver.on_turn_complete("ok");
            return "ok";
        },
        "parent-7");

    const std::string result = handle.collect();
    EXPECT_EQ(result, "ok");
    ASSERT_EQ(parent_injections.size(), 1u);
    EXPECT_EQ(parent_injections[0], "result:ok");

    const auto status = manager.status("child-7");
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->parent_id.value_or(""), "parent-7");
}
