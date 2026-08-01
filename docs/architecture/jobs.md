# Jobs Architecture

Jobs are an engine-owned execution boundary for asynchronous and parallelizable work.

Implemented foundation:

- `JobBackend`
  - `immediate` backend for deterministic tests, tools, and samples
  - `thread_pool` backend for worker-thread execution

- `IJobSystem`
  - submits named jobs
  - exposes backend name, bounded-queue counters, queue age, and backpressure totals
  - requests queued or cooperative running-job cancellation with an explicit reason
  - drains completed job results explicitly
  - reports job success and failure through structured result records

- Job descriptors
  - stable job name
  - stable job type
  - priority
  - caller-defined, nonzero estimated cost
  - work function

- Job results
  - job id
  - name and type
  - priority
  - estimated cost
  - state
  - cancellation reason
  - completion order
  - enqueue/start/completion timestamps relative to job-system startup
  - queue, execution, and total latency
  - error code/message for failed jobs

The immediate backend executes jobs synchronously on submit and queues completed results
for deterministic tests and simple tools.

The thread-pool backend starts a fixed number of worker threads, runs queued jobs by priority,
publishes structured success/failure/cancellation records, and joins workers on shutdown. Priority
chooses the next queued job; it does not preempt work already running. Waiting jobs age into higher
effective priority after a bounded number of dispatches, so a continuous stream of high-priority
work cannot starve older normal or low-priority work. Job callback exceptions are converted into
failed result records.
Completed results are drained explicitly so callers can decide where authoritative
world/save state is allowed to change.

`max_pending_jobs` bounds all accepted work whose result has not yet been published.
`max_completed_results` independently bounds the completed-result mailbox. Submission returns
`jobs.pending_queue_full` when the pending limit is reached and `jobs.completed_queue_full` when a
full mailbox must be drained first; neither condition allocates an unbounded overflow list.
`pending_count` includes queued, running, and result-publication work. `JobSystemStats` splits queued,
running, publishing, and completed-mailbox counts and reports rejected submissions, cancellation
totals, oldest queued age, and maximum observed queue latency. Typed mesh, collision, light, and
residency job systems set the generic pending limit to their own concurrency limit.

`request_cancel` removes a queued job immediately when result-mailbox space exists and publishes a
cancelled result without invoking its callback. If the mailbox is full, the queued job is marked and
the next available worker skips its callback after publication space is released. A running callback
observes cancellation through `JobContext::cancellation_requested()` and
`JobContext::cancellation_reason()` at coarse safe points. Running work is never preempted; the final
generic result is cancelled even when a cooperative callback returns success after observing its
token. The first cancellation reason wins.

Shutdown requests `shutdown` cancellation for running work and marks queued callbacks to be skipped.
Non-cooperative running callbacks still have to return before their worker can join. If the completed
mailbox is full once shutdown begins, results that cannot be published are discarded so destruction
can join rather than deadlock. Callers that need every result must stop submitting and drain
completions before destroying the system.

Chunk meshing layers a typed result mailbox over this generic execution API. Job closures capture
only immutable neighborhood/render-table snapshots and a cancellation token. The mailbox owns
`ChunkMeshResult` payloads until the renderer owner thread drains them; generic job results remain
useful for lifecycle and failure accounting. Cancellation never grants a worker access to live
world state and is checked before the expensive mesh build.

Typed chunk schedulers retain their payload-specific cancellation tokens because they must publish
typed cancelled records and release pooled snapshots. The generic token is independently available
to every callback and owns the execution-level cancellation state.

Gameplay code should not own raw threads or platform-specific synchronization. Future
parallel systems should submit work through engine-owned job APIs, keep save/world
mutation on authoritative paths, and use explicit result handoff points.
