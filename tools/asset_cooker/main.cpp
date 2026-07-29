#include "engine/assets/asset_catalog.hpp"
#include "engine/assets/asset_cooker.hpp"
#include "engine/assets/cooked_asset_manifest.hpp"
#include "engine/assets/cooked_asset_store.hpp"
#include "engine/assets/resource_pack.hpp"
#include "engine/assets/virtual_file_system.hpp"
#include "engine/content/content_validation.hpp"
#include "engine/core/logging.hpp"
#include "engine/core/process_entry.hpp"
#include "engine/debug/inspection.hpp"
#include "engine/modding/mod_discovery.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {

void log_diagnostic(const heartstead::modding::ModDiagnostic& diagnostic) {
    using heartstead::core::LogLevel;
    using heartstead::modding::DiagnosticSeverity;

    const auto level = diagnostic.severity == DiagnosticSeverity::error     ? LogLevel::error
                       : diagnostic.severity == DiagnosticSeverity::warning ? LogLevel::warning
                                                                            : LogLevel::info;

    heartstead::core::log(level, diagnostic.code + ": " + diagnostic.message + " (" +
                                     diagnostic.source.generic_string() + ")");
}

bool parse_backend(std::string_view value, heartstead::assets::AssetCookBackend& backend) {
    if (value == "development" || value == "development_passthrough") {
        backend = heartstead::assets::AssetCookBackend::development_passthrough;
        return true;
    }
    if (value == "production" || value == "production_converters") {
        backend = heartstead::assets::AssetCookBackend::production_converters;
        return true;
    }
    return false;
}

void print_usage(const char* executable, std::ostream& output) {
    output << "usage: " << executable
           << " [source_root] [asset_manifest_path] [development|production]"
              " [--only LOGICAL_ID] [--entity-visuals] [--inspect]\n"
           << "       " << executable << " --inspect-store <cooked_root> [asset_manifest.txt]\n";
}

int inspect_store(const std::filesystem::path& root,
                  const std::filesystem::path& manifest_relative_path) {
    auto store = heartstead::assets::CookedAssetStore::load(root, manifest_relative_path);
    if (!store) {
        heartstead::core::log(heartstead::core::LogLevel::error, store.error().message);
        return 1;
    }

    const auto inspection = heartstead::debug::Inspector::inspect(store.value());
    std::cout << heartstead::debug::Inspector::render_text(inspection);
    return inspection.has_errors() ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    return heartstead::core::run_process_entry(argv[0], [argc, argv] {
        using namespace heartstead;

        if (argc == 2 &&
            (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h")) {
            print_usage(argv[0], std::cout);
            return 0;
        }
        if (argc >= 2 && std::string_view(argv[1]) == "--inspect-store") {
            if (argc < 3 || argc > 4) {
                print_usage(argv[0], std::cerr);
                return 2;
            }
            const std::filesystem::path manifest_relative_path =
                argc >= 4 ? std::filesystem::path(argv[3])
                          : std::filesystem::path("asset_manifest.txt");
            return inspect_store(argv[2], manifest_relative_path);
        }

        auto source_root = std::filesystem::path(HEARTSTEAD_SOURCE_ROOT);
        std::filesystem::path output_path;
        assets::AssetCookBackend cook_backend = assets::AssetCookBackend::development_passthrough;
        bool inspect_after_cook = false;
        bool include_entity_visuals = false;
        std::vector<std::string> only_logical_ids;
        auto positional_count = 0;
        for (int index = 1; index < argc; ++index) {
            const auto argument = std::string_view(argv[index]);
            if (argument == "--inspect") {
                inspect_after_cook = true;
                continue;
            }
            if (argument == "--entity-visuals") {
                include_entity_visuals = true;
                continue;
            }
            if (argument == "--only") {
                if (index + 1 >= argc || std::string_view(argv[index + 1]).starts_with("--")) {
                    print_usage(argv[0], std::cerr);
                    return 2;
                }
                only_logical_ids.emplace_back(argv[++index]);
                continue;
            }
            if (argument.starts_with("--")) {
                print_usage(argv[0], std::cerr);
                return 2;
            }

            switch (positional_count) {
            case 0:
                source_root = std::filesystem::path(argv[index]);
                break;
            case 1:
                output_path = std::filesystem::path(argv[index]);
                break;
            case 2:
                if (!parse_backend(argument, cook_backend)) {
                    core::log(core::LogLevel::error,
                              "unknown asset cook backend: " + std::string(argument) +
                                  " (expected development or production)");
                    return 2;
                }
                break;
            default:
                print_usage(argv[0], std::cerr);
                return 2;
            }
            ++positional_count;
        }
        if (output_path.empty()) {
            output_path = source_root / "build" / "cooked_assets" / "asset_manifest.txt";
        }

        const auto mods_root = source_root / "mods";
        const auto resource_packs_root = source_root / "resource_packs";

        assets::AssetCatalog catalog;

        modding::ModDiscoverer mod_discoverer;
        auto mods = mod_discoverer.discover(mods_root);
        for (const auto& diagnostic : mods.diagnostics) {
            log_diagnostic(diagnostic);
        }
        if (mods.has_errors()) {
            return 1;
        }

        for (const auto& mod : mods.mods) {
            const auto assets_root = mod.root / "assets";
            auto indexed = assets::AssetCatalogBuilder::index_directory(
                catalog, assets_root, mod.id, assets::AssetSourceKind::mod, mod.id, 0);
            for (const auto& diagnostic : indexed.diagnostics) {
                log_diagnostic(diagnostic);
            }
            if (indexed.has_errors()) {
                return 1;
            }
        }

        assets::ResourcePackDiscoverer pack_discoverer;
        auto packs = pack_discoverer.discover(resource_packs_root);
        for (const auto& diagnostic : packs.diagnostics) {
            log_diagnostic(diagnostic);
        }
        if (packs.has_errors()) {
            return 1;
        }

        auto pack_plan = assets::ResourcePackLoadPlanner::plan(packs.packs);
        if (!pack_plan) {
            core::log(core::LogLevel::error, pack_plan.error().message);
            return 1;
        }

        for (const auto& entry : pack_plan.value().entries) {
            auto indexed = assets::ResourcePackPolicy::index_assets(catalog, entry.manifest,
                                                                    entry.asset_priority);
            for (const auto& diagnostic : indexed.diagnostics) {
                log_diagnostic(diagnostic);
            }
            if (indexed.has_errors()) {
                return 1;
            }
        }

        if (cook_backend == assets::AssetCookBackend::production_converters) {
            auto dependency_status = assets::discover_asset_dependencies(catalog);
            if (!dependency_status) {
                core::log(core::LogLevel::error, dependency_status.error().message);
                return 1;
            }
        }
        if (include_entity_visuals) {
            const auto content_report = content::ContentValidation::validate(source_root);
            if (content_report.has_errors()) {
                for (const auto& diagnostic : content_report.diagnostics) {
                    if (diagnostic.severity == modding::DiagnosticSeverity::error) {
                        log_diagnostic(diagnostic);
                    }
                }
                return 1;
            }
            for (const auto& visual : content_report.visual_definitions.definitions()) {
                only_logical_ids.push_back(visual.model_asset);
            }
        }
        if (include_entity_visuals || !only_logical_ids.empty()) {
            assets::AssetCatalog filtered;
            std::vector<std::string> pending;
            pending.reserve(only_logical_ids.size());
            for (const auto& logical_id : only_logical_ids) {
                const auto* selected = catalog.find_active(logical_id);
                if (selected == nullptr) {
                    core::log(core::LogLevel::error, "selected asset is not active: " + logical_id);
                    return 1;
                }
                pending.push_back(selected->logical_id);
            }
            std::unordered_set<std::string> included;
            while (!pending.empty()) {
                auto logical_id = std::move(pending.back());
                pending.pop_back();
                if (!included.insert(logical_id).second) {
                    continue;
                }
                const auto* record = catalog.find_active(logical_id);
                if (record == nullptr) {
                    core::log(core::LogLevel::error,
                              "selected asset dependency is not active: " + logical_id);
                    return 1;
                }
                auto status = filtered.add(*record);
                if (!status) {
                    core::log(core::LogLevel::error, status.error().message);
                    return 1;
                }
                for (const auto& dependency : record->dependencies) {
                    pending.push_back(assets::asset_logical_id(dependency));
                }
            }
            catalog = std::move(filtered);
        }

        assets::AssetCookConfig cook_config;
        cook_config.backend = cook_backend;
        cook_config.output_root = output_path.parent_path();
        if (cook_config.output_root.empty()) {
            cook_config.output_root = ".";
        }
        cook_config.manifest_relative_path = output_path.filename();
        const auto backend_info = assets::asset_cook_backend_info(cook_config.backend);
        core::log(core::LogLevel::info, "Asset cook backend: " + std::string(backend_info.name) +
                                            " (" + std::string(backend_info.status) + ")");

        auto cooked = assets::AssetCooker::cook(catalog, cook_config);
        if (!cooked) {
            core::log(core::LogLevel::error, cooked.error().message);
            return 1;
        }
        auto store = assets::CookedAssetStore::load(cook_config.output_root,
                                                    cook_config.manifest_relative_path);
        if (!store) {
            core::log(core::LogLevel::error, store.error().message);
            return 1;
        }

        core::log(core::LogLevel::info,
                  "Cooked assets: " + std::to_string(cooked.value().cooked_file_count) +
                      " files, " + std::to_string(cooked.value().cooked_payload_bytes) +
                      " source bytes");
        core::log(
            core::LogLevel::info,
            "Cooked asset manifest: " + std::to_string(cooked.value().manifest.records.size()) +
                " records, " + std::to_string(cooked.value().manifest.active_count()) + " active");
        core::log(core::LogLevel::info,
                  "Verified cooked asset store: " +
                      std::to_string(store.value().manifest().records.size()) + " records");
        if (inspect_after_cook) {
            const auto inspection = debug::Inspector::inspect(store.value());
            std::cout << debug::Inspector::render_text(inspection);
            if (inspection.has_errors()) {
                return 1;
            }
        }
        core::log(core::LogLevel::info, "Wrote " + cooked.value().manifest_path.string());
        return 0;
    });
}
