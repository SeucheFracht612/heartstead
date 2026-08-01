#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace heartstead::core {

[[nodiscard]] constexpr std::string_view trim_ascii_whitespace(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' ||
                              value.front() == '\r' || value.front() == '\n')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
                              value.back() == '\r' || value.back() == '\n')) {
        value.remove_suffix(1);
    }
    return value;
}

[[nodiscard]] constexpr bool is_valid_utf8_text(std::string_view value,
                                                std::size_t maximum_bytes,
                                                std::string_view forbidden_ascii = {}) noexcept {
    if (value.empty() || value.size() > maximum_bytes) {
        return false;
    }
    std::size_t index = 0;
    while (index < value.size()) {
        const auto first = static_cast<std::uint8_t>(value[index]);
        if (first < 0x80U) {
            if (first < 0x20U || first == 0x7fU ||
                forbidden_ascii.find(static_cast<char>(first)) != std::string_view::npos) {
                return false;
            }
            ++index;
            continue;
        }
        std::size_t continuation_count = 0;
        std::uint32_t codepoint = 0;
        if (first >= 0xc2U && first <= 0xdfU) {
            continuation_count = 1;
            codepoint = first & 0x1fU;
        } else if (first >= 0xe0U && first <= 0xefU) {
            continuation_count = 2;
            codepoint = first & 0x0fU;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            continuation_count = 3;
            codepoint = first & 0x07U;
        } else {
            return false;
        }
        if (index + continuation_count >= value.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset <= continuation_count; ++offset) {
            const auto next = static_cast<std::uint8_t>(value[index + offset]);
            if ((next & 0xc0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (next & 0x3fU);
        }
        const auto overlong = (continuation_count == 1 && codepoint < 0x80U) ||
                              (continuation_count == 2 && codepoint < 0x800U) ||
                              (continuation_count == 3 && codepoint < 0x10000U);
        if (overlong || codepoint > 0x10ffffU ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU) ||
            (codepoint >= 0x80U && codepoint <= 0x9fU)) {
            return false;
        }
        index += continuation_count + 1;
    }
    return true;
}

} // namespace heartstead::core
