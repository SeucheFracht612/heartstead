# Debug Inspection

Every important engine object should expose structured inspection data early. This keeps
systems visible before UI overlays and editor tools exist.

Implemented foundation:

- `InspectionData`
  - object type
  - display name
  - prototype id
  - save id
  - runtime id
  - state
  - named fields
  - structured issues

- `Inspector`
  - backend capability records for platform, renderer, physics, and asset cooking
  - virtual file system mounts, visible virtual file entries, asset records, and asset catalog
    summaries
  - resource-pack load plans
  - aggregate content validation reports
  - cooked asset records, manifests, and stores
  - compiled shader records and shader compile results
  - platform display records
  - script backend, script module, script host API, and script host event records
  - transport backend records, transport capabilities, server welcome handshake records, and client
    session records
  - command operation traces, dispatch results, host-session command reports/results, and replay
    log/step/report summaries
  - client protocol session records
  - simulation LOD policies, raw subjects, decisions, and frame plans
  - world-derived replication interest reports
  - item stacks
  - cargo records
  - entity records
  - physical resource records
  - build pieces
  - assemblies
  - process instances
  - spatial networks
  - region graphs
  - room graphs
  - dirty region trackers
  - mod lifecycle plans
  - world operations
  - save database stats
  - save slot summaries
  - save snapshots
  - world state
  - text rendering for console/debug output

- `GameInspector`
  - script host command route records
  - script host command report records
  - script host command batch records
  - chunk-delta publication advances, avoided full snapshots, and revision gaps
  - voxel-fluid topology reconciliation, dirty collection, active frontier, and processed work

`WorldState` inspection reports save metadata, the next stable save id, the next runtime
handle, the next entity net id, the next process id, dirty-region count, and
per-kind dirty-region and per-representation database counts. This is the backing data shape for
`heartstead_world_inspector`.

Backend capability inspection reports unavailable production slots as structured warnings.
This lets console tools and future overlays show why Vulkan, native platform windows, Jolt,
or production asset converters are unavailable without needing backend-specific handles.

Asset inspection reports VFS mount counts, mounted namespaces, visible file entries, asset logical
ids, virtual paths, source kind/id, content hashes, active catalog counts, and override counts. This
keeps resource-pack replacement and asset selection visible to tools before renderer/runtime asset
loading.
Resource-pack load-plan inspection reports pack count, first/last pack ids, assigned priorities,
and invalid priority-order issues so override order is visible before asset indexing.
Content validation inspection reports mod, resource-pack, prototype, script, asset, semantic
definition, material asset-reference, and material override counts. It also includes the
resource-pack load-plan priority summary and forwards validation diagnostics as structured
inspection issues.
Cooked asset inspection reports manifest profile/schema, active and inactive cooked outputs,
per-kind counts, source and cooked hashes, cooked payload paths, unresolved dependency counts, and
store roots. This keeps cooked output identity and payload-integrity state visible after asset
cooking and before runtime asset loading.
Shader compile inspection reports manifest paths, compiled record counts, language/role counts,
source and compiled hashes, backend names, source byte counts, and compiler diagnostics. This keeps
the renderer-owned shader validation/cook boundary visible before shader-pack UI exists.

Save database inspection reports legacy versus generation-backed layout, active generation,
committed/staged/stale generation counts, snapshot size, effective chunk-delta table size, both
journal sizes/sequences, and warnings for pending full-snapshot or chunk-delta journal work. This
keeps the save acceptance/checkpoint boundary visible after generation manifests are introduced.
The live runtime overlay separately reports deferred checkpoint roots, in-flight attempts, total
retry submissions, completions, exhausted roots, and terminal checkpoint failures.
Save database maintenance inspection reports recovered staged generation count, full-snapshot and
chunk-journal recovery/compaction results, pruned stale generation count, compacted orphan count,
before/after generation counts, and whether the maintenance pass changed persistent storage.
Save database migration inspection reports previous/final schema versions, applied migration count,
whether an upgraded snapshot was written, and before/after generation counts.
Save slot catalog inspection reports root path, slot count, empty/generation/legacy/invalid slot
counts, staged generation count, and chunk-delta totals. Per-slot inspection reports slot id,
display name, timestamps, path, active layout, snapshot presence, generation counts, and
chunk-delta count so tools can distinguish empty slots from generation-backed saves.

World operation inspection reports transaction stage sequence, mutation count,
derived-update count, event count, save dirty state, replication dirty state, and failure
issues. This keeps server-authoritative command handling and replay diagnostics visible.
Command dispatch results, host-session command reports, and replay steps retain the same local
operation trace so command tooling can inspect committed stages and dirty flags after the
`WorldOperation` object has gone out of scope.
Inspection exposes that trace directly, including stage sequence, mutation count, derived update
count, first mutation/update labels, save dirty state, replication dirty state, and malformed-trace
warnings or errors.
Replay step and report inspection also exposes whether deterministic replay expectations were
checked, so command logs can distinguish unverified execution from exact transaction outcome
verification.

Assembly inspection reports port count, bound port source build-piece ids, and total
assembly-port capacity. This keeps multiblock machine access points visible in tools
without requiring nearby-block scans.

Entity inspection reports runtime handle, session net id, save id, prototype, kind, persistence,
sleeping state, and transform fields so dynamic objects remain visible to simulation, save, and
networking diagnostics.

Spatial network inspection reports node, edge, and port capacity totals plus owned/sourced port
counts in addition to graph counts. Network ports also carry owner/source identity so systems can
query how much storage, cart, smoke, power, ward, water, or logistics capacity is exposed to a
given owner.

Mod lifecycle inspection reports total staged task count, per-stage task counts, each task's
stage/kind/mod/source tuple, and lifecycle diagnostics. This is the backing data shape for
showing settings, prototype, data-update, final-fix, asset/resource, migration,
runtime-server, and runtime-client mod work in validation tools.

Script module inspection reports stable module id, source mod, source path, stage, API version,
source byte count, and declared permissions. Game-side script command inspection reports
host-event-to-command route fields, accepted/rejected command report state, committed mutation
state, emitted event count, reserved id count, and error details. Transport inspection reports
backend availability, host capabilities, server welcome handshake fields, and accepted client
session state such as protocol version, server/client session ids, payload limits, ordering
support, and handshake time.
Host-session command result inspection reports accepted/rejected response state, sequence,
command type, committed mutation state, event count, reserved id count, and error details.
Replication batch inspection reports server stream sequence, source client and command
sequence/type, event count, reserved id count, first event details, and empty/malformed batch
warnings. Host-session tick inspection reports
transport message counts, command response counts, command report totals, accepted/rejected and
committed command totals, replication send counts, relevance-report totals, and mismatches between
tick counters and retained reports.
Replication relevance inspection reports command sequence/type, candidate/relevant/filtered client
counts, broadcast-default state, first relevant/filtered clients, and filtering reasons.
Client protocol session inspection reports accepted server/client ids, next command sequence,
pending command count, queued result count, queued replication batch/event counts, and the last
accepted replication sequence. It also reports server disconnect reason code/message and the
disconnect timestamp once a session has been closed.
Client runtime synchronization separately reports applied chunk-snapshot slices, completed chunk
snapshots, and reliable chunk-subscription removals, so unsubscribe traffic cannot masquerade as
snapshot throughput.
Server/runtime inspection reports configured subscribe/retain radii and client cap; current client,
subscription, current/stale/partial publication, deferred transition/snapshot, and pending bootstrap
state counts; per-frame additions/removals; atomic snapshot chunks/slices/payload bytes; shared codec
operations/time/overshoot; serialization-budget deferrals; and reliable admission deferrals. It also
reports the configured global chunk-snapshot serialization boundary. Tracy builds plot subscription
population, snapshot slices, and deferred snapshots.
Simulation LOD inspection reports policy radii and tick intervals, raw subject identity,
process ids, coordinates, persistence, sleeping/forced state, per-subject LOD decisions, offline
timestamp deltas, due-tick state, frame-plan counts, and counter mismatches in manually
constructed or corrupted plans.

Game runtime inspection reports the active server/client composition, fixed-tick configuration,
authoritative world and chunk counts, generation-safe entity/component/tombstone counts, command
and replication queues, presentation proxy counts, gameplay-module registration totals, and
per-system last/total timings. Collision and relight response fields include pending/completed
counts, rolling P95 and session maximum latency, and coalesced/abandoned invalidations; the F3
overlay mirrors those values for live diagnosis. A synchronization failure records its original
typed error as a terminal session fault and exposes it as both fields and an error issue, so a
failed feature replication or presentation callback cannot disappear behind a later
successful-looking frame.

Inspection output is not the final tool UI. It is the common data shape future debug
overlays, inspectors, replay reports, and save tools can render.
