#pragma once

#include <cstdint>
#include <functional>

namespace heartstead {

using BlockId = std::uint16_t;
inline constexpr BlockId air_block = 0;

struct Int3 {
    std::int32_t x{};
    std::int32_t y{};
    std::int32_t z{};

    friend constexpr bool operator==(const Int3&, const Int3&) = default;
};

[[nodiscard]] constexpr std::int32_t floor_div(std::int32_t value, std::int32_t divisor) noexcept {
    const auto quotient = value / divisor;
    const auto remainder = value % divisor;
    return quotient - static_cast<std::int32_t>(remainder != 0 && ((remainder < 0) != (divisor < 0)));
}

[[nodiscard]] constexpr std::int32_t floor_mod(std::int32_t value, std::int32_t divisor) noexcept {
    const auto remainder = value % divisor;
    return remainder + static_cast<std::int32_t>(remainder != 0 && ((remainder < 0) != (divisor < 0))) * divisor;
}

} // namespace heartstead

template <>
struct std::hash<heartstead::Int3> {
    [[nodiscard]] std::size_t operator()(const heartstead::Int3& value) const noexcept {
        auto hash = static_cast<std::uint64_t>(static_cast<std::uint32_t>(value.x));
        hash ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(value.y)) + 0x9e3779b97f4a7c15ULL +
                (hash << 6U) + (hash >> 2U);
        hash ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(value.z)) + 0x9e3779b97f4a7c15ULL +
                (hash << 6U) + (hash >> 2U);
        return static_cast<std::size_t>(hash);
    }
};

