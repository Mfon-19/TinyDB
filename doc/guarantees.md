# Guarantee Evidence

This matrix connects each application-visible promise to the smallest test
that would fail if the promise were broken. Test names are intentionally short;
this document carries the longer design rationale.

| Guarantee | Primary evidence |
|---|---|
| Unique byte keys, overwrite, idempotent delete, read-your-writes, atomic abort/commit | `Database.Transactions`, `Database.Model` |
| Unsigned lexicographic ordering and half-open/prefix ranges | `Contract.ByteOrder`, `Database.Ranges`, `Page.ByteOrder` |
| Stable read snapshots and cursor lifetime | `Database.Snapshots`, `Readers.Snapshot`, `Readers.Lifetime` |
| One writer and writer-fair publication | `Database.Concurrency`, `Readers.Fairness`, `Readers.Abandon` |
| Key, value, and transaction resource limits fail before publication | `Contract.Limits`, `Database.Limits` |
| Overflow values survive replacement, scans, checkpoint, and reopen | `Database.LargeValues`, `Format.Overflow` |
| Successful commit is durable before acknowledgement | `Durability.CommitOrder`, `Crash.Commit` |
| Creation and checkpoint synchronization order preserve a recoverable basis | `Durability.CreationOrder`, `Durability.CheckpointOrder`, `Durability.CheckpointFailure` |
| Recovery accepts a complete committed prefix and is repeatable after interruption | `Crash.Recovery`, `Durability.RecoveryRetry`, `Database.CrashCopy` |
| Torn WAL tails are discarded; durable-middle damage is corruption | `Durability.TornTail`, `Durability.MiddleCorruption` |
| WAL segment rotation and cleanup never lose live history | `Durability.Rotation`, `Crash.Commit` |
| A WAL can replay only into the database whose UUID it names | `Durability.ForeignWal` |
| A second process cannot concurrently own the database | `Database.Locking` |
| Explicit encodings are endian-stable, checksummed, and versioned | `Format.Encoding`, `Format.SuperblockGolden`, `Format.WalHeader`, `Format.WalRecord`, `Format.PageVersion`, `Format.WalVersion` |
| Either valid superblock survives damage or a torn replacement | `Format.Selection`, `Format.Damage`, `Format.Tears`, `Format.Conflict` |
| Page views reject malformed identity, slots, links, and routing | `Page.Identity`, `Page.Slots`, `Page.Links`, `Page.Mutations` |
| Environmental failure remains a returned status and leaves committed state usable or recovery-required | `Database.Failures`, `Durability.CheckpointFailure` |
| Detected persistent corruption is reported without repair | `Database.Corruption`, `Database.Health` |
| Public headers install and link without internal types | `TinyDB_installed_consumer` |

The CI matrix runs the suite with GCC and Clang, Debug and Release, ASan+UBSan,
and TSan. Deterministic crash tests use one subprocess termination before each
commit and recovery syscall; TSan excludes those fork/kill cases and retains
the in-process concurrency coverage.
