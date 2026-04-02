#pragma once

#include "agent.hpp"
#include <algorithm>
#include <cctype>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Concrete registry used by the Agent/LuaContext wiring
class DefaultToolRegistry : public ToolRegistry {
  public:
    void registerTool(std::shared_ptr<ITool> tool) override;
    std::shared_ptr<ITool> findTool(std::string_view name) const override;
    std::vector<ToolMetadata> listMetadata() const override;
    void forEach(const std::function<void(const ToolMetadata &, const ITool &)>
                     &fn) const override;
    std::vector<ToolDocView> topKDocs(std::string_view user_input,
                                      size_t k) const override;

  private:
    std::unordered_map<std::string, std::shared_ptr<ITool>> tools_;

    static std::string to_lower(std::string_view in);
    static std::string build_signature(const ToolMetadata &meta);
    static std::string build_brief(const ToolMetadata &meta);
};
