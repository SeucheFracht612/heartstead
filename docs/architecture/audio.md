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
production cooker validates them and records a versioned procedural-tone runtime format. The
miniaudio backend materializes deterministic mono PCM from the cooked payload. Authored WAV, FLAC,
and Ogg Vorbis assets use the same event path without gameplay changes. The miniaudio translation
unit enables stb_vorbis, so production-cooked Ogg Vorbis data is an end-to-end playback format;
Opus-in-Ogg remains unsupported.

`GameRuntime` owns the active asset catalog and event registry used to create an audio system.
The development executable also supplies its verified `CookedAssetStore`. Production playback loads
the event's payload from that store, registers encoded audio or generated tone PCM with miniaudio
under the stable logical asset ID, and reuses that registration for later voices. Source paths are
retained only as a compatibility path for tools or tests that do not provide a cooked store.
`ClientAudioPresentation` then updates the listener from the exact camera position, maintains the
ambient loop, and turns grounded walk/run distance into spatial footstep events. It queries the
supporting full or partial voxel and selects `interaction.footstep_sound`; an absent surface sound
uses `sounds.footstep_default` from the player visual. The audio system must be shut down before its
runtime because these registries are borrowed for the system lifetime.

## Runtime fallback

`AudioMixerConfig::fallback_event_id` defaults to `base:audio/interaction_fallback`. When playback
requests an unregistered event ID and that configured fallback exists, the mixer creates the voice
from the fallback definition. The production backend then resolves the asset from the accepted
voice snapshot, so it loads and plays the fallback's cooked payload rather than retrying the
original missing ID.

The fallback keeps its own spatialization, bus, gain, priority, and instance-limit policy. The base
fallback is positional, so a missing positional event still needs the request's emitter. Set the
configuration ID to an empty string to make missing events fail with `audio.event_missing`, which
is useful for isolated validation tests.

Fallback use is not silent: the mixer logs the missing and selected event IDs once per distinct
missing ID for its lifetime. `AudioSystemStats::fallback_voices` counts accepted fallback voices
and `fallback_diagnostics` counts the deduplicated warnings. This protects a running session from a
late or stale playback request; it does not make an invalid declared `sound_event` valid. Missing
or wrong-kind asset references still fail aggregate content validation before startup.

## Playback and device recovery

The production backend uses miniaudio's high-level engine/resource-manager APIs. Cooked assets are
registered once by logical ID; short SFX decode into managed buffers. Self-contained cooked
payloads currently decode from memory even when an event carries the streaming hint, while the
source-file compatibility path can still stream. Engine sound groups mirror the backend-neutral
buses. Device callbacks only pull audio and enqueue notification state. They never call gameplay
or rebuild the device.

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
  ramps, direction cones, priority stealing, rejected voices, deduplicated named-event fallback,
  fallback playback from a production-cooked payload, procedural PCM, the real miniaudio null
  device, device-loss recovery, and offline rendering.
- `heartstead_client_audio_presentation_tests` covers exact far-origin listener updates, grounded
  distance-based footsteps, partial-voxel surface selection, default footsteps, and ambient-loop
  lifecycle through the logical null backend.
- `heartstead_audio_benchmark` renders the real miniaudio graph offline. On an Intel Core Ultra 7
  258V, the Release build with 128 looping mono voices at 48 kHz stereo and 256-frame blocks
  measured 0.125 ms average and 0.178 ms p95 over 1,000 blocks (target: below 1.0 ms p95).
