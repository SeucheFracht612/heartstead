#pragma once

#include "engine/core/ids.hpp"
#include "engine/core/result.hpp"
#include "engine/math/vector.hpp"
#include "engine/platform/platform.hpp"
#include "engine/renderer/ui/ui_renderer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace heartstead::ui {

struct WidgetIdTag;
using WidgetId = core::StrongU64Id<WidgetIdTag>;

[[nodiscard]] WidgetId widget_id(std::string_view stable_name) noexcept;

struct UiRect {
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;

    [[nodiscard]] bool is_valid() const noexcept;
    [[nodiscard]] bool contains(math::Vec2f point) const noexcept;
    [[nodiscard]] UiRect inset(float left, float top, float right, float bottom) const noexcept;

    friend bool operator==(const UiRect&, const UiRect&) = default;
};

struct UiInsets {
    float left = 0.0F;
    float top = 0.0F;
    float right = 0.0F;
    float bottom = 0.0F;
};

enum class UiSizeMode : std::uint8_t {
    pixels,
    content,
    fill,
};

struct UiSize {
    UiSizeMode mode = UiSizeMode::content;
    float value = 0.0F;

    [[nodiscard]] static constexpr UiSize pixels(float pixels) noexcept {
        return {UiSizeMode::pixels, pixels};
    }
    [[nodiscard]] static constexpr UiSize content() noexcept {
        return {UiSizeMode::content, 0.0F};
    }
    [[nodiscard]] static constexpr UiSize fill(float weight = 1.0F) noexcept {
        return {UiSizeMode::fill, weight};
    }
};

enum class UiLayoutMode : std::uint8_t {
    row,
    column,
    grid,
    overlay,
};

enum class UiAlignment : std::uint8_t {
    start,
    center,
    end,
    stretch,
};

struct UiLayoutStyle {
    UiLayoutMode mode = UiLayoutMode::overlay;
    UiSize width = UiSize::content();
    UiSize height = UiSize::content();
    float minimum_width = 0.0F;
    float minimum_height = 0.0F;
    float maximum_width = 100'000.0F;
    float maximum_height = 100'000.0F;
    UiInsets padding{};
    float gap = 0.0F;
    std::uint32_t grid_columns = 1;
    float grid_cell_height = 48.0F;
    UiAlignment horizontal_alignment = UiAlignment::stretch;
    UiAlignment vertical_alignment = UiAlignment::stretch;
    bool clip_children = false;
};

enum class WidgetKind : std::uint8_t {
    panel,
    label,
    image,
    button,
    toggle,
    slider,
    text_input,
    scroll_area,
    tooltip,
    grid_slot,
};

struct UiAtlasRegion {
    std::string name;
    std::uint16_t texture_layer = 0;
    math::Vec2f uv_minimum{};
    math::Vec2f uv_maximum{1.0F, 1.0F};
    math::Vec2f source_size_pixels{1.0F, 1.0F};
};

struct UiNineSlice {
    std::string name;
    std::string atlas_region;
    UiInsets border_pixels{};
    std::array<float, 4> tint{1.0F, 1.0F, 1.0F, 1.0F};
    std::array<float, 4> text_color{0.96F, 0.92F, 0.82F, 1.0F};
};

struct UiSkinTheme {
    std::array<float, 4> shell_background{0.025F, 0.045F, 0.065F, 0.94F};
    std::array<float, 4> text_color{0.96F, 0.92F, 0.82F, 1.0F};
};

class UiSkin {
  public:
    [[nodiscard]] core::Status add_region(UiAtlasRegion region);
    [[nodiscard]] core::Status add_nine_slice(UiNineSlice slice);
    [[nodiscard]] const UiAtlasRegion* find_region(std::string_view name) const noexcept;
    [[nodiscard]] const UiNineSlice* find_nine_slice(std::string_view name) const noexcept;
    [[nodiscard]] const UiSkinTheme& theme() const noexcept;
    [[nodiscard]] core::Status set_theme(UiSkinTheme theme);
    [[nodiscard]] static UiSkin storybook_default();

  private:
    std::vector<UiAtlasRegion> regions_;
    std::vector<UiNineSlice> nine_slices_;
    UiSkinTheme theme_;
};

struct WidgetDesc {
    WidgetId id;
    WidgetId parent;
    WidgetKind kind = WidgetKind::panel;
    UiLayoutStyle layout{};
    std::string text;
    std::string accessibility_label;
    std::string tooltip;
    std::string atlas_region;
    std::string nine_slice;
    std::string drag_payload;
    std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F};
    std::array<float, 4> text_color{0.96F, 0.92F, 0.82F, 1.0F};
    float glyph_size_pixels = 14.0F;
    float value = 0.0F;
    float minimum_value = 0.0F;
    float maximum_value = 1.0F;
    bool visible = true;
    bool enabled = true;
    bool focusable = false;
    bool pointer_events = false;
    bool checked = false;
    bool draggable = false;
    bool drop_target = false;
    bool blocks_gameplay = false;
};

enum class UiNavigation : std::uint8_t {
    none,
    next,
    previous,
    left,
    right,
    up,
    down,
    activate,
    cancel,
};

struct UiInputFrame {
    math::Vec2f pointer{};
    math::Vec2f pointer_delta{};
    float wheel_delta = 0.0F;
    bool pointer_inside = false;
    bool primary_down = false;
    bool primary_pressed = false;
    bool primary_released = false;
    bool backspace_pressed = false;
    bool delete_pressed = false;
    UiNavigation navigation = UiNavigation::none;
    std::string text;

    [[nodiscard]] static UiInputFrame
    from_platform(const platform::WindowInputSnapshot& snapshot) noexcept;
};

enum class UiEventKind : std::uint8_t {
    clicked,
    toggled,
    value_changed,
    value_committed,
    text_changed,
    focus_changed,
    drag_started,
    dropped,
    cancelled,
};

struct UiEvent {
    UiEventKind kind = UiEventKind::clicked;
    WidgetId target;
    WidgetId source;
    std::string payload;
    std::string text;
    float value = 0.0F;
    bool checked = false;
};

struct UiInputConsumption {
    bool pointer = false;
    bool keyboard = false;
    bool text = false;
    bool gamepad = false;
    bool blocks_gameplay = false;
};

struct UiRouteResult {
    UiInputConsumption consumed;
    std::vector<UiEvent> events;
};

struct UiLayoutStats {
    std::uint32_t widget_count = 0;
    std::uint32_t visible_widget_count = 0;
    std::uint32_t clipped_widget_count = 0;
    std::uint32_t focusable_widget_count = 0;
    std::uint32_t layout_passes = 0;
};

struct UiPaintStats {
    std::uint32_t submitted_quads = 0;
    std::uint32_t submitted_glyphs = 0;
    std::uint32_t nine_slice_quads = 0;
    std::uint32_t clipped_widgets = 0;
};

class WidgetTree {
  public:
    explicit WidgetTree(UiSkin skin = UiSkin::storybook_default());

    [[nodiscard]] core::Status add(WidgetDesc desc);
    [[nodiscard]] core::Status update(WidgetDesc desc);
    [[nodiscard]] core::Status remove(WidgetId id);
    void clear() noexcept;

    [[nodiscard]] core::Status layout(math::Vec2f viewport_pixels, float dpi_scale = 1.0F);
    [[nodiscard]] UiRouteResult route_input(const UiInputFrame& input);
    [[nodiscard]] core::Result<UiPaintStats> paint(renderer::UiRenderer& renderer) const;

    [[nodiscard]] WidgetDesc* find(WidgetId id) noexcept;
    [[nodiscard]] const WidgetDesc* find(WidgetId id) const noexcept;
    [[nodiscard]] std::optional<UiRect> rect(WidgetId id) const noexcept;
    [[nodiscard]] WidgetId focused_widget() const noexcept;
    [[nodiscard]] WidgetId captured_widget() const noexcept;
    [[nodiscard]] WidgetId hovered_widget() const noexcept;
    [[nodiscard]] bool dragging() const noexcept;
    [[nodiscard]] std::string_view drag_payload() const noexcept;
    [[nodiscard]] float scroll_offset(WidgetId id) const noexcept;
    [[nodiscard]] std::span<const WidgetId> paint_order() const noexcept;
    [[nodiscard]] const UiLayoutStats& layout_stats() const noexcept;

    void set_visible(WidgetId id, bool visible) noexcept;
    void set_focus(WidgetId id) noexcept;

  private:
    struct Node {
        WidgetDesc desc;
        UiRect rect{};
        UiRect clip{};
        std::vector<WidgetId> children;
        float scroll_offset_y = 0.0F;
        float scroll_extent_y = 0.0F;
    };

    [[nodiscard]] Node* node(WidgetId id) noexcept;
    [[nodiscard]] const Node* node(WidgetId id) const noexcept;
    [[nodiscard]] core::Status validate_desc(const WidgetDesc& desc, bool updating) const;
    [[nodiscard]] math::Vec2f measure(const Node& node) const noexcept;
    void layout_node(Node& node, UiRect bounds, UiRect inherited_clip);
    void rebuild_paint_order();
    void append_paint_order(WidgetId id);
    [[nodiscard]] WidgetId hit_test(math::Vec2f point,
                                    bool drop_targets_only = false) const noexcept;
    [[nodiscard]] std::vector<WidgetId> focus_order() const;
    void navigate_focus(UiNavigation navigation, std::vector<UiEvent>& events);
    void activate(WidgetId id, std::vector<UiEvent>& events);
    [[nodiscard]] bool subtree_blocks_gameplay(WidgetId id) const noexcept;

    UiSkin skin_;
    std::vector<Node> nodes_;
    std::unordered_map<std::uint64_t, std::size_t> indices_;
    std::vector<WidgetId> roots_;
    std::vector<WidgetId> paint_order_;
    WidgetId focused_;
    WidgetId captured_;
    WidgetId hovered_;
    WidgetId pressed_;
    WidgetId drag_source_;
    std::string drag_payload_;
    math::Vec2f press_position_{};
    math::Vec2f pointer_position_{};
    math::Vec2f viewport_{};
    float dpi_scale_ = 1.0F;
    UiLayoutStats layout_stats_{};
};

[[nodiscard]] std::string_view widget_kind_name(WidgetKind kind) noexcept;

} // namespace heartstead::ui
