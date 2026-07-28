#include "engine/net/command_payload.hpp"

#include "engine/net/binary_message.hpp"

#include <cstdint>
#include <string>
#include <utility>

namespace heartstead::net {

namespace {

constexpr std::size_t max_payload_bytes = 64U * 1024U;
constexpr std::size_t max_payload_fields = 128;
constexpr std::size_t max_payload_key_bytes = 128;
constexpr std::size_t max_payload_value_bytes = 32U * 1024U;
constexpr std::uint32_t binary_magic = 0x31504348U; // "HCP1" in little-endian byte order.

[[nodiscard]] bool is_hex_digit(char value) noexcept {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

[[nodiscard]] char hex_value(char value) noexcept {
    if (value >= '0' && value <= '9') {
        return static_cast<char>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return static_cast<char>(10 + value - 'a');
    }
    return static_cast<char>(10 + value - 'A');
}

[[nodiscard]] std::string percent_escape(std::string_view input) {
    std::string result;
    result.reserve(input.size());

    constexpr char hex[] = "0123456789ABCDEF";
    for (const auto value : input) {
        const auto byte = static_cast<unsigned char>(value);
        if (value == '%' || value == ';' || value == '=' || value == '|' || value == '\n' ||
            value == '\r') {
            result.push_back('%');
            result.push_back(hex[(byte >> 4u) & 0x0Fu]);
            result.push_back(hex[byte & 0x0Fu]);
        } else {
            result.push_back(value);
        }
    }
    return result;
}

[[nodiscard]] std::size_t percent_escaped_size(std::string_view input) noexcept {
    std::size_t size = 0;
    for (const auto value : input) {
        if (value == '%' || value == ';' || value == '=' || value == '|' || value == '\n' ||
            value == '\r') {
            size += 3;
        } else {
            ++size;
        }
    }
    return size;
}

[[nodiscard]] core::Result<std::string> percent_unescape(std::string_view input) {
    std::string result;
    result.reserve(input.size());

    for (std::size_t index = 0; index < input.size(); ++index) {
        if (input[index] != '%') {
            result.push_back(input[index]);
            continue;
        }
        if (index + 2 >= input.size() || !is_hex_digit(input[index + 1]) ||
            !is_hex_digit(input[index + 2])) {
            return core::Result<std::string>::failure("command_payload.invalid_escape",
                                                      "command payload contains an invalid escape");
        }
        const auto high = hex_value(input[index + 1]);
        const auto low = hex_value(input[index + 2]);
        result.push_back(static_cast<char>((high << 4) | low));
        index += 2;
    }

    return core::Result<std::string>::success(std::move(result));
}

} // namespace

bool is_valid_command_payload_key(std::string_view key) noexcept {
    if (key.empty() || key.front() == '.' || key.back() == '.') {
        return false;
    }

    for (const auto character : key) {
        const auto valid = (character >= 'a' && character <= 'z') ||
                           (character >= '0' && character <= '9') || character == '_' ||
                           character == '-' || character == '.';
        if (!valid) {
            return false;
        }
    }
    return true;
}

core::Status CommandPayload::set(std::string key, std::string value) {
    if (!is_valid_command_payload_key(key)) {
        return core::Status::failure("command_payload.invalid_key",
                                     "command payload key is invalid: " + key);
    }
    if (key.size() > max_payload_key_bytes || value.size() > max_payload_value_bytes) {
        return core::Status::failure("command_payload.limit_exceeded",
                                     "command payload exceeds its field or byte limits");
    }
    if (fields_.contains(key)) {
        return core::Status::failure("command_payload.duplicate_key",
                                     "command payload key is duplicated: " + key);
    }
    if (fields_.size() >= max_payload_fields) {
        return core::Status::failure("command_payload.limit_exceeded",
                                     "command payload exceeds its field or byte limits");
    }
    std::size_t encoded_bytes = fields_.empty() ? 0 : fields_.size() - 1;
    for (const auto& [existing_key, existing_value] : fields_) {
        encoded_bytes += existing_key.size() + 1 + percent_escaped_size(existing_value);
    }
    const auto added_bytes =
        (fields_.empty() ? 0 : 1) + key.size() + 1 + percent_escaped_size(value);
    if (encoded_bytes > max_payload_bytes || added_bytes > max_payload_bytes - encoded_bytes) {
        return core::Status::failure("command_payload.limit_exceeded",
                                     "command payload exceeds its encoded byte limit");
    }

    fields_.emplace(std::move(key), std::move(value));
    return core::Status::ok();
}

const std::string* CommandPayload::find(std::string_view key) const {
    const auto found = fields_.find(std::string(key));
    if (found == fields_.end()) {
        return nullptr;
    }
    return &found->second;
}

core::Result<std::string_view> CommandPayload::require(std::string_view key) const {
    const auto* value = find(key);
    if (value == nullptr || value->empty()) {
        return core::Result<std::string_view>::failure("command_payload.missing_required_key",
                                                       "command payload is missing required key: " +
                                                           std::string(key));
    }
    return core::Result<std::string_view>::success(*value);
}

const std::map<std::string, std::string>& CommandPayload::fields() const noexcept {
    return fields_;
}

std::size_t CommandPayload::size() const noexcept {
    return fields_.size();
}

std::string CommandPayloadTextCodec::encode(const CommandPayload& payload) {
    std::string result;
    bool first = true;
    for (const auto& [key, value] : payload.fields()) {
        if (!first) {
            result.push_back(';');
        }
        first = false;
        result.append(key);
        result.push_back('=');
        result.append(percent_escape(value));
    }
    return result;
}

core::Result<CommandPayload> CommandPayloadTextCodec::decode(std::string_view text) {
    if (text.empty()) {
        return core::Result<CommandPayload>::failure("command_payload.empty",
                                                     "command payload must not be empty");
    }
    if (text.size() > max_payload_bytes) {
        return core::Result<CommandPayload>::failure("command_payload.too_large",
                                                     "command payload exceeds 64 KiB");
    }

    CommandPayload payload;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find(';', start);
        const auto entry =
            end == std::string_view::npos ? text.substr(start) : text.substr(start, end - start);
        if (entry.empty()) {
            return core::Result<CommandPayload>::failure("command_payload.invalid_entry",
                                                         "command payload contains an empty entry");
        }
        if (entry.find('\n') != std::string_view::npos ||
            entry.find('\r') != std::string_view::npos) {
            return core::Result<CommandPayload>::failure(
                "command_payload.invalid_entry",
                "command payload entries must not contain raw line breaks");
        }

        const auto separator = entry.find('=');
        if (separator == std::string_view::npos || separator == 0) {
            return core::Result<CommandPayload>::failure(
                "command_payload.invalid_entry",
                "command payload entries must use key=value syntax");
        }

        const auto key = entry.substr(0, separator);
        const auto value = entry.substr(separator + 1);
        if (!is_valid_command_payload_key(key)) {
            return core::Result<CommandPayload>::failure("command_payload.invalid_key",
                                                         "command payload key is invalid: " +
                                                             std::string(key));
        }
        if (value.find('=') != std::string_view::npos) {
            return core::Result<CommandPayload>::failure(
                "command_payload.invalid_entry",
                "command payload values must escape reserved delimiters");
        }

        auto decoded_value = percent_unescape(value);
        if (!decoded_value) {
            return core::Result<CommandPayload>::failure(decoded_value.error().code,
                                                         decoded_value.error().message);
        }
        auto status = payload.set(std::string(key), std::move(decoded_value).value());
        if (!status) {
            return core::Result<CommandPayload>::failure(status.error().code,
                                                         status.error().message);
        }

        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }

    return core::Result<CommandPayload>::success(std::move(payload));
}

std::string CommandPayloadBinaryCodec::encode(const CommandPayload& payload) {
    BinaryMessageWriter writer;
    writer.u32(binary_magic);
    writer.var_u64(static_cast<std::uint64_t>(payload.fields().size()));
    for (const auto& [key, value] : payload.fields()) {
        writer.text_var(key);
        writer.text_var(value);
    }
    return writer.take();
}

core::Result<CommandPayload> CommandPayloadBinaryCodec::decode(std::string_view bytes) {
    if (bytes.size() > max_payload_bytes) {
        return core::Result<CommandPayload>::failure("command_payload.too_large",
                                                     "command payload exceeds 64 KiB");
    }
    BinaryMessageReader reader(bytes);
    std::uint32_t decoded_magic = 0;
    std::uint64_t field_count = 0;
    if (!reader.u32(decoded_magic) || decoded_magic != binary_magic ||
        !reader.var_u64(field_count)) {
        return core::Result<CommandPayload>::failure(
            "command_payload.invalid_binary",
            "command payload does not contain a valid binary header");
    }
    if (field_count == 0 || field_count > max_payload_fields) {
        return core::Result<CommandPayload>::failure(
            "command_payload.invalid_binary",
            "binary command payload field count is outside its bounds");
    }

    CommandPayload payload;
    for (std::uint64_t index = 0; index < field_count; ++index) {
        std::string key;
        std::string value;
        if (!reader.text_var(key, max_payload_key_bytes) ||
            !reader.text_var(value, max_payload_value_bytes)) {
            return core::Result<CommandPayload>::failure(
                "command_payload.invalid_binary",
                "binary command payload contains a truncated or oversized field");
        }
        auto status = payload.set(std::move(key), std::move(value));
        if (!status) {
            return core::Result<CommandPayload>::failure(status.error().code,
                                                         status.error().message);
        }
    }
    if (!reader.finished()) {
        return core::Result<CommandPayload>::failure(
            "command_payload.trailing_data", "binary command payload contains trailing bytes");
    }
    return core::Result<CommandPayload>::success(std::move(payload));
}

bool CommandPayloadBinaryCodec::is_encoded(std::string_view bytes) noexcept {
    BinaryMessageReader reader(bytes);
    std::uint32_t decoded_magic = 0;
    return reader.u32(decoded_magic) && decoded_magic == binary_magic;
}

} // namespace heartstead::net
