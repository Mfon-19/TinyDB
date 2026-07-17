# TinyDB Design

Status: authoritative implemented architecture

This document defines the storage-engine contract TinyDB is built to uphold.
It supersedes design descriptions in source comments and the repository README
when they disagree. Changes to the implementation must update this design and
its guarantee evidence without introducing a second semantic path.

TinyDB is not "a B+ tree with persistence." It is a contract about what an
application may believe after a transaction returns, including after process
death, power loss, I/O failure, or concurrent reads.

## 1. Product contract

TinyDB is an embedded, ordered, transactional key-value store for local
application state.

It provides:

- Unique byte-string keys ordered by unsigned lexicographic byte order.
- Byte-string values, including values larger than one database page.
- Atomic multi-key read/write transactions.
- Strict serializability within the owning process.
- Many concurrent read transactions and at most one write transaction.
- Snapshot-stable forward cursors for the lifetime of a read transaction.
- Read-your-writes behavior inside a write transaction.
- Synchronous commits that survive process death, operating-system crashes,
  and ordinary power loss, subject to the operating system, filesystem, and
  device honoring synchronization requests.
- Automatic recovery to the latest complete durable transaction.
- An explicitly encoded, checksummed, versioned file format.
- Exclusive ownership of a database by one process.
- Bounded cache and write-transaction memory, with explicit backpressure.
- Checkpointing that bounds recovery time and WAL size.
- Integrity verification that reports corruption without terminating the host.

It intentionally does not provide:

- Network access, authentication, or a server protocol.
- Multiple processes concurrently opening one database, including read-only
  processes.
- Concurrent write transactions.
- Publication while arbitrarily long readers remain active. Existing readers
  must drain before a durable write becomes visible.
- Distributed transactions, replication, or high availability.
- SQL, schemas, joins, or engine-maintained secondary indexes.
- Cross-database transactions.
- Custom key comparators. Applications encode sortable keys themselves.

The application builds its own secondary indexes by updating their keys in the
same write transaction as the primary record.

## 2. Data-model semantics

- Empty keys and empty values are valid.
- `Put(key, value)` inserts or replaces the value for `key`.
- `Delete(key)` succeeds when the key is absent.
- Ranges are half-open: `[start, end)`.
- Either range bound may be absent.
- Prefix scans are a first-class operation and do not require sentinel keys.
- Key ordering is stable across processes, architectures, and releases.
- Keys are at most 1,024 bytes.
- Value lengths are encoded as 64-bit unsigned integers. Runtime options may
  reject new values or transactions above configured memory limits, but those
  limits do not change the file format.
- A database page is 4,096 bytes. The page size is recorded in the superblock;
  a binary rejects unsupported page sizes rather than reinterpreting them.

## 3. Core invariants

These invariants organize the entire implementation.

### 3.1 State ownership

```text
database file       = state through checkpoint_lsn
WAL                 = durable transactions after checkpoint_lsn
committed cache     = latest visible page versions
write overlay       = private, uncommitted page versions
```

The frontiers always satisfy:

```text
checkpoint_lsn <= visible_lsn <= durable_lsn
```

At idle, `visible_lsn == durable_lsn`. They differ only after a WAL commit is
durable and before existing readers have drained for publication.

### 3.2 Private before durability

Before the WAL durability point, every mutation is private to its write
transaction. Aborting the transaction drops its overlay; shared committed
pages and persistent allocator state have not changed.

### 3.3 Infallible after durability

Before issuing the WAL write, commit constructs every object publication will
need. After WAL synchronization succeeds, publication performs no allocation,
no file I/O, no page decoding, and no fallible container growth. It consists
only of bounded locking, reader drainage, `noexcept` ownership transfers, and
scalar state publication.

### 3.4 One logical path

Convenience operations such as `Database::Put` create and commit a normal write
transaction. Recovery replays physical records and never calls the public API
or reruns B+ tree algorithms. Checkpointing does not create a second mutation
path.

### 3.5 Persistent input is untrusted

Every superblock, WAL record, database page, child reference, leaf link, and
overflow link is validated before use. Malformed persistent state returns a
structured error. `TINYDB_CHECK` is reserved for in-memory programmer errors.

## 4. System architecture

```text
Application
    |
    v
Database -- ReadTransaction -- Cursor
    |
    +---- WriteTransaction
              |
              v
       TransactionPages overlay
              |
              v
        B+ tree + allocator
              |
              v
       Commit coordinator ----> WAL segments
              |
              v
         Reader gate
              |
              v
      Committed page cache
              |
              v
      Checkpoint manager -----> Database file
```

The principal modules are:

- **Database**: process ownership, public API, global state machine, and
  subsystem lifetime.
- **Reader gate**: snapshot admission, active-reader accounting, and fair
  publication.
- **TransactionPages**: transaction-local copy-on-write page overlay.
- **B+ tree**: ordered-map behavior over an abstract page source.
- **Transactional allocator**: high-water allocation, retired extents, and
  checkpoint-qualified reuse.
- **Committed page cache**: immutable visible page frames and bounded memory.
- **Commit coordinator**: freeze, WAL assembly, durability, and publication.
- **WAL**: transaction durability newer than the checkpoint.
- **Checkpoint manager**: immutable checkpoint snapshots and the only ordinary
  writer of database data pages.
- **Recovery manager**: physical WAL validation and redo before the handle is
  exposed.

These names describe responsibilities. They do not require one class or source
file per box. Code is split only when ownership or testing improves.

## 5. Process and thread ownership

`Database::Open` acquires an OS-level exclusive lock before inspecting,
recovering, or mutating database state. The lock is held until every internal
resource is released. A second open returns `Busy`.

The implementation must also prevent duplicate opens in the same process and
must identify files by their opened file identity rather than only by their
path spelling. Symlinks and hard links must not bypass ownership.

`Database` is safe for concurrent method calls. Transaction and cursor objects
are not safe for concurrent calls on the same object. Their internal snapshot
tokens are reference-counted so destruction does not depend on unlocking a
thread-owned mutex.

At most one `WriteTransaction` exists. It owns the writer permit from
`BeginWrite` until commit, abort, or destruction.

## 6. Reader gate and isolation

TinyDB provides strict serializability without general MVCC. Committed pages
are immutable while visible, and publication waits for existing readers.

### 6.1 Begin read

Under the reader-gate mutex:

1. Wait until `publication_pending` is false.
2. Increment `active_readers`.
3. Capture the current immutable `DatabaseState` containing the visible root,
   allocator root, and transaction/LSN frontier.
4. Release the mutex.

All cursors from the read transaction share this snapshot token. The active
reader count is decremented only when the transaction and all its cursors have
released it.

### 6.2 End read

Releasing the final snapshot token decrements `active_readers` and notifies a
waiting publisher.

### 6.3 Publication gate

After the WAL commit is durable, the writer:

1. Sets `publication_pending = true` under the gate mutex.
2. Prevents new readers from starting.
3. Waits until `active_readers == 0`.
4. Publishes the prepared cache and database-state changes.
5. Clears `publication_pending` and admits readers.

Readers that overlap write preparation or WAL synchronization see the previous
state and are serialized before the write. Readers admitted after publication
see the new state and are serialized after it. New readers cannot starve a
durable publisher.

Long readers delay publication. Statistics expose their count and oldest age;
TinyDB diagnoses them but never invalidates a live transaction behind the
application's back.

## 7. Transaction-local pages

The B+ tree does not depend on the committed cache directly. It operates on a
page-access context with these logical operations:

```text
Read(page_id)
Edit(page_id)
Allocate(page_type)
Free(page_id)
```

This boundary need not use virtual dispatch. It may be implemented with
templates, concepts, or concrete context objects.

A write transaction owns:

```text
base DatabaseState
private page map: page_id -> mutable frame
new page IDs
retired page IDs
private allocator state
resulting root and allocator root
transaction memory usage
state: Active | Frozen | WritingWal | Durable | Published | Aborted |
       Indeterminate
```

Rules:

- `Read` checks the private map first, then the captured committed state.
- `Edit` copies a committed page into a private heap-owned frame on first
  write. Repeated edits return the same private frame.
- `Allocate` reserves an ID in private allocator state and creates a zeroed,
  typed private frame. It does not extend the database file.
- `Free` makes the page unreachable in the transaction and records its
  retirement. A page retired by a transaction is never reused by that same
  transaction.
- Aborting destroys all private state. No shared state requires undo.
- Page guards keep frame addresses stable across container growth.
- Transaction memory is charged as pages and large-value buffers are created.
  Exceeding the configured limit aborts before WAL I/O.

## 8. B+ tree and page access

The B+ tree owns logical map behavior only. It does not know about WAL records,
checkpointing, process locks, or cache eviction.

It implements:

```text
Get
Put
Delete
Seek
Next
Scan range
Scan prefix
```

### 8.1 Root management

The root page ID may change. A root split allocates a new root and records it in
the transaction's resulting `DatabaseState`. Root collapse publishes the
remaining child as the new root. There is no permanent root-page-ID invariant.

### 8.2 Page representation

Leaf and internal pages use explicitly encoded slotted records.

- Read-only page views validate the common header, checksum, slot bounds,
  record bounds, key order, and referenced page IDs.
- Lookups binary-search the slot directory without copying every key or value.
- Mutations operate on a private logical page builder and emit one compact,
  fully packed final page image.
- Deletion does not leave tombstone flags in ordinary tree pages.
- Splits are selected by encoded byte occupancy rather than record count.
- Leaf pages carry a validated forward sibling link for cursors.

This deliberately avoids complicated fragmented in-place mutation. Slotted
pages earn their cost on the read path; private pages are rebuilt on writes.

### 8.3 Deletion policy

Deletion has three separate concerns:

- Logical deletion is immediate.
- Empty/unreachable pages are retired transactionally.
- Occupancy optimization is policy, not correctness.

Cheap local merges may occur in the deleting transaction. Broader rebalancing
or compaction runs as an ordinary maintenance write transaction. An underfull
page remains correct and readable.

## 9. Large values

Small values are stored inline in leaf records. Large values use overflow
pages. An overflow descriptor records the total byte length, first overflow
page, and value checksum.

Each overflow page records:

```text
common page header
owning value identifier
chunk index
next page ID or null
payload length
payload bytes
```

Recovery treats overflow pages like any other physical page image. Reads
validate total length, ordering, checksums, duplicate page references, and
cycles.

The copying `Get` API returns an owned value and therefore works for inline and
overflow values uniformly. Cursors expose value length plus a copying read;
they do not pretend a multi-page value is one contiguous borrowed view. A
future streaming value reader may be added without changing the file format.

## 10. Transactional allocator

Persistent allocator state contains:

```text
high-water page ID
free-extent index root
```

Free extents carry the LSN at which they were retired. An extent is eligible
for reuse only when:

```text
retire_lsn <= checkpoint_lsn
```

This checkpoint-delayed reuse prevents page-ID ABA across recovery,
checkpoint snapshots, and dirty cache versions.

The free-extent index is mutated through the same transaction overlay as the
B+ tree. To avoid recursive allocation, allocator metadata page splits always
allocate from the high-water frontier; they never ask the free index to supply
their own growth pages.

Abort discards high-water and free-index changes. Publishing a transaction
publishes allocator state atomically with the B+ tree root.

## 11. Committed page cache

The committed cache contains immutable page frames. Each frame records:

```text
page ID and type
encoded bytes
page LSN / transaction ID
pin count
checkpoint status
I/O status
```

Properties:

- A dense page table resolves a page ID to its latest visible frame. Commit
  preflight grows it through the transaction's high-water page ID.
- Publishing transfers transaction-owned frames into the cache without copying
  page bytes.
- Replaced frames remain alive while pinned by a checkpoint snapshot or
  internal operation.
- Dirty committed frames are recoverable from the WAL but cannot be evicted
  until the database file contains at least their LSN. Ordinary reads never
  consult the WAL.
- Cache capacity is a soft byte target. Publication may exceed it by at most
  the configured write-transaction limit.
- Dirty pressure requests a checkpoint. If checkpoint I/O continues to fail,
  writers receive backpressure before memory grows without bound.
- Eviction uses a real reference-aware policy. The policy is replaceable and
  does not affect correctness.

Checkpoint capture scans the dense page table and retains every current frame
newer than `checkpoint_lsn`. This avoids a second allocation-bearing dirty
index in the post-durability publication path.

## 12. Commit protocol

The commit protocol is:

```text
prepare privately
    -> freeze and validate
    -> build publication plan
    -> encode WAL transaction
    -> append WAL transaction
    -> fsync WAL segment
    -> close reader admission
    -> drain existing readers
    -> publish without failure
    -> return CommitInfo
```

Detailed steps:

1. Validate application limits and transaction state.
2. Finish every B+ tree split, merge, overflow update, retirement, and allocator
   mutation.
3. Freeze the overlay; further mutation is rejected.
4. Validate every final page image and the resulting database state.
5. Assign the transaction ID and LSN range.
6. Grow the dense cache page table, construct immutable frame ownership and the
   immutable `DatabaseState` object, and reserve the permitted cache overage.
   Concurrent cache activity cannot invalidate this plan.
7. Encode the entire WAL transaction in memory, including its commit record.
8. Rotate to a new WAL segment first if the transaction would exceed the
   current segment's soft target. One transaction never spans segments; a
   segment may exceed its target by at most one transaction.
9. Append the encoded transaction.
10. Synchronize the WAL segment. If it was newly created, synchronize its
    directory entry before claiming durability.
11. Close reader admission and wait for existing readers.
12. Splice prepared frames and index nodes into the committed cache.
13. Publish the immutable database state and `visible_lsn` with release
    semantics.
14. Reopen reader admission.
15. Return `CommitInfo { transaction_id, commit_lsn }`.

After step 10, steps 11-14 are not allowed to fail. A crash between durability
and publication causes recovery to publish the transaction on the next open;
the application did not receive success and must resolve the outcome through
its own idempotency key stored in the transaction.

## 13. Commit failure semantics

Commit has three application-visible outcomes.

### 13.1 Committed

Success is returned only after WAL durability and publication. Immediate reads
see the transaction, and recovery preserves it.

### 13.2 Definitely aborted

Validation, memory-budget, B+ tree, and allocation failures before WAL append
leave the old database usable.

If WAL append fails before the complete encoded transaction reaches the file,
the commit record cannot be complete. The writer truncates back to the
transaction's known-good starting offset. The truncation does not need its own
sync to prove abort: a crash may restore only an incomplete ignored tail, while
the next successful append overwrites and synchronizes that location. If live
tail repair succeeds, the transaction is definitely aborted and the database
may accept another writer.

### 13.3 Indeterminate

The outcome is indeterminate when:

- WAL synchronization fails after the complete transaction was appended.
- A newly created WAL segment's directory synchronization fails.
- The live WAL tail cannot be restored to its known-good offset.
- The process dies after any commit bytes might have become durable.

The database enters `NeedsRecovery` and rejects all subsequent reads and writes
until it is closed and reopened. Recovery determines whether the commit record
is durable. An application-supplied idempotency key written inside the
transaction is the authoritative way to resolve the outcome.

The consumed write transaction enters the terminal `Indeterminate` state. It
must never be represented as `Aborted`: recovery may still publish it.

Ordinary pre-WAL transaction failures never poison the database.

## 14. Database file format

The database file contains:

```text
page 0      superblock A
page 1      superblock B
page 2...   typed pages
```

All integers use fixed-width little-endian encoding. Padding is never
persisted. Encoders and decoders operate on byte spans, not native structs.

Each superblock contains:

```text
magic
format major and minor version
required and optional feature flags
128-bit database UUID
generation
page size
checkpoint LSN
last checkpointed transaction ID
B+ tree root page ID
allocator root page ID
high-water page ID
checksum
```

The valid superblock with the highest generation is authoritative. Advancing a
checkpoint writes the inactive superblock once with the next generation and
synchronizes the database file. There is no separate active flag or second
activation write.

Every non-superblock page begins with a common encoded header:

```text
page magic and format version
page type
page ID
page LSN / transaction ID
payload length
flags
checksum
```

The checksum covers the complete page with the checksum field zeroed. Page
types include B+ tree leaf, B+ tree internal, overflow, and allocator metadata.

Format-major changes require explicit migration or rejection. Minor changes
may add ignorable optional features. A binary never silently rewrites an
unsupported format.

## 15. WAL format and lifecycle

The WAL is a set of ordered segment files associated with one database UUID.
The segment-size option is a rotation target, not an on-disk compatibility
parameter.

Each segment header contains:

```text
WAL magic and format version
database UUID
segment number
starting LSN
checksum
```

Each record contains:

```text
encoded length
record type
transaction ID
LSN
record sequence number
payload
checksum over header and payload
```

Record types are:

- `PAGE_IMAGE`
- `DATABASE_STATE`
- `COMMIT`

The commit record binds:

```text
transaction ID
first and final LSN
expected page-image count
digest of all prior records in the transaction
digest of the resulting database state
```

This prevents a valid commit marker from accepting a transaction with a
missing, duplicated, or reordered middle record.

Rules:

- One transaction is fully contained in one segment.
- A newly created segment is not a durable home for a commit until the segment
  and its parent directory are synchronized.
- An incomplete final transaction is ignored.
- Invalid CRC, digest, ordering, or a missing segment in the durable middle is
  corruption, not a torn tail.
- Segment removal occurs only after a covering superblock is durable, followed
  by directory synchronization.
- No new transaction is appended beyond an unrepaired invalid tail.

## 16. Checkpointing

Only the checkpoint manager writes ordinary database data pages. Cache
eviction may request a checkpoint but does not independently write a newer or
older version of a page. This single-writer rule prevents per-page LSN
reordering.

One checkpoint runs at a time:

1. Briefly synchronize with publication.
2. Capture `target_lsn = visible_lsn`, the immutable database state, and strong
   references to the latest dirty frame for each page at or before the target.
   Capture serializes with the page/state replacement inside publication but
   does not close reader admission or wait for existing readers.
3. Release publication; later transactions may prepare and publish.
4. Write the captured page frames to their database-file offsets. Snapshot
   references retain exact old versions even if the cache publishes newer
   ones.
5. Extend the file where necessary.
6. Synchronize the database file.
7. Write the inactive superblock with the captured roots, allocator state,
   high-water mark, target LSN, and next generation.
8. Synchronize the database file again.
9. Advance in-memory `checkpoint_lsn`.
10. Mark current frames at or below the target checkpointed and evictable.
11. Delete immutable WAL segments fully covered by the target and synchronize
    the WAL directory. Never truncate the active append target during ordinary
    checkpointing; it may retain harmless covered records until rotation or
    recovery.
12. Release snapshot frames and wake writers waiting on dirty-memory pressure.

Advancing only `checkpoint_lsn` does not invalidate any page or logical root,
so it serializes with transaction publication but does not drain existing read
snapshots. Those readers retain their older immutable `DatabaseState`; newly
admitted transactions capture the advanced reuse frontier.

If any step before the superblock sync fails, the old superblock and WAL remain
authoritative. If cleanup fails afterward, obsolete WAL may remain but recovery
is still correct.

A write transaction may begin while checkpoint I/O is in progress. Its WAL
state records the checkpoint frontier used for allocator decisions, which may
be older than the superblock that becomes durable before the transaction
commits. Publication merges the newer durable frontier into the private
preallocated `DatabaseState`; recovery accepts the recorded frontier only when
it is no newer than the selected superblock.

Checkpoint triggers are WAL bytes, dirty-cache pressure, elapsed time,
explicit application request, and optional clean shutdown.
`Close` is not a durability boundary.

## 17. Recovery

Open performs:

1. Acquire exclusive process ownership.
2. Open the database and validate both superblocks.
3. Select the highest-generation valid superblock.
4. Locate WAL segments with the same database UUID.
5. Perform a validation pass from `checkpoint_lsn`: validate segment sequence,
   framing, checksums, transaction sequence, record counts, and commit digests.
6. Ignore only an incomplete trailing transaction. Reject corruption in the
   durable middle before modifying database pages.
7. Perform a redo pass, applying committed page images in transaction order.
8. Restore the latest committed database state from `DATABASE_STATE` records.
9. Synchronize the database file.
10. Write and synchronize the alternate superblock at the recovered frontier.
11. Delete covered WAL segments and synchronize the WAL directory.
12. Construct the committed cache and expose the database handle.

Recovery uses physical page images and does not instantiate a B+ tree. It is
idempotent: until the recovered superblock is durable, the previous superblock
and complete WAL remain sufficient to repeat the operation.

Segment continuity is evaluated relative to the selected superblock. The live
suffix beginning at `checkpoint_lsn + 1` must have exact LSN and transaction-ID
continuity. Cleanup may leave any subset of older segment files behind after a
covering superblock is durable; gaps wholly behind `checkpoint_lsn` are stale
history, not missing live WAL, and recovery validates then removes them.

A missing database file is not reconstructed from an arbitrary remaining WAL.
After any checkpoint, the WAL's live suffix contains only changes newer than
the base file, although the active segment may physically retain covered
records. The WAL is not a complete backup. Missing base state is reported as
corruption.

## 18. Verification and dump

### 18.1 Verification

`Verify` operates on a read snapshot and validates:

- Page checksums, types, IDs, and LSN bounds.
- Tree key order and separator boundaries.
- Leaf-link order and cycles.
- Overflow ownership, length, order, and cycles.
- Allocator-index structure and extent overlap.
- Reachable pages marked free.
- Double references where the page type forbids them.
- Unreachable leaked pages.

Detected corruption produces a structured report. Verification never repairs
persistent state. Corruption is report data so a partial audit is still useful;
I/O and resource failures remain `Result` errors. A malformed edge stops only
the traversal that would require trusting it, while safe ownership and leak
checks continue where possible.

### 18.2 Dump

`tinydb_dump` uses normal open, recovery, verification, and a read cursor. It
emits reversible hexadecimal key/value rows and therefore never interprets
application bytes as text.

## 19. Public API shape

Only application-facing types live under `include/tinydb`.

```cpp
class Database {
 public:
  static auto Open(Path, Options = {}) -> Result<Database>;

  auto BeginRead() -> Result<ReadTransaction>;
  auto BeginWrite() -> Result<WriteTransaction>;

  auto Get(BytesView key) -> Result<std::optional<Bytes>>;
  auto Put(BytesView key, BytesView value) -> Status;
  auto Delete(BytesView key) -> Status;

  auto Checkpoint() -> Status;
  auto Verify(VerifyOptions = {}) -> Result<VerifyReport>;
  auto Stats() const -> Result<DatabaseStats>;
  auto Close() -> Status;
};

class ReadTransaction {
 public:
  auto Get(BytesView key) -> Result<std::optional<Bytes>>;
  auto Scan(KeyRange range) -> Result<Cursor>;
};

class WriteTransaction {
 public:
  auto Get(BytesView key) -> Result<std::optional<Bytes>>;
  auto Put(BytesView key, BytesView value) -> Status;
  auto Delete(BytesView key) -> Status;
  auto Commit() && -> Result<CommitInfo>;
  void Abort() noexcept;
};

class Cursor {
 public:
  auto Seek(BytesView key) -> Status;
  auto First() -> Status;
  auto Next() -> Status;
  auto Valid() const -> bool;
  auto Key() const -> BytesView;
  auto ValueSize() const -> std::uint64_t;
  auto CopyValue() const -> Result<Bytes>;
};
```

API rules:

- `Get` returns owned bytes. It does not expose cache-frame lifetime.
- A cursor key view remains valid until cursor movement or destruction.
- Cursor end is `Valid() == false` with an `Ok` movement status; I/O and
  corruption remain distinguishable errors.
- A cursor owns a reference to its read snapshot, so it may safely outlive the
  `ReadTransaction` wrapper that created it.
- A write transaction is consumed by commit. Its destructor aborts if active.
- `WriteTransaction::Get` sees private writes and deletes.
- An invalid key or argument leaves a write transaction active and unchanged.
- Convenience calls create one-operation transactions.
- `Close` with active transactions returns `Busy`; it never invalidates them.
- The only durability mode is synchronous. Cache and checkpoint policies are
  configurable without weakening commit semantics.

## 20. Database state and errors

The database handle has one state:

```text
Open
CheckpointDegraded
NeedsRecovery
Corrupt
Closed
```

- `CheckpointDegraded` permits reads and commits while memory/WAL limits allow;
  persistent checkpoint failure eventually applies writer backpressure.
- `NeedsRecovery` rejects normal data and maintenance operations because an
  indeterminate commit may become visible after reopen.
- `Corrupt` rejects normal reads, writes, and checkpoints. Verification remains
  available to report the damage.
- `Closed` owns no file lock or storage resources.

State-level admission is fixed by this table. An admitted operation may still
fail for its own reason; for example, `Close` returns `Busy` when transactions
are active, and a degraded checkpoint may eventually make a write return
`ResourceExhausted`.

| State | Read/write/checkpoint | Verify | Stats | Close |
|---|---|---|---|---|
| `Open` | Allowed | Allowed | Allowed | Allowed |
| `CheckpointDegraded` | Allowed | Allowed | Allowed | Allowed |
| `NeedsRecovery` | `NeedsRecovery` | `NeedsRecovery` | Allowed | Allowed |
| `Corrupt` | `Corruption` | Allowed | Allowed | Allowed |
| `Closed` | `Closed` | `Closed` | `Closed` | Idempotent success |

Public error codes include:

```text
InvalidArgument
Busy
ResourceExhausted
IoError
Corruption
UnsupportedFormat
IndeterminateCommit
NeedsRecovery
Closed
```

`NotFound` is represented by `std::nullopt` for point lookup. Cursor exhaustion
is normal state, not an error.

## 21. Resource management and observability

Runtime options include:

- Cache target bytes.
- Maximum write-transaction bytes.
- WAL segment rotation target.
- WAL and dirty-cache checkpoint thresholds.
- Checkpoint interval.

Limits that alter on-disk interpretation are not runtime options.

Statistics expose facts needed to operate the guarantees:

```text
visible, durable, and checkpoint LSNs
active reader count and oldest reader age
writer preparation, WAL sync, and publication-wait latency
WAL bytes and segment count
dirty committed bytes
checkpoint age and last checkpoint error
cache hit/miss/eviction counts
reusable and retired page counts
```

Tree-height and occupancy diagnostics may be produced by verification or
sampling; they are not maintained synchronously on every mutation unless a
measured need justifies the cost.

## 22. Testing obligations

Tests are organized around guarantees.

### Model tests

Random multi-key transactions are compared with a reference `std::map`,
including commits, aborts, replacements, deletes, scans, overflow values,
maintenance, checkpoints, and reopen cycles.

### Crash tests

Subprocess termination and a durable-file model cover every boundary in commit,
checkpoint, segment rotation, and recovery. Required assertions are:

- Every acknowledged commit is present.
- No uncommitted transaction is partially visible.
- An in-flight commit is wholly present or absent.
- Recovery itself may crash and be repeated.
- A superblock never advances beyond durable data pages.
- WAL is never removed before a covering superblock is durable.

### Fault injection

Inject short reads and writes, `EINTR`, `ENOSPC`, allocation failure,
`fsync`/directory-sync failure, truncation failure, and cache-publication
preflight failure. Publication after WAL durability has no injectable failure
point by construction.

### Corruption testing

Deterministic malformed-input and randomized mutation tests exercise
superblocks, WAL segments and records, page decoders, tree links, overflow
chains, and allocator metadata. Corruption in the durable middle is never
classified as a torn tail.

### Concurrency

Run under ThreadSanitizer and exercise reader admission, long readers, writer
fairness, publication after WAL durability, cursor lifetimes, checkpoint
snapshot retention, and close with active transactions.

### Format compatibility

Golden files exist for every supported format. New binaries either open them
correctly or return `UnsupportedFormat`; they never reinterpret them.

## 23. Repository boundaries

The intended source organization is:

```text
include/tinydb/       public database, transaction, cursor, bytes, options,
                      status, and statistics types only
src/api/              public API implementation and ownership
src/txn/              overlay, allocator state, reader gate, commit protocol
src/btree/            tree algorithms and page views/builders
src/storage/          common page/superblock codecs and database file
src/cache/            committed cache, guards, eviction, dirty index
src/io/               POSIX I/O, locking helpers, test syscall boundary
src/wal/              segment codec, reader, writer
src/checkpoint/       immutable snapshot and checkpoint manager
src/recovery/         WAL validation and physical redo
src/verify/           read-only cross-page verifier
tests/                focused guarantee, fault, crash, and format tests
bench/                durability-matched measurement harness
tools/                CLI and dump programs
```

Dependencies point toward byte codecs and storage primitives. Recovery and
checkpointing use page codecs and files directly; they never depend on the
public API. Internal headers do not live under `include/tinydb`.

## 24. Deliberate extension points, not current promises

The format and ownership model leave room for, but do not promise:

- Streaming large-value readers.
- Reverse cursors and previous-leaf links.
- Group commit.
- Compression.
- Encryption.
- Full MVCC that permits publication while old readers remain active.
- Read-only access from other processes.

Each requires a new application-level guarantee and its own failure model. It
must not be introduced merely because another database implements it.
