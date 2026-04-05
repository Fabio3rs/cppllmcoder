#pragma once
#include "utils/Strutils.hpp"
#include <string>
#include <string_view>

struct AgentAction {
    std::string thought;
    std::string lua_code;
    bool has_code = false;

    static AgentAction parse(std::string_view raw) {
        AgentAction action;
        const std::string_view s_tag = "<code>";
        const std::string_view e_tag = "</code>";

        const auto start = raw.find(s_tag);
        const auto end = start == std::string_view::npos
                             ? std::string_view::npos
                             : raw.find(e_tag, start + s_tag.size());

        if (start != std::string_view::npos && end != std::string_view::npos &&
            end > start) {
            std::string_view thought_view = raw.substr(0, start);
            std::string_view code_inner =
                raw.substr(start + s_tag.size(), end - (start + s_tag.size()));

            // Limpeza de Markdown (```lua ... ```)
            if (const auto md_start = code_inner.find("```lua");
                md_start != std::string_view::npos) {
                code_inner = code_inner.substr(md_start + 6);
                if (const auto md_end = code_inner.rfind("```");
                    md_end != std::string_view::npos) {
                    code_inner = code_inner.substr(0, md_end);
                }
            }

            action.thought.assign(thought_view);
            action.lua_code.assign(Strutils::trim(code_inner));
            action.has_code = action.lua_code.size() > 0;
        } else {
            action.thought = std::string(raw);
        }
        return action;
    }
};
