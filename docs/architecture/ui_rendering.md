# UI Rendering

Status: authoritative for Renderer V2.

`WidgetTree` is retained and stable-ID based. Layout uses logical pixels and window DPI scale. Nested
clips become scissors; panels support nine-slice regions; images/icons use atlases; input supports
focus, text editing, scrolling, drag/drop, tooltips, and pointer capture. Gameplay and Asset Lab use
the same `UiRenderer`.

## Text

Noto Sans is stored at `mods/base/assets/fonts/heartstead-ui.ttf` under SIL OFL 1.1. Startup builds a
deterministic linear SDF atlas for Basic/Extended Latin, Greek, Cyrillic, punctuation, and currency.
UTF-8 decoding rejects overlong encodings, surrogates, and out-of-range scalars and substitutes U+FFFD.
Whitespace advances without geometry. Derivative coverage keeps text sharp across DPI scales.

Complex shaping and CJK fallback are documented limitations and are not claimed complete.
The SFNT is currently a CMake-staged renderer bootstrap asset; the cooker validates fonts, but the
runtime does not yet load a precomputed SDF atlas from the cooked store.

## Maps and composition

`MapViewRenderer` consumes gameplay-owned `MapDiscovery`, supporting signed 64-bit cells, surface,
underground, aerial, mod-defined layers, and stable custom markers. The HUD paints a minimap; `M`
opens the full map and Escape closes it.

UI is composed after tone mapping into the full-resolution target, so exposure and internal render
scale never dim or blur it. New widgets require semantic accessibility labels and may not use color as
the only carrier of state.

See [Game UI](game_ui.md).
