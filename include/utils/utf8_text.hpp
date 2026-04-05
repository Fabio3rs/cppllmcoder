#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace utf8 {

[[nodiscard]] std::size_t align_offset(std::string_view utf8str,
                                       std::size_t offset_bytes) noexcept;

[[nodiscard]] std::string_view prefix_by_bytes(std::string_view utf8str,
                                               std::size_t max_bytes) noexcept;

[[nodiscard]] std::string_view slice_by_bytes(std::string_view utf8str,
                                              std::size_t offset_bytes,
                                              std::size_t max_bytes) noexcept;

[[nodiscard]] std::string truncate_for_display(std::string_view utf8str,
                                               std::size_t max_bytes,
                                               std::string_view suffix = "...");

} // namespace utf8
