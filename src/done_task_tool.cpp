#include "done_task_tool.hpp"

#include <sol/sol.hpp>
#include <string>

namespace {

class DoneTaskTool final : public ITool {
  public:
    explicit DoneTaskTool(std::shared_ptr<DoneTaskSignal> signal)
        : signal_(std::move(signal)) {}

    ToolMetadata describe() const override {
        return ToolMetadata{
            .name = "done_task",
            .description =
                "I finished the task asked, wait for user new message",
            .arguments = {},
            .usage_example = "tools.done_task()",
            .returns = "string confirmation",
            .danger_tags = {},
            .is_sensitive = false,
            .always_show_in_prompt = true};
    }

    std::expected<sol::object, std::string>
    invoke(const sol::object &lua_args) const override {
        (void)lua_args;
        if (signal_) {
            signal_->mark_done();
        }
        sol::state_view lua(lua_args.lua_state());
        return sol::make_object(lua, std::string("task marked as done"));
    }

  private:
    std::shared_ptr<DoneTaskSignal> signal_;
};
} // namespace

std::shared_ptr<DoneTaskSignal>
registerDoneTaskTool(ToolRegistry &registry,
                     std::shared_ptr<DoneTaskSignal> signal) {
    if (!signal) {
        signal = std::make_shared<DoneTaskSignal>();
    }
    registry.registerTool(std::make_shared<DoneTaskTool>(signal));
    return signal;
}
