#include "engine/modding/prototype_registry.hpp"

#include <algorithm>
#include <utility>

namespace heartstead::modding {

namespace {

[[nodiscard]] bool is_known_kind(std::string_view kind) noexcept {
    return kind == PrototypeKinds::item || kind == PrototypeKinds::cargo ||
           kind == PrototypeKinds::entity || kind == PrototypeKinds::voxel ||
           kind == PrototypeKinds::block_model || kind == PrototypeKinds::build_piece ||
           kind == PrototypeKinds::assembly || kind == PrototypeKinds::workpiece ||
           kind == PrototypeKinds::pattern || kind == PrototypeKinds::process ||
           kind == PrototypeKinds::fire || kind == PrototypeKinds::room_descriptor ||
           kind == PrototypeKinds::material || kind == PrototypeKinds::scenario ||
           kind == PrototypeKinds::recipe || kind == PrototypeKinds::biome ||
           kind == PrototypeKinds::world_feature || kind == PrototypeKinds::crop ||
           kind == PrototypeKinds::animal || kind == PrototypeKinds::map_layer ||
           kind == PrototypeKinds::ui_panel || kind == PrototypeKinds::network ||
           kind == PrototypeKinds::ward || kind == PrototypeKinds::admin_command ||
           kind == PrototypeKinds::sound_event || kind == PrototypeKinds::particle;
}

} // namespace

bool PrototypeRegistryBuildResult::has_errors() const noexcept {
    return std::ranges::any_of(diagnostics, [](const ModDiagnostic& diagnostic) {
        return diagnostic.severity == DiagnosticSeverity::error;
    });
}

PrototypeRegistryBuildResult PrototypeRegistry::build(std::vector<GenericPrototype> prototypes) {
    PrototypeRegistryBuildResult result;
    prototypes_ = std::move(prototypes);
    by_id_.clear();
    by_kind_.clear();

    for (std::size_t index = 0; index < prototypes_.size(); ++index) {
        const auto& prototype = prototypes_[index];
        if (!prototype.id.is_valid()) {
            result.diagnostics.push_back(ModDiagnostic{DiagnosticSeverity::error, prototype.source,
                                                       "prototype_registry.invalid_id",
                                                       "prototype has an invalid id"});
            continue;
        }
        if (!is_known_kind(prototype.kind)) {
            result.diagnostics.push_back(ModDiagnostic{
                DiagnosticSeverity::error, prototype.source, "prototype_registry.unknown_kind",
                "unknown prototype kind: " + prototype.kind});
            continue;
        }

        if (!by_id_.emplace(prototype.id.value(), index).second) {
            result.diagnostics.push_back(ModDiagnostic{
                DiagnosticSeverity::error, prototype.source, "prototype_registry.duplicate_id",
                "duplicate prototype id: " + prototype.id.value()});
            continue;
        }

        by_kind_[prototype.kind].push_back(index);
    }

    return result;
}

const GenericPrototype* PrototypeRegistry::find(const core::PrototypeId& id) const noexcept {
    const auto found = by_id_.find(id.value());
    if (found == by_id_.end()) {
        return nullptr;
    }
    return &prototypes_[found->second];
}

bool PrototypeRegistry::contains(const core::PrototypeId& id) const noexcept {
    return find(id) != nullptr;
}

std::size_t PrototypeRegistry::size() const noexcept {
    return by_id_.size();
}

std::size_t PrototypeRegistry::count_kind(std::string_view kind) const noexcept {
    const auto found = by_kind_.find(std::string(kind));
    return found == by_kind_.end() ? 0 : found->second.size();
}

std::vector<const GenericPrototype*>
PrototypeRegistry::prototypes_of_kind(std::string_view kind) const {
    std::vector<const GenericPrototype*> result;
    const auto found = by_kind_.find(std::string(kind));
    if (found == by_kind_.end()) {
        return result;
    }

    result.reserve(found->second.size());
    for (const auto index : found->second) {
        result.push_back(&prototypes_[index]);
    }
    return result;
}

core::Status PrototypeRegistry::require(const core::PrototypeId& id) const {
    if (!id.is_valid()) {
        return core::Status::failure("prototype_registry.invalid_reference",
                                     "prototype reference id is invalid");
    }
    if (!contains(id)) {
        return core::Status::failure("prototype_registry.missing_reference",
                                     "prototype reference does not exist: " + id.value());
    }
    return core::Status::ok();
}

core::Status PrototypeRegistry::require_kind(const core::PrototypeId& id,
                                             std::string_view expected_kind) const {
    auto status = require(id);
    if (!status) {
        return status;
    }

    const auto* prototype = find(id);
    if (prototype == nullptr || prototype->kind != expected_kind) {
        return core::Status::failure("prototype_registry.kind_mismatch",
                                     "prototype " + id.value() + " is not expected kind " +
                                         std::string(expected_kind));
    }

    return core::Status::ok();
}

} // namespace heartstead::modding
