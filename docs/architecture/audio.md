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

The base mod currently uses bounded `.tone` manifests for source-controlled development sounds.
They describe a sine/noise generator and envelope rather than embedding binary media. The
production cooker validates them, records sample-rate/frame metadata, and the miniaudio backend
materializes deterministic mono PCM at load time. Authored WAV and FLAC assets use the same event
path without gameplay changes. OGG is catalogued and production-cooked, but is not an end-to-end
playback format yet: the current miniaudio build does not include a Vorbis decoder.

`GameRuntime` owns the active asset catalog and event registry used to create an audio system.
`ClientAudioPresentation` then updates the listener from the exact camera position, maintains the
ambient loop, and turns grounded walk/run distance into spatial footstep events. It queries the
supporting full or partial voxel and selects `interaction.footstep_sound`; an absent surface sound
uses `sounds.footstep_default` from the player visual. The audio system must be shut down before its
runtime because these registries are borrowed for the system lifetime.

## Playback and device recovery

The production backend uses miniaudio's high-level engine/resource-manager APIs. Short SFX decode
into managed buffers; music and long ambient beds stream. Engine sound groups mirror the
backend-neutral buses. Device callbacks only pull audio and enqueue notification state. They never
call gameplay or rebuild the device.

Output stop/reroute notifications are consumed by the application owner thread. That thread may
reinitialize playback and restore logical looping voices; failure moves the system to a silent
fallback while gameplay continues. This follows miniaudio's rule that device start, stop,
initialization, and uninitialization must not run in the device callback.

Tests can select miniaudio's null output device through `AudioSystemDesc`. This still runs the real
device callback and mixer graph without depending on host speakers; it is distinct from
Heartstead's logical null audio backend.

## Current external basis

The implementation pins miniaudio 0.11.25 through the vcpkg baseline. The vcpkg port distributes
the upstream header-only form, so one private translation unit defines `MINIAUDIO_IMPLEMENTATION`.
No other engine source defines it, and public engine headers never include `miniaudio.h`.

Primary references:

- <https://miniaud.io/docs/manual/index.html>
- <https://github.com/mackron/miniaudio/blob/master/CHANGES.md>
- <https://github.com/microsoft/vcpkg/tree/master/ports/miniaudio>

## Verification

- `heartstead_audio_system_tests` covers asset resolution, floating-origin spatial math, gain
  ramps, direction cones, priority stealing, rejected voices, procedural PCM, the real miniaudio
  null device, device-loss recovery, and offline rendering.
- `heartstead_client_audio_presentation_tests` covers exact far-origin listener updates, grounded
  distance-based footsteps, partial-voxel surface selection, default footsteps, and ambient-loop
  lifecycle through the logical null backend.
- `heartstead_audio_benchmark` renders the real miniaudio graph offline. On an Intel Core Ultra 7
  258V, the Release build with 128 looping mono voices at 48 kHz stereo and 256-frame blocks
  measured 0.125 ms average and 0.178 ms p95 over 1,000 blocks (target: below 1.0 ms p95).
