#include "engine/ui/widget_tree.hpp"

#include "engine/core/hash.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <utility>

namespace heartstead::ui {

namespace {

[[nodiscard]] bool finite_nonnegative(float value) noexcept {
    return std::isfinite(value) && value >= 0.0F;
}

[[nodiscard]] bool valid_color(const std::array<float, 4>& color) noexcept {
    return std::ranges::all_of(color, [](float channel) {
        return std::isfinite(channel) && channel >= 0.0F && channel <= 1.0F;
    });
}

[[nodiscard]] UiRect intersection(UiRect left, UiRect right) noexcept {
    const auto x0 = std::max(left.x, right.x);
    const auto y0 = std::max(left.y, right.y);
    const auto x1 = std::min(left.x + left.width, right.x + right.width);
    const auto y1 = std::min(left.y + left.height, right.y + right.height);
    return {x0, y0, std::max(0.0F, x1 - x0), std::max(0.0F, y1 - y0)};
}

[[nodiscard]] float clamp_size(float value, float minimum, float maximum) noexcept {
    return std::clamp(value, minimum, maximum);
}

[[nodiscard]] float resolve_size(UiSize size, float available, float measured, float minimum,
                                 float maximum) noexcept {
    float value = measured;
    switch (size.mode) {
    case UiSizeMode::pixels:
        value = size.value;
        break;
    case UiSizeMode::content:
        value = measured;
        break;
    case UiSizeMode::fill:
        value = available;
        break;
    }
    return clamp_size(value, minimum, maximum);
}

[[nodiscard]] float resolve_scaled_size(UiSize size, float available, float measured,
                                        float minimum, float maximum, float dpi_scale) noexcept {
    const auto logical = resolve_size(size, available, measured, minimum, maximum);
    return size.mode == UiSizeMode::fill ? logical
                                        : std::min(available, logical * dpi_scale);
}

[[nodiscard]] UiRect aligned_rect(UiRect available, float width, float height,
                                  UiAlignment horizontal, UiAlignment vertical) noexcept {
    width = horizontal == UiAlignment::stretch ? available.width : std::min(width, available.width);
    height =
        vertical == UiAlignment::stretch ? available.height : std::min(height, available.height);
    auto x = available.x;
    auto y = available.y;
    if (horizontal == UiAlignment::center) {
        x += (available.width - width) * 0.5F;
    } else if (horizontal == UiAlignment::end) {
        x += available.width - width;
    }
    if (vertical == UiAlignment::center) {
        y += (available.height - height) * 0.5F;
    } else if (vertical == UiAlignment::end) {
        y += available.height - height;
    }
    return {x, y, std::max(0.0F, width), std::max(0.0F, height)};
}

[[nodiscard]] bool same_scissor(UiRect left, UiRect right) noexcept {
    constexpr float epsilon = 0.01F;
    return std::abs(left.x - right.x) < epsilon && std::abs(left.y - right.y) < epsilon &&
           std::abs(left.width - right.width) < epsilon &&
           std::abs(left.height - right.height) < epsilon;
}

[[nodiscard]] renderer::UiScissorRect to_scissor(UiRect rect) noexcept {
    const auto x = static_cast<std::uint32_t>(std::max(0.0F, std::floor(rect.x)));
    const auto y = static_cast<std::uint32_t>(std::max(0.0F, std::floor(rect.y)));
    const auto width = static_cast<std::uint32_t>(std::max(1.0F, std::ceil(rect.width)));
    const auto height = static_cast<std::uint32_t>(std::max(1.0F, std::ceil(rect.height)));
    return {x, y, width, height};
}

[[nodiscard]] bool contains_key(std::span<const platform::KeyCode> values,
                                platform::KeyCode key) noexcept {
    return std::ranges::find(values, key) != values.end();
}

[[nodiscard]] bool contains_button(std::span<const platform::MouseButton> values,
                                   platform::MouseButton button) noexcept {
    return std::ranges::find(values, button) != values.end();
}

} // namespace

WidgetId widget_id(std::string_view stable_name) noexcept {
    const auto hash = core::stable_hash64(stable_name);
    return WidgetId::from_value(hash == 0 ? 1 : hash);
}

bool UiRect::is_valid() const noexcept {
    return std::isfinite(x) && std::isfinite(y) && finite_nonnegative(width) &&
           finite_nonnegative(height);
}

bool UiRect::contains(math::Vec2f point) const noexcept {
    return is_valid() && point.x >= x && point.y >= y && point.x < x + width &&
           point.y < y + height;
}

UiRect UiRect::inset(float left, float top, float right, float bottom) const noexcept {
    return {x + left, y + top, std::max(0.0F, width - left - right),
            std::max(0.0F, height - top - bottom)};
}

core::Status UiSkin::add_region(UiAtlasRegion region) {
    if (region.name.empty() || !region.uv_minimum.is_finite() || !region.uv_maximum.is_finite() ||
        !region.source_size_pixels.is_finite() || region.source_size_pixels.x <= 0.0F ||
        region.source_size_pixels.y <= 0.0F || region.uv_minimum.x < 0.0F ||
        region.uv_minimum.y < 0.0F || region.uv_maximum.x > 1.0F || region.uv_maximum.y > 1.0F ||
        region.uv_minimum.x >= region.uv_maximum.x || region.uv_minimum.y >= region.uv_maximum.y) {
        return core::Status::failure("ui_skin.invalid_region",
                                     "atlas region requires valid name, UVs, and source size");
    }
    if (find_region(region.name) != nullptr) {
        return core::Status::failure("ui_skin.duplicate_region",
                                     "atlas region name is already registered");
    }
    regions_.push_back(std::move(region));
    return core::Status::ok();
}

core::Status UiSkin::add_nine_slice(UiNineSlice slice) {
    if (slice.name.empty() || find_region(slice.atlas_region) == nullptr ||
        !finite_nonnegative(slice.border_pixels.left) ||
        !finite_nonnegative(slice.border_pixels.top) ||
        !finite_nonnegative(slice.border_pixels.right) ||
        !finite_nonnegative(slice.border_pixels.bottom) || !valid_color(slice.tint) ||
        !valid_color(slice.text_color)) {
        return core::Status::failure(
            "ui_skin.invalid_nine_slice",
            "nine-slice requires a known atlas region and non-negative borders");
    }
    if (find_nine_slice(slice.name) != nullptr) {
        return core::Status::failure("ui_skin.duplicate_nine_slice",
                                     "nine-slice name is already registered");
    }
    nine_slices_.push_back(std::move(slice));
    return core::Status::ok();
}

const UiAtlasRegion* UiSkin::find_region(std::string_view name) const noexcept {
    const auto found = std::ranges::find_if(
        regions_, [name](const UiAtlasRegion& current) { return current.name == name; });
    return found == regions_.end() ? nullptr : &*found;
}

const UiNineSlice* UiSkin::find_nine_slice(std::string_view name) const noexcept {
    const auto found = std::ranges::find_if(
        nine_slices_, [name](const UiNineSlice& current) { return current.name == name; });
    return found == nine_slices_.end() ? nullptr : &*found;
}

const UiSkinTheme& UiSkin::theme() const noexcept {
    return theme_;
}

core::Status UiSkin::set_theme(UiSkinTheme theme) {
    if (!valid_color(theme.shell_background) || !valid_color(theme.text_color)) {
        return core::Status::failure("ui_skin.invalid_theme",
                                     "UI theme colors must contain normalized finite channels");
    }
    theme_ = theme;
    return core::Status::ok();
}

UiSkin UiSkin::storybook_default() {
    UiSkin result;
    (void)result.add_region({"solid", 0, {}, {1.0F, 1.0F}, {32.0F, 32.0F}});
    (void)result.add_nine_slice({"carved_panel",
                                 "solid",
                                 {6.0F, 6.0F, 6.0F, 6.0F},
                                 {0.25F, 0.13F, 0.045F, 0.98F}});
    (void)result.add_nine_slice({"carved_button",
                                 "solid",
                                 {4.0F, 4.0F, 4.0F, 4.0F},
                                 {0.42F, 0.23F, 0.075F, 1.0F}});
    (void)result.add_nine_slice({"carved_slot",
                                 "solid",
                                 {3.0F, 3.0F, 3.0F, 3.0F},
                                 {0.30F, 0.18F, 0.075F, 1.0F}});
    return result;
}

UiInputFrame UiInputFrame::from_platform(const platform::WindowInputSnapshot& snapshot) noexcept {
    UiInputFrame result;
    result.pointer = {static_cast<float>(snapshot.mouse.x), static_cast<float>(snapshot.mouse.y)};
    result.pointer_delta = {static_cast<float>(snapshot.mouse_delta_x),
                            static_cast<float>(snapshot.mouse_delta_y)};
    result.pointer_inside = snapshot.mouse.inside;
    result.wheel_delta = static_cast<float>(snapshot.wheel_delta_y);
    result.primary_down = contains_button(snapshot.down_mouse_buttons, platform::MouseButton::left);
    result.primary_pressed =
        contains_button(snapshot.pressed_mouse_buttons, platform::MouseButton::left);
    result.primary_released =
        contains_button(snapshot.released_mouse_buttons, platform::MouseButton::left);
    if (contains_key(snapshot.pressed_keys, platform::KeyCode::tab)) {
        result.navigation = contains_key(snapshot.down_keys, platform::KeyCode::left_shift)
                                ? UiNavigation::previous
                                : UiNavigation::next;
    } else if (contains_key(snapshot.pressed_keys, platform::KeyCode::enter) ||
               contains_key(snapshot.pressed_keys, platform::KeyCode::space)) {
        result.navigation = UiNavigation::activate;
    } else if (contains_key(snapshot.pressed_keys, platform::KeyCode::escape)) {
        result.navigation = UiNavigation::cancel;
    } else if (contains_key(snapshot.pressed_keys, platform::KeyCode::arrow_left)) {
        result.navigation = UiNavigation::left;
    } else if (contains_key(snapshot.pressed_keys, platform::KeyCode::arrow_right)) {
        result.navigation = UiNavigation::right;
    } else if (contains_key(snapshot.pressed_keys, platform::KeyCode::arrow_up)) {
        result.navigation = UiNavigation::up;
    } else if (contains_key(snapshot.pressed_keys, platform::KeyCode::arrow_down)) {
        result.navigation = UiNavigation::down;
    }
    result.backspace_pressed = contains_key(snapshot.pressed_keys, platform::KeyCode::backspace);
    result.delete_pressed = contains_key(snapshot.pressed_keys, platform::KeyCode::delete_key);
    for (const auto& text : snapshot.text) {
        result.text += text;
    }
    return result;
}

WidgetTree::WidgetTree(UiSkin skin) : skin_(std::move(skin)) {}

core::Status WidgetTree::validate_desc(const WidgetDesc& desc, bool updating) const {
    if (!desc.id.is_valid()) {
        return core::Status::failure("widget_tree.invalid_id", "widget id must be valid");
    }
    if (!updating && indices_.contains(desc.id.value())) {
        return core::Status::failure("widget_tree.duplicate_id", "widget id is already present");
    }
    if (updating && !indices_.contains(desc.id.value())) {
        return core::Status::failure("widget_tree.missing_widget", "widget id is not present");
    }
    if (desc.parent == desc.id) {
        return core::Status::failure("widget_tree.self_parent", "widget cannot parent itself");
    }
    if (desc.parent.is_valid() && !indices_.contains(desc.parent.value())) {
        return core::Status::failure("widget_tree.missing_parent", "widget parent is not present");
    }
    const auto& style = desc.layout;
    if (!finite_nonnegative(style.minimum_width) || !finite_nonnegative(style.minimum_height) ||
        !finite_nonnegative(style.maximum_width) || !finite_nonnegative(style.maximum_height) ||
        style.minimum_width > style.maximum_width || style.minimum_height > style.maximum_height ||
        !finite_nonnegative(style.gap) || style.grid_columns == 0 ||
        !finite_nonnegative(style.grid_cell_height) || !finite_nonnegative(style.padding.left) ||
        !finite_nonnegative(style.padding.top) || !finite_nonnegative(style.padding.right) ||
        !finite_nonnegative(style.padding.bottom) || !std::isfinite(desc.glyph_size_pixels) ||
        desc.glyph_size_pixels <= 0.0F || !std::isfinite(desc.value) ||
        !std::isfinite(desc.minimum_value) || !std::isfinite(desc.maximum_value) ||
        desc.minimum_value > desc.maximum_value) {
        return core::Status::failure("widget_tree.invalid_desc",
                                     "widget layout, text, or value range is invalid");
    }
    if (!desc.atlas_region.empty() && skin_.find_region(desc.atlas_region) == nullptr) {
        return core::Status::failure("widget_tree.missing_atlas_region",
                                     "widget references an unknown atlas region");
    }
    if (!desc.nine_slice.empty() && skin_.find_nine_slice(desc.nine_slice) == nullptr) {
        return core::Status::failure("widget_tree.missing_nine_slice",
                                     "widget references an unknown nine-slice");
    }
    return core::Status::ok();
}

core::Status WidgetTree::add(WidgetDesc desc) {
    auto status = validate_desc(desc, false);
    if (!status) {
        return status;
    }
    const auto id = desc.id;
    const auto parent = desc.parent;
    indices_.emplace(id.value(), nodes_.size());
    nodes_.push_back({std::move(desc), {}, {}, {}});
    if (parent.is_valid()) {
        node(parent)->children.push_back(id);
    } else {
        roots_.push_back(id);
    }
    return core::Status::ok();
}

core::Status WidgetTree::update(WidgetDesc desc) {
    auto status = validate_desc(desc, true);
    if (!status) {
        return status;
    }
    auto* current = node(desc.id);
    const auto old_parent = current->desc.parent;
    if (old_parent != desc.parent) {
        auto& old_children = old_parent.is_valid() ? node(old_parent)->children : roots_;
        std::erase(old_children, desc.id);
        auto& new_children = desc.parent.is_valid() ? node(desc.parent)->children : roots_;
        new_children.push_back(desc.id);
    }
    current->desc = std::move(desc);
    return core::Status::ok();
}

core::Status WidgetTree::remove(WidgetId id) {
    auto* target = node(id);
    if (target == nullptr) {
        return core::Status::failure("widget_tree.missing_widget", "widget id is not present");
    }
    std::vector<WidgetId> removal{id};
    for (std::size_t cursor = 0; cursor < removal.size(); ++cursor) {
        if (const auto* current = node(removal[cursor]); current != nullptr) {
            removal.insert(removal.end(), current->children.begin(), current->children.end());
        }
    }
    const auto parent = target->desc.parent;
    auto& siblings = parent.is_valid() ? node(parent)->children : roots_;
    std::erase(siblings, id);
    const auto removes = [&removal](const Node& candidate) {
        return std::ranges::find(removal, candidate.desc.id) != removal.end();
    };
    nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(), removes), nodes_.end());
    indices_.clear();
    for (std::size_t index = 0; index < nodes_.size(); ++index) {
        indices_.emplace(nodes_[index].desc.id.value(), index);
    }
    if (std::ranges::find(removal, focused_) != removal.end()) {
        focused_ = {};
    }
    if (std::ranges::find(removal, captured_) != removal.end()) {
        captured_ = {};
    }
    if (std::ranges::find(removal, hovered_) != removal.end()) {
        hovered_ = {};
    }
    if (std::ranges::find(removal, drag_source_) != removal.end()) {
        drag_source_ = {};
        drag_payload_.clear();
    }
    rebuild_paint_order();
    return core::Status::ok();
}

void WidgetTree::clear() noexcept {
    nodes_.clear();
    indices_.clear();
    roots_.clear();
    paint_order_.clear();
    focused_ = {};
    captured_ = {};
    hovered_ = {};
    pressed_ = {};
    drag_source_ = {};
    drag_payload_.clear();
    layout_stats_ = {};
}

WidgetTree::Node* WidgetTree::node(WidgetId id) noexcept {
    const auto found = indices_.find(id.value());
    return found == indices_.end() ? nullptr : &nodes_[found->second];
}

const WidgetTree::Node* WidgetTree::node(WidgetId id) const noexcept {
    const auto found = indices_.find(id.value());
    return found == indices_.end() ? nullptr : &nodes_[found->second];
}

WidgetDesc* WidgetTree::find(WidgetId id) noexcept {
    auto* found = node(id);
    return found == nullptr ? nullptr : &found->desc;
}

const WidgetDesc* WidgetTree::find(WidgetId id) const noexcept {
    const auto* found = node(id);
    return found == nullptr ? nullptr : &found->desc;
}

math::Vec2f WidgetTree::measure(const Node& current) const noexcept {
    const auto horizontal_padding =
        current.desc.layout.padding.left + current.desc.layout.padding.right;
    const auto vertical_padding =
        current.desc.layout.padding.top + current.desc.layout.padding.bottom;
    auto width = horizontal_padding;
    auto height = vertical_padding;
    if (!current.desc.text.empty()) {
        width +=
            static_cast<float>(current.desc.text.size()) * current.desc.glyph_size_pixels * 0.75F;
        height += current.desc.glyph_size_pixels;
    }
    if (current.children.empty()) {
        return {std::max(width, current.desc.layout.minimum_width),
                std::max(height, current.desc.layout.minimum_height)};
    }
    if (current.desc.layout.mode == UiLayoutMode::row) {
        float child_width = 0.0F;
        float child_height = 0.0F;
        for (const auto child_id : current.children) {
            if (const auto* child = node(child_id); child != nullptr && child->desc.visible) {
                const auto child_size = measure(*child);
                child_width += child_size.x;
                child_height = std::max(child_height, child_size.y);
            }
        }
        child_width +=
            current.desc.layout.gap *
            static_cast<float>(current.children.empty() ? 0 : current.children.size() - 1);
        width = std::max(width, horizontal_padding + child_width);
        height = std::max(height, vertical_padding + child_height);
    } else if (current.desc.layout.mode == UiLayoutMode::column) {
        float child_width = 0.0F;
        float child_height = 0.0F;
        for (const auto child_id : current.children) {
            if (const auto* child = node(child_id); child != nullptr && child->desc.visible) {
                const auto child_size = measure(*child);
                child_width = std::max(child_width, child_size.x);
                child_height += child_size.y;
            }
        }
        child_height +=
            current.desc.layout.gap *
            static_cast<float>(current.children.empty() ? 0 : current.children.size() - 1);
        width = std::max(width, horizontal_padding + child_width);
        height = std::max(height, vertical_padding + child_height);
    } else if (current.desc.layout.mode == UiLayoutMode::grid) {
        const auto columns = std::max(1U, current.desc.layout.grid_columns);
        const auto rows =
            static_cast<std::uint32_t>((current.children.size() + columns - 1) / columns);
        float cell_width = 0.0F;
        for (const auto child_id : current.children) {
            if (const auto* child = node(child_id); child != nullptr && child->desc.visible) {
                cell_width = std::max(cell_width, measure(*child).x);
            }
        }
        width = std::max(width, horizontal_padding + cell_width * static_cast<float>(columns) +
                                    current.desc.layout.gap * static_cast<float>(columns - 1));
        height = std::max(
            height, vertical_padding +
                        current.desc.layout.grid_cell_height * static_cast<float>(rows) +
                        current.desc.layout.gap * static_cast<float>(rows == 0 ? 0 : rows - 1));
    } else {
        for (const auto child_id : current.children) {
            if (const auto* child = node(child_id); child != nullptr && child->desc.visible) {
                const auto child_size = measure(*child);
                width = std::max(width, horizontal_padding + child_size.x);
                height = std::max(height, vertical_padding + child_size.y);
            }
        }
    }
    return {
        clamp_size(width, current.desc.layout.minimum_width, current.desc.layout.maximum_width),
        clamp_size(height, current.desc.layout.minimum_height, current.desc.layout.maximum_height)};
}

void WidgetTree::layout_node(Node& current, UiRect bounds, UiRect inherited_clip) {
    current.rect = bounds;
    current.clip = inherited_clip;
    ++layout_stats_.visible_widget_count;
    if (!same_scissor(intersection(current.clip, bounds), bounds)) {
        ++layout_stats_.clipped_widget_count;
    }
    if (current.desc.focusable && current.desc.enabled) {
        ++layout_stats_.focusable_widget_count;
    }
    auto content = bounds.inset(current.desc.layout.padding.left * dpi_scale_,
                                current.desc.layout.padding.top * dpi_scale_,
                                current.desc.layout.padding.right * dpi_scale_,
                                current.desc.layout.padding.bottom * dpi_scale_);
    const auto child_clip =
        current.desc.layout.clip_children || current.desc.kind == WidgetKind::scroll_area
            ? intersection(current.clip, current.rect)
            : current.clip;
    const auto gap = current.desc.layout.gap * dpi_scale_;
    if (current.children.empty()) {
        return;
    }

    if (current.desc.layout.mode == UiLayoutMode::row ||
        current.desc.layout.mode == UiLayoutMode::column) {
        const auto row = current.desc.layout.mode == UiLayoutMode::row;
        const auto available_main = (row ? content.width : content.height) -
                                    gap * static_cast<float>(current.children.size() - 1);
        float fixed_main = 0.0F;
        float fill_weight = 0.0F;
        for (const auto child_id : current.children) {
            const auto* child = node(child_id);
            if (child == nullptr || !child->desc.visible) {
                continue;
            }
            const auto size = measure(*child);
            const auto spec = row ? child->desc.layout.width : child->desc.layout.height;
            if (spec.mode == UiSizeMode::fill) {
                fill_weight += std::max(0.001F, spec.value);
            } else {
                fixed_main += resolve_size(spec, available_main, row ? size.x : size.y,
                                           row ? child->desc.layout.minimum_width
                                               : child->desc.layout.minimum_height,
                                           row ? child->desc.layout.maximum_width
                                               : child->desc.layout.maximum_height) *
                              dpi_scale_;
            }
        }
        const auto fill_space = std::max(0.0F, available_main - fixed_main);
        if (!row && current.desc.kind == WidgetKind::scroll_area) {
            current.scroll_extent_y = std::max(
                content.height,
                fixed_main + gap * static_cast<float>(
                                       current.children.empty() ? 0 : current.children.size() - 1));
            current.scroll_offset_y =
                std::clamp(current.scroll_offset_y, 0.0F,
                           std::max(0.0F, current.scroll_extent_y - content.height));
        }
        float cursor = row ? content.x : content.y - current.scroll_offset_y;
        for (const auto child_id : current.children) {
            auto* child = node(child_id);
            if (child == nullptr || !child->desc.visible) {
                continue;
            }
            const auto measured = measure(*child);
            const auto main_spec = row ? child->desc.layout.width : child->desc.layout.height;
            const auto cross_spec = row ? child->desc.layout.height : child->desc.layout.width;
            const auto minimum_main =
                row ? child->desc.layout.minimum_width : child->desc.layout.minimum_height;
            const auto maximum_main =
                row ? child->desc.layout.maximum_width : child->desc.layout.maximum_height;
            const auto minimum_cross =
                row ? child->desc.layout.minimum_height : child->desc.layout.minimum_width;
            const auto maximum_cross =
                row ? child->desc.layout.maximum_height : child->desc.layout.maximum_width;
            const auto main =
                main_spec.mode == UiSizeMode::fill
                    ? fill_space * std::max(0.001F, main_spec.value) / std::max(0.001F, fill_weight)
                    : resolve_size(main_spec, available_main, row ? measured.x : measured.y,
                                   minimum_main, maximum_main) *
                          dpi_scale_;
            const auto cross_available = row ? content.height : content.width;
            const auto cross =
                resolve_size(cross_spec, cross_available, row ? measured.y : measured.x,
                             minimum_cross, maximum_cross) *
                (cross_spec.mode == UiSizeMode::fill ? 1.0F : dpi_scale_);
            UiRect slot = row ? UiRect{cursor, content.y, main, content.height}
                              : UiRect{content.x, cursor, content.width, main};
            auto child_bounds =
                row ? aligned_rect(slot, main, cross, UiAlignment::stretch,
                                   child->desc.layout.vertical_alignment)
                    : aligned_rect(slot, cross, main, child->desc.layout.horizontal_alignment,
                                   UiAlignment::stretch);
            layout_node(*child, child_bounds, child_clip);
            cursor += main + gap;
        }
        return;
    }

    if (current.desc.layout.mode == UiLayoutMode::grid) {
        const auto columns = std::max(1U, current.desc.layout.grid_columns);
        const auto cell_width =
            std::max(0.0F, (content.width - gap * static_cast<float>(columns - 1)) /
                               static_cast<float>(columns));
        const auto cell_height = current.desc.layout.grid_cell_height * dpi_scale_;
        const auto visible_count =
            static_cast<std::uint32_t>(std::ranges::count_if(current.children, [this](WidgetId id) {
                const auto* child = node(id);
                return child != nullptr && child->desc.visible;
            }));
        const auto rows = (visible_count + columns - 1U) / columns;
        current.scroll_extent_y = rows == 0 ? content.height
                                            : static_cast<float>(rows) * cell_height +
                                                  static_cast<float>(rows - 1U) * gap;
        if (current.desc.kind == WidgetKind::scroll_area) {
            current.scroll_offset_y =
                std::clamp(current.scroll_offset_y, 0.0F,
                           std::max(0.0F, current.scroll_extent_y - content.height));
        }
        std::uint32_t visible_index = 0;
        for (const auto child_id : current.children) {
            auto* child = node(child_id);
            if (child == nullptr || !child->desc.visible) {
                continue;
            }
            const auto column = visible_index % columns;
            const auto row = visible_index / columns;
            UiRect slot{content.x + static_cast<float>(column) * (cell_width + gap),
                        content.y + static_cast<float>(row) * (cell_height + gap) -
                            current.scroll_offset_y,
                        cell_width, cell_height};
            const auto measured = measure(*child);
            const auto width = resolve_scaled_size(
                child->desc.layout.width, slot.width, measured.x,
                child->desc.layout.minimum_width, child->desc.layout.maximum_width, dpi_scale_);
            const auto height = resolve_scaled_size(
                child->desc.layout.height, slot.height, measured.y,
                child->desc.layout.minimum_height, child->desc.layout.maximum_height, dpi_scale_);
            layout_node(*child,
                        aligned_rect(slot, width, height, child->desc.layout.horizontal_alignment,
                                     child->desc.layout.vertical_alignment),
                        child_clip);
            ++visible_index;
        }
        return;
    }

    for (const auto child_id : current.children) {
        auto* child = node(child_id);
        if (child == nullptr || !child->desc.visible) {
            continue;
        }
        const auto measured = measure(*child);
        const auto width = resolve_scaled_size(
            child->desc.layout.width, content.width, measured.x,
            child->desc.layout.minimum_width, child->desc.layout.maximum_width, dpi_scale_);
        const auto height = resolve_scaled_size(
            child->desc.layout.height, content.height, measured.y,
            child->desc.layout.minimum_height, child->desc.layout.maximum_height, dpi_scale_);
        layout_node(*child,
                    aligned_rect(content, width, height, child->desc.layout.horizontal_alignment,
                                 child->desc.layout.vertical_alignment),
                    child_clip);
    }
}

core::Status WidgetTree::layout(math::Vec2f viewport_pixels, float dpi_scale) {
    if (!viewport_pixels.is_finite() || viewport_pixels.x <= 0.0F || viewport_pixels.y <= 0.0F ||
        !std::isfinite(dpi_scale) || dpi_scale <= 0.0F) {
        return core::Status::failure("widget_tree.invalid_viewport",
                                     "UI viewport and DPI scale must be positive and finite");
    }
    viewport_ = viewport_pixels;
    dpi_scale_ = dpi_scale;
    layout_stats_ = {};
    layout_stats_.widget_count = static_cast<std::uint32_t>(nodes_.size());
    ++layout_stats_.layout_passes;
    const UiRect viewport{0.0F, 0.0F, viewport_pixels.x, viewport_pixels.y};
    for (const auto root_id : roots_) {
        auto* root = node(root_id);
        if (root == nullptr || !root->desc.visible) {
            continue;
        }
        const auto measured = measure(*root);
        const auto width = resolve_scaled_size(
            root->desc.layout.width, viewport.width, measured.x, root->desc.layout.minimum_width,
            root->desc.layout.maximum_width, dpi_scale_);
        const auto height = resolve_scaled_size(
            root->desc.layout.height, viewport.height, measured.y,
            root->desc.layout.minimum_height, root->desc.layout.maximum_height, dpi_scale_);
        layout_node(*root,
                    aligned_rect(viewport, width, height, root->desc.layout.horizontal_alignment,
                                 root->desc.layout.vertical_alignment),
                    viewport);
    }
    rebuild_paint_order();
    return core::Status::ok();
}

void WidgetTree::append_paint_order(WidgetId id) {
    const auto* current = node(id);
    if (current == nullptr || !current->desc.visible) {
        return;
    }
    paint_order_.push_back(id);
    for (const auto child : current->children) {
        append_paint_order(child);
    }
}

void WidgetTree::rebuild_paint_order() {
    paint_order_.clear();
    paint_order_.reserve(nodes_.size());
    for (const auto root : roots_) {
        append_paint_order(root);
    }
}

WidgetId WidgetTree::hit_test(math::Vec2f point, bool drop_targets_only) const noexcept {
    for (auto iterator = paint_order_.rbegin(); iterator != paint_order_.rend(); ++iterator) {
        const auto* current = node(*iterator);
        if (current == nullptr || !current->desc.visible || !current->desc.enabled ||
            !current->clip.contains(point) || !current->rect.contains(point)) {
            continue;
        }
        if (drop_targets_only ? current->desc.drop_target : current->desc.pointer_events) {
            return current->desc.id;
        }
    }
    return {};
}

std::vector<WidgetId> WidgetTree::focus_order() const {
    std::vector<WidgetId> result;
    result.reserve(layout_stats_.focusable_widget_count);
    for (const auto id : paint_order_) {
        const auto* current = node(id);
        if (current != nullptr && current->desc.visible && current->desc.enabled &&
            current->desc.focusable) {
            result.push_back(id);
        }
    }
    return result;
}

void WidgetTree::navigate_focus(UiNavigation navigation, std::vector<UiEvent>& events) {
    const auto focusable = focus_order();
    if (focusable.empty()) {
        return;
    }
    const auto current = std::ranges::find(focusable, focused_);
    std::size_t index = current == focusable.end()
                            ? 0
                            : static_cast<std::size_t>(std::distance(focusable.begin(), current));
    if (navigation == UiNavigation::previous || navigation == UiNavigation::left ||
        navigation == UiNavigation::up) {
        index = index == 0 ? focusable.size() - 1 : index - 1;
    } else if (current != focusable.end()) {
        index = (index + 1) % focusable.size();
    }
    if (focused_ != focusable[index]) {
        focused_ = focusable[index];
        events.push_back({UiEventKind::focus_changed, focused_, {}, {}, {}, 0.0F, false});
    }
}

void WidgetTree::activate(WidgetId id, std::vector<UiEvent>& events) {
    auto* current = node(id);
    if (current == nullptr || !current->desc.enabled) {
        return;
    }
    if (current->desc.kind == WidgetKind::toggle) {
        current->desc.checked = !current->desc.checked;
        events.push_back(
            {UiEventKind::toggled, id, {}, {}, {}, current->desc.value, current->desc.checked});
    } else {
        events.push_back({UiEventKind::clicked, id, {}, {}, {}, 0.0F, false});
    }
}

UiRouteResult WidgetTree::route_input(const UiInputFrame& input) {
    UiRouteResult result;
    pointer_position_ = input.pointer;
    hovered_ = input.pointer_inside ? hit_test(input.pointer) : WidgetId{};
    if (hovered_.is_valid()) {
        result.consumed.pointer = true;
    }
    if (input.pointer_inside && input.wheel_delta != 0.0F) {
        for (auto iterator = paint_order_.rbegin(); iterator != paint_order_.rend(); ++iterator) {
            auto* current = node(*iterator);
            if (current == nullptr || !current->desc.visible ||
                current->desc.kind != WidgetKind::scroll_area ||
                !current->rect.contains(input.pointer)) {
                continue;
            }
            current->scroll_offset_y =
                std::clamp(current->scroll_offset_y - input.wheel_delta * 24.0F * dpi_scale_, 0.0F,
                           std::max(0.0F, current->scroll_extent_y - current->rect.height));
            (void)layout(viewport_, dpi_scale_);
            result.consumed.pointer = true;
            break;
        }
    }
    if (input.primary_pressed && hovered_.is_valid()) {
        captured_ = hovered_;
        pressed_ = hovered_;
        press_position_ = input.pointer;
        if (const auto* current = node(hovered_);
            current != nullptr && current->desc.focusable && focused_ != hovered_) {
            focused_ = hovered_;
            result.events.push_back(
                {UiEventKind::focus_changed, focused_, {}, {}, {}, 0.0F, false});
        }
        result.consumed.pointer = true;
    }
    if (captured_.is_valid()) {
        result.consumed.pointer = true;
        auto* current = node(captured_);
        if (current != nullptr && current->desc.kind == WidgetKind::slider && input.primary_down) {
            const auto range = current->desc.maximum_value - current->desc.minimum_value;
            const auto normalized =
                current->rect.width <= 0.0F
                    ? 0.0F
                    : std::clamp((input.pointer.x - current->rect.x) / current->rect.width, 0.0F,
                                 1.0F);
            const auto value = current->desc.minimum_value + normalized * range;
            if (value != current->desc.value) {
                current->desc.value = value;
                result.events.push_back(
                    {UiEventKind::value_changed, current->desc.id, {}, {}, {}, value, false});
            }
        }
        if (current != nullptr && current->desc.draggable && input.primary_down &&
            !drag_source_.is_valid()) {
            const auto dx = input.pointer.x - press_position_.x;
            const auto dy = input.pointer.y - press_position_.y;
            if (dx * dx + dy * dy >= 16.0F) {
                drag_source_ = current->desc.id;
                drag_payload_ = current->desc.drag_payload;
                result.events.push_back({UiEventKind::drag_started,
                                         drag_source_,
                                         drag_source_,
                                         drag_payload_,
                                         {},
                                         0.0F,
                                         false});
            }
        }
    }
    if (input.primary_released && captured_.is_valid()) {
        if (drag_source_.is_valid()) {
            const auto drop_target = hit_test(input.pointer, true);
            if (drop_target.is_valid()) {
                result.events.push_back({UiEventKind::dropped,
                                         drop_target,
                                         drag_source_,
                                         drag_payload_,
                                         {},
                                         0.0F,
                                         false});
            }
            drag_source_ = {};
            drag_payload_.clear();
        } else {
            const auto* released = node(captured_);
            if (released != nullptr && released->desc.kind == WidgetKind::slider) {
                result.events.push_back({UiEventKind::value_committed,
                                         released->desc.id,
                                         {},
                                         {},
                                         {},
                                         released->desc.value,
                                         false});
            } else if (pressed_ == hovered_) {
                activate(captured_, result.events);
            }
        }
        captured_ = {};
        pressed_ = {};
    }

    if (input.navigation == UiNavigation::next || input.navigation == UiNavigation::previous ||
        input.navigation == UiNavigation::left || input.navigation == UiNavigation::right ||
        input.navigation == UiNavigation::up || input.navigation == UiNavigation::down) {
        navigate_focus(input.navigation, result.events);
        result.consumed.keyboard = true;
        result.consumed.gamepad = true;
    } else if (input.navigation == UiNavigation::activate && focused_.is_valid()) {
        activate(focused_, result.events);
        result.consumed.keyboard = true;
        result.consumed.gamepad = true;
    } else if (input.navigation == UiNavigation::cancel) {
        result.events.push_back({UiEventKind::cancelled, focused_, {}, {}, {}, 0.0F, false});
        result.consumed.keyboard = true;
        result.consumed.gamepad = true;
    }

    auto* focused = node(focused_);
    if (focused != nullptr && focused->desc.kind == WidgetKind::text_input) {
        bool changed = false;
        if (!input.text.empty()) {
            focused->desc.text += input.text;
            changed = true;
            result.consumed.text = true;
        }
        if (input.backspace_pressed && !focused->desc.text.empty()) {
            focused->desc.text.pop_back();
            changed = true;
            result.consumed.keyboard = true;
        }
        if (input.delete_pressed && !focused->desc.text.empty()) {
            focused->desc.text.clear();
            changed = true;
            result.consumed.keyboard = true;
        }
        if (changed) {
            result.events.push_back(
                {UiEventKind::text_changed, focused_, {}, {}, focused->desc.text, 0.0F, false});
        }
    }

    for (const auto root : roots_) {
        if (subtree_blocks_gameplay(root)) {
            result.consumed.blocks_gameplay = true;
            break;
        }
    }
    return result;
}

bool WidgetTree::subtree_blocks_gameplay(WidgetId id) const noexcept {
    const auto* current = node(id);
    if (current == nullptr || !current->desc.visible) {
        return false;
    }
    if (current->desc.blocks_gameplay) {
        return true;
    }
    return std::ranges::any_of(current->children,
                               [this](WidgetId child) { return subtree_blocks_gameplay(child); });
}

core::Result<UiPaintStats> WidgetTree::paint(renderer::UiRenderer& output) const {
    UiPaintStats stats;
    const auto submit_quad = [&output, &stats](UiRect rect, const UiAtlasRegion& region,
                                               const std::array<float, 4>& color, UiRect clip,
                                               bool nine_slice) -> core::Status {
        if (rect.width <= 0.0F || rect.height <= 0.0F || clip.width <= 0.0F ||
            clip.height <= 0.0F) {
            return core::Status::ok();
        }
        renderer::UiQuadDesc quad;
        quad.minimum_pixels = {rect.x, rect.y};
        quad.maximum_pixels = {rect.x + rect.width, rect.y + rect.height};
        quad.uv_minimum = region.uv_minimum;
        quad.uv_maximum = region.uv_maximum;
        quad.color = color;
        quad.texture_layer = region.texture_layer;
        quad.scissor_enabled = true;
        quad.scissor = to_scissor(clip);
        auto status = output.submit_quad(quad);
        if (status) {
            ++stats.submitted_quads;
            stats.nine_slice_quads += nine_slice ? 1U : 0U;
        }
        return status;
    };

    for (const auto id : paint_order_) {
        const auto* current = node(id);
        if (current == nullptr || !current->desc.visible || current->clip.width <= 0.0F ||
            current->clip.height <= 0.0F) {
            continue;
        }
        if (!same_scissor(intersection(current->clip, current->rect), current->rect)) {
            ++stats.clipped_widgets;
        }
        if (!current->desc.nine_slice.empty()) {
            const auto* slice = skin_.find_nine_slice(current->desc.nine_slice);
            const auto* region =
                slice == nullptr ? nullptr : skin_.find_region(slice->atlas_region);
            if (slice != nullptr && region != nullptr) {
                const auto left =
                    std::min(slice->border_pixels.left * dpi_scale_, current->rect.width * 0.5F);
                const auto right =
                    std::min(slice->border_pixels.right * dpi_scale_, current->rect.width * 0.5F);
                const auto top =
                    std::min(slice->border_pixels.top * dpi_scale_, current->rect.height * 0.5F);
                const auto bottom =
                    std::min(slice->border_pixels.bottom * dpi_scale_, current->rect.height * 0.5F);
                const std::array xs{current->rect.x, current->rect.x + left,
                                    current->rect.x + current->rect.width - right,
                                    current->rect.x + current->rect.width};
                const std::array ys{current->rect.y, current->rect.y + top,
                                    current->rect.y + current->rect.height - bottom,
                                    current->rect.y + current->rect.height};
                const auto uv_width = region->uv_maximum.x - region->uv_minimum.x;
                const auto uv_height = region->uv_maximum.y - region->uv_minimum.y;
                const std::array us{region->uv_minimum.x,
                                    region->uv_minimum.x + uv_width * slice->border_pixels.left /
                                                               region->source_size_pixels.x,
                                    region->uv_maximum.x - uv_width * slice->border_pixels.right /
                                                               region->source_size_pixels.x,
                                    region->uv_maximum.x};
                const std::array vs{region->uv_minimum.y,
                                    region->uv_minimum.y + uv_height * slice->border_pixels.top /
                                                               region->source_size_pixels.y,
                                    region->uv_maximum.y - uv_height * slice->border_pixels.bottom /
                                                               region->source_size_pixels.y,
                                    region->uv_maximum.y};
                for (std::size_t y = 0; y < 3; ++y) {
                    for (std::size_t x = 0; x < 3; ++x) {
                        UiAtlasRegion cell = *region;
                        cell.uv_minimum = {us[x], vs[y]};
                        cell.uv_maximum = {us[x + 1], vs[y + 1]};
                        auto status =
                            submit_quad({xs[x], ys[y], xs[x + 1] - xs[x], ys[y + 1] - ys[y]}, cell,
                                        current->desc.color, current->clip, true);
                        if (!status) {
                            return core::Result<UiPaintStats>::failure(status.error().code,
                                                                       status.error().message);
                        }
                    }
                }
            }
        } else if (!current->desc.atlas_region.empty() || current->desc.kind != WidgetKind::label) {
            const auto* region = skin_.find_region(
                current->desc.atlas_region.empty() ? std::string_view{"solid"}
                                                   : std::string_view{current->desc.atlas_region});
            if (region != nullptr) {
                auto status =
                    submit_quad(current->rect, *region, current->desc.color, current->clip, false);
                if (!status) {
                    return core::Result<UiPaintStats>::failure(status.error().code,
                                                               status.error().message);
                }
            }
        }

        if (current->desc.kind == WidgetKind::toggle && current->desc.checked) {
            if (const auto* solid = skin_.find_region("solid"); solid != nullptr) {
                const auto mark =
                    current->rect.inset(current->rect.width * 0.28F, current->rect.height * 0.28F,
                                        current->rect.width * 0.28F, current->rect.height * 0.28F);
                auto status =
                    submit_quad(mark, *solid, {0.94F, 0.74F, 0.24F, 1.0F}, current->clip, false);
                if (!status) {
                    return core::Result<UiPaintStats>::failure(status.error().code,
                                                               status.error().message);
                }
            }
        }
        if (current->desc.kind == WidgetKind::slider) {
            if (const auto* solid = skin_.find_region("solid"); solid != nullptr) {
                const auto range = current->desc.maximum_value - current->desc.minimum_value;
                const auto normalized =
                    range <= 0.0F
                        ? 0.0F
                        : std::clamp((current->desc.value - current->desc.minimum_value) / range,
                                     0.0F, 1.0F);
                const UiRect fill{current->rect.x, current->rect.y,
                                  current->rect.width * normalized, current->rect.height};
                auto status =
                    submit_quad(fill, *solid, {0.68F, 0.43F, 0.18F, 1.0F}, current->clip, false);
                if (!status) {
                    return core::Result<UiPaintStats>::failure(status.error().code,
                                                               status.error().message);
                }
            }
        }
        if (!current->desc.text.empty()) {
            renderer::UiTextDesc text;
            text.position_pixels = {
                current->rect.x + current->desc.layout.padding.left * dpi_scale_,
                current->rect.y + current->desc.layout.padding.top * dpi_scale_};
            text.text = current->desc.text;
            text.glyph_size_pixels = current->desc.glyph_size_pixels * dpi_scale_;
            text.color = current->desc.text_color;
            text.scissor_enabled = true;
            text.scissor = to_scissor(current->clip);
            auto status = output.submit_text(text);
            if (!status) {
                return core::Result<UiPaintStats>::failure(status.error().code,
                                                           status.error().message);
            }
            stats.submitted_glyphs += static_cast<std::uint32_t>(current->desc.text.size());
        }
    }

    const auto* hovered = node(hovered_);
    if (hovered != nullptr && !hovered->desc.tooltip.empty()) {
        const auto width =
            std::max(80.0F, static_cast<float>(hovered->desc.tooltip.size()) * 10.5F + 16.0F);
        UiRect tooltip{std::min(pointer_position_.x + 14.0F, viewport_.x - width - 2.0F),
                       std::min(pointer_position_.y + 18.0F, viewport_.y - 28.0F), width, 26.0F};
        const UiRect viewport{0.0F, 0.0F, viewport_.x, viewport_.y};
        if (const auto* solid = skin_.find_region("solid"); solid != nullptr) {
            auto status =
                submit_quad(tooltip, *solid, {0.10F, 0.065F, 0.035F, 0.96F}, viewport, false);
            if (!status) {
                return core::Result<UiPaintStats>::failure(status.error().code,
                                                           status.error().message);
            }
        }
        auto status = output.submit_text({{tooltip.x + 8.0F, tooltip.y + 6.0F},
                                          hovered->desc.tooltip,
                                          12.0F,
                                          {1.0F, 0.93F, 0.72F, 1.0F},
                                          true,
                                          to_scissor(viewport)});
        if (!status) {
            return core::Result<UiPaintStats>::failure(status.error().code, status.error().message);
        }
        stats.submitted_glyphs += static_cast<std::uint32_t>(hovered->desc.tooltip.size());
    }
    return core::Result<UiPaintStats>::success(stats);
}

std::optional<UiRect> WidgetTree::rect(WidgetId id) const noexcept {
    const auto* found = node(id);
    return found == nullptr ? std::nullopt : std::optional<UiRect>{found->rect};
}

WidgetId WidgetTree::focused_widget() const noexcept {
    return focused_;
}

WidgetId WidgetTree::captured_widget() const noexcept {
    return captured_;
}

WidgetId WidgetTree::hovered_widget() const noexcept {
    return hovered_;
}

bool WidgetTree::dragging() const noexcept {
    return drag_source_.is_valid();
}

std::string_view WidgetTree::drag_payload() const noexcept {
    return drag_payload_;
}

float WidgetTree::scroll_offset(WidgetId id) const noexcept {
    const auto* found = node(id);
    return found == nullptr ? 0.0F : found->scroll_offset_y;
}

std::span<const WidgetId> WidgetTree::paint_order() const noexcept {
    return paint_order_;
}

const UiLayoutStats& WidgetTree::layout_stats() const noexcept {
    return layout_stats_;
}

void WidgetTree::set_visible(WidgetId id, bool visible) noexcept {
    if (auto* found = node(id); found != nullptr) {
        found->desc.visible = visible;
    }
}

void WidgetTree::set_focus(WidgetId id) noexcept {
    const auto* found = node(id);
    focused_ =
        found != nullptr && found->desc.visible && found->desc.enabled && found->desc.focusable
            ? id
            : WidgetId{};
}

std::string_view widget_kind_name(WidgetKind kind) noexcept {
    constexpr std::array names{"panel",  "label",      "image",       "button",  "toggle",
                               "slider", "text_input", "scroll_area", "tooltip", "grid_slot"};
    const auto index = static_cast<std::size_t>(kind);
    return index < names.size() ? names[index] : "unknown";
}

} // namespace heartstead::ui
