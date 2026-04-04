#include "tool_registry.hpp"

#include <unordered_set>

void DefaultToolRegistry::registerTool(std::shared_ptr<ITool> tool) {
    if (!tool) {
        return;
    }
    tools_[tool->describe().name] = std::move(tool);
}

namespace {

std::string trim_copy(std::string_view in) {
    size_t start = 0;
    size_t end = in.size();
    while (start < end && std::isspace(static_cast<unsigned char>(in[start]))) {
        ++start;
    }
    while (end > start &&
           std::isspace(static_cast<unsigned char>(in[end - 1]))) {
        --end;
    }
    return std::string(in.substr(start, end - start));
}

std::vector<std::string>
normalize_tags(const std::vector<std::string> &raw_tags) {
    auto to_lower_local = [](std::string_view in) {
        std::string out;
        out.reserve(in.size());
        for (char c : in) {
            out.push_back(
                static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        return out;
    };

    std::vector<std::string> out;
    out.reserve(raw_tags.size());
    std::unordered_set<std::string> seen;

    for (const auto &tag : raw_tags) {
        auto trimmed = trim_copy(tag);
        if (trimmed.empty()) {
            continue;
        }
        auto lowered = to_lower_local(trimmed);
        if (seen.insert(lowered).second) {
            out.push_back(std::move(lowered));
        }
    }
    return out;
}

} // namespace

std::shared_ptr<ITool>
DefaultToolRegistry::findTool(std::string_view name) const {
    if (auto it = tools_.find(std::string{name}); it != tools_.end()) {
        return it->second;
    }
    return nullptr;
}

std::vector<ToolMetadata> DefaultToolRegistry::listMetadata() const {
    std::vector<ToolMetadata> metas;
    metas.reserve(tools_.size());
    for (const auto &kv : tools_) {
        auto meta = kv.second->describe();
        meta.tags = normalize_tags(meta.tags);
        metas.push_back(std::move(meta));
    }
    return metas;
}

void DefaultToolRegistry::forEach(
    const std::function<void(const ToolMetadata &, const ITool &)> &fn) const {
    for (const auto &kv : tools_) {
        const auto &tool = kv.second;
        auto meta = tool->describe();
        meta.tags = normalize_tags(meta.tags);
        fn(meta, *tool);
    }
}

std::vector<ToolDocView>
DefaultToolRegistry::topKDocs(std::string_view user_input, size_t k) const {
    const std::string query = to_lower(user_input);

    std::vector<std::pair<int, ToolDocView>> scored;
    scored.reserve(tools_.size());

    for (const auto &kv : tools_) {
        auto meta = kv.second->describe();
        meta.tags = normalize_tags(meta.tags);
        ToolDocView view{meta.name, build_signature(meta), build_brief(meta),
                         meta.is_sensitive, meta.always_show_in_prompt};

        int score = 0;
        const std::string name_l = to_lower(meta.name);
        const std::string desc_l = to_lower(meta.description);
        if (!query.empty()) {
            if (name_l.find(query) != std::string::npos) {
                score += 2;
            }
            if (desc_l.find(query) != std::string::npos) {
                score += 1;
            }
            for (const auto &arg : meta.arguments) {
                const auto arg_l = to_lower(arg.name);
                if (arg_l.find(query) != std::string::npos) {
                    score += 1;
                }
            }
            for (const auto &tag : meta.tags) {
                if (tag.find(query) != std::string::npos) {
                    score += 2;
                    break;
                }
            }
        }

        if (meta.always_show_in_prompt) {
            score = std::max(score, 1000); // bubble to front
        }

        scored.emplace_back(score, view);
    }

    // Sort by score desc, then name asc for stability
    std::ranges::sort(scored, [](const auto &a, const auto &b) {
        if (a.first != b.first) {
            return a.first > b.first;
        }
        return a.second.name < b.second.name;
    });

    std::vector<ToolDocView> out;
    out.reserve(scored.size());

    size_t always_count = 0;
    for (const auto &pair : scored) {
        if (pair.second.always) {
            out.push_back(pair.second);
            always_count++;
        }
    }

    for (const auto &pair : scored) {
        if (pair.second.always) {
            continue;
        }
        if (out.size() >= always_count + k) {
            break;
        }
        out.push_back(pair.second);
    }

    return out;
}

std::string DefaultToolRegistry::to_lower(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in)
        out.push_back(
            static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

std::string DefaultToolRegistry::build_signature(const ToolMetadata &meta) {
    std::string sig = meta.name + "(";
    bool first = true;
    for (const auto &arg : meta.arguments) {
        if (!first) {
            sig += ", ";
        }
        first = false;
        sig += arg.name;
        sig += ":" + arg.type;
        if (!arg.required) {
            sig += "?";
        }
    }
    sig += ")";
    return sig;
}

std::string DefaultToolRegistry::build_brief(const ToolMetadata &meta) {
    if (!meta.description.empty())
        return meta.description;
    return "No description";
}
