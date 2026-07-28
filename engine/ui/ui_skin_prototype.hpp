#pragma once

#include "engine/modding/generic_prototype.hpp"
#include "engine/ui/widget_tree.hpp"

#include <span>

namespace heartstead::ui {

[[nodiscard]] core::Result<UiSkin>
ui_skin_from_panel_prototypes(std::span<const modding::GenericPrototype* const> prototypes);

} // namespace heartstead::ui
