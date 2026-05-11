#pragma once

#include <cstddef>
#include <string>
#include <vector>

enum class ChatRole {
    User,
    Assistant,
    System,
    Tool,
};

struct ChatItem {
    ChatRole role;
    std::string title;
    std::string text;
    std::string agent_id;
    bool collapsible = false;
    bool expanded = false;
    int preview_len = 200;
};

inline int
findLatestConversationIndexForAgent(const std::vector<ChatItem> &conversation,
                                    const std::string &agent_id) {
    for (int i = static_cast<int>(conversation.size()) - 1; i >= 0; --i) {
        const auto &item = conversation[static_cast<size_t>(i)];
        if (item.role == ChatRole::Assistant && item.agent_id == agent_id) {
            return i;
        }
    }
    return -1;
}

inline bool clearRetryTarget(std::vector<ChatItem> &conversation,
                             const std::string &agent_id,
                             int *selected_chat = nullptr) {
    const int index =
        findLatestConversationIndexForAgent(conversation, agent_id);
    if (index < 0) {
        return false;
    }
    conversation[static_cast<size_t>(index)].text.clear();
    if (selected_chat) {
        *selected_chat = index;
    }
    return true;
}
