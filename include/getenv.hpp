#pragma once

#include <string_view>

inline std::string_view getenv_var(std::string_view name) {
    if (const char *value = std::getenv(name.data())) {
        return value;
    }
    return {};
}
