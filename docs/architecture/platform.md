# Platform Abstraction

The platform layer owns the boundary between engine code and OS/window/input APIs.

Implemented foundation:

- `WindowDesc`
- `WindowState`
- `PlatformEvent`
- validated platform event queuing
- key/input enums
- retained `KeyInputState` snapshots
- retained mouse button, pointer position, and wheel input snapshots
- per-window `WindowInputSnapshot` for down/pressed/released keys, mouse buttons, pointer
  position, wheel deltas, and text input
- backend-neutral clipboard text contract
- `PlatformClock`
- `IPlatform`
  - backend-neutral platform contract for windows, displays, clipboard text, events, input
    snapshots, quit state, and clock
- `PlatformDesc`
  - selects `headless` or the `native` backend
  - is created through `create_platform`
- `PlatformBackendCapabilities`
  - reports backend availability before creation
  - distinguishes headless logical windows from native OS windows
  - reports keyboard input, text input, mouse input, display metadata, Vulkan surface, clipboard,
    and window-system support
- `DisplayInfo`
  - exposes backend-neutral display index, name, virtual-desktop origin, pixel extent, physical
    extent when available, DPI when available, refresh rate when available, and primary-display
    status
  - gives samples, tools, and future UI/renderer setup a display query without exposing X11 types
- `NativeWindowHandle`
  - exposes an opaque, backend-owned display/window token for renderer integration
  - avoids leaking Xlib or future platform-library types into gameplay, save, network, or mod code
- `HeadlessPlatform`
  - implements `IPlatform`
- X11 native backend
  - is compiled when CMake finds X11 and `HEARTSTEAD_ENABLE_X11_NATIVE` is enabled
  - creates real OS windows through Xlib
  - prefers XRandR connected outputs for monitor names, origin, physical size, refresh, and
    primary-output metadata when `HEARTSTEAD_ENABLE_XRANDR` is enabled and the extension is linked
  - falls back to Xlib screens as `DisplayInfo` records
  - returns X11 native window handles for renderer surface creation
  - owns, serves, and retrieves byte text through the X11 `CLIPBOARD`
    selection, including bounded large direct property reads and ICCCM `INCR` incremental
    transfers
  - maps X11 configure, close, key, text, pointer motion, mouse button, and wheel events into the
    same `PlatformEvent` and retained input snapshot model used by the headless backend
  - treats cursor capture as a retained request: a not-yet-viewable window, a temporary competing
    grab, or focus loss suspends capture without aborting application startup, and the event pump
    retries the request once X11 can grant it
  - reports unavailable when compiled without X11 or when no X11 display is available
- `run_headless_app`
  - headless-specific helper retained for deterministic tests
- `run_platform_app`
  - backend-neutral frame loop for future native backends

The headless backend creates logical windows, queues events, tracks resize/input/quit
state, exposes per-frame key pressed/released transitions, captures text input for the
current frame, exposes a deterministic virtual display, provides an in-process deterministic
clipboard, and runs a frame callback. The optional X11 native backend provides the first
real-window implementation behind the same `IPlatform` contract. SDL3 or another platform
library can still replace or supplement this backend later without changing game/runtime systems.

`run_platform_app` and `run_headless_app` clear one-frame input transitions before each
callback. Backends should process queued OS events during the frame, then allow
engine/game code to query:

- `is_key_down`
- `was_key_pressed`
- `was_key_released`
- `is_mouse_button_down`
- `was_mouse_button_pressed`
- `was_mouse_button_released`
- `input_snapshot`

Public event injection goes through `queue_event`, which validates event kind, target
window, window dimensions, key identity, mouse button identity, finite wheel deltas, and that text
payloads are non-empty before updating retained platform state. It does not validate UTF-8.
Backend-generated lifecycle events such as
`window_created` remain owned by platform window creation/destruction.

The `native` backend reports `platform.native_unavailable` when the optional X11 backend
is not compiled or no display is available. The platform layer exposes native handles but
does not create renderer resources. Vulkan surface, swapchain, and presentation ownership already
belong to the renderer backend. The X11 backend can own and serve clipboard bytes it sets, and it
can retrieve bytes owned by another application through synchronous selection conversion for
`UTF8_STRING`, `STRING`, or `TEXT` targets. It prefers `UTF8_STRING`, but fallback `STRING`/`TEXT`
bytes and key text produced by `XLookupString` are not transcoded or UTF-8 validated. Callers that
require UTF-8 must validate the returned/input bytes themselves.

External X11 clipboard retrieval is a blocking call. Each selection request has a short timeout,
an incremental transfer resets a bounded timeout as chunks arrive, and received native payloads
are capped at 64 MiB. The deterministic headless clipboard is only an in-process string store and
does not apply that native transfer cap.

The platform layer should remain below rendering, gameplay, modding, save, and network
systems. Meaningful world state must still flow through server-authoritative commands
and transactions.
