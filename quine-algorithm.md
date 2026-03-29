# Quine Algorithm

This document describes the CDC-based delta compression algorithm used by
quine, including the parallel indexing strategy and I/O design choices.

## Unified flat address space

All files from A and all files from B are treated as a single logically
contiguous binary stream, sorted lexicographically by relative path:

```
[ A/file1 | A/file2 | ... | B/file1 | B/file2 | ... ]
  0                         sum_A
```

A `REF` opcode is a `(global_offset, length)` pair addressing this flat space.
There is no `file_idx` field — the offset alone identifies the source.  The
decompressor resolves any offset to the correct file and local position via a
small slot table built from the file-size manifest in the patch header.

## Content-defined chunking (CDC)

Each file is split into variable-length chunks (4–64 KB) using a Rabin rolling
hash over a 48-byte sliding window.  A chunk boundary is declared whenever the
low 14 bits of the hash are zero, giving an average chunk size of ~16 KB.

Because boundaries are determined by the local content of the window — not by
absolute byte position — they are **shift-resistant**: inserting bytes early in
a file does not invalidate chunk boundaries later in the same file or in
subsequent files.  Each chunk is identified by its SHA-256 digest.

## Compression

### Parallel indexing

Files are split into **segments** of approximately `total_bytes / nproc` each
(minimum 1 MB).  Large files get multiple segments; small files are one
segment.  This ensures all CPU cores stay busy even with few large files.

A file indexing uses `read()` with per-segment temporary buffers (no mmap).
B file indexing uses `mmap` with `MADV_SEQUENTIAL` + `MADV_WILLNEED` prefetch.
A and B index concurrently — A threads use heap buffers (freed after
processing), B threads use evictable page cache (kernel-managed).

Each worker thread:
1. Runs CDC + SHA-256 on its segments
2. Inserts `sha256 → global_offset` into a thread-private hash index
3. For B segments, also stores pre-computed chunk boundaries (`PreChunkList`)

After all threads complete, per-thread indexes are merged: A indexes first
(smallest global offsets), then B indexes in thread order.  `idx_insert` keeps
the first entry per SHA, guaranteeing the earliest occurrence is stored.

### Query-time offset filtering

Rather than indexing chunks incrementally during encoding (which forces
single-threaded B processing), all chunks are indexed up front in parallel.
The strictly-before invariant is enforced at **query time**: when encoding a
chunk at global position P, `idx_lookup_before(sha, P)` only returns a match
if the matched entry's data ends before P.

This allows the expensive work (CDC + SHA-256) to be fully parallelized for
both A and B files, while the sequential encode phase becomes a lightweight
loop of hash lookups and opcode writes.

### Sequential encode

For each B file in lexicographic order:
1. Emit a `NEWFILE` opcode
2. Iterate pre-computed chunks, do filtered lookup:
   - **Match found** → flush pending literals, emit `REF(global_offset, len)`
   - **No match** → accumulate into a literal run
3. Flush remaining literals at end of file

Literal runs are tracked as zero-copy pointers into the mmap'd B file data.
On flush, the data goes directly from the mmap to the buffered patch writer —
one memcpy instead of three.

### Intra-file and inter-B deduplication

Because both A and B chunks are indexed before encoding begins, the index
contains all chunk occurrences.  The offset filter ensures correctness:

- **Intra-file**: if a repeated region appears later in the same B file, the
  second occurrence finds the first in the index (offset filter passes since
  the first chunk is earlier).
- **Inter-file**: chunks from earlier B files have smaller global offsets and
  are always available to later B files via the offset filter.

## Decompression

1. Read the patch file via a 4 MB buffered reader (no mmap — see I/O notes).
2. Parse the header.  Open every A file read-only; populate the slot table.
   Pre-register all B file slots with `fd = -1` (not yet written).
3. Read opcodes sequentially:

   | Opcode | Action |
   |--------|--------|
   | `NEWFILE` | Open the next output file. Also open a separate read-only fd on the same inode for self-referential REFs. |
   | `REF` | Binary-search the slot table → `pread()` into copy buffer → `write()` to output. |
   | `LIT` | Stream directly from the patch reader buffer to the output fd. |
   | `END` | Done. |

**Self-referential reads** (a REF that points into the file currently being
written) are safe because the referenced bytes lie strictly before the current
write position.  The read-fd and write-fd are separate file descriptions on
the same inode.

## Memory guarantees

| Phase | Heap allocation |
|-------|----------------|
| Compression | O(total_chunks) for the hash index (~64 MB per million chunks) |
| Decompression | O(\|A files\| + \|B files\|) for the slot table + one `malloc(chunk_max)` = 64 KB copy buffer + 4 MB patch reader buffer |

The decompressor's peak RSS is ~6.5 MB regardless of input size.

## I/O design notes

### mmap vs buffered read

On WSL2 (9P filesystem over NTFS), `mmap` for sequential reading causes
per-page faults through the 9P protocol — each 4 KB page is a separate
round trip.  Buffered `read()` with large chunks (4 MB) batches these into
fewer, larger transfers.

Measured results on 4 GB inputs:

| Approach | Decompress time | Peak RSS |
|----------|----------------|----------|
| mmap patch | 38.5s | 1.8 GB |
| buffered read() | 31.3s | 6.5 MB |

For writing, the same principle applies:
- **mmap for output**: 3.5x slower due to per-page fault overhead on sparse file allocation
- **sendfile()**: slower than userspace buffered I/O on WSL2's 9P layer
- **fwrite with 256 KB buffer**: fastest — batches writes efficiently

The current implementation uses:
- **A indexing**: `read()` with per-segment buffers (freed after use)
- **B indexing**: `mmap` with `MADV_SEQUENTIAL` (page cache is kernel-managed,
  evictable; data stays mapped for the zero-copy encode phase)
- **Patch output**: buffered `fwrite` with 256 KB buffer
- **Patch input (decompress)**: buffered `read()` with 4 MB buffer

On native Linux with ext4/NVMe, mmap may perform equally or better.  The
buffered approach was chosen for portability and predictable RSS.
