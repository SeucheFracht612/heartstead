#include "engine/entities/entity_visual.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <tuple>
#include <unordered_set>
#include <utility>

namespace heartstead::entities {

namespace {

[[nodiscard]] const std::string* field(const modding::GenericPrototype& prototype,
                                       std::string_view key) {
    const auto found = prototype.fields.find(std::string(key));
    return found == prototype.fields.end() ? nullptr : &found->second;
}

[[nodiscard]] bool supported_animation_role(std::string_view role) noexcept {
    return role == "idle" || role == "walk" || role == "run" || role == "jump" || role == "fall" ||
           role == "swim";
}

[[nodiscard]] bool supported_sound_role(std::string_view role) noexcept {
    return role == "footstep_default" || role == "jump" || role == "land";
}

[[nodiscard]] core::Result<bool> parse_bool(std::string_view value, std::string_view name) {
    if (value == "true") {
        return core::Result<bool>::success(true);
    }
    if (value == "false") {
        return core::Result<bool>::success(false);
    }
    return core::Result<bool>::failure("entity_visual.invalid_bool",
                                       std::string(name) + " must be true or false");
}

[[nodiscard]] bool valid_visual_name(std::string_view value) noexcept {
    return !value.empty() && value.size() <= 256U && value.find('\0') == std::string_view::npos;
}

[[nodiscard]] bool supported_preview_lighting(std::string_view value) noexcept {
    constexpr std::array presets{
        std::string_view{"studio"},       std::string_view{"overcast"},
        std::string_view{"noon"},         std::string_view{"sunset"},
        std::string_view{"night"},        std::string_view{"fire_lit_interior"},
        std::string_view{"cave"},         std::string_view{"forest_shade"},
        std::string_view{"rain_wetness"}, std::string_view{"snow_frost"},
        std::string_view{"underwater"},
    };
    return std::ranges::find(presets, value) != presets.end();
}

template <typename Number> [[nodiscard]] bool parse_number(std::string_view text, Number& output) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), output);
    return error == std::errc{} && end == text.data() + text.size();
}

[[nodiscard]] std::vector<std::string> split_names(std::string_view value) {
    std::vector<std::string> result;
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const auto end = value.find(',', begin);
        auto part =
            value.substr(begin, end == std::string_view::npos ? value.size() - begin : end - begin);
        while (!part.empty() && (part.front() == ' ' || part.front() == '\t')) {
            part.remove_prefix(1);
        }
        while (!part.empty() && (part.back() == ' ' || part.back() == '\t')) {
            part.remove_suffix(1);
        }
        if (!part.empty()) {
            result.emplace_back(part);
        }
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1U;
    }
    return result;
}

[[nodiscard]] core::Result<math::Bounds3f> parse_bounds(std::string_view value) {
    const auto parts = split_names(value);
    if (parts.size() != 6U) {
        return core::Result<math::Bounds3f>::failure(
            "visual_prefab.invalid_bounds",
            "visual prefab bounds must contain six comma-separated floats");
    }
    std::array<float, 6> values{};
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (!parse_number(parts[index], values[index])) {
            return core::Result<math::Bounds3f>::failure(
                "visual_prefab.invalid_bounds", "visual prefab bounds contain an invalid float");
        }
    }
    math::Bounds3f bounds{{values[0], values[1], values[2]}, {values[3], values[4], values[5]}};
    if (!bounds.is_valid()) {
        return core::Result<math::Bounds3f>::failure(
            "visual_prefab.invalid_bounds", "visual prefab bounds must be finite and ordered");
    }
    return core::Result<math::Bounds3f>::success(bounds);
}

[[nodiscard]] VisualStateRule& find_or_add_state_rule(std::vector<VisualStateRule>& rules,
                                                      std::string channel, std::string value) {
    const auto found = std::ranges::find_if(rules, [&](const VisualStateRule& rule) {
        return rule.channel == channel && rule.value == value;
    });
    if (found != rules.end()) {
        return *found;
    }
    VisualStateRule rule;
    rule.channel = std::move(channel);
    rule.value = std::move(value);
    rules.push_back(std::move(rule));
    return rules.back();
}

[[nodiscard]] core::Status require_model_asset(const assets::AssetCatalog& assets,
                                               std::string_view logical_id,
                                               std::string_view context) {
    const auto* model = assets.find_active(logical_id);
    if (model == nullptr || model->kind != assets::AssetKind::model) {
        return core::Status::failure(
            "visual_prefab.missing_model",
            std::string(context) + " does not resolve as a model: " + std::string(logical_id));
    }
    return core::Status::ok();
}

} // namespace

core::Status EntityVisualDefinition::validate() const {
    if (!id.is_valid() || !entity_prototype.is_valid() || model_asset.empty() ||
        !std::isfinite(model_scale) || model_scale <= 0.0F || model_scale > 100.0F ||
        !std::isfinite(bounds_padding) || bounds_padding < 0.0F || bounds_padding > 100.0F ||
        transition_ticks > 600U) {
        return core::Status::failure(
            "entity_visual.invalid_definition",
            "entity visual requires valid ids, a model, positive bounded scale/padding, and "
            "transition ticks");
    }
    for (const auto& [role, clip] : animation_clips) {
        if (!supported_animation_role(role) || clip.empty()) {
            return core::Status::failure(
                "entity_visual.invalid_animation",
                "entity visual animation roles must be supported and name a clip");
        }
    }
    for (const auto& [role, sound_event] : sound_events) {
        if (!supported_sound_role(role) || !sound_event.is_valid()) {
            return core::Status::failure(
                "entity_visual.invalid_sound",
                "entity visual sound roles must be supported and reference an event");
        }
    }
    if (!supported_preview_lighting(preview.lighting_preset) ||
        !std::isfinite(preview.camera_distance) || preview.camera_distance <= 0.0F ||
        !std::isfinite(preview.yaw_degrees) || !std::isfinite(preview.pitch_degrees) ||
        (bounds_override.has_value() && !bounds_override->is_valid())) {
        return core::Status::failure(
            "visual_prefab.invalid_preview",
            "visual prefab preview lighting, camera, or bounds are invalid");
    }
    std::unordered_set<std::string> material_slots;
    for (const auto& material : material_overrides) {
        if (!valid_visual_name(material.slot) || !material.material.is_valid() ||
            !material_slots.insert(material.slot).second) {
            return core::Status::failure(
                "visual_prefab.invalid_material_override",
                "visual prefab material overrides require unique slots and valid materials");
        }
    }
    std::unordered_set<std::string> aliases;
    for (const auto& [alias, socket] : socket_aliases) {
        if (!valid_visual_name(alias) || !valid_visual_name(socket) ||
            !aliases.insert(alias).second) {
            return core::Status::failure(
                "visual_prefab.invalid_socket_alias",
                "visual prefab socket aliases require unique bounded names");
        }
    }
    std::unordered_set<std::string> anchor_names;
    for (const auto& anchor : anchors) {
        const auto valid_kind = anchor.kind == VisualAnchorKind::equipment ||
                                anchor.kind == VisualAnchorKind::effect ||
                                anchor.kind == VisualAnchorKind::light;
        if (!valid_visual_name(anchor.name) || !valid_visual_name(anchor.socket) || !valid_kind ||
            !anchor_names
                 .insert(std::to_string(static_cast<std::uint8_t>(anchor.kind)) + ":" + anchor.name)
                 .second) {
            return core::Status::failure(
                "visual_prefab.invalid_anchor",
                "visual prefab anchors require unique names, valid kinds, and named sockets");
        }
    }
    std::unordered_set<std::string> group_names;
    for (const auto& group : visibility_groups) {
        std::unordered_set<std::string> nodes;
        if (!valid_visual_name(group.name) || group.nodes.empty() ||
            !group_names.insert(group.name).second ||
            !std::ranges::all_of(group.nodes, [&](const std::string& node) {
                return valid_visual_name(node) && nodes.insert(node).second;
            })) {
            return core::Status::failure(
                "visual_prefab.invalid_visibility_group",
                "visual prefab visibility groups require unique names and model nodes");
        }
    }
    std::unordered_set<std::string> state_keys;
    for (const auto& rule : state_rules) {
        if (!valid_visual_name(rule.channel) || !valid_visual_name(rule.value) ||
            !state_keys.insert(rule.channel + ":" + rule.value).second ||
            std::ranges::any_of(
                rule.group_visibility,
                [&](const auto& visibility) { return !group_names.contains(visibility.first); }) ||
            std::ranges::any_of(rule.material_overrides, [](const auto& material) {
                return !valid_visual_name(material.slot) || !material.material.is_valid();
            })) {
            return core::Status::failure(
                "visual_prefab.invalid_state",
                "visual prefab state rules require unique state values and valid presentation "
                "mappings");
        }
    }
    float expected_minimum = 0.0F;
    for (std::size_t index = 0; index < lods.size(); ++index) {
        const auto& lod = lods[index];
        const auto valid_maximum =
            lod.maximum_distance == 0.0F ||
            (std::isfinite(lod.maximum_distance) && lod.maximum_distance > lod.minimum_distance);
        if (lod.level != index || lod.model_asset.empty() || !std::isfinite(lod.minimum_distance) ||
            lod.minimum_distance != expected_minimum || !valid_maximum ||
            (lod.maximum_distance == 0.0F && index + 1U != lods.size())) {
            return core::Status::failure(
                "visual_prefab.invalid_lod",
                "visual prefab LODs must be contiguous with ordered distance ranges");
        }
        expected_minimum = lod.maximum_distance;
    }
    if (impostor.enabled() &&
        (!std::isfinite(impostor.start_distance) || impostor.start_distance <= 0.0F ||
         impostor.view_count == 0U || impostor.view_count > 256U)) {
        return core::Status::failure(
            "visual_prefab.invalid_impostor",
            "visual prefab impostors require a model, positive distance, and bounded views");
    }
    if (shadow_policy == VisualShadowPolicy::none && cast_shadow) {
        return core::Status::failure(
            "visual_prefab.shadow_policy_mismatch",
            "visual prefab shadow policy and legacy cast_shadow field disagree");
    }
    return core::Status::ok();
}

const std::string* EntityVisualDefinition::animation(std::string_view role) const noexcept {
    const auto found = animation_clips.find(std::string(role));
    return found == animation_clips.end() ? nullptr : &found->second;
}

const core::PrototypeId* EntityVisualDefinition::sound(std::string_view role) const noexcept {
    const auto found = sound_events.find(std::string(role));
    return found == sound_events.end() ? nullptr : &found->second;
}

const VisualVisibilityGroup*
EntityVisualDefinition::visibility_group(std::string_view name) const noexcept {
    const auto found =
        std::ranges::find_if(visibility_groups, [name](const VisualVisibilityGroup& group) {
            return group.name == name;
        });
    return found == visibility_groups.end() ? nullptr : &*found;
}

const VisualStateRule* EntityVisualDefinition::resolve_state_rule(
    std::span<const VisualStateValue> states) const noexcept {
    const VisualStateRule* selected = nullptr;
    for (const auto& rule : state_rules) {
        const auto matches = std::ranges::any_of(states, [&](const VisualStateValue& state) {
            return state.channel == rule.channel && state.value == rule.value;
        });
        if (!matches) {
            continue;
        }
        if (selected == nullptr || rule.priority > selected->priority ||
            (rule.priority == selected->priority &&
             std::tie(rule.channel, rule.value) < std::tie(selected->channel, selected->value))) {
            selected = &rule;
        }
    }
    return selected;
}

std::string_view
EntityVisualDefinition::resolve_model(std::span<const VisualStateValue> states) const noexcept {
    const auto* rule = resolve_state_rule(states);
    return rule != nullptr && !rule->model_asset.empty() ? std::string_view(rule->model_asset)
                                                         : std::string_view(model_asset);
}

core::Status VisualDefinitionRegistry::add(EntityVisualDefinition definition) {
    auto status = definition.validate();
    if (!status) {
        return status;
    }
    if (by_id_.contains(definition.id.value())) {
        return core::Status::failure("entity_visual.duplicate_id",
                                     "visual definition id is duplicated: " +
                                         definition.id.value());
    }
    if (by_entity_.contains(definition.entity_prototype.value())) {
        return core::Status::failure("entity_visual.duplicate_entity",
                                     "entity prototype has more than one visual definition: " +
                                         definition.entity_prototype.value());
    }
    const auto index = definitions_.size();
    by_id_.emplace(definition.id.value(), index);
    by_entity_.emplace(definition.entity_prototype.value(), index);
    definitions_.push_back(std::move(definition));
    return core::Status::ok();
}

const EntityVisualDefinition*
VisualDefinitionRegistry::find(const core::PrototypeId& visual_id) const noexcept {
    const auto found = by_id_.find(visual_id.value());
    return found == by_id_.end() ? nullptr : &definitions_[found->second];
}

const EntityVisualDefinition* VisualDefinitionRegistry::find_for_entity(
    const core::PrototypeId& entity_prototype) const noexcept {
    const auto found = by_entity_.find(entity_prototype.value());
    return found == by_entity_.end() ? nullptr : &definitions_[found->second];
}

const std::vector<EntityVisualDefinition>& VisualDefinitionRegistry::definitions() const noexcept {
    return definitions_;
}

std::size_t VisualDefinitionRegistry::size() const noexcept {
    return definitions_.size();
}

core::Result<EntityVisualDefinition>
entity_visual_definition_from_prototype(const modding::GenericPrototype& prototype,
                                        const modding::PrototypeRegistry& prototypes,
                                        const assets::AssetCatalog& assets) {
    if (prototype.kind != modding::PrototypeKinds::entity_visual &&
        prototype.kind != modding::PrototypeKinds::visual_prefab) {
        return core::Result<EntityVisualDefinition>::failure("entity_visual.kind_mismatch",
                                                             "prototype is not an entity visual");
    }
    const auto* entity_text = field(prototype, "entity");
    const auto* model_text = field(prototype, "model");
    const auto entity = entity_text == nullptr ? std::optional<core::PrototypeId>{}
                                               : core::PrototypeId::parse(*entity_text);
    if (!entity || model_text == nullptr || model_text->empty()) {
        return core::Result<EntityVisualDefinition>::failure(
            "entity_visual.missing_reference",
            "entity visual must reference an entity prototype and model asset");
    }
    auto status = prototypes.require_kind(*entity, modding::PrototypeKinds::entity);
    if (!status) {
        return core::Result<EntityVisualDefinition>::failure(status.error().code,
                                                             status.error().message);
    }
    const auto* model = assets.find_active(*model_text);
    if (model == nullptr || model->kind != assets::AssetKind::model) {
        return core::Result<EntityVisualDefinition>::failure(
            "entity_visual.missing_model",
            "entity visual model asset does not resolve as a model: " + *model_text);
    }

    EntityVisualDefinition definition;
    definition.id = prototype.id;
    definition.entity_prototype = *entity;
    definition.model_asset = *model_text;
    struct PendingLod {
        std::string model;
        float maximum_distance = 0.0F;
        bool has_model = false;
        bool has_maximum = false;
    };
    std::map<std::uint32_t, PendingLod> pending_lods;
    for (const auto& [key, value] : prototype.fields) {
        constexpr std::string_view animation_prefix = "animations.";
        constexpr std::string_view sound_prefix = "sounds.";
        if (key.starts_with(animation_prefix)) {
            definition.animation_clips.emplace(key.substr(animation_prefix.size()), value);
        } else if (key.starts_with(sound_prefix)) {
            auto sound_event = core::PrototypeId::parse(value);
            if (!sound_event) {
                return core::Result<EntityVisualDefinition>::failure(
                    "entity_visual.invalid_sound_reference",
                    "entity visual sound reference must be a prototype id");
            }
            status = prototypes.require_kind(*sound_event, modding::PrototypeKinds::sound_event);
            if (!status) {
                return core::Result<EntityVisualDefinition>::failure(status.error().code,
                                                                     status.error().message);
            }
            definition.sound_events.emplace(key.substr(sound_prefix.size()), *sound_event);
        } else if (key.starts_with("lods.")) {
            const auto suffix = std::string_view(key).substr(5U);
            const auto separator = suffix.find('.');
            std::uint32_t level = 0;
            if (separator == std::string_view::npos ||
                !parse_number(suffix.substr(0, separator), level)) {
                return core::Result<EntityVisualDefinition>::failure(
                    "visual_prefab.invalid_lod",
                    "visual prefab LOD fields must use lods.<level>.model/max_distance");
            }
            auto& pending = pending_lods[level];
            const auto property = suffix.substr(separator + 1U);
            if (property == "model") {
                status = require_model_asset(assets, value, "visual prefab LOD");
                if (!status) {
                    return core::Result<EntityVisualDefinition>::failure(status.error().code,
                                                                         status.error().message);
                }
                pending.model = value;
                pending.has_model = true;
            } else if (property == "max_distance") {
                if (!parse_number(value, pending.maximum_distance) ||
                    !std::isfinite(pending.maximum_distance) || pending.maximum_distance < 0.0F) {
                    return core::Result<EntityVisualDefinition>::failure(
                        "visual_prefab.invalid_lod",
                        "visual prefab LOD max_distance must be a non-negative float");
                }
                pending.has_maximum = true;
            }
        } else if (key.starts_with("materials.")) {
            auto material = core::PrototypeId::parse(value);
            if (!material) {
                return core::Result<EntityVisualDefinition>::failure(
                    "visual_prefab.invalid_material_override",
                    "visual prefab material override must reference a material prototype");
            }
            status = prototypes.require_kind(*material, modding::PrototypeKinds::material);
            if (!status) {
                return core::Result<EntityVisualDefinition>::failure(status.error().code,
                                                                     status.error().message);
            }
            definition.material_overrides.push_back(
                {key.substr(std::string_view{"materials."}.size()), *material});
        } else if (key.starts_with("socket_aliases.")) {
            definition.socket_aliases.emplace(
                key.substr(std::string_view{"socket_aliases."}.size()), value);
        } else if (key.starts_with("groups.")) {
            definition.visibility_groups.push_back(
                {key.substr(std::string_view{"groups."}.size()), split_names(value)});
        } else if (key.starts_with("anchors.")) {
            const auto suffix = std::string_view(key).substr(8U);
            const auto separator = suffix.find('.');
            if (separator == std::string_view::npos) {
                return core::Result<EntityVisualDefinition>::failure(
                    "visual_prefab.invalid_anchor",
                    "visual prefab anchors must use anchors.<kind>.<name>");
            }
            const auto kind = suffix.substr(0, separator);
            const auto anchor_kind = kind == "equipment"
                                         ? std::optional{VisualAnchorKind::equipment}
                                     : kind == "effect" ? std::optional{VisualAnchorKind::effect}
                                     : kind == "light"  ? std::optional{VisualAnchorKind::light}
                                                        : std::nullopt;
            if (!anchor_kind) {
                return core::Result<EntityVisualDefinition>::failure(
                    "visual_prefab.invalid_anchor",
                    "visual prefab anchor kind must be equipment, effect, or light");
            }
            definition.anchors.push_back(
                {std::string(suffix.substr(separator + 1U)), *anchor_kind, value});
        } else if (key.starts_with("states.")) {
            const auto suffix = std::string_view(key).substr(7U);
            const auto channel_end = suffix.find('.');
            const auto value_end = channel_end == std::string_view::npos
                                       ? std::string_view::npos
                                       : suffix.find('.', channel_end + 1U);
            if (channel_end == std::string_view::npos || value_end == std::string_view::npos) {
                return core::Result<EntityVisualDefinition>::failure(
                    "visual_prefab.invalid_state",
                    "visual prefab state fields must use states.<channel>.<value>.<mapping>");
            }
            auto& rule = find_or_add_state_rule(
                definition.state_rules, std::string(suffix.substr(0, channel_end)),
                std::string(suffix.substr(channel_end + 1U, value_end - channel_end - 1U)));
            const auto property = suffix.substr(value_end + 1U);
            if (property == "model") {
                status = require_model_asset(assets, value, "visual prefab state model");
                if (!status) {
                    return core::Result<EntityVisualDefinition>::failure(status.error().code,
                                                                         status.error().message);
                }
                rule.model_asset = value;
            } else if (property == "animation") {
                rule.animation_clip = value;
            } else if (property == "priority") {
                if (!parse_number(value, rule.priority)) {
                    return core::Result<EntityVisualDefinition>::failure(
                        "visual_prefab.invalid_state",
                        "visual prefab state priority must be an integer");
                }
            } else if (property.starts_with("groups.")) {
                auto visible = parse_bool(value, property);
                if (!visible) {
                    return core::Result<EntityVisualDefinition>::failure(visible.error().code,
                                                                         visible.error().message);
                }
                rule.group_visibility.emplace(
                    std::string(property.substr(std::string_view{"groups."}.size())),
                    visible.value());
            } else if (property.starts_with("materials.")) {
                auto material = core::PrototypeId::parse(value);
                if (!material) {
                    return core::Result<EntityVisualDefinition>::failure(
                        "visual_prefab.invalid_material_override",
                        "visual prefab state material must reference a material prototype");
                }
                status = prototypes.require_kind(*material, modding::PrototypeKinds::material);
                if (!status) {
                    return core::Result<EntityVisualDefinition>::failure(status.error().code,
                                                                         status.error().message);
                }
                rule.material_overrides.push_back(
                    {std::string(property.substr(std::string_view{"materials."}.size())),
                     *material});
            }
        } else if (key.starts_with("transitions.")) {
            std::uint32_t ticks = 0;
            if (!parse_number(value, ticks) || ticks > 600U) {
                return core::Result<EntityVisualDefinition>::failure(
                    "visual_prefab.invalid_transition",
                    "visual prefab animation transition must be at most 600 ticks");
            }
            definition.animation_transitions.emplace(
                key.substr(std::string_view{"transitions."}.size()), ticks);
        } else if (key.starts_with("preview.state.")) {
            definition.preview.states.emplace(key.substr(std::string_view{"preview.state."}.size()),
                                              value);
        }
    }
    if (!pending_lods.empty()) {
        float minimum = 0.0F;
        std::uint32_t expected_level = 0;
        for (const auto& [level, pending] : pending_lods) {
            if (level != expected_level++ || !pending.has_model || !pending.has_maximum) {
                return core::Result<EntityVisualDefinition>::failure(
                    "visual_prefab.invalid_lod",
                    "visual prefab LODs must be contiguous and define model and max_distance");
            }
            definition.lods.push_back({level, pending.model, minimum, pending.maximum_distance});
            minimum = pending.maximum_distance;
        }
        if (definition.lods.back().maximum_distance != 0.0F) {
            return core::Result<EntityVisualDefinition>::failure(
                "visual_prefab.invalid_lod",
                "the final visual prefab LOD must use max_distance = 0");
        }
    }
    if (const auto* scale = field(prototype, "model_scale"); scale != nullptr) {
        const auto [end, error] =
            std::from_chars(scale->data(), scale->data() + scale->size(), definition.model_scale);
        if (error != std::errc{} || end != scale->data() + scale->size()) {
            return core::Result<EntityVisualDefinition>::failure(
                "entity_visual.invalid_model_scale",
                "entity visual model_scale must be a decimal number");
        }
    }
    if (const auto* padding = field(prototype, "bounds_padding"); padding != nullptr) {
        const auto [end, error] = std::from_chars(
            padding->data(), padding->data() + padding->size(), definition.bounds_padding);
        if (error != std::errc{} || end != padding->data() + padding->size()) {
            return core::Result<EntityVisualDefinition>::failure(
                "entity_visual.invalid_bounds_padding",
                "entity visual bounds_padding must be a decimal number");
        }
    }
    if (const auto* ticks = field(prototype, "transition_ticks"); ticks != nullptr) {
        const auto [end, error] = std::from_chars(ticks->data(), ticks->data() + ticks->size(),
                                                  definition.transition_ticks);
        if (error != std::errc{} || end != ticks->data() + ticks->size()) {
            return core::Result<EntityVisualDefinition>::failure(
                "entity_visual.invalid_transition_ticks",
                "entity visual transition_ticks must be an integer");
        }
    }
    if (const auto* shadow = field(prototype, "cast_shadow"); shadow != nullptr) {
        auto parsed = parse_bool(*shadow, "cast_shadow");
        if (!parsed) {
            return core::Result<EntityVisualDefinition>::failure(parsed.error().code,
                                                                 parsed.error().message);
        }
        definition.cast_shadow = parsed.value();
        definition.shadow_policy =
            parsed.value() ? VisualShadowPolicy::cast : VisualShadowPolicy::none;
    }
    if (const auto* shadow = field(prototype, "shadow_policy"); shadow != nullptr) {
        if (*shadow == "cast") {
            definition.shadow_policy = VisualShadowPolicy::cast;
            definition.cast_shadow = true;
        } else if (*shadow == "none") {
            definition.shadow_policy = VisualShadowPolicy::none;
            definition.cast_shadow = false;
        } else {
            return core::Result<EntityVisualDefinition>::failure(
                "visual_prefab.invalid_shadow_policy",
                "visual prefab shadow_policy must be cast or none");
        }
    }
    if (const auto* bounds = field(prototype, "bounds"); bounds != nullptr) {
        auto parsed = parse_bounds(*bounds);
        if (!parsed) {
            return core::Result<EntityVisualDefinition>::failure(parsed.error().code,
                                                                 parsed.error().message);
        }
        definition.bounds_override = parsed.value();
    }
    if (const auto* lighting = field(prototype, "preview.lighting"); lighting != nullptr) {
        definition.preview.lighting_preset = *lighting;
    }
    for (const auto& [key, output] :
         {std::pair{std::string_view{"preview.camera_distance"},
                    &definition.preview.camera_distance},
          std::pair{std::string_view{"preview.yaw_degrees"}, &definition.preview.yaw_degrees},
          std::pair{std::string_view{"preview.pitch_degrees"},
                    &definition.preview.pitch_degrees}}) {
        if (const auto* value = field(prototype, key);
            value != nullptr && !parse_number(*value, *output)) {
            return core::Result<EntityVisualDefinition>::failure(
                "visual_prefab.invalid_preview",
                "visual prefab preview camera fields must be decimal numbers");
        }
    }
    if (const auto* impostor_model = field(prototype, "impostor.model");
        impostor_model != nullptr) {
        status = require_model_asset(assets, *impostor_model, "visual prefab impostor");
        if (!status) {
            return core::Result<EntityVisualDefinition>::failure(status.error().code,
                                                                 status.error().message);
        }
        definition.impostor.model_asset = *impostor_model;
    }
    if (const auto* distance = field(prototype, "impostor.start_distance");
        distance != nullptr && !parse_number(*distance, definition.impostor.start_distance)) {
        return core::Result<EntityVisualDefinition>::failure(
            "visual_prefab.invalid_impostor",
            "visual prefab impostor start_distance must be a decimal number");
    }
    if (const auto* views = field(prototype, "impostor.view_count");
        views != nullptr && !parse_number(*views, definition.impostor.view_count)) {
        return core::Result<EntityVisualDefinition>::failure(
            "visual_prefab.invalid_impostor",
            "visual prefab impostor view_count must be an integer");
    }
    if (definition.lods.empty()) {
        definition.lods.push_back({0U, definition.model_asset, 0.0F, 0.0F});
    }
    std::ranges::sort(
        definition.state_rules, [](const VisualStateRule& left, const VisualStateRule& right) {
            return std::tie(left.channel, left.value) < std::tie(right.channel, right.value);
        });
    status = definition.validate();
    if (!status) {
        return core::Result<EntityVisualDefinition>::failure(status.error().code,
                                                             status.error().message);
    }
    return core::Result<EntityVisualDefinition>::success(std::move(definition));
}

core::Result<VisualDefinitionRegistry>
visual_definition_registry_from_prototypes(const modding::PrototypeRegistry& prototypes,
                                           const assets::AssetCatalog& assets) {
    VisualDefinitionRegistry registry;
    for (const auto kind :
         {modding::PrototypeKinds::entity_visual, modding::PrototypeKinds::visual_prefab}) {
        for (const auto* prototype : prototypes.prototypes_of_kind(kind)) {
            auto definition =
                entity_visual_definition_from_prototype(*prototype, prototypes, assets);
            if (!definition) {
                return core::Result<VisualDefinitionRegistry>::failure(definition.error().code,
                                                                       definition.error().message);
            }
            auto status = registry.add(std::move(definition).value());
            if (!status) {
                return core::Result<VisualDefinitionRegistry>::failure(status.error().code,
                                                                       status.error().message);
            }
        }
    }
    return core::Result<VisualDefinitionRegistry>::success(std::move(registry));
}

} // namespace heartstead::entities
