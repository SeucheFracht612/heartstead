#include "engine/world/voxels/voxel_storage_experiment.hpp"

#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace {

namespace benchmark = heartstead::world::benchmark;
namespace world = heartstead::world;

constexpr std::array all_corpus_kinds{
    benchmark::VoxelCorpusKind::empty,           benchmark::VoxelCorpusKind::uniform_solid,
    benchmark::VoxelCorpusKind::layered_terrain, benchmark::VoxelCorpusKind::sparse_caves,
    benchmark::VoxelCorpusKind::lit_settlement,  benchmark::VoxelCorpusKind::checkerboard,
    benchmark::VoxelCorpusKind::high_entropy,
};

[[nodiscard]] std::uint64_t dense_type_checksum(std::span<const world::VoxelCell> cells) {
    std::uint64_t result = 0;
    for (const auto cell : cells) {
        result = (result * 1'099'511'628'211ULL) ^ cell.type;
    }
    return result;
}

void test_corpora_are_deterministic_and_round_trip() {
    for (const auto edge : {std::uint16_t{16}, std::uint16_t{32}}) {
        for (const auto kind : all_corpus_kinds) {
            const benchmark::VoxelCorpusConfig config{kind, edge, 32, 0x5eedULL};
            auto generated = benchmark::generate_voxel_storage_corpus(config);
            auto repeated = benchmark::generate_voxel_storage_corpus(config);
            assert(generated);
            assert(repeated);
            assert(generated.value().validate());
            assert(generated.value().cells == repeated.value().cells);
            assert(benchmark::voxel_corpus_name(kind) != "unknown");

            const auto& corpus = generated.value();
            const auto corpus_stats = corpus.stats();
            const auto expected_count = static_cast<std::size_t>(edge) * edge * edge;
            assert(corpus_stats.cell_count == expected_count);
            assert(corpus_stats.unique_cell_count > 0);
            assert(corpus_stats.unique_block_value_count > 0);

            auto split = benchmark::SplitVoxelSectionExperiment::encode(corpus.cells, edge);
            assert(split);
            assert(split.value().validate());
            assert(split.value().decode() == corpus.cells);
            assert(split.value().scan_type_checksum() == dense_type_checksum(corpus.cells));
            const auto split_stats = split.value().stats();
            assert(split_stats.cell_count == expected_count);
            assert(split_stats.payload_bytes == expected_count * 9U);
            assert(split_stats.allocated_bytes >= split_stats.payload_bytes);

            auto packed =
                benchmark::PalettePackedVoxelSectionExperiment::encode(corpus.cells, edge);
            assert(packed);
            assert(packed.value().validate());
            assert(packed.value().decode() == corpus.cells);
            assert(packed.value().scan_type_checksum() == dense_type_checksum(corpus.cells));
            const auto packed_stats = packed.value().stats();
            assert(packed_stats.cell_count == expected_count);
            if (packed_stats.dense_blocks) {
                assert(kind == benchmark::VoxelCorpusKind::high_entropy);
                assert(packed_stats.palette_size == 0);
                assert(packed_stats.packed_index_bytes == 0);
                assert(packed_stats.bits_per_index == 0);
            } else {
                assert(packed_stats.palette_size == corpus_stats.unique_block_value_count);
            }
            assert(packed_stats.metadata_entry_count == corpus_stats.metadata_cell_count);
            assert(packed_stats.allocated_bytes >= packed_stats.payload_bytes);

            if (kind == benchmark::VoxelCorpusKind::empty ||
                kind == benchmark::VoxelCorpusKind::uniform_solid ||
                kind == benchmark::VoxelCorpusKind::layered_terrain) {
                assert(packed_stats.payload_bytes < corpus.cells.size() * sizeof(world::VoxelCell));
            }
        }
    }

    const benchmark::VoxelCorpusConfig first_config{benchmark::VoxelCorpusKind::high_entropy, 16,
                                                    32, 1};
    auto first = benchmark::generate_voxel_storage_corpus(first_config);
    auto second_config = first_config;
    second_config.seed = 2;
    auto second = benchmark::generate_voxel_storage_corpus(second_config);
    assert(first);
    assert(second);
    assert(first.value().cells != second.value().cells);
}

void test_corpus_statistics_describe_content() {
    auto empty =
        benchmark::generate_voxel_storage_corpus({benchmark::VoxelCorpusKind::empty, 16, 8, 11});
    assert(empty);
    const auto empty_stats = empty.value().stats();
    assert(empty_stats.non_air_count == 0);
    assert(empty_stats.unique_cell_count == 1);
    assert(empty_stats.unique_block_value_count == 1);
    assert(empty_stats.metadata_cell_count == 0);

    auto solid = benchmark::generate_voxel_storage_corpus(
        {benchmark::VoxelCorpusKind::uniform_solid, 16, 8, 11});
    assert(solid);
    const auto solid_stats = solid.value().stats();
    assert(solid_stats.non_air_count == solid_stats.cell_count);
    assert(solid_stats.unique_cell_count == 1);
    assert(solid_stats.unique_block_value_count == 1);
    assert(solid_stats.metadata_cell_count == 0);

    auto entropy = benchmark::generate_voxel_storage_corpus(
        {benchmark::VoxelCorpusKind::high_entropy, 32, 32, 11});
    assert(entropy);
    const auto entropy_stats = entropy.value().stats();
    assert(entropy_stats.unique_cell_count > 1'000);
    assert(entropy_stats.unique_block_value_count > 1'000);
    assert(entropy_stats.metadata_cell_count > 0);
}

void test_palette_growth_repacking_and_channel_promotion() {
    constexpr std::uint16_t edge = 4;
    std::vector<world::VoxelCell> source(static_cast<std::size_t>(edge) * edge * edge);
    auto packed = benchmark::PalettePackedVoxelSectionExperiment::encode(source, edge);
    assert(packed);
    assert(packed.value().validate());
    assert(packed.value().stats().palette_size == 1);
    assert(packed.value().stats().bits_per_index == 0);
    assert(packed.value().stats().uniform_light);

    const world::VoxelCell first{1, 17, 3, 99};
    assert(packed.value().set(0, first));
    assert(packed.value().cell(0) == first);
    assert(packed.value().stats().palette_size == 2);
    assert(packed.value().stats().bits_per_index == 1);
    assert(!packed.value().stats().uniform_light);
    assert(packed.value().stats().metadata_entry_count == 1);

    const world::VoxelCell second{2, 0, 4, 0};
    const world::VoxelCell third{3, 0, 5, 0};
    const world::VoxelCell fourth{4, 0, 6, 0};
    assert(packed.value().set(1, second));
    assert(packed.value().stats().palette_size == 3);
    assert(packed.value().stats().bits_per_index == 2);
    assert(packed.value().set(2, third));
    assert(packed.value().stats().palette_size == 4);
    assert(packed.value().stats().bits_per_index == 2);
    assert(packed.value().set(3, fourth));
    assert(packed.value().stats().palette_size == 5);
    assert(packed.value().stats().bits_per_index == 3);
    assert(packed.value().cell(0) == first);
    assert(packed.value().cell(1) == second);
    assert(packed.value().cell(2) == third);
    assert(packed.value().cell(3) == fourth);
    assert(packed.value().validate());

    assert(packed.value().set(0, world::VoxelCell::air()));
    assert(packed.value().cell(0) == world::VoxelCell::air());
    assert(packed.value().stats().metadata_entry_count == 0);
    assert(packed.value().validate());
    assert(!packed.value().set(source.size(), world::VoxelCell::air()));

    auto split = benchmark::SplitVoxelSectionExperiment::encode(source, edge);
    assert(split);
    assert(split.value().set(0, first));
    assert(split.value().cell(0) == first);
    assert(split.value().validate());
    assert(!split.value().set(source.size(), first));
}

void test_packed_indices_cross_machine_word_boundaries() {
    constexpr std::uint16_t edge = 4;
    std::vector<world::VoxelCell> source(static_cast<std::size_t>(edge) * edge * edge);
    for (std::size_t index = 0; index < source.size(); ++index) {
        source[index].type = static_cast<std::uint16_t>((index % 17U) + 1U);
        source[index].state_bits = static_cast<std::uint16_t>(index % 17U);
    }
    auto packed = benchmark::PalettePackedVoxelSectionExperiment::encode(source, edge);
    assert(packed);
    assert(packed.value().stats().palette_size == 17);
    assert(packed.value().stats().bits_per_index == 5);
    assert(packed.value().validate());
    assert(packed.value().decode() == source);
}

void test_adaptive_block_storage_falls_back_to_split_dense() {
    for (const auto edge : {std::uint16_t{16}, std::uint16_t{32}}) {
        auto generated = benchmark::generate_voxel_storage_corpus(
            {benchmark::VoxelCorpusKind::high_entropy, edge, 32, 0x5eedULL});
        assert(generated);

        auto adaptive =
            benchmark::PalettePackedVoxelSectionExperiment::encode(generated.value().cells, edge);
        auto palette_only = benchmark::PalettePackedVoxelSectionExperiment::encode(
            generated.value().cells, edge, benchmark::ExperimentalBlockStoragePolicy::palette_only);
        assert(adaptive);
        assert(palette_only);
        assert(adaptive.value().validate());
        assert(palette_only.value().validate());
        assert(adaptive.value().stats().dense_blocks);
        assert(!palette_only.value().stats().dense_blocks);
        assert(adaptive.value().stats().palette_size == 0);
        assert(palette_only.value().stats().palette_size ==
               generated.value().stats().unique_block_value_count);
        assert(adaptive.value().stats().payload_bytes < palette_only.value().stats().payload_bytes);
        assert(adaptive.value().decode() == generated.value().cells);
        assert(palette_only.value().decode() == generated.value().cells);
    }
}

void test_edits_promote_oversized_palette_to_split_dense() {
    constexpr std::uint16_t edge = 4;
    std::vector<world::VoxelCell> oracle(static_cast<std::size_t>(edge) * edge * edge);
    auto adaptive = benchmark::PalettePackedVoxelSectionExperiment::encode(oracle, edge);
    assert(adaptive);

    for (std::uint16_t unique = 1; unique <= 51; ++unique) {
        const world::VoxelCell value{unique, static_cast<std::uint8_t>(unique), unique,
                                     unique == 1 ? 99U : 0U};
        const auto index = static_cast<std::size_t>(unique - 1U);
        oracle[index] = value;
        assert(adaptive.value().set(index, value));
    }
    assert(!adaptive.value().stats().dense_blocks);
    assert(adaptive.value().stats().palette_size == 52);

    const world::VoxelCell crossing_value{52, 77, 52, 1234};
    oracle[51] = crossing_value;
    assert(adaptive.value().set(51, crossing_value));
    assert(adaptive.value().stats().dense_blocks);
    assert(adaptive.value().stats().palette_size == 0);
    assert(adaptive.value().stats().packed_index_bytes == 0);
    assert(adaptive.value().stats().bits_per_index == 0);
    assert(adaptive.value().validate());
    assert(adaptive.value().decode() == oracle);

    const world::VoxelCell post_promotion{60'000, 201, 65'000, 0};
    oracle.back() = post_promotion;
    assert(adaptive.value().set(oracle.size() - 1U, post_promotion));
    assert(adaptive.value().validate());
    assert(adaptive.value().decode() == oracle);
}

void test_every_supported_palette_width_round_trips() {
    constexpr std::uint16_t edge = 32;
    constexpr std::array<std::size_t, 17> palette_sizes{
        1, 2, 3, 5, 9, 17, 33, 65, 129, 257, 513, 1'025, 2'049, 4'097, 8'193, 16'385, 32'768,
    };
    std::vector<world::VoxelCell> source(static_cast<std::size_t>(edge) * edge * edge);
    for (const auto palette_size : palette_sizes) {
        for (std::size_t index = 0; index < source.size(); ++index) {
            source[index] =
                world::VoxelCell{static_cast<std::uint16_t>((index % palette_size) + 1U), 0, 0, 0};
        }
        auto packed = benchmark::PalettePackedVoxelSectionExperiment::encode(
            source, edge, benchmark::ExperimentalBlockStoragePolicy::palette_only);
        assert(packed);
        const auto expected_width =
            palette_size == 1 ? std::uint8_t{0}
                              : static_cast<std::uint8_t>(std::bit_width(palette_size - 1U));
        assert(packed.value().stats().palette_size == palette_size);
        assert(packed.value().stats().bits_per_index == expected_width);
        assert(packed.value().validate());
        assert(packed.value().decode() == source);
    }
}

void test_mixed_edits_match_a_dense_oracle() {
    constexpr std::uint16_t edge = 16;
    std::vector<world::VoxelCell> oracle(static_cast<std::size_t>(edge) * edge * edge);
    auto packed = benchmark::PalettePackedVoxelSectionExperiment::encode(oracle, edge);
    assert(packed);

    std::uint64_t random = 0xc0ffeeULL;
    for (std::size_t edit = 0; edit < 2'000; ++edit) {
        random = random * 6'364'136'223'846'793'005ULL + 1'442'695'040'888'963'407ULL;
        const auto index = static_cast<std::size_t>(random % oracle.size());
        world::VoxelCell value;
        if (edit % 11U != 0) {
            value.type = static_cast<std::uint16_t>((random % 300U) + 1U);
            value.state_bits = static_cast<std::uint16_t>((random >> 17U) & 0x03ffU);
            value.light = static_cast<std::uint8_t>(random >> 41U);
            if (edit % 7U == 0) {
                value.metadata_handle = static_cast<std::uint32_t>((random >> 29U) | 1U);
            }
        }
        oracle[index] = value;
        assert(packed.value().set(index, value));
        assert(packed.value().cell(index) == value);
        if (edit % 127U == 0) {
            assert(packed.value().validate());
            assert(packed.value().decode() == oracle);
        }
    }
    assert(packed.value().validate());
    assert(packed.value().decode() == oracle);
    assert(packed.value().scan_type_checksum() == dense_type_checksum(oracle));
}

void test_invalid_inputs_fail_closed() {
    benchmark::VoxelCorpusConfig config;
    config.edge_length = 0;
    assert(!config.validate());
    config.edge_length = 33;
    assert(!config.validate());
    config.edge_length = 16;
    config.material_count = 0;
    assert(!config.validate());
    config.material_count = std::numeric_limits<std::uint16_t>::max();
    assert(!config.validate());
    config.material_count = 8;
    config.kind = static_cast<benchmark::VoxelCorpusKind>(255);
    assert(!config.validate());
    assert(benchmark::voxel_corpus_name(config.kind) == "unknown");
    assert(!benchmark::generate_voxel_storage_corpus(config));

    benchmark::VoxelStorageCorpus mismatched;
    mismatched.config = {benchmark::VoxelCorpusKind::empty, 2, 8, 1};
    mismatched.cells.resize(1);
    assert(!mismatched.validate());

    const std::array one_cell{world::VoxelCell::air()};
    assert(!benchmark::SplitVoxelSectionExperiment::encode(one_cell, 2));
    assert(!benchmark::PalettePackedVoxelSectionExperiment::encode(one_cell, 2));
    assert(!benchmark::SplitVoxelSectionExperiment::encode(one_cell, 0));
    assert(!benchmark::PalettePackedVoxelSectionExperiment::encode(one_cell, 0));
    assert(!benchmark::PalettePackedVoxelSectionExperiment::encode(
        one_cell, 1, static_cast<benchmark::ExperimentalBlockStoragePolicy>(255)));
}

} // namespace

int main() {
    test_corpora_are_deterministic_and_round_trip();
    test_corpus_statistics_describe_content();
    test_palette_growth_repacking_and_channel_promotion();
    test_packed_indices_cross_machine_word_boundaries();
    test_adaptive_block_storage_falls_back_to_split_dense();
    test_edits_promote_oversized_palette_to_split_dense();
    test_every_supported_palette_width_round_trips();
    test_mixed_edits_match_a_dense_oracle();
    test_invalid_inputs_fail_closed();
    return 0;
}
