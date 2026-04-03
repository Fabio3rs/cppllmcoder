#include "tool_registry.hpp"
#include <gtest/gtest.h>

class DummyTool : public ITool {
  public:
    explicit DummyTool(std::string name, bool always = false)
        : name_(std::move(name)), always_(always) {}

    ToolMetadata describe() const override {
        return ToolMetadata{
            .name = name_,
            .description = "dummy",
            .arguments = {},
            .usage_example = "",
            .returns = "",
            .danger_tags = {},
            .is_sensitive = false,
            .always_show_in_prompt = always_,
        };
    }

    std::expected<sol::object, std::string>
    invoke(sol::variadic_args va, sol::this_state s) const override {
        sol::state_view lua(s);
        (void)va;
        return sol::make_object(lua, "ok");
    }

  private:
    std::string name_;
    bool always_;
};

TEST(DefaultToolRegistry, AlwaysShowRespectedBeyondK) {
    DefaultToolRegistry reg;
    reg.registerTool(std::make_shared<DummyTool>("a_always", true));
    reg.registerTool(std::make_shared<DummyTool>("b_normal", false));
    reg.registerTool(std::make_shared<DummyTool>("c_normal", false));

    const auto docs = reg.topKDocs("b", 1); // K=1 but always tool must appear

    ASSERT_EQ(docs.size(), 2u);
    EXPECT_EQ(docs[0].name, "a_always");
    EXPECT_TRUE(docs[0].always);
    EXPECT_EQ(docs[1].name, "b_normal");
}

TEST(DefaultToolRegistry, SensitiveFlagPropagatesToDocView) {
    class SensitiveTool : public DummyTool {
      public:
        using DummyTool::DummyTool;
        ToolMetadata describe() const override {
            auto meta = DummyTool::describe();
            meta.is_sensitive = true;
            return meta;
        }
    };

    DefaultToolRegistry reg;
    reg.registerTool(std::make_shared<SensitiveTool>("secret", false));

    const auto docs = reg.topKDocs("", 5);
    ASSERT_EQ(docs.size(), 1u);
    EXPECT_TRUE(docs[0].sensitive);
    EXPECT_EQ(docs[0].name, "secret");
}
