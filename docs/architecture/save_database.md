# Save Database

`FileSaveDatabase` is the first file-backed save database foundation.

`FileSaveSlotCatalog` is the first file-backed save-slot foundation. It owns a directory of safe
slot ids, persists per-slot display metadata, creates slot directories, lists slot summaries, and
returns `FileSaveDatabase` instances for per-slot persistence.

Implemented layout:

```text
save_slots_root/
  <slot_id>/
    slot.txt
    current.txt
    generations/
      generation_<n>/
        snapshot.hssb
        chunks/
          index.txt
          c_<x>_<y>_<z>.delta
```

Implemented behavior:

- writes and reads full snapshots through `SaveBinaryCodec`
- exposes a validated read helper that checks the loaded snapshot against the active
  `PrototypeRegistry` before callers materialize gameplay/runtime state
- stages full snapshot commits in `generations/generation_<n>.tmp`, promotes the finished
  generation directory, then replaces `current.txt` through a temporary-file rename
- falls back to the older flat `snapshot.hssb` plus `chunks/` layout when no generation manifest
  exists, so early save fixtures remain readable
- writes chunk edit deltas as independent per-chunk payload files
- stores a chunk index so streamed chunk delta records can be loaded separately
- exposes basic database statistics
- reports whether the active save is legacy or generation-backed, the active generation name,
  committed generation count, staged generation count, and stale generation count through
  `SaveDatabaseStats`
- treats the external chunk-delta table as authoritative whenever its index exists, including an
  intentionally empty index, instead of falling back to chunk records embedded in the snapshot
- writes streamed chunk-delta updates into the active generation when a generation manifest exists
- provides a world-streaming adapter that converts a missing per-chunk delta into an empty optional
  while preserving real save/database errors
- prunes stale committed generations with an explicit keep count while preserving the active
  generation and staged `.tmp` generation directories
- recovers from interrupted full-snapshot commits by removing abandoned staged `.tmp` generation
  directories after validating the active generation when a manifest exists
- compacts active chunk-delta storage by removing unreferenced `.delta` payload files while keeping
  indexed chunk deltas and unrelated sidecar files intact
- exposes an explicit save-database maintenance policy that can recover staged generation
  directories, prune stale committed generations, and compact orphaned active chunk-delta payloads
  in a deterministic order with an inspectable result
- migrates the active database snapshot through the ordered save migration registry and writes the
  upgraded snapshot as a new generation only when migrations apply
- manages save slots as safe lowercase directory ids without exposing real paths to gameplay code
- persists save-slot metadata in `slot.txt`, including slot id, display name, created timestamp,
  last-played timestamp, and last-saved timestamp
- validates save-slot metadata before writing or listing slots and rejects metadata whose slot id
  does not match its directory name
- exposes a catalog-level snapshot commit helper that writes the per-slot save database and advances
  `last_saved_at_ms` only after a successful snapshot commit; wall-clock corrections never move an
  existing saved or played timestamp backwards
- updates `last_played_at_ms` separately when a loaded world becomes active, so Continue ordering
  does not confuse opening a world with saving it
- keeps an optional `preview.png` sidecar beside the authoritative generation store; production
  captures the last unobscured rendered world frame on explicit/final saves, while preview read,
  write, or GPU-upload failures remain non-authoritative and cannot fail a world save
- treats missing `slot.txt` files as legacy/default metadata so older slot directories remain
  discoverable
- lists save-slot summaries with metadata and `SaveDatabaseStats` for each visible slot
- exposes a save-slot catalog summary for aggregate inspection of slot count, empty slots, active
  generation slots, legacy slots, staged generations, and chunk-delta totals

`current.txt` is the only authority for the active generation. Readers do not guess the newest
generation if the manifest is malformed or points to a missing directory. A completed generation
that was promoted before manifest publication failed is therefore stale, not implicitly active.
Maintenance validates an existing active generation before deleting staged directories; it does
not repair a corrupt manifest or choose an older generation automatically.

The replacement helpers close files and rename temporary paths, but they do not `fsync` file or
directory contents and do not provide inter-process locking. On filesystems where replacing an
existing path by rename fails, the compatibility fallback removes the destination before retrying.
Consequently this foundation provides staged failure isolation for ordinary API errors, not a
formal power-loss durability or concurrent-writer guarantee. Callers must serialize writes and use
an external backup/export policy for production data.

This is not a final production save store. It establishes the engine boundary:

- permanent world state remains typed `SaveSnapshot` data
- chunk deltas can be streamed independently
- derived data remains rebuildable and is not saved as authoritative state
- file layout and slot naming are owned by the engine, not by gameplay systems or mods

Future work should add durable commit/fsync policy, concurrent-writer exclusion, production-scale
backup/export policy, and save-slot UI workflows.
