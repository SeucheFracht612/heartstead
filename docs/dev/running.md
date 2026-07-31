# Running Heartstead

This guide covers the maintained applications and their user-facing behavior. Paths below assume a
`default-debug` build from the repository root.

## Development game

Start the normal local interactive slice:

```bash
./build/default-debug/apps/dev_game/heartstead_dev_game
```

An unbounded local native launch owns both the authoritative server and client and uses
`saves/foundation_slice_0_1` by default. The name is a compatibility path, not an active milestone
name.

Useful options:

```text
--headless                    run without native presentation
--frames N                    run a bounded headless frame count
--native-frames N             run a bounded native frame count
--connect ADDRESS:PORT        run as a remote client using numeric IPv4
--save DIRECTORY              use an explicit local save directory
--autosave-seconds N          set the local autosave interval
--no-save                     disable local persistence
--diagnostic-asset-fallbacks  allow diagnostic presentation fallbacks
```

Run `heartstead_dev_game --help` for the exact accepted syntax and validation rules.

### Save ownership

- A normal unbounded local native run uses the default save directory.
- Bounded or headless runs save only when `--save DIRECTORY` is supplied.
- `--no-save` disables persistence for local runs.
- A remote client never owns the authoritative save and cannot combine `--connect` with `--save`.
- A headless run with no explicit frame bound uses the executable's 120-frame smoke default rather
  than becoming a long-lived server.

Examples:

```bash
# Deterministic headless smoke
./build/default-debug/apps/dev_game/heartstead_dev_game --headless --frames 120

# Bounded native integration run
./build/default-debug/apps/dev_game/heartstead_dev_game --native-frames 300 --no-save

# Explicit local save
./build/default-debug/apps/dev_game/heartstead_dev_game \
  --save saves/my_test_world --autosave-seconds 30
```

## Controls

| Action | Default input |
| --- | --- |
| Move | `W`, `A`, `S`, `D` |
| Jump / traversal | `Space` |
| Sprint | `Left Shift` |
| Crouch | `Left Ctrl` |
| Dash | `Q` |
| Roll | `Left Alt` |
| Interact | `E` |
| Inventory | `Tab` |
| Hotbar | `1`–`9` |
| Camera mode | `F1` |
| Diagnostics | `F3` |
| Controller/debug geometry | `F4` |
| Close menu / pause | `Escape` |
| Primary world action | Left mouse button |
| Secondary world action | Right mouse button |

In the current voxel interaction slice, the primary action removes the selected voxel and the
secondary action places the available clay voxel in the adjacent cell. World commands remain
server-authoritative in both local and remote compositions.

Inventory interaction uses left-button dragging for normal stack movement. Holding the right mouse
button while dropping a stack moves half of it, rounded up. UI behavior and content are still
development-facing rather than final game UX.

## Dedicated server and remote client

Start the standalone headless server:

```bash
./build/default-debug/apps/dedicated_server/heartstead_dedicated_server \
  --bind 0.0.0.0:7777
```

Options:

```text
--bind ADDRESS:PORT  numeric IPv4 bind endpoint; default 0.0.0.0:7777
--ticks N            stop after a positive bounded tick count
```

Without `--ticks`, the process runs until `SIGINT` or `SIGTERM`. A bounded smoke run is useful for
CI or launch validation:

```bash
./build/default-debug/apps/dedicated_server/heartstead_dedicated_server \
  --bind 127.0.0.1:7777 --ticks 120
```

Join from a client process:

```bash
./build/default-debug/apps/dev_game/heartstead_dev_game \
  --connect 127.0.0.1:7777
```

For another machine on the LAN, replace the address with the server's numeric IPv4 address and
allow the chosen UDP port through the host firewall.

The current dedicated executable is **memory-only**. It has no save-directory or autosave option,
so stopping it discards that session. Remote clients correctly do not write authoritative state.

The UDP transport is intended for controlled LAN and test environments. It has admission cookies,
session tokens, bounded reliability, fragmentation, rate limits, keepalives, and timeouts, but it
does not provide encryption, account authentication, NAT traversal, matchmaking, or public-service
hardening. Do not expose it to untrusted Internet clients.

`tools/netem_multiplayer.sh` can apply or clear a local Linux `tc netem` latency/loss profile for
manual multiplayer testing. Read the printed privileged operation before applying it.

## Renderer applications

Run the native renderer smoke scene:

```bash
./build/default-debug/apps/render_smoke/heartstead_render_smoke
```

It requires X11 and a present-capable Vulkan device. It exercises the retained chunk renderer,
asynchronous meshing, staged uploads, camera-relative rendering, depth, resizing, minimizing, and
clean shutdown.

Run a deterministic headless benchmark:

```bash
./build/default-release/apps/render_benchmark/heartstead_render_benchmark \
  --scene mountains --warmup 120 --frames 1000 --radius 2 \
  --output build/benchmarks/mountains.json
```

Use `--list-scenes` to enumerate current workloads and add `--vulkan` for a native run with GPU
timestamps. See [Renderer benchmarks](../performance/renderer_benchmarks.md) before comparing
results.

## Asset Lab

Asset Lab loads the production cooked store and the same renderer, materials, model presentation,
visual-prefab state mapping, terrain meshing, and particle presentation used by gameplay. Build and
inspect the base player visual interactively:

```bash
cmake --build --preset default-debug --target heartstead_asset_lab
./build/default-debug/apps/asset_lab/heartstead_asset_lab \
  --prefab base:visuals/player --preview character --lighting overcast
```

Use the deterministic headless route for import and cooked-payload validation without X11/Vulkan
presentation:

```bash
./build/default-debug/apps/asset_lab/heartstead_asset_lab \
  --headless --prefab base:visuals/player --preview visual-prefab
```

Run `heartstead_asset_lab --list` for accepted preview, lighting, and debug names, and `--help` for
the current CLI contract. See [Asset Lab](../asset_lab.md) for the maintained workflow.

## Other applications, samples, and tools

The default build also provides audio, UI, and scripting benchmarks. Focused samples cover the
platform, renderer, physics, network, scripting, jobs, math, mod, workpiece, chunk, room, and world
state boundaries. Tools include the asset cooker, block-model viewer, chunk inspector, log viewer,
map-profile inspector, mod validator, prototype inspector, replay inspector, save inspector, shader
compiler, workpiece inspector, and world inspector.

Avoid maintaining a second exhaustive executable list here. Discover exact current targets with:

```bash
cmake --build --preset default-debug --target help
```

Most tools and benchmark applications provide `--help`. Use that output before scripting them.
