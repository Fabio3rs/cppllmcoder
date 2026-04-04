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
            .tags = {"agent", "control", "task", "completion"},
            .danger_tags = {},
            .is_sensitive = false,
            .always_show_in_prompt = true};
    }

    std::expected<sol::object, std::string>
    invoke(sol::variadic_args, sol::this_state s) const override {
        if (signal_) {
            signal_->mark_done();
        }
        sol::state_view lua(s);
        return sol::make_object(lua, std::string("task marked as done"));
    }

    auto get_signal() const -> std::shared_ptr<DoneTaskSignal> {
        return signal_;
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
