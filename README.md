# PES-VCS Lab Report

**Name:** Mohammed Zahoor Mashahir  
**SRN:** PES1UG24CS579  
**Platform:** Ubuntu 22.04

---

## Building

```bash
sudo apt update && sudo apt install -y gcc build-essential libssl-dev
export PES_AUTHOR="Mohammed Zahoor Mashahir <PES1UG24CS579>"
make all
```

---

## Phase 1 — Object Storage Foundation

### Implementation Notes

`object_write` builds the full object as `"<type> <size>\0<data>"` in a heap
buffer, computes SHA-256 over the entire object, checks for deduplication with
`object_exists`, creates the shard directory, writes to a `.tmp` file, calls
`fsync`, then renames atomically.

`object_read` reverifies the SHA-256 after reading to catch corruption, parses
the type from the header prefix, and copies the data portion (everything after
the `\0`) into a caller-owned buffer.

### Screenshot 1A — `./test_objects` output

```
Stored blob with hash: d58213f5dbe0629b5c2fa28e5c7d4213ea09227ed0221bbe9db5e5c4b9aafc12
Object stored at: .pes/objects/d5/8213f5dbe0629b5c2fa28e5c7d4213ea09227ed0221bbe9db5e5c4b9aafc12
PASS: blob storage
PASS: deduplication
PASS: integrity check

All Phase 1 tests passed.
```

### Screenshot 1B — Sharded object store after Phase 1 tests

```
.pes/objects/25/ef1fa07ea68a52f800dc80756ee6b7ae34b337afedb9b46a1af8e11ec4f476
.pes/objects/2a/594d39232787fba8eb7287418aec99c8fc2ecdaf5aaf2e650eda471e566fcf
.pes/objects/d5/8213f5dbe0629b5c2fa28e5c7d4213ea09227ed0221bbe9db5e5c4b9aafc12
```

Each object lives in `.pes/objects/XX/YY...` where `XX` is the first two hex
characters of its SHA-256 hash (directory sharding).

---

## Phase 2 — Tree Objects

### Implementation Notes

`write_tree_level` is a recursive helper that processes index entries sorted by
path. For a flat file (no `/` in the remaining path) it adds a blob `TreeEntry`
directly. For a directory it groups all entries sharing the same top-level
component, recurses, and adds a `040000` tree entry pointing at the resulting
subtree hash.

`tree_from_index` heap-allocates the `Index` struct (~5.6 MB) to avoid a stack
overflow, sorts entries by path, then calls `write_tree_level`.

### Screenshot 2A — `./test_tree` output

```
Serialized tree: 139 bytes
PASS: tree serialize/parse roundtrip
PASS: tree deterministic serialization

All Phase 2 tests passed.
```

### Screenshot 2B — Raw binary of a tree object (`xxd`, first 20 lines)

```
00000000: 7472 6565 2039 3200 3130 3036 3434 2066  tree 92.100644 f
00000010: 312e 7478 7400 2cf8 d83d 9ee2 9543 b34a  1.txt.,..=...C.J
00000020: 8772 7421 fdec b7e3 f3a1 83d3 3763 9025  .rt!........7c.%
00000030: de57 6db9 ebb4 3130 3036 3434 2066 322e  .Wm...100644 f2.
00000040: 7478 7400 e00c 50e1 6a2d f38f 8d6b f809  txt...P.j-...k..
00000050: e181 ad02 48da 6e67 19f3 5f9f 7e65 d6f6  ....H.ng.._.~e..
00000060: 0619 9f7f                                ....
```

The header `tree 92\0` is followed by entries in the format
`<mode-octal-ascii> <name>\0<32-byte-binary-hash>`.

---

## Phase 3 — The Index (Staging Area)

### Implementation Notes

`index_load` opens `.pes/index` with `fopen("r")` and parses each line using
`fscanf` with format `%o %64s %llu %u %511s`. A missing file means an empty
index (not an error).

`index_save` heap-allocates a sorted copy, writes to a `.tmp` file, calls
`fflush` + `fsync` for durability, then `rename` for atomicity.

`index_add` reads the file, stores it as `OBJ_BLOB`, `stat()`s for metadata,
upserts the entry, and calls `index_save`.

### Screenshot 3A — `pes init` → `pes add` → `pes status`

```
$ ./pes init
Initialized empty PES repository in .pes/

$ ./pes add f1.txt f2.txt

$ ./pes status
Staged changes:
  staged:     f1.txt
  staged:     f2.txt

Unstaged changes:
  (nothing to show)

Untracked files:
  untracked:  bye.txt
  ...
```

### Screenshot 3B — `cat .pes/index`

```
100644 2cf8d83d9ee29543b34a87727421fdecb7e3f3a183d337639025de576db9ebb4 1776399048 6 f1.txt
100644 e00c50e16a2df38f8d6bf809e181ad0248da6e6719f35f9f7e65d6f606199f7f 1776399048 6 f2.txt
```

Text format: `<mode-octal> <sha256-hex> <mtime-epoch> <size-bytes> <path>`

---

## Phase 4 — Commits and History

### Implementation Notes

`commit_create` calls `tree_from_index` to snapshot the staged state, reads the
current HEAD as the parent (skipped for the first commit), populates a `Commit`
struct with author (`PES_AUTHOR` env var), `time(NULL)` timestamp, and message,
serializes it, stores it via `object_write(OBJ_COMMIT)`, then calls
`head_update` to advance the branch pointer atomically.

### Screenshot 4A — `pes log` with three commits

```
commit 31238d700739bb851c404155bc6bdd4b9a22e262785a3f46489bbc3431ed305a
Author: Mohammed Zahoor Mashahir <PES1UG24CS579>
Date:   1776399048

    Add farewell

commit 870f481deaced4c93500919150661098a017fe0ca17727cbfd62699511fb7322
Author: Mohammed Zahoor Mashahir <PES1UG24CS579>
Date:   1776399048

    Update f1.txt

commit cec37e4ddcfc7a27974749dcb2fcb66f48469bb2108899848b20fc244cc21188
Author: Mohammed Zahoor Mashahir <PES1UG24CS579>
Date:   1776399048

    Initial commit
```

### Screenshot 4B — `find .pes -type f | sort` after three commits

```
.pes/HEAD
.pes/index
.pes/objects/2c/f8d83d9ee29543b34a87727421fdecb7e3f3a183d337639025de576db9ebb4
.pes/objects/31/238d700739bb851c404155bc6bdd4b9a22e262785a3f46489bbc3431ed305a
.pes/objects/48/31029452657a7570fbe1f2440d768bf404c6a4f4e5bb4716509612b912c87b
.pes/objects/87/0f481deaced4c93500919150661098a017fe0ca17727cbfd62699511fb7322
.pes/objects/a6/cee62dcbbffd397401d91905fbe15eef346cde3e9fa1de7387bfe1084e3fb2
.pes/objects/b1/7b838c5951aa88c09635c5895ef7e08f7fa1974d901ce282f30e08de0ccd92
.pes/objects/b9/0d721e824c5b0f7ea8ed89a47651b9968f583ac3e63380a8b9603390b312d9
.pes/objects/ce/c37e4ddcfc7a27974749dcb2fcb66f48469bb2108899848b20fc244cc21188
.pes/objects/db/5c6d09280cfbe92a8436a7c864c3f4e38b4a041e84e6697e4ad97f3153c1a4
.pes/objects/e0/0c50e16a2df38f8d6bf809e181ad0248da6e6719f35f9f7e65d6f606199f7f
.pes/refs/heads/main
```

10 objects: 3 blobs (f1 v1, f1 v2, f2), 3 trees (one per commit snapshot),
3 commits, 1 blob for bye.txt.

### Screenshot 4C — Reference chain

```
$ cat .pes/refs/heads/main
31238d700739bb851c404155bc6bdd4b9a22e262785a3f46489bbc3431ed305a

$ cat .pes/HEAD
ref: refs/heads/main
```

HEAD is a symbolic ref pointing to `refs/heads/main`; that file holds the latest
commit hash.

### Final — Full integration test (`make test-integration`)

```
=== PES-VCS Integration Test ===

--- Repository Initialization ---
Initialized empty PES repository in .pes/
PASS: .pes/objects exists
PASS: .pes/refs/heads exists
PASS: .pes/HEAD exists

--- Staging Files ---
Status after add:
Staged changes:
  staged:     file.txt
  staged:     hello.txt
Unstaged changes:
  (nothing to show)
Untracked files:
  (nothing to show)

--- First Commit ---
Committed: c31b9ab3a8c6... Initial commit

Log after first commit:
commit c31b9ab3a8c652607b5726fc06d311e1dfb010c1f68aa03e843baf4d7ce8ee64
Author: PES User <pes@localhost>
Date:   1776398996

    Initial commit

--- Second Commit ---
Committed: 1f026e292509... Update file.txt

--- Third Commit ---
Committed: 037dbb483961... Add farewell

--- Full History ---
commit 037dbb483961635e28b20be688ff1643680db5c69ab5c71872c82380c6237a8f
Author: PES User <pes@localhost>
Date:   1776398996

    Add farewell

commit 1f026e292509b9d633cb826b69835a48db541b7cc402e54af5c077432b7d4902
Author: PES User <pes@localhost>
Date:   1776398996

    Update file.txt

commit c31b9ab3a8c652607b5726fc06d311e1dfb010c1f68aa03e843baf4d7ce8ee64
Author: PES User <pes@localhost>
Date:   1776398996

    Initial commit

--- Reference Chain ---
HEAD:
ref: refs/heads/main
refs/heads/main:
037dbb483961635e28b20be688ff1643680db5c69ab5c71872c82380c6237a8f

--- Object Store ---
Objects created: 10

=== All integration tests completed ===
```

---

## Phase 5 — Branching and Checkout (Analysis)

### Q5.1 — How would you implement `pes checkout <branch>`?

**Files that must change in `.pes/`:**

1. `.pes/HEAD` — rewrite from `ref: refs/heads/<old>` to
   `ref: refs/heads/<new>` (or create the branch file if the branch is new).
2. The working directory must be updated to match the target branch's tree
   (see below).

**Working-directory update algorithm:**

1. Read the target branch's tip commit from `.pes/refs/heads/<branch>`.
2. Load the root tree of that commit via `object_read`.
3. Recursively walk the tree: for each blob entry write/overwrite the file in the
   working directory; for tree (040000) entries create directories as needed.
4. Delete working-directory files that are tracked in the *current* HEAD's tree
   but absent from the target tree.

**What makes it complex:**

- **Dirty working-directory detection**: if a tracked file is modified but
  unstaged, and the target branch has a *different* version of that file,
  checkout must refuse. Otherwise the user's local edits are silently destroyed.
- **Untracked file collision**: if an untracked file exists at a path that the
  target branch would create, checkout must refuse to avoid clobbering it.
- **Partial failure**: a crash mid-checkout leaves the working directory in an
  inconsistent mixed state. A production system would write a "merge-in-progress"
  marker and provide a recovery path.
- **Three-way diff**: determining which files need to be touched requires
  comparing the current-HEAD tree, the target-HEAD tree, and the index
  simultaneously.

### Q5.2 — Detecting dirty working-directory conflicts using index + object store

Algorithm (no git diff needed):

1. Load the **current index** (the staged snapshot that matches HEAD after the
   last commit).
2. For each path in the index, `stat()` the working-directory file:
   - If `st.st_mtime != entry->mtime_sec || st.st_size != entry->size`, the
     file *may* have changed. Re-hash it (`SHA-256`) and compare to
     `entry->hash`. A mismatch means the file is **dirty**.
3. For each path in the **target branch's tree** (read via `object_read`):
   - If that path appears in the current index AND is dirty (step 2), refuse
     checkout for that path.
4. For **untracked** files: if a path would be created by the target tree but
   does not exist in the current index, check whether the path already exists in
   the working directory. If it does, refuse (to avoid overwriting an untracked
   file).

The index's `mtime`/`size` metadata serves as a fast "has this file changed?"
filter; the full SHA-256 re-hash is the authoritative check used only when
metadata differs.

### Q5.3 — Detached HEAD and recovery

**What happens:** When HEAD contains a raw commit hash (not `ref: refs/heads/…`),
`head_update` writes new commit hashes directly into `HEAD`. Each commit still
correctly chains to its parent, but no branch file is updated. Once you check out
a branch, HEAD is overwritten and the detached-HEAD chain has no ref pointing to
it — it becomes unreachable from all named references.

**Recovery before switching:**
```bash
# Note the hash shown in HEAD
cat .pes/HEAD
# Create a branch pointing to it
echo <hash> > .pes/refs/heads/recovery-branch
echo "ref: refs/heads/recovery-branch" > .pes/HEAD
```

**Recovery after switching away:** the commit objects still exist in the object
store. Walk every object under `.pes/objects/`, read each with `object_read`, and
look for `OBJ_COMMIT` objects whose parent chain is not reachable from any branch
ref. (Git calls this `git fsck --lost-found`.) The grace period in GC (see Q6.2)
means the objects survive for at least two weeks in a real Git repo.

---

## Phase 6 — Garbage Collection (Analysis)

### Q6.1 — Algorithm to find and delete unreachable objects

**Mark-and-sweep:**

**Mark phase** — build the reachable set:
1. Enumerate every file under `.pes/refs/` and read `HEAD`. These are the GC
   roots.
2. For each root commit hash, call `object_read`. Add the commit hash, its tree
   hash, and every blob/subtree hash reachable by recursively walking the tree to
   a `reachable` hash set.
3. Follow each commit's parent pointer until `has_parent == 0`.

**Data structure:** a sorted array of `ObjectID` (32 bytes each) with binary
search — O(n log n) build, O(log n) lookup. For larger repos, a hash table
(e.g., open-addressed with SHA-256 hash as the key) gives O(1) average lookup.

**Sweep phase** — delete unreachable objects:
- Walk every file under `.pes/objects/XX/`. Reconstruct the 64-hex hash from
  directory name + filename.
- Convert to `ObjectID` with `hex_to_hash`. If not in `reachable`, call
  `unlink()`.

**Estimate for 100,000 commits, 50 branches:**

Assume ~4 unique objects per commit on average (1 commit, ~1–2 trees, ~1–2
new blobs per commit, rest shared):
- Reachable set ≈ 400,000 objects → 400,000 mark-phase `object_read` calls.
- Sweep: walk all ~400,000 files in the object store.
- Total: ~**800,000 file accesses**.

### Q6.2 — Race condition between GC and a concurrent commit

**The race:**

| Time | `pes commit` | GC |
|------|-------------|-----|
| t1 | `object_write(OBJ_BLOB)` stores blob `B` | — |
| t2 | — | Mark phase begins; reads HEAD → old commit. Blob `B` is not reachable. |
| t3 | `object_write(OBJ_TREE)` stores tree `T` referencing `B` | — |
| t4 | — | Sweep phase: `B` is not in reachable set → `unlink(B)` |
| t5 | `object_write(OBJ_COMMIT)` + `head_update` | — |

Result: HEAD now points to a commit whose tree references the deleted blob `B`.
The repository is corrupt.

**How Git avoids this:**

1. **Grace period (`gc.pruneExpire`, default 2 weeks):** GC skips any loose
   object whose filesystem `mtime` is younger than the grace period. A freshly
   written blob is always newer than 2 weeks, so it is never collected before it
   is referenced by a committed tree.
2. **Ref-based keep-alive:** Git writes temporary refs (`FETCH_HEAD`,
   `MERGE_HEAD`, `ORIG_HEAD`) for in-flight operations so GC sees those objects
   as reachable roots.
3. **Pack-before-prune ordering:** when converting loose objects to packfiles, Git
   writes and verifies the pack before deleting any loose objects, so there is
   never a window where an object exists only in a half-written pack.
4. **The atomic rename trick does not help here** — the blob is fully written
   before GC runs. The window is purely temporal (mark → commit racing), and only
   the grace period closes it reliably.
