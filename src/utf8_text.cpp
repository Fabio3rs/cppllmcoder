#include "utils/utf8_text.hpp"

#include "utils/simple_utf8view.hpp"

extern "C" {
#include <utf8proc.h>
}

namespace {

bool is_grapheme_break(utf8proc_int32_t previous, utf8proc_int32_t current,
                       utf8proc_int32_t &state) noexcept {
    if (utf8proc_grapheme_break_stateful(previous, current, &state) != 0) {
        state = 0;
        return true;
    }
    return false;
}

std::size_t next_grapheme_boundary(std::string_view utf8str,
                                   std::size_t offset_bytes) noexcept {
    if (offset_bytes == 0) {
        return 0;
    }
    if (offset_bytes >= utf8str.size()) {
        return utf8str.size();
    }

    const char *base = utf8str.data();
    const char *end = base + utf8str.size();
    const char *ptr = base;
    utf8proc_int32_t previous = 0;
    utf8proc_int32_t state = 0;
    bool have_previous = false;

    while (ptr < end) {
        const auto [codepoint, size] = utf8::decode(ptr, end);
        if (have_previous &&
            is_grapheme_break(
                previous, static_cast<utf8proc_int32_t>(codepoint), state)) {
            const auto boundary = static_cast<std::size_t>(ptr - base);
            if (offset_bytes <= boundary) {
                return boundary;
            }
        }
        previous = static_cast<utf8proc_int32_t>(codepoint);
        ptr += size;
        have_previous = true;
    }

    return utf8str.size();
}

std::size_t last_boundary_within_budget(std::string_view utf8str,
                                        std::size_t max_bytes) noexcept {
    if (max_bytes >= utf8str.size()) {
        return utf8str.size();
    }

    const char *base = utf8str.data();
    const char *end = base + utf8str.size();
    const char *ptr = base;
    const char *last_boundary = base;
    utf8proc_int32_t previous = 0;
    utf8proc_int32_t state = 0;
    bool have_previous = false;

    while (ptr < end) {
        const auto [codepoint, size] = utf8::decode(ptr, end);
        if (have_previous &&
            is_grapheme_break(
                previous, static_cast<utf8proc_int32_t>(codepoint), state)) {
            if (static_cast<std::size_t>(ptr - base) > max_bytes) {
                return static_cast<std::size_t>(last_boundary - base);
            }
            last_boundary = ptr;
        }
        previous = static_cast<utf8proc_int32_t>(codepoint);
        ptr += size;
        have_previous = true;
    }

    return static_cast<std::size_t>(last_boundary - base);
}

} // namespace

namespace utf8 {

std::size_t align_offset(std::string_view utf8str,
                         std::size_t offset_bytes) noexcept {
    return next_grapheme_boundary(utf8str, offset_bytes);
}

std::string_view prefix_by_bytes(std::string_view utf8str,
                                 std::size_t max_bytes) noexcept {
    return utf8str.substr(0, last_boundary_within_budget(utf8str, max_bytes));
}

std::string_view slice_by_bytes(std::string_view utf8str,
                                std::size_t offset_bytes,
                                std::size_t max_bytes) noexcept {
    if (max_bytes == 0 || offset_bytes >= utf8str.size()) {
        return {};
    }

    const std::size_t start = align_offset(utf8str, offset_bytes);
    if (start >= utf8str.size()) {
        return {};
    }

    return prefix_by_bytes(utf8str.substr(start), max_bytes);
}

std::string truncate_for_display(std::string_view utf8str,
                                 std::size_t max_bytes,
                                 std::string_view suffix) {
    const auto prefix = prefix_by_bytes(utf8str, max_bytes);
    std::string out(prefix);
    if (prefix.size() < utf8str.size()) {
        out.append(suffix);
    }
    return out;
}

} // namespace utf8
