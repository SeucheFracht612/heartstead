#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace heartstead::net {

class BinaryMessageWriter final {
  public:
    void u8(std::uint8_t value) {
        bytes_.push_back(static_cast<char>(value));
    }
    void u16(std::uint16_t value) {
        unsigned_integer(value);
    }
    void u32(std::uint32_t value) {
        unsigned_integer(value);
    }
    void u64(std::uint64_t value) {
        unsigned_integer(value);
    }
    void i16(std::int16_t value) {
        u16(std::bit_cast<std::uint16_t>(value));
    }
    void i32(std::int32_t value) {
        u32(std::bit_cast<std::uint32_t>(value));
    }
    void i64(std::int64_t value) {
        u64(std::bit_cast<std::uint64_t>(value));
    }
    void f64(double value) {
        u64(std::bit_cast<std::uint64_t>(value));
    }
    void boolean(bool value) {
        u8(value ? 1U : 0U);
    }
    void var_u64(std::uint64_t value) {
        while (value >= 0x80U) {
            u8(static_cast<std::uint8_t>((value & 0x7fU) | 0x80U));
            value >>= 7U;
        }
        u8(static_cast<std::uint8_t>(value));
    }
    void var_i64(std::int64_t value) {
        const auto bits = std::bit_cast<std::uint64_t>(value);
        var_u64((bits << 1U) ^ (0U - (bits >> 63U)));
    }
    [[nodiscard]] bool text_u16(std::string_view value) {
        if (value.size() > std::numeric_limits<std::uint16_t>::max()) {
            return false;
        }
        u16(static_cast<std::uint16_t>(value.size()));
        bytes_.append(value);
        return true;
    }
    [[nodiscard]] std::string take() {
        return std::move(bytes_);
    }

  private:
    template <typename T>
        requires(std::is_unsigned_v<T>)
    void unsigned_integer(T value) {
        for (std::size_t index = 0; index < sizeof(T); ++index) {
            bytes_.push_back(static_cast<char>((value >> (index * 8U)) & 0xffU));
        }
    }

    std::string bytes_;
};

class BinaryMessageReader final {
  public:
    explicit BinaryMessageReader(std::string_view bytes) : bytes_(bytes) {}

    [[nodiscard]] bool u8(std::uint8_t& value) {
        return unsigned_integer(value);
    }
    [[nodiscard]] bool u16(std::uint16_t& value) {
        return unsigned_integer(value);
    }
    [[nodiscard]] bool u32(std::uint32_t& value) {
        return unsigned_integer(value);
    }
    [[nodiscard]] bool u64(std::uint64_t& value) {
        return unsigned_integer(value);
    }
    [[nodiscard]] bool i16(std::int16_t& value) {
        std::uint16_t bits = 0;
        if (!u16(bits)) {
            return false;
        }
        value = std::bit_cast<std::int16_t>(bits);
        return true;
    }
    [[nodiscard]] bool i32(std::int32_t& value) {
        std::uint32_t bits = 0;
        if (!u32(bits)) {
            return false;
        }
        value = std::bit_cast<std::int32_t>(bits);
        return true;
    }
    [[nodiscard]] bool i64(std::int64_t& value) {
        std::uint64_t bits = 0;
        if (!u64(bits)) {
            return false;
        }
        value = std::bit_cast<std::int64_t>(bits);
        return true;
    }
    [[nodiscard]] bool f64(double& value) {
        std::uint64_t bits = 0;
        if (!u64(bits)) {
            return false;
        }
        value = std::bit_cast<double>(bits);
        return true;
    }
    [[nodiscard]] bool boolean(bool& value) {
        std::uint8_t encoded = 0;
        if (!u8(encoded) || encoded > 1) {
            return false;
        }
        value = encoded != 0;
        return true;
    }
    [[nodiscard]] bool var_u64(std::uint64_t& value) {
        value = 0;
        for (unsigned index = 0; index < 10; ++index) {
            std::uint8_t byte = 0;
            if (!u8(byte) || (index == 9 && byte > 1U)) {
                return false;
            }
            value |= static_cast<std::uint64_t>(byte & 0x7fU) << (index * 7U);
            if ((byte & 0x80U) == 0) {
                return true;
            }
        }
        return false;
    }
    [[nodiscard]] bool var_i64(std::int64_t& value) {
        std::uint64_t encoded = 0;
        if (!var_u64(encoded)) {
            return false;
        }
        const auto bits = (encoded >> 1U) ^ (0U - (encoded & 1U));
        value = std::bit_cast<std::int64_t>(bits);
        return true;
    }
    [[nodiscard]] bool text_u16(std::string& value) {
        std::uint16_t size = 0;
        if (!u16(size) || remaining() < size) {
            return false;
        }
        value.assign(bytes_.substr(offset_, size));
        offset_ += size;
        return true;
    }
    [[nodiscard]] std::size_t remaining() const noexcept {
        return bytes_.size() - offset_;
    }
    [[nodiscard]] bool finished() const noexcept {
        return offset_ == bytes_.size();
    }

  private:
    template <typename T>
        requires(std::is_unsigned_v<T>)
    [[nodiscard]] bool unsigned_integer(T& value) {
        if (remaining() < sizeof(T)) {
            return false;
        }
        std::uint64_t decoded = 0;
        for (std::size_t index = 0; index < sizeof(T); ++index) {
            decoded |= static_cast<std::uint64_t>(
                           static_cast<unsigned char>(bytes_[offset_ + index]))
                       << (index * 8U);
        }
        value = static_cast<T>(decoded);
        offset_ += sizeof(T);
        return true;
    }

    std::string_view bytes_;
    std::size_t offset_ = 0;
};

} // namespace heartstead::net
