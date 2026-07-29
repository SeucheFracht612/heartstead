# Assets and Resource Packs

Assets are resolved through virtual paths and indexed separately from gameplay
prototypes.

Implemented foundation:

- `VirtualFileSystem`
  - parses namespace-relative virtual paths
  - rejects absolute paths, backslashes, empty segments, `.`, and `..`
  - resolves mounted namespaces in override order without escaping mount roots
  - reads both text and binary payloads through the same safe resolution path and a configurable
    byte limit (256 MiB by default)
  - lists active files below a virtual directory with later mounts overriding earlier files at the
    same virtual path
  - bounds aggregate traversal entries and depth, rejects symlinks in listed trees, and reports
    filesystem failures instead of silently falling back to a lower-priority mount

- `ResourcePackDiscoverer`
  - discovers `resource_pack.toml`
  - validates namespace-style resource pack ids
  - parses manifests through the shared bounded, quote-aware flat-manifest reader
  - rejects duplicate and unknown manifest fields, invalid boolean values, and unknown shader
    extension declarations
  - reports structured diagnostics

- `ResourcePackPolicy`
  - prevents resource packs from declaring gameplay content or overriding gameplay paths
  - owns resource-pack catalog indexing so runtime validation, tools, and samples apply the same
    restrictions
  - stages catalog changes and commits them only when every indexed pack asset passes policy
  - rejects generic data and unknown asset kinds instead of treating arbitrary files as
    presentation overrides
  - requires shader assets to use a controlled `shaders/extensions/<extension>/` path declared by
    the pack manifest
  - rejects malformed asset identities, undeclared/unscoped shader paths, and raw Vulkan/SPIR-V
    shader payload paths

- `ResourcePackLoadPlanner`
  - sorts discovered resource packs into a deterministic default load plan
  - assigns explicit increasing asset priorities so later packs override earlier packs by plan data,
    not incidental iteration order
  - is used by content validation, asset cooking, shader compilation, and the mod sandbox

- `AssetCatalog`
  - logical asset ids based on relative paths
  - virtual paths for actual source files
  - asset kind inference
  - source kind tracking for mods, resource packs, and engine assets
  - simple content hashes
  - bounded streaming hashes that report short reads and oversized source files
  - priority-based active asset selection
  - transactional directory indexing that leaves the existing catalog unchanged if any file in a
    source fails traversal, hashing, identity, or insertion validation

- Material prototype assets
  - material prototypes refer to shader templates and textures through virtual paths
  - material asset validation resolves those references through active catalog records
  - resource-pack overrides can satisfy material texture bindings by winning the logical asset id
  - renderer material definitions keep those references declarative until cooking/backend binding

- `AssetCatalogBuilder`
  - indexes mounted asset directories through the virtual file system
  - lets resource packs override mod-provided presentation assets without changing
    gameplay prototypes
  - can index the active VFS namespace or a virtual subdirectory for safe overlay discovery without
    making the VFS responsible for asset kind inference, hashing, or cook decisions
  - keeps direct directory indexing available for tools that need source-specific records before
    overlay collapse

- `CookedAssetManifest`
  - records active cooked asset outputs without changing gameplay prototype identity
  - stores source virtual path, source hash, cooked relative output path, cooked hash,
    pipeline version, and dependencies
  - treats `cooked_hash` as the hash of the exact cooked payload wrapper bytes written by the
    cooker, not just the uncooked source bytes
  - reports unresolved active dependency edges before assets are cooked
  - has a versioned text codec for deterministic tool output and tests

- `CookedAssetStore`
  - loads a cooked manifest from disk
  - reads cooked payload files by logical asset id
  - validates the cooked payload hash against the manifest before decoding header fields or source
    bytes
  - validates payload magic, backend, logical id, kind, source virtual path, source hash,
    pipeline version, and byte count against the manifest record
  - decodes deterministic `meta.*` payload fields into a generic metadata map for tools/runtime
  - exposes source payload bytes behind the cooked manifest identity
  - applies configurable manifest/payload byte limits plus fixed header line/field limits before
    allocating decoded payload state

- `AssetCooker`
  - builds the cooked manifest from the catalog
  - fails early when an active cooked asset dependency cannot resolve to another active cooked
    record
  - selects an explicit cook backend through `AssetCookConfig`
  - rejects a source that exceeds the configured per-asset byte limit (256 MiB by default)
  - reports backend and per-kind pipeline metadata for tooling
  - writes cooked payload files under deterministic relative paths
  - uses explicit development passthrough backends for textures, models, shaders,
    audio, materials, fonts, UI, localization, and data assets
  - exposes a partial production backend for data-like assets, material payloads, converted
    glTF/GLB runtime models, decoded PNG/JPEG RGBA8 textures, validated KTX2 payloads, SPIR-V shader
    payloads, validated WAV/OGG/FLAC audio payloads, and validated SFNT font payloads while
    rejecting unsupported source formats with explicit validation errors
  - emits production metadata for media payloads, such as texture dimensions, glTF/GLB container
    details and runtime geometry/skin/clip counts, SPIR-V word/bound data, WAV/OGG/FLAC container
    details, and SFNT/TTC font table data
  - writes the cook profile into payload headers so the cooked asset store can verify development
    payloads against development pipelines and production payloads against production pipelines

- `ShaderCompiler`
  - filters the same asset catalog down to active shader assets
  - writes deterministic compiled-shader payload wrappers and a shader manifest
  - supports a development profile for source validation and a production profile for validated
    SPIR-V passthrough
  - rejects shader sources above its configurable 16 MiB default limit
  - keeps shader processing behind a renderer-owned validation/cook stage instead of exposing
    backend graphics API access to mods or resource packs

- `heartstead_asset_cooker`
  - discovers mods and resource packs
  - indexes raw assets through the same catalog rules as runtime
  - accepts `development` or `production` as an optional cook backend argument
  - accepts `--only <logical_id>` to cook one active catalog record through the same validated
    pipeline
  - accepts `--entity-visuals` to discover every declared entity visual model and cook its
    transitive dependency closure
  - accepts `--inspect` to print cooked asset store inspection data after a successful cook
  - accepts `--inspect-store <cooked_root> [manifest]` to inspect an existing cooked asset store
  - writes cooked payload files and `build/cooked_assets/asset_manifest.txt` by default
  - reloads the cooked asset store after writing to verify manifest/payload consistency

- `heartstead_shader_compiler`
  - discovers mods and resource packs
  - indexes active shader assets through the asset catalog
  - accepts a compile profile argument, defaulting to `development`
  - accepts `--inspect` to print shader compile result inspection data after compilation
  - writes `build/compiled_shaders/shader_manifest.txt` by default

- `ContentValidation`
  - aggregates mod validation, resource-pack discovery, asset catalog indexing, material registry
    construction, and material asset-reference validation
  - backs `heartstead_mod_validator` so missing material shader and texture assets are reported
    before runtime
  - exposes the aggregate validation report through debug inspection so tooling can inspect
    resource-pack priorities, active asset counts, material overrides, and diagnostics from one
    stable data shape

The default cook backend is `development_passthrough`: it preserves source bytes behind a
small cooked payload header so the runtime and tools can validate source selection, override
priority, deterministic output paths, manifest identity, and cooked payload loading before
production formats exist.

Current content, manifest, shader, and cooked-payload hashes use the engine's shared stable
64-bit hash helper. This is a deterministic compatibility and corruption-detection primitive, not
a hostile-tamper security boundary; a future stronger digest should be introduced through a
manifest version or explicit hash algorithm field.

The `production_converters` backend is available but partial. It can currently cook data-like
assets (`data`, `localization`, `ui`, and unknown/raw data), material assets, glTF/GLB `model`
assets, PNG/KTX2/JPEG `texture` assets, `.spv` `shader` assets, WAV/OGG/FLAC `sound` or `music`
assets, and SFNT `font` assets into deterministic production-profile payload wrappers. Text glTF
and GLB models are strictly parsed and validated by fastgltf, bounded by engine-owned limits, and
converted into the versioned `heartstead.model.v2` binary. That runtime format contains indexed
triangle geometry, UV0, four-influence skin vertices, a node hierarchy, inverse-bind matrices,
bounded translation/rotation/scale animation clips, decoded RGBA8 base-color images, glTF
base-color factors, and per-primitive material references. Opaque, alpha-mask, and double-sided
material state is preserved; alpha blend and transformed/non-UV0 base-color textures fail closed.
The runtime never parses JSON or follows external glTF paths. Unsupported topology, morph-weight
animation, excess joint influences, malformed hierarchies, and out-of-range accessors fail as
`asset_cooker.invalid_model`. PNG textures are validated for signature and IHDR shape; KTX2
textures are validated for identifier, dimensions, DFD range, and level index ranges; JPEG textures
are validated for SOI, sane marker ranges, nonzero frame dimensions, and EOI. Malformed textures
fail with `asset_cooker.invalid_texture`. SPIR-V shaders are validated for header shape, magic,
version, and id bound through the renderer-owned shader validation helper; malformed SPIR-V fails
with `shader_compiler.invalid_spirv`, and Slang/HLSL sources still fail with
`shader_compiler.production_compiler_unavailable` until a real compiler backend is linked. WAV
audio is validated for RIFF/WAVE shape plus `fmt ` and non-empty `data` chunks; OGG is validated
for an initial OggS page with a non-empty payload; FLAC is validated for a native FLAC stream with
a first STREAMINFO block. Malformed audio fails with `asset_cooker.invalid_wav`,
`asset_cooker.invalid_ogg`, or `asset_cooker.invalid_flac`. Fonts are validated as
TrueType/OpenType SFNT or TrueType Collection containers before cooking; malformed fonts fail with
`asset_cooker.invalid_font`.

Production cooked payloads also carry deterministic metadata fields in the payload header. The
store preserves those fields as decoded metadata, so inspectors and runtime systems can query
facts such as `texture.width`, `texture.height`, `audio.sample_rate`, `model.container`, or
`model.vertices`, `model.skins`, `model.animations`, or `font.table_count` without reparsing the
source container on every load. These metadata fields
remain advisory derived data; the manifest identity, virtual path, source hash, pipeline version,
cooked payload hash, and payload validation still define compatibility.

Other binary media source formats, such as unsupported compressed audio containers, are not
converted yet and fail through the relevant format validation error. Future model optimization,
additional PBR material channels, compressed texture/audio conversion, and Slang/HLSL-to-SPIR-V
production compilation must keep the same virtual path, override, profile, and manifest rules.
