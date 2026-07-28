# Audio

Heartstead keeps audio policy separate from device playback. Gameplay and mods emit stable
`sound_event` prototype ids. The event registry resolves those prototypes to active logical asset
ids during aggregate content validation; gameplay never names or opens a file.

## Ownership and boundaries

- Applications own one `IAudioSystem`, alongside platform and renderer ownership.
- `AudioBackend::null_backend` owns logical voices and runs all mixer policy without opening an
  output device. Headless tests and dedicated servers use this backend.
- `AudioBackend::miniaudio` is the production playback backend. miniaudio types stay inside
  `engine/audio/miniaudio`.
- `SoundEventDefinition` contains the selected asset, bus, gain, distance attenuation, direction
  cone, per-event instance limit, priority, looping, streaming, and spatialization policy.
- `AudioMixer` owns stable voice ids, bus gain ramps, exact listener/emitter transforms,
  attenuation, equal-power pan, per-event limits, and deterministic priority/age voice stealing.
- Exact `WorldPosition` values remain intact until the mixer subtracts the listener anchor and
  local offset. This avoids losing voxel-scale precision in far-away regions.

The four buses are master, music, SFX, and ambient. A bus gain change is a target change; the
mixer ramps the current value over a configured interval so UI changes do not click.

## Event prototype

```toml
kind = "sound_event"
id = "base:audio/footstep_grass"
display_name = "Grass footstep"
asset = "base:sounds/footsteps/grass.wav"
bus = "sfx"
spatialized = true
looping = false
streaming = false
gain = 0.8
minimum_distance = 1.0
maximum_distance = 24.0
priority = 160
maximum_instances = 12
```

`asset` must resolve to an active `sound` or `music` record in the content catalog. Invalid
references fail content validation before session startup.

## Playback and device recovery

The production backend uses miniaudio's high-level engine/resource-manager APIs. Short SFX decode
into managed buffers; music and long ambient beds stream. Engine sound groups mirror the
backend-neutral buses. Device callbacks only pull audio and enqueue notification state. They never
call gameplay or rebuild the device.

Output stop/reroute notifications are consumed by the application owner thread. That thread may
reinitialize playback and restore logical looping voices; failure moves the system to a silent
fallback while gameplay continues. This follows miniaudio's rule that device start, stop,
initialization, and uninitialization must not run in the device callback.

## Current external basis

The implementation tracks miniaudio 0.11.x through the pinned vcpkg baseline. It uses the
split `miniaudio.c` implementation form recommended ahead of 0.12, rather than defining
`MINIAUDIO_IMPLEMENTATION` in an engine source file.

Primary references:

- <https://miniaud.io/docs/manual/index.html>
- <https://github.com/mackron/miniaudio/blob/master/CHANGES.md>
- <https://github.com/microsoft/vcpkg/tree/master/ports/miniaudio>

## Verification

- `heartstead_audio_system_tests` covers asset resolution, floating-origin spatial math, gain
  ramps, direction cones, priority stealing, rejected voices, and null-backend lifecycle.
- The production gate adds device-loss/recovery tests and an offline 128-voice, 48 kHz stereo,
  256-frame benchmark with a published p95 target below 1 ms.
