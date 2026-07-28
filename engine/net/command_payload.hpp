#pragma once

#include "engine/core/result.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <string_view>

namespace heartstead::net {

class CommandPayload {
  public:
    [[nodiscard]] core::Status set(std::string key, std::string value);
    [[nodiscard]] const std::string* find(std::string_view key) const;
    [[nodiscard]] core::Result<std::string_view> require(std::string_view key) const;

    [[nodiscard]] const std::map<std::string, std::string>& fields() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

  private:
    std::map<std::string, std::string> fields_;
};

class CommandPayloadTextCodec {
  public:
    [[nodiscard]] static std::string encode(const CommandPayload& payload);
    [[nodiscard]] static core::Result<CommandPayload> decode(std::string_view text);
};

class CommandPayloadBinaryCodec {
  public:
    [[nodiscard]] static std::string encode(const CommandPayload& payload);
    [[nodiscard]] static core::Result<CommandPayload> decode(std::string_view bytes);
    [[nodiscard]] static bool is_encoded(std::string_view bytes) noexcept;
};

[[nodiscard]] bool is_valid_command_payload_key(std::string_view key) noexcept;

} // namespace heartstead::net
