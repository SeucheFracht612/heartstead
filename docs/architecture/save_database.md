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
        chunk_journal/
          entry_<zero-padded-sequence>.hcdj
    journal/
      checkpoint.txt
      entry_<zero-padded-sequence>.hsj
```

Implemented behavior:

- writes and reads full snapshots through `SaveBinaryCodec`
- accepts a snapshot durably by writing a versioned, checksummed, immutable journal entry; readers
  prefer the highest valid accepted entry newer than the compacted checkpoint
- bounds the journal to eight entries and 1 GiB, rejects non-canonical names, duplicate/exhausted
  sequences, oversized payloads, truncation, checksum mismatch, and unsupported versions
- compacts the newest accepted entry into the generation store, durably advances the journal
  checkpoint, and only then removes covered journal entries
- recovers interrupted journal work by removing owned `.tmp` files, flushing the directory change,
  and compacting any accepted entry newer than the checkpoint
- exposes a validated read helper that checks the loaded snapshot against the active
  `PrototypeRegistry` before callers materialize gameplay/runtime state
- stages full snapshot commits in `generations/generation_<n>.tmp`, promotes the finished
  generation directory, then replaces `current.txt` through a temporary-file rename
- falls back to the older flat `snapshot.hssb` plus `chunks/` layout when no generation manifest
  exists, so early save fixtures remain readable
- stores the checkpointed chunk-edit base as independent per-chunk payload files plus a sorted index
- opens a generation-scoped retained `FileChunkDeltaWriter` that validates and accounts for the
  base/journal once, then durably publishes each streamed update as one immutable, versioned,
  checksummed journal entry instead of reading and rewriting the complete table
- bounds individual journal payloads to 16 MiB, the effective table and journal to 512 MiB each,
  the effective table to one million coordinates, and one checkpoint interval to 65,536 entries
- opens a generation-scoped `FileChunkDeltaReader` that selects and validates the authoritative
  base index plus current journal end mark once; retained readers serve concurrent requests by
  binary-searching that immutable view and reading only the selected base or journal payload
- overlays only the highest journal sequence for each coordinate while retaining earlier immutable
  entries until checkpoint, so repeated streamed writes have exact last-accepted ordering
- checkpoints chunk deltas by durably materializing the complete effective base table, atomically
  moving the covered active journal to an ignored compacted directory, and only then deleting it;
  a crash before the move still overlays the journal and a crash after it reads the durable base
- recovers chunk-journal work by removing owned `.hcdj.tmp` entries and a leftover compacted
  directory, then validates every committed entry and checksum fail-closed
- pins legacy inline snapshot deltas in the reader when no external table exists, preserving old
  fixtures without putting full-snapshot decoding in each worker request
- exposes basic database statistics
- reports snapshot-journal and chunk-journal entry count/bytes/highest-sequence data with the
  generation statistics
- reports whether the active save is legacy or generation-backed, the active generation name,
  committed generation count, staged generation count, and stale generation count through
  `SaveDatabaseStats`
- treats the external chunk-delta table as authoritative whenever its index exists, including an
  intentionally empty index, instead of falling back to chunk records embedded in the snapshot
- writes streamed chunk-delta updates into the active generation journal when a generation manifest
  exists; the streaming sink owns a retained writer so a flush batch does not reopen or rescan the
  table for every chunk
- provides a world-streaming adapter that owns an already-opened indexed reader and converts a
  missing per-chunk delta into an empty optional while preserving real save/database errors
- prunes stale committed generations with an explicit keep count while preserving the active
  generation and staged `.tmp` generation directories
- recovers from interrupted full-snapshot commits by removing abandoned staged `.tmp` generation
  directories after validating the active generation when a manifest exists
- compacts active chunk-delta storage by removing unreferenced `.delta` payload files while keeping
  indexed chunk deltas and unrelated sidecar files intact
- exposes an explicit save-database maintenance policy that can recover staged generation and both
  journal types, checkpoint the chunk journal, prune stale committed generations, and compact
  orphaned active chunk-delta payloads in a deterministic order with an inspectable result
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
- provides a one-worker `SaveScheduler` with bounded active/completed requests and measured
  per-request/aggregate working-memory reservations; serialization, stable-storage waits,
  compaction, and slot-metadata publication stay on that worker
- treats journal acceptance as success even if later checkpoint compaction fails, reports the
  compaction error separately, and lets normal reads/recovery select the accepted entry
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

`current.txt` is the only authority for the compacted generation. Readers do not guess the newest
generation if the manifest is malformed or points to a missing directory. A completed generation
that was promoted before manifest publication failed is therefore stale, not implicitly active.
An accepted journal entry newer than the journal checkpoint is a separate, explicit authority and
is preferred by readers. While that full-snapshot authority is pending, chunk readers select its
inline deltas and chunk writers, bulk replacement, chunk checkpoint, and chunk recovery reject the
overlap. Maintenance publishes the accepted full snapshot before recovering the newly selected
generation's chunk journal, so corruption in the superseded generation cannot block recovery of the
newer accepted save. Maintenance validates an existing active generation before deleting staged
directories; it does not repair a corrupt manifest or choose an older generation automatically.

Durable replacement closes the staged file, requests stable storage for it, atomically replaces the
destination, and persists the containing directory entry. POSIX uses `fsync` for files and
directories; Windows uses `FlushFileBuffers` and `MoveFileExW(..., MOVEFILE_WRITE_THROUGH)`, where
there is no portable directory-`fsync` equivalent. These calls establish the engine's acceptance
boundary but cannot override guarantees of the filesystem, device firmware, virtualization layer,
or platform. The scheduler's single worker serializes requests submitted through one scheduler
instance. A retained chunk writer is likewise a single-writer session and rejects generation
rollover, pending full-snapshot authority, exhausted bounds, and sequential conflicts at its
expected next sequence. Direct database callers must still externally serialize that session with
bulk replacement, checkpoint, and full-snapshot publication; simultaneous filesystem races and
separate processes are not locked by this layer. Backup/export policy remains an operational
responsibility.

An indexed reader is a view of the generation and journal end mark selected when it opens. Appends
published later are intentionally invisible. Callers reopen after checkpoint or generation
publication and keep the selected generation from being pruned for the reader's lifetime, because
those operations replace/remove files referenced by the old view. Publishing a newer generation
does not redirect an existing reader; this makes all worker requests in one streaming epoch observe
the same base-plus-journal authority. Opening fails immediately on a malformed manifest, index,
journal header, or journal checksum rather than deferring that failure to an arbitrary worker
request.

This is not a final production save store. It establishes the engine boundary:

- permanent world state remains typed `SaveSnapshot` data
- chunk deltas can be streamed independently
- derived data remains rebuildable and is not saved as authoritative state
- file layout and slot naming are owned by the engine, not by gameplay systems or mods

Warm and Linux cache-drop-advice physical read benchmarks exercise the indexed base path at 16,384
records. The companion
[chunk delta journal benchmark](../performance/chunk_delta_journal_benchmarks.md) measures one-file
durable appends, base-plus-journal reopen, exact restart recovery, and complete checkpoint at the
same record scale. They do not establish guaranteed-cold behavior or cover other filesystems/media.
Future work should add in-process mutation/checkpoint coordination, cross-process writer exclusion,
production-scale backup/export policy, large-world snapshot-capture and save-under-load benchmarks,
guaranteed-cold/multi-filesystem validation, and complete save-slot UI workflows.
