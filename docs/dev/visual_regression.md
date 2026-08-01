# Visual Regression

The benchmark captures final post-UI RGBA8 through the RHI. Headless has no pixels, so use Vulkan.

```bash
mkdir -p build/visual-regression

./build/default-debug-werror/apps/render_benchmark/heartstead_render_benchmark \
  --vulkan --scene starting-biome --warmup 60 --frames 30 \
  --width 1280 --height 720 \
  --output build/visual-regression/starting-biome.json \
  --capture build/visual-regression/starting-biome.png

./build/default-debug-werror/apps/render_benchmark/heartstead_render_benchmark \
  --vulkan --scene starting-biome --warmup 60 --frames 30 \
  --width 1280 --height 720 \
  --output build/visual-regression/starting-biome-compare.json \
  --compare build/visual-regression/starting-biome.png
```

Each PNG has a `.capture.json` sidecar with schema, extent, and RGBA8 hash. Comparison reports changed
fraction, RMSE, maximum channel delta, and hashes. Defaults allow 0.25% changed pixels, RMSE 1.5, and
three values of per-channel noise.

Smoke baselines in `tests/visual_baselines` cover flat terrain, starting biome, and character workshop
at 320x180. Regenerate only for intentional visual changes on the documented baseline driver and
review images rather than updating hashes blindly.

Capture and comparison must use identical scene, seed, resolution, warm-up, and frame counts.
Different Vulkan drivers may produce small legitimate differences; do not weaken thresholds to hide
an unexplained change.
