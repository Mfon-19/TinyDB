# TinyDB Architecture Roadmap

This roadmap implements the architecture in [design.md](design.md). It is
ordered by dependency and proof obligation, not by feature visibility.

The target is one coherent engine. During the transition:

- Do not keep old and new transaction, WAL, allocator, page, or recovery paths
  selectable by flags.
- Do not preserve the current native-struct file format through runtime shims.
  Mint the new format magic and reject older files. A migration tool is a
  separate future product decision.
- Do not make tests pass by weakening guarantees. A failing crash or model test
  is evidence that the design or implementation is incomplete.
- Delete superseded code in the same milestone that replaces it.
- Keep every milestone reviewable: state the invariant it establishes and the
  obsolete invariant it removes.

## Current baseline

The working tree already contains valuable durability hardening that should be
landed or otherwise preserved as a coherent baseline before the architectural
rewrite:

- Exclusive process locking.
- Database/WAL identity pairing.
- Header checksums and header-recovery handling.
- Header-geometry validation.
- Short-I/O handling.
- Structural integrity checks.
- Crash, power-loss, and recovery-interruption tests.
- Repeatable local performance measurement tooling.

These tests and lessons remain useful even where the implementation is later
replaced. First make the baseline reproducible from a clean checkout; do not
begin a second architectural layer on top of an unreviewable mixed diff.

## Milestone 1: Contract and state-machine tests

Status: complete

Delivered:

- Added the four public status codes and classified lock contention as `Busy`.
- Defined exhaustive database and transaction transition graphs, including an
  explicit terminal `Indeterminate` transaction state.
- Defined and exhaustively tested operation admission in every database state.
- Defined `Close` with active transactions and opening an owned database as
  `Busy` without coupling those rules to the current storage implementation.
- Added reusable transaction conformance scenarios covering commit, abort,
  destructor abort, overwrite, idempotent delete, read-your-writes, one-writer
  admission, byte ordering, and bounded or unbounded half-open ranges.
- Added a reference model that executes those scenarios now; a real TinyDB
  adapter can run the same assertions when the transaction API exists.
- Made the 1,024-byte key limit, unsigned byte comparator, invalid-mutation
  behavior, and page-independent value semantics executable contracts.
- Reused that model as the oracle for the existing randomized storage-engine
  reopen and checkpoint test.

### Goal

Turn the design contract into executable tests before changing ownership.

### Work

- Add public error codes required by the design:
  - `Busy`
  - `UnsupportedFormat`
  - `IndeterminateCommit`
  - `NeedsRecovery`
- Define internal database and transaction state enums.
- Add a reference-model test harness for multi-key transactions:
  - commit;
  - abort;
  - overwrite;
  - idempotent delete;
  - read-your-writes;
  - half-open and unbounded ranges.
- Express commit outcomes as committed, definitely aborted, or indeterminate.
- Define `Close` with active transactions and second-open ownership behavior
  independently of the current handle implementation.
- Package transaction semantics as reusable conformance scenarios. Bind them
  to the reference model here and to the real engine when it exists.

### Exit criteria

- Tests describe the final public semantics without depending on current
  buffer-pool mutation behavior.
- Every public operation has a defined result in every database state.
- The design document and tests agree about key ordering, limits, transaction
  visibility, and commit failure semantics.

## Milestone 2: Explicit storage codecs and dual superblocks

Status: complete

Delivered:

- Added bounded fixed-width little-endian integer and byte-span primitives.
- Defined the new 4 KiB superblock format with explicit field offsets, format
  versions, required and optional feature flags, database UUID, generation,
  checkpoint LSN, transaction ID, root IDs, and allocator high-water ID.
- Added full-page CRC validation and semantic validation of persistent page
  references.
- Added dual-superblock selection by highest valid generation, including
  rejection of conflicting copies at the same generation.
- Added host-independent golden bytes, round-trip, corruption, feature/version,
  opposite-endian, and exhaustive torn-write boundary tests.
- Replaced the native single header with alternating pages 0 and 1; checkpoints
  mirror their final durable generation before the WAL is reset.
- Added the common checksummed data-page header and explicit leaf, internal,
  allocator, and overflow encodings.
- Replaced native WAL framing with versioned, UUID-bound segment headers and
  checksummed records carrying explicit transaction IDs and LSNs.
- Synchronized the database file and its parent directory in creation order,
  including retryable interrupted creation.
- Removed the native persisted structs, ABI size assertions, old format magic,
  and compatibility path. Old database and WAL formats are rejected before
  mutation.

### Goal

Make persistent bytes a stable, validated contract before higher layers depend
on them.

### Work

- Introduce fixed-width little-endian encoding helpers.
- Implement byte-span codecs for:
  - superblocks;
  - common data-page headers;
  - B+ tree leaf pages;
  - B+ tree internal pages;
  - overflow pages;
  - allocator metadata pages;
  - WAL segment headers and records.
- Add complete-page checksums and UUID encoding.
- Reserve pages 0 and 1 for alternating superblocks.
- Implement highest-valid-generation selection.
- Persist root, allocator root, high-water ID, checkpoint LSN, transaction ID,
  and format feature flags.
- Synchronize the database file and parent directory during creation.
- Validate page IDs, lengths, LSNs, and checksums without `TINYDB_CHECK`.
- Mint a new database and WAL format magic.
- Remove native persisted structs and size-based ABI assertions.

### Tests

- Round-trip every codec.
- Golden byte fixtures independent of host struct layout.
- Corrupt each encoded field and checksum.
- Torn superblock A and B at every byte boundary.
- Opposite-endian fixture expectations.
- Unsupported major/minor/feature combinations.
- File-creation durability ordering.

### Exit criteria

- No on-disk field is produced by copying a native C++ object representation.
- One valid superblock is sufficient to reject or recover from damage to the
  other.
- Old formats are rejected before mutation.
- Persistent corruption is returned as an error rather than aborting.

## Milestone 3: Page views, builders, and page-source boundary

Status: complete

Delivered:

- Added validated, immutable leaf and internal page views that borrow pinned
  encoded bytes and binary-search slot directories without decoding complete
  nodes into owning vectors.
- Moved tree descent, point lookup, range scans, and integrity traversal onto
  page views; mutation paths use builders created from those same validated
  views rather than a second owning decoder.
- Applied the contract's unsigned lexicographic byte comparator consistently
  across page validation, views, and current mutation builders.
- Removed dormant tombstone behavior and its fabricated test. The former flag
  byte is reserved and nonzero values are corruption.
- Added direct-view tests for borrowed values, lower-bound and equal-goes-right
  routing, unsigned-byte ordering, page identity, reserved bytes, and malformed
  slots.
- Replaced the owning `LeafNode` and `InternalNode` decoders with private
  `LeafPageBuilder` and `InternalPageBuilder` mutation types. Builders consume
  validated views and fully repack the final page, leaving one decoder and one
  encoding path.
- Added the allocation-free `PageHandle` lease and the four-operation
  `PageSource` boundary. The B+ tree now depends only on `Read`, `Edit`,
  `Allocate`, and `Free`; the current buffer pool is isolated behind a small
  adapter.
- Removed the obsolete public `PageRef` and moved the B+ tree header out of the
  public include surface.
- Made root identity logical state. Root splits allocate a new internal root;
  root collapses promote the sole child and retire the old root without copying
  page contents. The storage engine logs changed root metadata with the same
  operation.
- Added a forward `BTreeCursor` with seek/next and one-page lease ownership.
  Materialized scans now use that primitive, ready for a transactional cursor
  wrapper in the reader milestone.
- Split deletion into logical erase, optional occupancy repair, and page
  retirement phases. Underfull pages remain valid and searchable.
- Converted descent, leaf-chain cycles, invalid child references, and ordering
  damage into returned `Corruption` statuses instead of process aborts.
- Added an in-memory page-source harness covering sparse underfull trees,
  invalid references, cursor traversal, changing root IDs, and allocation
  failure at every split allocation point with lease-balance assertions.
- Added checksum-aware randomized decoder mutations and targeted malformed
  slot, cell, link, identity, reserved-byte, and ordering tests.

### Goal

Separate B+ tree behavior from cache ownership and stop decoding entire pages
on reads.

### Work

- Replace `LeafNode`/`InternalNode` owning decode objects with:
  - validated immutable page views;
  - private logical page builders.
- Binary-search slot directories in place.
- Repack final private pages completely on mutation.
- Remove tombstone flags and tombstone-specific tests.
- Introduce the logical page context used by the tree:
  - `Read`
  - `Edit`
  - `Allocate`
  - `Free`
- Change B+ tree algorithms to accept that context rather than `BufferPool`.
- Allow the root page ID to change on split and collapse.
- Implement forward seek/next primitives used by cursors.
- Keep split decisions based on encoded bytes.
- Separate logical deletion, empty-page retirement, and optional occupancy
  repair.

### Tests

- Existing map-model workloads against the new views/builders.
- Randomized decoder mutation tests for slots, cells, links, and ordering.
- Root split/collapse with changing root IDs.
- Underfull but correct pages.
- Leaf-link cycles and invalid page references return corruption.
- Allocation-failure injection at every builder step.

### Exit criteria

- The B+ tree does not include or name the committed cache, WAL, disk file, or
  checkpoint manager.
- Point reads allocate only for the returned value, not every record on each
  visited page.
- There is one page encoding path and one B+ tree implementation.

## Milestone 4: Immutable committed cache and reader gate

Status: complete

Delivered:

- Added immutable `DatabaseState` snapshots containing tree and allocator roots,
  the high-water page ID, transaction ID, visible LSN, and checkpoint frontier.
- Renamed the handle state machine to `DatabaseLifecycle`, removing the naming
  collision between handle admission state and committed database state.
- Added shared snapshot tokens whose final owner releases one reader admission;
  cursors can therefore outlive transaction wrappers without owning a
  thread-affine mutex lock.
- Added a writer-fair reader gate. Pending publication closes new admission,
  drains existing readers, replaces the visible state, and reopens admission
  through a failure-safe publication guard.
- Added exact active-reader, publication-pending, and oldest-reader diagnostics.
- Added a heap-owned immutable committed page cache with move-only page guards,
  explicit pin accounting, a byte target, LRU reference tracking, and validated
  database-file misses.
- Added a latest-version page table and dirty-page index. Uncheckpointed
  committed pages remain resident until the durable checkpoint frontier passes
  their LSN; immutable old bytes remain valid for any existing guard.
- Added multithreaded tests for shared snapshots, cursor-like token lifetimes,
  publication fairness, concurrent cache reads, pinned-frame eviction, dirty
  retention, immutable replacement, and invalid version installation.
- Split the tree boundary into read-only `PageReader` and mutable `PageSource`
  interfaces. The committed cache implements only the read vocabulary, so it
  cannot expose shared mutation by construction.
- Added an allocation-free committed-page adapter. A tree page lease transfers
  the cache frame's shared lifetime and pin directly rather than allocating a
  wrapper on every cache hit.
- Added the internal `ReadSnapshot` and `SnapshotCursor` path. Every operation
  starts from the captured root, and every cursor retains the same snapshot
  token after its transaction wrapper is destroyed.
- Proved publication integration end to end: a cursor continues reading its old
  immutable page, publication waits for it to drain, and the next snapshot sees
  the replacement page and database state together.
- Passed the complete release and ASan/UBSan suites and the focused reader,
  cache, and snapshot suites under Clang ThreadSanitizer.

### Goal

Establish committed-state ownership and strict reader snapshots before adding
write publication.

### Work

- Replace mutable fixed frames with heap-owned immutable committed frames.
- Implement RAII page guards and pin accounting.
- Implement a byte-budgeted cache and real reference-aware eviction.
- Add the latest-visible page table and dirty-frame index.
- Implement immutable `DatabaseState` containing roots and LSN frontiers.
- Implement the reader-admission gate:
  - `publication_pending`;
  - active-reader count;
  - snapshot-token capture;
  - publisher notification;
  - oldest-reader diagnostics.
- Make cursors share ownership of the read snapshot token.
- Keep cache locks internal; do not expose frame or pin types publicly.

### Tests

- Many simultaneous readers of one immutable state.
- Snapshot token outliving its `ReadTransaction` wrapper through a cursor.
- Writer admission gate blocks new readers once publication is pending.
- Existing readers drain without being invalidated.
- Writer cannot starve under a stream of new reader attempts.
- Cache eviction never removes pinned or uncheckpointed dirty frames.
- ThreadSanitizer coverage for reader/cache interactions.

### Exit criteria

- A read transaction sees one immutable root and page-version set.
- Reader lifetime does not depend on unlocking `std::shared_mutex` from the
  creating thread.
- Cache eviction and reader admission are independent of B+ tree logic.

## Milestone 5: Transactional allocator and write overlay

Status: complete

Delivered:

- Added `TransactionPages`, a private copy-on-write implementation of the
  tree's mutable page boundary. `Read` resolves private pages first, `Edit`
  copies a committed page once, and stable heap-owned private frames retain
  their addresses across page-map growth.
- Made allocation entirely transaction-local. Reusable IDs and high-water IDs
  are reserved without growing the database file; abort drops the resulting
  frontier, allocations, page copies, retirements, and root changes together.
- Replaced the intrusive LIFO free list with checksummed allocator pages holding
  sorted, coalesced free extents and their retirement LSNs.
- Enforced checkpoint-qualified reuse and prevented a transaction from reusing
  any page it retired. Adjacent extents conservatively inherit the newest
  retirement LSN when coalesced.
- Made allocator-index growth consume only the private high-water frontier,
  avoiding recursive allocation through the index being expanded.
- Added one transaction memory budget shared by private page frames and retained
  value bytes. Budget exhaustion occurs before another private frame or value
  buffer becomes part of the transaction.
- Added explicit freeze, borrowed final-image inspection, ownership transfer to
  committed frames, and complete abort without shared-state undo.
- Migrated the compatibility `Put` and `Remove` path to the overlay. All B+ tree
  and allocator failures before WAL append now discard private state and leave
  the committed database usable.
- Made immutable snapshots the engine's ordinary read path and retained the
  existing WAL only as the temporary single-operation durability bridge pending
  Milestone 6's transactional commit coordinator.
- Reduced `DiskManager` to database-file and published-superblock ownership.
  Logical allocation, reclamation, and operation metadata no longer mutate that
  shared layer.
- Deleted the mutable fixed-frame buffer pool, its page-source adapter, its
  no-steal operation bracket, and white-box tests tied to the superseded shared
  mutation implementation.
- Extended structural verification to account separately for tree pages, free
  extents, and allocator metadata pages.
- Added tests for private read-your-writes, abort, high-water rollback, shared
  page/value memory limits, checkpoint-delayed reuse, same-transaction reuse
  prevention, allocator-index growth, randomized allocate/free/abort/commit
  sequences, every B+ tree page-budget failure, root-change uniqueness, and
  readers concurrent with private write preparation and publication.

### Goal

Make every write private and abortable, including allocation and reclamation.

### Work

- Implement `TransactionPages` over the committed cache.
- Copy a page on first `Edit`; keep stable private frame addresses.
- Implement private new-page allocation without `ftruncate`.
- Implement the persistent free-extent index.
- Allocate allocator-metadata growth only from the high-water frontier.
- Record `retire_lsn` and permit reuse only through `checkpoint_lsn`.
- Prevent same-transaction reuse of retired IDs.
- Count private pages and value buffers against a transaction memory budget.
- Implement read-your-writes and private deletes.
- Drop the complete overlay on abort.
- Remove current free-list mutation from `DiskManager` and current per-operation
  metadata-image plumbing.

### Tests

- Abort after every B+ tree and allocator mutation point.
- High-water rollback on abort.
- Free-extent index splits without recursive free-index allocation.
- No retired ID reused before a covering checkpoint.
- Random allocate/free/abort/commit model.
- Transaction memory-limit enforcement.
- Page IDs remain unique across root changes and allocator metadata growth.

### Exit criteria

- No uncommitted page or allocator mutation is visible outside the write
  transaction.
- Every pre-WAL tree/allocator error is a definite abort and leaves the
  database usable.
- `BufferPool::BeginOp`, `EndOp`, `OpDirtyFrames`, `op_dirty`, and
  `DiskManager::TakeOpImages` have no remaining equivalent or caller.

## Milestone 6: WAL segments and transaction commit coordinator

Status: complete

Delivered:

- Replaced the production WAL envelope with ordered `PAGE_IMAGE`,
  `DATABASE_STATE`, and `COMMIT` records. Page records contain only final data
  pages; logical roots and allocator state are no longer disguised as a
  superblock page image.
- Made record LSNs one monotonic sequence independent of transaction IDs and
  physical byte offsets. Each transaction also carries a zero-based record
  sequence so individually valid records cannot be silently reordered or
  spliced.
- Built the complete encoded transaction in memory before its first append.
  The commit record binds the first and final LSN, page and record counts, the
  exact preceding record bytes, and the resulting database-state digest.
- Made recovery validate a complete transaction before applying any of its
  page images. Missing, duplicated, reordered, state-less, and corrupt record
  runs are rejected, while an incomplete final transaction remains ignorable.
- Reconstructed durable superblocks from the committed logical database state
  after replay and removed `DiskManager::PrepareStateImage`; superblocks now
  belong only to the checkpoint/recovery boundary.
- Preserved the global LSN frontier when the current WAL is reset after a
  checkpoint or recovery instead of restarting record identity at byte zero.
- Kept this as the only production WAL path and passed the complete unit,
  durability, fault-injection, and crash-recovery suite with the new format.
- Added ordered immutable segment archives plus one active segment. Rotation
  synchronizes the archived name and the next active header before appending,
  and never splits a transaction across segment files.
- Added known-good tail repair. An append failure with successful truncation is
  a definite abort that permits another commit; failed repair or WAL sync
  ambiguity transitions the database to `NeedsRecovery`.
- Split overlay finalization into structural freeze and exact-LSN seal. Page
  checksums, allocator retirements, `DATABASE_STATE`, and `COMMIT` now carry the
  same coordinator-assigned commit frontier.
- Replaced the allocation-bearing hash/LRU publication path with a dense page
  table and prebuilt `PublicationPlan`. Frame ownership and table capacity are
  prepared before append; cache installation, metadata adoption, and visible
  state replacement are `noexcept` after WAL durability.
- Added public `ReadTransaction` and multi-key `WriteTransaction` handles over
  one stable shared database core. Convenience `Get`, `Put`, and `Remove` use
  those same transaction paths, and active handles make `Close` return `Busy`
  without invalidation.
- Replaced boolean closed/poisoned combinations with the defined lifecycle
  states. Checkpoint failure is maintenance degradation; indeterminate commit
  prevents subsequent access until reopen.
- Ran the reusable transaction conformance scenarios against TinyDB and added
  multi-page crash recovery, reader/publication, rotation ordering, repaired
  append, failed repair, and indeterminate synchronization coverage.

### Goal

Make a frozen transaction durably atomic, then publish it without failure.

### Work

- Implement ordered WAL segments with UUID, sequence, starting LSN, and
  checksummed headers.
- Encode one transaction wholly within one segment.
- Implement `PAGE_IMAGE`, `DATABASE_STATE`, and `COMMIT` records.
- Bind commit records to page count, record ordering, and transaction digest.
- Build the complete WAL transaction in memory before append.
- Implement soft segment rotation and directory synchronization.
- Implement known-good tail offsets and append-failure rollback.
- Transition to `NeedsRecovery` when WAL sync or tail repair is indeterminate.
- Build a publication plan before append:
  - prepared cache frames;
  - preallocated page-table/index nodes;
  - dirty-index updates;
  - immutable resulting `DatabaseState`;
  - approved cache-budget overage.
- After WAL durability, close reader admission, drain readers, and splice the
  plan using only `noexcept` operations.
- Expose multi-key `WriteTransaction` and make convenience mutations use it.
- Delete the current per-operation WAL commit and poisoned-frame path.

### Tests

- Model transactions with many page images and root/allocator changes.
- Missing, duplicated, reordered, and corrupt transaction records.
- Segment rotation immediately before a transaction.
- Crash at every append, file-sync, directory-sync, reader-gate, and publication
  boundary.
- Append failure followed by successful tail repair and another commit.
- Tail-repair failure transitions to `NeedsRecovery`.
- Crash after WAL durability but before publication recovers the transaction.
- Allocation injection proves no allocation occurs after WAL durability.
- Publication cannot produce a partial visible state.
- Run every reusable transaction conformance scenario against TinyDB.
- `Close` with live read or write transactions returns `Busy` without
  invalidating those transactions.

### Exit criteria

- A successful commit is both visible and durable.
- A definite abort leaves no committed state and permits continued use.
- An indeterminate commit prevents further access until recovery.
- No fallible operation remains after the WAL durability point.
- Single-key and multi-key writes use exactly the same commit path.

## Milestone 7: Two-pass recovery

Status: complete

Delivered:

- Moved recovery out of the WAL writer and removed the public `Wal::Recover`
  entry point. `StorageEngine::Open` now invokes one internal recovery
  subsystem while holding process ownership.
- Added a write-free validation pass that selects the authoritative
  superblock, validates the ordered WAL sequence, authenticates every complete
  transaction, and builds the complete redo plan before opening the database
  read/write.
- Added physical redo that writes transaction page images in order,
  synchronizes them, advances only the inactive superblock, synchronizes it,
  and only then cleans the covered WAL sequence.
- Refused missing, zeroed, or wholly damaged base superblocks instead of
  treating a WAL suffix as a complete backup.
- Added a transaction-digest regression proving that corruption discovered in
  a later complete transaction produces no earlier database-page writes.
- Centralized retrying positional I/O and parent-directory synchronization for
  the WAL writer and recovery protocols.
- Made cleanup-restart validation checkpoint-aware. Missing or non-contiguous
  live records remain corruption, while any subset of stale segment history
  wholly covered by the authoritative superblock is harmless and removable.
- Added platform file-offset, LSN, record-sequence, transaction-ID, and
  allocation-frontier overflow checks before physical database growth.
- Added a tracked recovery suite that sends `SIGKILL` before every hooked
  filesystem boundary, retries partially applied redo, exercises cleanup
  failure with covered archives, corrupts every durable-middle record class,
  and proves hostile frontiers cause no database mutation.
- Passed all unit, durability, fault-injection, and subprocess crash tests, as
  well as the recovery suite under AddressSanitizer and UndefinedBehaviorSanitizer.

### Goal

Restore the latest durable state without running logical mutation code.

### Work

- Acquire process ownership before recovery.
- Select the authoritative superblock.
- Discover and order WAL segments by database UUID and segment number.
- First pass:
  - validate every segment and transaction after `checkpoint_lsn`;
  - distinguish an incomplete final transaction from durable-middle
    corruption;
  - determine the latest committed state without writing database pages.
- Second pass:
  - replay complete physical page images in transaction order;
  - extend the database file as needed;
  - synchronize data pages;
  - write and synchronize the alternate superblock;
  - retire covered segments and synchronize their directory.
- Construct the committed cache only after recovery completes.
- Refuse missing base database files rather than guessing that remaining WAL is
  a complete backup.

### Tests

- Recovery crash at every read, write, sync, superblock, and segment-cleanup
  boundary, followed by another recovery.
- Corruption in every durable-middle record returns `Corruption` before replay.
- Incomplete trailing transaction is ignored.
- Partially applied earlier recovery is idempotently redone.
- Stale extra segments covered by the superblock are harmless and removable.
- Wrong database UUID is rejected.
- Missing segment in the durable sequence is corruption.

### Exit criteria

- Recovery has no dependency on B+ tree mutation code or the public API.
- A crash during recovery cannot destroy the previous recovery basis.
- The engine exposes a handle only after `visible_lsn == durable_lsn`.

## Milestone 8: Immutable checkpoint snapshots

Status: complete

Delivered:

- Added one serialized checkpoint manager and made it the only production path
  that writes ordinary database pages.
- Added a capture lock shared with transaction publication but independent of
  reader admission. Checkpoint capture retains exact immutable page frames and
  state without waiting for active readers. Advancing the checkpoint-only field
  also replaces the current immutable state without invalidating old readers.
- Released publication synchronization before checkpoint I/O, allowing a
  transaction to publish a newer version of a captured page while the old
  frame remains pinned for the target checkpoint.
- Reworked `DiskManager` to describe only the newest durable superblock. Data
  pages are synchronized first, then only the inactive superblock is written
  and synchronized; in-memory durable metadata changes only after success.
- Removed commit-time `DiskManager` adoption and merged a concurrently advanced
  checkpoint frontier into the transaction's already allocated publication
  state without adding a fallible post-WAL operation.
- Allowed WAL transactions begun during checkpoint I/O to record their older,
  conservative allocator frontier. Recovery accepts it only when it is no
  newer than the selected durable superblock.
- Marked only current cache versions at or below the completed target
  checkpointed. Newer replacements remain WAL-backed, dirty, and unevictable.
- Replaced destructive active-WAL reset with covered immutable-segment cleanup.
  The active append target is never truncated by ordinary checkpointing;
  redundant covered records are validated and removed during recovery.
- Added WAL-size, dirty-memory, elapsed-time, explicit API, and maintenance-
  retry triggers. Repeated failure applies hard bounded write backpressure and
  successful retry reopens admission.
- Kept checkpoint failure in the maintenance-degraded state: acknowledged
  commits remain readable and recoverable from WAL.
- Added concurrency tests for old/new versions of one page, writes published
  during checkpoint I/O, active readers during capture, both database sync
  failure regions, retryable segment cleanup, and recoverable backpressure.

### Goal

Bound WAL and dirty memory without racing newer page versions.

### Work

- Make the checkpoint manager the only ordinary writer of database data pages.
- Serialize checkpoints.
- Capture target visible state and strong references to exact dirty frames
  under brief publication synchronization.
- Permit later publication while captured frames are written.
- Write only the captured versions and extend the file as necessary.
- Synchronize pages before advancing the alternate superblock.
- Advance in-memory `checkpoint_lsn` only after superblock durability.
- Mark eligible current frames checkpointed and evictable.
- Make retired extents automatically reusable when their `retire_lsn` becomes
  checkpoint-covered.
- Delete covered WAL segments and synchronize the directory.
- Trigger checkpoints from WAL size, dirty pressure, time, and explicit
  requests.
- Implement bounded writer backpressure after repeated checkpoint failure.

### Tests

- A checkpoint targets `P@N` while a later transaction publishes `P@N+1`.
- An old checkpoint write can never overwrite a newer on-disk page version.
- Current frames newer than the target remain dirty and unevictable.
- Checkpoint crash before and after each database and superblock sync.
- Segment deletion failure leaves correctness intact.
- Retired IDs become reusable only after successful checkpoint advancement.
- Dirty-pressure backpressure is bounded and recoverable.

### Exit criteria

- `checkpoint_lsn <= visible_lsn <= durable_lsn` holds through every failure.
- The database never reloads a page older than the current visible frame after
  eviction.
- WAL removal never precedes a durable covering superblock.
- Checkpoint failure does not invalidate committed transactions.

## Milestone 9: Transactional cursors and overflow values

Status: complete

Delivered:

- Added the public move-only `Cursor` and owned `KeyRange` boundary with all,
  from, until, half-open, and byte-prefix ranges.
- Added explicit `First`, `Seek`, and `Next` movement. Seeking below a bounded
  range clamps to its lower bound; reaching its exclusive upper bound is a
  successful invalid position rather than an error.
- Made cursor keys borrowed until movement and values explicit owned copies
  through `ValueSize` and `CopyValue`.
- Made a public cursor an independently counted database handle. It shares the
  read transaction's snapshot admission, may outlive that wrapper safely, and
  keeps `Close` busy until destruction.
- Added true leftmost-leaf descent instead of using a fabricated minimum-key
  sentinel for unbounded scans.
- Removed the B+ tree's vector-producing scan helper. The CLI and range
  benchmark now consume public cursors one row at a time.
- Added public contract tests for unsigned prefix successors, optional and
  inverted ranges, repositioning, value ownership, transaction-wrapper
  lifetime, writer-publication drainage, checkpoints, and traversal across
  many leaf splits.
- Removed the last vector-producing `StorageEngine::Scan` compatibility API;
  production scans now have one transaction-and-cursor semantic path.
- Replaced the page-derived entry cap with a 4 MiB logical value contract.
  Inline selection uses encoded leaf footprint, so maximum-sized keys and
  large values are independent constraints.
- Added fixed leaf overflow descriptors containing logical length, first-page
  value identity, and a checksum over the complete logical value.
- Completed the overflow-page representation with owner identity, ordered
  chunk index, successor, exact payload length, and borrowed decoded payload.
- Added canonical chain allocation in the transaction overlay. Every overflow
  page is an ordinary private page image and therefore uses the existing WAL,
  publication, recovery, checkpoint, and abort protocols without a side path.
- Added whole-chain validation for frontier bounds, per-page checksums,
  ownership, chunk order, full non-terminal chunks, exact termination, cycles,
  duplicate ownership, and the logical value checksum.
- Made replacement and deletion validate and retire old chains through the
  transactional allocator. An abort discards both new chains and retirements.
- Made point reads, write-transaction reads, and cursor value copies reconstruct
  overflow values with identical validation; `ValueSize` remains allocation
  free and `CopyValue` remains explicitly owning.
- Extended global integrity accounting so every overflow page is reachable
  from exactly one descriptor, allocated metadata, or the free index.
- Added tests for multi-page reads and cursors, maximum values and keys,
  inline/overflow replacement, deletion and abort, crash recovery of creation
  and retirement, and corrupt, cyclic, duplicated, truncated, reordered, and
  misowned chains.

### Goal

Complete the application-facing ordered KV behavior without unbounded scan
materialization or page-sized values.

### Work

- Implement forward cursor seek, first, and next over a read snapshot.
- Implement optional range bounds and prefix scans.
- Keep cursor key views valid until movement.
- Return values as owned bytes from point reads and cursor copies.
- Implement inline/overflow value selection.
- Implement overflow allocation, retirement, checksums, and chain validation.
- Charge overflow pages and copied values to transaction/resource limits.
- Replace materialized `Scan` in production code and benchmarks.

### Tests

- Empty, unbounded, inverted, and prefix ranges.
- Cursor movement across leaf splits, empty leaves, and checkpoints.
- Cursor keeps its snapshot while a writer prepares and waits to publish.
- Large values across many overflow pages.
- Corrupt, cyclic, duplicated, truncated, and misowned overflow chains.
- Replacement of large with small values and vice versa.
- Abort and crash during overflow creation and retirement.

### Exit criteria

- Range memory use is bounded by cursor/page/value-copy state, not result size.
- Point and cursor APIs have explicit ownership and lifetime semantics.
- Values are no longer limited by leaf-page split geometry.

## Milestone 10: Public boundary and repository cleanup

Status: complete

Delivered:

- Replaced the implementation-era `StorageEngine` surface with the final
  move-constructible `Database`, `ReadTransaction`, `WriteTransaction`,
  `Cursor`, byte, options, status, and statistics headers.
- Made public write commit an rvalue-qualified consuming operation and removed
  unused public and internal move assignments that had no ownership use case.
- Moved page identity, file descriptors, database UUIDs, disk management, WAL,
  checks, tree, cache, allocator, checkpoint, and recovery headers under
  `src/`; `include/tinydb` now contains application types only.
- Added one `Database::Impl` owner around the stable shared core retained by
  transactions and cursors. Convenience operations continue to use the same
  public transaction path.
- Wired `Options` into the committed-cache budget, write-transaction budget,
  WAL segment size, and checkpoint pressure policy, with validation before
  filesystem mutation.
- Added coherent public statistics for LSN frontiers, WAL pressure, reader
  admission, cache residency, dirty pages, and checkpoint health.
- Removed the public partial integrity checker; ordinary `Open` still validates
  reachable persistent state, while the complete reporting verifier remains a
  Milestone 11 operation.
- Replaced repetitive CMake test blocks with one helper and removed conditional
  guards around checked-in sources.
- Added install rules, exported `TinyDB::TinyDB` package metadata, installed
  public-header file sets, and a CTest that configures and links a separate
  downstream project against the installed package only.
- Renamed the implementation and integration test around `Database`, updated
  the one-shot CLI, removed obsolete REPL and performance claims, and recorded
  only current public behavior in the README.

### Goal

Expose one small embedded API and remove implementation-era surface area.

### Work

- Move storage, cache, WAL, allocator, page, and tree headers out of
  `include/tinydb`.
- Add final public headers for database, transactions, cursor, bytes, options,
  status, and statistics.
- Use one private implementation owner for stable internal addresses.
- Remove unused move assignment and flush APIs unless a real caller remains.
- Remove current poisoned-handle semantics superseded by transaction-local
  abort and `NeedsRecovery`.
- Collapse `closed_`/`poisoned_` combinations into the final database state
  machine.
- Consolidate algorithm essays into `doc/design.md`; keep only local invariant
  comments beside code.
- Replace repetitive CMake test blocks with a helper.
- Add install/export/package rules for the public library.
- Update README and CLI documentation to describe implemented behavior only.

### Tests

- Compile and link a downstream consumer using only installed public headers.
- Prove that no public header requires a storage, cache, page, tree, allocator,
  WAL, or recovery type.

### Exit criteria

- An external application can use TinyDB without including an internal type.
- Public operations have one implementation path.
- No documentation advertises unimplemented behavior or stale performance.
- The build supports clean install and downstream consumption.

## Milestone 11: Verification and observability

Status: complete

Delivered:

- Replaced the B+ tree's private integrity checker with one structured
  cross-page verifier shared by startup and the public `Database::Verify`
  read-snapshot operation.
- Added categorized reports for checksummed page framing and LSN bounds, tree
  routing, leaf links, overflow chains, allocator metadata, double ownership,
  leaked pages, and checkpoint-gated retired/reusable extents. Corruption is
  report data; environmental failures remain errors.
- Proved verification leaves both database and WAL bytes unchanged and added
  hostile-page tests for every supported cross-page corruption class.
- Completed statistics for WAL segments, dirty bytes, cache hits/misses/
  evictions, write attempts, commit preparation/WAL-sync/publication-wait
  latency, maximum publication wait, allocator retirement state, checkpoint
  age, checkpoint request/failure state, and last checkpoint error.
- Added a concurrency test that repeatedly samples statistics while commits
  publish and checks all exported frontier and counter relationships.
- Added `tinydb_dump`, which verifies then emits binary-safe hexadecimal rows.

### Goal

Make the engine operable without weakening transaction semantics.

### Work

- Implement full verification over a read snapshot.
- Report checksums, tree structure, links, overflow ownership, allocator
  reachability, double allocation, and leaked pages.
- Add the final operational statistics:
  - LSN frontiers;
  - reader age/count;
  - writer phase latency;
  - WAL bytes/segments;
  - dirty committed bytes;
  - checkpoint health;
  - cache behavior;
  - retired/reusable pages.
- Add a binary-safe dump tool.

### Tests

- Verifier detects each supported corruption class.
- Verifier is read-only even when it fails.
- Statistics remain race-free and do not alter storage semantics.

### Exit criteria

- Verification never repairs, aborts the process, or writes persistent state.
- Operational limits and stalls can be diagnosed from exported statistics.

## Milestone 12: Hardening and measurement

Status: complete

Delivered:

- Reduced the suite from 187 cases in 17 binaries to 59 guarantee-focused cases
  in seven binaries, plus one installed-consumer CTest. Test-local fixtures,
  classes, scenario frameworks, and duplicated layer tests were removed; names
  now identify the contract in one or two words.
- Added GCC and Clang Debug/Release CI plus independent ASan+UBSan and TSan
  configurations. Crash subprocesses are labeled separately because fork/kill
  instrumentation is not a TSan concurrency proof.
- Used TSan to expose and remove a real writer/lifecycle lock-order inversion;
  the core now documents and enforces one global order, including Close and
  indeterminate-commit transitions.
- Rebuilt deterministic commit and recovery crash sweeps around public
  transactions. Each sweep kills a subprocess before successive syscall
  boundaries and accepts only a complete acknowledged prefix.
- Moved superblock and WAL golden bytes into checked-in external fixtures read
  by the format suite and every compiler CI job.
- Replaced the stale microbenchmarks with one TinyDB-only CSV harness. Warmups,
  repeated trials, deterministic access order, result validation, and
  distribution statistics make measurements reproducible without carrying an
  external performance baseline in the source tree. It measures insert and
  overwrite commits, WAL amplification, point reads, cursor traversal,
  checkpointing, recovery, and churn space reuse.
- Added [guarantees.md](guarantees.md), which maps every product promise to its
  smallest primary test and records the sanitizer and crash coverage.
- Profiling and optimization were deliberately excluded from this milestone;
  no unmeasured tuning or second semantic path was introduced.

### Goal

Prove the guarantees under hostile timing and establish reproducible
performance measurements.

### Work

- Run all tests under ASan, UBSan, TSan, and supported compilers.
- Maintain deterministic crash sweeps for commit, checkpoint, recovery, segment
  rotation.
- Add format golden files to CI.
- Benchmark TinyDB operations directly with setup outside timed regions,
  discarded warmups, repeated trials, fixed workload seeds, and validation.
- Measure:
  - insert and overwrite throughput and p50/p95/p99 commit latency;
  - WAL bytes per application byte;
  - checkpoint pause and bandwidth;
  - convenience and transaction-scoped point lookup throughput;
  - cursor throughput;
  - cache hit rate;
  - recovery time;
  - space reuse under long churn.
- Defer optimization until a separate profiling task identifies a limiting
  component.

### Exit criteria

- Every product guarantee has a model, crash, fault, corruption, or concurrency
  test that would fail if the guarantee were violated.
- Performance claims include durability mode and complete methodology.
- No optimization creates a second semantic path or weakens the failure model.

## Final definition of done

The architecture is complete when all of the following are true:

- Multi-key transactions are atomic, strictly serializable, and crash durable.
- Pre-WAL failure is abortable without poisoning committed state.
- Post-WAL publication is structurally infallible.
- Indeterminate commit outcomes force recovery and can be resolved through an
  application idempotency key.
- Readers see stable snapshots; new readers cannot starve publication.
- Recovery returns the latest durable committed state and is itself
  crash-idempotent.
- Checkpointing cannot expose stale page versions or remove necessary WAL.
- Page IDs are reused only after checkpoint-safe retirement.
- Database and WAL bytes are explicit, checksummed, versioned, and UUID-bound.
- Corruption is reported, never silently treated as a torn tail or process
  invariant violation.
- Scans stream and values are not constrained to one leaf page.
- Cache, transaction, WAL, and dirty-memory growth are bounded.
- The public API exposes no internal ownership or persistence types.
- Verification upholds the read-snapshot and corruption contracts.
- Documentation, tests, benchmarks, and implementation describe the same
  engine.
