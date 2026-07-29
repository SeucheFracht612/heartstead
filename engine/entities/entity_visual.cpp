#include "engine/entities/entity_visual.hpp"

#include <charconv>
#include <cmath>
#include <optional>
#include <ranges>
#include <string>
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

} // namespace

core::Status EntityVisualDefinition::validate() const {
    if (!id.is_valid() || !entity_prototype.is_valid() || model_asset.empty() ||
        !std::isfinite(bounds_padding) || bounds_padding < 0.0F || bounds_padding > 100.0F ||
        transition_ticks > 600U) {
        return core::Status::failure(
            "entity_visual.invalid_definition",
            "entity visual requires valid ids, a model, bounded padding, and transition ticks");
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
    if (prototype.kind != modding::PrototypeKinds::entity_visual) {
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
    }
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
    for (const auto* prototype :
         prototypes.prototypes_of_kind(modding::PrototypeKinds::entity_visual)) {
        auto definition = entity_visual_definition_from_prototype(*prototype, prototypes, assets);
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
    return core::Result<VisualDefinitionRegistry>::success(std::move(registry));
}

} // namespace heartstead::entities
