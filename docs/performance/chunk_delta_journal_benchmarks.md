# Chunk delta journal benchmarks

Status: the production retained writer, restart readers/writers, durable append path, recovery
invariants, and full checkpoint pass their declared reference-machine gates at 16,384 base records.
Checkpoint work remains proportional to the complete effective table and is not yet coordinated with
live streaming as an asynchronous runtime service.

The `heartstead_chunk_delta_journal_benchmark` executable drives a real `FileSaveDatabase` on the
configured filesystem. Schema v1 reports physical fixture construction, initial writer open,
individual stable-storage appends, restart-style reader and writer opens with a journal backlog, one
full checkpoint, restart verification, cleanup, and source/build/machine provenance.

## Durability and timing contract

The benchmark creates one generation containing an indexed base table. Fixture construction is
outside every retained append/open sample and remains visible as `fixture.generation_write_ms`.
It then:

1. opens one production `FileChunkDeltaWriter` and reports that initial base-index/file-size scan;
2. performs unmeasured warmup appends followed by individually timed retained appends through the
   same writer;
3. opens fresh writers and readers against the resulting base-plus-journal end mark, discarding the
   declared warmups and retaining each later open time;
4. times one production checkpoint of the complete effective table;
5. creates a fresh database/reader, verifies the latest accepted payload for every touched
   coordinate, requires a zero-entry active journal, and removes the unique fixture root.

Each append measurement begins after its coordinate and fixed-size payload are prepared. It includes
snapshot-authority and active-generation validation, writing the immutable entry, requesting stable
storage for the entry, atomically publishing its canonical filename, and persisting the directory
entry. It does not include owner-thread snapshot capture or queue handoff. The stable-storage request
is blocking and belongs on the save worker.

The journal uses engine-specific immutable, versioned, checksummed entry files; it is not a SQLite
database. Its publication/checkpoint ordering follows the same broad write-ahead principle that
committed log data remains authoritative until checkpoint data is durable and the covered log can
be removed. SQLite's primary documentation describes its WAL end-mark/checkpoint model and atomic
commit ordering in [Write-Ahead Logging](https://www.sqlite.org/wal.html) and
[Atomic Commit](https://www.sqlite.org/atomiccommit.html).

## Fail-closed invariants

The executable returns no successful report unless all of these hold:

- the base generation contains exactly the configured record and payload-byte counts;
- every retained receipt has the expected sequence, coordinate, payload size, encoded size, and
  monotonically increasing journal count/bytes;
- fresh writers and readers recover the exact effective record count and journal end mark;
- checkpoint reports exactly the warmup-plus-retained entry count as merged and removed;
- restart selects the compacted external table with no active journal entries;
- every coordinate touched by warmup or retained appends returns its latest expected payload;
- final statistics retain the original effective record count; and
- fixture cleanup succeeds before report validation.

The benchmark serializes all mutation. It does not claim concurrent writer/checkpointer safety,
cross-process exclusion, crash injection at every filesystem boundary, guaranteed-cold behavior,
or save-under-streaming latency. Dedicated journal tests separately cover pinned append end marks,
repeated-key ordering, stale writer/generation rejection, pending full-snapshot authority, restart,
temporary-file recovery, checkpoint recovery, legacy inline snapshots, and checksum corruption.

## Default regression gates

| Metric | Default limit |
| --- | ---: |
| Initial writer open | 250 ms |
| Durable retained append P95 | 25 ms |
| Writer reopen with journal backlog P95 | 250 ms |
| Reader reopen with journal backlog P95 | 250 ms |
| Complete checkpoint | 75,000 ms |
| Post-checkpoint reader open | 250 ms |

These are conservative single-filesystem regression caps, not foreground frame budgets or portable
storage guarantees. In particular, the checkpoint cap records the current cost so regressions fail
closed; a 48-second checkpoint is not acceptable synchronous gameplay work. `--enforce-gates`
writes the complete report and returns exit code 3 if any limit is exceeded.

## Schema v1 calibration

The retained 2026-08-01 calibration was produced from the clean tracked revision
`0706573a0d19f38397815366cc1bea3047d1a0ad` with:

```text
cmake -S . -B build/default-release -DCMAKE_BUILD_TYPE=Release
cmake --build build/default-release --target heartstead_chunk_delta_journal_benchmark -j2
build/default-release/apps/chunk_delta_journal_benchmark/heartstead_chunk_delta_journal_benchmark \
  --base-records 16384 \
  --payload-bytes 88 \
  --warmup-appends 8 \
  --appends 128 \
  --open-warmup 2 \
  --open-repetitions 9 \
  --fixture-parent build/default-release/benchmarks/physical-fixtures \
  --enforce-gates \
  --output build/default-release/benchmarks/chunk-delta-journal-0706573-runN.json
```

Each process constructed and removed its own fixture, retained 128 append samples after eight
warmups, retained nine writer and nine reader opens after two warmups, checkpointed all 136 entries,
and verified 136 updated coordinates after restart. The base table contained 16,384 records and
1,441,792 payload bytes; the journal contained 20,128 bytes before checkpoint.

| Property | Value |
| --- | --- |
| Machine | Intel Core Ultra 7 258V, 8 logical CPUs |
| OS | Linux 6.17.0-1030-oem, x86-64 |
| Compiler/build | GCC 13.3.0, Release |
| Source revision | `0706573a0d19f38397815366cc1bea3047d1a0ad` |
| Source state | clean tracked tree in every retained run |

| Metric | Run 1 | Run 2 | Run 3 | Median process value | Gate |
| --- | ---: | ---: | ---: | ---: | ---: |
| Generation write | 48.241 s | 48.047 s | 47.383 s | 48.047 s | report only |
| Initial writer open | 21.585 ms | 26.481 ms | 24.691 ms | 24.691 ms | 250 ms |
| Durable append median | 2.908 ms | 2.896 ms | 2.976 ms | 2.908 ms | report only |
| Durable append P95 | 3.313 ms | 3.476 ms | 3.336 ms | 3.336 ms | 25 ms |
| Durable append P99 | 3.760 ms | 4.157 ms | 3.532 ms | 3.760 ms | report only |
| Durable append maximum | 5.794 ms | 10.495 ms | 3.609 ms | 5.794 ms | report only |
| Writer reopen P95 | 21.732 ms | 21.581 ms | 21.854 ms | 21.732 ms | 250 ms |
| Reader reopen P95 | 13.883 ms | 13.913 ms | 13.802 ms | 13.883 ms | 250 ms |
| Complete checkpoint | 48.294 s | 47.563 s | 48.254 s | 48.254 s | 75 s |
| Post-checkpoint reader open | 12.809 ms | 12.673 ms | 13.781 ms | 12.809 ms | 250 ms |

Every process passed every configured gate, reported exact journal/checkpoint counts, verified every
touched coordinate, returned to zero active journal entries, and removed its fixture. Raw reports
remain outside Git under `build/default-release/benchmarks/`.

The median append P95 is roughly 14,500 times smaller than the median complete checkpoint. That
ratio is useful evidence that the foreground operation no longer rewrites the full table, but it is
not a paired before/after speedup claim: the historical benchmark measured full generation setup,
not the old single-update implementation in an otherwise identical process.

### Base-record-count sweep

Clean-revision probes held the 88-byte payload, 8/128 append shape, 2/9 open shape, and filesystem
constant while varying the indexed base table:

| Base records | Payload bytes | Generation write | Initial writer open | Append P95 | Writer reopen P95 | Reader reopen P95 | Checkpoint | Post-checkpoint open |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 128 | 11,264 | 0.400 s | 0.415 ms | 3.247 ms | 3.159 ms | 1.324 ms | 0.388 s | 0.347 ms |
| 4,096 | 360,448 | 12.034 s | 5.492 ms | 3.345 ms | 4.902 ms | 3.402 ms | 12.088 s | 3.647 ms |
| 16,384 | 1,441,792 | 48.047 s | 24.691 ms | 3.336 ms | 21.732 ms | 13.883 ms | 48.254 s | 12.809 ms |

Durable append P95 is flat across this sweep because the retained writer validates/indexes the base
once and each accepted update publishes one bounded file. Initial/restart opens and checkpoint grow
with base or journal state. Checkpoint remains essentially the same full-table cost as generation
write, confirming that compaction must stay off the gameplay thread and still needs runtime
scheduling/coordination work.

## Remaining persistence work

This milestone closes the measured full-table rewrite from the streamed foreground update path. It
does not close the broader M5 persistence goal. Remaining work includes:

- serialize live append, full-snapshot publication, bulk replacement, and checkpoint ownership
  without relying on an external caller contract;
- move checkpoint execution behind an explicit bounded background policy and expose backlog/age;
- measure owner-thread capture and worker handoff with large dirty populations;
- run load/save/teleport concurrency workloads and prove streaming deadlines under persistence I/O;
- add crash injection around append publication and checkpoint directory transitions;
- add guaranteed-cold and additional filesystem/media calibration; and
- add cross-process writer exclusion before treating one save directory as multi-process safe.
